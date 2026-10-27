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
 * iPod UI shell: Search (Music > Search)
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
/* The 7G's Search (owner photos search-empty-b, search-results-b; G12
 * p.30). A full-width list of two-line rows with a 55 px tile, 4 per
 * view (the album rows' geometry):
 *   row 0      the grey magnifier, "Search" over "No Search Results",
 *              "Results for: <typed>" or "Updating Library..."
 *   results    a grey type icon per kind (rowglyphs.h srch_glyph), the
 *              label over its second line, one alphabetical list
 *              (lib_search(), L), then the matching playlists (⚠️ S2)
 *   "More..."  a grey single-line row when the result was capped
 *              (⚠️ S6: the alphabetical tail is missing; we never cap
 *              silently)
 * Over the list, near the bottom, a dark rounded strip: a white text
 * field (what was typed and a cursor) and the letters A-Z then 0-9
 * (SRCH_ALPHABET, ⚠️ S3), the current one in a blue rounded box; the
 * letters scroll so the box stays in view, a letter cut at the right end.
 * No list row is highlighted while the strip shows.
 *
 * Keys, strip showing: the wheel moves the box (clamped, no wrap);
 * SELECT enters the letter; NEXT adds a space; PREV deletes the last
 * character; MENU hides the strip (the full result list, the first result
 * selected). Strip hidden: an ordinary list; SELECT on the top row or on
 * "More..." brings the strip back; SELECT on a result opens it (an
 * artist's albums, an album's songs, a book's page) or plays it (a song,
 * an episode from its kind's rows, a playlist file); holding SELECT on a
 * result opens the hold menu; MENU leaves (⚠️ S4).
 *
 * Cost (docs/phase11-brief.md section 3.1): a search runs only when a
 * character is entered or deleted, boosted, never on a wheel step; a key
 * that arrives meanwhile cancels it (the rows stay as they were) and it
 * runs again once the keys stop. 0 storage reads per keystroke; at entry
 * one opendir of /Playlists and the library's group rows
 * (lib_search_begin()). A wheel step repaints the letters only (inside
 * the strip, no text but the ~13 letters) and pushes the strip's band;
 * a new result redraws the list body once (its content changed).
 *
 * State: the screen arena (UI_ARENA_B_SEARCH): the query, the strip's
 * per-row colours and letter widths (computed once), and up to SR_PL_MAX
 * playlist names packed as the Playlists screen packs them.
 *
 * Trace (UPDATE_TRACE): search screen q=<len> n=<rows> pl=<n> cap=<0|1>
 *                         rc=<lib_search()> (each search run) */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "string-extra.h"
#include "file.h"
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "logf.h"
#include "ipodui/ipodui.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/list.h"
#include "ipodui/menu.h"
#include "ipodui/anim.h"
#include "ipodui/search.h"
#include "ipodui/rowglyphs.h"
#include "ipodui/screens/screens.h"
#include "ipodui/screens/holdmenu.h"
#include "ipodui/srchletters.h"
_Static_assert(ARRAYLEN(srl_w) == SRCH_ALPHABET_N,
               "srchletters.h: one glyph per SRCH_ALPHABET letter");

/* The anti-aliased font path's blit (firmware/drivers/lcd-color-common.c;
 * the core exports it, lcd.h leaves it out): 4-bit alpha 'src' (Rockbox's
 * format, rowglyphs.py emit_alpha()), the viewport's foreground blended
 * over the framebuffer in DRMODE_FG. One call per glyph or corner row. */
extern void lcd_alpha_bitmap_part(const unsigned char *src, int src_x,
                                  int src_y, int stride, int x, int y,
                                  int width, int height);

/* Strip geometry (LCD px; rectified owner photos, ~ +-1 px). Provisional
 * look: compile-time constants (owner, section 1.2). */
#define STRIP_X     25
#define STRIP_Y     175
#define STRIP_W     276
#define STRIP_H     45
#define FIELD_X     33          /* the white text field */
#define FIELD_Y     186
#define FIELD_W     64
#define FIELD_H     23
#define FIELD_PAD   4           /* text inset */
#define LET_X0      112         /* the first letter's left edge */
#define LET_CLIP0   104         /* the letters' area, repainted per step */
#define LET_CLIP1   296         /* (exclusive; left of the right corner) */
#define LET_BASE    204         /* the letters' baseline (cap 13: #52) */
#define LET_TRACK   2           /* added to each letter's advance */
#define BOX_Y       187         /* the blue box */
#define BOX_H       21
#define BOX_PAD     2           /* box beyond the letter's advance, each side */
#define CUR_H       13          /* the text cursor */

#define TILE_BG     LCD_RGBPACK(0xeb, 0xeb, 0xee)   /* ~ the icon tile */
#define GLYPH_FG    LCD_RGBPACK(0x8a, 0x8c, 0x92)   /* ~ its grey glyph */
#define STRIP_FG    LCD_RGBPACK(0xff, 0xff, 0xff)
#define FIELD_FG    LCD_RGBPACK(0x00, 0x00, 0x00)
#define FIELD_RIM   LCD_RGBPACK(0x7a, 0x7a, 0x7c)   /* its top inner shadow */
#define FIELD_RIM2  LCD_RGBPACK(0xd8, 0xd8, 0xda)

#define SR_PL_MAX   64          /* playlists searched (as Playlists lists) */
#define SR_PL_BYTES 1792        /* their names */
#define STATE_POLL  HZ          /* library state while updating */
#define GLYPH_MAG   SRCH_KINDS  /* srch_glyph[]: the kinds, then this */

/* The dark strip: near-black, a soft gloss on its upper part, a light
 * first line (~ the photos' rim) */
static const struct ui_gloss strip_gloss = {
    .top    = LCD_RGBPACK(0x44, 0x44, 0x46),
    .bot    = LCD_RGBPACK(0x06, 0x06, 0x06),
    .line   = LCD_RGBPACK(0x78, 0x78, 0x7a),
    .edge   = LCD_RGBPACK(0x00, 0x00, 0x00),
    .hi_top = 44,
    .hi_mid = 10,
    .split  = 110,
    .flags  = UI_GLOSS_LINE | UI_GLOSS_EDGE,
};

static const unsigned char *const corner3[4] = {
    rcorner3[0], rcorner3[1], rcorner3[2], rcorner3[3] };
static const unsigned char *const corner4[4] = {
    rcorner4[0], rcorner4[1], rcorner4[2], rcorner4[3] };
static const unsigned char *const corner5[4] = {
    rcorner5[0], rcorner5[1], rcorner5[2], rcorner5[3] };

struct search {
    struct ui_list list;
    int  view;                  /* the LIB_SEARCH view, < 0 none yet */
    int  begun;                 /* lib_search_begin() result */
    int  nres;                  /* its rows shown (0: empty query) */
    int  gen;                   /* lib_generation() at the last search */
    long next_poll;
    bool strip;                 /* the letter strip shows */
    bool capped;                /* "More..." */
    bool due;                   /* a search waits (cancelled, updating) */
    bool updating;              /* the library is being built */
    bool letters_only;          /* the pending band is the letters' */
    unsigned char pos, first;   /* boxed letter; first letter drawn */
    unsigned char len;          /* typed bytes */
    unsigned char npl;          /* matching playlists (plm[]) */
    unsigned char pln;          /* playlists read */
    char raw[SRCH_MAX_Q + 1];   /* as typed (lower case, spaces) */
    struct srch_query q;
    unsigned short strip_col[STRIP_H];      /* the strip's row colours */
    unsigned char plm[SR_PL_MAX];
    short ploff[SR_PL_MAX];
    char plnames[SR_PL_BYTES];
};
_Static_assert(sizeof(struct search) <= UI_ARENA_B_SEARCH, "search: arena bound");

static struct search *live;     /* the open Search (one at a time) */
static bool cancelled;          /* search_cancel() fired */

/* -- rows ------------------------------------------------------------------ */

static int row_count(const struct search *r)
{
    return 1 + r->nres + r->npl + (r->capped ? 1 : 0);
}

enum row_type { ROW_TOP, ROW_RESULT, ROW_PLAYLIST, ROW_MORE, ROW_NONE };

static enum row_type row_type(const struct search *r, int row, int *i)
{
    *i = row - 1;
    if (row < 0)
        return ROW_NONE;        /* no selection while the strip shows */
    if (row == 0)
        return ROW_TOP;
    if (*i < r->nres)
        return ROW_RESULT;
    *i -= r->nres;
    if (*i < r->npl)
        return ROW_PLAYLIST;
    if (*i == r->npl && r->capped)
        return ROW_MORE;
    return ROW_NONE;
}

/* A result row's kind and id: lib_id() of a LIB_SEARCH row is the row
 * itself, SRCH_ROW(kind, master index) (search.h; L). */
static enum srch_kind result_kind(const struct search *r, int i, int *id)
{
    uint32_t v = (uint32_t)lib_id(r->view, i);
    enum srch_kind k = SRCH_ROW_KIND(v);

    *id = SRCH_ROW_IDX(v);
    return k < SRCH_KINDS ? k : SRCH_SONG;
}

/* A playlist's name without its extension */
static const char *pl_name(const struct search *r, int k, char *buf,
                           size_t len)
{
    char *dot;

    strlcpy(buf, r->plnames + r->ploff[r->plm[k]], len);
    dot = strrchr(buf, '.');
    if (dot)
        *dot = '\0';
    return buf;
}

static const char *shown_label(const char *l)
{
    if (!strcmp(l, lib_unknown_artist))
        return str(LANG_IPODUI_UNKNOWN_ARTIST);
    if (!strcmp(l, lib_unknown_album))
        return str(LANG_IPODUI_UNKNOWN_ALBUM);
    return l;
}

static const char *sr_label(int row, void *data, char *buf, size_t len)
{
    const struct search *r = data;
    int i;

    switch (row_type(r, row, &i))
    {
        case ROW_TOP:
            return str(LANG_IPODUI_SEARCH);
        case ROW_RESULT:
            return shown_label(lib_label(r->view, i, buf, len));
        case ROW_PLAYLIST:
            return pl_name(r, i, buf, len);
        default:
            return "";          /* "More..." is drawn by sr_extra() */
    }
}

static const char *sr_sublabel(int row, void *data, char *buf, size_t len)
{
    const struct search *r = data;
    const char *s;
    int i;

    switch (row_type(r, row, &i))
    {
        case ROW_TOP:
            /* never "No Search Results" for a search that could not run:
             * while the library is built the footer explains (menu.h),
             * and a search still due (cancelled by a key, or waiting for
             * the library) has not answered yet; with no library at all
             * (LIB_UNAVAILABLE) there is nothing to find */
            if (r->updating)
                return NULL;
            if (r->nres + r->npl <= 0)
            {
                if (r->due && lib_state() != LIB_UNAVAILABLE)
                    return NULL;
                return str(LANG_IPODUI_NO_SEARCH_RESULTS);
            }
            snprintf(buf, len, str(LANG_IPODUI_RESULTS_FOR), r->raw);
            return buf;
        case ROW_RESULT:
            s = lib_sublabel(r->view, i, buf, len);
            if (!s)
                return NULL;
            return s[0] ? s : (const char *)str(LANG_IPODUI_UNKNOWN_ARTIST);
        default:
            return NULL;
    }
}

static bool sr_chevron(int row, void *data)
{
    const struct search *r = data;
    int i, id;

    if (row_type(r, row, &i) != ROW_RESULT)
        return false;
    switch (result_kind(r, i, &id))
    {
        case SRCH_ARTIST:
        case SRCH_ALBUM:
        case SRCH_BOOK:
            return true;        /* they open a list or a book's page */
        default:
            return false;
    }
}

/* The art box: the light tile with the grey icon (none on "More...") */
static bool sr_thumb(int row, void *data, int size, bool fetch)
{
    const struct ui_theme *t = ui_theme;
    const struct search *r = data;
    int i, id, g;

    (void)fetch;
    switch (row_type(r, row, &i))
    {
        case ROW_TOP:
            g = GLYPH_MAG;
            break;
        case ROW_RESULT:
            g = result_kind(r, i, &id);
            break;
        case ROW_PLAYLIST:
            g = SRCH_PLAYLIST;
            break;
        default:
            lcd_set_foreground(t->list_bg);
            lcd_fillrect(0, 0, size, size);
            return true;
    }
    lcd_set_foreground(TILE_BG);
    lcd_fillrect(0, 0, size, size - 1);
    lcd_set_foreground(t->list_bg);         /* the row gap */
    lcd_hline(0, size - 1, size - 1);
    lcd_set_foreground(GLYPH_FG);
    lcd_set_drawmode(DRMODE_FG);
    lcd_alpha_bitmap_part(srch_glyph[g], 0, 0, SRCH_GLYPH_W,
                          (size - SRCH_GLYPH_W) / 2,
                          (size - 1 - SRCH_GLYPH_H) / 2,
                          SRCH_GLYPH_W, SRCH_GLYPH_H);
    lcd_set_drawmode(DRMODE_SOLID);
    return true;
}

/* "More...": grey, one line, centred in the row */
static void sr_extra(int row, void *data, int w, int h, bool sel)
{
    const struct ui_theme *t = ui_theme;
    const struct search *r = data;
    int i;

    (void)w;
    if (row_type(r, row, &i) == ROW_MORE)
        ui_row_text(str(LANG_IPODUI_SEARCH_MORE), r->list.style.text_x, h,
                    sel ? t->sel_fg : t->list_fg_sub);
}

/* -- the strip --------------------------------------------------------------- */

/* A rounded rect at LCD x, y (current viewport) with one colour per row,
 * anti-aliased corners of radius rad blended over what is there. */
static void round_rect(int x, int y, int w, int h, int rad,
                       const unsigned char *const *c,
                       const unsigned short *col)
{
    int i, cr;

    lcd_set_drawmode(DRMODE_FG);
    for (i = 0; i < h; i++)
    {
        lcd_set_foreground(col[i]);
        cr = i < rad ? i : i >= h - rad ? i - (h - rad) : -1;
        if (cr < 0)
        {
            lcd_hline(x, x + w - 1, y + i);
            continue;
        }
        lcd_alpha_bitmap_part(c[i < rad ? 0 : 2], 0, cr, rad, x, y + i,
                              rad, 1);
        lcd_hline(x + rad, x + w - rad - 1, y + i);
        lcd_alpha_bitmap_part(c[i < rad ? 1 : 3], 0, cr, rad,
                              x + w - rad, y + i, rad, 1);
    }
    lcd_set_drawmode(DRMODE_SOLID);
}

static int letter_x(const struct search *r, int k)
{
    int x = LET_X0, j;

    for (j = r->first; j < k; j++)
        x += srl_w[j] + LET_TRACK;
    return x;
}

/* Keeps the boxed letter (and its box) inside the letters' area */
static void follow(struct search *r)
{
    if (r->pos < r->first)
        r->first = r->pos;
    while (r->first < r->pos
           && letter_x(r, r->pos) + srl_w[r->pos] + BOX_PAD > LET_CLIP1)
        r->first++;
}

/* A viewport of x .. x + w over the LCD rows [y0, y1): what is drawn in it
 * at LCD y goes to y - y0 and is clipped to the band. */
static void band_vp(struct viewport *vp, int x, int w, int y0, int y1)
{
    const struct ui_theme *t = ui_theme;

    memset(vp, 0, sizeof(*vp));
    vp->x = x;
    vp->y = y0;
    vp->width = w;
    vp->height = y1 - y0;
    vp->font = t->font_ui;
    vp->drawmode = DRMODE_SOLID;
    vp->fg_pattern = STRIP_FG;
    vp->bg_pattern = t->list_bg;
    lcd_set_viewport(vp);
}

/* The letters' area: the strip's rows, the box, the letters, clipped to
 * [y0, y1). Inside the strip's flat middle only (never its corners), so
 * a wheel step repaints it on its own. */
static void draw_letters(const struct search *r, int y0, int y1)
{
    unsigned short col[BOX_H];
    struct viewport vp;
    int i, k, x, ty = LET_BASE - SRL_CAP - y0;

    band_vp(&vp, LET_CLIP0, LET_CLIP1 - LET_CLIP0, y0, y1);
    for (i = 1; i < STRIP_H - 1; i++)
    {
        lcd_set_foreground(r->strip_col[i]);
        lcd_hline(0, vp.width - 1, STRIP_Y + i - y0);
    }
    for (i = 0; i < BOX_H; i++)
        col[i] = ui_paint_selection_row(i, BOX_H);
    round_rect(letter_x(r, r->pos) - LET_CLIP0 - BOX_PAD, BOX_Y - y0,
               srl_w[r->pos] + 2 * BOX_PAD, BOX_H, 3, corner3, col);

    /* the letters: cap-13 glyphs from their atlas (srchletters.h; owner
     * photo search-empty-b, larger than the list's text) */
    lcd_set_foreground(STRIP_FG);
    lcd_set_drawmode(DRMODE_FG);
    x = LET_X0 - LET_CLIP0;
    for (k = r->first; k < SRCH_ALPHABET_N && x < vp.width; k++)
    {
        lcd_alpha_bitmap_part(srl_alpha, srl_x[k], 0, SRL_STRIDE, x, ty,
                              MIN(srl_w[k], vp.width - x), SRL_H);
        x += srl_w[k] + LET_TRACK;
    }
    lcd_set_viewport(NULL);
}

/* The field: the typed text (its tail, if long) and the cursor, clipped
 * to [y0, y1) */
static void draw_field(const struct search *r, int y0, int y1)
{
    const struct ui_theme *t = ui_theme;
    unsigned short col[FIELD_H];
    struct viewport vp;
    int i, w = 0, x, inner = FIELD_W - 2 * FIELD_PAD;

    col[0] = FIELD_RIM;
    col[1] = FIELD_RIM2;
    for (i = 2; i < FIELD_H; i++)
        col[i] = LCD_RGBPACK(0xff, 0xff, 0xff);
    band_vp(&vp, 0, LCD_WIDTH, y0, y1);
    round_rect(FIELD_X, FIELD_Y - y0, FIELD_W, FIELD_H, 4, corner4, col);

    band_vp(&vp, FIELD_X + FIELD_PAD, inner, y0, y1);
    if (r->len)
        font_getstringsize(r->raw, &w, NULL, t->font_ui);
    x = MIN(0, inner - 2 - w);              /* the tail stays in view */
    lcd_set_foreground(FIELD_FG);
    lcd_set_drawmode(DRMODE_FG);
    if (r->len)                 /* the cursor follows typed text only
                                   (photo search-empty-b shows none) */
    {
        lcd_putsxy(x, LET_BASE - (int)font_get(t->font_ui)->ascent - y0,
                   r->raw);
        lcd_vline(x + w + 1, FIELD_Y + (FIELD_H - CUR_H) / 2 - y0,
                  FIELD_Y + (FIELD_H + CUR_H) / 2 - 1 - y0);
    }
    lcd_set_viewport(NULL);
}

/* The whole strip, clipped to the LCD band [y, y + h): the list's
 * overlay hook draws it after the rows of that band, so its corners
 * blend over freshly drawn rows only. */
static void draw_strip(const struct search *r, int y, int h)
{
    struct viewport vp;
    int y0 = MAX(y, STRIP_Y), y1 = MIN(y + h, STRIP_Y + STRIP_H);

    if (y1 <= y0)
        return;
    band_vp(&vp, 0, LCD_WIDTH, y0, y1);
    round_rect(STRIP_X, STRIP_Y - y0, STRIP_W, STRIP_H, 5, corner5,
               r->strip_col);
    lcd_set_viewport(NULL);
    draw_field(r, y0, y1);
    draw_letters(r, y0, y1);
}

static void sr_overlay(void *data, int y, int h)
{
    const struct search *r = data;

    if (r->strip)
        draw_strip(r, y, h);
    ui_libfoot_overlay(&r->list, y, h);     /* "Updating Library..." */
}

/* -- searching ------------------------------------------------------------- */

static bool search_cancel(void)
{
    if (button_queue_count() > 0)
    {
        cancelled = true;
        return true;
    }
    return false;
}

static void match_playlists(struct search *r)
{
    char name[MAX_PATH];
    int k;

    r->npl = 0;
    if (!r->q.len)
        return;
    for (k = 0; k < r->pln; k++)
    {
        r->plm[r->npl] = k;
        if (srch_match(pl_name(r, r->npl, name, sizeof(name)), &r->q))
            r->npl++;
    }
}

/* The list shows the new rows: from the top, nothing selected while the
 * strip shows */
static void rows_changed(struct search *r)
{
    ui_list_set_count(&r->list, row_count(r));
    if (r->strip)
    {
        r->list.selected = -1;
        r->list.top = 0;
    }
    ui_invalidate(UI_DIRTY_BODY);
}

static void run_search(struct search *r)
{
    struct srch_query prev = r->q;
    bool capped = false, was_due = r->due;
    int v, gen, y, h;

    r->due = false;
    r->updating = lib_state() == LIB_UPDATING;
    srch_fold_query(r->raw, &r->q);
    if (!was_due && r->view > 0 && r->q.len && r->q.len == prev.len
        && lib_state() == LIB_READY && r->gen == lib_generation()
        && !memcmp(r->q.folded, prev.folded, prev.len))
    {
        /* a trailing space typed or deleted folds to the same query: the
         * same rows, so no search and no full redraw; only the typed text
         * shows it (the field, and "Results for:" on the top row) */
        if (ui_list_row_band(&r->list, 0, &y, &h))
            ui_invalidate_band(y, h);
        ui_invalidate_band(STRIP_Y, STRIP_H);
        return;
    }
    if (!r->len || !r->q.len)
    {
        r->nres = 0;
        r->capped = false;
    }
    else if (lib_state() != LIB_READY)
    {
        r->nres = 0;            /* "Updating Library...": retried when ready */
        r->capped = false;
        r->due = true;
    }
    else
    {
        /* lib_search_begin() ran at entry (or when the library became
         * ready): never again per keystroke (0 reads per key) */
        cancelled = false;
        gen = ui_hold_db_gen();
        cpu_boost(true);
        v = lib_search(&r->q, r->view, &capped, search_cancel);
        cpu_boost(false);
        if (v >= 0)
        {
            r->view = v;
            r->nres = MAX(0, lib_count(v));
            r->capped = capped;
            r->gen = lib_generation();
        }
        else if (cancelled || gen != ui_hold_db_gen())
        {
            r->due = true;      /* a key came, or the database moved: the
                                   rows stay until it runs again */
            return;
        }
        else
        {
            r->nres = 0;
            r->capped = false;
        }
        UPDATE_TRACE("search screen q=%d n=%d pl=%d cap=%d rc=%d",
                     r->len, r->nres, r->npl, (int)r->capped, v);
    }
    match_playlists(r);
    rows_changed(r);
}

/* -- the screen ---------------------------------------------------------- */

static void strip_show(struct search *r, bool on)
{
    r->strip = on;
    if (on)
    {
        r->list.selected = -1;
        r->list.top = 0;
    }
    else
        ui_list_set_count(&r->list, row_count(r));  /* selection in range */
    ui_invalidate(UI_DIRTY_BODY);
}

static int sr_enter(struct ui_screen *s, const void *arg)
{
    const struct ui_theme *t = ui_theme;
    struct search *r = ui_screen_alloc(s, sizeof(struct search));
    struct ui_row_style st;
    struct viewport vp;
    int i;

    if (!r)
        return -1;
    s->title = arg ? arg : str(LANG_IPODUI_SEARCH);
    r->view = -1;
    r->pln = ui_pl_read_dir(r->ploff, r->plnames, SR_PL_MAX, SR_PL_BYTES);
    r->begun = lib_state() == LIB_READY ? lib_search_begin() : -1;
    r->updating = lib_state() == LIB_UPDATING;
    r->gen = lib_generation();
    r->next_poll = current_tick + STATE_POLL;
    for (i = 0; i < STRIP_H; i++)
        r->strip_col[i] = ui_gloss_color(&strip_gloss, i, STRIP_H);

    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;
    ui_list_init(&r->list, &vp, 1, sr_label, r);
    memset(&st, 0, sizeof(st));
    st.rows = t->list_rows_art;
    st.thumb = t->art_box;
    st.text_x = t->art_text_x;
    st.flags = UI_ROW_TWO_LINE | UI_ROW_SEP;
    ui_list_set_style(&r->list, &st);
    r->list.get_sublabel = sr_sublabel;
    r->list.has_chevron = sr_chevron;
    r->list.draw_thumb = sr_thumb;
    r->list.draw_extra = sr_extra;
    r->list.overlay = sr_overlay;
    strip_show(r, true);
    live = r;
    return 0;
}

static void sr_leave(struct ui_screen *s)
{
    live = NULL;
    lib_search_end();           /* the rows and the result view */
    ui_screen_free(s);
}

static void sr_draw(struct ui_screen *s, unsigned dirty)
{
    struct search *r = s->priv;
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    if (dirty & UI_DIRTY_BODY)
        ui_list_draw(&r->list, r->list.vp.y, r->list.vp.height);
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, h);
        if (r->letters_only && r->strip)
            draw_letters(r, STRIP_Y, STRIP_Y + STRIP_H);    /* wheel step */
        else
            for (i = 0; i < n; i++)
                ui_list_draw(&r->list, y[i], h[i]);
    }
    r->letters_only = false;
    lib_trace_frame();
}

static int sr_tick(struct ui_screen *s)
{
    struct search *r = s->priv;
    int wait, i;

    if (!TIME_BEFORE(current_tick, r->next_poll))
    {
        r->next_poll = current_tick + STATE_POLL;
        if (r->updating && lib_state() == LIB_READY)
        {
            r->updating = false;
            if (r->begun < 0)
                r->begun = lib_search_begin();
            if (r->len)
                r->due = true;
            else
                ui_invalidate(UI_DIRTY_BODY);   /* the second line */
        }
        else if (lib_state() == LIB_READY && lib_generation() != r->gen)
        {
            /* a commit while open: the group rows and the result view
             * went stale (one begin per commit, not per key) */
            lib_search_end();
            r->view = -1;
            r->begun = lib_search_begin();
            r->gen = lib_generation();
            r->due = r->len > 0;
        }
    }
    if (r->due && lib_state() == LIB_READY && !ui_list_gliding(&r->list))
        run_search(r);

    wait = ui_list_tick(&r->list);
    if (wait == UI_TICK_ANIM)
    {
        ui_libfoot_tick(&r->list);  /* the glide frames draw it */
        return wait;
    }
    i = ui_libfoot_tick(&r->list);
    if (i != TIMEOUT_BLOCK)
        wait = wait == TIMEOUT_BLOCK ? i : MIN(wait, i);
    if (r->updating || r->due)
        wait = wait == TIMEOUT_BLOCK ? STATE_POLL : MIN(wait, STATE_POLL);
    return wait;
}

static void type_char(struct search *r, char c)
{
    if (r->len >= SRCH_MAX_Q)
        return;
    if (c == ' ' && (!r->len || r->raw[r->len - 1] == ' '))
        return;                 /* no leading or double space */
    r->raw[r->len++] = c;
    r->raw[r->len] = '\0';
    run_search(r);
}

static enum ui_result strip_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    struct search *r = s->priv;
    int p;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            p = r->pos + in->wheel_steps;
            p = p < 0 ? 0 : p >= SRCH_ALPHABET_N ? SRCH_ALPHABET_N - 1 : p;
            if (p != r->pos)
            {
                r->pos = p;
                follow(r);
                r->letters_only = true;
                ui_invalidate_band(STRIP_Y, STRIP_H);
            }
            return UI_HANDLED;

        case UI_KEY_SELECT:
            if (in->release && !in->long_press)
                type_char(r, SRCH_ALPHABET[r->pos]);
            return UI_HANDLED;

        case UI_KEY_NEXT:               /* a space; never a skip or seek */
            if (in->release && !in->long_press)
                type_char(r, ' ');
            return UI_HANDLED;

        case UI_KEY_PREV:               /* delete; never a skip or seek */
            if (in->release && !in->long_press && r->len)
            {
                r->raw[--r->len] = '\0';
                run_search(r);
            }
            return UI_HANDLED;

        case UI_KEY_MENU:
            if (!in->release || in->long_press)
                return UI_IGNORED;
            strip_show(r, false);
            if (row_count(r) > 1)
                r->list.selected = 1;   /* the first result */
            return UI_HANDLED;

        default:
            return UI_IGNORED;
    }
}

/* SELECT on a result: open or play it */
static void open_result(struct search *r, int i)
{
    struct ui_browse_arg a;
    char title[40];
    int id;
    enum srch_kind k = result_kind(r, i, &id);

    memset(&a, 0, sizeof(a));
    a.f.artist = a.f.album = a.f.genre = a.f.composer = LIB_ANY;
    a.title = shown_label(lib_label(r->view, i, title, sizeof(title)));
    switch (k)
    {
        case SRCH_ARTIST:
            a.cat = LIB_ALBUMS;
            a.f.artist = id;
            a.all = UI_ALL_SONGS;
            ui_push(&ui_browse_screen, &a);
            return;
        case SRCH_ALBUM:
            a.cat = LIB_SONGS;
            a.f.album = id;
            ui_push(&ui_browse_screen, &a);
            return;
        case SRCH_BOOK:
        {
            /* the page needs a LIB_AUDIOBOOKS row: the books view (from
             * the precompute file; a user action), the row of this book,
             * then the page (it keeps nothing of that view) */
            int bv = lib_open(LIB_AUDIOBOOKS, NULL), row;
            struct ui_book_page_arg pa = { bv, 0, a.title };

            if (bv > 0 && (row = lib_find(bv, id)) >= 0)
            {
                pa.idx = row;
                if (ui_push(&ui_book_page_screen, &pa) < 0
                    && lib_play(bv, row, false) == 0)
                    ui_show_wps();      /* no page: the book plays, as
                                           from Audiobooks */
            }
            if (bv > 0)
                lib_close(bv);
            return;
        }
        default:
            break;
    }
    if (lib_play(r->view, i, false) == 0)
        ui_show_wps();
    else
        logf("ipodui: search: play %d failed", i);
}

static void hold_result(struct search *r, int i)
{
    struct ui_hold_arg a;
    int id;
    enum srch_kind k = result_kind(r, i, &id);

    memset(&a, 0, sizeof(a));
    a.view = r->view;
    a.idx = i;
    a.db_gen = ui_hold_db_gen();
    a.ctx = (k == SRCH_SONG || k == SRCH_EPISODE) ? UI_HOLD_SONG
                                                  : UI_HOLD_GROUP;
    a.tc_idx = a.ctx == UI_HOLD_SONG ? id + 1 : 0;
    a.spoken = k == SRCH_BOOK || k == SRCH_EPISODE;
    ui_hold_push(&a);
}

static enum ui_result list_input(struct ui_screen *s,
                                 const struct ui_input *in)
{
    struct search *r = s->priv;
    char name[MAX_PATH];
    int y[2], h[2], i, n;
    enum row_type t;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            n = ui_list_move(&r->list, in->wheel_steps, y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            return UI_HANDLED;

        case UI_KEY_SELECT:
            t = row_type(r, r->list.selected, &i);
            if (in->long_press && !in->release)
            {
                if (t == ROW_RESULT)
                    hold_result(r, i);
                return UI_HANDLED;
            }
            if (!in->release || in->long_press)
                return UI_HANDLED;
            switch (t)
            {
                case ROW_RESULT:
                    open_result(r, i);
                    break;
                case ROW_PLAYLIST:
                    strlcpy(name, r->plnames + r->ploff[r->plm[i]],
                            sizeof(name));
                    ui_pl_play(name);
                    break;
                default:                /* the top row, "More..." */
                    strip_show(r, true);
                    break;
            }
            return UI_HANDLED;

        default:
            return UI_IGNORED;          /* MENU leaves Search */
    }
}

static enum ui_result sr_input(struct ui_screen *s, const struct ui_input *in)
{
    struct search *r = s->priv;

    return r->strip ? strip_input(s, in) : list_input(s, in);
}

const struct ui_screen_ops ui_search_screen = {
    .name    = "search",
    .enter   = sr_enter,
    .leave   = sr_leave,
    .draw    = sr_draw,
    .input   = sr_input,
    .timeout = NULL,
    .tick    = sr_tick,
};

int ui_search_strip_pos(void)
{
    const struct ui_screen *top = ui_top();

    if (!live || !live->strip || !top || top->ops != &ui_search_screen)
        return -1;
    return live->pos;
}
