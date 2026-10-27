/***************************************************************************
 *              __________                   __    __________
 *   Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
 *   Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
 *   Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
 *   Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
 *                                               \/        \/               \/
 *
 * Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
 * Not affiliated with or endorsed by Apple Inc.
 *
 * iPod UI shell: Settings > Main Menu / Music Menu checklists (Phase 11)
 *
 * Copyright (C) 2026 prokopto-dev
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* A split value list in the choice_screen style (settings.c): the panel
 * keeps the Main Menu / Music Menu tile and caption. One row per item the
 * shell ships (UI_MM_OFFERED / UI_MU_OFFERED), in the fixed canonical
 * order of the 7G (main-menu-custom-hold.jpg: an added item takes its
 * own slot), a blue tick at the right of a checked row, then "Reset
 * Menu" last. SELECT toggles the row in place and saves; it does not pop
 * (Settings is pinned: stock refuses to uncheck it). Reset Menu restores
 * the default mask and puts the selection back on row 0.
 *
 * State: one struct in the screen arena (UI_ARENA_B_SETTING), freed
 * with the screen; no core_alloc. */
/* #106: a module tenant defines its real screen names (the core sees the
 * stand-ins, modload.h); the target's module build passes this itself */
#ifndef IPODUI_IN_MODULE
#define IPODUI_IN_MODULE
#endif
#include "config.h"
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "logf.h"
#include "settings.h"
#include "ipodui/fe_be.h"   /* P19 S1: settings saves cross */
#include "lang.h"
#include "ipodui/screen_stack.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/list.h"
#include "ipodui/preview.h"
#include "ipodui/anim.h"
#include "ipodui/menucfg.h"
#include "ipodui/menucfg_core.h"
#include "ipodui/checkglyph.h"
#include "ipodui/screens/panels.h"
#include "ipodui/screens/screens.h"
#include "ipodui/screens/checklist.h"
#include "ipodui/panelart.h"

/* the label of every bit that is ever offered (0 = not offered) */
static const short mm_lang[UI_MM_BITS] = {
    [UI_MM_MUSIC]        = LANG_IPODUI_MUSIC,
    [UI_MM_PLAYLISTS]    = LANG_IPODUI_PLAYLISTS,
    [UI_MM_ARTISTS]      = LANG_IPODUI_ARTISTS,
    [UI_MM_ALBUMS]       = LANG_IPODUI_ALBUMS,
    [UI_MM_COMPILATIONS] = LANG_IPODUI_COMPILATIONS,
    [UI_MM_SONGS]        = LANG_IPODUI_SONGS,
    [UI_MM_GENRES]       = LANG_IPODUI_GENRES,
    [UI_MM_COMPOSERS]    = LANG_IPODUI_COMPOSERS,
    [UI_MM_AUDIOBOOKS]   = LANG_IPODUI_AUDIOBOOKS,
    [UI_MM_SEARCH]       = LANG_IPODUI_SEARCH,
#ifdef HAVE_BCMFW_VIDEO
    [UI_MM_VIDEOS]       = LANG_IPODUI_VIDEOS,     /* Phase 14a */
#endif
    [UI_MM_PODCASTS]     = LANG_IPODUI_PODCASTS,
    [UI_MM_EXTRAS]       = LANG_IPODUI_EXTRAS,
    /* the Extras that may sit on the Main Menu (UI_MM_EXTRAS_LANDED and
     * the ones offered only where they can run): every offered bit needs
     * its label, or the row reads LANG id 0 */
    [UI_MM_ALARMS]       = LANG_IPODUI_X_ALARMS,
    [UI_MM_CALENDARS]    = LANG_IPODUI_X_CALENDARS,
    [UI_MM_CLOCKS]       = LANG_IPODUI_X_CLOCKS,
    [UI_MM_CONTACTS]     = LANG_IPODUI_X_CONTACTS,
    [UI_MM_GAMES]        = LANG_IPODUI_X_GAMES,
    [UI_MM_NOTES]        = LANG_IPODUI_X_NOTES,
    [UI_MM_SCREENLOCK]   = LANG_IPODUI_X_SCREEN_LOCK,
    [UI_MM_STOPWATCH]    = LANG_IPODUI_X_STOPWATCH,
    [UI_MM_SETTINGS]     = LANG_SETTINGS,
    [UI_MM_SHUFFLE]      = LANG_IPODUI_SHUFFLE_SONGS,
};
static const short mu_lang[UI_MU_BITS] = {
    [UI_MU_PLAYLISTS]    = LANG_IPODUI_PLAYLISTS,
    [UI_MU_ARTISTS]      = LANG_IPODUI_ARTISTS,
    [UI_MU_ALBUMS]       = LANG_IPODUI_ALBUMS,
    [UI_MU_COMPILATIONS] = LANG_IPODUI_COMPILATIONS,
    [UI_MU_SONGS]        = LANG_IPODUI_SONGS,
    [UI_MU_GENRES]       = LANG_IPODUI_GENRES,
    [UI_MU_COMPOSERS]    = LANG_IPODUI_COMPOSERS,
    [UI_MU_AUDIOBOOKS]   = LANG_IPODUI_AUDIOBOOKS,
    [UI_MU_SEARCH]       = LANG_IPODUI_SEARCH,
};

struct check_def {
    int title, caption;             /* LANG_*: the tile's name and caption */
    unsigned offered, deflt;
    int pinned;                     /* a bit that can't be unchecked, or -1 */
    const short *lang;              /* label per bit */
    int *mask;                      /* the setting (global_settings) */
};

/* The checklists, in the order of UI_CHECKLIST_* (checklist.h). */
static void def_get(int kind, struct check_def *d)
{
    if (kind == 2)
    {
        d->title = LANG_IPODUI_MUSIC_MENU;
        d->caption = LANG_IPODUI_CAP_MUSIC_MENU;
        d->offered = UI_MU_OFFERED;
        d->deflt = UI_MU_DEFAULT;
        d->pinned = -1;
        d->lang = mu_lang;
        d->mask = &global_settings.ipodui_music_menu;
    }
    else
    {
        d->title = LANG_IPODUI_MAIN_MENU;
        d->caption = LANG_IPODUI_CAP_MAIN_MENU;
        d->offered = UI_MM_OFFERED;
        d->deflt = UI_MM_DEFAULT;
        d->pinned = UI_MM_SETTINGS;
        d->lang = mm_lang;
        d->mask = &global_settings.ipodui_main_menu;
    }
    /* never offer a row without a label: str(0) would show LANG id 0 */
    {
        int bits = kind == 2 ? UI_MU_BITS : UI_MM_BITS, b;

        for (b = 0; b < bits; b++)
            if (!d->lang[b])
                d->offered &= ~(1u << b);
    }
}

struct check_state {
    struct ui_list list;
    int kind;                       /* 1 main, 2 music */
    int rows;                       /* offered + Reset Menu */
};
/* P11 screen arena bound (screen_stack.h) */
_Static_assert(sizeof(struct check_state) <= UI_ARENA_B_SETTING,
               "check_state: arena bound");

/* The state lives in the pinned arena block: s->priv, valid until leave */
static struct check_state *cs_of(struct ui_screen *s)
{
    struct check_state *cs = s ? s->priv : NULL;

    if (cs)
        cs->list.data = cs;
    return cs;
}

static bool is_reset_row(const struct check_state *cs, int idx)
{
    return idx == cs->rows - 1;
}

static const char *check_label(int idx, void *data, char *buf, size_t len)
{
    const struct check_state *cs = data;
    struct check_def d;

    (void)buf; (void)len;
    if (is_reset_row(cs, idx))
        return str(LANG_IPODUI_RESET_MENU);
    def_get(cs->kind, &d);
    return str(d.lang[mc_nth_bit(d.offered, idx)]);
}

static bool row_checked(const struct check_state *cs, int idx)
{
    struct check_def d;

    if (is_reset_row(cs, idx))
        return false;
    def_get(cs->kind, &d);
    return mc_checked((unsigned)*d.mask, d.offered,
                      mc_nth_bit(d.offered, idx), d.pinned);
}

/* The blue tick at the right of a checked row (the inline values' blue,
 * white on the selection), blended over what is under it: the list
 * background, or the selection bar's row colour. Row coordinates. */
#define CHECK_PAD_R 12

static void check_extra(int idx, void *data, int w, int h, bool sel)
{
    const struct ui_theme *t = ui_theme;
    const struct check_state *cs = data;
    int x0, y0, r, c;
    unsigned fg;

    if (!row_checked(cs, idx))
        return;
    x0 = w - CHECK_PAD_R - CHECK_W;
    y0 = (h - CHECK_H) / 2;
    fg = sel ? t->sel_fg : t->value_fg;
    for (r = 0; r < CHECK_H; r++)
    {
        unsigned bg = sel ? ui_paint_selection_row(y0 + r, h) : t->list_bg;

        for (c = 0; c < CHECK_W; c++)
        {
            int a = check_cov[r][c];

            if (!a)
                continue;
            lcd_set_foreground(a >= 15 ? fg : ui_paint_blend(bg, fg, a));
            lcd_drawpixel(x0 + c, y0 + r);
        }
    }
}

static int check_enter(struct ui_screen *s, const void *arg)
{
    const struct ui_theme *t = ui_theme;
    struct check_state *cs;
    struct check_def d;
    struct viewport vp;
    int kind = (int)(intptr_t)arg;

    kind = kind == 2 ? 2 : 1;
    def_get(kind, &d);
    cs = ui_arena_alloc(sizeof(*cs));
    if (!cs)
    {
        logf("ipodui: checklist: arena full");
        return -1;
    }
    s->priv = cs;
    cs->kind = kind;
    cs->rows = mc_count(d.offered) + 1;
    s->title = str(d.title);
    s->header_w = t->split_x;
    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = t->split_x;
    vp.height = LCD_HEIGHT - t->header_h;
    ui_list_init(&cs->list, &vp, cs->rows, check_label, cs);
    cs->list.draw_extra = check_extra;
    ui_panel_select_screen(s);
    return 0;
}

/* the arena takes the state back with the screen (screen_stack.c) */
static void check_leave(struct ui_screen *s)
{
    s->priv = NULL;
}

static void check_draw(struct ui_screen *s, unsigned dirty)
{
    struct check_state *cs = cs_of(s);
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    if (!cs)
        return;
    if (dirty & UI_DIRTY_BODY)
    {
        ui_list_draw(&cs->list, cs->list.vp.y, cs->list.vp.height);
        ui_panel_draw_now();
    }
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&cs->list, y[i], h[i]);
    }
}

static void row_changed(struct check_state *cs, int idx)
{
    int y, h;

    if (ui_list_row_band(&cs->list, idx, &y, &h))
        ui_invalidate_band(y, h);
    else
        ui_invalidate(UI_DIRTY_BODY);
}

static void do_select(struct check_state *cs)
{
    struct check_def d;
    int idx = cs->list.selected, bit;
    unsigned old, now;

    def_get(cs->kind, &d);
    old = (unsigned)*d.mask;
    if (is_reset_row(cs, idx))
    {
        /* back to the shipped menu; the selection goes to row 0 */
        *d.mask = (int)d.deflt;
        ui_list_move(&cs->list, -idx, (int[2]){0}, (int[2]){0});
        fe_settings_save();
        ui_invalidate(UI_DIRTY_BODY);
        UPDATE_TRACE("checklist reset kind=%d mask=%x", cs->kind, d.deflt);
        return;
    }
    bit = mc_nth_bit(d.offered, idx);
    now = mc_toggle(old, d.offered, bit, d.pinned);
    if (now == old)
        return;                         /* Settings stays checked */
    *d.mask = (int)now;
    fe_settings_save();
    row_changed(cs, idx);
    UPDATE_TRACE("checklist toggle kind=%d row=%d bit=%d mask=%x", cs->kind,
                 idx, bit, now);
}

static enum ui_result check_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    struct check_state *cs = cs_of(s);
    int y[2], h[2], i, n;

    if (!cs)
        return UI_IGNORED;
    switch (in->key)
    {
        case UI_KEY_WHEEL:
            n = ui_list_move(&cs->list, in->wheel_steps, y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            do_select(cs);
            return UI_HANDLED;          /* stays: ticks in place */
        default:
            return UI_IGNORED;
    }
}

static int check_tick(struct ui_screen *s)
{
    struct check_state *cs = cs_of(s);

    return cs ? ui_list_tick(&cs->list) : TIMEOUT_BLOCK;
}

static void check_panel(struct ui_screen *s, const struct ui_panel *p)
{
    struct check_state *cs = cs_of(s);
    struct check_def d;

    if (!cs)
    {
        lcd_set_viewport(p->vp);
        lcd_set_foreground(ui_theme->panel_bg);
        lcd_fillrect(0, 0, UI_PANEL_W, UI_PANEL_H);
        return;
    }
    def_get(cs->kind, &d);
    ui_panel_draw_tile(p, str(d.title), PANELART_MENU, str(d.caption));
}

const struct ui_screen_ops ui_checklist_screen = {
    .name    = "checklist",
    .enter   = check_enter,
    .leave   = check_leave,
    .draw    = check_draw,
    .input   = check_input,
    .timeout = NULL,
    .tick    = check_tick,
    .draw_into_panel = check_panel,
};
