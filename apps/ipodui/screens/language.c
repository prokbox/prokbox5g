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
 * or is paused.
 *
 * Language lists one row, the language's own name ("English"): v1 ships
 * English only, the language backend stays (rule 8). SELECT keeps it; it
 * never calls lang_core_load(). After Reset Settings the same list shows
 * once more and SELECT then lands on the main menu (COMP-7). */

#include "config.h"
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "lang.h"
#include "settings.h"
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

static int lang_count(void)
{
    return 1;                           /* English only in v1 (COMP-6) */
}

static int lang_get(void)
{
    return 0;
}

static int lang_set(int idx)
{
    (void)idx;                          /* the one language: nothing to load */
    return 1;
}

static int lang_set_after_reset(int idx)
{
    (void)idx;
    UPDATE_TRACE("reset: language done%s", "");
    return UI_VLIST_ROOT;               /* then the main menu (COMP-7) */
}

static const char *lang_label(int idx, char *buf, size_t len)
{
    (void)idx; (void)buf; (void)len;
    return str(LANG_IPODUI_LANG_NAME);
}

static const struct ui_vlist lang_list = {
    LANG_LANGUAGE, UI_TILE_WORDS, lang_count, lang_get, lang_set,
    lang_label, NULL };

const struct ui_vlist ui_language_after_reset = {
    LANG_LANGUAGE, UI_TILE_WORDS, lang_count, lang_get,
    lang_set_after_reset, lang_label, NULL };

UI_VLIST_SCREEN(ui_language_screen, "language", &lang_list);
