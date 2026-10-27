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

/* iPod UI shell: Extras > Notes (P12 W1-N, MOD_NOTES, Class A; spec
 * extras.md, owner photo 1200). A full-width list of the /Notes
 * folder (notes and folders mixed, by name; a folder row has the chevron,
 * a note none), a folder's list (one level, stock) and the note reader:
 * light, under the normal header titled with the note's name, its first
 * 4 KB (stock) word-wrapped in the small type, the wheel a line per step,
 * PREVIOUS/NEXT a page, a scrollbar thumb on a long note. MENU backs out.
 *
 * Storage (brief 2.1, Class A): the list reads the sync-time index
 * (notes.idx, built by MOD_XINDEX at the post-USB moment or the boot
 * window), never the folder. Entry: one open and one read of the
 * index's head (its header, page directory and first page; the whole
 * index when it fits the cache); a note: one open and one read of its
 * first 4 KB. A longer index is paged (xindex_core.h): the entry read
 * keeps every page it brought; the rows around the selection come a
 * page at a time (12 rows at least, ~16 typical, 512-724 B), when a
 * folder opens or closes and when the selection leaves the cached pages,
 * one open and one range read each, booked as a page load
 * (modclass_page(), the manifest's pages rate; logged "notes page"). A
 * name longer than a row's label is read whole when it opens (one more
 * range read, booked the same way). While a paged list is used the drive
 * is held awake and the next page read ahead when that costs no spin-up
 * (docs/storage-choices.md 5.1).
 *
 * Text: UTF-8 (a BOM skipped), UTF-16 with a BOM, or else Windows-1252.
 * The type covers Latin up to Extended-B and the common punctuation; any
 * other character shows as a hollow box (counted, "notes view missing="),
 * never as a blank or a guess (other scripts land with P22's languages).
 *
 * Memory: the module's one working block (mod_work_alloc(), from the
 * root's enter): both lists' state, the open folder's name, the reader's
 * text and line table, and the index cache. No other allocation. */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "string-extra.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/anim.h"
#include "ipodui/list.h"
#include "ipodui/screen_stack.h"
#include "ipodui/modload.h"
#include "ipodui/modclass.h"
#include "ipodui/xindex.h"
#include "ipodui/screens/notes.h"

/* The working block: 8 KB from the screen arena in a resident build,
 * 16 KB from the module arena (notes.mod). Equal sizes but the cache. */
#if defined(IPODUI_MODULES)
#define NOTES_WORK_KB   16
#else
#define NOTES_WORK_KB   8
#endif
#define NOTES_WORK      (NOTES_WORK_KB * 1024)

#define LINES_MAX   320             /* wrapped lines of a 4 KB note */
#define TITLE_MAX   96              /* the reader's header title, bytes */
#define MARGIN      10
#define SBAR_W      7
#define TEXT_W      (LCD_WIDTH - 2 * MARGIN - SBAR_W)
#define LINE_PITCH  15
#define TOP_PAD     8
#define BOX_W       9               /* a character the type lacks */
#define TAB_SPACES  4
#define RUN_MAX     120             /* UTF-8 bytes drawn in one call */
/* Page loads a second from browsing (contacts.c has the rule): a window's
 * pair while the last second's stays at or under LOAD_MAX - 2, a
 * prefetch under it; NOTES_PAGES - LOAD_MAX kept for the entries' */
#define LOAD_MAX    (NOTES_PAGES - 2)

enum { ENC_UTF8, ENC_U16LE, ENC_U16BE, ENC_1252 };

struct notes_list {
    struct ui_list list;
    uint32_t first, count;          /* the folder's record range */
};

struct notes_work {
    /* the index: identity, layout, and the cached window of it */
    uint32_t sig, count, size, str_off, str_bytes;
    uint16_t npage, hdr_end;        /* the directory ends at hdr_end */
    uint8_t  seq, ok, whole;        /* whole: the file is in the cache */
    struct xidx_pcache pc;          /* pages past the head (not whole) */
    struct xidx_rate rate;          /* the page loads' ticks */
    /* the open folder: its record, list and name (the note's path) */
    uint32_t dir_rec;
    struct notes_list lists[2];     /* the root, the open folder */
    char folder[XNOTE_NAME_MAX + 1];
    /* the reader */
    char title[TITLE_MAX];
    uint16_t len, lines, top, rows, missing;
    uint8_t enc, start;             /* encoding, the BOM's bytes */
    uint16_t line[LINES_MAX];
    uint8_t text[XNOTE_SHOW_MAX];
    uint8_t cache[] __attribute__((aligned(4)));   /* the index cache:
                                       the head and the page cache, or the
                                       whole file */
};
#define CACHE_BYTES (NOTES_WORK - sizeof(struct notes_work))
/* the largest head (header, table, directory) and the two largest pages
 * a window spans (each padded to 4 bytes) */
_Static_assert(sizeof(struct notes_work) + XNOTE_HEAD_MAX
               + 2 * (XNOTE_PAGE_LIMIT + 3) <= NOTES_WORK,
               "notes: the cache holds a head and two pages");

static struct notes_work *nw;       /* the root's enter to its leave */

static const struct ui_screen_ops notes_dir_screen, notes_view_screen;

/* The class accountant (modclass.h; sim and developer builds, no-ops in
 * a release build). brief 2.1: "at most 2 reads per entry (... one
 * prepared blob such as notes.idx, or one note file)": extras_open()
 * opens the root's entry window; a folder or a note opened inside Notes
 * opens another (modclass_entry()), marked before its reads. */
/* -- the index cache ------------------------------------------------------- */

static const struct xidx_pdir *dir_of(const struct notes_work *w)
{
    return (const struct xidx_pdir *)(const void *)(w->cache
                                                    + sizeof(struct xidx_hdr)
                                                    + sizeof(struct xnote_tab));
}

static bool page_ok(const struct notes_work *w, int p, const void *pg)
{
    const struct xidx_pdir *d = dir_of(w);

    return xidx_page_check(pg, &d[p], xidx_page_rows(d, w->npage, p,
                                                     w->count),
                           sizeof(struct xnote_rec)) == XIDX_OK;
}

/* Reads page p: one open, one range read (a page load) */
static const void *load_page(struct notes_work *w, int p)
{
    const struct xidx_pdir *d = &dir_of(w)[p];
    void *at;
    int fd;
    bool ok = false;

    if (xindex_busy())
        return NULL;                    /* the index is being replaced */
    at = xidx_pc_room(&w->pc, d->bytes);
    if (!at)
        return NULL;
    xidx_rate_add(&w->rate, current_tick);
    modclass_page(true);
    fd = open(XNOTES_IDX, O_RDONLY);
    if (fd >= 0)
    {
        ok = lseek(fd, d->off, SEEK_SET) >= 0
             && read(fd, at, d->bytes) == d->bytes;
        close(fd);
    }
    modclass_page(false);
    ok = ok && page_ok(w, p, at);
    UPDATE_TRACE("notes page p=%d rows=%lu ok=%d", p,
                 (unsigned long)xidx_page_rows(dir_of(w), w->npage, p,
                                               w->count), ok ? 1 : 0);
    if (!ok)
        return NULL;
    xidx_pc_add(&w->pc, 0, p, d->bytes);
    return at;
}

/* Record r's page (cached; load: read it when it is not) */
static const void *page_get(struct notes_work *w, uint32_t r, uint32_t *i,
                            bool load)
{
    const struct xidx_pdir *d;
    const void *pg;
    int p;

    if (!w->ok || r >= w->count)
        return NULL;
    d = dir_of(w);
    p = xidx_dir_find(d, w->npage, r);
    if (p < 0)
        return NULL;
    *i = r - d[p].row0;
    if (w->whole)
        return w->cache + d[p].off;     /* checked at entry */
    pg = xidx_pc_get(&w->pc, 0, p);
    if (!pg && load)
        pg = load_page(w, p);
    return pg;
}

static const struct xnote_rec *rec_get(struct notes_work *w, uint32_t r)
{
    uint32_t i;
    const void *pg = page_get(w, r, &i, false);

    return pg ? xidx_page_rec(pg, i, sizeof(struct xnote_rec)) : NULL;
}

/* the file name of record r (with its extension), or NULL: not cached */
static const char *name_get(struct notes_work *w, uint32_t r,
                            const struct xnote_rec **out)
{
    uint32_t i;
    const void *pg = page_get(w, r, &i, false);
    const struct xnote_rec *e = pg ? xidx_page_rec(pg, i, sizeof(*e)) : NULL;
    const char *name = e ? xidx_page_str(pg, e->name_off) : NULL;

    if (name && out)
        *out = e;
    return name;
}

/* Record e's file name (with its extension) into buf: its label, or a
 * name too long for one from the string block (in the cache when the
 * file is whole, else one open and one range read, booked as a page
 * load). NULL: not readable. */
static const char *full_name(struct notes_work *w, const struct xnote_rec *e,
                             const char *label, char *buf, size_t len)
{
    uint32_t off, n;
    bool ok = false;
    int fd;

    if (!(e->path_off & XNOTE_PATH_FAR))
        return label;
    off = e->path_off & ~XNOTE_PATH_FAR;
    if (off >= w->str_bytes || len < 2)
        return NULL;
    n = MIN(w->str_bytes - off, (uint32_t)len);
    if (w->whole)
    {
        memcpy(buf, w->cache + w->str_off + off, n);
        ok = true;
    }
    else if (!xindex_busy())
    {
        xidx_rate_add(&w->rate, current_tick);
        modclass_page(true);
        fd = open(XNOTES_IDX, O_RDONLY);
        if (fd >= 0)
        {
            ok = lseek(fd, w->str_off + off, SEEK_SET) >= 0
                 && read(fd, buf, n) == (ssize_t)n;
            close(fd);
        }
        modclass_page(false);
    }
    /* the name ends inside what was read */
    return ok && memchr(buf, '\0', n) ? buf : NULL;
}

/* Entry: one read of the file's head (2 storage commands with the open):
 * the whole file when it fits the cache */
static void load_index(struct notes_work *w)
{
    const struct xidx_hdr *h = (const struct xidx_hdr *)w->cache;
    const struct xnote_tab *tab = (const struct xnote_tab *)(h + 1);
    ssize_t got = 0;
    off_t size = 0;
    int fd, rc = XIDX_ESHORT, p;

    w->ok = w->whole = 0;
    w->count = 0;
    w->seq = xindex_seq();
    fd = open(XNOTES_IDX, O_RDONLY);
    if (fd >= 0)
    {
        size = ffilesize(fd);
        got = read(fd, w->cache, CACHE_BYTES);
        close(fd);
    }
    if (got >= (ssize_t)(sizeof(*h) + sizeof(*tab)) && size > 0)
        rc = xidx_check(h, (uint32_t)size);
    if (rc == XIDX_OK && (h->kind != XIDX_NOTES || h->rec_off != sizeof(*h)))
        rc = XIDX_EKIND;
    /* the head: the directory inside what was read (a cache too small
     * for it shows the empty list, logged with head= and cache=) */
    if (rc == XIDX_OK
        && (tab->hdr_end != sizeof(*h) + sizeof(*tab)
                            + tab->npage * sizeof(struct xidx_pdir)
            || tab->hdr_end > (uint32_t)got))
        rc = XIDX_EBOUNDS;
    if (rc == XIDX_OK && got == size)
        rc = xidx_check_body(w->cache, (uint32_t)size);
    if (rc == XIDX_OK)
    {
        w->sig = h->sig;
        w->count = h->count;
        w->npage = tab->npage;
        w->hdr_end = tab->hdr_end;
        w->size = (uint32_t)size;
        w->str_off = h->str_off;
        w->str_bytes = h->str_bytes;
        w->whole = got == size;
        rc = w->npage > XNOTE_PDIR_MAX ? XIDX_EBOUNDS
             : xidx_dir_check(dir_of(w), w->npage, w->count, w->hdr_end,
                              w->str_off);
        for (p = 0; w->whole && rc == XIDX_OK && p < w->npage; p++)
            if (!page_ok(w, p, w->cache + dir_of(w)[p].off))
                rc = XIDX_EBOUNDS;
    }
    if (rc == XIDX_OK && !w->whole)
    {
        const struct xidx_pdir *d = dir_of(w);

        /* the page cache after the head: every page the entry read
         * brought (page 0, then the ones after it in file order) */
        xidx_pc_init(&w->pc, w->cache + w->hdr_end, CACHE_BYTES - w->hdr_end);
        for (p = 0; p < w->npage && w->pc.n < XIDX_PC_SLOTS
                    && d[p].off == w->hdr_end + w->pc.used
                    && d[p].off + d[p].bytes <= (uint32_t)got
                    && page_ok(w, p, w->pc.buf + w->pc.used); p++)
            xidx_pc_add(&w->pc, 0, p, d[p].bytes);
    }
    w->ok = rc == XIDX_OK;
    UPDATE_TRACE("notes open n=%lu whole=%d rc=%d bytes=%ld pages=%u "
                 "head=%u cache=%u",
                 (unsigned long)(w->ok ? w->count - 1 : 0), w->whole, rc,
                 (long)size, w->npage, w->hdr_end, (unsigned)CACHE_BYTES);
}

/* The rows a window at top (of list l) shows, with the one past either
 * end the scroll's animation draws */
static void window_rows(const struct notes_list *l, int top, uint32_t *lo,
                        uint32_t *hi)
{
    uint32_t t = l->first + MAX(top, 0);

    *lo = t > l->first ? t - 1 : t;
    *hi = MIN(t + (uint32_t)l->list.style.rows, l->first + l->count - 1);
}

/* Every page of the window's rows is in the cache (a peek) */
static bool window_cached(const struct notes_work *w,
                          const struct notes_list *l, int top)
{
    const struct xidx_pdir *d = dir_of(w);
    uint32_t lo, hi;
    int p, p1;

    window_rows(l, top, &lo, &hi);
    p = xidx_dir_find(d, w->npage, lo);
    p1 = xidx_dir_find(d, w->npage, hi);
    for (; p >= 0 && p <= p1; p++)
        if (!xidx_pc_peek(&w->pc, 0, p))
            return false;
    return p >= 0;
}

static unsigned loads_recent(const struct notes_work *w)
{
    return xidx_rate_recent(&w->rate, current_tick, HZ);
}

/* A wheel move of steps rows, or fewer: with the second's budget spent
 * (LOAD_MAX), only as far as the cached pages show whole windows */
static int clamp_steps(const struct notes_work *w, const struct notes_list *l,
                       int steps)
{
    int one = steps > 0 ? 1 : -1;

    if (!w->ok || w->whole || !steps || loads_recent(w) + 2 <= LOAD_MAX)
        return steps;
    while (steps && !window_cached(w, l, ui_list_top_after(&l->list, steps)))
        steps -= one;
    UPDATE_TRACE("notes held steps=%d", steps);
    return steps;
}

/* The rows on screen at top are cached (paged mode only): every page of
 * the rows from just above the list's top to just below its last visible row
 * (two at most: a page holds XIDX_PAGE_ROWS_MIN rows), read when they
 * come into view; the cache keeps two of the largest pages beside the
 * largest head (the static assert above). Then, the drive warm and the
 * second's budget not near its end, the next page the way the list moves
 * (dir) is read ahead when the cache holds it beside the visible pages
 * (docs/storage-choices.md "Paged lists"). */
static void keep_window(struct notes_work *w, struct notes_list *l, int top,
                        int dir)
{
    const struct xidx_pdir *d;
    uint32_t lo, hi, slo, shi, i;
    int p, p0, p1;

    if (!w->ok || w->whole || !l->count)
        return;
    d = dir_of(w);
    /* the rows a glide still shows on its way (cached: they are on
     * screen), then the window's, read if they are not: the newest */
    window_rows(l, ui_list_shown_top(&l->list), &slo, &shi);
    page_get(w, slo, &i, false);
    page_get(w, shi, &i, false);
    window_rows(l, top, &lo, &hi);
    p0 = xidx_dir_find(d, w->npage, lo);
    p1 = xidx_dir_find(d, w->npage, hi);
    if (p0 < 0 || p1 < 0)
        return;
    for (p = p0; p <= p1; p++)
        page_get(w, d[p].row0, &i, true);
    page_get(w, lo, &i, true);
    p = dir > 0 ? p1 + 1 : dir < 0 ? p0 - 1 : -1;
    if (p >= 0 && p < w->npage && d[p].row0 < l->first + l->count
        && (p + 1 >= w->npage || d[p + 1].row0 > l->first)
        && !xidx_pc_peek(&w->pc, 0, p)
        && loads_recent(w) + 1 <= LOAD_MAX - 2
        && w->pc.cap >= 3 * (XNOTE_PAGE_LIMIT + 3) && xindex_disk_warm())
    {
        load_page(w, p);
        page_get(w, hi, &i, false);     /* the visible ones stay newer */
        page_get(w, lo, &i, false);
    }
}

/* The folder record's range */
static void dir_range(struct notes_work *w, uint32_t r,
                      uint32_t *first, uint32_t *count)
{
    const struct xnote_rec *e = rec_get(w, r);

    *first = *count = 0;
    if (e && (e->size & XNOTE_DIR) && XNOTE_FIRST(e->size) <= w->count
        && XNOTE_COUNT(e->size) <= w->count - XNOTE_FIRST(e->size))
    {
        *first = XNOTE_FIRST(e->size);
        *count = XNOTE_COUNT(e->size);
    }
}

/* -- the lists ------------------------------------------------------------- */

/* A label with every character the type lacks as '?' (the list draws
 * labels itself; the reader draws boxes) */
static bool has_glyph(const struct font *pf, uint32_t cp);

static int utf8_get(const uint8_t *s, int len, int *i);

static const char *row_label(int idx, void *data, char *buf, size_t len)
{
    struct notes_list *l = data;
    const struct xnote_rec *e;
    const char *name = name_get(nw, l->first + idx, &e);
    const struct font *pf = font_get(ui_theme->font_ui);
    int n, i = 0, o = 0;

    if (!name)
    {
        /* never with a window's pages cached (keep_window): the scripts
         * forbid the line */
        UPDATE_TRACE("notes row blank r=%lu", (unsigned long)(l->first + idx));
        return "";
    }
    n = xnote_display_len(name, (e->size & XNOTE_DIR) != 0);
    while (i < n && o + 4 < (int)len)
    {
        int at = i, cp = utf8_get((const uint8_t *)name, n, &i);

        if (cp == -1)
            break;
        if (cp >= 0 && has_glyph(pf, cp))
        {
            memcpy(buf + o, name + at, i - at);
            o += i - at;
        }
        else
            buf[o++] = '?';
    }
    buf[o] = '\0';
    return buf;
}

static bool row_chevron(int idx, void *data)
{
    struct notes_list *l = data;
    const struct xnote_rec *e = rec_get(nw, l->first + idx);

    return e && (e->size & XNOTE_DIR);
}

static void list_setup(struct notes_list *l, uint32_t first, uint32_t count)
{
    const struct ui_theme *t = ui_theme;
    struct viewport vp;

    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;
    l->first = first;
    l->count = count;
    /* paged: the first window's pages before the list measures its rows */
    if (nw && nw->ok && !nw->whole && count)
    {
        const struct xidx_pdir *d = dir_of(nw);
        uint32_t i, hi = first + MIN(count - 1, (uint32_t)t->list_rows);
        int p = xidx_dir_find(d, nw->npage, first);
        int p1 = xidx_dir_find(d, nw->npage, hi);

        for (; p >= 0 && p <= p1; p++)
            page_get(nw, d[p].row0, &i, true);
        page_get(nw, first, &i, true);
    }
    ui_list_init(&l->list, &vp, count, row_label, l);
    l->list.has_chevron = row_chevron;
}

/* The empty state: our own sentence, centred in grey (Q12: the folder a
 * user drops text files into) */
static void draw_empty(void)
{
    const struct ui_theme *t = ui_theme;
    const char *s = str(LANG_IPODUI_X_NOTES_EMPTY);
    struct viewport vp;
    char line[2][80];
    int w, h, n = 0, i, cut, y;

    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.font = t->font_small;
    vp.drawmode = DRMODE_SOLID;
    lcd_set_viewport(&vp);
    lcd_set_foreground(t->list_bg);
    lcd_fillrect(0, 0, vp.width, vp.height);
    /* two lines at most: break at the last space that fits */
    while (*s && n < 2)
    {
        int len = strlen(s);

        cut = MIN(len, (int)sizeof(line[0]) - 1);
        for (;;)
        {
            memcpy(line[n], s, cut);
            line[n][cut] = '\0';
            font_getstringsize((const unsigned char *)line[n], &w, &h,
                               t->font_small);
            if (w <= LCD_WIDTH - 4 * MARGIN || cut <= 1)
                break;
            while (cut > 1 && s[cut - 1] != ' ')
                cut--;
            while (cut > 1 && s[cut - 1] == ' ')
                cut--;
        }
        s += cut;
        while (*s == ' ')
            s++;
        n++;
    }
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(t->list_fg_dim);
    y = (vp.height - n * LINE_PITCH) / 2 - LINE_PITCH / 2;
    for (i = 0; i < n; i++)
    {
        font_getstringsize((const unsigned char *)line[i], &w, &h,
                           t->font_small);
        lcd_putsxy((LCD_WIDTH - w) / 2, y + i * LINE_PITCH,
                   (const unsigned char *)line[i]);
    }
    lcd_set_viewport(NULL);
}

static void list_draw(struct notes_list *l, unsigned dirty)
{
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    if (!l->count)
    {
        if (dirty & UI_DIRTY_BODY)
            draw_empty();
        return;
    }
    if (dirty & UI_DIRTY_BODY)
        ui_list_draw(&l->list, l->list.vp.y, l->list.vp.height);
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&l->list, y[i], h[i]);
    }
}

static enum ui_result list_input(struct notes_list *l,
                                 const struct ui_input *in)
{
    const struct xnote_rec *e;
    int y[2], h[2], i, n, steps;
    uint32_t r;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            if (!l->count)
                return UI_HANDLED;
            if (!nw->whole)
                xindex_browse_keep(true);
            /* the pages first: the move measures the rows it shows */
            steps = clamp_steps(nw, l, in->wheel_steps);
            keep_window(nw, l, ui_list_top_after(&l->list, steps), steps);
            n = ui_list_move(&l->list, steps, y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press || !l->count)
                return UI_HANDLED;
            r = l->first + l->list.selected;
            if (!name_get(nw, r, &e))
                return UI_HANDLED;
            if (e->size & XNOTE_DIR)
                ui_push(&notes_dir_screen, (const void *)(intptr_t)r);
            else
                ui_push(&notes_view_screen, (const void *)(intptr_t)r);
            return UI_HANDLED;
        default:
            return UI_IGNORED;          /* MENU backs out */
    }
}

/* the root: the whole folder */
static int root_enter(struct ui_screen *s, const void *arg)
{
    uint32_t first, count;

    (void)arg;
    s->title = str(LANG_IPODUI_X_NOTES);
    modclass_entry(MOD_NOTES);
    nw = mod_work_alloc(MOD_NOTES, NOTES_WORK);
    s->priv = nw;
    if (!nw)
    {
        /* the reduced view: no rows (never another allocation) */
        UPDATE_TRACE("notes work refused bytes=%d", NOTES_WORK);
        return 0;
    }
    memset(nw, 0, sizeof(*nw));
    load_index(nw);
    dir_range(nw, 0, &first, &count);
    list_setup(&nw->lists[0], first, count);
    return 0;
}

static void root_leave(struct ui_screen *s)
{
    (void)s;
    nw = NULL;                          /* the block goes with the module */
}

static void root_draw(struct ui_screen *s, unsigned dirty)
{
    struct notes_work *w = s->priv;

    if (w)
        list_draw(&w->lists[0], dirty);
    else if (dirty & UI_DIRTY_BODY)
    {
        lcd_set_foreground(ui_theme->list_bg);  /* the reduced view */
        lcd_fillrect(0, ui_theme->header_h, LCD_WIDTH,
                     LCD_HEIGHT - ui_theme->header_h);
    }
}

static enum ui_result root_input(struct ui_screen *s,
                                 const struct ui_input *in)
{
    struct notes_work *w = s->priv;

    if (!w)
        return in->key == UI_KEY_MENU ? UI_IGNORED : UI_HANDLED;
    return list_input(&w->lists[0], in);
}

/* A list's tick; paged, the drive held awake a while after the last
 * input (xindex_browse_keep(), each second) */
static int list_tick(struct notes_work *w, struct notes_list *l)
{
    int t;

    if (!l->count)
        return TIMEOUT_BLOCK;
    t = ui_list_tick(&l->list);
    if (w->ok && !w->whole && xindex_browse_keep(false)
        && (t == TIMEOUT_BLOCK || t > HZ))
        t = HZ;
    return t;
}

static int root_tick(struct ui_screen *s)
{
    struct notes_work *w = s->priv;
    uint32_t first, count;
    int sel;

    if (!w)
        return TIMEOUT_BLOCK;
    if (w->seq != xindex_seq() && !xindex_busy())
    {
        /* a new index (the post-USB moment): read it again */
        sel = w->lists[0].list.selected;
        modclass_entry(MOD_NOTES);
        load_index(w);
        dir_range(w, 0, &first, &count);
        list_setup(&w->lists[0], first, count);
        if (count)
        {
            int y[2], h[2], d = MIN(sel, (int)count - 1);

            /* the pages first: the move measures the rows it shows */
            keep_window(w, &w->lists[0],
                        ui_list_top_after(&w->lists[0].list, d), 0);
            ui_list_move(&w->lists[0].list, d, y, h);
        }
        ui_invalidate(UI_DIRTY_ALL);
        return 0;
    }
    return list_tick(w, &w->lists[0]);
}

const struct ui_screen_ops ui_x_notes_screen = {
    .name    = "notes",
    .enter   = root_enter,
    .leave   = root_leave,
    .draw    = root_draw,
    .input   = root_input,
    .timeout = NULL,
    .tick    = root_tick,
};

/* a folder: arg = its record */
static int dir_enter(struct ui_screen *s, const void *arg)
{
    struct notes_work *w = nw;
    uint32_t r = (uint32_t)(intptr_t)arg, first, count;
    const struct xnote_rec *e;
    const char *name = w ? name_get(w, r, &e) : NULL;

    if (!name)
        return -1;
    modclass_entry(MOD_NOTES);
    if (!(e->path_off & XNOTE_PATH_FAR))
        strlcpy(w->folder, name, sizeof(w->folder));
    else if (!full_name(w, e, name, w->folder, sizeof(w->folder)))
        return -1;
    s->priv = w;
    s->title = w->folder;
    w->dir_rec = r;
    dir_range(w, r, &first, &count);
    list_setup(&w->lists[1], first, count);
    keep_window(w, &w->lists[1], w->lists[1].list.top, 0);
    return 0;
}

static void dir_leave(struct ui_screen *s)
{
    struct notes_work *w = s->priv;

    /* window mode: the root's rows again, before its first frame */
    if (w && w->seq == xindex_seq())
        keep_window(w, &w->lists[0], w->lists[0].list.top, 0);
    if (w)
        w->dir_rec = 0;
}

static void dir_draw(struct ui_screen *s, unsigned dirty)
{
    struct notes_work *w = s->priv;

    list_draw(&w->lists[1], dirty);
}

static enum ui_result dir_input(struct ui_screen *s,
                                const struct ui_input *in)
{
    struct notes_work *w = s->priv;

    return list_input(&w->lists[1], in);
}

static int dir_tick(struct ui_screen *s)
{
    struct notes_work *w = s->priv;

    if (w->seq != xindex_seq())
    {
        ui_pop_request(s);              /* the root reads the new index */
        return 0;
    }
    return list_tick(w, &w->lists[1]);
}

static const struct ui_screen_ops notes_dir_screen = {
    .name    = "notes-dir",
    .enter   = dir_enter,
    .leave   = dir_leave,
    .draw    = dir_draw,
    .input   = dir_input,
    .timeout = NULL,
    .tick    = dir_tick,
};

/* -- the reader: text ------------------------------------------------------ */

/* Windows-1252's 0x80..0x9F */
static const uint16_t cp1252[32] = {
    0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
    0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178,
};

/* One code point of UTF-8 at *i (advanced); -1 at the end (or a
 * sequence cut by the end), UTF8_BAD for a byte that starts none */
#define UTF8_BAD (-2)
static int utf8_get(const uint8_t *s, int len, int *i)
{
    int c, n, k;

    if (*i >= len)
        return -1;
    c = s[(*i)++];
    if (c < 0x80)
        return c;
    n = c >= 0xf8 ? 0 : c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (!n)
        return UTF8_BAD;
    if (*i + n > len)
    {
        *i = len;
        return -1;
    }
    c &= 0x3f >> n;
    for (k = 0; k < n; k++)
    {
        if ((s[*i] & 0xc0) != 0x80)
            return UTF8_BAD;
        c = (c << 6) | (s[(*i)++] & 0x3f);
    }
    return c;
}

/* Valid UTF-8, a sequence cut by the 4 KB limit allowed at the end */
static bool utf8_ok(const uint8_t *s, int len)
{
    int i = 0;

    while (i < len)
        if (utf8_get(s, len, &i) == UTF8_BAD)
            return false;
    return true;
}

static int next_cp(const struct notes_work *w, int *i)
{
    const uint8_t *t = w->text;
    int c;

    switch (w->enc)
    {
        case ENC_U16LE:
        case ENC_U16BE:
            if (*i + 2 > w->len)
                return -1;
            c = w->enc == ENC_U16LE ? t[*i] | t[*i + 1] << 8
                                    : t[*i] << 8 | t[*i + 1];
            *i += 2;
            if (c >= 0xd800 && c < 0xdc00 && *i + 2 <= w->len)
            {
                *i += 2;                /* a pair: beyond the type anyway */
                return 0x10000;
            }
            return c;
        case ENC_1252:
            if (*i >= w->len)
                return -1;
            c = t[(*i)++];
            if (c >= 0x80 && c < 0xa0)
                c = cp1252[c - 0x80] ? cp1252[c - 0x80] : 0xfffd;
            return c;
        default:
            c = utf8_get(t, w->len, i);
            return c == UTF8_BAD ? 0xfffd : c;
    }
}

/* What the shell type draws: its range, without the C1 slots, plus the
 * punctuation firmware/font.c maps onto them */
static bool has_glyph(const struct font *pf, uint32_t cp)
{
    if (cp >= 0x2013 && cp <= 0x2026)
        return cp == 0x2013 || cp == 0x2014 || cp == 0x2018 || cp == 0x2019
               || cp == 0x201c || cp == 0x201d || cp == 0x2026;
    if (cp >= 0x80 && cp < 0xa0)
        return false;
    return cp >= pf->firstchar && cp < pf->firstchar + (uint32_t)pf->size;
}

static bool is_break(int c)
{
    return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029;
}

/* the advance of c; 0 for a control */
static int cp_width(struct font *pf, int c, int space)
{
    if (c == '\t')
        return TAB_SPACES * space;
    if (c < 0x20 || c == 0x7f || is_break(c))
        return 0;
    return has_glyph(pf, c) ? font_get_width(pf, c) : BOX_W;
}

/* Greedy word wrap into the line table (byte offsets) */
static void wrap(struct notes_work *w)
{
    struct font *pf = font_get(ui_theme->font_small);
    int space = font_get_width(pf, ' ');
    int start = w->start, pos, next, c, x, brk;
    bool hard = true;                   /* the line follows a break */

    /* the characters the type lacks, once */
    w->lines = w->missing = 0;
    for (pos = start; (c = next_cp(w, &pos)) >= 0; )
        if (c >= 0x20 && c != 0x7f && c != 0xa0 && !is_break(c)
            && !has_glyph(pf, c))
            w->missing++;
    while (start < w->len && w->lines < LINES_MAX)
    {
        /* a wrapped line drops the spaces it starts with */
        if (!hard)
            for (pos = start; (c = next_cp(w, &pos)) == ' '; start = pos)
                ;
        if (start >= w->len)
            break;
        w->line[w->lines++] = start;
        x = 0;
        brk = -1;
        for (pos = start; ; pos = next)
        {
            next = pos;
            c = next_cp(w, &next);
            if (c < 0)
            {
                start = w->len;
                break;
            }
            if (is_break(c))
            {
                if (c == '\r')          /* CR LF */
                {
                    int k = next;

                    if (next_cp(w, &k) == '\n')
                        next = k;
                }
                start = next;
                hard = true;
                break;
            }
            x += cp_width(pf, c, space);
            if (x > TEXT_W && pos > start)
            {
                start = brk > start ? brk : pos;
                hard = false;
                break;
            }
            if (c == ' ' || c == '\t')
                brk = next;
        }
    }
}

static void load_note(struct notes_work *w, uint32_t r, const char *name)
{
    char path[MAX_PATH];
    int fd, n;

    (void)r;                            /* the trace's only */
    w->len = w->start = 0;
    w->enc = ENC_UTF8;
    if (w->dir_rec)
        n = snprintf(path, sizeof(path), XNOTE_ROOT_DIR "/%s/%s", w->folder,
                     name);
    else
        n = snprintf(path, sizeof(path), XNOTE_ROOT_DIR "/%s", name);
    fd = n > 0 && n < (int)sizeof(path) ? open(path, O_RDONLY) : -1;
    if (fd >= 0)
    {
        n = read(fd, w->text, sizeof(w->text));
        close(fd);
        w->len = n > 0 ? n : 0;
    }
    if (w->len >= 2 && w->text[0] == 0xff && w->text[1] == 0xfe)
        w->enc = ENC_U16LE, w->start = 2;
    else if (w->len >= 2 && w->text[0] == 0xfe && w->text[1] == 0xff)
        w->enc = ENC_U16BE, w->start = 2;
    else if (w->len >= 3 && w->text[0] == 0xef && w->text[1] == 0xbb
             && w->text[2] == 0xbf)
        w->start = 3;
    else if (!utf8_ok(w->text, w->len))
        w->enc = ENC_1252;
    wrap(w);
    UPDATE_TRACE("notes view r=%lu bytes=%u lines=%u missing=%u enc=%u "
                 "ok=%d", (unsigned long)r, w->len, w->lines, w->missing,
                 w->enc, fd >= 0 ? 1 : 0);
}

/* -- the reader: screen ---------------------------------------------------- */

static int view_enter(struct ui_screen *s, const void *arg)
{
    struct notes_work *w = nw;
    uint32_t r = (uint32_t)(intptr_t)arg;
    const struct xnote_rec *e;
    const char *name = w ? name_get(w, r, &e) : NULL;
    char full[XNOTE_NAME_MAX + 1];
    int cut;

    if (!name)
        return -1;
    modclass_entry(MOD_NOTES);
    name = full_name(w, e, name, full, sizeof(full));
    if (!name)
        return -1;
    s->priv = w;
    /* the title: the display name, cut at a character */
    cut = MIN(xnote_display_len(name, 0), TITLE_MAX - 1);
    while (cut > 0 && (name[cut] & 0xc0) == 0x80)
        cut--;
    memcpy(w->title, name, cut);
    w->title[cut] = '\0';
    s->title = w->title;
    load_note(w, r, name);
    w->rows = (LCD_HEIGHT - ui_theme->header_h - TOP_PAD) / LINE_PITCH;
    w->top = 0;
    return 0;
}

static int line_end(const struct notes_work *w, int i)
{
    return i + 1 < w->lines ? w->line[i + 1] : w->len;
}

static void draw_line(struct notes_work *w, struct font *pf, int i, int y)
{
    char run[RUN_MAX + 4];
    int pos = w->line[i], end = line_end(w, i), next, c, o = 0;
    int x = MARGIN, x0 = MARGIN, space = font_get_width(pf, ' ');

    for (; pos < end; pos = next)
    {
        next = pos;
        c = next_cp(w, &next);
        if (c < 0 || is_break(c))
            break;
        if (c >= 0x20 && c != 0x7f && c != '\t' && has_glyph(pf, c))
        {
            /* the run: drawn with the type, the punctuation remapped by
             * font.c */
            if (c < 0x80)
                run[o++] = c;
            else if (c < 0x800)
            {
                run[o++] = 0xc0 | (c >> 6);
                run[o++] = 0x80 | (c & 0x3f);
            }
            else
            {
                run[o++] = 0xe0 | (c >> 12);
                run[o++] = 0x80 | ((c >> 6) & 0x3f);
                run[o++] = 0x80 | (c & 0x3f);
            }
            x += font_get_width(pf, c);
            if (o < RUN_MAX)
                continue;
        }
        if (o)
        {
            run[o] = '\0';
            lcd_putsxy(x0, y, (const unsigned char *)run);
            o = 0;
            x0 = x;
        }
        if (c < 0x20 || c == 0x7f || has_glyph(pf, c))
        {
            if (c == '\t')
                x += TAB_SPACES * space;
            x0 = x;
            continue;
        }
        /* a character the type lacks: a hollow box, cap high */
        lcd_drawrect(x + 1, y + pf->ascent - 10, BOX_W - 3, 10);
        x += BOX_W;
        x0 = x;
    }
    if (o)
    {
        run[o] = '\0';
        lcd_putsxy(x0, y, (const unsigned char *)run);
    }
}

static void view_draw(struct ui_screen *s, unsigned dirty)
{
    const struct ui_theme *t = ui_theme;
    struct notes_work *w = s->priv;
    struct font *pf = font_get(t->font_small);
    struct viewport vp;
    int i, y, th, ty, track;

    if (!(dirty & (UI_DIRTY_BODY | UI_DIRTY_BAND)))
        return;
    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.font = t->font_small;
    vp.drawmode = DRMODE_SOLID;
    lcd_set_viewport(&vp);
    lcd_set_foreground(t->list_bg);
    lcd_fillrect(0, 0, vp.width, vp.height);
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(t->list_fg);
    for (i = w->top, y = TOP_PAD; i < w->lines && y + LINE_PITCH <= vp.height;
         i++, y += LINE_PITCH)
        draw_line(w, pf, i, y);
    /* the scrollbar thumb on a note longer than the page */
    if (w->lines > w->rows)
    {
        track = vp.height - 4;
        th = MAX(track * w->rows / w->lines, 16);
        ty = 2 + (track - th) * w->top / (w->lines - w->rows);
        lcd_set_drawmode(DRMODE_SOLID);
        ui_paint_gloss(LCD_WIDTH - SBAR_W, ty, SBAR_W, th, t->sbar_gloss);
    }
    lcd_set_viewport(NULL);
}

static enum ui_result view_input(struct ui_screen *s,
                                 const struct ui_input *in)
{
    struct notes_work *w = s->priv;
    int top = w->top, max = MAX(w->lines - w->rows, 0);

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            top += in->wheel_steps;
            break;
        case UI_KEY_NEXT:
        case UI_KEY_PREV:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            top += (in->key == UI_KEY_NEXT ? 1 : -1) * (w->rows - 1);
            break;
        case UI_KEY_SELECT:
            return UI_HANDLED;
        default:
            return UI_IGNORED;          /* MENU backs out */
    }
    top = MIN(MAX(top, 0), max);
    if (top != w->top)
    {
        w->top = top;
        /* a text page: the body as one band (lean rule 4: read, not
         * browsed; ~14 short lines) */
        ui_invalidate_band(ui_theme->header_h,
                           LCD_HEIGHT - ui_theme->header_h);
    }
    return UI_HANDLED;
}

static int view_tick(struct ui_screen *s)
{
    struct notes_work *w = s->priv;

    if (w->seq != xindex_seq())
    {
        ui_pop_request(s);
        return 0;
    }
    return TIMEOUT_BLOCK;
}

static const struct ui_screen_ops notes_view_screen = {
    .name    = "notes-view",
    .enter   = view_enter,
    .leave   = NULL,
    .draw    = view_draw,
    .input   = view_input,
    .timeout = NULL,
    .tick    = view_tick,
};
