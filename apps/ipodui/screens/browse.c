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
 * iPod UI shell: generic library browse screen
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
/* One list over one library view (library.h): Artists, Albums, Songs,
 * Genres, Composers, Compilations, Audiobooks, Podcasts, and every
 * filtered child of them. The view is an index array owned by the
 * library; labels are fetched for the rows being drawn only.
 *
 * Children (7G, photo-artist-albums.jpg):
 *   Artists -> the artist's albums, "All Songs" first
 *   Genres -> the genre's artists, "All Albums" first
 *     "All Albums" -> the genre's albums, "All Songs" first
 *   Composers -> the composer's albums, "All Songs" first
 *   Albums, Compilations -> the album's songs (track order)
 *   Podcasts -> the show's episodes (newest first)
 *   Audiobooks -> the book's page (audiobooks.c: Resume + entries)
 *   Songs, episodes -> play from that row, then the WPS.
 * Holding SELECT on a row opens the hold menu (holdmenu.c, P11): a song,
 * an episode or a book's track gets Add to On-The-Go / Browse Album /
 * Browse Artist / Cancel, any other row (artist, album, genre, composer,
 * book, show) Add to On-The-Go / Cancel.
 *
 * Row styles (docs/design/7g-spec.md "List geometry", owner photos):
 *   albums (any album list, compilations): two lines with mini art,
 *     4 per view: the title over the album artist (unfiltered lists,
 *     albums-list-twoline-b) or "N Songs" (an artist's, genre's or
 *     composer's albums); "All Songs" has the note tile and "N Songs".
 *   books (Phase 6 owner decision): the same as unfiltered albums, cover
 *     over the author (album artist, else artist), the book glyph when
 *     there is no cover; the chevron stays, since a book opens its page.
 *   podcast shows: the same with show art (or the podcast glyph),
 *     "N Episodes" and the blue unplayed dot left of the title (~).
 *   songs (with an artist line: Songs, an artist's All Songs, ...): two
 *     lines, 5 per view, title over artist (songs-list-scrollbar-b).
 *   episodes: two lines, 5 per view, the dot, the show's name (~). The
 *     dot is "never started" (lib_unplayed()); no progress bar (P11, ⚠️
 *     P1: the 7G has none).
 *   everything else (artists, genres, an album's tracks):
 *     single-line, 9 per view.
 * Videos (Phase 14a, video builds; docs/phase14a-brief.md 3.6): Movies
 *   and TV Shows are album-like (poster / show art, title over the
 *   duration or "N Episodes"); a show opens its Seasons ("Season N",
 *   skipped when it has one), which open the Episodes (the library's
 *   "N. Title" over the duration, the unplayed dot). A movie or episode
 *   plays on the video screen (ui_video_play()). An empty Movies or TV
 *   Shows list shows one row, "No Movies" / "No TV Shows". In Podcasts
 *   a video episode carries a small screen mark at the right edge and
 *   plays on the video screen too.
 * Mini art comes from the art cache through rowart.c (read at rest only;
 * a glyph while gliding or while the builder hasn't reached the album).
 *
 * State: the screen arena (P11, screen_stack.h), so opening a list never
 * allocates from the audio buffer; the header title lives in it too (the
 * arena never moves). */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "file.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "core_alloc.h"
#include "audio.h"
#include "playback.h"
#include "font.h"
#include "lang.h"
#include "logf.h"
#include "ipodui/ipodui.h"
#include "ipodui/theme.h"
#include "ipodui/list.h"
#include "ipodui/menu.h"
#include "ipodui/qscroll.h"
#include "ipodui/anim.h"
#include "ipodui/rowart.h"
#include "ipodui/npmark.h"
#include "ipodui/paint.h"
#include "ipodui/rowglyphs.h"
#include "ipodui/screens/screens.h"
#include "ipodui/screens/holdmenu.h"
#ifdef HAVE_BCMFW_VIDEO
#include "ipodui/screens/video.h"
#endif

#define TITLE_LEN       40
#define STATE_POLL      HZ      /* library state and generation */

struct browse {
    struct ui_list list;
    struct ui_qscroll qs;
    struct lib_filter f;
    int  view;                  /* < 0: none (the list is empty) */
    unsigned char cat;          /* enum lib_cat */
    unsigned char all;          /* enum ui_browse_all */
    bool art;                   /* rows have an art box */
    bool dot;                   /* rows have the unplayed dot */
    bool empty;                 /* video lists: the one "No ..." row shows */
    bool vmark;                 /* Podcasts: some episode is a video */
    int  all_count;             /* "All Songs": tracks under it, -1 unknown */
    long next_art_poll;         /* tick of the next art builder check */
    bool ready;                 /* lib_state() was LIB_READY at open */
    int  generation;            /* lib_generation() of the view */
    int  pin_gen;               /* #67: lib_pin_generation() at open */
    int  sel_id;                /* lib_id() of the selected row, -1 none */
    unsigned char mark;         /* #69: UI_NPMARK_* the rows compare, 0 none */
    struct ui_npmark_now np;    /* #69: what plays, as the rows show it */
    long next_poll;             /* tick of the next state check */
    char title[TITLE_LEN];      /* the header's (s->title points here) */
};
/* P11 screen arena bound (screen_stack.h): the state moves into the
 * arena */
_Static_assert(sizeof(struct browse) <= UI_ARENA_B_BROWSE, "browse: arena bound");

static const short all_lang[] = { -1, LANG_IPODUI_ALL_SONGS,
                                   LANG_IPODUI_ALL_ALBUMS };

/* An untagged artist / album row: the language string, not library.c's
 * sort key (library.h). A show with no name (stored empty, brief 3.2 M7)
 * is "Unknown Show". */
static const char *shown_label(const char *l, bool show)
{
    if (!strcmp(l, lib_unknown_artist))
        return str(LANG_IPODUI_UNKNOWN_ARTIST);
    if (!strcmp(l, lib_unknown_album))
#ifdef HAVE_BCMFW_VIDEO
        return str(show ? LANG_IPODUI_UNKNOWN_SHOW : LANG_IPODUI_UNKNOWN_ALBUM);
#else
        return str(LANG_IPODUI_UNKNOWN_ALBUM);
#endif
    (void)show;
    return l;
}

/* -- video lists (HAVE_BCMFW_VIDEO) ---------------------------------------- */

#ifdef HAVE_BCMFW_VIDEO
static bool is_movies(const struct browse *b) { return b->cat == LIB_MOVIES; }
static bool is_tvshows(const struct browse *b) { return b->cat == LIB_TVSHOWS; }
static bool is_seasons(const struct browse *b)
{
    return b->cat == LIB_TVSEASONS;
}
static bool is_vepisodes(const struct browse *b)
{
    return b->cat == LIB_TVEPISODES;
}
#else
#define is_movies(b)    false
#define is_tvshows(b)   false
#define is_seasons(b)   false
#define is_vepisodes(b) false
#endif

static int lib_rows(const struct browse *b);

/* Movies / TV Shows with nothing in them: one row says so. */
static bool empty_state(const struct browse *b)
{
    return (is_movies(b) || is_tvshows(b)) && lib_rows(b) == 0;
}

static struct browse *state(struct ui_screen *s)
{
    return s->priv;             /* pinned (arena): list.data stays valid */
}

/* Rows that play (no chevron): songs, books, a show's episodes. */
static bool is_track_view(const struct browse *b)
{
    return b->cat == LIB_SONGS || b->cat == LIB_AUDIOBOOKS ||
           is_movies(b) || is_vepisodes(b) ||
           (b->cat == LIB_PODCASTS && b->f.album != LIB_ANY);
}

static int extra_rows(const struct browse *b, int n)
{
    return (b->all != UI_ALL_NONE && n > 0) ? 1 : 0;
}

static int lib_rows(const struct browse *b)
{
    return b->view > 0 ? lib_count(b->view) : 0;
}

/* row -> library index, -1 for the "All ..." row (and the empty list's
 * one row) */
static int lib_idx(const struct browse *b, int row)
{
    if (empty_state(b))
        return -1;
    return row - extra_rows(b, lib_rows(b));
}

/* the rows the list shows */
static int total_rows(const struct browse *b)
{
    if (empty_state(b))
        return 1;
    return lib_rows(b) + extra_rows(b, lib_rows(b));
}

/* -- list callbacks -------------------------------------------------------- */

static const char *browse_label(int row, void *data, char *buf, size_t len)
{
    struct browse *b = data;
    int i = lib_idx(b, row);

    if (empty_state(b))
        return str(is_movies(b) ? LANG_IPODUI_NO_MOVIES
                                : LANG_IPODUI_NO_TV_SHOWS);
    if (i < 0)
        return b->all > 0 ? (const char *)str(all_lang[b->all]) : NULL;
    if (is_seasons(b))
    {
        snprintf(buf, len, str(LANG_IPODUI_SEASON_N), lib_id(b->view, i));
        return buf;
    }
    return shown_label(lib_label(b->view, i, buf, len), is_tvshows(b));
}

static bool browse_chevron(int row, void *data)
{
    struct browse *b = data;
    /* a book opens its page (Phase 6, owner Rev 2) */
    if (empty_state(b))
        return false;
    return lib_idx(b, row) < 0 || !is_track_view(b)
           || b->cat == LIB_AUDIOBOOKS;
}

#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a (T4): the screen mark of a video episode among the podcasts:
 * rowglyphs.h vscreen in the second line's grey, at the row's right edge
 * (an episode row has no chevron). */
#define VMARK_RIGHT 8
#define VMARK_GAP   6           /* label to mark */

static int cov_at(const unsigned char *cov, int w, int x, int y)
{
    int i = y * w + x;
    return (i & 1) ? (cov[i >> 1] & 0x0f) : (cov[i >> 1] >> 4);
}

static void draw_vmark(int x, int y, int row_h, bool sel)
{
    const struct ui_theme *t = ui_theme;
    unsigned ink = sel ? t->sel_fg_sub : t->list_fg_sub;
    int cx, cy;

    for (cy = 0; cy < VSCREEN_H; cy++)
    {
        unsigned bg = sel ? ui_paint_selection_row(y + cy, row_h)
                          : t->list_bg;

        for (cx = 0; cx < VSCREEN_W; cx++)
        {
            int a = cov_at(vscreen_cov, VSCREEN_W, cx, cy);
            if (!a)
                continue;
            lcd_set_foreground(a == 15 ? ink : ui_paint_blend(bg, ink, a));
            lcd_drawpixel(x + cx, y + cy);
        }
    }
}

#endif /* HAVE_BCMFW_VIDEO */

static void browse_extra(int row, void *data, int w, int h, bool sel)
{
    const struct ui_theme *t = ui_theme;
    struct browse *b = data;
    int i = lib_idx(b, row);

    if (i < 0)
        return;
    if (b->dot && lib_unplayed(b->view, i))
        ui_rowart_dot(b->list.style.text_x - t->dot_gutter,
                      (h - UI_ROWART_DOT_D) / 2, h, sel);
#ifdef HAVE_BCMFW_VIDEO
    if (b->vmark && lib_is_video(b->view, i))
        draw_vmark(w - VMARK_RIGHT - VSCREEN_W, (h - VSCREEN_H) / 2, h, sel);
#else
    (void)w;
#endif
}

/* #69 now-playing mark: a song (or a book's track) by its database id,
 * a book by its key (the book map's, lib_art_key() of the row). */
static bool browse_mark(int row, void *data)
{
    struct browse *b = data;
    int i = lib_idx(b, row);

    if (i < 0)
        return false;
    if (b->mark & UI_NPMARK_TC)
        return b->np.tc > 0 && lib_id(b->view, i) == b->np.tc - 1;
    return b->np.book && lib_art_key(b->view, i) == b->np.book;
}

/* The list's identity for ui_npmark_continue(): its category and
 * filter, so the same album opened again is the same list */
static uint32_t browse_key(const struct browse *b)
{
    struct { struct lib_filter f; int cat; } k;

    memset(&k, 0, sizeof(k));
    k.f = b->f;
    k.cat = b->cat;
    return ui_npmark_key(&k, sizeof(k));
}

/* Which rows can carry the mark (spec music-lists.md): song lists, the
 * Audiobooks list and a book's track list. */
static void set_mark(struct browse *b)
{
    b->mark = b->cat == LIB_SONGS ? UI_NPMARK_TC
            : b->cat != LIB_AUDIOBOOKS ? 0
            : b->f.album == LIB_ANY ? UI_NPMARK_BOOK : UI_NPMARK_TC;
    b->list.has_mark = b->mark ? browse_mark : NULL;
    if (b->mark)
        ui_npmark_get(&b->np, b->mark);
}

/* "13 Songs", "1 Song", "7 Episodes" */
static const char *count_label(int n, bool episodes, char *buf, size_t len)
{
    if (n < 0)
        return NULL;
    if (n == 1)
        return str(episodes ? LANG_IPODUI_ONE_EPISODE : LANG_IPODUI_ONE_SONG);
    snprintf(buf, len, str(episodes ? LANG_IPODUI_N_EPISODES
                                    : LANG_IPODUI_N_SONGS), n);
    return buf;
}

/* The second line: the artist (song and unfiltered album lists, from
 * the library), else the count under the row, else the show's name under
 * an episode. */
static const char *browse_sublabel(int row, void *data, char *buf,
                                   size_t len)
{
    struct browse *b = data;
    int i = lib_idx(b, row);
    const char *s;

    if (i < 0)
        return b->all == UI_ALL_SONGS
               ? count_label(b->all_count, false, buf, len) : NULL;
    s = lib_sublabel(b->view, i, buf, len);
    if (is_movies(b) || is_vepisodes(b))
        return s;                       /* the duration (V3), or none */
    if (is_seasons(b))
        return count_label(lib_child_count(b->view, i), true, buf, len);
    if (s)
        return s[0] ? s : (const char *)str(LANG_IPODUI_UNKNOWN_ARTIST);
    if (b->cat == LIB_PODCASTS && b->f.album != LIB_ANY)
        return b->title;
    return count_label(lib_child_count(b->view, i),
                       b->cat == LIB_PODCASTS || is_tvshows(b), buf, len);
}

static bool browse_thumb(int row, void *data, int size, bool fetch)
{
    struct browse *b = data;
    int i = lib_idx(b, row);
    enum ui_rowart_glyph g = b->cat == LIB_PODCASTS ? UI_GLYPH_PODCAST
                           : b->cat == LIB_AUDIOBOOKS ? UI_GLYPH_BOOK
                           : UI_GLYPH_NOTE;
    uint64_t key = i >= 0 ? lib_art_key(b->view, i) : 0;

#ifdef HAVE_BCMFW_VIDEO
    /* no poster (or none built yet): the film tile, pre-rendered once */
    if (is_movies(b) || is_tvshows(b))
        g = UI_GLYPH_FILM;
#endif
    return ui_rowart_draw(key, g, 0, 0, size, fetch);
}

/* Tracks under "All Songs": the sum of the album rows' counts (O(1)
 * each, from the precompute), -1 while any is unknown. */
static void count_all(struct browse *b)
{
    int i, n = lib_rows(b), sum = 0;

    b->all_count = -1;
    if (b->all != UI_ALL_SONGS || b->view <= 0)
        return;
    for (i = 0; i < n; i++)
    {
        int c = lib_child_count(b->view, i);
        if (c < 0)
            return;
        sum += c;
    }
    b->all_count = sum;
}

/* Picks the row style for the view (see the top of the file). */
static void set_style(struct browse *b)
{
    const struct ui_theme *t = ui_theme;
    struct ui_row_style st;
    char buf[8];
    bool shows = b->cat == LIB_PODCASTS && b->f.album == LIB_ANY;
    bool episodes = b->cat == LIB_PODCASTS && !shows;
    bool books = b->cat == LIB_AUDIOBOOKS && b->f.album == LIB_ANY;
    bool vart = !empty_state(b) && (is_movies(b) || is_tvshows(b));

    episodes = episodes || is_vepisodes(b);
    memset(&st, 0, sizeof(st));
    st.rows = t->list_rows;
    st.text_x = t->text_pad_x;
    b->art = b->cat == LIB_ALBUMS || b->cat == LIB_COMPILATIONS || shows
             || books || vart;
    b->dot = shows || episodes || vart;
    if (b->art)
    {
        st.rows = t->list_rows_art;
        st.thumb = t->art_box;
        st.text_x = t->art_text_x;
        st.flags = UI_ROW_TWO_LINE | UI_ROW_SEP;
    }
    else if (episodes || (b->cat == LIB_SONGS && b->view > 0 &&
                          lib_sublabel(b->view, 0, buf, sizeof(buf))))
    {
        st.rows = t->list_rows_two;
        st.flags = UI_ROW_TWO_LINE | UI_ROW_SEP;
    }
    if (b->dot)
        st.text_x += t->dot_gutter;
    ui_list_set_style(&b->list, &st);
    b->list.get_sublabel = (st.flags & UI_ROW_TWO_LINE) ? browse_sublabel
                                                        : NULL;
    b->list.draw_thumb = b->art ? browse_thumb : NULL;
    b->list.draw_extra = b->dot ? browse_extra : NULL;
}

/* Podcasts: does any episode play on the video screen? Then every row
 * keeps room for the screen mark (RAM flags only, no I/O). */
static void set_vmark(struct browse *b)
{
    b->vmark = false;
    b->list.extra_w = 0;
#ifdef HAVE_BCMFW_VIDEO
    if (b->cat == LIB_PODCASTS && b->f.album != LIB_ANY && b->view > 0)
    {
        int i, n = lib_rows(b);

        for (i = 0; i < n && !b->vmark; i++)
            b->vmark = lib_is_video(b->view, i);
        if (b->vmark)
            b->list.extra_w = VSCREEN_W + VMARK_RIGHT + VMARK_GAP;
    }
#endif
}

static void browse_overlay(void *data, int y, int h)
{
    struct browse *b = data;
    ui_qscroll_draw(&b->qs, &b->list, y, h);
    ui_libfoot_overlay(&b->list, y, h);     /* "Updating Library..." */
}

static char sel_letter(struct browse *b)
{
    int i = lib_idx(b, b->list.selected);
    return i >= 0 ? lib_letter(b->view, i) : 0;
}

/* -- screen ---------------------------------------------------------------- */

static int browse_enter(struct ui_screen *s, const void *arg)
{
    const struct ui_browse_arg *a = arg;
    const struct ui_theme *t = ui_theme;
    struct browse *b;
    struct viewport vp;
    int view, pin_gen;

    /* the view first (the library's pool), then our state (the arena) */
    pin_gen = lib_pin_generation();     /* #67: before the pin pass */
    view = lib_open(a->cat, &a->f);
    if (view < 0)
        logf("ipodui: browse %s: no view (%d)", a->title ? a->title : "",
             view);
    b = ui_screen_alloc(s, sizeof(struct browse));
    if (!b)
    {
        logf("ipodui: browse: no memory");
        if (view > 0)
            lib_close(view);
        return -1;
    }
    strlcpy(b->title, a->title ? a->title : "", TITLE_LEN);
    s->title = b->title;
    b->view = view;
    b->f = a->f;
    b->cat = a->cat;
    b->all = a->all;
    b->ready = lib_state() == LIB_READY;
    b->generation = lib_generation();
    b->pin_gen = pin_gen;
    b->sel_id = -1;
    b->next_poll = current_tick + STATE_POLL;
    ui_qscroll_init(&b->qs);

    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;
    ui_list_init(&b->list, &vp, 0, browse_label, b);
    b->list.has_chevron = browse_chevron;
    b->list.overlay = browse_overlay;
    b->empty = empty_state(b);
    set_style(b);
    set_mark(b);
    set_vmark(b);
    ui_list_set_count(&b->list, total_rows(b));
    count_all(b);
    b->next_art_poll = current_tick + STATE_POLL;
    return 0;
}

static void browse_leave(struct ui_screen *s)
{
    struct browse *b = state(s);

    if (b->view > 0)
        lib_close(b->view);
    ui_screen_free(s);
}

static void browse_draw(struct ui_screen *s, unsigned dirty)
{
    struct browse *b;
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX];

    /* drawing may read thumbnails (rowart.c), which yields: our state
     * is pinned (the arena), so the list stays in place meanwhile */
    b = state(s);
    if (dirty & UI_DIRTY_BODY)
    {
        ui_qscroll_reset(&b->qs);
        ui_list_draw(&b->list, b->list.vp.y, b->list.vp.height);
    }
    else if (dirty & UI_DIRTY_BAND)
    {
        int i, n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&b->list, y[i], h[i]);
    }
    lib_trace_frame();
}

/* The library finished building, or a background update was committed
 * (the RAM cache reloaded: the view went stale), while we were open:
 * reopen the view. The selection stays on the same item when it is still
 * there (ids survive an update), else on the same row (clamped); the
 * selected row keeps its place on screen. */
static void reopen(struct ui_screen *s)
{
    struct browse *b = state(s);
    struct lib_filter f = b->f;
    int cat = b->cat, view, sel, extra, i;
    int sel_id = b->sel_id;
    int above = b->list.selected - b->list.top;

    if (b->view > 0)
        lib_close(b->view);
    b->pin_gen = lib_pin_generation();  /* #67: before the pin pass */
    view = lib_open(cat, &f);
    b->view = view;
    b->ready = true;
    b->generation = lib_generation();
    ui_qscroll_reset(&b->qs);

    extra = extra_rows(b, lib_rows(b));
    sel = b->list.selected;
    i = lib_find(view, sel_id);
    if (i >= 0)
        sel = i + extra;
    b->list.selected = sel;
    b->list.top = sel - above;          /* clamped by set_count */
    if (b->empty != empty_state(b))     /* the "No ..." row came or went */
    {
        b->empty = empty_state(b);
        set_style(b);
    }
    set_vmark(b);
    ui_list_set_count(&b->list, total_rows(b));
    count_all(b);
    ui_rowart_flush();                  /* a rebuild may change art */
    ui_invalidate(UI_DIRTY_BODY);
    logf("ipodui: browse reopened, %d rows", lib_rows(b));
}

/* A reopen is due when the library is ready and the view is from another
 * database generation (or there was none). Cheap: a few loads and
 * compares. */
static bool reopen_due(const struct browse *b)
{
    return lib_state() == LIB_READY
           && (!b->ready || lib_generation() != b->generation
               /* #67: the pinned book changed (books only, RAM only) */
               || (b->cat == LIB_AUDIOBOOKS
                   && lib_pin_generation() != b->pin_gen));
}

/* reopen_due(), checked once per STATE_POLL at most, and not while gliding
 * (a later tick) */
static bool stale(struct browse *b)
{
    if (TIME_BEFORE(current_tick, b->next_poll) || ui_list_gliding(&b->list))
        return false;
    b->next_poll = current_tick + STATE_POLL;
    return reopen_due(b);
}

/* Remembers the selected row's item while the view is fresh, for
 * reopen(). */
static void track_selection(struct browse *b)
{
    int i = lib_idx(b, b->list.selected);
    int id = i >= 0 ? lib_id(b->view, i) : -1;

    if (b->view > 0 && lib_generation() == b->generation && id != -2)
        b->sel_id = id;
}

/* The art builder made progress: rows showing a glyph may have art now
 * (checked at rest, once per STATE_POLL at most). */
static void poll_art(struct browse *b)
{
    if (!b->art || ui_list_gliding(&b->list) || ui_anim_active()
        || TIME_BEFORE(current_tick, b->next_art_poll))
        return;
    b->next_art_poll = current_tick + STATE_POLL;
    if (ui_rowart_poll())
        ui_invalidate_band(b->list.vp.y, b->list.vp.height);
}

static int min_wait(int a, int b)
{
    if (a == TIMEOUT_BLOCK)
        return b;
    if (b == TIMEOUT_BLOCK)
        return a;
    return MIN(a, b);
}

static int browse_tick(struct ui_screen *s)
{
    struct browse *b = state(s);
    int wait;

    if (stale(b))
        reopen(s);
    else
        track_selection(b);

    wait = ui_list_tick(&b->list);  /* the art fill may yield: pinned */
    ui_npmark_sync(&b->list, &b->np, b->mark);  /* at rest; RAM only */
    poll_art(b);    /* on the main loop's own wake-ups (<= 1 s while lit) */
    if (wait != UI_TICK_ANIM)
    {
        wait = min_wait(wait, ui_qscroll_tick(&b->qs, &b->list));
        wait = min_wait(wait, ui_libfoot_tick(&b->list));
    }
    else
    {
        ui_qscroll_tick(&b->qs, &b->list);
        ui_libfoot_tick(&b->list);
    }

    /* while not ready, poll; once ready the main loop's own wake-ups
     * (at most 1 s apart while lit, 5 s when dark) are often enough to
     * notice a background commit */
    if (!b->ready && wait != UI_TICK_ANIM)
        wait = min_wait(wait, STATE_POLL);
    lib_trace_frame();
    return wait;
}

/* Pushes the list a row opens. Returns false if the row plays instead. */
static bool open_child(struct ui_screen *s, struct browse *b, int row)
{
    struct ui_browse_arg a;
    char buf[TITLE_LEN];
    int i = lib_idx(b, row), id;

    memset(&a, 0, sizeof(a));
    a.f = b->f;
    a.title = b->title;

    if (i < 0)
    {
        /* "All Songs" / "All Albums": same filter, one level down */
        if (b->all == UI_ALL_SONGS)
            a.cat = LIB_SONGS;
        else
        {
            a.cat = LIB_ALBUMS;
            a.all = UI_ALL_SONGS;
        }
        ui_push(&ui_browse_screen, &a);
        return true;
    }

    id = lib_id(b->view, i);
    a.title = shown_label(lib_label(b->view, i, buf, sizeof(buf)),
                          is_tvshows(b));
    switch (b->cat)
    {
        case LIB_ARTISTS:
            a.cat = LIB_ALBUMS;
            a.f.artist = id;
            a.all = UI_ALL_SONGS;
            break;
        case LIB_GENRES:
            a.cat = LIB_ARTISTS;
            a.f.genre = id;
            a.all = UI_ALL_ALBUMS;
            break;
        case LIB_COMPOSERS:
            a.cat = LIB_ALBUMS;
            a.f.composer = id;
            a.all = UI_ALL_SONGS;
            break;
        case LIB_ALBUMS:
        case LIB_COMPILATIONS:
            a.cat = LIB_SONGS;
            a.f.album = id;
            break;
        case LIB_AUDIOBOOKS:
        {
            /* any book opens its page (owner Rev 2); with no page (the
             * push refused it) the book plays from here, as Phase 5 */
            struct ui_book_page_arg pa = { b->view, i, a.title };
            return ui_push(&ui_book_page_screen, &pa) >= 0;
        }
        case LIB_PODCASTS:
            if (b->f.album != LIB_ANY)
                return false;           /* an episode */
            a.cat = LIB_PODCASTS;
            a.f.album = id;
            break;
#ifdef HAVE_BCMFW_VIDEO
        case LIB_TVSHOWS:
        {
            /* the show's seasons; a show of one season opens its
             * episodes at once (T2) */
            struct lib_filter sf = LIB_FILTER_ANY;
            int sv, ns;

            sf.album = id;
            sv = lib_open(LIB_TVSEASONS, &sf);
            ns = sv > 0 ? lib_count(sv) : 0;
            if (sv > 0)
                lib_close(sv);
            a.f.album = id;
            if (ns == 1)
                a.cat = LIB_TVEPISODES;
            else
                a.cat = LIB_TVSEASONS;
            break;
        }
        case LIB_TVSEASONS:
            /* the season's episodes; the filter's album is the show */
            a.cat = LIB_TVEPISODES;
            a.f.season = id;            /* the season number */
            snprintf(buf, sizeof(buf), str(LANG_IPODUI_SEASON_N), id);
            a.title = buf;
            break;
        case LIB_MOVIES:
        case LIB_TVEPISODES:
            return false;               /* plays */
#endif
        default:
            return false;
    }
    (void)s;
    ui_push(&ui_browse_screen, &a);
    return true;
}

/* Rows of single tracks for the hold menu (a song, an episode, a book's
 * track); everything else is a group (⚠️ H1). */
static bool track_rows(const struct browse *b)
{
    return b->cat == LIB_SONGS || is_movies(b) || is_vepisodes(b)
           || ((b->cat == LIB_PODCASTS || b->cat == LIB_AUDIOBOOKS)
               && b->f.album != LIB_ANY);
}

/* Hold SELECT on library row i: the hold menu (P11). */
static void open_hold(const struct browse *b, int i)
{
    struct ui_hold_arg a;
    int id = lib_id(b->view, i);

    memset(&a, 0, sizeof(a));
    a.ctx = track_rows(b) ? UI_HOLD_SONG : UI_HOLD_GROUP;
    a.view = b->view;
    a.idx = i;
    a.tc_idx = (a.ctx == UI_HOLD_SONG && id >= 0) ? id + 1 : 0;
    a.db_gen = ui_hold_db_gen();
    a.spoken = b->cat == LIB_AUDIOBOOKS || b->cat == LIB_PODCASTS;
    ui_hold_push(&a);
}

static enum ui_result browse_input(struct ui_screen *s,
                                   const struct ui_input *in)
{
    struct browse *b = state(s);
    int y[2], h[2], i, n, row;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            n = ui_list_move(&b->list, in->wheel_steps, y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            ui_qscroll_wheel(&b->qs, &b->list, in->wheel_velocity,
                             sel_letter(b));
            return UI_HANDLED;

        case UI_KEY_SELECT:
            if (reopen_due(b))
                reopen(s);  /* the press lands on the reopened view */
            if (b->list.count <= 0)
                return UI_HANDLED;
            row = b->list.selected;
            i = lib_idx(b, row);
            if (empty_state(b))
                return UI_HANDLED;      /* the "No ..." row does nothing */

            if (in->long_press && !in->release)
            {
                if (i >= 0 && b->view > 0)
                {
                    ui_qscroll_reset(&b->qs);
                    open_hold(b, i);
                }
                return UI_HANDLED;
            }
            if (!in->release || in->long_press)
                return UI_HANDLED;

            ui_qscroll_reset(&b->qs);
            if (open_child(s, b, row))
                return UI_HANDLED;
#ifdef HAVE_BCMFW_VIDEO
            /* a movie, an episode, or a video among the podcasts: the
             * video screen, never the audio playlist (brief 3.2a) */
            if (i >= 0 && (is_movies(b) || is_vepisodes(b)
                           || lib_is_video(b->view, i)))
            {
                char path[MAX_PATH];

                if (lib_path(b->view, i, path, sizeof(path)))
                {
                    ui_video_play(path, lib_id(b->view, i));
                    if (b->dot)
                        lib_progress_refresh(b->view);
                }
                else
                    logf("ipodui: browse: video has no path");
                return UI_HANDLED;
            }
#endif
            /* the playing track: carry on with it (#69 mark) */
            if (ui_npmark_continue(&b->list, &b->np, b->mark,
                                   browse_key(b), -1))
                return UI_HANDLED;
            /* a track: play the list from here, then Now Playing */
            if (lib_play(b->view, i, false) == 0)
            {
                ui_npmark_origin(browse_key(b));
                ui_show_wps();
                if (b->dot)     /* a started episode loses its dot */
                    lib_progress_refresh(b->view);
            }
            else
                logf("ipodui: browse: play failed");
            return UI_HANDLED;

        default:
            return UI_IGNORED;
    }
}

const struct ui_screen_ops ui_browse_screen = {
    .name    = "browse",
    .enter   = browse_enter,
    .leave   = browse_leave,
    .draw    = browse_draw,
    .input   = browse_input,
    .timeout = NULL,
    .tick    = browse_tick,
};

/* -- U1 shared helpers (screens.h) ----------------------------------------- */

void *ui_screen_alloc(struct ui_screen *s, size_t size)
{
    void *p = ui_arena_alloc(size);
    int h;

    if (p || audio_status())
    {
        s->priv = p;
        return p;
    }
    /* the arena is full ("arena full" traced): audio is stopped, so a
     * block can come off the audio buffer; checked just above, no yield
     * before the allocation. Pinned: the state's pointers stay valid. */
    h = core_alloc(size);
    if (h <= 0)
        return NULL;
    core_pin(h);
    s->handle = h;
    s->priv = core_get_data(h);
    memset(s->priv, 0, size);
    logf("ipodui: %s: state off the arena (%u B)", s->ops->name,
         (unsigned)size);
    return s->priv;
}

void ui_screen_free(struct ui_screen *s)
{
    if (s->handle > 0)
    {
        core_unpin(s->handle);
        s->handle = core_free(s->handle);
        audio_buffer_regrow();      /* the next playback sizes it anew */
    }
    s->priv = NULL;
}

void ui_row_text(const char *text, int x, int h, unsigned fg)
{
    const struct ui_theme *t = ui_theme;
    const struct font *pf = font_get(t->font_ui);
    int base = t->row_baseline + (h - 25) / 2;
    int y = t->font_ui == FONT_UI ? (h - (int)pf->height) / 2
                                  : base - (int)pf->ascent;

    lcd_setfont(t->font_ui);
    lcd_set_foreground(fg);
    lcd_set_drawmode(DRMODE_FG);
    lcd_putsxy(x, y, text);
    lcd_set_drawmode(DRMODE_SOLID);
}
