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
 * iPod UI shell: the On-The-Go list (Music > Playlists > On-The-Go)
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
/* The On-The-Go store (otg.h, L) as a list: its songs as two-line rows,
 * title over artist like Songs (⚠️ H4), then "Save Playlist" and "Clear
 * Playlist" at the end (both only while it holds songs). The rows come
 * from the store's LIB_OTG view (otg_open_view(), which resolves the
 * store once after a database commit, before our first draw).
 *   SELECT on a song      plays On-The-Go from it (otg_play(): a fresh
 *                         queue built from the store's paths; the store
 *                         itself is never the playlist, Rev 1 H-2)
 *   hold SELECT on a song the hold menu with Remove from On-The-Go
 *   Save Playlist         "New Playlist N" in /Playlists, the store
 *                         emptied (otg_save()); back to Playlists, which
 *                         lists the new file
 *   Clear Playlist        a light prompt, Clear | Cancel (Cancel
 *                         preselected); Clear empties the store and goes
 *                         back to Playlists
 *
 * The list strip: otg_play() and otg_save() borrow the list strip for
 * their copy buffers (Rev 1 M-2), so they only run with no glide in
 * flight (a glide is landed first) and the list redraws in full after.
 *
 * State: the screen arena (UI_ARENA_B_OTG); the rows live in the view
 * pool. The Clear prompt keeps its segment in s->priv (no state). */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "file.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "lang.h"
#include "logf.h"
#include "ipodui/ipodui.h"
#include "ipodui/theme.h"
#include "ipodui/list.h"
#include "ipodui/anim.h"
#include "ipodui/otg.h"
#include "ipodui/npmark.h"
#include "ipodui/screens/screens.h"
#include "ipodui/screens/holdmenu.h"
#include "ipodui/screens/setwidgets.h"

#define OTG_ACTIONS     2       /* Save Playlist, Clear Playlist */

struct otgl {
    struct ui_list list;
    int  view;                  /* LIB_OTG view, <= 0: none */
    int  n;                     /* songs */
    struct ui_npmark_now np;    /* #69: what plays, as the rows show it */
};
_Static_assert(sizeof(struct otgl) <= UI_ARENA_B_OTG, "otg list: arena bound");

static bool otg_stale;          /* ui_otg_changed(): reopen on the next
                                   full draw */

void ui_otg_changed(void)
{
    otg_stale = true;
}

static int rows_of(int n)
{
    return n > 0 ? n + OTG_ACTIONS : 0;
}

static const char *otg_label(int row, void *data, char *buf, size_t len)
{
    const struct otgl *o = data;

    if (row < 0 || row >= o->n)
        return "";              /* the actions: drawn by otg_extra() */
    return lib_label(o->view, row, buf, len);
}

static const char *otg_sublabel(int row, void *data, char *buf, size_t len)
{
    const struct otgl *o = data;
    const char *s;

    if (row < 0 || row >= o->n)
        return NULL;
    s = lib_sublabel(o->view, row, buf, len);
    if (!s)
        return NULL;
    return s[0] ? s : (const char *)str(LANG_IPODUI_UNKNOWN_ARTIST);
}

/* #69: the playing song's row(s), by database id (an unresolved row,
 * lib_id() -2, never matches) */
static bool otg_mark(int row, void *data)
{
    const struct otgl *o = data;

    return row < o->n && o->np.tc > 0 && lib_id(o->view, row) == o->np.tc - 1;
}

/* Save Playlist / Clear Playlist: one line, centred in the two-line row */
static void otg_extra(int row, void *data, int w, int h, bool sel)
{
    const struct ui_theme *t = ui_theme;
    const struct otgl *o = data;
    int a = row - o->n;

    (void)w;
    if (a < 0 || a >= OTG_ACTIONS)
        return;
    ui_row_text(str(a == 0 ? LANG_IPODUI_SAVE_PLAYLIST
                           : LANG_IPODUI_CLEAR_PLAYLIST),
                o->list.style.text_x, h, sel ? t->sel_fg : t->list_fg);
}

static void open_view(struct otgl *o)
{
    if (o->view > 0)
        lib_close(o->view);
    o->view = otg_open_view();
    o->n = o->view > 0 ? lib_count(o->view) : 0;
    if (o->n < 0)
        o->n = 0;
    otg_stale = false;
    ui_list_set_count(&o->list, rows_of(o->n));
}

static int otg_enter(struct ui_screen *s, const void *arg)
{
    const struct ui_theme *t = ui_theme;
    struct otgl *o = ui_screen_alloc(s, sizeof(struct otgl));
    struct ui_row_style st;
    struct viewport vp;

    if (!o)
        return -1;
    s->title = arg ? arg : str(LANG_IPODUI_ON_THE_GO);
    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;
    ui_list_init(&o->list, &vp, 0, otg_label, o);
    memset(&st, 0, sizeof(st));
    st.rows = t->list_rows_two;         /* like Songs (⚠️ H4) */
    st.text_x = t->text_pad_x;
    st.flags = UI_ROW_TWO_LINE | UI_ROW_SEP;
    ui_list_set_style(&o->list, &st);
    o->list.get_sublabel = otg_sublabel;
    o->list.draw_extra = otg_extra;
    o->list.has_mark = otg_mark;
    ui_npmark_get(&o->np, UI_NPMARK_TC);
    open_view(o);                       /* resolves before our first draw */
    return 0;
}

static void otg_leave(struct ui_screen *s)
{
    struct otgl *o = s->priv;

    if (o->view > 0)
        lib_close(o->view);
    ui_screen_free(s);
}

static void otg_draw(struct ui_screen *s, unsigned dirty)
{
    struct otgl *o = s->priv;
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    if (dirty & UI_DIRTY_BODY)
    {
        if (otg_stale)              /* a Remove (or an Add) just now */
            open_view(o);
        ui_list_draw(&o->list, o->list.vp.y, o->list.vp.height);
    }
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&o->list, y[i], h[i]);
    }
    lib_trace_frame();
}

static int otg_tick(struct ui_screen *s)
{
    struct otgl *o = s->priv;
    int wait = ui_list_tick(&o->list);

    ui_npmark_sync(&o->list, &o->np, UI_NPMARK_TC);
    return wait;
}

/* Lands a glide (one full draw) before an action borrows the list strip;
 * the list is redrawn in full after it anyway. */
static void land(struct otgl *o)
{
    if (ui_list_gliding(&o->list))
        ui_list_draw(&o->list, o->list.vp.y, o->list.vp.height);
}

static const struct ui_screen_ops otg_clear_screen;

static enum ui_result otg_select(struct otgl *o)
{
    char name[MAX_PATH];
    int row = o->list.selected, rc;

    land(o);
    if (row < o->n)
    {
        if (ui_npmark_continue(&o->list, &o->np, UI_NPMARK_TC,
                               UI_NPMARK_ORIGIN_OTG, row))
            return UI_HANDLED;
        rc = otg_play(row);
        if (rc >= 0)
        {
            ui_npmark_origin(UI_NPMARK_ORIGIN_OTG);
            ui_show_wps();
        }
        else
            logf("ipodui: otg: play %d failed (%d)", row, rc);
        ui_invalidate(UI_DIRTY_BODY);
        return UI_HANDLED;
    }
    if (row == o->n)
    {
        rc = otg_save(name, sizeof(name));
        UPDATE_TRACE("otg list save rc=%d", rc);
        if (rc < 0)
        {
            ui_invalidate(UI_DIRTY_BODY);
            return UI_HANDLED;
        }
        ui_pl_changed();            /* Playlists lists "New Playlist N" */
        ui_otg_changed();
        return UI_POP;
    }
    ui_push(&otg_clear_screen, NULL);
    return UI_HANDLED;
}

static enum ui_result otg_input(struct ui_screen *s, const struct ui_input *in)
{
    struct otgl *o = s->priv;
    struct ui_hold_arg a;
    int y[2], h[2], i, n, row, id;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            n = ui_list_move(&o->list, in->wheel_steps, y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            return UI_HANDLED;

        case UI_KEY_SELECT:
            if (o->list.count <= 0)
                return UI_HANDLED;
            row = o->list.selected;
            if (in->long_press && !in->release)
            {
                if (row >= o->n)
                    return UI_HANDLED;  /* the actions have no menu */
                id = lib_id(o->view, row);
                memset(&a, 0, sizeof(a));
                a.ctx = UI_HOLD_IN_OTG;
                a.view = o->view;
                a.idx = row;
                a.otg_pos = row;
                a.tc_idx = id >= 0 ? id + 1 : 0;   /* unresolved: none */
                a.db_gen = ui_hold_db_gen();
                ui_hold_push(&a);
                return UI_HANDLED;
            }
            if (!in->release || in->long_press)
                return UI_HANDLED;
            return otg_select(o);

        default:
            return UI_IGNORED;
    }
}

const struct ui_screen_ops ui_otg_screen = {
    .name    = "otg",
    .enter   = otg_enter,
    .leave   = otg_leave,
    .draw    = otg_draw,
    .input   = otg_input,
    .timeout = NULL,
    .tick    = otg_tick,
};

/* -- Clear Playlist: the light prompt, Clear | Cancel ---------------------- */

static const short clear_seg_lang[2] = { LANG_IPODUI_CLEAR, LANG_CANCEL_0 };
#define CLEAR_SEG_CANCEL 1          /* preselected */

static int clear_get(const struct ui_screen *s)
{
    return (int)(intptr_t)s->priv;
}

static int clear_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
    s->title = str(LANG_IPODUI_ON_THE_GO);
    s->priv = (void *)(intptr_t)CLEAR_SEG_CANCEL;
    return 0;
}

static void clear_draw(struct ui_screen *s, unsigned dirty)
{
    if (dirty & UI_DIRTY_BODY)
    {
        ui_prompt_draw(LANG_IPODUI_CLEAR_OTG_HEADING,
                       LANG_IPODUI_CLEAR_OTG_BODY);
        ui_seg_draw(clear_seg_lang, 2, clear_get(s), true);
    }
    else if (dirty & UI_DIRTY_BAND)
        ui_seg_draw(clear_seg_lang, 2, clear_get(s), false);
}

static enum ui_result clear_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    int v = clear_get(s), d = 0, rc;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            d = in->wheel_steps;
            break;
        case UI_KEY_NEXT:
        case UI_KEY_PREV:
            if (!in->release || in->long_press)
                return UI_HANDLED;  /* a step on the release, no seek */
            d = in->key == UI_KEY_NEXT ? 1 : -1;
            break;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            if (v == CLEAR_SEG_CANCEL)
                return UI_POP;
            rc = otg_clear();
            UPDATE_TRACE("otg list clear rc=%d", rc);
            (void)rc;               /* the trace compiles out on target */
            ui_otg_changed();
            ui_pop();               /* this prompt; then the list */
            return UI_POP;
        default:
            return UI_IGNORED;      /* MENU: back, nothing cleared */
    }
    v += d;
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    if (v != clear_get(s))
    {
        s->priv = (void *)(intptr_t)v;
        ui_invalidate_band(UI_SEG_Y, UI_SEG_H);
    }
    return UI_HANDLED;
}

static const struct ui_screen_ops otg_clear_screen = {
    .name    = "otg-clear",
    .enter   = clear_enter,
    .leave   = NULL,
    .draw    = clear_draw,
    .input   = clear_input,
    .timeout = NULL,
};
