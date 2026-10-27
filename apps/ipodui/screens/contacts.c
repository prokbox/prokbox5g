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
 * iPod UI shell: Extras > Contacts, the MOD_CONTACTS module (P12 W3-K).
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

/* iPod UI shell: Extras > Contacts (P12 W3-K, MOD_CONTACTS, Class A; spec
 * extras.md; Contacts is not photographed, so the list is the Notes
 * list's full-width look and the card follows the 7G's contact page by
 * description). The list shows every card as "Given Family", in the
 * order of Settings > Sort By (First or Last name); a card opens its
 * page: the organisation in grey, then one block per item (a small blue
 * label, the value below it in the list type): phones, e-mails,
 * addresses, birthday, note. The wheel scrolls a line per step,
 * PREVIOUS/NEXT a page; MENU backs out.
 *
 * Storage (brief 2.1, Class A): the list reads the sync-time index
 * (contacts.idx, built by MOD_XINDEX at the post-USB moment or the boot
 * window; xcontact_core.h has the layout), never /Contacts. Entry: one
 * open and one read of the index's head (its header, both page
 * directories and both orders' first pages; the whole index when it fits
 * the cache; else every page the read brought). A longer list is read a
 * page at a time (12 rows at least, ~44 typical, 1 KB) when the rows
 * around the selection leave the cached pages: one open and one range
 * read each, booked as a page load (modclass_page(), the manifest's
 * pages rate; logged "contacts page"). A card: one open and one range
 * read. Nothing while browsing inside the cached pages. While the list
 * is used the drive is held awake and the next page read ahead when that
 * costs no spin-up (docs/storage-choices.md 5.1).
 *
 * Text: the index holds UTF-8; any character the type lacks shows as '?'
 * (a list) or is replaced the same way on the card (counted, "contacts
 * view missing="), never as a blank. The shell type covers Latin up to
 * Extended-B; other scripts land with P22's languages.
 *
 * Memory: the module's one working block (mod_work_alloc(), from the
 * root's enter): the window cache, the card's text and its line table. */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "settings.h"
#include "string-extra.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/anim.h"
#include "ipodui/list.h"
#include "ipodui/screen_stack.h"
#include "ipodui/modload.h"
#include "ipodui/modclass.h"
#include "ipodui/xindex.h"
#include "ipodui/xcontact_core.h"
#include "ipodui/screens/contacts.h"

/* The working block: 7 KB from the screen arena in a resident build, 16 KB
 * from the module arena (contacts.mod). Equal but for the window cache. */
#if defined(IPODUI_MODULES)
#define CT_WORK_KB  16
#else
#define CT_WORK_KB  7
#endif
#define CT_WORK     (CT_WORK_KB * 1024)

#define LINES_MAX   80              /* a card's lines: 14 items x (1 + 4) */
#define VALUE_LINES 4               /* a value shows at most this many */
#define NOTE_LINES  7
#define TITLE_MAX   XIDX_LABEL_MAX  /* the card's title: its row's label */
/* Page loads a second from browsing: a pair for a window (the visible
 * pages) only while the last second's stays at or under LOAD_MAX - 2, a
 * prefetch under LOAD_MAX - 2; the entry reads' loads (a reload, the
 * first window) are not refused and keep CONTACTS_PAGES - LOAD_MAX in
 * reserve. A spin past the budget stops at the cached rows' edge. */
#define LOAD_MAX    (CONTACTS_PAGES - 2)
#define MARGIN      10
#define SBAR_W      7
#define TEXT_W      (LCD_WIDTH - 2 * MARGIN - SBAR_W)
#define H_ORG       26              /* line heights, px */
#define H_LABEL     19              /* a gap above, a 12 px line */
#define H_VALUE     20
#define TOP_PAD     6

enum { CL_ORG, CL_LABEL, CL_VALUE };

struct ct_line {
    uint16_t off;                   /* into text[]; a label: its LANG id */
    uint16_t len;
    uint8_t  style;
    uint8_t  pad;
};

struct ct_work {
    /* the index */
    uint32_t sig, count, str_off, str_bytes, size;
    uint16_t npage[2], hdr_end;     /* the directories end at hdr_end */
    uint8_t  seq, ok, whole, last;  /* whole: the file is in the cache;
                                       last: sorted by Last name */
    struct xidx_pcache pc;          /* pages past the head (not whole) */
    struct xidx_rate rate;          /* the page loads' ticks */
    struct ui_list list;
    /* the open card */
    char title[TITLE_MAX];
    uint16_t nlines, top, view_h, max_top, missing;
    struct ct_line line[LINES_MAX];
    char text[XC_NAME_MAX + 1 + XC_DETAIL_MAX + 1];
    /* the cache: the file's head (header, directories) and the page
     * cache, or the whole file */
    uint8_t  cache[] __attribute__((aligned(4)));
};
#define CACHE_BYTES ((CT_WORK - sizeof(struct ct_work)) & ~3u)
/* the head of the largest index (two full directories) and the two
 * largest pages a window spans (each padded to 4 bytes) */
_Static_assert(sizeof(struct ct_work) + XCONTACT_DIR_OFF
               + 2 * sizeof(struct xidx_pdir) * XCONTACT_PDIR_MAX
               + 2 * (XC_PAGE_LIMIT + 3) <= CT_WORK,
               "contacts: the cache holds a head and two pages");

static struct ct_work *cw;          /* the root's enter to its leave */

static const struct ui_screen_ops contacts_card_screen;

/* -- text ------------------------------------------------------------------ */

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

/* s with every character the type lacks as '?', into out (len bytes);
 * a '\n' is kept when keep_nl. Returns the bytes written (no NUL). */
static int clean(const struct font *pf, const char *s, int n, char *out,
                 int len, bool keep_nl, uint16_t *missing)
{
    int i = 0, o = 0;

    while (i < n)
    {
        int at = i, cp = utf8_get((const uint8_t *)s, n, &i), w;

        if (cp == -1)
            break;
        if (cp == '\n' && keep_nl)
            w = 1;
        else if (cp >= 0x20 && cp != 0x7f && has_glyph(pf, cp))
            w = i - at;
        else
            w = 0;
        if (o + (w ? w : 1) >= len)
            break;
        if (cp == '\n' && keep_nl)
            out[o++] = '\n';
        else if (w)
        {
            memmove(out + o, s + at, w);    /* in place: o <= at */
            o += w;
        }
        else
        {
            out[o++] = '?';
            if (missing)
                (*missing)++;
        }
    }
    out[o] = '\0';
    return o;
}

/* -- the index cache ------------------------------------------------------- */
/* Whole: the file is in the cache, a page is found at its file offset.
 * Paged: the cache keeps the file's head (header, both directories) and
 * after it the page cache (xindex_core.h), which the entry read fills
 * with both first pages. */

static const struct xidx_pdir *dir_of(const struct ct_work *w, int last)
{
    return (const struct xidx_pdir *)(const void *)(w->cache
                                                    + XCONTACT_DIR_OFF)
           + (last ? w->npage[0] : 0);
}

static bool page_ok(const struct ct_work *w, int last, int p, const void *pg)
{
    const struct xidx_pdir *d = dir_of(w, last);

    return xidx_page_check(pg, &d[p], xidx_page_rows(d, w->npage[last], p,
                                                     w->count),
                           sizeof(struct xcontact_rec)) == XIDX_OK;
}

/* Reads page p of the order: one open, one range read (a page load) */
static const void *load_page(struct ct_work *w, int last, int p)
{
    const struct xidx_pdir *d = &dir_of(w, last)[p];
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
    fd = open(XCONTACTS_IDX, O_RDONLY);
    if (fd >= 0)
    {
        ok = lseek(fd, d->off, SEEK_SET) >= 0
             && read(fd, at, d->bytes) == d->bytes;
        close(fd);
    }
    modclass_page(false);
    ok = ok && page_ok(w, last, p, at);
    UPDATE_TRACE("contacts page l=%d p=%d rows=%lu ok=%d", last, p,
                 (unsigned long)xidx_page_rows(dir_of(w, last),
                                               w->npage[last], p, w->count),
                 ok ? 1 : 0);
    if (!ok)
        return NULL;
    xidx_pc_add(&w->pc, last, p, d->bytes);
    return at;
}

/* The page holding row of the current order: cached only (load: read it
 * when it is not) */
static const void *page_get(struct ct_work *w, uint32_t row, uint32_t *i,
                            bool load)
{
    const struct xidx_pdir *d;
    const void *pg;
    int p;

    if (!w->ok || row >= w->count)
        return NULL;
    d = dir_of(w, w->last);
    p = xidx_dir_find(d, w->npage[w->last], row);
    if (p < 0)
        return NULL;
    *i = row - d[p].row0;
    if (w->whole)
        return w->cache + d[p].off;     /* checked at entry */
    pg = xidx_pc_get(&w->pc, w->last, p);
    if (!pg && load)
        pg = load_page(w, w->last, p);
    return pg;
}

static const char *label_of(struct ct_work *w, uint32_t row)
{
    uint32_t i;
    const void *pg = page_get(w, row, &i, false);
    const struct xcontact_rec *r;

    if (!pg)
        return NULL;
    r = xidx_page_rec(pg, i, sizeof(*r));
    return r ? xidx_page_str(pg, r->label) : NULL;
}

/* The offset of the row's card in the card block (its organisation, then
 * its items), or 0xffffffff when the row is not cached */
static uint32_t card_of(struct ct_work *w, uint32_t row)
{
    uint32_t i;
    const void *pg = page_get(w, row, &i, false);
    const struct xcontact_rec *r = pg ? xidx_page_rec(pg, i, sizeof(*r))
                                      : NULL;

    return r ? r->card : 0xffffffffu;
}

/* Takes page p of an order already in the cache at the page cache's tail
 * (the entry read brought it) */
static bool adopt(struct ct_work *w, int last, int p, uint32_t got)
{
    const struct xidx_pdir *d = &dir_of(w, last)[p];
    const uint8_t *at = w->pc.buf + w->pc.used;

    if (p >= w->npage[last] || w->pc.n >= XIDX_PC_SLOTS
        || d->off != w->hdr_end + w->pc.used
        || d->off + d->bytes > got || !page_ok(w, last, p, at))
        return false;
    xidx_pc_add(&w->pc, last, p, d->bytes);
    return true;
}

/* Entry: one read of the file's head (2 storage commands with the open,
 * the class rule): the whole file when it fits the cache */
static void load_index(struct ct_work *w)
{
    const struct xidx_hdr *h = (const struct xidx_hdr *)(const void *)w->cache;
    const struct xcontact_tab *tab = (const struct xcontact_tab *)(h + 1);
    ssize_t got = 0;
    off_t size = 0;
    int fd, rc = XIDX_ESHORT, l, p;

    w->ok = w->whole = 0;
    w->count = 0;
    w->seq = xindex_seq();
    w->last = global_settings.ipodui_sort_last ? 1 : 0;
    fd = open(XCONTACTS_IDX, O_RDONLY);
    if (fd >= 0)
    {
        size = ffilesize(fd);
        got = read(fd, w->cache, CACHE_BYTES);
        close(fd);
    }
    if (got >= (ssize_t)XCONTACT_DIR_OFF && size > 0)
        rc = xidx_check(h, (uint32_t)size);
    if (rc == XIDX_OK && (h->kind != XIDX_CONTACTS
                          || h->rec_off != XCONTACT_TAB_OFF))
        rc = XIDX_EKIND;
    /* the head: both directories inside what was read */
    if (rc == XIDX_OK
        && (tab->npage[0] > XCONTACT_PDIR_MAX
            || tab->npage[1] > XCONTACT_PDIR_MAX
            || tab->hdr_end != XCONTACT_DIR_OFF + sizeof(struct xidx_pdir)
                               * (tab->npage[0] + tab->npage[1])
            || tab->hdr_end > (uint32_t)got || tab->hdr_end > h->str_off))
        rc = XIDX_EBOUNDS;
    if (rc == XIDX_OK && got == size)
        rc = xidx_check_body(w->cache, (uint32_t)size);
    if (rc == XIDX_OK)
    {
        w->sig = h->sig;
        w->count = h->count;
        w->str_off = h->str_off;
        w->str_bytes = h->str_bytes;
        w->npage[0] = tab->npage[0];
        w->npage[1] = tab->npage[1];
        w->hdr_end = tab->hdr_end;
        w->size = (uint32_t)size;
        w->whole = got == size;
        for (l = 0; l < 2 && rc == XIDX_OK; l++)
        {
            rc = xidx_dir_check(dir_of(w, l), w->npage[l], w->count,
                                w->hdr_end, w->str_off);
            /* whole: every page now (none is read later) */
            for (p = 0; w->whole && rc == XIDX_OK && p < w->npage[l]; p++)
                if (!page_ok(w, l, p, w->cache + dir_of(w, l)[p].off))
                    rc = XIDX_EBOUNDS;
        }
    }
    if (rc == XIDX_OK && !w->whole)
    {
        /* the page cache after the head: every page the entry read
         * brought (both first pages, then the First order's in file
         * order), so a short scroll needs no load at all */
        xidx_pc_init(&w->pc, w->cache + w->hdr_end, CACHE_BYTES - w->hdr_end);
        if (adopt(w, 0, 0, got) && adopt(w, 1, 0, got))
        {
            for (p = 1; adopt(w, 0, p, got); p++)
                ;
            if (p >= w->npage[0])
                for (p = 1; adopt(w, 1, p, got); p++)
                    ;
        }
    }
    w->ok = rc == XIDX_OK;
    UPDATE_TRACE("contacts open n=%lu whole=%d last=%d rc=%d bytes=%ld "
                 "pages=%u+%u head=%u cache=%u",
                 (unsigned long)(w->ok ? w->count : 0), w->whole, w->last,
                 rc, (long)size, w->npage[0], w->npage[1], w->hdr_end,
                 (unsigned)CACHE_BYTES);
}

/* The rows a window at top shows, with the one past either end the
 * scroll's animation draws */
static void window_rows(const struct ct_work *w, int top, uint32_t *lo,
                        uint32_t *hi)
{
    uint32_t t = MAX(top, 0);

    *lo = t ? t - 1 : 0;
    *hi = MIN(t + (uint32_t)w->list.style.rows, w->count - 1);
}

/* Every page of rows lo..hi is in the cache (a peek, no use counted) */
static bool window_cached(const struct ct_work *w, int top)
{
    const struct xidx_pdir *d = dir_of(w, w->last);
    uint32_t lo, hi;
    int p, p1;

    window_rows(w, top, &lo, &hi);
    p = xidx_dir_find(d, w->npage[w->last], lo);
    p1 = xidx_dir_find(d, w->npage[w->last], hi);
    for (; p >= 0 && p <= p1; p++)
        if (!xidx_pc_peek(&w->pc, w->last, p))
            return false;
    return p >= 0;
}

/* The page loads of the last second */
static unsigned loads_recent(const struct ct_work *w)
{
    return xidx_rate_recent(&w->rate, current_tick, HZ);
}

/* A wheel move of steps rows, or fewer: with the second's budget spent
 * (LOAD_MAX), only as far as the cached pages show whole windows */
static int clamp_steps(const struct ct_work *w, int steps)
{
    int one = steps > 0 ? 1 : -1;

    if (w->whole || !steps || loads_recent(w) + 2 <= LOAD_MAX)
        return steps;
    while (steps && !window_cached(w, ui_list_top_after(&w->list, steps)))
        steps -= one;
    UPDATE_TRACE("contacts held steps=%d", steps);
    return steps;
}

/* The rows on screen at top are cached (paged mode only): every page of
 * the rows from just above the list's top to just below its last visible row
 * (two at most: a page holds XIDX_PAGE_ROWS_MIN rows), read when they
 * come into view; the cache keeps two of the largest pages beside the
 * head (the static assert above). Then, the drive warm and the second's
 * budget not near its end, the next page the way the list moves (dir) is
 * read ahead when the cache holds it beside the visible pages, so a
 * steady scroll meets cached rows (docs/storage-choices.md). */
static void keep_window(struct ct_work *w, int top, int dir)
{
    const struct xidx_pdir *d;
    uint32_t lo, hi, slo, shi, i;
    int p, p0, p1, np;

    if (!w->ok || !w->count || w->whole)
        return;
    d = dir_of(w, w->last);
    np = w->npage[w->last];
    /* the rows a glide still shows on its way (cached: they are on
     * screen), then the window's, read if they are not: the newest */
    window_rows(w, ui_list_shown_top(&w->list), &slo, &shi);
    page_get(w, slo, &i, false);
    page_get(w, shi, &i, false);
    window_rows(w, top, &lo, &hi);
    p0 = xidx_dir_find(d, np, lo);
    p1 = xidx_dir_find(d, np, hi);
    if (p0 < 0 || p1 < 0)
        return;
    for (p = p0; p <= p1; p++)
        page_get(w, d[p].row0, &i, true);
    page_get(w, lo, &i, true);
    p = dir > 0 ? p1 + 1 : dir < 0 ? p0 - 1 : -1;
    if (p >= 0 && p < np && !xidx_pc_peek(&w->pc, w->last, p)
        && loads_recent(w) + 1 <= LOAD_MAX - 2
        && w->pc.cap >= 3 * (XC_PAGE_LIMIT + 3) && xindex_disk_warm())
    {
        load_page(w, w->last, p);
        page_get(w, hi, &i, false);     /* the visible ones stay newer */
        page_get(w, lo, &i, false);
    }
}

/* -- the list -------------------------------------------------------------- */

static const char *row_label(int idx, void *data, char *buf, size_t len)
{
    struct ct_work *w = data;
    const char *s = label_of(w, idx);

    if (!s)
    {
        /* never with a window's pages cached (keep_window): the scripts
         * forbid the line */
        UPDATE_TRACE("contacts row blank r=%d", idx);
        return "";
    }
    clean(font_get(ui_theme->font_ui), s, strlen(s), buf, len, false, NULL);
    return buf;
}

static void list_setup(struct ct_work *w)
{
    const struct ui_theme *t = ui_theme;
    struct viewport vp;

    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;
    ui_list_init(&w->list, &vp, w->ok ? w->count : 0, row_label, w);
}

/* The empty state: our own sentence, centred in grey (Q12: the folder a
 * user drops contact cards into) */
static void draw_empty(void)
{
    const struct ui_theme *t = ui_theme;
    const char *s = str(LANG_IPODUI_X_CONTACTS_EMPTY);
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
    y = (vp.height - n * 15) / 2 - 7;
    for (i = 0; i < n; i++)
    {
        font_getstringsize((const unsigned char *)line[i], &w, &h,
                           t->font_small);
        lcd_putsxy((LCD_WIDTH - w) / 2, y + i * 15,
                   (const unsigned char *)line[i]);
    }
    lcd_set_viewport(NULL);
}

static void list_draw(struct ct_work *w, unsigned dirty)
{
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    if (!w->ok || !w->count)
    {
        if (dirty & UI_DIRTY_BODY)
            draw_empty();
        return;
    }
    if (dirty & UI_DIRTY_BODY)
        ui_list_draw(&w->list, w->list.vp.y, w->list.vp.height);
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&w->list, y[i], h[i]);
    }
}

static enum ui_result root_input(struct ui_screen *s,
                                 const struct ui_input *in)
{
    struct ct_work *w = s->priv;
    int y[2], h[2], i, n, steps;

    if (!w)
        return in->key == UI_KEY_MENU ? UI_IGNORED : UI_HANDLED;
    switch (in->key)
    {
        case UI_KEY_WHEEL:
            if (!w->ok || !w->count)
                return UI_HANDLED;
            if (!w->whole)
                xindex_browse_keep(true);
            /* the pages first: the move measures the rows it shows */
            steps = clamp_steps(w, in->wheel_steps);
            keep_window(w, ui_list_top_after(&w->list, steps), steps);
            n = ui_list_move(&w->list, steps, y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press || !w->ok || !w->count)
                return UI_HANDLED;
            if (label_of(w, w->list.selected))
                ui_push(&contacts_card_screen,
                        (const void *)(intptr_t)w->list.selected);
            return UI_HANDLED;
        default:
            return UI_IGNORED;          /* MENU backs out */
    }
}

static int root_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
    s->title = str(LANG_IPODUI_X_CONTACTS);
    modclass_entry(MOD_CONTACTS);
    cw = mod_work_alloc(MOD_CONTACTS, CT_WORK);
    s->priv = cw;
    if (!cw)
    {
        /* the reduced view: no rows (never another allocation) */
        UPDATE_TRACE("contacts work refused bytes=%d", CT_WORK);
        return 0;
    }
    memset(cw, 0, sizeof(*cw));
    load_index(cw);
    list_setup(cw);
    return 0;
}

static void root_leave(struct ui_screen *s)
{
    (void)s;
    cw = NULL;                          /* the block goes with the module */
}

static void root_draw(struct ui_screen *s, unsigned dirty)
{
    struct ct_work *w = s->priv;

    if (w)
        list_draw(w, dirty);
    else if (dirty & UI_DIRTY_BODY)
    {
        lcd_set_foreground(ui_theme->list_bg);  /* the reduced view */
        lcd_fillrect(0, ui_theme->header_h, LCD_WIDTH,
                     LCD_HEIGHT - ui_theme->header_h);
    }
}

static int root_tick(struct ui_screen *s)
{
    struct ct_work *w = s->priv;
    int sel, t;

    if (!w)
        return TIMEOUT_BLOCK;
    if (w->seq != xindex_seq() && !xindex_busy())
    {
        /* a new index (the post-USB moment): read it again */
        sel = w->list.selected;
        modclass_entry(MOD_CONTACTS);
        load_index(w);
        list_setup(w);
        if (w->ok && w->count)
        {
            int y[2], h[2], d = MIN(sel, (int)w->count - 1);

            /* the pages first: the move measures the rows it shows */
            keep_window(w, ui_list_top_after(&w->list, d), 0);
            ui_list_move(&w->list, d, y, h);
        }
        ui_invalidate(UI_DIRTY_ALL);
        return 0;
    }
    if (!w->ok || !w->count)
        return TIMEOUT_BLOCK;
    t = ui_list_tick(&w->list);
    /* paged: the drive held awake a while after the last input */
    if (!w->whole && xindex_browse_keep(false)
        && (t == TIMEOUT_BLOCK || t > HZ))
        t = HZ;
    return t;
}

const struct ui_screen_ops ui_x_contacts_screen = {
    .name    = "contacts",
    .enter   = root_enter,
    .leave   = root_leave,
    .draw    = root_draw,
    .input   = root_input,
    .timeout = NULL,
    .tick    = root_tick,
};

/* -- the card -------------------------------------------------------------- */

static void line_add(struct ct_work *w, int style, int off, int len)
{
    if (w->nlines >= LINES_MAX)
        return;
    w->line[w->nlines].off = off;
    w->line[w->nlines].len = len;
    w->line[w->nlines].style = style;
    w->nlines++;
}

/* Word-wraps text[off..off+len) into at most maxl lines of CL_VALUE */
static void wrap_value(struct ct_work *w, struct font *pf, int off, int len,
                       int maxl)
{
    const char *t = w->text;
    int pos = off, end = off + len, lines = 0;

    while (pos < end && lines < maxl)
    {
        int start = pos, x = 0, sp = -1, sp_next = -1, i = pos, stop = end;
        int at, c;

        pos = end;
        while (i < end)
        {
            at = i;
            c = utf8_get((const uint8_t *)t, end, &i);
            if (c == -1)
                break;
            if (c == '\n')
            {
                stop = at;
                pos = i;
                break;
            }
            x += font_get_width(pf, c);
            if (x > TEXT_W && at > start)
            {
                if (sp > start)
                {
                    stop = sp;
                    pos = sp_next;
                }
                else
                    stop = pos = at;
                break;
            }
            if (c == ' ')
            {
                sp = at;
                sp_next = i;
            }
        }
        line_add(w, CL_VALUE, start, stop - start);
        lines++;
        while (pos < end && t[pos] == ' ' && stop != pos)
            pos++;                      /* a wrapped line drops its spaces */
    }
}

static int label_id(int kind, int sub)
{
    switch (kind)
    {
        case XCD_PHONE:
            return sub == XCS_MOBILE ? LANG_IPODUI_X_MOBILE
                 : sub == XCS_HOME ? LANG_IPODUI_X_HOME
                 : sub == XCS_WORK ? LANG_IPODUI_X_WORK
                 : sub == XCS_OTHER ? LANG_IPODUI_X_OTHER
                 : LANG_IPODUI_X_PHONE;
        case XCD_EMAIL:
            return sub == XCS_HOME ? LANG_IPODUI_X_HOME
                 : sub == XCS_WORK ? LANG_IPODUI_X_WORK
                 : sub == XCS_OTHER ? LANG_IPODUI_X_OTHER
                 : LANG_IPODUI_X_EMAIL;
        case XCD_ADDR:
            return sub == XCS_HOME ? LANG_IPODUI_X_HOME
                 : sub == XCS_WORK ? LANG_IPODUI_X_WORK
                 : sub == XCS_OTHER ? LANG_IPODUI_X_OTHER
                 : LANG_IPODUI_X_ADDRESS;
        case XCD_BDAY:
            return LANG_IPODUI_X_BIRTHDAY;
        default:
            return LANG_IPODUI_X_NOTE;
    }
}

static int line_h(int style)
{
    return style == CL_ORG ? H_ORG : style == CL_LABEL ? H_LABEL : H_VALUE;
}

/* The page: organisation, then each item as a label and its value */
static void layout(struct ct_work *w, int items, int dlen)
{
    struct font *pf = font_get(ui_theme->font_ui);
    int orglen = strlen(w->text);
    char *p = w->text + items, *end = p + dlen;
    int h, i;

    w->nlines = w->missing = 0;
    if (orglen)
        line_add(w, CL_ORG, 0, orglen);
    while (p < end)
    {
        int kind = XCD_KIND((uint8_t)p[0]), sub = XCD_SUB((uint8_t)p[0]);
        char *v = p + 1;
        int vl = strlen(v), cl;

        p = v + vl + 1;
        if (kind < XCD_PHONE || kind > XCD_NOTE)
            continue;
        cl = clean(pf, v, vl, v, vl + 1, true, &w->missing);
        line_add(w, CL_LABEL, label_id(kind, sub), 0);
        wrap_value(w, pf, v - w->text, cl,
                   kind == XCD_NOTE ? NOTE_LINES : VALUE_LINES);
    }
    /* the first line that, as the top, still shows the last line */
    w->view_h = LCD_HEIGHT - ui_theme->header_h;
    for (i = w->nlines, h = TOP_PAD; i > 0; i--)
    {
        h += line_h(w->line[i - 1].style);
        if (h > w->view_h)
            break;
    }
    w->max_top = i;
    w->top = 0;
}

static int card_open(struct ui_screen *s, const void *arg)
{
    struct ct_work *w = cw;
    uint32_t row = (uint32_t)(intptr_t)arg, card;
    const char *lab = w ? label_of(w, row) : NULL;
    int orglen = 0, dlen = 0, n = 0;
    bool ok = false;

    card = lab ? card_of(w, row) : 0xffffffffu;
    if (!lab || card >= w->str_bytes)
        return -1;
    s->priv = w;
    clean(font_get(ui_theme->font_title), lab, strlen(lab), w->title,
          sizeof(w->title), false, NULL);
    s->title = w->title;
    modclass_entry(MOD_CONTACTS);
    w->text[0] = '\0';
    n = MIN((uint32_t)XC_CARD_MAX, w->str_bytes - card);
    if (w->whole)
    {
        memcpy(w->text, w->cache + w->str_off + card, n);
        ok = true;
    }
    else if (!xindex_busy())
    {
        /* one open, one read of the card's bytes (they end in a 0 byte) */
        int fd = open(XCONTACTS_IDX, O_RDONLY);

        if (fd >= 0)
        {
            ok = lseek(fd, w->str_off + card, SEEK_SET) >= 0
                 && read(fd, w->text, n) == n;
            close(fd);
        }
    }
    if (ok)
    {
        /* the organisation, its NUL, the items ending at a 0 kind byte */
        const char *p;

        w->text[n - 1] = '\0';           /* bounded whatever the bytes */
        orglen = strlen(w->text);
        p = w->text + orglen + 1;
        while (p < w->text + n - 1 && *p)
            p += 1 + strlen(p + 1) + 1;
        dlen = p - (w->text + orglen + 1);
    }
    w->missing = 0;
    if (orglen)
        clean(font_get(ui_theme->font_small), w->text, orglen, w->text,
              orglen + 1, false, &w->missing);
    layout(w, ok ? orglen + 1 : 0, dlen);
    UPDATE_TRACE("contacts view row=%lu items=%d lines=%u missing=%u ok=%d",
                 (unsigned long)row, dlen, w->nlines, w->missing, ok ? 1 : 0);
    return 0;
}

static void card_draw(struct ct_work *w, struct viewport *vp)
{
    const struct ui_theme *t = ui_theme;
    int i, y = TOP_PAD;

    for (i = w->top; i < w->nlines; i++)
    {
        const struct ct_line *l = &w->line[i];
        int h = line_h(l->style);

        if (y + h > vp->height + 4)
            break;
        switch (l->style)
        {
            case CL_ORG:
                lcd_setfont(t->font_small);
                lcd_set_foreground(t->list_fg_dim);
                break;
            case CL_LABEL:
                lcd_setfont(t->font_small);
                lcd_set_foreground(t->value_fg);
                break;
            default:
                lcd_setfont(t->font_ui);
                lcd_set_foreground(t->list_fg);
                break;
        }
        if (l->style == CL_LABEL)
            lcd_putsxy(MARGIN, y + h - 15, (const unsigned char *)
                       str(l->off));
        else if (l->len)
        {
            char run[2 * XC_VAL_MAX + 4];
            int n = MIN((int)l->len, (int)sizeof(run) - 1);

            memcpy(run, w->text + l->off, n);
            run[n] = '\0';
            lcd_putsxy(MARGIN, y + (l->style == CL_ORG ? 2 : 0),
                       (const unsigned char *)run);
        }
        y += h;
    }
}

static void card_paint(struct ui_screen *s, unsigned dirty)
{
    const struct ui_theme *t = ui_theme;
    struct ct_work *w = s->priv;
    struct viewport vp;
    int th, ty, track;

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
    card_draw(w, &vp);
    /* the scrollbar thumb on a card longer than the page */
    if (w->max_top > 0)
    {
        track = vp.height - 4;
        th = MAX(track * (w->nlines - w->max_top) / w->nlines, 16);
        ty = 2 + (track - th) * w->top / w->max_top;
        lcd_set_drawmode(DRMODE_SOLID);
        ui_paint_gloss(LCD_WIDTH - SBAR_W, ty, SBAR_W, th, t->sbar_gloss);
    }
    lcd_set_viewport(NULL);
}

static enum ui_result card_input(struct ui_screen *s,
                                 const struct ui_input *in)
{
    struct ct_work *w = s->priv;
    int top = w->top;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            top += in->wheel_steps;
            break;
        case UI_KEY_NEXT:
        case UI_KEY_PREV:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            top += (in->key == UI_KEY_NEXT ? 1 : -1) * (w->view_h / H_VALUE - 1);
            break;
        case UI_KEY_SELECT:
            return UI_HANDLED;
        default:
            return UI_IGNORED;          /* MENU backs out */
    }
    top = MIN(MAX(top, 0), (int)w->max_top);
    if (top != w->top)
    {
        w->top = top;
        /* a short page of text: the body as one band */
        ui_invalidate_band(ui_theme->header_h,
                           LCD_HEIGHT - ui_theme->header_h);
    }
    return UI_HANDLED;
}

static int card_tick(struct ui_screen *s)
{
    struct ct_work *w = s->priv;

    if (w->seq != xindex_seq())
    {
        ui_pop_request(s);              /* the root reads the new index */
        return 0;
    }
    return TIMEOUT_BLOCK;
}

static const struct ui_screen_ops contacts_card_screen = {
    .name    = "contacts-card",
    .enter   = card_open,
    .leave   = NULL,
    .draw    = card_paint,
    .input   = card_input,
    .timeout = NULL,
    .tick    = card_tick,
};
