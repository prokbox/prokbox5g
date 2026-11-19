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
 * iPod UI shell
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

/* Settings > Language (docs/phase11-brief.md section 3.6; COMP-6) and the
 * split value list U2b's leaf screens share (Language, EQ, Time Zone).
 *
 * The value list is the Settings pattern photographed for Backlight
 * (backlight-values.jpg; NAV-1 for screens nobody photographed): a split
 * screen, the list at the left with the current value highlighted on
 * entry and no checkmark, the item's tile panel at the right; SELECT sets
 * the value and returns. Its state lives in the screen arena
 * (UI_ARENA_B_SETTING), so opening it never allocates while audio plays
 * or is paused. Choosing another language stops playback first and then
 * allocates with nothing playing (#487, lang_apply()'s settle step): its
 * strings' block (under 40 KB), the CJK glyph tier and pin.
 *
 * Language lists the shell's languages, each by its own name (English,
 * Deutsch, Nederlands; P20; 日本語, #266, when the CJK fonts loaded). SELECT on another one stops playback,
 * loads it (lang_core_switch(), on the back end in a split build) and
 * returns to the main menu, as the 7G does; the current one just leaves
 * the list.
 * After Reset Settings the same list shows once more and SELECT then
 * lands on the main menu (COMP-7). */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "lang.h"
#include "language.h"
#include "rbpaths.h"
#include "settings.h"
#include "string-extra.h"
#include "audio.h"
#include "hang_watch.h"
#include "ipodui/ipodui.h"
#include "ipodui/fe_be.h"
#include "ipodui/menu.h"
#include "ipodui/uilog.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/list.h"
#include "ipodui/preview.h"
#include "ipodui/screen_stack.h"
#include "ipodui/screens/panels.h"
#include "ipodui/screens/screens.h"

/* -- the value list ---------------------------------------------------------- */

struct vlist_state {
    struct ui_list list;
    const struct ui_vlist *v;
};
_Static_assert(sizeof(struct vlist_state) <= UI_ARENA_B_SETTING,
               "vlist_state: arena bound");

/* Screens a SELECT asked to leave beyond the list it was made on (a Time
 * Zone city leaves the region list too): each value list that comes back
 * on top while this is set pops itself from its tick(). */
static unsigned char pending_pops;

static const char *vlist_label(int idx, void *data, char *buf, size_t len)
{
    const struct vlist_state *vs = data;
    return vs->v->label(idx, buf, len);
}

static int vlist_enter(struct ui_screen *s, const void *arg)
{
    const struct ui_vlist *v = arg;
    const struct ui_theme *t = ui_theme;
    struct vlist_state *vs = ui_arena_alloc(sizeof(*vs));
    struct viewport vp;
    int n = v->count();

    if (!vs)
        return -1;                      /* traced by the arena */
    s->priv = vs;
    s->title = str(v->title);
    s->header_w = t->split_x;
    vs->v = v;
    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = t->split_x;
    vp.height = LCD_HEIGHT - t->header_h;
    ui_list_init(&vs->list, &vp, n, vlist_label, vs);
    vs->list.selected = MIN(MAX(v->get(), 0), MAX(n - 1, 0));
    ui_list_set_count(&vs->list, n);    /* keeps it visible */
    ui_panel_select_screen(s);
    return 0;
}

static void vlist_draw(struct ui_screen *s, unsigned dirty)
{
    struct vlist_state *vs = s->priv;
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    if (dirty & UI_DIRTY_BODY)
    {
        ui_list_draw(&vs->list, vs->list.vp.y, vs->list.vp.height);
        ui_panel_draw_now();
    }
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&vs->list, y[i], h[i]);
    }
}

static enum ui_result vlist_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    struct vlist_state *vs = s->priv;
    int y[2], h[2], i, n;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            n = ui_list_move(&vs->list, in->wheel_steps, y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            if (n && vs->v->caption)
                ui_panel_invalidate();      /* the caption follows */
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            if (vs->list.count <= 0)
                return UI_HANDLED;
            n = vs->v->set(vs->list.selected);
            if (n == UI_VLIST_ROOT)
            {
                /* leaves this screen too: touch nothing after it */
                UPDATE_TRACE("vlist to root%s", "");
                ui_pop_to_root();
                return UI_HANDLED;
            }
            if (n <= 0)
            {
                ui_invalidate(UI_DIRTY_BODY);
                return UI_HANDLED;
            }
            pending_pops = n - 1;
            return UI_POP;
        default:
            return UI_IGNORED;
    }
}

static int vlist_tick(struct ui_screen *s)
{
    struct vlist_state *vs = s->priv;

    if (pending_pops)
    {
        pending_pops--;
        ui_pop_request(s);
        return TIMEOUT_BLOCK;
    }
    return ui_list_tick(&vs->list);
}

static void vlist_panel(struct ui_screen *s, const struct ui_panel *p)
{
    struct vlist_state *vs = s->priv;
    const struct ui_vlist *v = vs->v;
    char buf[48];

    ui_panel_draw_tile(p, str(v->title), v->art,
                       v->caption ? v->caption(vs->list.selected, buf,
                                               sizeof(buf)) : NULL);
}

const struct ui_screen_ops ui_vlist_screen = {
    .name    = "vlist",
    .enter   = vlist_enter,
    .leave   = NULL,                    /* arena: released by the pop */
    .draw    = vlist_draw,
    .input   = vlist_input,
    .timeout = NULL,
    .tick    = vlist_tick,
    .draw_into_panel = vlist_panel,
};

/* -- Language ---------------------------------------------------------------- */

/* The shell's languages (P20): the .lng base name in LANG_DIR ("" is the
 * built-in English) and the language's name in its own language. The
 * names are endonyms, the same in every language, so they are not
 * language strings (rule 8 covers words that change with the language).
 * Every shell zip carries these .lng files (tools/buildzip.pl). */
static const struct {
    const char *file;
    const char *name;
} shell_langs[] = {
    { "",           "English"    },
    { "deutsch",    "Deutsch"    },
    { "nederlands", "Nederlands" },
    { "japanese",   "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" }, /* 日本語 (#266) */
};

/* Japanese needs the ShellCJK companion fonts (ui_theme_has_cjk()); without
 * them its rows would draw blank, so the list ends before it. It is last. */
static int lang_avail(void)
{
    return ui_theme_has_cjk() ? (int)ARRAYLEN(shell_langs)
                              : (int)ARRAYLEN(shell_langs) - 1;
}

static int lang_count(void)
{
    return lang_avail();
}

static int lang_get(void)
{
    const char *cur = (const char *)global_settings.lang_file;
    unsigned i;

    for (i = 1; i < (unsigned)lang_avail(); i++)
        if (!strcmp(cur, shell_langs[i].file))
            return i;
    return 0;                           /* "" or "english": built-in */
}

/* #487: the busy glyph's next dot, between two settle steps */
static void settle_tick(int *phase)
{
    *phase = *phase % 8 + 1;
    ui_header_busy_step(*phase);
    hang_beat(HANG_UI);         /* a step done: the shell's thread moves */
}

static long settle_ms(long since)
{
    return (current_tick - since) * (1000 / HZ);
}

/* Loads language 'idx' now and saves it. False (logged, nothing on
 * screen: rule 1) if its file is missing or refused; the strings in use
 * stay.
 *
 * #487 (owner, first-hand: the 7G's language change): playback stops
 * first, then one settle step does every load and allocation the new
 * language needs, one after the other with nothing playing: the strings,
 * then the CJK glyph tier and pin (ui_theme_cjk_for_lang(), no longer
 * owed to a quiet moment). The header's busy glyph steps between them
 * and each step's time is logged ("lang settle ..."). For safety, not
 * speed: with the audio stopped no allocation is cut off the playing
 * buffer's end, where a pinned block once made the next song's buffer
 * fail (the 0.9.0 road test's "audio_reset_buffer(): OOM!"). */
static bool lang_apply(int idx)
{
    char path[MAX_PATH];
    int rc = 0, phase = 0, was = audio_status();
    const long t0 = current_tick;
    long t;

    if (was)
        audio_stop();
    ui_log("lang", "settle stop was=%d ms=%ld", was, settle_ms(t0));
    settle_tick(&phase);
    t = current_tick;
    if (idx == 0)
        lang_core_builtin();
    else
    {
        int reserve = 0, i;

        /* room for the largest one, so a later switch loads in place */
        for (i = 1; i < lang_avail(); i++)
        {
            snprintf(path, sizeof(path), LANG_DIR "/%s.lng",
                     shell_langs[i].file);
            reserve = MAX(reserve, lang_core_file_size(path));
        }
        snprintf(path, sizeof(path), LANG_DIR "/%s.lng",
                 shell_langs[idx].file);
        rc = lang_core_switch(path, reserve);
    }
    UPDATE_TRACE("language set %s rc=%d",
                 idx ? shell_langs[idx].file : "english", rc);
    ui_log("lang", "settle strings rc=%d ms=%ld", rc, settle_ms(t));
    if (rc)
    {
        ui_log("lang", "%s refused (%d)", shell_langs[idx].file, rc);
        ui_header_busy_step(0);
        return false;
    }
    strmemccpy((char *)global_settings.lang_file, shell_langs[idx].file,
               sizeof(global_settings.lang_file));
    t = current_tick;
    fe_settings_save();
    ui_log("lang", "settle save ms=%ld", settle_ms(t));
    settle_tick(&phase);
    t = current_tick;
    ui_theme_cjk_for_lang();    /* Japanese: the larger CJK glyph cache */
    ui_log("lang", "settle glyphs ms=%ld", settle_ms(t));
    settle_tick(&phase);
    /* the labels the shell keeps outside its screens: Settings at shell
     * start, the main and Music menus at menu entry */
    ui_settings_init();
    ui_menu_relabel();
    ui_header_busy_step(0);
    ui_log("lang", "settle done %s ms=%ld",
           idx ? shell_langs[idx].file : "english", settle_ms(t0));
    return true;
}

/* A new language goes back to the main menu, which redraws in it: every
 * screen above it holds titles of the old one. The same language just
 * leaves the list. */
static int lang_set(int idx)
{
    if (idx == lang_get() || !lang_apply(idx))
        return 1;
    return UI_VLIST_ROOT;
}

static int lang_set_after_reset(int idx)
{
    if (idx != lang_get())
        lang_apply(idx);
    UPDATE_TRACE("reset: language done%s", "");
    return UI_VLIST_ROOT;               /* then the main menu (COMP-7) */
}

static const char *lang_label(int idx, char *buf, size_t len)
{
    (void)buf; (void)len;
    return shell_langs[idx].name;
}

static const struct ui_vlist lang_list = {
    LANG_LANGUAGE, UI_TILE_WORDS, lang_count, lang_get, lang_set,
    lang_label, NULL };

const struct ui_vlist ui_language_after_reset = {
    LANG_LANGUAGE, UI_TILE_WORDS, lang_count, lang_get,
    lang_set_after_reset, lang_label, NULL };

UI_VLIST_SCREEN(ui_language_screen, "language", &lang_list);
