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
 * iPod UI shell: Audiobooks and Podcasts lists
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
/* The Audiobooks list (one row per book; the list itself is the browse
 * screen), a book's page, and the Podcasts list.
 *
 * Audiobooks (Phase 6, owner Rev 2): the list shows no progress at all
 * (no dots, no bar: the 7G only knows whether a resume spot exists).
 * Selecting any book opens its page (photo audiobook-open-chapters-b):
 * the header is the book title, then a "Resume" row only when a saved
 * spot exists (ab_page_has_resume()), then the chapter or file entries,
 * full-width, single-line, no chevrons. SELECT on Resume continues the
 * book; on an entry, plays the book from that entry's start. The entries
 * come from ab_page_*() (B: the library's chapter view); with no page
 * (ab_page_open() < 0, e.g. the shared stub) the list plays the book
 * itself, as Phase 5 did.
 *
 * Podcasts (P11, ⚠️ P1): shows and episodes carry the blue unplayed dot
 * while never started (lib_unplayed(): no play counted, no saved
 * position, not playing now) and nothing once started; no progress bar
 * (our earlier glossy bar was not stock).
 *
 * State: the book page's lives in the screen arena (P11). */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "lang.h"
#include "logf.h"
#include "ipodui/ipodui.h"
#include "ipodui/theme.h"
#include "ipodui/list.h"
#include "ipodui/ab_book.h"
#include "ipodui/npmark.h"
#include "ipodui/screens/screens.h"

#define ANY LIB_FILTER_ANY

/* The titles are language strings, filled by menu.c's labels_init()
 * before either is pushed. */
struct ui_browse_arg ui_audiobooks_arg = {
    NULL, LIB_AUDIOBOOKS, ANY, UI_ALL_NONE, false
};
struct ui_browse_arg ui_podcasts_arg = {
    NULL, LIB_PODCASTS, ANY, UI_ALL_NONE, false
};

/* -- a book's page ---------------------------------------------------------- */

#define PAGE_TITLE_LEN  40

struct book_page {
    struct ui_list list;
    bool resume;                /* the "Resume" row is shown */
    uint64_t key;               /* #69: the book's key (ab_page_key()) */
    struct ui_npmark_now np;    /* #69: what plays, as the rows show it */
    char title[PAGE_TITLE_LEN]; /* the header's (s->title points here;
                                   the arena never moves) */
};
/* P11 screen arena bound (screen_stack.h): the state moves into the
 * arena */
_Static_assert(sizeof(struct book_page) <= UI_ARENA_B_BOOK_PAGE, "book_page: arena bound");

static struct book_page *page_state(struct ui_screen *s)
{
    return s->priv;
}

static int page_rows(bool resume)
{
    return (resume ? 1 : 0) + ab_page_entries();
}

static const char *page_label(int row, void *data, char *buf, size_t len)
{
    const struct book_page *p = data;
    int i = row - (p->resume ? 1 : 0);

    if (i < 0)
        return str(LANG_IPODUI_RESUME);
    ab_page_entry_title(i, buf, len);
    if (!buf[0])                /* untitled: its position (⚠️6) */
        snprintf(buf, len, str(LANG_IPODUI_CHAPTER_N), i + 1);
    return buf;
}

/* #69: the entry the playing position is in, when this book plays (not
 * the Resume row) */
static bool page_mark(int row, void *data)
{
    const struct book_page *p = data;
    int i = row - (p->resume ? 1 : 0);

    return i >= 0 && p->key && p->np.book == p->key && p->np.entry == i;
}

static int page_enter(struct ui_screen *s, const void *arg)
{
    const struct ui_book_page_arg *a = arg;
    const struct ui_theme *t = ui_theme;
    struct book_page *p;
    struct viewport vp;

    /* the page first (it lives in the library's view pool), then ours
     * (the arena) */
    if (ab_page_open(a->view, a->idx) < 0)
    {
        ab_page_close();
        return -1;                  /* no page: the caller plays the book */
    }
    p = ui_screen_alloc(s, sizeof(struct book_page));
    if (!p)
    {
        logf("ipodui: book page: no memory");
        ab_page_close();
        return -1;
    }
    strlcpy(p->title, a->title ? a->title : "", sizeof(p->title));
    s->title = p->title;
    p->resume = ab_page_has_resume();
    p->key = ab_page_key();

    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;
    ui_list_init(&p->list, &vp, 0, page_label, p);
    ui_list_set_count(&p->list, page_rows(p->resume));
    p->list.has_mark = page_mark;
    ui_npmark_get(&p->np, UI_NPMARK_BOOK);
    return 0;                       /* Resume (or the first entry) selected */
}

static void page_leave(struct ui_screen *s)
{
    ab_page_close();
    ui_screen_free(s);
}

/* Back from Now Playing: a spot may have been saved (or cleared: a
 * finished book), so the Resume row comes or goes; the selection stays
 * on the same item. */
static void page_sync_resume(struct book_page *p)
{
    bool r = ab_page_has_resume();
    int sel = p->list.selected;

    if (r == p->resume)
        return;
    p->resume = r;
    sel += r ? 1 : -1;
    p->list.selected = sel < 0 ? 0 : sel;
    ui_list_set_count(&p->list, page_rows(r));
}

static void page_draw(struct ui_screen *s, unsigned dirty)
{
    struct book_page *p = page_state(s);
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    if (dirty & UI_DIRTY_BODY)
    {
        page_sync_resume(p);
        ui_list_draw(&p->list, p->list.vp.y, p->list.vp.height);
    }
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&p->list, y[i], h[i]);
    }
}

static enum ui_result page_input(struct ui_screen *s,
                                 const struct ui_input *in)
{
    struct book_page *p = page_state(s);
    int y[2], h[2], i, n;
    bool ok;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            n = ui_list_move(&p->list, in->wheel_steps, y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            return UI_HANDLED;

        case UI_KEY_SELECT:
            if (!in->release || in->long_press || p->list.count <= 0)
                return UI_HANDLED;
            if (ui_npmark_continue(&p->list, &p->np, UI_NPMARK_BOOK, 0, -1))
                return UI_HANDLED;
            i = p->list.selected - (p->resume ? 1 : 0);
            ok = i < 0 ? ab_page_resume() : ab_page_play_entry(i);
            if (ok)
                ui_show_wps();
            else
                logf("ipodui: book page: play %d failed", i);
            return UI_HANDLED;

        default:
            return UI_IGNORED;
    }
}

static int page_tick(struct ui_screen *s)
{
    struct book_page *p = page_state(s);
    int wait = ui_list_tick(&p->list);

    ui_npmark_sync(&p->list, &p->np, UI_NPMARK_BOOK);
    return wait;
}

const struct ui_screen_ops ui_book_page_screen = {
    .name    = "book-page",
    .enter   = page_enter,
    .leave   = page_leave,
    .draw    = page_draw,
    .input   = page_input,
    .timeout = NULL,
    .tick    = page_tick,
};
