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
/* The music library: sorted views over the tagcache RAM cache. See
 * library.h for what each category lists and how tracks are classified.
 *
 * Building a view (rows_build(), shared by lib_open() and the precompute):
 *  1. scan the master index once (yielding every 4,096 entries) and fill
 *     a temporary block of 12 B records {k1, k2, master index} with the
 *     entries that pass the class and parent filters; group views keep
 *     only identities (tag seeks) not in a 4 KB table of recent ones;
 *  2. group views: sort by group identity and keep one record per group,
 *     the representative (its smallest master index);
 *  3. replace k1/k2 by the first 8 bytes of the row's ui_groupkey() (or
 *     disc/track numbers, or the file time) and sort; equal prefixes fall
 *     back to full keys, computed only for the tie. Title-ordered track
 *     views (Songs, a genre's or composer's songs: the big ones) skip
 *     this: they walk the title tag file, which tagcache stores in
 *     ui_groupkey() order (sorted at commit, in the background);
 *  4. group views: mark neighbours that are the same group in another
 *     letter case (several tag strings, one row);
 *  5. compact the representatives into the view's rows (4 B/row) and
 *     trim the view: the records live in the view's own space;
 *  6. book and show lists: every row's listened fraction (and, for
 *     shows, whether an episode is unplayed), in two passes over the
 *     index for all rows (progress_compute()).
 * Sorting is a heap sort that yields (sort_recs()): it takes a context
 * (qsort() has none, and the precompute sorts on another thread).
 *
 * Precompute (docs/phase4b-brief.md, Decisions 3-6, Revision 2 D3):
 * after every successful RAM cache load and after a deleted-file pass,
 * the tagcache thread runs lib_precompute(), which runs steps 1-4 for
 * every unfiltered category, counts each group's tracks, computes the
 * art key of every album/book/show row and writes it all to
 * database_lib.dat (format: libfile.h), stamped with the database's
 * commit id, entry count and SORTKEY_VERSION and checksummed. lib_open()
 * of an unfiltered category then reads its rows (and counts, keys) from
 * that file instead of scanning; a missing, old or torn file falls back
 * to the scan (a torn one also asks for a rebuild). The precompute runs
 * unboosted, in a 20 B/entry work block reserved at shell start for the
 * database on disk + 25 % (lib_reserve(), H1), so also while audio
 * plays; without one (no database at boot, or the library outgrew it) it
 * takes a transient block, only while audio is stopped (it would come out
 * of the audio buffer), until the next boot. It yields every 4,096 entries or
 * 2,048 comparisons, and gives up at the first queued tagcache event
 * (USB, a commit, a power-off): it is redone from the start later. When
 * the new file is complete, the library generation changes and open
 * lists reopen from it.
 *
 * Memory: all views live in one movable pool (core_alloc), sized for the
 * database in RAM (12 B/entry + 64 KB) when a library call first needs it
 * after a (re)load. Views are stacked in it and compacted on close. One
 * block, because the audio buffer takes all free memory once playback
 * starts and every later core_alloc makes playback stop, shrink its
 * buffer and restart (an audible gap: measured 340 ms in the sim); the
 * pool is (re)allocated by lib_play/lib_shuffle_all before they replace
 * playback anyway, or by lib_open after a database change. Builds that
 * do not fit grow the pool (copy). The playlist scratch (search + path)
 * is stacked on top while used, with the pool pinned; file reads go
 * straight into the pool, pinned while read() runs.
 * Nothing yields while RAM cache pointers are held; after each yield
 * the generation is checked and handles are dereferenced again. Numeric
 * tags are read straight from the RAM index (never blocks); only the
 * runtime statistics (stat_of()) go through tagcache_ui_numeric(), which
 * may block on the update queue's mutex, and never inside a sort.
 *
 * Measured: see "Measured" at the end of this file. */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "core_alloc.h"
#include "tagcache.h"
#include "playlist.h"
#include "audio.h"
#include "playback.h"
#include "settings.h"
#include "logf.h"
#include "debug.h"
#include "ipodui/maint.h"
#include "ipodui/anim.h"
#include "ipodui/sortkey.h"
#include "ipodui/libfile.h"
#include "ipodui/artkey.h"
#include "ipodui/artcache.h"
#include "ipodui/library.h"
#include "ipodui/lib_shuffle.h"
#include "ipodui/align_check.h"
#ifdef HAVE_DEVSTATS
#include "devstats.h"     /* #47: the lib open trace's cmds= */
#endif
#ifdef HAVE_BCMFW_VIDEO
#include "ipodui/vixpass.h"
#endif
/* #109: queues by database index (dbqueue.c); the host tests (tests/
 * ipodui, tests/search, tests/otg: ARTFMT_HOST) keep the path queue,
 * which is also this build's fallback when the slot table finds no
 * memory */
#ifndef ARTFMT_HOST
#define LIB_DBQ
#include "ipodui/dbqueue.h"
/* the playlist size setting (S2: the queue's capacity now grows) */
#define LIB_QUEUE_SETTING   (global_settings.max_files_in_playlist)
#define LIB_QUEUE_SPARE     256     /* words past a long list (1 KB) */
/* bytes a queue of n rows allocates: the slot table, and indices[] past
 * the setting's size */
static size_t lib_queue_need(int n)
{
    size_t words = n > LIB_QUEUE_SETTING ? (size_t)n + LIB_QUEUE_SPARE : 0;
    return dbq_need(n) + words * sizeof(unsigned long);
}
#else
#define LIB_QUEUE_SETTING   (playlist_get_current()->max_playlist_size)
#endif

/* P6 books (books.dat, books.pak, the page): not in the library host
 * test (tests/ipodui, ARTFMT_HOST), whose stubs predate them; there a
 * book plays as in Phase 5 */
#ifndef ARTFMT_HOST
#define LIB_BOOKS
#include "metadata.h"
#include "metadata/chapters.h"
#include "ipodui/ab_book.h"
#include "ipodui/ab_store.h"
#include <ctype.h>
/* owed at boot and after every library load (lib_committed(), the
 * tagcache thread): books.dat's seed retries on that event, not on a
 * timer (M2) */
static volatile bool seed_owed = true;
#endif

/* #67: the last-played book pinned at the top of Audiobooks. The library
 * host test (tests/ipodui) builds it with LIB_PIN_HOST and its own
 * ab_store_pin_key(). */
#if defined(LIB_BOOKS) || defined(LIB_PIN_HOST)
#define LIB_PIN
#ifndef LIB_BOOKS
#include "ipodui/ab_store.h"
#endif
#endif

#if !defined(HAVE_TAGCACHE) || !defined(HAVE_TC_RAMCACHE)
#error "the iPod UI library needs the tagcache RAM cache"
#endif

/* P11 (Search, On-The-Go views, library stats): everywhere but the
 * Phase 4 library host test (tests/ipodui), which links no search_core.c
 * and whose stub tagcache has no tag_bitrate; tests/search and tests/otg
 * build it with LIB_P11_HOST. */
#if !defined(ARTFMT_HOST) || defined(LIB_P11_HOST)
#define LIB_P11
#include "ipodui/search.h"
#include "ipodui/otg.h"
#endif
#if defined(HAVE_BCMFW_VIDEO) && !defined(LIB_P11)
#error "the Phase 14a video lists need the P11 stats (LIB_P11)"
#endif

#define MAX_VIEWS   16          /* the deepest browse stack is ~5 */
#define KEY_LEN     128         /* full sort keys, for ties only */
#define YIELD_EVERY 4096        /* rows between yields while building */
#define SORT_YIELD  2048        /* comparisons between yields (sorting) */
#define LIB_SUB_LEN 48          /* second-line cache: longer ones refetch */

/* Phase 14a (docs/phase14a-brief.md 3.2a): a video is never music */
#define CLASS_MASK  (TAGCACHE_UI_AUDIOBOOK | TAGCACHE_UI_PODCAST \
                     | TAGCACHE_UI_ANY_VIDEO)
#ifdef HAVE_BCMFW_VIDEO
#define VKIND_MASK  (TAGCACHE_UI_VIDEO | TAGCACHE_UI_TV | TAGCACHE_UI_PODCAST)
#endif

enum group { G_NONE, G_ARTIST, G_ALBUM, G_GENRE, G_COMPOSER
#ifdef HAVE_BCMFW_VIDEO
             , G_SEASON         /* LIB_TVSEASONS: rows hold season numbers */
#endif
};
enum order { O_TITLE, O_TRACK, O_ARTIST_ALL, O_NEWEST,
             O_BOOK_AUTHOR };   /* #59, precompute only: see cmp_tie() */
enum cls   { C_ANY, C_MUSIC, C_MUSIC_SOLO, C_COMPILATION, C_BOOK, C_PODCAST
#ifdef HAVE_BCMFW_VIDEO
             , C_MOVIE          /* VIDEO, neither TV nor PODCAST */
             , C_TV             /* VIDEO | TV */
#endif
};

/* What a view keeps besides its rows (view_layout()) */
#define X_PROG  0x01            /* int16 listened fraction per row */
#define X_UNPL  0x02            /* uint8 "has an unplayed episode" per row */
#define X_CNT   0x04            /* uint32 tracks per row (-1: unknown) */
#define X_KEY   0x08            /* 2 x uint32 art key per row (0: none) */
#define X_SUB   0x10            /* second-line label cache */

enum sub { SUB_NONE, SUB_ALBUM_ARTIST, SUB_TRACK_ARTIST, SUB_SHOW
#ifdef HAVE_BCMFW_VIDEO
           , SUB_DURATION       /* a movie's or episode's length */
#endif
};

struct view {
    int8_t  cat, group, order, cls;
    int8_t  sub;                /* lib_sublabel(): SUB_* */
    int8_t  by_author;          /* #59: books in author order (lib_letter) */
    int8_t  pinned;             /* #67: row 0 is the pinned book */
    int     generation;
    int     count;
    int     live_row;           /* group row whose progress came from the
                                 * playing track, or -1 */
    uint32_t off_prog, off_unpl, off_cnt, off_key, off_sub;
                                /* extras, bytes from the view; 0: none */
    uint32_t off_names;         /* LIB_OTG: names of unresolved rows */
    int32_t cache_row[LIB_CACHE_ROWS];      /* row in each slot, or -1 */
    char    cache[LIB_CACHE_ROWS][LIB_CACHE_LEN];
    int32_t rows[];             /* master index per row; the extras follow */
};

struct subcache {               /* X_SUB: like the label cache */
    int32_t row[LIB_CACHE_ROWS];
    char    s[LIB_CACHE_ROWS][LIB_SUB_LEN];
};

struct rec {
    uint32_t k1, k2;
    int32_t  idx;
};
/* tagcache.c reserves this much per entry for the pool (pool_sync()) */
typedef char lib_rec_size_check[
    sizeof(struct rec) <= TAGCACHE_UI_VIEW_BYTES ? 1 : -1];
/* Sections a precompute writes (libfile.h): artists 2, albums 5, songs,
 * genres, composers 1 each, compilations 4, audiobooks 5, podcasts 4,
 * album table 1, panel set 2, stats 1, books by author 3 (#59). A file
 * with more than LIBFILE_MAX_SEC is never written (pre_sec_begin()).
 * Phase 14a video builds: movies 2, TV shows 5, video stats 1 more.
 * #47: the artists' albums 1 more. */
#ifdef HAVE_BCMFW_VIDEO
#define LIB_PRE_SECTIONS 39
#else
#define LIB_PRE_SECTIONS 31
#endif
/* #47: one version for the lists and the tag files' order */
typedef char lib_grouping_check[
    LIBFILE_GROUPING == UI_GROUPING_VERSION ? 1 : -1];
typedef char lib_sections_check[
    LIB_PRE_SECTIONS <= LIBFILE_MAX_SEC ? 1 : -1];
typedef char lib_hdr_size_check[
    sizeof(struct libfile_hdr) == LIBFILE_HDR_BYTES
    && sizeof(struct libfile_album) == 24
    ? 1 : -1];

/* A filter resolved against the RAM cache: representatives' seeks. Valid
 * only until the next yield. */
struct match {
    int cls;
#ifdef HAVE_BCMFW_VIDEO
    int season;                 /* LIB_TVEPISODES: a season, or LIB_ANY */
#endif
    int32_t rep[4];             /* artist, album, genre, composer; -1 any */
    const int32_t *seeks[4];
    int32_t flag[4];
};

/* One build (a view on the UI thread, or a category of the precompute on
 * the tagcache thread): where its records are, what it sorts by, and how
 * it yields. Nothing in it is shared between the two threads. */
struct bctx {
    int gen;                    /* database generation it reads */
    int group, order;           /* the running sort; order < 0: identity */
    int h;                      /* view handle (pool) or core_alloc handle */
    bool pool;                  /* h is a view in the pool */
    size_t recs_off, seen_off;  /* from the block's start */
    size_t aux_off;             /* O_BOOK_AUTHOR: the books' reps (int32) */
    struct {                    /* artist_tag()'s memo */
        int     generation;
        int32_t seek;
        bool    tagged;
    } memo;
    bool (*stop)(void);         /* precompute: the tagcache thread's check */
    int yields;
};

/* Open views: handle = index + 1. A view is 'size' bytes at 'off' in the
 * pool; off < 0 when it has no memory (opened before the database was
 * ready, or the pool was reset for a new database since). */
static struct slot {
    int off, size;              /* size 0: free */
    int count;                  /* stays when the view goes stale */
    int generation;
} slots[MAX_VIEWS];
static struct {
    int handle;
    int size, used;
    int generation;             /* database it was sized for */
    bool owed;                  /* fb-stall: the size it should have
                                   waits for a stop (a reload while
                                   playing kept the block) */
} pool;
#define POOL_GEN_NONE INT_MIN   /* reserved at shell start, not synced */
#define POOL_SPARE  (64 * 1024)
/* Worker-owned reservations, adopted only by the UI at pool_sync(). The
 * active pool is never resized or freed underneath a yielding UI call. */
static struct future_pool {
    struct tagcache_context *ctx;
    int handle, generation;
    size_t size;
} future_pools[2];

void lib_generation_pool_abandon(struct tagcache_context *ctx)
{
    for (unsigned i = 0; i < ARRAYLEN(future_pools); i++)
    {
        struct future_pool *p = &future_pools[i];
        if (p->ctx == ctx && ctx != tagcache_context_active())
        {
            int handle = p->handle;
            memset(p, 0, sizeof(*p));
            if (handle > 0)
                core_free(handle);
        }
    }
}

bool lib_generation_pool_prepare(struct tagcache_context *ctx)
{
    int n = tagcache_ui_count_ctx(ctx);
    if (n < 0)
        return false;
    size_t want = MAX(POOL_SPARE + (size_t)n * sizeof(struct rec),
                      (size_t)MAX(pool.size, 0));
    struct future_pool *empty = NULL;
    for (unsigned i = 0; i < ARRAYLEN(future_pools); i++)
    {
        struct future_pool *p = &future_pools[i];
        if (p->ctx == ctx && p->handle > 0)
        {
            if (p->generation == tagcache_ui_generation_ctx(ctx)
                && p->size >= want)
                return true;
            lib_generation_pool_abandon(ctx);
        }
        if (!p->handle)
            empty = p;
    }
    /* audio on: free memory only, or a paused window's (#88) */
    if (!empty || (audio_status() && core_allocatable() < want + 64
                   && !maint_take(MAINT_COMMIT, want + 64)))
        return false;
    int handle = core_alloc(want);
    if (handle <= 0)
        return false;
    empty->ctx = ctx;
    empty->generation = tagcache_ui_generation_ctx(ctx);
    empty->size = want;
    empty->handle = handle; /* publish ticket last; worker never frees active */
    return true;
}
/* Growth is refused while audio plays (P6 Rev 1 item 2), so the spare
 * must hold what a user stacks up then. Measured (fix round M4,
 * tests/perf/lib50k.sh 50000 playing; "lib pool peak"): playing from
 * Songs, then Albums (4,882 rows), Artists > artist > album, Songs, a
 * book page, Songs: peak 618,568 of 665,536 B, no refusal; 46,968 B
 * left covers a book page's blob (AB_BLOB_MAX_SIZE 43,048). Past that a
 * list opens empty, as with no database, and "lib alloc refused" is
 * logged; no splash. */
static int fetches;             /* label cache misses since the last frame */
static int yields;              /* trace: yields of the running lib_open() */

/* The precompute file, as the UI thread last found it. 'files' is bumped
 * by the tagcache thread when it has written a new file (lib_generation()
 * changes with it, so open lists reopen from it). */
static volatile int pre_files;
/* #59: the books list's order (lib_set_book_order(), UI thread only) and
 * how often it changed (part of lib_generation()) */
static int8_t book_order = LIB_BOOKS_BY_TITLE;
static int book_order_gen;
static struct {
    int tc_gen, files;          /* what 'state' was found for */
    int8_t state;               /* 1 usable, -1 not (no file, old, torn) */
    int8_t tab_ok;              /* album table checksum: 1 ok, -1 bad */
} pre_ui;

static const char * const cat_names[] UNUSED_ATTR = {
    "artists", "albums", "songs", "genres", "composers", "compilations",
    "audiobooks", "podcasts", "search", "otg",
#ifdef HAVE_BCMFW_VIDEO
    "movies", "tvshows", "tvseasons", "tvepisodes",
#endif
};

/* Microseconds for the trace (differences only).
 * now_us(): elapsed time: the PP5022's microsecond timer on the device,
 * the host's performance counter in the simulator.
 * cpu_us(): time this thread computed: the same on the device (nothing
 * else runs while lib_open() computes, except in its yields); in the
 * simulator the thread's CPU time, because elapsed host time varied 3x
 * between runs (other threads, host scheduling).
 * open_us(): lib open's figure: on the device the same; in the simulator
 * the thread's USER time only. check_trace.py scales the open's us= by
 * SIM_FACTOR (400) as an estimate of the device's computing, and a
 * file-backed open's host system-call time (its cmds= reads, counted
 * apart) is no such estimate: it was 90% of us= for the artists list
 * (n=49, src=file: user 8 us of 109) and swung the estimate 31..597 ms
 * with the host's load (p19-gatefix4).
 * The host clocks are in simclock.c: the host's <time.h> must not meet
 * the apps headers. */
#ifdef SIMULATOR
#include "ipodui/simclock.h"
#endif

static unsigned long now_us(void)
{
#if defined(SIMULATOR)
    return sim_clock_now_us();
#elif defined(USEC_TIMER)
    return USEC_TIMER;
#else
    return current_tick * (1000000 / HZ);
#endif
}

static unsigned long cpu_us(void)
{
#if defined(SIMULATOR)
    return sim_clock_cpu_us();
#else
    return now_us();
#endif
}

static unsigned long open_us(void)
{
#if defined(SIMULATOR)
    return sim_clock_cpu_user_us();
#else
    return now_us();
#endif
}

/* -- tag access (RAM cache; never yields) -------------------------------- */

static const char *str_of(struct tagcache_context *ctx, const int32_t *s, int tag)
{
    return tagcache_ui_str_ctx(ctx, tag, s[tag]);
}

/* The album artist, or the track artist when there is none. */
static const char *eff_artist(struct tagcache_context *ctx, const int32_t *s, int *tag)
{
    const char *a = str_of(ctx, s, tag_albumartist);

    *tag = tag_albumartist;
    if (a)
        return a;
    *tag = tag_artist;
    return str_of(ctx, s, tag_artist);
}

/* One group's name (#47: ui_casefold_cmp(), sortkey.h: case folded for
 * Latin, Greek and Cyrillic, accent-sensitive; ASCII-only strcasecmp()
 * before). */
static bool str_eq(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;              /* both untagged */
    return a == b || !ui_casefold_cmp(a, b);
}

static bool same_tag(struct tagcache_context *ctx, const int32_t *a, const int32_t *b, int tag)
{
    return a[tag] == b[tag] || str_eq(str_of(ctx, a, tag), str_of(ctx, b, tag));
}

static bool same_artist(struct tagcache_context *ctx, const int32_t *a, const int32_t *b)
{
    int ta, tb;
    const char *x = eff_artist(ctx, a, &ta), *y = eff_artist(ctx, b, &tb);

    if (ta == tb && a[ta] == b[tb])
        return true;
    return str_eq(x, y);
}

/* A book or show file without an album tag is a group of its own. */
static bool own_group(struct tagcache_context *ctx, const int32_t *s, int32_t flag)
{
#ifdef HAVE_BCMFW_VIDEO
    /* a TV episode without a show joins the one "Unknown Show" row
     * (stored empty, M7: the screen shows the language string) */
    if (flag & TAGCACHE_UI_TV)
        return false;
#endif
    return (flag & CLASS_MASK) && !str_of(ctx, s, tag_album);
}

#ifdef HAVE_BCMFW_VIDEO
/* A TV show groups by its show (the album tag) alone, whoever the
 * episodes' artists are (Phase 14a 3.2, D3) */
static bool is_tv(int32_t flag)
{
    return (flag & VKIND_MASK) == (TAGCACHE_UI_VIDEO | TAGCACHE_UI_TV);
}
#endif

static bool same_album(struct tagcache_context *ctx, const int32_t *a, int32_t aflag, int aidx,
                       const int32_t *b, int32_t bflag, int bidx)
{
    if (own_group(ctx, a, aflag) || own_group(ctx, b, bflag))
        return aidx == bidx;
#ifdef HAVE_BCMFW_VIDEO
    if (is_tv(aflag) || is_tv(bflag))
        return is_tv(aflag) && is_tv(bflag) && same_tag(ctx, a, b, tag_album);
#endif
    return same_tag(ctx, a, b, tag_album) && same_artist(ctx, a, b);
}

static bool class_ok(int cls, int32_t flag)
{
    bool music = !(flag & CLASS_MASK);

    switch (cls)
    {
    case C_MUSIC:       return music;
    case C_MUSIC_SOLO:  return music && !(flag & TAGCACHE_UI_COMPILATION);
    case C_COMPILATION: return music && (flag & TAGCACHE_UI_COMPILATION);
#ifdef HAVE_BCMFW_VIDEO
    case C_BOOK:        return (flag & TAGCACHE_UI_AUDIOBOOK)
                            && !(flag & TAGCACHE_UI_VIDEO);
    case C_MOVIE:       return (flag & VKIND_MASK) == TAGCACHE_UI_VIDEO;
    case C_TV:          return is_tv(flag);
#else
    case C_BOOK:        return flag & TAGCACHE_UI_AUDIOBOOK;
#endif
    case C_PODCAST:     return flag & TAGCACHE_UI_PODCAST;
    default:            return true;
    }
}

static bool unfiltered(const struct lib_filter *f)
{
    return !f || (f->artist == LIB_ANY && f->album == LIB_ANY
                  && f->genre == LIB_ANY && f->composer == LIB_ANY
#ifdef HAVE_BCMFW_VIDEO
                  && f->season == LIB_ANY
#endif
                  );
}

#ifdef HAVE_BCMFW_VIDEO
/* An episode's season: the stored discnumber (vclass: else 1) */
static int season_of(const int32_t *s)
{
    return s[tag_discnumber] > 0 ? (int)s[tag_discnumber] : 1;
}
#endif

/* Resolves the filter; false if a representative is gone (no rows). */
static bool match_init(struct tagcache_context *ctx, struct match *m, int cls, const struct lib_filter *f)
{
    int i;

    m->cls = cls;
#ifdef HAVE_BCMFW_VIDEO
    m->season = f ? f->season : LIB_ANY;
#endif
    m->rep[0] = f ? f->artist : LIB_ANY;
    m->rep[1] = f ? f->album : LIB_ANY;
    m->rep[2] = f ? f->genre : LIB_ANY;
    m->rep[3] = f ? f->composer : LIB_ANY;
    for (i = 0; i < 4; i++)
    {
        m->seeks[i] = NULL;
        if (m->rep[i] == LIB_ANY)
            continue;
        m->seeks[i] = tagcache_ui_seeks_ctx(ctx, m->rep[i], &m->flag[i]);
        if (!m->seeks[i])
            return false;
    }
    return true;
}

static bool match_entry(struct tagcache_context *ctx, const struct match *m, int idx, const int32_t *s,
                        int32_t flag)
{
    if (!class_ok(m->cls, flag))
        return false;
#ifdef HAVE_BCMFW_VIDEO
    if (m->season != LIB_ANY && season_of(s) != m->season)
        return false;
#endif
    if (m->seeks[1] && !same_album(ctx, s, flag, idx,
                                   m->seeks[1], m->flag[1], m->rep[1]))
        return false;
    if (m->seeks[0] && !same_artist(ctx, s, m->seeks[0]))
        return false;
    if (m->seeks[2] && !same_tag(ctx, s, m->seeks[2], tag_genre))
        return false;
    if (m->seeks[3] && !same_tag(ctx, s, m->seeks[3], tag_composer))
        return false;
    return true;
}

/* The sort keys (and lib_label() text) of an untagged artist / album row;
 * the screens show the language strings for them (library.h). */
const char lib_unknown_artist[] = "Unknown Artist";
const char lib_unknown_album[] = "Unknown Album";

/* The label of a row: what the list shows and what it sorts by. */
static const char *label_of(struct tagcache_context *ctx, int group, int idx)
{
    int32_t flag;
    const int32_t *s = tagcache_ui_seeks_ctx(ctx, idx, &flag);
    const char *p;
    int t;

    if (!s)
        return NULL;
    switch (group)
    {
    case G_ARTIST:
        p = eff_artist(ctx, s, &t);
        return p ? p : lib_unknown_artist;
    case G_ALBUM:
        p = str_of(ctx, s, tag_album);
        if (p)
            return p;
        if (own_group(ctx, s, flag))
            return str_of(ctx, s, tag_title);
        return lib_unknown_album;
    case G_GENRE:
        return str_of(ctx, s, tag_genre);
    case G_COMPOSER:
        return str_of(ctx, s, tag_composer);
    default:
        return str_of(ctx, s, tag_title);
    }
}

/* -- sorting ------------------------------------------------------------ */

/* First 8 bytes of the sort key, big-endian, so that unsigned compares
 * order like strcmp on the full keys (up to ties). */
static void key_prefix(const char *label, uint32_t *k1, uint32_t *k2)
{
    unsigned char key[16];

    memset(key, 0, sizeof(key));
    /* 15 bytes always hold the first 8 bytes of the full key (truncation
     * drops at most a partial 4-byte character) */
    ui_groupkey(label, (char *)key, sizeof(key));
    *k1 = (uint32_t)key[0] << 24 | key[1] << 16 | key[2] << 8 | key[3];
    *k2 = (uint32_t)key[4] << 24 | key[5] << 16 | key[6] << 8 | key[7];
}

/* Group order: the group key, then the case-folded name, so the names
 * str_eq() puts in one group are next to each other (#47). 0 for two
 * names of one group. */
static int cmp_groups(const char *a, const char *b)
{
    char ka[KEY_LEN], kb[KEY_LEN];
    int r;

    ui_groupkey(a, ka, sizeof(ka));
    ui_groupkey(b, kb, sizeof(kb));
    r = strcmp(ka, kb);
    if (r)
        return r;
    if (!a || !b)
        return (a != NULL) - (b != NULL);
    return ui_casefold_cmp(a, b);
}

static int cmp_labels(const char *a, const char *b)
{
    int r = cmp_groups(a, b);

    return r || !a || !b ? r : strcmp(a, b);
}

static int cmp_num(long a, long b)
{
    return (a > b) - (a < b);
}

/* Disc and track number of entry 'idx', straight from the RAM index:
 * never blocks, so it is safe in a sort. (tagcache_ui_numeric() checks
 * the update queue under its mutex, which can block and yield; the queue
 * only ever holds runtime statistics, see stat_of(), never these tags.) */
static void track_no(struct tagcache_context *ctx, int idx, long *disc, long *track)
{
    const int32_t *s = tagcache_ui_seeks_ctx(ctx, idx, NULL);

    *disc = s ? s[tag_discnumber] : -1;
    *track = s ? s[tag_tracknumber] : -1;
}

/* A runtime statistic (tag_lastelapsed, tag_lastoffset, tag_playcount):
 * its newest value may still sit in tagcache's update queue, read under
 * the queue's mutex, so this can block and yield. Callers hold no RAM
 * cache or pool pointer across it and check the generation afterwards. */
static long stat_of(int idx, int tag)
{
    return tagcache_ui_numeric(idx, tag);
}

/* disc, then track; missing numbers last */
static int cmp_track_no(struct tagcache_context *ctx, int a, int b)
{
    long da, db, ta, tb;
    int r;

    track_no(ctx, a, &da, &ta);
    track_no(ctx, b, &db, &tb);
    r = cmp_num(da, db);
    if (r)
        return r;
    if (ta <= 0)
        ta = LONG_MAX;
    if (tb <= 0)
        tb = LONG_MAX;
    return cmp_num(ta, tb);
}

static char *bx_base(const struct bctx *c);

/* Everything after equal prefixes. */
static int cmp_tie(struct tagcache_context *ctx, const struct bctx *c, int a, int b)
{
    const int32_t *sa, *sb;
    int r, t;

    if (c->group != G_NONE)
    {
        if (c->group != G_ALBUM)
            return cmp_labels(label_of(ctx, c->group, a),
                              label_of(ctx, c->group, b));
        /* same album (any case): by album artist, then the album's case,
         * so one artist's case variants of an album stay next to each
         * other and form one row, as in a filtered list (#47) */
        r = cmp_groups(label_of(ctx, G_ALBUM, a), label_of(ctx, G_ALBUM, b));
        if (r)
            return r;
        sa = tagcache_ui_seeks_ctx(ctx, a, NULL);
        sb = tagcache_ui_seeks_ctx(ctx, b, NULL);
        if (sa && sb)
        {
            r = cmp_labels(eff_artist(ctx, sa, &t), eff_artist(ctx, sb, &t));
            if (r)
                return r;
        }
        return cmp_labels(label_of(ctx, G_ALBUM, a), label_of(ctx, G_ALBUM, b));
    }

    switch (c->order)
    {
    case O_BOOK_AUTHOR:
        /* #59: a and b are title rows of the books, whose reps are at
         * aux_off; equal authors fall to the title row (cmp_rec(ctx, )). One
         * author's books usually share a tag seek: equal without
         * building two full keys */
        {
            const int32_t *rp = (const int32_t *)(bx_base(c) + c->aux_off);
            const int32_t *xa = tagcache_ui_seeks_ctx(ctx, rp[a], NULL);
            const int32_t *xb = tagcache_ui_seeks_ctx(ctx, rp[b], NULL);
            int ta, tb;

            if (xa && xb)
            {
                eff_artist(ctx, xa, &ta);
                eff_artist(ctx, xb, &tb);
                if (ta == tb && xa[ta] == xb[tb])
                    return 0;
            }
            return cmp_labels(label_of(ctx, G_ARTIST, rp[a]),
                              label_of(ctx, G_ARTIST, rp[b]));
        }
    case O_ARTIST_ALL:
        r = cmp_labels(label_of(ctx, G_ALBUM, a), label_of(ctx, G_ALBUM, b));
        if (r)
            return r;
        r = cmp_track_no(ctx, a, b);
        if (r)
            return r;
        break;
    case O_TRACK:
        r = cmp_track_no(ctx, a, b);
        if (r)
            return r;
        break;
    default:
        break;
    }
    return cmp_labels(label_of(ctx, G_NONE, a), label_of(ctx, G_NONE, b));
}

static int cmp_rec(struct tagcache_context *ctx, const struct bctx *c, const struct rec *a,
                   const struct rec *b)
{
    int r;

    if (a->k1 != b->k1)
        return a->k1 < b->k1 ? -1 : 1;
    if (a->k2 != b->k2)
        return a->k2 < b->k2 ? -1 : 1;
    if (c->order >= 0)
    {
        r = cmp_tie(ctx, c, a->idx, b->idx);
        if (r)
            return r;
    }
    return cmp_num(a->idx, b->idx);
}

/* Yields; false if the database changed meanwhile, or (precompute) the
 * tagcache thread has an event to handle. */
static bool bx_yield(struct tagcache_context *ctx, struct bctx *c)
{
    yield();
    c->yields++;
    return (!c->pool || ctx == tagcache_context_active())
        && tagcache_ui_generation_ctx(ctx) == c->gen && tagcache_ui_count_ctx(ctx) >= 0
        && !(c->stop && c->stop());
}

static struct rec *recs_at(const struct bctx *c, size_t off)
{
    return (struct rec *)(bx_base(c) + off);
}

/* Heap sort of n records at 'off' in c's block, by cmp_rec(); yields
 * every SORT_YIELD comparisons (the block may move then). The order is
 * total (the master index breaks ties), so the result is the same as any
 * other sort's. False if bx_yield() said stop. */
static bool sift(struct tagcache_context *ctx, struct bctx *c, size_t off, int root, int end, int *steps)
{
    for (;;)
    {
        struct rec *a, t;
        int child = 2 * root + 1;

        if (child >= end)
            return true;
        if (++*steps >= SORT_YIELD)
        {
            *steps = 0;
            if (!bx_yield(ctx, c))
                return false;
        }
        a = recs_at(c, off);
        if (child + 1 < end && cmp_rec(ctx, c, &a[child], &a[child + 1]) < 0)
            child++;
        if (cmp_rec(ctx, c, &a[root], &a[child]) >= 0)
            return true;
        t = a[root];
        a[root] = a[child];
        a[child] = t;
        root = child;
    }
}

static bool sort_recs(struct tagcache_context *ctx, struct bctx *c, size_t off, int n)
{
    int i, steps = 0;

    for (i = n / 2 - 1; i >= 0; i--)
        if (!sift(ctx, c, off, i, n, &steps))
            return false;
    for (i = n - 1; i > 0; i--)
    {
        struct rec *a = recs_at(c, off), t = a[0];

        a[0] = a[i];
        a[i] = t;
        if (!sift(ctx, c, off, 0, i, &steps))
            return false;
    }
    return true;
}

/* Sort prefix of a row (step 3). */
static void sort_prefix(struct tagcache_context *ctx, struct rec *r, int group, int order)
{
    const int32_t *s;
    long d, n;

    if (group != G_NONE)
    {
        key_prefix(label_of(ctx, group, r->idx), &r->k1, &r->k2);
        return;
    }
    switch (order)
    {
    case O_TRACK:
        track_no(ctx, r->idx, &d, &n);
        r->k1 = d;
        r->k2 = n > 0 ? (uint32_t)n : 0xffffffffu;
        break;
    case O_ARTIST_ALL:
        key_prefix(label_of(ctx, G_ALBUM, r->idx), &r->k1, &r->k2);
        break;
    case O_NEWEST:
        s = tagcache_ui_seeks_ctx(ctx, r->idx, NULL);  /* never queued: no block */
        r->k1 = ~(uint32_t)(s ? s[tag_mtime] : -1);
        r->k2 = 0;
        break;
    default:
        key_prefix(label_of(ctx, G_NONE, r->idx), &r->k1, &r->k2);
        break;
    }
}

/* Group identity from seeks, for de-duplication (step 2). */
/* The tag eff_artist() would use, without reading the string for every
 * track: the master index is in folder order, so the album artist seek
 * repeats from one track to the next and the last answer is remembered
 * (per build: each thread has its own). */
static int artist_tag(struct tagcache_context *ctx, struct bctx *c, const int32_t *s)
{
    int gen = tagcache_ui_generation_ctx(ctx);

    if (c->memo.generation != gen || c->memo.seek != s[tag_albumartist])
    {
        c->memo.generation = gen;
        c->memo.seek = s[tag_albumartist];
        c->memo.tagged = str_of(ctx, s, tag_albumartist) != NULL;
    }
    return c->memo.tagged ? tag_albumartist : tag_artist;
}

static void group_ident(struct tagcache_context *ctx, struct bctx *c, struct rec *r, int group,
                        const int32_t *s, int32_t flag)
{
    int t;

    switch (group)
    {
    case G_ARTIST:
        t = artist_tag(ctx, c, s);
        r->k1 = s[t];
        r->k2 = t;
        break;
    case G_ALBUM:
        if (own_group(ctx, s, flag))
        {
            r->k1 = 0xffffffffu;
            r->k2 = r->idx;
            break;
        }
#ifdef HAVE_BCMFW_VIDEO
        if (is_tv(flag))
        {
            r->k1 = s[tag_album];       /* the show alone (same_album(ctx, )) */
            r->k2 = 0x7fffffffu;        /* no artist seek or flagged one */
            break;
        }
#endif
        t = artist_tag(ctx, c, s);
        r->k1 = s[tag_album];
        r->k2 = s[t] | (t == tag_albumartist ? 0x80000000u : 0);
        break;
    case G_GENRE:
        r->k1 = s[tag_genre];
        r->k2 = 0;
        break;
    case G_COMPOSER:
        r->k1 = s[tag_composer];
        r->k2 = 0;
        break;
    }
}

static bool same_group(struct tagcache_context *ctx, int group, int a, int b)
{
    int32_t fa, fb;
    const int32_t *sa = tagcache_ui_seeks_ctx(ctx, a, &fa);
    const int32_t *sb = tagcache_ui_seeks_ctx(ctx, b, &fb);

    if (!sa || !sb)
        return false;
    switch (group)
    {
    case G_ARTIST:   return same_artist(ctx, sa, sb);
    case G_ALBUM:    return same_album(ctx, sa, fa, a, sb, fb, b);
    case G_GENRE:    return same_tag(ctx, sa, sb, tag_genre);
    case G_COMPOSER: return same_tag(ctx, sa, sb, tag_composer);
    default:         return false;
    }
}

/* -- views -------------------------------------------------------------- */

static char *pool_base(void)
{
    return pool.handle > 0 ? core_get_data(pool.handle) : NULL;
}

/* Sizes the pool for the database in RAM, once per (re)load. Views from
 * an older database are stale already; they lose their memory.
 *
 * fb-stall (owner device report: a FLAC halted for seconds on entering
 * Audiobooks): it used to free and re-allocate the block at the first
 * lib_open() after any reload, playing or not; while audio plays that
 * allocation makes playback stop, shrink its buffer and restart (the
 * whole block is asked of it: a multi-second gap). Now, while audio plays
 * or is paused, the block is kept and the views start over in it (its
 * 64 KB spare covers ~5,400 new entries; past that a view that does not
 * fit is refused as with any growth while playing), and the exact size
 * is owed until a stop: lib_pool_idle() (main loop, stopped) or the next
 * lib_open() then re-sizes it. The pool is reserved at shell start
 * (lib_reserve()), so it always exists before any playback. The check
 * and the allocation have no yield between them. */
static void pool_sync(void)
{
    int gen = tagcache_ui_generation();
    int n = tagcache_ui_count();
    size_t want;
    int i;

    if (n < 0)
        return;
    if (pool.handle > 0 && pool.generation == gen
        && (!pool.owed || audio_status()))
        return;
    want = POOL_SPARE + (size_t)n * sizeof(struct rec);
    if (pool.generation != gen)
    {
        for (i = 0; i < MAX_VIEWS; i++)
            slots[i].off = -1;
        pool.used = 0;
        for (unsigned j = 0; j < ARRAYLEN(future_pools); j++)
        {
            struct future_pool *p = &future_pools[j];
            if (p->handle > 0 && p->ctx == tagcache_context_active()
                && p->generation == gen)
            {
                int old = pool.handle;
                pool.handle = p->handle;
                pool.size = p->size;
                pool.generation = gen;
                pool.owed = false;
                memset(p, 0, sizeof(*p));
                if (old > 0)
                    core_free(old);
                return;
            }
        }
    }
    if (audio_status())
    {
        if (pool.handle > 0)
        {
            pool.generation = gen;
            pool.owed = (size_t)pool.size != want;
            UPDATE_TRACE("lib pool kept size=%d want=%ld playing", pool.size,
                         (long)want);
        }
        else                        /* an earlier allocation failed: the
                                       lists stay empty until a stop */
            UPDATE_TRACE("lib alloc refused what=%s bytes=%ld", "pool",
                         (long)want);
        return;
    }
    if (pool.handle > 0)
        pool.handle = core_free(pool.handle);
    pool.size = pool.used = 0;
    pool.owed = false;
    pool.handle = core_alloc(want);
    if (pool.handle <= 0)
    {
        UPDATE_TRACE("lib alloc failed what=%s bytes=%ld", "pool",
                     (long)want);
        pool.handle = 0;
        pool.generation = POOL_GEN_NONE;
        return;
    }
    pool.size = want;
    pool.generation = gen;
    DEBUGF("lib pool %ld bytes for %d entries\n", (long)want, n);
}

static void lib_pool_idle_held(void)
{
    if (!audio_status() && (pool.owed || pool.handle <= 0
                            || pool.generation != tagcache_ui_generation()))
        pool_sync();
}

void lib_pool_idle(void)
{
    struct tagcache_context *held = tagcache_context_acquire();
    lib_pool_idle_held();
    tagcache_context_release(held);
}

/* At shell start, before any playback: the pool for the database on disk
 * (the RAM cache may not be loaded yet), so the first lib_open() never
 * allocates, even when music resumes at boot. pool_sync() re-sizes it
 * once the RAM cache is in, while stopped. */
static void pool_reserve_start(void)
{
    long n = tagcache_ui_disk_entries();
    size_t want = POOL_SPARE + (size_t)MAX(n, 0) * sizeof(struct rec);

    if (pool.handle > 0)
        return;
    pool.handle = core_alloc(want);
    if (pool.handle <= 0)
    {
        UPDATE_TRACE("lib alloc failed what=%s bytes=%ld", "pool",
                     (long)want);
        pool.handle = 0;
        return;
    }
    pool.size = want;
    pool.used = 0;
    pool.generation = POOL_GEN_NONE;
    pool.owed = true;
    DEBUGF("lib pool %ld bytes reserved for %ld entries\n", (long)want, n);
}

/* Makes room for 'need' more bytes at the end of the pool. */
static bool pool_reserve(size_t need)
{
    size_t size;
    int h;

    if (pool.handle <= 0)
        return false;
    if ((size_t)pool.used + need <= (size_t)pool.size)
        return true;
    if (audio_status())
    {
        /* P6 Rev 1 item 2: 0 B allocated while audio plays */
        UPDATE_TRACE("lib alloc refused what=%s bytes=%ld", "pool-grow",
                     (long)need);
        return false;
    }
    size = pool.used + need + POOL_SPARE;
    h = core_alloc(size);
    if (h <= 0)
    {
        UPDATE_TRACE("lib alloc failed what=%s bytes=%ld", "pool-grow",
                     (long)size);
        return false;
    }
    memcpy(core_get_data(h), pool_base(), pool.used);
    core_free(pool.handle);
    pool.handle = h;
    pool.size = size;
    DEBUGF("lib pool grown to %ld bytes\n", (long)size);
    return true;
}

#ifdef SIMULATOR
/* the pool's high-water mark (tests/perf: sizing POOL_SPARE, M4) */
static void pool_peak_trace(void)
{
    static int peak;

    if (pool.used > peak)
    {
        peak = pool.used;
        UPDATE_TRACE("lib pool peak used=%d size=%d", pool.used, pool.size);
    }
}
#endif

/* Views start 8-byte aligned in the pool: a view may hold a struct
 * with 64-bit members (the playlist scratch's tagcache_search; UBSan in
 * the simulator, H1). */
#define SLOT_ALIGN(n) ALIGN_UP((n), 8)

/* A new view of 'size' bytes at the end of the pool (or, with
 * size 0, without memory). Returns its handle or -1. */
static int slot_new(size_t size)
{
    int i;

    size = SLOT_ALIGN(size);
    for (i = 0; i < MAX_VIEWS && slots[i].size; i++)
        ;
    if (i == MAX_VIEWS)
    {
        UPDATE_TRACE("lib alloc failed what=%s bytes=%d", "view", 0);
        return -1;
    }
    slots[i].off = -1;
    slots[i].size = 1;          /* in use */
    slots[i].count = 0;
    slots[i].generation = tagcache_ui_generation();
    if (size)
    {
        if (!pool_reserve(size))
        {
            slots[i].size = 0;
            return -1;
        }
        slots[i].off = pool.used;
        slots[i].size = size;
        pool.used += size;
#ifdef SIMULATOR
        pool_peak_trace();
#endif
    }
    return i + 1;
}

/* Resizes the newest view to 'size' bytes (growing may move the pool). */
static bool slot_resize(int h, size_t size)
{
    struct slot *sl = &slots[h - 1];

    size = SLOT_ALIGN(size);
    if (sl->off < 0 || sl->off + sl->size != pool.used)
        return (size_t)sl->size >= size;
    if (size > (size_t)sl->size && !pool_reserve(size - sl->size))
        return false;
    sl->size = size;
    pool.used = sl->off + size;
#ifdef SIMULATOR
    pool_peak_trace();
#endif
    return true;
}

/* #47: the open Artists list read from the precompute file (its rows are
 * the file's LIB_ARTISTS ROWS, in order), or 0: an artist's row number
 * there finds its albums in the file (from_artist_slice()) */
static int art_view;
#ifdef SIMULATOR
static bool artalb_scan_only;   /* lib_artalb_check(): the reference */
#endif

static void slot_free(int h)
{
    struct slot *sl;
    int i, off, size;

    if (h <= 0 || h > MAX_VIEWS || !slots[h - 1].size)
        return;
    if (h == art_view)
        art_view = 0;
    sl = &slots[h - 1];
    off = sl->off;
    size = sl->size;
    sl->size = 0;
    if (off < 0 || pool.handle <= 0)
        return;
    /* close the gap: later views move down */
    memmove(pool_base() + off, pool_base() + off + size,
            pool.used - off - size);
    pool.used -= size;
    for (i = 0; i < MAX_VIEWS; i++)
        if (slots[i].size && slots[i].off > off)
            slots[i].off -= size;
}

static struct view *get_view(int handle)
{
    struct slot *sl;
    struct view *v;

    if (handle <= 0 || handle > MAX_VIEWS)
        return NULL;
    sl = &slots[handle - 1];
    if (!sl->size || sl->off < 0 || pool.handle <= 0)
        return NULL;
    v = (struct view *)(pool_base() + sl->off);
    CHECK_ALIGN(v);
    return v;
}

static char *bx_base(const struct bctx *c)
{
    return c->pool ? (char *)get_view(c->h) : core_get_data(c->h);
}

static bool view_fresh(const struct view *v)
{
    return v->generation == tagcache_ui_generation()
        && tagcache_ui_count() >= 0;
}

static void *view_ext(struct view *v, uint32_t off)
{
    return off ? (char *)v + off : NULL;
}

static int16_t *view_progress(struct view *v)
{
    return view_ext(v, v->off_prog);
}

/* Places the extras 'what' (X_*) after n rows; returns the view's size. */
static size_t view_layout(struct view *v, int n, unsigned what)
{
    size_t off = sizeof(struct view) + (size_t)n * sizeof(int32_t);

    v->off_prog = v->off_unpl = v->off_cnt = v->off_key = v->off_sub = 0;
    if (what & X_PROG)
    {
        v->off_prog = off;
        off += (size_t)n * sizeof(int16_t);
    }
    if (what & X_UNPL)
    {
        v->off_unpl = off;
        off += n;
    }
    off = ALIGN_UP(off, 4);
    if (what & X_CNT)
    {
        v->off_cnt = off;
        off += (size_t)n * sizeof(uint32_t);
    }
    if (what & X_KEY)
    {
        v->off_key = off;
        off += (size_t)n * 2 * sizeof(uint32_t);
    }
    if (what & X_SUB)
    {
        v->off_sub = off;
        off += sizeof(struct subcache);
    }
    return ALIGN_UP(off, 4);
}

static bool has_progress(int cat, int group)
{
#ifdef HAVE_BCMFW_VIDEO
    /* TV shows: progress and the unplayed dot (progress_compute());
     * lib_child_count() stays the file's COUNT, the show's episodes */
    if (group != G_NONE && cat == LIB_TVSHOWS)
        return true;
#endif
    return group != G_NONE && (cat == LIB_AUDIOBOOKS || cat == LIB_PODCASTS);
}

/* What a category lists, given its filter. */
static void classify(enum lib_cat cat, const struct lib_filter *f,
                     int *group, int *order, int *cls)
{
    bool album = f && f->album != LIB_ANY;

    *group = G_NONE;
    *order = O_TITLE;
    *cls = C_MUSIC;
    switch (cat)
    {
    case LIB_ARTISTS:      *group = G_ARTIST; *cls = C_MUSIC_SOLO; break;
    case LIB_ALBUMS:       *group = G_ALBUM; break;
    case LIB_COMPILATIONS: *group = G_ALBUM; *cls = C_COMPILATION; break;
    case LIB_GENRES:       *group = G_GENRE; break;
    case LIB_COMPOSERS:    *group = G_COMPOSER; break;
    case LIB_AUDIOBOOKS:
        *cls = C_BOOK;
        if (album)
            *order = O_TRACK;
        else
            *group = G_ALBUM;
        break;
    case LIB_PODCASTS:
        *cls = C_PODCAST;
        if (album)
            *order = O_NEWEST;
        else
            *group = G_ALBUM;
        break;
#ifdef HAVE_BCMFW_VIDEO
    /* Phase 14a (3.3): movies in title order (the walk); shows grouped by
     * show; a show's seasons (collapsed from its episodes after the
     * build: build_seasons()) and its episodes in season, episode, title
     * order (cmp_tie(tagcache_context_active(), ) O_TRACK) */
    case LIB_MOVIES:
        *cls = C_MOVIE;
        break;
    case LIB_TVSHOWS:
        *cls = C_TV;
        *group = G_ALBUM;
        break;
    case LIB_TVSEASONS:
    case LIB_TVEPISODES:
        *cls = C_TV;
        *order = O_TRACK;
        break;
#endif
    default:    /* LIB_SONGS */
        if (album)
        {
            *order = O_TRACK;
            *cls = C_ANY;       /* the album decides (a book's tracks) */
        }
        else if (f && f->artist != LIB_ANY)
            *order = O_ARTIST_ALL;
        break;
    }
}

/* What lib_sublabel() gives: the album artist under an Albums,
 * Compilations or Audiobooks row (a book's author, P6 owner decision),
 * the artist under a song (Songs, an artist's, genre's or composer's
 * songs; not an album's track list). */
static int sub_of(int cat, const struct lib_filter *f)
{
    if ((cat == LIB_ALBUMS || cat == LIB_COMPILATIONS ||
         cat == LIB_AUDIOBOOKS) && unfiltered(f))
        return SUB_ALBUM_ARTIST;
    if (cat == LIB_SONGS && !(f && f->album != LIB_ANY))
        return SUB_TRACK_ARTIST;
#ifdef HAVE_BCMFW_VIDEO
    if (cat == LIB_MOVIES || cat == LIB_TVEPISODES)
        return SUB_DURATION;
#endif
    return SUB_NONE;
}

/* The extras a view keeps (X_*). */
static unsigned extras_of(int cat, int group, const struct lib_filter *f)
{
    unsigned what = 0;
    bool all = unfiltered(f);

    if (has_progress(cat, group))
        what |= X_PROG | (cat == LIB_PODCASTS ? X_UNPL : 0);
#ifdef HAVE_BCMFW_VIDEO
    if (has_progress(cat, group) && cat == LIB_TVSHOWS)
        what |= X_UNPL;
#endif
    if (group == G_ALBUM)
        what |= X_CNT | X_KEY;
    if (group == G_ARTIST && all)
        what |= X_CNT;
    if (sub_of(cat, f) != SUB_NONE)
        what |= X_SUB;
#ifdef HAVE_BCMFW_VIDEO
    if (cat == LIB_MOVIES && all)
        what |= X_KEY;                  /* each movie's poster */
#endif
    return what;
}

/* Title order needs no sort: the title tag file is stored sorted by
 * ui_groupkey(), then ui_casefold_cmp() (tagcache.c sorts it at commit,
 * #47), so walking it yields the rows in list order. */
static bool walked(int group, int order)
{
    return group == G_NONE && order == O_TITLE;
}

/* Group identities seen while scanning: a direct-mapped table in the
 * build's block (4 KB, see rows_build()). */
#define SEEN_SLOTS  512
#define SEEN_BYTES  (SEEN_SLOTS * 2 * sizeof(uint32_t))

/* True if identity k1/k2 is in the table; else enters it. */
static bool seen(uint32_t *t, uint32_t k1, uint32_t k2)
{
    uint32_t *e = &t[2 * ((k1 * 2654435761u ^ k2 * 40503u) >> 23)];

    if (e[0] == k1 && e[1] == k2)
        return true;
    e[0] = k1;
    e[1] = k2;
    return false;
}

static uint32_t *bx_seen(const struct bctx *c)
{
    return (uint32_t *)(bx_base(c) + c->seen_off);
}

/* Fills the build's records (up to 'cap') with the rows; returns the row
 * count, or -1 if the database changed or is not usable.
 * Group views keep one record per group identity (exact tag seeks) they
 * have not seen recently: a 512-slot table holds every genre, most
 * composers, and an album's or artist's tracks come together in the
 * master index (folder order), so step 2 sorts a few thousand records
 * instead of every track. An identity seen again after eviction only
 * adds a duplicate, which step 2 removes (the first, smallest index,
 * stays the representative). Untagged genres and composers are checked
 * once per new identity, not per track.
 * Yields every YIELD_EVERY entries (see the timings at the end), then
 * re-checks the generation and re-fetches the records, the table and the
 * filter's RAM cache pointers. */
static int scan(struct tagcache_context *ctx, struct bctx *c, int cls, int group, int order,
                const struct lib_filter *f, int cap)
{
    struct match m;
    struct rec *tmp = recs_at(c, c->recs_off);
    uint32_t *table = bx_seen(c);
    int total = tagcache_ui_count_ctx(ctx);
    int32_t pos = 0, i = -1;
    int n = 0, steps = 0;

    if (total < 0)
        return -1;
    if (!match_init(ctx, &m, cls, f))
        return 0;
    memset(table, 0xff, SEEN_BYTES);    /* no identity is ~0, ~0 */
    for (;;)
    {
        int32_t flag;
        const int32_t *s;

        if (++steps == YIELD_EVERY)
        {
            steps = 0;
            if (!bx_yield(ctx, c))
                return -1;
            tmp = recs_at(c, c->recs_off);
            table = bx_seen(c);
            if (!match_init(ctx, &m, cls, f))
                return 0;
        }
        if (walked(group, order))
        {
            if (!tagcache_ui_walk_ctx(ctx, tag_title, &pos, &i))
                break;
        }
        else if (++i >= total)
            break;
        s = tagcache_ui_seeks_ctx(ctx, i, &flag);
        if (!s || !match_entry(ctx, &m, i, s, flag))
            continue;
        if (n == cap)
            break;
        tmp[n].idx = i;
        group_ident(ctx, c, &tmp[n], group, s, flag);
        if (group != G_NONE)
        {
            if (seen(table, tmp[n].k1, tmp[n].k2))
                continue;
            if (group == G_GENRE && !str_of(ctx, s, tag_genre))
                continue;               /* untagged: stays in the table */
            if (group == G_COMPOSER && !str_of(ctx, s, tag_composer))
                continue;
        }
        n++;
    }
    return n;
}

/* Steps 1-4. Afterwards the build's records 0..m-1 are in list order with
 * k1 = the row they belong to and k2 = 1 for a row's first record (its
 * representative, idx), 0 for a case variant of the row before (group
 * views; step 4). Returns m and the row count in *rows, or -1 if the
 * database changed (or the precompute must stop). */
static int rows_build(struct tagcache_context *ctx, struct bctx *c, int cls, int group, int order,
                      const struct lib_filter *f, int cap, int *rows)
{
    struct rec *tmp;
    int n, i, j, r = 0, rep = -1;

    *rows = 0;
    n = scan(ctx, c, cls, group, order, f, cap);
    if (n < 0)
        return -1;

    c->group = G_NONE;
    c->order = -1;              /* identity only, no tie-breaks */
    if (group != G_NONE && n > 1)
    {
        /* step 2: one record per group (the smallest index) */
        if (!sort_recs(ctx, c, c->recs_off, n))
            return -1;
        tmp = recs_at(c, c->recs_off);
        for (i = 1, j = 1; i < n; i++)
            if (tmp[i].k1 != tmp[j - 1].k1 || tmp[i].k2 != tmp[j - 1].k2)
                tmp[j++] = tmp[i];
        n = j;
    }

    /* step 3: sort prefixes */
    for (i = 0; i < n && !walked(group, order); i++)
    {
        if (i % YIELD_EVERY == YIELD_EVERY - 1 && !bx_yield(ctx, c))
            return -1;
        sort_prefix(ctx, &recs_at(c, c->recs_off)[i], group, order);
    }
    if (!bx_yield(ctx, c))
        return -1;
    c->group = group;
    c->order = order;
    if (!walked(group, order) && !sort_recs(ctx, c, c->recs_off, n))
        return -1;

    /* step 4: rows, and case variants of one group */
    for (i = 0; i < n; i++)
    {
        struct rec *t;

        if (i % YIELD_EVERY == YIELD_EVERY - 1 && !bx_yield(ctx, c))
            return -1;
        t = &recs_at(c, c->recs_off)[i];
        if (group == G_NONE || rep < 0 || !same_group(ctx, group, t->idx, rep))
        {
            rep = t->idx;
            t->k2 = 1;
            r++;
        }
        else
            t->k2 = 0;
        t->k1 = r - 1;
    }
    *rows = r;
    return n;
}

/* -- precomputed lists (the UI side) ------------------------------------ */

static void pre_path(struct tagcache_context *ctx, char *buf, size_t len, const char *name)
{
    snprintf(buf, len, "%s/%s", tagcache_ui_db_path_ctx(ctx), name);
}

/* Checks a header read from the file against the database in RAM. */
static bool pre_hdr_ok(struct tagcache_context *ctx, const struct libfile_hdr *h)
{
#ifdef HAVE_BCMFW_VIDEO
    /* M8: no 800 B copy on the caller's stack (tagcache and UI threads):
     * the sum runs over the header in three pieces, the sum field as 0 */
    static const uint32_t zero;
#else
    struct libfile_hdr copy;
#endif
    struct libfile_sum_state s;

    if (h->magic != LIBFILE_MAGIC || h->version != LIBFILE_VERSION
        || h->sortkey_version != SORTKEY_VERSION
        || h->commitid != tagcache_ui_commitid_ctx(ctx)
        || h->entries != tagcache_ui_count_ctx(ctx)
        || h->nsec > LIBFILE_MAX_SEC)
        return false;
#ifdef HAVE_BCMFW_VIDEO
    libfile_sum_init(&s);
    libfile_sum_add(&s, h, offsetof(struct libfile_hdr, sum));
    libfile_sum_add(&s, &zero, sizeof(zero));
    libfile_sum_add(&s, &h->reserved,
                    sizeof(*h) - offsetof(struct libfile_hdr, reserved));
#else
    memcpy(&copy, h, sizeof(copy));
    copy.sum = 0;
    libfile_sum_init(&s);
    libfile_sum_add(&s, &copy, sizeof(copy));
#endif
    return libfile_sum_end(&s) == h->sum;
}

static const struct libfile_sec *pre_sec(const struct libfile_hdr *h,
                                         int cat, int col, int elem)
{
    unsigned i;

    for (i = 0; i < h->nsec; i++)
        if (h->sec[i].cat == cat && h->sec[i].col == col)
            return h->sec[i].elem == elem ? &h->sec[i] : NULL;
    return NULL;
}

/* Shows in the library (LIB_PODCASTS rows), from the header of the
 * current precompute file: set on the tagcache thread when the file is
 * written or found current, read by the Podcasts panel (no allocation,
 * no storage read from a draw). -1 until a file has been seen; a load
 * that cannot precompute yet (LIB_PRE_WAIT) keeps the last value. */
static volatile int pre_podcasts = -1;

static void pre_note_counts(const struct libfile_hdr *h)
{
    const struct libfile_sec *s = pre_sec(h, LIB_PODCASTS, LIBFILE_ROWS,
                                          sizeof(int32_t));

    pre_podcasts = s ? (int)s->count : 0;
}

int lib_podcast_count(void)
{
    return pre_podcasts;
}

#ifdef HAVE_BCMFW_VIDEO
/* The .vix pass owed for the file of this database generation (3.4):
 * set when a precompute wrote the file or found it current, cleared when
 * vixpass_run() finished. While it is owed, lib_precompute() runs only
 * the pass (returning LIB_PRE_WAIT while audio plays: no I/O then, the
 * tagcache thread retries every second). -1: none. */
static volatile int vix_owed_gen = -1;
static int vix_owed_files;
static bool vix_owed_forced;
#endif

/* Remembers what the UI found (until the database or the file changes);
 * a torn file also asks the tagcache thread for a new one. */
static void pre_ui_note(int state, bool torn)
{
    pre_ui.tc_gen = tagcache_ui_generation();
    pre_ui.files = pre_files;
    pre_ui.state = state;
    if (state < 0)
        pre_ui.tab_ok = 0;
    if (torn)
    {
#ifdef HAVE_BCMFW_VIDEO
        vix_owed_gen = -1;      /* the rewrite comes first (vix_due(tagcache_context_active(), )) */
#endif
        UPDATE_TRACE("lib io precompute %s", "damaged");
        tagcache_ui_precompute_request();
    }
}

static bool pre_ui_known_bad(void)
{
    if (pre_ui.tc_gen != tagcache_ui_generation() || pre_ui.files != pre_files)
    {
        pre_ui.state = 0;
        pre_ui.tab_ok = 0;
        return false;
    }
    return pre_ui.state < 0;
}

/* Opens the file and checks its header (read into temporary view th,
 * LIBFILE_HDR_SIZE bytes). Returns the fd or -1. */
static int pre_ui_open(int th)
{
    char path[MAX_PATH];
    ssize_t got;
    int fd;
    bool ok;

    if (pre_ui_known_bad())
        return -1;
    pre_path(tagcache_context_active(), path, sizeof(path), LIBFILE_NAME);
    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        pre_ui_note(-1, false);
        return -1;
    }
    core_pin(pool.handle);
    got = read(fd, get_view(th), LIBFILE_HDR_SIZE);
    core_unpin(pool.handle);
    ok = got == (ssize_t)LIBFILE_HDR_SIZE
        && pre_hdr_ok(tagcache_context_active(), (struct libfile_hdr *)get_view(th));
    if (!ok)
    {
        close(fd);
        /* an old file is replaced anyway (the load queued a precompute) */
        pre_ui_note(-1, got == (ssize_t)LIBFILE_HDR_SIZE
                    && ((struct libfile_hdr *)get_view(th))->commitid
                       == tagcache_ui_commitid()
                    && ((struct libfile_hdr *)get_view(th))->entries
                       == tagcache_ui_count()
                    && ((struct libfile_hdr *)get_view(th))->magic
                       == LIBFILE_MAGIC);
        return -1;
    }
    /* also when the tagcache thread could not look yet (a boot that
     * resumed playback waits to precompute) */
    pre_note_counts((const struct libfile_hdr *)get_view(th));
    pre_ui_note(1, false);
    return fd;
}

/* Reads section 'sec' into view h at byte 'off' and checks its sum. */
static bool pre_ui_read(int fd, int h, size_t off,
                        const struct libfile_sec *sec)
{
    struct libfile_sum_state s;
    size_t bytes = (size_t)sec->count * sec->elem;
    ssize_t got;

    if (lseek(fd, sec->offset, SEEK_SET) != (off_t)sec->offset)
        return false;
    core_pin(pool.handle);
    got = read(fd, (char *)get_view(h) + off, bytes);
    core_unpin(pool.handle);
    if (got != (ssize_t)bytes)
        return false;
    libfile_sum_init(&s);
    libfile_sum_add(&s, (char *)get_view(h) + off, bytes);
    return libfile_sum_end(&s) == sec->sum;
}

/* Unfiltered category 'cat' from the precompute file into view h (sized
 * for a scan of every entry). Lays the view out (extras 'what'; counts
 * and keys read too) and returns its row count, or -1 (no usable file:
 * scan instead), -2 (no memory). Category 'alt' (>= 0) stands in for a
 * 'cat' the file lacks (*used_alt then). */
static int from_file(int h, int cat, int alt, unsigned what, bool *used_alt)
{
    struct libfile_sec rows, cnt, key;
    const struct libfile_sec *p;
    const struct libfile_hdr *hd;
    struct view *v;
    size_t size, orig = slots[h - 1].size;
    int fd, th, n;
    bool ok;

    th = slot_new(LIBFILE_HDR_SIZE);
    if (th <= 0)
        return -1;
    fd = pre_ui_open(th);
    if (fd < 0)
    {
        slot_free(th);
        return -1;
    }
    hd = (const struct libfile_hdr *)get_view(th);
    memset(&cnt, 0, sizeof(cnt));
    memset(&key, 0, sizeof(key));
    p = pre_sec(hd, cat, LIBFILE_ROWS, sizeof(int32_t));
    *used_alt = false;
    if (!p && alt >= 0)
    {
        /* #59: a file from before the author slice (pre_current(tagcache_context_active(), ) has it
         * rewritten at the next load) is not damaged: the books in title
         * order, from the header already read */
        cat = alt;
        *used_alt = true;
        p = pre_sec(hd, cat, LIBFILE_ROWS, sizeof(int32_t));
    }
    ok = p != NULL;
    if (p)
        rows = *p;
    if (ok && (what & X_CNT))
    {
        p = pre_sec(hd, cat, LIBFILE_COUNT, sizeof(uint32_t));
        ok = p && p->count == rows.count;
        if (ok)
            cnt = *p;
    }
    if (ok && (what & X_KEY))
    {
        p = pre_sec(hd, cat, LIBFILE_KEY, 2 * sizeof(uint32_t));
        ok = p && p->count == rows.count;
        if (ok)
            key = *p;
    }
    slot_free(th);              /* view h is the newest again */
    if (!ok || rows.count > (uint32_t)tagcache_ui_count())
    {
        close(fd);
        pre_ui_note(-1, true);
        return -1;
    }

    n = rows.count;
    v = get_view(h);
    size = view_layout(v, n, what);
    if (!slot_resize(h, size))
    {
        close(fd);
        return slot_resize(h, orig) ? -1 : -2;
    }
    /* get_view() after each read: read() may yield, the pool move */
    ok = pre_ui_read(fd, h, sizeof(struct view), &rows)
        && (!(what & X_CNT)
            || pre_ui_read(fd, h, get_view(h)->off_cnt, &cnt))
        && (!(what & X_KEY)
            || pre_ui_read(fd, h, get_view(h)->off_key, &key));
    close(fd);
    if (!ok)
    {
        pre_ui_note(-1, true);
        /* the scan needs the view's whole size back (records, table) */
        return slot_resize(h, orig) ? -1 : -2;
    }
    return n;
}

/* Reads album table entry 'i' (sec at its offset) into *e. */
static bool tab_get(int fd, const struct libfile_sec *sec, uint32_t i,
                    struct libfile_album *e)
{
    off_t at = sec->offset + (off_t)i * sizeof(*e);

    return lseek(fd, at, SEEK_SET) == at
        && read(fd, e, sizeof(*e)) == (ssize_t)sizeof(*e);
}

/* Checks the album table's sum once per file (streamed through th). */
static bool tab_verify(int fd, int th, const struct libfile_sec *sec)
{
    struct libfile_sum_state s;
    size_t left = (size_t)sec->count * sec->elem;

    if (pre_ui.tab_ok)
        return pre_ui.tab_ok > 0;
    if (lseek(fd, sec->offset, SEEK_SET) != (off_t)sec->offset)
        return false;
    libfile_sum_init(&s);
    while (left)
    {
        size_t chunk = MIN(left, (size_t)LIBFILE_HDR_SIZE / 16 * 16);
        ssize_t got;

        core_pin(pool.handle);
        got = read(fd, get_view(th), chunk);
        core_unpin(pool.handle);
        if (got != (ssize_t)chunk)
            break;
        libfile_sum_add(&s, get_view(th), chunk);
        left -= chunk;
    }
    pre_ui.tab_ok = (!left && libfile_sum_end(&s) == sec->sum) ? 1 : -1;
    if (pre_ui.tab_ok < 0)
        pre_ui_note(-1, true);
    return pre_ui.tab_ok > 0;
}

/* A filtered album view (a composer's albums; an artist's when the file
 * has no ALBUMS range for it, #47): each row's track count and
 * art key from the precomputed album table, by its representative
 * (binary search on the file: ~13 small reads per row, one open per
 * lib_open). Rows not found (a filter that picks another representative,
 * e.g. genre + artist) keep -1 / 0. */
static void counts_from_table(int h)
{
    struct libfile_sec tab = { 0 };
    const struct libfile_sec *p;
    struct libfile_album e;
    int th, fd, i;
    bool ok;

    th = slot_new(LIBFILE_HDR_SIZE);
    if (th <= 0)
        return;
    fd = pre_ui_open(th);
    if (fd < 0)
    {
        slot_free(th);
        return;
    }
    p = pre_sec((const struct libfile_hdr *)get_view(th),
                LIBFILE_CAT_ALBUMTAB, 0, sizeof(struct libfile_album));
    ok = p != NULL;
    if (ok)
        tab = *p;
    ok = ok && tab_verify(fd, th, &tab);
    for (i = 0; ok && i < slots[h - 1].count; i++)
    {
        int32_t rep = get_view(h)->rows[i];
        uint32_t lo = 0, hi = tab.count;

        while (lo < hi)
        {
            uint32_t mid = (lo + hi) / 2;

            if (!tab_get(fd, &tab, mid, &e))
            {
                ok = false;
                break;
            }
            if (e.rep == rep)
            {
                struct view *v = get_view(h);
                uint32_t *k = view_ext(v, v->off_key);

                ((uint32_t *)view_ext(v, v->off_cnt))[i] = e.count;
                k[2 * i] = e.key_lo;
                k[2 * i + 1] = e.key_hi;
                break;
            }
            if (e.rep < rep)
                lo = mid + 1;
            else
                hi = mid;
        }
    }
    close(fd);
    slot_free(th);
}

/* #47: an artist's albums (lib_open(LIB_ALBUMS, artist filter)) from the
 * precompute file's per-artist range (libfile.h, LIB_ARTISTS ALBUMS) into
 * view h, laid out with the extras 'what' (counts and keys filled), when
 * the artist is a row of the open Artists list read from the file
 * (art_view). One open, the header, one 8-byte read and one read of the
 * artist's records; no RAM walk, no album table search. Each record is
 * checked against the RAM cache (the range has no sum of its own).
 * Returns the row count, or -1 (scan instead, the view as it came). */
static int from_artist_slice(int h, const struct lib_filter *f,
                             unsigned what)
{
    struct tagcache_context *ctx = tagcache_context_active();
    const struct libfile_sec *p;
    const struct libfile_hdr *hd;
    struct libfile_sec sec;
    struct view *v = get_view(art_view);
    size_t size, raw, orig = slots[h - 1].size;
    uint32_t se[2], arts, row;
    const int32_t *a;
    int fd, th, n, i;
    bool ok;

    if (!v || v->cat != LIB_ARTISTS || !view_fresh(v))
        return -1;
    arts = v->count;
    for (row = 0; row < arts && v->rows[row] != f->artist; row++)
        ;
    if (row == arts)
        return -1;
    th = slot_new(LIBFILE_HDR_SIZE);
    if (th <= 0)
        return -1;
    fd = pre_ui_open(th);
    if (fd < 0)
    {
        slot_free(th);
        return -1;
    }
    hd = (const struct libfile_hdr *)get_view(th);
    p = pre_sec(hd, LIB_ARTISTS, LIBFILE_ROWS, sizeof(int32_t));
    ok = p && p->count == arts;
    p = ok ? pre_sec(hd, LIB_ARTISTS, LIBFILE_ALBUMS, sizeof(uint32_t))
           : NULL;
    ok = p && p->count >= LIBFILE_ARTALB_HDR(arts);
    if (ok)
        sec = *p;
    slot_free(th);              /* view h is the newest again */
    ok = ok
        && lseek(fd, sec.offset + row * 4, SEEK_SET)
           == (off_t)(sec.offset + row * 4)
        && read(fd, se, sizeof(se)) == (ssize_t)sizeof(se)
        && se[0] >= LIBFILE_ARTALB_HDR(arts) && se[0] <= se[1]
        && se[1] <= sec.count
        && (se[1] - se[0]) % LIBFILE_ARTALB_WORDS == 0;
    if (!ok)
    {
        close(fd);
        return -1;
    }
    n = (se[1] - se[0]) / LIBFILE_ARTALB_WORDS;
    size = view_layout(get_view(h), n, what);
    raw = (size_t)n * sizeof(struct libfile_artalb);
    /* grown only when the scan's room is too small (an artist with most
     * of the library's albums); never shrunk here, so a fallback to the
     * scan never has to grow the view back */
    if (size + raw > orig && !slot_resize(h, size + raw))
    {
        close(fd);
        return -1;              /* the view kept 'orig': scan */
    }
    ok = lseek(fd, sec.offset + se[0] * 4, SEEK_SET)
         == (off_t)(sec.offset + se[0] * 4);
    if (ok)
    {
        core_pin(pool.handle);
        ok = read(fd, (char *)get_view(h) + size, raw) == (ssize_t)raw;
        core_unpin(pool.handle);
    }
    close(fd);
    /* after the reads (they may yield): the view and the RAM cache */
    v = get_view(h);
    a = tagcache_ui_seeks_ctx(ctx, f->artist, NULL);
    ok = ok && a && v->off_cnt && v->off_key;
    for (i = 0; ok && i < n; i++)
    {
        const struct libfile_artalb *e = (const struct libfile_artalb *)
            ((char *)v + size) + i;
        uint32_t *k = view_ext(v, v->off_key);
        const int32_t *s;
        int32_t flag;

        s = e->rep >= 0 && e->rep < tagcache_ui_count()
            ? tagcache_ui_seeks_ctx(ctx, e->rep, &flag) : NULL;
        ok = s && class_ok(C_MUSIC, flag) && same_artist(ctx, s, a);
        /* rows and extras lie below 'size', the records above it */
        v->rows[i] = e->rep;
        ((uint32_t *)view_ext(v, v->off_cnt))[i] = e->count;
        k[2 * i] = e->key_lo;
        k[2 * i + 1] = e->key_hi;
    }
    if (!ok)
    {
        UPDATE_TRACE("lib artist slice bad row=%lu n=%d", (unsigned long)row,
                     n);
        if (size + raw > orig)
            slot_resize(h, orig);       /* a shrink: cannot fail */
        return -1;
    }
    return n;
}

/* -- building a view (UI thread) ---------------------------------------- */

static void progress_compute(int h, int only);

/* -- #67: the last-played book on top (Phase 8b §3.2, §4.1) ------------- */

int lib_pin_find(const uint32_t *keys, int n, uint64_t k)
{
    uint32_t lo = (uint32_t)k, hi = (uint32_t)(k >> 32);
    int i;

    if (!keys || !k || n < 2)
        return -1;
    for (i = 0; i < n; i++)
        if (keys[2 * i] == lo && keys[2 * i + 1] == hi)
            return i;
    return -1;
}

#ifdef LIB_PIN
/* #47: the pre-#47 key of the book of track 'rep' (art keys folded ASCII
 * only), 0 if it is the same or none. Tagged books only: a folder-keyed
 * book with a non-ASCII capital in its path keeps no old record. */
static ab_key_t book_key_ascii(int32_t rep, ab_key_t key)
{
    const int32_t *s = rep >= 0 ? tagcache_ui_seeks(rep, NULL) : NULL;
    const char *album = s ? str_of(tagcache_context_active(), s, tag_album)
                          : NULL;
    ab_key_t old = album
        ? artcache_key_ascii(str_of(tagcache_context_active(), s,
                                    tag_albumartist),
                             str_of(tagcache_context_active(), s, tag_artist),
                             album, NULL)
        : 0;

    return old != key ? old : 0;
}

/* The scan path's book keys (no precompute file): from the RAM cache
 * only, over each row's representative, with the precedence of
 * artcache_key_for_id3() and the book queue's key (album artist, artist,
 * album). A book without an album tag would need its first track's path
 * (a disk read on the device): it gets 0, so no pin and the glyph, as
 * before. No yield in the loop: the seeks stay valid. */
static uint64_t book_tag_key(int32_t rep)
{
    const int32_t *s = tagcache_ui_seeks(rep, NULL);
    const char *album = s ? str_of(tagcache_context_active(), s, tag_album) : NULL;

    return album ? artcache_key(str_of(tagcache_context_active(), s, tag_albumartist),
                                str_of(tagcache_context_active(), s, tag_artist), album, NULL) : 0;
}

static void book_keys_scan(struct view *v, int n)
{
    uint32_t *k = view_ext(v, v->off_key);
    int i;

    for (i = 0; k && i < n; i++)
    {
        uint64_t key = book_tag_key(v->rows[i]);

        k[2 * i] = (uint32_t)key;
        k[2 * i + 1] = (uint32_t)(key >> 32);
    }
}

/* Moves element 'row' of an array of 'sz'-byte elements to the front,
 * the ones before it one place down (sz <= 8). */
static void move_front(void *base, int row, size_t sz)
{
    unsigned char t[8], *b = base;

    if (!b)
        return;
    memcpy(t, b + (size_t)row * sz, sz);
    memmove(b + sz, b, (size_t)row * sz);
    memcpy(b, t, sz);
}

/* Pins the last-played book of an unfiltered books view of n rows: the
 * same row moves to row 0 with its count, key and unplayed flag; the
 * caller computes progress afterwards, on the final order. */
static void pin_pass(struct view *v, int n, bool file)
{
    const uint32_t *keys = view_ext(v, v->off_key);
    uint64_t k = ab_store_pin_key(NULL);
    int row;

    (void)file;                         /* trace only */
    v->pinned = 0;
    if (!k || !keys)
    {
        UPDATE_TRACE("lib books pin none why=%s", "nokey");
        return;
    }
    if (n < 2)
    {
        UPDATE_TRACE("lib books pin none why=%s", "small");
        return;
    }
    row = lib_pin_find(keys, n, k);
    if (row < 0)
    {
        /* #47: a record still under its pre-#47 key (until the book's
         * next checkpoint): the rows' old keys, from RAM */
        for (row = n - 1; row >= 0; row--)
            if (book_key_ascii(v->rows[row], 0) == k)
                break;
        if (row >= 0)
            k = book_tag_key(v->rows[row]);
    }
    if (row < 0)
    {
        UPDATE_TRACE("lib books pin none why=%s", "absent");
        return;
    }
    /* an untagged book (keyed by its folder in the precompute file) is
     * never pinned, on either path: its tags give no key */
    if (book_tag_key(v->rows[row]) != k)
    {
        UPDATE_TRACE("lib books pin none why=%s", "nokey");
        return;
    }
    if (row > 0)
    {
        move_front(v->rows, row, sizeof(int32_t));
        move_front(view_ext(v, v->off_cnt), row, sizeof(uint32_t));
        move_front(view_ext(v, v->off_key), row, 2 * sizeof(uint32_t));
        move_front(view_ext(v, v->off_unpl), row, 1);
    }
    v->pinned = 1;
    UPDATE_TRACE("lib books pin row=%d key=%08lx src=%s order=%s path=%s",
                 row, (unsigned long)(uint32_t)k,
                 ab_store_pin_src() == AB_PIN_PLAY ? "play"
                 : ab_store_pin_src() == AB_PIN_PENDING ? "pending" : "stamp",
                 v->by_author ? "author" : "title",
                 file ? "file" : "scan");
}
#endif /* LIB_PIN */

#ifdef HAVE_BCMFW_VIDEO
/* LIB_TVSEASONS: the show's n episodes (rows in season order) become its
 * distinct seasons, ascending, in place. RAM index only, no yield. */
static int build_seasons(struct view *v, int n)
{
    int i, r = 0;

    for (i = 0; i < n; i++)
    {
        const int32_t *s = tagcache_ui_seeks(v->rows[i], NULL);
        int season = s ? season_of(s) : 1;

        if (r == 0 || v->rows[r - 1] != season)
            v->rows[r++] = season;
    }
    return r;
}
#endif

static void bctx_view(struct bctx *c, int h, int gen)
{
    memset(c, 0, sizeof(*c));
    c->gen = gen;
    c->h = h;
    c->pool = true;
    c->recs_off = sizeof(struct view);
    c->seen_off = slots[h - 1].size - SEEN_BYTES;
    c->memo.generation = -1;
}

/* Builds an (unregistered) view. Returns its handle or < 0. *src says
 * where its rows came from (trace). */
static int build(enum lib_cat cat, const struct lib_filter *f,
                 const char **src)
{
    int group, order, cls;
    int gen = tagcache_ui_generation();
    int n, m, r = 0, i, h;
    unsigned what;
    size_t size;
    struct bctx c;
    struct view *v;
    bool boost, file = false, by_author = false;

    classify(cat, f, &group, &order, &cls);
    what = extras_of(cat, group, f);
    *src = "scan";

    /* one pass: room for every entry (the pool is sized for that), the
     * view is trimmed to its rows afterwards */
    n = tagcache_ui_count();
    if (n < 0)
        n = 0;                  /* not ready: an empty, stale-able view */

    boost = n > 1000;
    if (boost)
    {
        cpu_boost(true);
    }

    if (tagcache_ui_count() < 0)
    {
        /* not ready: an empty view without memory, stale on arrival */
        h = slot_new(0);
        goto out;
    }
    pool_sync();
    h = slot_new(sizeof(struct view) + (size_t)n * sizeof(struct rec)
                 + SEEN_BYTES);
    if (h <= 0)
    {
        UPDATE_TRACE("lib alloc failed what=%s rows=%d", "rows", n);
        h = -2;
        goto out;
    }
    if (n > 0 && unfiltered(f))
    {
        int fcat = cat;
        bool alt = false;

        /* #59: Sort By Author reads the books' author-order slice */
        if (cat == LIB_AUDIOBOOKS && book_order == LIB_BOOKS_BY_AUTHOR)
            fcat = LIBFILE_CAT_BOOKS_AUTHOR;
        r = from_file(h, fcat, fcat != (int)cat ? (int)cat : -1, what, &alt);
        by_author = r >= 0 && !alt && fcat != (int)cat;
        if (r == -2)
        {
            slot_free(h);
            h = -2;
            goto out;
        }
        if (tagcache_ui_generation() != gen)
            goto changed;
        file = r >= 0;
        if (file)
            *src = "file";
    }
    else if (n > 0 && cat == LIB_ALBUMS && f->artist != LIB_ANY
             && f->album == LIB_ANY && f->genre == LIB_ANY
             && f->composer == LIB_ANY
#ifdef HAVE_BCMFW_VIDEO
             && f->season == LIB_ANY
#endif
#ifdef SIMULATOR
             && !artalb_scan_only
#endif
             )
    {
        /* #47: an artist's albums, one range of the file */
        r = from_artist_slice(h, f, what);
        if (tagcache_ui_generation() != gen)
            goto changed;
        file = r >= 0;
        if (file)
            *src = "slice";
    }
    if (!file)
    {
        r = 0;
        if (n > 0)
        {
            /* making room may have yielded or moved the RAM cache */
            if (tagcache_ui_generation() != gen)
                goto changed;
            bctx_view(&c, h, gen);
            m = rows_build(tagcache_context_active(), &c, cls, group, order, f, n, &r);
            yields += c.yields;
            if (m < 0)
                goto changed;
            /* step 5: rows[j] (4 B) never overtakes record i (12 B) */
            v = get_view(h);
            for (i = 0, r = 0; i < m; i++)
            {
                struct rec t = ((struct rec *)v->rows)[i];

                if (t.k2)
                    v->rows[r++] = t.idx;
            }
#ifdef HAVE_BCMFW_VIDEO
            if (cat == LIB_TVSEASONS)
                r = build_seasons(v, r);
#endif
        }
        v = get_view(h);
        size = view_layout(v, r, what);
        if (!slot_resize(h, size))
        {
            UPDATE_TRACE("lib alloc failed what=%s rows=%d", "rows", r);
            slot_free(h);
            h = -2;
            goto out;
        }
        v = get_view(h);
        if (v->off_cnt)
            memset(view_ext(v, v->off_cnt), 0xff, (size_t)r * 4);
        if (v->off_key)
            memset(view_ext(v, v->off_key), 0, (size_t)r * 8);
#ifdef LIB_PIN
        /* #67: the books' keys from the RAM cache, never from disk */
        if (cat == LIB_AUDIOBOOKS && unfiltered(f))
            book_keys_scan(v, r);
#endif
    }
    slots[h - 1].count = r;
    if (!file && group == G_ALBUM && !unfiltered(f) && r > 0)
        counts_from_table(h);
    slot_resize(h, view_layout(get_view(h), r, what));

    v = get_view(h);
    v->cat = cat;
    if (cat == LIB_ARTISTS && file)
        art_view = h;           /* #47 */
    v->sub = sub_of(cat, f);
    v->by_author = by_author;
    v->pinned = 0;
    if (cat == LIB_AUDIOBOOKS && unfiltered(f))
    {
        UPDATE_TRACE("lib books order=%s n=%d", by_author ? "author"
                     : book_order == LIB_BOOKS_BY_AUTHOR ? "fallback"
                     : "title", r);
#ifdef LIB_PIN
        pin_pass(v, r, file);       /* before unplayed and progress */
#endif
    }
    v->group = group;
#ifdef HAVE_BCMFW_VIDEO
    if (cat == LIB_TVSEASONS)
        v->group = G_SEASON;    /* rows are season numbers, not tracks */
#endif
    v->order = order;
    v->cls = cls;
    v->generation = gen;
    v->count = r;
    v->live_row = -1;
    for (i = 0; i < LIB_CACHE_ROWS; i++)
        v->cache_row[i] = -1;
    if (v->off_sub)
    {
        struct subcache *sc = view_ext(v, v->off_sub);
        for (i = 0; i < LIB_CACHE_ROWS; i++)
            sc->row[i] = -1;
    }
    if (v->off_unpl)
        memset(view_ext(v, v->off_unpl), 0, r);
    if (has_progress(cat, group))
        progress_compute(h, -1);    /* every row, in one pass */
    goto out;

changed:
    logf("lib: database changed while building");
    slot_free(h);
    h = -3;
out:
    if (boost)
    {
        cpu_boost(false);
    }
    return h;
}

#ifdef SIMULATOR
static int lib_opens;                   /* lib_open() calls, for traces */

int lib_open_calls(void)
{
    return lib_opens;
}
#endif

enum lib_state lib_state(void)
{
    if (tagcache_ui_count() >= 0)
        return LIB_READY;
    if (!tagcache_is_initialized() || tagcache_get_commit_step() > 0
        || tagcache_ui_busy())
        return LIB_UPDATING;
    return LIB_UNAVAILABLE;     /* RAM cache off or out of memory */
}

int lib_generation(void)
{
    /* all only ever grow: the sum changes when any does */
    return tagcache_ui_generation() + pre_files + book_order_gen;
}

int lib_pin_generation(void)
{
#ifdef LIB_PIN
    uint32_t gen;

    ab_store_pin_key(&gen);
    return (int)gen;
#else
    return 0;
#endif
}

void lib_set_book_order(int order)
{
    order = order == LIB_BOOKS_BY_AUTHOR ? LIB_BOOKS_BY_AUTHOR
                                         : LIB_BOOKS_BY_TITLE;
    if (order == book_order)
        return;
    book_order = order;
    book_order_gen++;           /* an open books list reopens */
}

static int lib_open_held(enum lib_cat cat, const struct lib_filter *f)
{
    unsigned long start UNUSED_ATTR = open_us();
    unsigned long t0 UNUSED_ATTR = now_us();
    const char *src = "scan";
    int h;
#ifdef HAVE_DEVSTATS
    uint32_t cmds0 UNUSED_ATTR;  /* logf off: the trace is empty */
/* the caller's devstats slot: THREAD_ID_SLOT() of kernel/thread-internal.h,
 * which apps/ does not include */
#define LIB_SELF_CMDS() (devstats.t[thread_self() & 0xff].cmds)
#define LIB_CMDS_FMT    " cmds=%lu"
#define LIB_CMDS_ARG    , (unsigned long)(LIB_SELF_CMDS() - cmds0)
#else
#define LIB_CMDS_FMT    ""
#define LIB_CMDS_ARG
#endif

#ifdef SIMULATOR
    lib_opens++;
#endif
    if ((unsigned)cat > LIB_PODCASTS
#ifdef HAVE_BCMFW_VIDEO
        && (cat < LIB_MOVIES || cat > LIB_TVEPISODES)
#endif
        )
        return -1;
    yields = 0;
#ifdef HAVE_DEVSTATS
    /* #47: storage commands of this open (sim: reads and opens) */
    cmds0 = LIB_SELF_CMDS();
#endif
    h = build(cat, f, &src);
    if (h <= 0)
    {
        UPDATE_TRACE("lib open %s failed %d", cat_names[cat], h);
        return h < 0 ? h : -1;
    }
    UPDATE_TRACE("lib open %s n=%d us=%lu yields=%d src=%s el_us=%lu"
                 LIB_CMDS_FMT,
                 cat_names[cat], slots[h - 1].count, open_us() - start,
                 yields, src, now_us() - t0 LIB_CMDS_ARG);
    return h;
}

#ifdef SIMULATOR
/* #47 check (sim only, PROKBOX_ARTALB_CHECK set in the environment): at
 * the first artist's albums opened, every artist of the open Artists list
 * (art_view) is opened twice, from its range and by the scan, and rows,
 * counts and keys compared. Logs "lib artalb check artists= albums=
 * slices= bad=" (tests/perf/artalb_check.sh wants slices = artists and
 * bad=0). */
static void lib_artalb_check(void)
{
    static bool done;
    int i, k, bad = 0, total = 0, rows = 0, slices = 0, ah = art_view;
    uint32_t *ref = NULL;

    if (done || !getenv("PROKBOX_ARTALB_CHECK") || !get_view(ah))
        return;
    done = true;
    for (i = 0; get_view(ah) && i < get_view(ah)->count; i++)
    {
        struct lib_filter f = LIB_FILTER_ANY;
        const char *src;
        struct view *v;
        int h, n;

        f.artist = get_view(ah)->rows[i];
        h = build(LIB_ALBUMS, &f, &src);
        total++;
        if (h <= 0)
        {
            bad++;
            continue;
        }
        slices += !strcmp(src, "slice");
        v = get_view(h);
        n = v->count;
        free(ref);
        ref = malloc((size_t)(n + 1) * 16);
        for (k = 0; ref && k < n; k++)
        {
            ref[4 * k] = v->rows[k];
            ref[4 * k + 1] = ((uint32_t *)view_ext(v, v->off_cnt))[k];
            ref[4 * k + 2] = ((uint32_t *)view_ext(v, v->off_key))[2 * k];
            ref[4 * k + 3] = ((uint32_t *)view_ext(v, v->off_key))[2 * k + 1];
        }
        slot_free(h);
        artalb_scan_only = true;
        h = build(LIB_ALBUMS, &f, &src);
        artalb_scan_only = false;
        v = h > 0 ? get_view(h) : NULL;
        rows += n;
        if (!ref || !v || v->count != n)
            bad++;
        else
            for (k = 0; k < n; k++)
            {
                const uint32_t *c = view_ext(v, v->off_cnt);
                const uint32_t *y = view_ext(v, v->off_key);

                if (ref[4 * k] != (uint32_t)v->rows[k] || ref[4 * k + 1] != c[k]
                    || ref[4 * k + 2] != y[2 * k]
                    || ref[4 * k + 3] != y[2 * k + 1])
                {
                    UPDATE_TRACE("lib artalb check differs artist=%d row=%d",
                                 i, k);
                    bad++;
                    break;
                }
            }
        if (h > 0)
            slot_free(h);
    }
    free(ref);
    UPDATE_TRACE("lib artalb check artists=%d albums=%d slices=%d bad=%d",
                 total, rows, slices, bad);
}
#endif

int lib_open(enum lib_cat cat, const struct lib_filter *f)
{
    struct tagcache_context *held = tagcache_context_acquire();
    int result = lib_open_held(cat, f);
#ifdef SIMULATOR
    if (cat == LIB_ALBUMS && f && f->artist != LIB_ANY)
        lib_artalb_check();
#endif
    tagcache_context_release(held);
    return result;
}

void lib_close(int view)
{
    slot_free(view);
}

int lib_count(int view)
{
    if (view <= 0 || view > MAX_VIEWS || !slots[view - 1].size)
        return 0;
    return slots[view - 1].count;
}

#ifdef LIB_P11
/* Unresolved On-The-Go rows carry their name's offset with this flag */
#define OTG_ROW_UNRES   LIB_OTG_UNRES

/* A Search row's label: what it lists and sorts by, per kind */
static const char *search_label(uint32_t row)
{
    int i = SRCH_ROW_IDX(row);

    switch (SRCH_ROW_KIND(row))
    {
    case SRCH_ARTIST:   return label_of(tagcache_context_active(), G_ARTIST, i);
    case SRCH_ALBUM:
    case SRCH_BOOK:     return label_of(tagcache_context_active(), G_ALBUM, i);
    default:            return label_of(tagcache_context_active(), G_NONE, i);
    }
}
#endif

/* The label of row idx (RAM cache, or an On-The-Go name in the view). */
static const char *row_label(struct view *v, int idx)
{
#ifdef LIB_P11
    int32_t r = v->rows[idx];

    if (v->cat == LIB_SEARCH)
        return search_label((uint32_t)r);
    if (v->cat == LIB_OTG && (r & OTG_ROW_UNRES))
        return (const char *)v + v->off_names + (r & ~OTG_ROW_UNRES);
#endif
    return label_of(tagcache_context_active(), v->group, v->rows[idx]);
}

/* The track (master index) row idx stands for, or -1: a group row, an
 * unresolved On-The-Go row. */
static int row_track(struct view *v, int idx)
{
    int32_t r = v->rows[idx];

#ifdef LIB_P11
    if (v->cat == LIB_SEARCH)
    {
        int k = SRCH_ROW_KIND((uint32_t)r);
        return k == SRCH_SONG || k == SRCH_EPISODE
            ? SRCH_ROW_IDX((uint32_t)r) : -1;
    }
    if (v->cat == LIB_OTG)
        return (r & OTG_ROW_UNRES) ? -1 : r;
#endif
    return v->group == G_NONE ? r : -1;
}

const char *lib_label(int view, int idx, char *buf, size_t len)
{
    struct view *v = get_view(view);
    const char *p;
    int slot;

    if (!buf || len == 0)
        return "";
    buf[0] = '\0';
    if (!v || idx < 0 || idx >= v->count)
        return buf;

    /* a stale view still shows the labels it has (the rows on screen):
     * a commit takes seconds, the screen reopens the view after it */
    slot = idx % LIB_CACHE_ROWS;
    if (v->cache_row[slot] == idx)
    {
        strlcpy(buf, v->cache[slot], len);
        return buf;
    }
    if (!view_fresh(v)
#ifdef LIB_P11
        && !(v->cat == LIB_OTG && (v->rows[idx] & OTG_ROW_UNRES))
#endif
        )
        return buf;

    fetches++;
#ifdef HAVE_BCMFW_VIDEO
    if (v->group == G_SEASON)
    {
        /* the number: the screen says "Season %d" with lib_id() */
        snprintf(buf, len, "%d", (int)v->rows[idx]);
        return buf;
    }
#endif
    p = row_label(v, idx);
    if (!p)
        p = "";
#ifdef HAVE_BCMFW_VIDEO
    if (v->cat == LIB_TVEPISODES)
    {
        /* "%d. Title" (3.6); an episode without a number: the title */
        const int32_t *s = tagcache_ui_seeks(v->rows[idx], NULL);

        if (s && s[tag_tracknumber] > 0)
        {
            snprintf(buf, len, "%ld. %s", (long)s[tag_tracknumber], p);
            if (strlen(buf) < LIB_CACHE_LEN)
            {
                strcpy(v->cache[slot], buf);
                v->cache_row[slot] = idx;
            }
            return buf;
        }
    }
#endif
    /* for check_trace.py: no book or podcast in a music list */
    UPDATE_TRACE("lib row %s %s", cat_names[v->cat], p);
    if (strlen(p) < LIB_CACHE_LEN)
    {
        strcpy(v->cache[slot], p);
        v->cache_row[slot] = idx;
    }
    strlcpy(buf, p, len);
    return buf;
}

const char *lib_sublabel(int view, int idx, char *buf, size_t len)
{
    struct view *v = get_view(view);
    struct subcache *sc;
    const int32_t *s;
    const char *p;
    int slot, t;

    int sub;

    if (!v || !v->off_sub || !buf || len == 0)
        return NULL;
    buf[0] = '\0';
    if (idx < 0 || idx >= v->count)
        return buf;
    sub = v->sub;
#ifdef LIB_P11
    if (v->cat == LIB_SEARCH)
    {
        static const int8_t sub_of_kind[SRCH_KINDS] = {
            SUB_NONE, SUB_ALBUM_ARTIST, SUB_TRACK_ARTIST, SUB_ALBUM_ARTIST,
            SUB_SHOW, SUB_NONE,
        };
        sub = sub_of_kind[SRCH_ROW_KIND((uint32_t)v->rows[idx])];
        if (sub == SUB_NONE)
            return NULL;                /* an artist row: one line */
    }
    else if (v->cat == LIB_OTG && (v->rows[idx] & OTG_ROW_UNRES))
        return buf;                     /* not in the database */
#endif
    sc = view_ext(v, v->off_sub);
    slot = idx % LIB_CACHE_ROWS;
    if (sc->row[slot] == idx)
    {
        strlcpy(buf, sc->s[slot], len);
        return buf;
    }
    if (!view_fresh(v))
        return buf;
    t = v->rows[idx];
#ifdef LIB_P11
    if (v->cat == LIB_SEARCH)
        t = SRCH_ROW_IDX((uint32_t)t);
#endif
    s = tagcache_ui_seeks(t, NULL);
    p = NULL;
    if (s && sub == SUB_TRACK_ARTIST)
    {
        p = str_of(tagcache_context_active(), s, tag_artist);
        if (!p)
            p = str_of(tagcache_context_active(), s, tag_albumartist);
    }
    else if (s && sub == SUB_SHOW)
        p = str_of(tagcache_context_active(), s, tag_album);
#ifdef HAVE_BCMFW_VIDEO
    else if (sub == SUB_DURATION)
    {
        /* h:mm:ss or m:ss, digits only (no language string needed) */
        long sec = s && s[tag_length] > 0 ? s[tag_length] / 1000 : 0;

        if (sec >= 3600)
            snprintf(buf, len, "%ld:%02ld:%02ld", sec / 3600,
                     sec / 60 % 60, sec % 60);
        else
            snprintf(buf, len, "%ld:%02ld", sec / 60, sec % 60);
        if (strlen(buf) < LIB_SUB_LEN)
        {
            strcpy(sc->s[slot], buf);
            sc->row[slot] = idx;
        }
        return buf;
    }
#endif
    else if (s)
        p = eff_artist(tagcache_context_active(), s, &t);
    if (!p)
        p = "";                 /* untagged: the caller says so (lang) */
    if (strlen(p) < LIB_SUB_LEN)
    {
        strcpy(sc->s[slot], p);
        sc->row[slot] = idx;
    }
    strlcpy(buf, p, len);
    return buf;
}

int lib_id(int view, int idx)
{
    struct view *v = get_view(view);

    if (!v || idx < 0 || idx >= v->count || !view_fresh(v))
        return -2;              /* matches nothing (LIB_ANY is -1) */
#ifdef LIB_P11
    if (v->cat == LIB_OTG && (v->rows[idx] & OTG_ROW_UNRES))
        return -2;              /* not in the database */
#endif
    return v->rows[idx];
}

int lib_find(int view, int id)
{
    struct view *v = get_view(view);
    int i;

    if (!v || id < 0 || !view_fresh(v))
        return -1;
    for (i = 0; i < v->count; i++)
        if (v->rows[i] == id)
            return i;
    return -1;
}

char lib_letter(int view, int idx)
{
    struct view *v = get_view(view);

    if (!v || idx < 0 || idx >= v->count || !view_fresh(v))
        return '#';
    if (v->pinned && idx == 0)  /* #67: the pinned book has none */
        return 0;
    if (v->by_author)           /* #59: the author's letter */
        return ui_sortletter(label_of(tagcache_context_active(), G_ARTIST, v->rows[idx]));
#ifdef HAVE_BCMFW_VIDEO
    if (v->group == G_SEASON)
        return 0;               /* a few numbered rows: no letters */
#endif
    return ui_sortletter(row_label(v, idx));
}

int lib_child_count(int view, int idx)
{
    struct view *v = get_view(view);
    uint32_t c;

    if (!v || !v->off_cnt || idx < 0 || idx >= v->count)
        return -1;
    c = ((uint32_t *)view_ext(v, v->off_cnt))[idx];
    return c > INT_MAX ? -1 : (int)c;
}

uint64_t lib_art_key(int view, int idx)
{
    struct view *v = get_view(view);
    const uint32_t *k;

    if (!v || !v->off_key || idx < 0 || idx >= v->count)
        return 0;
    k = view_ext(v, v->off_key);
    return (uint64_t)k[2 * idx + 1] << 32 | k[2 * idx];
}

void lib_trace_frame(void)
{
    if (fetches)
    {
        UPDATE_TRACE("lib fetch n=%d", fetches);
        fetches = 0;
    }
}

/* -- progress and resume ------------------------------------------------ */

#ifdef HAVE_BCMFW_VIDEO
/* Master entry idx is a video (its flag word; RAM, no I/O, no yield) */
static bool row_is_video(int idx)
{
    int32_t flag;

    return tagcache_ui_seeks(idx, &flag) && (flag & TAGCACHE_UI_VIDEO);
}

bool lib_is_video(int view, int idx)
{
    struct view *v = get_view(view);
    int t;

    if (!v || idx < 0 || idx >= v->count || !view_fresh(v))
        return false;
    t = row_track(v, idx);
    return t >= 0 && row_is_video(t);
}
#endif

/* The track to resume in a book or show (rows of track view h): the last
 * one, in list order, with a saved position. Returns its row, or -1 (also
 * if the database changed meanwhile). */
static int resume_row(int h, unsigned long *elapsed, unsigned long *offset)
{
    int gen = tagcache_ui_generation();
    int i, idx;

    for (i = lib_count(h) - 1; i >= 0; i--)
    {
        long e, o;
        struct view *v = get_view(h);   /* again after each stat_of() */

        if (!v)
            return -1;
        idx = v->rows[i];
#ifdef HAVE_BCMFW_VIDEO
        if (row_is_video(idx))
            continue;           /* an audio queue never starts on one */
#endif
        e = stat_of(idx, tag_lastelapsed);
        if (tagcache_ui_generation() != gen)
            return -1;
        if (e > 0)
        {
            o = stat_of(idx, tag_lastoffset);
            *elapsed = e;
            *offset = o > 0 ? o : 0;
            return i;
        }
    }
    return -1;
}

static int fraction(long done, long total)
{
    if (total <= 0 || done < 0)
        return -1;
    if (done >= total)
        return 1000;
    return (int)((uint64_t)done * 1000 / (uint64_t)total);
}

/* The track playing (or paused) now, as a master index, and its
 * position: a book being listened to shows where it is, not where it last
 * ended (pausing saves nothing). -1 if none or not from the database. */
static int playing_idx(long *elapsed)
{
    struct mp3entry *id3 = audio_status() ? audio_current_track() : NULL;

    if (!id3 || id3->tagcache_idx <= 0 || id3->elapsed == 0)
        return -1;
    *elapsed = id3->elapsed;
    return id3->tagcache_idx - 1;
}

/* The track loaded in playback (playing or paused), whatever its
 * position, or -1: "not playing now" of the podcast dot (P11 3.8: an
 * episode just started, still at 0 ms, is no longer unplayed). */
static int loaded_idx(void)
{
    struct mp3entry *id3 = audio_status() ? audio_current_track() : NULL;

    return id3 && id3->tagcache_idx > 0 ? (int)id3->tagcache_idx - 1 : -1;
}

/* An episode never played: no play counted, no saved position, and not
 * the one loaded now. May block (stat_of()). */
static bool episode_unplayed(int idx, int np)
{
    if (idx == np)
        return false;
    return stat_of(idx, tag_playcount) <= 0
        && stat_of(idx, tag_lastelapsed) <= 0;
}

/* Per group row while progress is computed (temporary, in the pool). */
struct gacc {
    uint32_t len_all;           /* ms, all tracks */
    uint32_t len_before;        /* ms, tracks before the resume track */
    int32_t  cur;               /* resume track (master index) or -1 */
    int32_t  elapsed;           /* its position, ms */
    int32_t  unplayed;          /* an episode never played */
};

/* Temporary block of progress_compute(): 'm' identity records (k1/k2 =
 * group_ident(), idx = group row), sorted, then n accumulators. */
struct ptmp {
    int m;
    int pad;
    struct rec key[];
};

static struct gacc *ptmp_acc(struct ptmp *t)
{
    return (struct gacc *)&t->key[t->m];
}

/* The group row of book/show track i (class already checked), rows g0 to
 * g1 - 1 only: exact identity first (binary search), then, for a case
 * variant that step 4 merged into another row, a linear check. -1 if
 * none. */
static int group_of(struct bctx *c, struct view *v, struct ptmp *t, int g0,
                    int g1, int i, const int32_t *s, int32_t flag)
{
    struct rec k;
    int lo = 0, hi = t->m, g;

    k.idx = i;
    group_ident(tagcache_context_active(), c, &k, G_ALBUM, s, flag);
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        const struct rec *r = &t->key[mid];

        if (r->k1 == k.k1 && r->k2 == k.k2)
            return r->idx;
        if (r->k1 < k.k1 || (r->k1 == k.k1 && r->k2 < k.k2))
            lo = mid + 1;
        else
            hi = mid;
    }
    for (g = g0; g < g1; g++)
        if (same_group(tagcache_context_active(), G_ALBUM, i, v->rows[g]))
            return g;
    return -1;
}

/* Listened fractions of a book or show list's rows (all with only < 0, or
 * row 'only'), into view_progress(), and for shows whether an episode is
 * unplayed: the resume point (the track playing, else the last one in
 * disc/track order with a saved position) within the sum of the group's
 * track lengths. Two passes over the master index for all rows together
 * (they used to be two per row, while drawing). Yields every YIELD_EVERY
 * entries; stat_of() may block too: pointers are fetched again after
 * each, and a database change gives up (the view is stale anyway, -1
 * everywhere). */
static void progress_compute(int h, int only)
{
    struct view *v = get_view(h);
    struct ptmp *t;
    struct gacc *a;
    struct bctx c;
    int n = v->count, cls = v->cls, gen = v->generation;
    int g0 = only < 0 ? 0 : only, g1 = only < 0 ? n : only + 1;
    int m = g1 - g0, total = tagcache_ui_count(), th, pass, i, g;
    long np_elapsed = 0;
    int np = playing_idx(&np_elapsed), ld = loaded_idx();
    bool shows = v->off_unpl != 0;

    for (g = g0; g < g1; g++)
        view_progress(v)[g] = -1;
    if (m <= 0 || total < 0)
        return;
    th = slot_new(ALIGN_UP(sizeof(struct ptmp) + m * sizeof(struct rec)
                           + m * sizeof(struct gacc), 4));
    if (th <= 0)
    {
        UPDATE_TRACE("lib alloc failed what=%s bytes=%d", "progress", 0);
        return;
    }

    /* identities of the rows' representatives, sorted */
    memset(&c, 0, sizeof(c));
    c.gen = gen;
    c.h = th;
    c.pool = true;
    c.memo.generation = -1;
    c.group = G_NONE;
    c.order = -1;
    v = get_view(h);                    /* slot_new() may move the pool */
    t = (struct ptmp *)get_view(th);
    t->m = m;
    for (g = g0; g < g1; g++)
    {
        int32_t flag;
        const int32_t *s = tagcache_ui_seeks(v->rows[g], &flag);
        struct rec *k = &t->key[g - g0];

        k->k1 = k->k2 = 0xffffffffu;    /* gone: matches nothing */
        k->idx = g;
        if (s)
            group_ident(tagcache_context_active(), &c, k, G_ALBUM, s, flag);
    }
    if (!sort_recs(tagcache_context_active(), &c, offsetof(struct ptmp, key), m))
        goto changed;
    yields += c.yields;
    a = ptmp_acc((struct ptmp *)get_view(th));
    for (g = 0; g < m; g++)
    {
        a[g].len_all = a[g].len_before = 0;
        a[g].cur = -1;
        a[g].elapsed = 0;
        a[g].unplayed = 0;
    }

    /* pass 0: lengths and resume tracks; pass 1: lengths before them */
    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < total; i++)
        {
            int32_t flag;
            const int32_t *s;
            uint32_t len;
            long e;
            bool unpl = false;

            if (i % YIELD_EVERY == YIELD_EVERY - 1)
            {
                yield();
                yields++;
                if (tagcache_ui_generation() != gen
                    || tagcache_ui_count() < 0)
                    goto changed;
            }
            s = tagcache_ui_seeks(i, &flag);
            if (!s || !class_ok(cls, flag))
                continue;
            v = get_view(h);
            t = (struct ptmp *)get_view(th);
            g = group_of(&c, v, t, g0, g1, i, s, flag);
            if (g < 0)
                continue;
            len = s[tag_length] > 0 ? s[tag_length] : 0;
            if (pass == 1)
            {
                a = &ptmp_acc(t)[g - g0];
                if (a->cur >= 0 && i != a->cur && cmp_track_no(tagcache_context_active(), i, a->cur) < 0)
                    a->len_before += len;
                continue;
            }
            /* no RAM cache pointer is used past stat_of() */
            e = i == np ? np_elapsed : stat_of(i, tag_lastelapsed);
            if (shows && e <= 0 && i != np && i != ld)
                unpl = stat_of(i, tag_playcount) <= 0;
            if (tagcache_ui_generation() != gen || tagcache_ui_count() < 0)
                goto changed;
            a = &ptmp_acc((struct ptmp *)get_view(th))[g - g0];
            a->len_all += len;
            a->unplayed |= unpl;
            if (e > 0 && (a->cur < 0 || i == np
                          || (a->cur != np && cmp_track_no(tagcache_context_active(), i, a->cur) > 0)))
            {
                a->cur = i;
                a->elapsed = e;
            }
        }
    }

    v = get_view(h);
    a = ptmp_acc((struct ptmp *)get_view(th));
    for (g = g0; g < g1; g++, a++)
    {
        if (shows)
            ((uint8_t *)view_ext(v, v->off_unpl))[g] = a->unplayed != 0;
        if (a->cur < 0)
            continue;
        view_progress(v)[g] = fraction((long)a->len_before + a->elapsed,
                                       a->len_all);
        if (a->cur == np)
            v->live_row = g;
    }
changed:
    slot_free(th);
}

int lib_progress(int view, int idx)
{
    struct view *v = get_view(view);
    const int32_t *s;
    long e;
    int rep;

    if (!v || idx < 0 || idx >= v->count || !view_fresh(v))
        return -1;
#ifdef HAVE_BCMFW_VIDEO
    /* a movie or an episode: its own position; a show: computed at
     * lib_open(); a season: none */
    if (v->group == G_SEASON || (v->group != G_NONE && !v->off_prog))
        return -1;
    if (v->cls != C_BOOK && v->cls != C_PODCAST
        && v->cls != C_MOVIE && v->cls != C_TV)
        return -1;
#else
    if (v->cls != C_BOOK && v->cls != C_PODCAST)
        return -1;
#endif
    if (v->group != G_NONE)
        return view_progress(v)[idx];   /* computed at lib_open() */

    /* an episode or a book's track: one entry */
    rep = v->rows[idx];
    if (playing_idx(&e) != rep)
        e = stat_of(rep, tag_lastelapsed);  /* v is not used after this */
    s = tagcache_ui_seeks(rep, NULL);
    if (e <= 0 || !s)
        return -1;
    return fraction(e, s[tag_length]);
}

bool lib_unplayed(int view, int idx)
{
    struct view *v = get_view(view);

#ifdef HAVE_BCMFW_VIDEO
    /* a movie or an episode: as a podcast episode; a show: computed at
     * lib_open() (progress_compute()); a season: none */
    if (v && (v->cls == C_MOVIE || v->cls == C_TV))
    {
        if (idx < 0 || idx >= v->count || !view_fresh(v))
            return false;
        if (v->group == G_NONE)
            return episode_unplayed(v->rows[idx], loaded_idx());
        return v->group == G_ALBUM && v->off_unpl
            && ((uint8_t *)view_ext(v, v->off_unpl))[idx];
    }
#endif
    if (!v || idx < 0 || idx >= v->count || !view_fresh(v)
        || v->cls != C_PODCAST)
        return false;
    if (v->group != G_NONE)             /* a show: computed at lib_open() */
        return v->off_unpl && ((uint8_t *)view_ext(v, v->off_unpl))[idx];
    return episode_unplayed(v->rows[idx], loaded_idx());
}

static void lib_progress_refresh_held(int view)
{
    struct view *v = get_view(view);
    const int32_t *s;
    int32_t flag;
    long e;
    int np, g, live;

    if (!v || !view_fresh(v) || !has_progress(v->cat, v->group))
        return;
    /* the row that showed a live position, and the row playing now */
    live = v->live_row;
    v->live_row = -1;
    if (live >= 0)
        progress_compute(view, live);
    np = playing_idx(&e);
    if (np < 0)
        np = loaded_idx();      /* just started: its dot goes (P11 3.8) */
    v = get_view(view);
    s = np >= 0 ? tagcache_ui_seeks(np, &flag) : NULL;
    if (!v || !s || !class_ok(v->cls, flag))
        return;
    for (g = 0; g < v->count; g++)
    {
        if (same_group(tagcache_context_active(), G_ALBUM, np, v->rows[g]))
        {
            if (g != live)
                progress_compute(view, g);
            return;
        }
    }
}

void lib_progress_refresh(int view)
{
    struct tagcache_context *held = tagcache_context_acquire();
    lib_progress_refresh_held(view);
    tagcache_context_release(held);
}

/* Album groups of the queue lib_play() built last (Revision 1 item 10):
 * bit k set = queue entry k starts a new album (insert order). Valid
 * while the playlist is that one (created_tick) with that many entries.
 * #109: the bits are in the queue's slot table (dbq_groups(), n/8 B, any
 * length); a path queue (the fallback) keeps none, and Albums then plays
 * it in order. */
static int grp_n = -1;
static unsigned long grp_tick;
/* ... and that queue is one audiobook (Revision 2: it ends by stopping) */
static bool grp_book;
/* ... or the queue a reboot resumed was one (the shuffle record's flag) */
static int book_n = -1;
static unsigned long book_tick;
/* ... and its book map is owed (P6 Rev 1 item 8: refilled once audio
 * plays, np_pb_idle() -> lib_book_resumed_idle()) */
static bool resumed_map_owed;

static void grp_note(int k, int entry, int32_t *prev_album,
                     int32_t *prev_aa)
{
#ifdef LIB_DBQ
    const int32_t *sk = tagcache_ui_seeks(entry, NULL);
    int32_t al = sk ? sk[tag_album] : -1, aa = sk ? sk[tag_albumartist] : -1;
    int n;
    uint8_t *bits = dbq_groups(&n, NULL);

    if (!bits || k >= n)
        return;
    if (k == 0 || al != *prev_album || aa != *prev_aa)
        bits[k >> 3] |= 1u << (k & 7);
    else
        bits[k >> 3] &= ~(1u << (k & 7));
    *prev_album = al;
    *prev_aa = aa;
#else
    (void)k; (void)entry; (void)prev_album; (void)prev_aa;
#endif
}

/* -- playback ----------------------------------------------------------- */

/* Scratch for building a playlist: the search (for file names) and a
 * path. Pinned while used: the tagcache keeps a pointer to it. */
struct play_scratch {
    struct tagcache_search tcs;
    char path[MAX_PATH];
};

static struct play_scratch *scratch_of(int h)
{
    struct play_scratch *ps = (struct play_scratch *)get_view(h);

    CHECK_ALIGN(ps);
    return ps;
}

/* Stacks the scratch on the pool and pins the pool (the tagcache keeps
 * a pointer to the search). Returns its handle or -1. */
static int play_scratch_open(void)
{
    int h;

    pool_sync();
    h = slot_new(ALIGN_UP(sizeof(struct play_scratch), 4));
    if (h <= 0)
        return -1;
    core_pin(pool.handle);
    if (!tagcache_search(&scratch_of(h)->tcs, tag_filename))
    {
        core_unpin(pool.handle);
        slot_free(h);
        return -1;
    }
    return h;
}

static void play_scratch_close(int h)
{
    tagcache_search_finish(&scratch_of(h)->tcs);
    core_unpin(pool.handle);
    slot_free(h);
}

/* Appends master entry 'idx' to the playlist being built. */
static bool play_add(int sh, struct playlist_insert_context *c, int idx)
{
    struct play_scratch *ps = scratch_of(sh);       /* pinned */

    if (!tagcache_retrieve(&ps->tcs, idx, tag_filename, ps->path,
                           sizeof(ps->path)))
        return false;
    return playlist_insert_context_add(c, ps->path) >= 0;
}

#ifdef LIB_BOOKS
/* P6: a book queue being built (lib_play() of a book, the page) */
struct book_ctx {
    ab_key_t key;               /* 0: from the first track */
    bool use_store;             /* resume at books.dat's spot */
    int  entry_q;               /* >= 0: start this file at entry_ms */
    unsigned long entry_ms;
    const struct ab_blob_view *page;    /* the opened page's blob */
    struct ab_book_map *map;    /* filled while adding */
    uint32_t len;               /* running length (RAM database) */
    int32_t rep;                /* a track of the book (#47), or -1 */
};

static ab_key_t book_key_of(int rep, const char *path)
{
    const int32_t *s = tagcache_ui_seeks(rep, NULL);
    const char *album = s ? str_of(tagcache_context_active(), s, tag_album) : NULL;
    ab_key_t key = 0;

    if (album)
        key = artcache_key(str_of(tagcache_context_active(), s, tag_albumartist), str_of(tagcache_context_active(), s, tag_artist),
                           album, NULL);
    if (!key && path)
        key = artcache_key(NULL, NULL, NULL, path);
    return key;
}

/* A book's record (ab_store_get()), or the one written under its pre-#47
 * key: read through, never moved here (the store's one pending slot is
 * the playing book's); the next checkpoint writes it under the new key,
 * and the old one ages out. */
static bool book_rec_get(int32_t rep, ab_key_t key, struct ab_rec *rec)
{
    ab_key_t old;

    if (ab_store_get(key, rec))
        return true;
    old = book_key_ascii(rep, key);
    if (!old || !ab_store_get(old, rec))
        return false;
    rec->book_key = key;
    return true;
}

static void book_note(struct book_ctx *bk, int k, int idx, const char *path)
{
    const int32_t *s = tagcache_ui_seeks(idx, NULL);
    uint32_t len = s && s[tag_length] > 0 ? (uint32_t)s[tag_length] : 0;

    if (k == 0 && !bk->key)
    {
        bk->key = book_key_of(idx, path);
        bk->rep = idx;
    }
    if (bk->map && k < bk->map->file_count)
    {
        bk->map->file_crc[k] = ab_path_crc(path);
        bk->map->file_start_ms[k] = bk->len;
    }
    bk->len += len;
}

/* The fallback map's starts again (a failed pak read wrote over them):
 * RAM only. */
static void book_starts(int vh, int first, int n, struct ab_book_map *m)
{
    uint32_t pos = 0;
    int k;

    for (k = 0; k < n && k < m->file_count; k++)
    {
        struct view *v = get_view(vh);
        const int32_t *s = v ? tagcache_ui_seeks(v->rows[first + k], NULL)
                             : NULL;

        m->file_start_ms[k] = pos;
        pos += s && s[tag_length] > 0 ? (uint32_t)s[tag_length] : 0;
    }
    m->len_ms = pos;
}

/* After the queue is built, before playback starts: the map, then where
 * to start (books.dat's spot, or an entry). */
static void book_start(struct book_ctx *bk, int vh, int first, int added,
                       int *pos, unsigned long *elapsed,
                       unsigned long *offset)
{
    struct ab_rec rec;
    int q;

    if (bk->map)
    {
        bk->map->book_key = bk->key;
        bk->map->old_key = book_key_ascii(bk->rep, bk->key);   /* #47 */
        if (added != bk->map->file_count || !bk->key)
            ab_map_begin(bk->key, 0);           /* traced "none" */
        else if (!ab_map_pak(bk->page))
        {
            book_starts(vh, first, added, bk->map);
            ab_map_fallback();
        }
    }
    if (bk->entry_q >= 0)
    {
        *pos = bk->entry_q < added ? bk->entry_q : 0;
        *elapsed = bk->entry_ms;
        *offset = 0;
        return;
    }
    *pos = 0;
    *elapsed = *offset = 0;
    if (!bk->use_store || !book_rec_get(bk->rep, bk->key, &rec)
        || ab_rec_state(&rec) != AB_REC_STARTED)
        return;                         /* unplayed or finished: from 0 */
    q = -1;
    if (bk->map && bk->map->file_count == added)
    {
        int k;
        for (k = 0; k < added && q < 0; k++)
            if (bk->map->file_crc[k] == rec.file_crc)
                q = k;
    }
    if (q < 0 && rec.file_idx < added)
        q = rec.file_idx;               /* rebuild-safe CRC missed */
    if (q < 0)
        return;
    *pos = q;
    *elapsed = rec.elapsed_ms;
    *offset = rec.offset;
    UPDATE_TRACE(AB_TR_RESUME, (unsigned long)(uint32_t)bk->key, q,
                 (unsigned long)rec.elapsed_ms);
}

#else
struct book_ctx;
#endif

/* Plays the rows of a track view from row 'start'. #109: the queue is
 * the rows' master indices (dbqueue.c): no name read and one control
 * line, up to the whole list; a book also reads its few names (its map
 * hashes them). Without memory for the slot table, the path queue. */
static int play_view(int vh, int start, bool shuffle,
                     unsigned long elapsed, unsigned long offset,
                     struct book_ctx *bk)
{
    struct playlist_insert_context c;
    struct view *v;
    unsigned long t0 UNUSED_ATTR = now_us();
    unsigned long c0 UNUSED_ATTR = cpu_us();
    int cap, n, first, last, need, left, i, sh = -1, pos = -1, added = 0;
    int gen = tagcache_ui_generation();
    int32_t g_album = -1, g_aa = -1;
    bool sample, dbq = false;
#ifdef LIB_DBQ
    const char *rsv UNUSED_ATTR = "ok";
    uint32_t tag;
#endif

    v = get_view(vh);
    if (!v)
        return -1;
    n = v->count;
    cap = playlist_get_current()->max_playlist_size;
    if (n <= 0 || start < 0 || start >= n || cap <= 0)
        return -1;

#ifdef LIB_DBQ
    /* H3 (Q-R4-7): a list longer than the queue holds, or a slot table
     * free heap can't hold, while audio keeps its buffer (a paused shrink
     * refuses this thread): stop the old track first. The queue is
     * replaced anyway; nothing paused is left to keep. audio_stop(), not
     * audio_hard_stop(): the latter frees the buffer without marking it
     * trashed, so the next play would run on freed memory; stopped, the
     * allocation's shrink gives the buffer up the proper way */
    if (audio_status()
        && core_allocatable() < lib_queue_need(n) + 1024)
    {
        /* review H-A: every allocation the new queue makes, summed
         * against the setting's capacity (playlist_create() shrinks back
         * to it first), so none of them shrinks playing audio */
        audio_stop();
        rsv = "stop";
    }
#endif
    if (playlist_create(NULL, NULL) < 0)
    {
        UPDATE_TRACE("lib play fail %s", "playlist");
        return -1;
    }
    cap = playlist_get_current()->max_playlist_size;
#ifdef LIB_DBQ
    if (n > cap)
    {
        /* a little headroom: an insert after it doesn't find it full */
        if (playlist_reserve_ipodui(n + LIB_QUEUE_SPARE, false) < 0)
        {
            rsv = "window";
            UPDATE_TRACE("dbq reserve fail want=%d got=%d", n, cap);
        }
        cap = playlist_get_current()->max_playlist_size;
    }
#endif

    /* which rows go in: all; or, past the queue's capacity (the reserve
     * failed), a window from 'start', or (shuffled) 'start' plus a random
     * sample of the rest */
    first = 0;
    last = n;
    sample = shuffle && n > cap;
    if (n > cap && !shuffle)
    {
        first = MIN(start, n - cap);
        last = first + cap;
    }
    need = cap - 1;             /* sampled rows besides 'start' */
    left = n - 1;               /* rows besides 'start' not yet passed */

#ifdef LIB_DBQ
    dbq = dbq_begin(MIN(n, cap));
    if (!dbq)
        rsv = "path";
#endif
    if (bk || !dbq)
    {
        sh = play_scratch_open();
        if (sh < 0)
        {
            UPDATE_TRACE("lib play fail %s", "search");
            return -1;
        }
    }
    if (!dbq && playlist_insert_context_create(NULL, &c, PLAYLIST_INSERT_LAST,
                                               false, false) < 0)
    {
        /* our own context with progress off: the default one splashes */
        playlist_insert_context_release(&c);    /* unlocks */
        play_scratch_close(sh);
        UPDATE_TRACE("lib play fail %s", "playlist");
        return -1;
    }

    srand(current_tick);
    for (i = first; i < last && added < cap; i++)
    {
        int row;

        if (tagcache_ui_generation() != gen)
            break;
        if (sample && i != start)
        {
            /* selection sampling (Knuth's algorithm S) */
            bool take = need > 0 && rand() % left < need;
            left--;
            if (!take)
                continue;
            need--;
        }
        v = get_view(vh);           /* pinned meanwhile, but cheap */
        row = v->rows[i];
#ifdef HAVE_BCMFW_VIDEO
        /* 3.2a (M10): an audio queue skips video rows (a podcast's video
         * episodes); a video plays alone through ui_video_play() */
        if (row_is_video(row))
            continue;
#endif
        if (!dbq)
        {
            if (!play_add(sh, &c, row))
                continue;
        }
#ifdef LIB_DBQ
        else if ((bk && !tagcache_retrieve(&scratch_of(sh)->tcs, row,
                                           tag_filename,
                                           scratch_of(sh)->path,
                                           sizeof(scratch_of(sh)->path)))
                 || dbq_add(row) < 0)
            continue;
#endif
#ifdef LIB_BOOKS
        if (bk)
            book_note(bk, added, row, scratch_of(sh)->path);
#endif
        if (i == start)
            pos = added;
        grp_note(added, row, &g_album, &g_aa);
        added++;
    }
#ifdef LIB_DBQ
    if (dbq)
    {
        unsigned long t1 UNUSED_ATTR = now_us();
        int cn = added > 0 ? dbq_commit(&tag) : -1;

        /* design 5.2: payload, dbq.hdr, then one T line */
        if (cn <= 0 || playlist_insert_dbq_ipodui(NULL, 0, cn, tag) < 0)
            added = 0;
        UPDATE_TRACE("dbq build n=%d bytes=%lu us=%lu reserve=%s", added,
                     (unsigned long)dbq_need(MIN(n, cap)),
                     now_us() - t1, rsv);
    }
    else
#endif
        playlist_insert_context_release(&c);
    if (sh >= 0)
        play_scratch_close(sh);
    grp_n = added;          /* groups: only with a slot table */
    grp_tick = playlist_get_current()->created_tick;
    grp_book = false;               /* lib_play() says when it is a book */

    if (added == 0)
    {
        UPDATE_TRACE("lib play fail %s", "empty");
        UPDATE_TRACE("lib play empty first=%d last=%d cap=%d dbq=%d gen=%d/%d",
                     first, last, cap, (int)dbq, gen,
                     tagcache_ui_generation());
        return -1;
    }
    if (pos < 0)
        pos = 0;
#ifndef LIB_BOOKS
    (void)bk;
#else
    if (bk && !shuffle)
    {
        book_start(bk, vh, first, added, &pos, &elapsed, &offset);
        /* before playback starts: playback.c's auto-resume asks
         * ab_shell_owns_resume() (Rev 1 item 1) */
        grp_book = true;
    }
#endif
    if (shuffle)
        pos = playlist_shuffle(current_tick, pos);
    UPDATE_TRACE("lib play n=%d us=%lu cpu_us=%lu", added,
                 now_us() - t0, cpu_us() - c0);
    playlist_start(pos, elapsed, offset);
    return 0;
}

static const char *lib_path_held(int view, int idx, char *buf, size_t len)
{
    struct view *v = get_view(view);
    struct play_scratch *ps;
    const char *ret = NULL;
    int sh, rep;

    if (!buf || len == 0)
        return NULL;
    if (!v || idx < 0 || idx >= v->count || !view_fresh(v)
        || (rep = row_track(v, idx)) < 0)
    {
        logf("lib: no path for view %d row %d", view, idx);
        return NULL;
    }
    sh = play_scratch_open();
    if (sh < 0)
        return NULL;
    ps = scratch_of(sh);
    if (tagcache_retrieve(&ps->tcs, rep, tag_filename, ps->path,
                          sizeof(ps->path)))
    {
        strlcpy(buf, ps->path, len);
        ret = buf;
    }
    else
        logf("lib: no path for entry %d", rep);
    play_scratch_close(sh);
    return ret;
}

const char *lib_path(int view, int idx, char *buf, size_t len)
{
    struct tagcache_context *held = tagcache_context_acquire();
    const char * result = lib_path_held(view, idx, buf, len);
    tagcache_context_release(held);
    return result;
}

#ifdef LIB_P11
static int search_play(int view, int idx, bool shuffle);
#endif

static int lib_play_held(int view, int idx, bool shuffle)
{
    struct view *v = get_view(view);
    unsigned long elapsed = 0, offset = 0;
    const char *src;
    bool resume, v_is_book;
    int rc, child, start;

#ifdef LIB_P11
    if (v && v->cat == LIB_OTG)
        return otg_play(idx);           /* the store's own paths */
#endif
    if (!v || idx < 0 || idx >= v->count || !view_fresh(v))
        return -1;
#ifdef LIB_P11
    if (v->cat == LIB_SEARCH)
        return search_play(view, idx, shuffle);
#endif
#ifdef HAVE_BCMFW_VIDEO
    /* a video row, a show or a season: the video screen's, never an
     * audio queue (screens/video.h ui_video_play()) */
    if (v->cls == C_MOVIE || v->cls == C_TV
        || (v->group == G_NONE && row_is_video(v->rows[idx])))
    {
        UPDATE_TRACE("lib play fail %s", "video");
        return -1;
    }
#endif
    resume = v->cls == C_BOOK || v->cls == C_PODCAST;
    v_is_book = v->cls == C_BOOK;

    if (v->group == G_NONE)
    {
        /* all songs, shuffled, more than the queue holds: a library
         * source (#27) instead of a sample */
        if (shuffle && v->cat == LIB_SONGS && v->cls == C_MUSIC
            /* #109 S2 (M2): the setting, not the queue's (now mutable)
             * capacity */
            && v->count > LIB_QUEUE_SETTING
            && v->count >= tagcache_ui_count() / 2
            && lib_shuffle_start(NP_SHUFFLE_SONGS, v->rows[idx]) == 0)
            return 0;
#ifdef LIB_BOOKS
        if (v_is_book && !shuffle)
        {
            /* a book's track list: the book queue from this file, at
             * books.dat's spot if it is that file (P6) */
            struct book_ctx bk = { 0, false, idx, 0, NULL, NULL, 0, -1 };
            bk.map = ab_map_begin(1, v->count);   /* key: the first track */
            rc = play_view(view, idx, false, 0, 0, &bk);
            grp_book = rc == 0;
            if (rc == 0)
                lib_shuffle_note_book(true);
            return rc;
        }
#endif
        if (resume)
        {
            int rep = v->rows[idx];     /* v is not used after stat_of() */
            long e = stat_of(rep, tag_lastelapsed);
            long o = stat_of(rep, tag_lastoffset);
            elapsed = e > 0 ? e : 0;
            offset = (e > 0 && o > 0) ? o : 0;
        }
        rc = play_view(view, idx, shuffle, elapsed, offset, NULL);
        grp_book = rc == 0 && v_is_book;
        if (rc == 0)
            lib_shuffle_note_book(grp_book);
        return rc;
    }

    /* a group row: all of its tracks */
    {
        struct lib_filter f = LIB_FILTER_ANY;
        int rep = v->rows[idx];

        switch (v->group)
        {
        case G_ARTIST:   f.artist = rep; break;
        case G_ALBUM:    f.album = rep; break;
        case G_GENRE:    f.genre = rep; break;
        default:         f.composer = rep; break;
        }
        child = build(LIB_SONGS, &f, &src);
    }
    if (child <= 0)
        return -1;
    start = 0;
#ifdef LIB_BOOKS
    if (v_is_book && !shuffle)
    {
        /* P6: books.dat is the only resume authority for books */
        struct book_ctx bk = { 0, true, -1, 0, NULL, NULL, 0, -1 };
        bk.key = lib_art_key(view, idx);
        bk.rep = lib_id(view, idx);
        bk.map = ab_map_begin(bk.key ? bk.key : 1, lib_count(child));
        rc = play_view(child, 0, false, 0, 0, &bk);
        grp_book = rc == 0;
        if (rc == 0)
            lib_shuffle_note_book(true);
        slot_free(child);
        return rc;
    }
#endif
    if (resume)
    {
        int r = resume_row(child, &elapsed, &offset);
        if (r >= 0)
            start = r;
    }
    rc = play_view(child, start, shuffle, elapsed, offset, NULL);
    grp_book = rc == 0 && v_is_book;
    if (rc == 0)
        lib_shuffle_note_book(grp_book);
    slot_free(child);
    return rc;
}

int lib_play(int view, int idx, bool shuffle)
{
    struct tagcache_context *held = tagcache_context_acquire();
    int result = lib_play_held(view, idx, shuffle);
    tagcache_context_release(held);
    return result;
}

int lib_shuffle_all(void)
{
    /* the whole library, every song once per lap (#27, shuffle.md) */
    return lib_shuffle_start(NP_SHUFFLE_SONGS, SHUF_NONE);
}

/* -- Phase 5: album groups of the queue, the library source ------------- */

bool lib_queue_groups(const uint8_t **bits, int *n)
{
#ifdef LIB_DBQ
    const struct playlist_info *pl = playlist_get_current();
    int dn;
    uint8_t *b = dbq_groups(&dn, NULL);

    if (!b || grp_n <= 0 || grp_n != dn || pl->created_tick != grp_tick
        || playlist_amount() != grp_n)
        return false;
    *bits = b;
    *n = grp_n;
    return true;
#else
    (void)bits; (void)n;
    return false;
#endif
}

bool lib_queue_is_book(const struct playlist_info *pl)
{
    if (pl != playlist_get_current())
        return false;
    if (grp_book && grp_n > 0 && pl->created_tick == grp_tick
        && playlist_amount() == grp_n)
        return true;
    return book_n > 0 && pl->created_tick == book_tick
           && playlist_amount() == book_n;
}

void lib_queue_resumed(void)
{
    const struct playlist_info *pl = playlist_get_current();

    book_n = -1;
    if (lib_shuffle_resumed_book())
    {
        book_n = playlist_amount();
        book_tick = pl->created_tick;
        resumed_map_owed = true;
    }
}

static int lib_scratch_open_held(void)
{
    if (tagcache_ui_count() < 0)
        return -1;
    return play_scratch_open();
}

int lib_scratch_open(void)
{
    struct tagcache_context *held = tagcache_context_acquire();
    int result = lib_scratch_open_held();
    tagcache_context_release(held);
    return result;
}

const char *lib_scratch_path(int sh, int entry)
{
    struct play_scratch *ps = scratch_of(sh);       /* pinned */

    if (!tagcache_retrieve(&ps->tcs, entry, tag_filename, ps->path,
                           sizeof(ps->path)))
        return NULL;
    return ps->path;
}

void lib_scratch_close(int sh)
{
    if (sh > 0)
        play_scratch_close(sh);
}

/* The header of the precompute file, read in 16 B pieces (no 544 B
 * buffer) and checked like pre_hdr_ok(); then the sections wanted. */
static bool tracks_hdr(int fd, struct lib_tracks *t, int cat)
{
    struct libfile_sum_state s;
    uint32_t w[4];
    uint32_t head[8], i, nsec;
    bool have_rows = false;

    libfile_sum_init(&s);
    if (read(fd, head, sizeof(head)) != (ssize_t)sizeof(head))
        return false;
    if (head[0] != LIBFILE_MAGIC || head[1] != LIBFILE_VERSION
        || head[2] != SORTKEY_VERSION
        || (int32_t)head[3] != tagcache_ui_commitid()
        || (int32_t)head[4] != tagcache_ui_count()
        || head[5] > LIBFILE_MAX_SEC)
        return false;
    nsec = head[5];
    {
        uint32_t sum = head[6];
        head[6] = 0;
        libfile_sum_add(&s, head, sizeof(head));
        head[6] = sum;
    }
    t->tracks_off = 0;
    for (i = 0; i < LIBFILE_MAX_SEC; i++)
    {
        const struct libfile_sec *sec = (const struct libfile_sec *)w;

        if (read(fd, w, sizeof(w)) != (ssize_t)sizeof(w))
            return false;
        libfile_sum_add(&s, w, sizeof(w));
        if (i >= nsec || sec->cat != cat)
            continue;
        if (sec->col == LIBFILE_ROWS && sec->elem == 4)
        {
            t->albums = sec->count;
            t->rows_off = sec->offset;
            t->rows_sum = sec->sum;
            have_rows = true;
        }
        else if (sec->col == LIBFILE_TRACKS && sec->elem == 4)
        {
            t->tracks_off = sec->offset;
            t->tracks_count = sec->count;
        }
    }
    if (libfile_sum_end(&s) != head[6] || !have_rows)
        return false;
    if (t->tracks_off
        && t->tracks_count < LIBFILE_TRACKS_HDR(t->albums))
        t->tracks_off = 0;
    return true;
}

static int lib_tracks_open_held(struct lib_tracks *t)
{
    char path[MAX_PATH];

    memset(t, 0, sizeof(*t));
    t->fd = -1;
    t->vh = -1;
    if (tagcache_ui_count() < 0)
        return -1;
    pre_path(tagcache_context_active(), path, sizeof(path), LIBFILE_NAME);
    t->fd = open(path, O_RDONLY);
    if (t->fd < 0)
        return -1;
    if (!tracks_hdr(t->fd, t, LIB_ALBUMS))
    {
        lib_tracks_close(t);
        return -1;
    }
    t->gen = tagcache_ui_generation();
    if (!t->tracks_off)
        logf("lib: no TRACKS section: album filter");
    return 0;
}

int lib_tracks_open(struct lib_tracks *t)
{
    struct tagcache_context *held = tagcache_context_acquire();
    int result = lib_tracks_open_held(t);
    tagcache_context_release(held);
    return result;
}

void lib_tracks_close(struct lib_tracks *t)
{
    if (t->vh > 0)
        slot_free(t->vh);
    t->vh = -1;
    if (t->fd >= 0)
        close(t->fd);
    t->fd = -1;
}

static bool tracks_word(struct lib_tracks *t, uint32_t off, uint32_t *v,
                        int n)
{
    ssize_t want = (ssize_t)n * 4;

    return lseek(t->fd, off, SEEK_SET) == (off_t)off
        && read(t->fd, v, want) == want;
}

/* Fallback (no TRACKS): album row a's tracks as a child view, built once
 * per album and kept until the next album or lib_tracks_close(). */
static bool tracks_view(struct lib_tracks *t, uint32_t a)
{
    struct lib_filter f = LIB_FILTER_ANY;
    const char *src;
    uint32_t rep;

    if (t->vh > 0 && t->valbum == a)
        return true;
    if (t->vh > 0)
        slot_free(t->vh);
    t->vh = -1;
    if (!tracks_word(t, t->rows_off + a * 4, &rep, 1)
        || (int)rep < 0 || (int)rep >= tagcache_ui_count())
        return false;
    f.album = (int)rep;
    t->vh = build(LIB_SONGS, &f, &src);
    t->valbum = a;
    return t->vh > 0;
}

static uint32_t lib_tracks_len_held(struct lib_tracks *t, uint32_t a)
{
    uint32_t se[2];

    if (t->fd < 0 || a >= t->albums || t->gen != tagcache_ui_generation())
        return 0;
    if (!t->tracks_off)
        return tracks_view(t, a) ? (uint32_t)get_view(t->vh)->count : 0;
    if (!tracks_word(t, t->tracks_off + a * 4, se, 2)
        || se[1] < se[0] || se[1] > t->tracks_count
        || se[0] < LIBFILE_TRACKS_HDR(t->albums))
        return 0;
    return se[1] - se[0];
}

uint32_t lib_tracks_len(struct lib_tracks *t, uint32_t a)
{
    struct tagcache_context *held = tagcache_context_acquire();
    uint32_t result = lib_tracks_len_held(t, a);
    tagcache_context_release(held);
    return result;
}

static int lib_tracks_entry_held(struct lib_tracks *t, uint32_t a, uint32_t k)
{
    uint32_t st;
    int32_t e;

    if (t->fd < 0 || a >= t->albums || t->gen != tagcache_ui_generation())
        return -1;
    if (!t->tracks_off)
    {
        struct view *v;

        if (!tracks_view(t, a))
            return -1;
        v = get_view(t->vh);
        return k < (uint32_t)v->count ? v->rows[k] : -1;
    }
    if (!tracks_word(t, t->tracks_off + a * 4, &st, 1)
        || st + k >= t->tracks_count
        || !tracks_word(t, t->tracks_off + (st + k) * 4, (uint32_t *)&e, 1))
        return -1;
    /* the slice is not checksummed (read in pieces): range-check it */
    return e >= 0 && e < tagcache_ui_count() ? e : -1;
}

int lib_tracks_entry(struct lib_tracks *t, uint32_t a, uint32_t k)
{
    struct tagcache_context *held = tagcache_context_acquire();
    int result = lib_tracks_entry_held(t, a, k);
    tagcache_context_release(held);
    return result;
}

static int lib_book_tracks_held(int32_t rep, uint32_t *idx, int max, void *buf,
                    size_t size)
{
    char path[MAX_PATH];
    struct lib_tracks t;
    int32_t *rows = buf;
    uint32_t se[2], a, chunk = (uint32_t)(size / sizeof(int32_t)), j;
    int fd, gen = tagcache_ui_generation(), n = -1, row = -1, total;

    total = tagcache_ui_count();
    if (total < 0 || rep < 0 || !buf || chunk == 0)
        return -1;
    pre_path(tagcache_context_active(), path, sizeof(path), LIBFILE_NAME);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    memset(&t, 0, sizeof(t));
    if (!tracks_hdr(fd, &t, LIB_AUDIOBOOKS) || !t.tracks_off)
        goto out;
    /* the book's row: ROWS hold the representatives, in list order */
    for (a = 0; a < t.albums && row < 0; a += chunk)
    {
        uint32_t k = MIN(chunk, t.albums - a);

        if (lseek(fd, t.rows_off + a * 4, SEEK_SET)
                != (off_t)(t.rows_off + a * 4)
            || read(fd, rows, k * 4) != (ssize_t)(k * 4))
            goto out;
        for (j = 0; j < k; j++)
            if (rows[j] == rep)
            {
                row = (int)(a + j);
                break;
            }
    }
    if (row < 0 || lseek(fd, t.tracks_off + row * 4, SEEK_SET)
                       != (off_t)(t.tracks_off + row * 4)
        || read(fd, se, sizeof(se)) != (ssize_t)sizeof(se)
        || se[1] < se[0] || se[1] > t.tracks_count
        || se[0] < LIBFILE_TRACKS_HDR(t.albums))
        goto out;
    n = (int)(se[1] - se[0]);
    if (n == 0 || n > max)
        goto out;                       /* too many: the count only */
    if (lseek(fd, t.tracks_off + se[0] * 4, SEEK_SET)
            != (off_t)(t.tracks_off + se[0] * 4)
        || read(fd, idx, n * 4) != (ssize_t)(n * 4))
    {
        n = -1;
        goto out;
    }
    /* the slice is not checksummed: range-check it */
    for (j = 0; j < (uint32_t)n; j++)
        if ((int32_t)idx[j] < 0 || (int32_t)idx[j] >= total)
            n = -1;
out:
    close(fd);
    return tagcache_ui_generation() == gen ? n : -1;
}

int lib_book_tracks(int32_t rep, uint32_t *idx, int max, void *buf,
                    size_t size)
{
    struct tagcache_context *held = tagcache_context_acquire();
    int result = lib_book_tracks_held(rep, idx, max, buf, size);
    tagcache_context_release(held);
    return result;
}

/* -- precompute (tagcache thread) --------------------------------------- */

/* The work block (reserved at shell start, or transient: see below):
 *   records     12 B x entries (steps 1-4), then the identity map
 *   seen table  4 KB (scan only), then per-row counts + newest mtimes
 *               (8 B x rows, after the identity map)
 *   ...         room up to 20 B x entries + 4 KB
 *   header      LIBFILE_HDR_SIZE: the file's header, filled as sections
 *               are written, written last
 *   write buf   PRE_WBUF: sections are streamed through it
 * The album table is assembled at the start of the block at the end. */
#define PRE_WBUF    2048
#define PRE_BYTES_PER_ENTRY 20
/* #59: pre_books_author() needs 16 B per book (a record + a rep), and
 * books never outnumber entries: it always fits, so its no-room branch
 * can never leave a file that pre_current() would rewrite forever */
typedef char lib_pre_books_room_check[
    PRE_BYTES_PER_ENTRY >= sizeof(struct rec) + sizeof(int32_t) ? 1 : -1];

/* The block, reserved once at shell start (lib_reserve(), H1 #41.2) for
 * the database on disk + 25 %, before anything can start playback: the
 * precompute then runs at any time, playing or not, and never allocates.
 * Movable, used by the tagcache thread only (re-fetched after every
 * yield, pinned around file I/O). None without a database at boot (a
 * first build): then, or once the library has outgrown it, the
 * precompute takes a transient block, only while audio is stopped, as in
 * 4b; the next boot reserves for the new size. */
static int pre_res_h;
static long pre_res_entries;
/* The running precompute's own block is transient: a playback start
 * stops it (its block would hold memory the audio buffer wants). */
static bool pre_transient;
static bool (*pre_events)(void);

static size_t pre_bytes(long entries)
{
    return (size_t)entries * PRE_BYTES_PER_ENTRY + SEEN_BYTES
        + LIBFILE_HDR_SIZE + PRE_WBUF;
}

/* bctx.stop of a precompute: an event to handle, or playback started
 * under a transient block */
static bool pre_stop(void)
{
    return (pre_events && pre_events())
           || (pre_transient && audio_status() && !maint_active());
}

/* (Re)reserves the precompute block for 'n' entries + 25 % (none for
 * n < 0). Only before any playback or while audio is stopped. */
static void pre_reserve(long n)
{
    size_t size;

    if (pre_res_h > 0)
    {
        pre_res_h = core_free(pre_res_h);
        audio_buffer_regrow();
    }
    pre_res_h = 0;
    pre_res_entries = 0;
    if (n < 0)
    {
        UPDATE_TRACE("lib reserve none%s", "");
        return;                 /* no database: stopped-only */
    }
    n += (n + 3) / 4;
    size = pre_bytes(n);
    pre_res_h = core_alloc(size);
    if (pre_res_h <= 0)
    {
        UPDATE_TRACE("lib alloc failed what=%s bytes=%ld", "precompute",
                     (long)size);
        pre_res_h = 0;
        return;
    }
    pre_res_entries = n;
    UPDATE_TRACE("lib reserve entries=%ld bytes=%lu", n, (unsigned long)size);
}

void lib_reserve(void)
{
    pre_reserve(tagcache_ui_disk_entries());
    pool_reserve_start();
}

struct pre {
    struct tagcache_context *tc;
    struct bctx c;              /* c.h: the work block */
    int fd;
    int n;                      /* entries */
    size_t size, hdr_off, wbuf_off;
    int wlen;                   /* bytes in the write buffer */
    uint32_t pos;               /* file offset of the write buffer */
    struct libfile_sum_state sum;
    int steps;
};

static struct libfile_hdr *pre_hdr(struct pre *p)
{
    return (struct libfile_hdr *)(bx_base(&p->c) + p->hdr_off);
}

/* Every YIELD_EVERY calls: a yield; false if the precompute must stop. */
static bool pre_step(struct tagcache_context *ctx, struct pre *p)
{
    if (++p->steps < YIELD_EVERY)
        return true;
    p->steps = 0;
    return bx_yield(ctx, &p->c);
}

static bool pre_write(struct pre *p, size_t off, size_t len)
{
    ssize_t w;

    core_pin(p->c.h);           /* write() may yield: the data must stay */
    w = write(p->fd, bx_base(&p->c) + off, len);
    core_unpin(p->c.h);
    return w == (ssize_t)len;
}

static bool pre_flush(struct pre *p)
{
    if (!p->wlen)
        return true;
    if (!pre_write(p, p->wbuf_off, p->wlen))
        return false;
    p->pos += p->wlen;
    p->wlen = 0;
    return true;
}

/* Appends 'len' (4 or 8, or a multiple of 4) bytes from the stack. */
static bool pre_put(struct pre *p, const void *d, size_t len)
{
    if (p->wlen + len > PRE_WBUF && !pre_flush(p))
        return false;
    memcpy(bx_base(&p->c) + p->wbuf_off + p->wlen, d, len);
    libfile_sum_add(&p->sum, d, len);
    p->wlen += len;
    return true;
}

static bool pre_sec_begin(struct pre *p, int cat, int col, int elem)
{
    struct libfile_hdr *hd = pre_hdr(p);
    struct libfile_sec *s;

    if (hd->nsec >= LIBFILE_MAX_SEC)
        return false;
    s = &hd->sec[hd->nsec];
    s->cat = cat;
    s->col = col;
    s->elem = elem;
    s->count = 0;
    s->offset = p->pos + p->wlen;
    s->sum = 0;
    libfile_sum_init(&p->sum);
    return true;
}

static void pre_sec_end(struct pre *p)
{
    struct libfile_hdr *hd = pre_hdr(p);
    struct libfile_sec *s = &hd->sec[hd->nsec];

    s->count = (p->pos + p->wlen - s->offset) / s->elem;
    s->sum = libfile_sum_end(&p->sum);
    hd->nsec++;
}

/* The art key of a row (its representative 'rep'): the album and the
 * album artist (else the artist) it groups by; a show by its album only
 * (Decision 7); an untagged album by its first track's folder, which
 * takes the file name from disk (tcs, opened on first use). 0 if none.
 * False if the precompute must stop. */
static bool pre_key(struct tagcache_context *ctx, struct pre *p, int cat, int rep,
                    struct tagcache_search *tcs, bool *tcs_open,
                    char *path, uint64_t *key)
{
    const int32_t *s = tagcache_ui_seeks_ctx(ctx, rep, NULL);
    const char *album;

    *key = 0;
    if (!s)
        return true;
    album = str_of(ctx, s, tag_album);
#ifdef HAVE_BCMFW_VIDEO
    /* Phase 14a (3.3): a movie is its own group, keyed by its whole path
     * (the folder key of an untagged album would give every movie in
     * Movies/ one poster); a show by its show alone, as a podcast */
    if (cat == LIB_MOVIES)
        album = NULL;
    if (album && cat == LIB_TVSHOWS)
    {
        *key = artcache_key(NULL, NULL, album, NULL);
        if (*key)
            return true;
    }
    else
#endif
    if (album)
    {
        if (cat == LIB_PODCASTS)
            *key = artcache_key(NULL, NULL, album, NULL);
        else
            *key = artcache_key(str_of(ctx, s, tag_albumartist),
                                str_of(ctx, s, tag_artist), album, NULL);
        if (*key)
            return true;
        /* an album artkey treats as untagged (blank, "<Untagged>" in
         * another case): keyed by its folder, as artcache_key() over the
         * path would */
    }
    if (!*tcs_open)
    {
        if (!tagcache_search_ctx(ctx, tcs, tag_filename))
            return true;        /* no key: the note glyph */
        *tcs_open = true;
    }
#ifdef HAVE_BCMFW_VIDEO
    if (cat == LIB_MOVIES)
    {
        if (tagcache_retrieve(tcs, rep, tag_filename, path, MAX_PATH))
            *key = artcache_key(NULL, NULL, path, NULL);
    }
    else
#endif
    if (tagcache_retrieve(tcs, rep, tag_filename, path, MAX_PATH))
        *key = artcache_key(NULL, NULL, NULL, path);
    /* the read may have yielded */
    return tagcache_ui_generation_ctx(ctx) == p->c.gen
        && !(p->c.stop && p->c.stop());
}

/* The KEY section of an album category: m records, rows marked (k2). */
static bool NO_INLINE pre_keys(struct tagcache_context *ctx, struct pre *p, int cat, int m)
{
    struct tagcache_search tcs;
    char path[MAX_PATH];
    bool tcs_open = false, ok;
    int i;

    ok = pre_sec_begin(p, cat, LIBFILE_KEY, 2 * sizeof(uint32_t));
    for (i = 0; ok && i < m; i++)
    {
        struct rec t;
        uint64_t key;
        uint32_t k[2];

        if (!pre_step(ctx, p))
        {
            ok = false;
            break;
        }
        t = recs_at(&p->c, p->c.recs_off)[i];
        if (!t.k2)
            continue;
        ok = pre_key(ctx, p, cat, t.idx, &tcs, &tcs_open, path, &key);
        k[0] = (uint32_t)key;
        k[1] = (uint32_t)(key >> 32);
        ok = ok && pre_put(p, k, sizeof(k));
    }
    if (tcs_open)
        tagcache_search_finish(&tcs);
    if (ok)
        pre_sec_end(p);
    return ok;
}

/* COUNT (and MTIME for album categories) of group category 'cat': turns
 * the m records into an identity -> row map, sorts it, then one pass over
 * the master index counts each row's tracks of class 'cls'. */
/* Row of album entry 'i' (seeks s, flag) through the identity map that
 * pre_counts() sorted at recs_off (m records), or -1. */
static int pre_row_of(struct tagcache_context *ctx, struct pre *p, int m, int i, const int32_t *s,
                      int32_t flag)
{
    const struct rec *map;
    struct rec k;
    int lo = 0, hi = m;

    k.idx = i;
    group_ident(ctx, &p->c, &k, G_ALBUM, s, flag);
    map = recs_at(&p->c, p->c.recs_off);
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;

        if (map[mid].k1 == k.k1 && map[mid].k2 == k.k2)
            return map[mid].idx;
        if (map[mid].k1 < k.k1
            || (map[mid].k1 == k.k1 && map[mid].k2 < k.k2))
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

/* disc, track, then master index: lib_open(LIB_SONGS, album filter)'s
 * order (RAM index only) */
static bool slice_lt(struct tagcache_context *ctx, int32_t a, int32_t b)
{
    int c = cmp_track_no(ctx, a, b);

#ifdef HAVE_BCMFW_VIDEO
    /* a show's episodes: season, episode, then title (3.3), as
     * lib_open(LIB_TVEPISODES) orders them (cmp_tie(ctx, ) O_TRACK) */
    if (c == 0)
    {
        int32_t fa;
        const int32_t *sa = tagcache_ui_seeks_ctx(ctx, a, &fa);

        if (sa && is_tv(fa))
            c = cmp_labels(label_of(ctx, G_NONE, a), label_of(ctx, G_NONE, b));
    }
#endif
    return c < 0 || (c == 0 && a < b);
}

/* Heap sort of one album's slice (n master indices at byte 'off' of the
 * block), yielding through pre_step(); re-fetched after every step. */
static bool pre_slice_sort(struct tagcache_context *ctx, struct pre *p, size_t off, int n)
{
    int end = n, i = n / 2;

#define SL(k) (((int32_t *)(bx_base(&p->c) + off))[k])
    for (;;)
    {
        int root;
        int32_t t;

        if (i > 0)
            root = --i;
        else
        {
            if (--end <= 0)
                break;
            t = SL(0); SL(0) = SL(end); SL(end) = t;
            root = 0;
        }
        for (;;)
        {
            int child = 2 * root + 1;

            if (!pre_step(ctx, p))
                return false;
            if (child >= end)
                break;
            if (child + 1 < end && slice_lt(ctx, SL(child), SL(child + 1)))
                child++;
            if (!slice_lt(ctx, SL(root), SL(child)))
                break;
            t = SL(root); SL(root) = SL(child); SL(child) = t;
            root = child;
        }
    }
#undef SL
    return true;
}

/* #49: the LIB_ALBUMS TRACKS section (libfile.h; P6 L2: LIB_AUDIOBOOKS
 * too, the books builder's file lists). Called by pre_counts()
 * once COUNT and MTIME are written: the identity map (12 B x m) is still
 * at recs_off and the per-row counts at 'arr' (r x uint32; the r mtimes
 * after them are free now). Lays out at 'arr': r fill cursors (made from
 * the counts), then T track slots: 12m + 4r + 4T bytes in all, inside the
 * existing 20 B/entry work block (no growth). A library too fragmented
 * for that gets no TRACKS section, and Albums shuffle falls back to the
 * album filter. One more pass over the index fills the slots; each
 * album's slice is then heap-sorted and streamed out. */
static bool pre_tracks(struct tagcache_context *ctx, struct pre *p, int cat, int cls, int m, int r,
                       size_t arr)
{
    size_t tr = arr + (size_t)r * sizeof(uint32_t);
    uint32_t t = 0, run, v;
    int total = tagcache_ui_count_ctx(ctx), i;

#define CUR(k) (((uint32_t *)(bx_base(&p->c) + arr))[k])
#define TRK(k) (((int32_t *)(bx_base(&p->c) + tr))[k])
    for (i = 0; i < r; i++)
        t += CUR(i);
    if (tr + (size_t)t * sizeof(int32_t) > p->hdr_off)
    {
        logf("lib: precompute: no TRACKS (%d albums)", r);
        return true;
    }
    if (!pre_sec_begin(p, cat, LIBFILE_TRACKS, sizeof(uint32_t)))
        return false;
    for (i = 0, run = 0; i < r; i++)
    {
        uint32_t c = CUR(i);

        v = LIBFILE_TRACKS_HDR(r) + run;
        if (!pre_put(p, &v, sizeof(v)))
            return false;
        CUR(i) = run;
        run += c;
    }
    v = LIBFILE_TRACKS_HDR(r) + run;
    if (!pre_put(p, &v, sizeof(v)))
        return false;
    for (i = 0; i < total; i++)
    {
        int32_t flag;
        const int32_t *s;
        int row;

        if (!pre_step(ctx, p))
            return false;
        s = tagcache_ui_seeks_ctx(ctx, i, &flag);
        if (!s || !class_ok(cls, flag))
            continue;
        row = pre_row_of(ctx, p, m, i, s, flag);
        if (row < 0 || row >= r || CUR(row) >= t)
            continue;
        TRK(CUR(row)++) = i;
    }
    /* each cursor now sits at its album's end */
    for (i = 0, run = 0; i < r; i++)
    {
        uint32_t end = CUR(i), k;

        if (end > run + 1
            && !pre_slice_sort(ctx, p, tr + run * sizeof(int32_t),
                               (int)(end - run)))
            return false;
        for (k = run; k < end; k++)
        {
            int32_t e = TRK(k);
            if (!pre_put(p, &e, sizeof(e)))
                return false;
        }
        run = end;
    }
#undef TRK
#undef CUR
    pre_sec_end(p);
    return true;
}

static bool pre_counts(struct tagcache_context *ctx, struct pre *p, int cat, int group, int cls, int m,
                       int r)
{
    size_t arr = ALIGN_UP(p->c.recs_off + (size_t)m * sizeof(struct rec), 4);
    int total = tagcache_ui_count_ctx(ctx), i;
    uint32_t *cnt, *mt;

    for (i = 0; i < m; i++)
    {
        struct rec *t, k;
        int32_t flag;
        const int32_t *s;

        if (!pre_step(ctx, p))
            return false;
        t = &recs_at(&p->c, p->c.recs_off)[i];
        k.idx = t->idx;
        s = tagcache_ui_seeks_ctx(ctx, t->idx, &flag);
        if (s)
            group_ident(ctx, &p->c, &k, group, s, flag);
        else
            k.k1 = k.k2 = 0xffffffffu;
        k.idx = t->k1;                      /* the row */
        *t = k;
    }
    p->c.group = G_NONE;
    p->c.order = -1;
    if (!sort_recs(ctx, &p->c, p->c.recs_off, m))
        return false;
    cnt = (uint32_t *)(bx_base(&p->c) + arr);
    memset(cnt, 0, (size_t)r * 2 * sizeof(uint32_t));

    for (i = 0; i < total; i++)
    {
        struct rec k;
        const struct rec *map;
        int32_t flag;
        const int32_t *s;
        int lo = 0, hi = m;

        if (!pre_step(ctx, p))
            return false;
        s = tagcache_ui_seeks_ctx(ctx, i, &flag);
        if (!s || !class_ok(cls, flag))
            continue;
        k.idx = i;
        group_ident(ctx, &p->c, &k, group, s, flag);
        map = recs_at(&p->c, p->c.recs_off);
        while (lo < hi)
        {
            int mid = (lo + hi) / 2;

            if (map[mid].k1 == k.k1 && map[mid].k2 == k.k2)
            {
                int row = map[mid].idx;

                cnt = (uint32_t *)(bx_base(&p->c) + arr);
                mt = cnt + r;
                cnt[row]++;
                if ((uint32_t)s[tag_mtime] > mt[row])
                    mt[row] = s[tag_mtime];
                break;
            }
            if (map[mid].k1 < k.k1
                || (map[mid].k1 == k.k1 && map[mid].k2 < k.k2))
                lo = mid + 1;
            else
                hi = mid;
        }
    }

    if (!pre_sec_begin(p, cat, LIBFILE_COUNT, sizeof(uint32_t)))
        return false;
    for (i = 0; i < r; i++)
    {
        uint32_t v = ((uint32_t *)(bx_base(&p->c) + arr))[i];
        if (!pre_put(p, &v, sizeof(v)))
            return false;
    }
    pre_sec_end(p);
    if (group != G_ALBUM)
        return true;
    if (!pre_sec_begin(p, cat, LIBFILE_MTIME, sizeof(uint32_t)))
        return false;
    for (i = 0; i < r; i++)
    {
        uint32_t v = ((uint32_t *)(bx_base(&p->c) + arr))[r + i];
        if (!pre_put(p, &v, sizeof(v)))
            return false;
    }
    pre_sec_end(p);
    if (cat == LIB_ALBUMS || cat == LIB_AUDIOBOOKS    /* P6 L2: books too */
#ifdef HAVE_BCMFW_VIDEO
        || cat == LIB_TVSHOWS                         /* 14a: episodes */
#endif
        )
        return pre_tracks(ctx, p, cat, cls, m, r, arr);
    return true;
}

/* One unfiltered category: ROWS, and KEY, COUNT, MTIME as libfile.h
 * lists them. */
static bool pre_cat(struct tagcache_context *ctx, struct pre *p, int cat)
{
    int group, order, cls, m, r, i;

    classify(cat, NULL, &group, &order, &cls);
    p->c.memo.generation = -1;
    m = rows_build(ctx, &p->c, cls, group, order, NULL, p->n, &r);
    if (m < 0)
        return false;
    if (!pre_sec_begin(p, cat, LIBFILE_ROWS, sizeof(int32_t)))
        return false;
    for (i = 0; i < m; i++)
    {
        struct rec t;

        if (!pre_step(ctx, p))
            return false;
        t = recs_at(&p->c, p->c.recs_off)[i];
        if (t.k2 && !pre_put(p, &t.idx, sizeof(t.idx)))
            return false;
    }
    pre_sec_end(p);
#ifdef HAVE_BCMFW_VIDEO
    if (cat == LIB_MOVIES)
        return pre_keys(ctx, p, cat, m);     /* ROWS, KEY (3.3) */
#endif
    if (group == G_ALBUM && !pre_keys(ctx, p, cat, m))
        return false;
    if (group == G_ALBUM || group == G_ARTIST)
        return pre_counts(ctx, p, cat, group, cls, m, r);
    return true;
}

/* Reads back column 'col' of category 'cat' (written above) into the
 * album entries at 'e' + 'first', field at byte 'field'. */
static bool pre_readback(struct pre *p, int cat, int col, size_t first,
                         size_t field)
{
    struct libfile_hdr *hd = pre_hdr(p);
    struct libfile_sec sec;
    unsigned i;
    size_t done = 0, total;

    for (i = 0; i < hd->nsec; i++)
        if (hd->sec[i].cat == cat && hd->sec[i].col == col)
            break;
    if (i == hd->nsec)
        return false;
    sec = hd->sec[i];
    total = (size_t)sec.count * sec.elem;
    if (lseek(p->fd, sec.offset, SEEK_SET) != (off_t)sec.offset)
        return false;
    while (done < total)
    {
        size_t chunk = MIN(total - done, (size_t)PRE_WBUF);
        ssize_t got;
        size_t j;
        char *b;

        core_pin(p->c.h);
        got = read(p->fd, bx_base(&p->c) + p->wbuf_off, chunk);
        core_unpin(p->c.h);
        if (got != (ssize_t)chunk)
            return false;
        b = bx_base(&p->c);
        for (j = 0; j < chunk; j += sec.elem)
        {
            struct libfile_album *e = (struct libfile_album *)b
                + first + (done + j) / sec.elem;
            memcpy((char *)e + field, b + p->wbuf_off + j, sec.elem);
        }
        done += chunk;
    }
    return true;
}

/* -- the books by author (#59): LIBFILE_CAT_BOOKS_AUTHOR ------------------ */

/* Reads section (cat, col) back whole (n elements of 'elem' bytes, as
 * written above) to byte 'off' of the block: one read(), then the file
 * position goes back to the end for the next section. */
static bool pre_load(struct pre *p, int cat, int col, int elem, size_t off,
                     uint32_t n)
{
    const struct libfile_sec *sec = pre_sec(pre_hdr(p), cat, col, elem);
    size_t bytes = (size_t)n * elem;
    ssize_t got;

    if (!sec || sec->count != n || !pre_flush(p)
        || lseek(p->fd, sec->offset, SEEK_SET) != (off_t)sec->offset)
        return false;
    core_pin(p->c.h);
    got = read(p->fd, bx_base(&p->c) + off, bytes);
    core_unpin(p->c.h);
    return got == (ssize_t)bytes
        && lseek(p->fd, p->pos, SEEK_SET) == (off_t)p->pos;
}

/* The LIB_AUDIOBOOKS rows, counts and keys (written above, in title
 * order) again, ordered by author, then title: the list under Sort By
 * Author, so that switching never sorts at browse time. The r title-order
 * reps are read back to 12r; records (author key prefix, title row) at 0
 * are sorted (equal prefixes compare the full authors, cmp_tie()), then
 * compacted into the permutation (4r at 0); ROWS is written through it,
 * then COUNT and KEY are each read back to 4r and written permuted. Peak
 * 16 B per book, inside the 20 B/entry block. */
static bool pre_books_author(struct tagcache_context *ctx, struct pre *p)
{
    const struct libfile_sec *rs = pre_sec(pre_hdr(p), LIB_AUDIOBOOKS,
                                           LIBFILE_ROWS, sizeof(int32_t));
    size_t reps_off, col_off;
    uint32_t r, i;

#define PERM(k) (((uint32_t *)bx_base(&p->c))[k])
#define REPS(k) (((int32_t *)(bx_base(&p->c) + reps_off))[k])
#define COL(k)  (((uint32_t *)(bx_base(&p->c) + col_off))[k])
    if (!rs)
        return true;            /* no books written: nothing to order */
    r = rs->count;
    col_off = (size_t)r * sizeof(uint32_t);
    reps_off = (size_t)r * sizeof(struct rec);
    if (reps_off + col_off > p->hdr_off)
    {
        /* unreachable (lib_pre_books_room_check): r <= entries */
        logf("lib: precompute: no room to order %lu books", (unsigned long)r);
        return true;            /* the list stays in title order */
    }
    if (!pre_load(p, LIB_AUDIOBOOKS, LIBFILE_ROWS, sizeof(int32_t),
                  reps_off, r))
        return false;
    for (i = 0; i < r; i++)
    {
        struct rec *t;

        if (!pre_step(ctx, p))
            return false;
        t = &recs_at(&p->c, 0)[i];
        t->idx = i;
        key_prefix(label_of(ctx, G_ARTIST, REPS(i)), &t->k1, &t->k2);
    }
    p->c.group = G_NONE;
    p->c.order = O_BOOK_AUTHOR;
    p->c.aux_off = reps_off;
    if (!sort_recs(ctx, &p->c, 0, r))
        return false;
    p->c.order = -1;
    /* perm[i] (4 B at 4i) never overtakes record i (12 B at 12i) */
    for (i = 0; i < r; i++)
    {
        int32_t row = recs_at(&p->c, 0)[i].idx;

        PERM(i) = (uint32_t)row;
    }

    if (!pre_sec_begin(p, LIBFILE_CAT_BOOKS_AUTHOR, LIBFILE_ROWS,
                       sizeof(int32_t)))
        return false;
    for (i = 0; i < r; i++)
    {
        int32_t v;

        if (!pre_step(ctx, p))
            return false;
        v = REPS(PERM(i));
        if (!pre_put(p, &v, sizeof(v)))
            return false;
    }
    pre_sec_end(p);

    if (!pre_load(p, LIB_AUDIOBOOKS, LIBFILE_COUNT, sizeof(uint32_t),
                  col_off, r)
        || !pre_sec_begin(p, LIBFILE_CAT_BOOKS_AUTHOR, LIBFILE_COUNT,
                          sizeof(uint32_t)))
        return false;
    for (i = 0; i < r; i++)
    {
        uint32_t v;

        if (!pre_step(ctx, p))
            return false;
        v = COL(PERM(i));
        if (!pre_put(p, &v, sizeof(v)))
            return false;
    }
    pre_sec_end(p);

    if (!pre_load(p, LIB_AUDIOBOOKS, LIBFILE_KEY, 2 * sizeof(uint32_t),
                  col_off, r)
        || !pre_sec_begin(p, LIBFILE_CAT_BOOKS_AUTHOR, LIBFILE_KEY,
                          2 * sizeof(uint32_t)))
        return false;
    for (i = 0; i < r; i++)
    {
        uint32_t k[2];

        if (!pre_step(ctx, p))
            return false;
        k[0] = COL(2 * PERM(i));
        k[1] = COL(2 * PERM(i) + 1);
        if (!pre_put(p, k, sizeof(k)))
            return false;
    }
    pre_sec_end(p);
#undef COL
#undef REPS
#undef PERM
    return true;
}

/* -- the panel set (H1): LIBFILE_CAT_PANELSET ---------------------------- */
#define PANEL_PLAYED 12         /* the most recently played albums */
#define PANEL_ADDED  10         /* the newest albums (tag_mtime) */
/* #58 (owner): the Audiobooks rows drift over book covers. Up to this
 * many books of the set's LIBFILE_PANELS_MAX entries (flag
 * LIBFILE_ALB_BOOK; the art builder files them as panels.pak's book set):
 * the most recently played, then the newest. Music keeps the rest. */
#define PANEL_BOOKS  8
#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a (3.5, M9 ⚠️ A2): up to this many movies and shows (flag
 * LIBFILE_ALB_VIDEO, a key), most recently played, then the newest; the
 * art builder files them as the video set. Taken only when there are
 * some, as the books' share. */
#define PANEL_VIDEOS 8
#define ALB_NOT_MUSIC (LIBFILE_ALB_BOOK | LIBFILE_ALB_PODCAST \
                       | LIBFILE_ALB_VIDEO)
#else
#define ALB_NOT_MUSIC (LIBFILE_ALB_BOOK | LIBFILE_ALB_PODCAST)
#endif

static struct libfile_album *alb_at(struct pre *p, size_t i)
{
    return (struct libfile_album *)bx_base(&p->c) + i;
}

/* Album i may join the set (music, or a book with 'book'), one entry per
 * key */
static bool panel_ok(struct pre *p, size_t i, const int *sel, int n,
                     bool book)
{
    const struct libfile_album *e = alb_at(p, i);
    int j;

    if (book ? !(e->flags & LIBFILE_ALB_BOOK)
             : (e->flags & ALB_NOT_MUSIC))
        return false;
    if (!e->key_lo && !e->key_hi)
        return false;
    for (j = 0; j < n; j++)
    {
        const struct libfile_album *s = alb_at(p, sel[j]);

        if (s->key_lo == e->key_lo && s->key_hi == e->key_hi)
            return false;
    }
    return true;
}

/* A top-K list of album keys by last play, newest first, one per key */
struct panel_top {
    uint64_t keys[PANEL_PLAYED];
    uint32_t when[PANEL_PLAYED];
    int n, max;
};

static void top_add(struct panel_top *t, uint64_t key, uint32_t w)
{
    int j;

    for (j = 0; j < t->n && t->keys[j] != key; j++)
        ;
    if (j < t->n)
    {
        if (w <= t->when[j])
            return;             /* this album was played later already */
        for (; j + 1 < t->n; j++)
        {
            t->keys[j] = t->keys[j + 1];
            t->when[j] = t->when[j + 1];
        }
        t->n--;
    }
    j = t->n < t->max ? t->n++ : t->n - 1;
    for (; j > 0 && t->when[j - 1] < w; j--)
    {
        t->keys[j] = t->keys[j - 1];
        t->when[j] = t->when[j - 1];
    }
    t->keys[j] = key;
    t->when[j] = w;
}

#ifdef HAVE_BCMFW_VIDEO
/* The videos' share: 'tv' holds show keys, 'mov' movie entries + 1
 * (a movie's key needs its path; its table row is found by its rep) */
struct panel_vtop {
    struct panel_top tv, mov;
};

/* Video entry i (seeks s, flag) was played at w: into the lists */
static void panel_vplayed(struct tagcache_context *ctx, struct panel_vtop *vt, int i, const int32_t *s,
                          int32_t flag, uint32_t w)
{
    if (!w || (flag & TAGCACHE_UI_PODCAST))
        return;                 /* video podcasts: Podcasts' rows */
    if (is_tv(flag))
    {
        const char *show = str_of(ctx, s, tag_album);
        uint64_t key = show ? artcache_key(NULL, NULL, show, NULL) : 0;

        if (key && (vt->tv.n < vt->tv.max || w > vt->tv.when[vt->tv.n - 1]))
            top_add(&vt->tv, key, w);
    }
    else if (vt->mov.n < vt->mov.max || w > vt->mov.when[vt->mov.n - 1])
        top_add(&vt->mov, (uint64_t)i + 1, w);
}

/* Album i may join the video share, one entry per key */
static bool panel_vok(struct pre *p, size_t i, const int *sel, int n)
{
    const struct libfile_album *e = alb_at(p, i);
    int j;

    if (!(e->flags & LIBFILE_ALB_VIDEO) || (!e->key_lo && !e->key_hi))
        return false;
    for (j = 0; j < n; j++)
        if (sel[j] == (int)i)
            return false;
    return true;
}

/* The share: recently played (the two lists merged by time), then the
 * newest (mtime); -1 if the precompute must stop */
static int panel_videos(struct tagcache_context *ctx, struct pre *p, size_t a, const struct panel_vtop *vt,
                        int *sel)
{
    int n = 0, it = 0, im = 0;
    size_t k, best;

    while (n < PANEL_VIDEOS && (it < vt->tv.n || im < vt->mov.n))
    {
        bool tv = im >= vt->mov.n
            || (it < vt->tv.n && vt->tv.when[it] >= vt->mov.when[im]);
        uint64_t id = tv ? vt->tv.keys[it++] : vt->mov.keys[im++];

        for (k = 0; k < a; k++)
        {
            const struct libfile_album *e = alb_at(p, k);
            bool hit = tv ? ((e->flags & LIBFILE_ALB_TV)
                             && e->key_lo == (uint32_t)id
                             && e->key_hi == (uint32_t)(id >> 32))
                          : (!(e->flags & LIBFILE_ALB_TV)
                             && (uint64_t)e->rep + 1 == id);

            if (hit)
            {
                if (panel_vok(p, k, sel, n))
                    sel[n++] = k;
                break;
            }
        }
    }
    while (n < PANEL_VIDEOS)
    {
        best = a;
        for (k = 0; k < a; k++)
            if ((best == a || alb_at(p, k)->mtime > alb_at(p, best)->mtime)
                && panel_vok(p, k, sel, n))
                best = k;
        if (best == a)
            break;
        sel[n++] = best;
        if (!pre_step(ctx, p))
            return -1;
    }
    return n;
}
#endif

/* The album keys of the most recently played music tracks and audiobook
 * files, newest play first, one per album (top-K over one pass of the
 * master index; a key is computed only for a track that enters a list).
 * False if the precompute must stop. */
static bool panel_played(struct tagcache_context *ctx, struct pre *p, struct panel_top *music,
                         struct panel_top *books
#ifdef HAVE_BCMFW_VIDEO
                         , struct panel_vtop *vt
#endif
                         )
{
    int total = tagcache_ui_count_ctx(ctx), i;

    for (i = 0; i < total; i++)
    {
        int32_t flag;
        const int32_t *s;
        const char *album;
        struct panel_top *t;
        uint32_t w;
        uint64_t key;

        if (!pre_step(ctx, p))
            return false;
        s = tagcache_ui_seeks_ctx(ctx, i, &flag);
        if (!s)
            continue;
#ifdef HAVE_BCMFW_VIDEO
        if (flag & TAGCACHE_UI_VIDEO)
        {
            panel_vplayed(ctx, vt, i, s, flag, s[tag_lastplayed]);
            continue;
        }
#endif
        t = class_ok(C_MUSIC, flag) ? music
            : (class_ok(C_BOOK, flag) ? books : NULL);
        if (!t)
            continue;
        w = s[tag_lastplayed];
        if (!w || (t->n == t->max && w <= t->when[t->n - 1]))
            continue;
        album = str_of(ctx, s, tag_album);
        if (!album)
            continue;           /* untagged: keyed by folder, not here */
        key = artcache_key(str_of(ctx, s, tag_albumartist), str_of(ctx, s, tag_artist),
                           album, NULL);
        if (key)
            top_add(t, key, w);
    }
    return true;
}

/* Adds the albums of 'top' that are in the table (and may join) to sel[];
 * at most up to 'limit' entries in sel[] */
static int panel_add_played(struct pre *p, size_t a, const struct panel_top *t,
                            int *sel, int n, int limit, bool book)
{
    size_t k;
    int i;

    for (i = 0; i < t->n && n < limit; i++)
        for (k = 0; k < a; k++)
        {
            const struct libfile_album *e = alb_at(p, k);

            if (e->key_lo == (uint32_t)t->keys[i]
                && e->key_hi == (uint32_t)(t->keys[i] >> 32)
                && panel_ok(p, k, sel, n, book))
            {
                sel[n++] = k;
                break;
            }
        }
    return n;
}

/* Adds up to 'count' of the newest albums that may join; -1 if the
 * precompute must stop */
static int panel_add_newest(struct tagcache_context *ctx, struct pre *p, size_t a, int *sel, int n,
                            int count, int limit, bool book)
{
    size_t k, best;
    int i;

    for (i = 0; i < count && n < limit; i++)
    {
        best = a;
        for (k = 0; k < a; k++)
            if ((best == a || alb_at(p, k)->mtime > alb_at(p, best)->mtime)
                && panel_ok(p, k, sel, n, book))
                best = k;
        if (best == a)
            break;
        sel[n++] = best;
        if (!pre_step(ctx, p))
            return -1;
    }
    return n;
}

/* Adds random albums to sel[] (n of them already) up to 'limit': the
 * lowest scores of a hash of the key and 'seed' (rendezvous hashing), one
 * per key; -1 if the precompute must stop */
static int panel_rand(struct tagcache_context *ctx, struct pre *p, size_t a,
                      uint32_t seed, int *sel, int n, int limit, bool book)
{
    uint32_t score[LIBFILE_POOL_MUSIC];
    int cand[LIBFILE_POOL_MUSIC], nc = 0, room = limit - n, i;
    size_t k;

    if (room > LIBFILE_POOL_MUSIC)
        room = LIBFILE_POOL_MUSIC;
    for (k = 0; k < a && room > 0; k++)
    {
        const struct libfile_album *e = alb_at(p, k);
        uint32_t sc = (e->key_lo ^ seed) * 0x9e3779b1u
                    ^ (e->key_hi + seed) * 0x85ebca6bu;

        sc ^= sc >> 15;
        if ((nc == room && sc >= score[nc - 1])
            || !panel_ok(p, k, sel, n, book))
            continue;
        i = nc < room ? nc++ : nc - 1;
        for (; i > 0 && score[i - 1] > sc; i--)
        {
            score[i] = score[i - 1];
            cand[i] = cand[i - 1];
        }
        score[i] = sc;
        cand[i] = k;
        if (!pre_step(ctx, p))
            return -1;
    }
    for (i = 0; i < nc; i++)
        sel[n++] = cand[i];
    return n;
}

/* #100: col 1 carries the pool order (the books count, the tag, the
 * count: 3 words at least) */
static bool pre_pool_ok(const struct libfile_sec *s)
{
    return s && s->count >= 3;
}

/* #100: col 1's pool order after the books count: the pinned (sel[0,
 * n_pin): played and newest music; then the videos and the books of the
 * default set), then random music and books by a fixed seed. Uses the
 * caller's arrays as scratch past their default-set entries. */
#define POOL_SEED 0x2545f491u
static bool pre_pool(struct tagcache_context *ctx, struct pre *p, size_t a,
                     const int *sel, int n_pin, const int *xsel, int nx,
                     const int *bsel, int nb)
{
    int pool[LIBFILE_POOL_MUSIC], bpool[LIBFILE_POOL_BOOKS];
    int nm, nbk, i;
    uint32_t w[2];

    memcpy(pool, sel, n_pin * sizeof(int));
    nm = panel_rand(ctx, p, a, POOL_SEED, pool, n_pin, LIBFILE_POOL_MUSIC,
                    false);
    memcpy(bpool, bsel, nb * sizeof(int));
    nbk = nm < 0 ? -1 : panel_rand(ctx, p, a, POOL_SEED, bpool, nb,
                                   LIBFILE_POOL_BOOKS, true);
    if (nbk < 0)
        return false;
    w[0] = LIBFILE_POOL_TAG;
    w[1] = nm + nx + nbk;
    if (!pre_put(p, w, sizeof(w)))
        return false;
    for (i = 0; i < nm + nx + nbk; i++)
    {
        int at = i < nm ? pool[i] : i < nm + nx ? xsel[i - nm]
                                                : bpool[i - nm - nx];
        struct libfile_pool e;

        e.a = *alb_at(p, at);
        e.pinned = i < nm ? i < n_pin : i < nm + nx ? 1 : i - nm - nx < nb;
        if (!pre_put(p, &e, sizeof(e)))
            return false;
    }
    return true;
}

/* Writes the section from the 'a' album entries at the block's start
 * (pre_albumtab()): music entries first, then books. False if the
 * precompute must stop. */
static bool pre_panels(struct tagcache_context *ctx, struct pre *p, size_t a)
{
    struct panel_top music = { .max = PANEL_PLAYED },
                     books = { .max = PANEL_BOOKS };
    int sel[LIBFILE_PANELS_MAX], bsel[PANEL_BOOKS];
    int n = 0, nb = 0, i, music_max, n_pin;
    uint32_t seed;
#ifdef HAVE_BCMFW_VIDEO
    struct panel_vtop vt = { .tv = { .max = PANEL_VIDEOS },
                             .mov = { .max = PANEL_VIDEOS } };
    int vsel[PANEL_VIDEOS], nv;

    if (!panel_played(ctx, p, &music, &books, &vt))
        return false;
    nv = panel_videos(ctx, p, a, &vt, vsel);
    if (nv < 0)
        return false;
#else
    if (!panel_played(ctx, p, &music, &books))
        return false;
#endif
    /* books: recently played, then the newest (their own list: one per
     * key holds across the two lists, a key is a book or not) */
    nb = panel_add_played(p, a, &books, bsel, 0, PANEL_BOOKS, true);
    nb = panel_add_newest(ctx, p, a, bsel, nb, PANEL_BOOKS, PANEL_BOOKS, true);
    if (nb < 0)
        return false;
    music_max = LIBFILE_PANELS_MAX - nb;
#ifdef HAVE_BCMFW_VIDEO
    music_max -= nv;
#endif
    /* 1. recently played */
    n = panel_add_played(p, a, &music, sel, 0, music_max, false);
    /* 2. the newest */
    n = panel_add_newest(ctx, p, a, sel, n, PANEL_ADDED, music_max, false);
    if (n < 0)
        return false;
    /* 3. random ones: the lowest scores of a hash of the key and an epoch
     * that moves every 8 commits (rendezvous hashing), so adding or
     * removing other albums leaves them alone and the art builder
     * redoes few panels per update */
    seed = ((uint32_t)tagcache_ui_commitid_ctx(ctx) >> 3) * 2654435761u;
    n_pin = n;
    n = panel_rand(ctx, p, a, seed, sel, n, music_max, false);
    if (n < 0)
        return false;
#ifdef HAVE_BCMFW_VIDEO
    /* the videos before the books: col 1 still counts col 0's last
     * entries as books; the art builder sorts by each entry's flags */
    for (i = 0; i < nv; i++)
        sel[n++] = vsel[i];
#endif
    for (i = 0; i < nb; i++)
        sel[n++] = bsel[i];

    if (!pre_sec_begin(p, LIBFILE_CAT_PANELSET, 0,
                       sizeof(struct libfile_album)))
        return false;
    for (i = 0; i < n; i++)
    {
        struct libfile_album e = *alb_at(p, sel[i]);

        if (!pre_put(p, &e, sizeof(e)))
            return false;
    }
    pre_sec_end(p);
    /* col 1 (#58): how many of col 0's entries (its last ones) are books.
     * A file without it is from before the book panels: pre_current(ctx, )
     * has it rewritten once. #100: then the pool order (pre_pool()) */
    {
        uint32_t books_n = nb;

        if (!pre_sec_begin(p, LIBFILE_CAT_PANELSET, 1, sizeof(books_n))
            || !pre_put(p, &books_n, sizeof(books_n))
#ifdef HAVE_BCMFW_VIDEO
            || !pre_pool(ctx, p, a, sel, n_pin, vsel, nv, bsel, nb))
#else
            || !pre_pool(ctx, p, a, sel, n_pin, NULL, 0, bsel, nb))
#endif
            return false;
        pre_sec_end(p);
    }
    return true;
}

#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a (3.5): a show's art comes from its first episode (season,
 * episode order: the LIB_TVSHOWS TRACKS section's first entry of the
 * show), not from its smallest master index, which is whichever file the
 * scanner met first. Two 4-byte reads per show; a show with no TRACKS
 * entry keeps its representative. The album table's rep of a show is
 * only read by the art builder (no filtered view looks shows up). */
static void pre_tv_reps(struct tagcache_context *ctx, struct pre *p, size_t first, size_t n)
{
    const struct libfile_sec *ts = pre_sec(pre_hdr(p), LIB_TVSHOWS,
                                           LIBFILE_TRACKS, 4);
    size_t i;

    if (!ts || ts->count < LIBFILE_TRACKS_HDR(n))
        return;
    for (i = 0; i < n; i++)
    {
        struct libfile_album *e;
        uint32_t st[2];
        int32_t idx;

        if (lseek(p->fd, ts->offset + i * 4, SEEK_SET) < 0
            || read(p->fd, st, sizeof(st)) != sizeof(st)
            || st[0] >= st[1] || st[1] > ts->count
            || lseek(p->fd, ts->offset + st[0] * 4, SEEK_SET) < 0
            || read(p->fd, &idx, sizeof(idx)) != sizeof(idx)
            || !tagcache_ui_seeks_ctx(ctx, idx, NULL))
            continue;
        /* the block may have moved during the reads */
        e = (struct libfile_album *)bx_base(&p->c) + first + i;
        e->rep = idx;
    }
}
#endif

/* -- #47: each artist's albums (LIB_ARTISTS, LIBFILE_ALBUMS) --------------- */

/* Group key order of artist row t (key prefix, rep) against 'key' (the
 * full ui_groupkey() of a label, prefix k1/k2). */
static int pre_key_cmp(struct tagcache_context *ctx, const struct rec *t,
                       uint32_t k1, uint32_t k2, const char *key)
{
    char kt[KEY_LEN];

    if (t->k1 != k1)
        return t->k1 < k1 ? -1 : 1;
    if (t->k2 != k2)
        return t->k2 < k2 ? -1 : 1;
    ui_groupkey(label_of(ctx, G_ARTIST, t->idx), kt, sizeof(kt));
    return strcmp(kt, key);
}

/* The artist row (of the r rows recorded at 'off': key prefix, rep) that
 * album rep 'rep' belongs to: -1 none, -2 more than one. The rows are the
 * str_eq() classes of the album artist (else the artist) in cmp_labels()
 * order, whose first level is ui_groupkey(), equal for every name of one
 * class (sortkey.h); and an album's tracks share one class
 * (same_album()). So the lower bound of the album's group key starts the
 * run of rows with that key (any length: "Elan", "Élan", "The Elan" ...
 * share it), and same_artist(), the scan's own test, picks the row. */
static int pre_artist_of(struct tagcache_context *ctx, struct pre *p,
                         size_t off, int r, int32_t rep)
{
    const int32_t *s = tagcache_ui_seeks_ctx(ctx, rep, NULL);
    char key[KEY_LEN];
    uint32_t k1, k2;
    int lo = 0, hi = r, c, found = -1;

    if (!s)
        return -1;
    ui_groupkey(label_of(ctx, G_ARTIST, rep), key, sizeof(key));
    key_prefix(label_of(ctx, G_ARTIST, rep), &k1, &k2);
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;

        if (pre_key_cmp(ctx, &recs_at(&p->c, off)[mid], k1, k2, key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (c = lo; c < r
         && !pre_key_cmp(ctx, &recs_at(&p->c, off)[c], k1, k2, key); c++)
    {
        const int32_t *a = tagcache_ui_seeks_ctx(ctx,
                               recs_at(&p->c, off)[c].idx, NULL);

        if (a && same_artist(ctx, s, a))
        {
            if (found >= 0)
                return -2;
            found = c;
        }
    }
    return found;
}

/* Whether any of the r artist rows takes album rep 'rep' (the check of a
 * -1 from pre_artist_of(): a compilation-only artist has no row; a row
 * found here means the grouping and the order disagree). L4: through a
 * table of the rows by ui_casefold_hash() of their labels ('tab', 'size'
 * a power of two, row + 1 per slot, 0 empty), built once per pass. */
static uint32_t pre_tab_get(struct pre *p, size_t tab, uint32_t slot,
                            uint32_t i)
{
    char *b = bx_base(&p->c) + tab;

    return slot == 2 ? ((uint16_t *)b)[i] : ((uint32_t *)b)[i];
}

static void pre_tab_set(struct pre *p, size_t tab, uint32_t slot,
                        uint32_t i, uint32_t v)
{
    char *b = bx_base(&p->c) + tab;

    if (slot == 2)
        ((uint16_t *)b)[i] = (uint16_t)v;
    else
        ((uint32_t *)b)[i] = v;
}

static bool pre_artist_any(struct tagcache_context *ctx, struct pre *p,
                           size_t off, size_t tab, uint32_t size,
                           uint32_t slot, int32_t rep)
{
    const int32_t *s = tagcache_ui_seeks_ctx(ctx, rep, NULL);
    uint32_t i;

    if (!s)
        return false;
    for (i = ui_casefold_hash(label_of(ctx, G_ARTIST, rep)) & (size - 1);;
         i = (i + 1) & (size - 1))
    {
        uint32_t row = pre_tab_get(p, tab, slot, i);
        const int32_t *a;

        if (!row)
            return false;
        a = tagcache_ui_seeks_ctx(ctx, recs_at(&p->c, off)[row - 1].idx,
                                  NULL);
        if (a && same_artist(ctx, s, a))
            return true;
    }
}

/* Writes the LIB_ARTISTS ALBUMS section (libfile.h) from the album
 * entries pre_albumtab() left at the block's start (the LIB_ALBUMS rows
 * first, in list order) and the artist rows read back, in the block from
 * 'work' (the album table's index, free again):
 *   work   int32 art[A]    artist row of album row j (-1: none)
 *          int32 perm[A]   album rows grouped by artist, in list order
 *          rec   ar[R]     artist key prefix and rep; then uint32
 *                          end[R] over it (each artist's group end)
 *          tab[]           rows by label hash (pre_artist_any(): 2 B
 *                          slots, load <= 3/4; skipped without room)
 * 8 B per album and 12 B per artist beyond the table (+ ~2.7 B per
 * artist for the check); a block without
 * that room (every track its own album) writes the section empty and the
 * reader scans. Two album rows of one artist that lib_open() would merge (case
 * variants split in the full list by another artist's album of the same
 * name) keep the first, as the filtered scan does. */
static bool pre_artist_albums(struct tagcache_context *ctx, struct pre *p,
                              size_t work)
{
    const struct libfile_sec *as = pre_sec(pre_hdr(p), LIB_ARTISTS,
                                           LIBFILE_ROWS, sizeof(int32_t));
    const struct libfile_sec *bs = pre_sec(pre_hdr(p), LIB_ALBUMS,
                                           LIBFILE_ROWS, sizeof(int32_t));
    size_t perm_off, ar_off, tab_off;
    uint32_t na, nr, i, j, run, bad, tsize, tslot;
    int32_t none_seek = -1;
    int none_tag = -1;

#define ART(k)  (((int32_t *)(bx_base(&p->c) + work))[k])
#define PERM(k) (((int32_t *)(bx_base(&p->c) + perm_off))[k])
#define END(k)  (((uint32_t *)(bx_base(&p->c) + ar_off))[k])
    nr = as ? as->count : 0;
    na = bs ? bs->count : 0;
    perm_off = work + (size_t)na * sizeof(int32_t);
    ar_off = perm_off + (size_t)na * sizeof(int32_t);
    tab_off = ar_off + (size_t)nr * sizeof(struct rec);
    /* the check's table: load at most 3/4, 2-byte slots (4 above 65,534
     * artists); without room the check is skipped, never the section
     * (50k artists: 128 KB) */
    for (tsize = 16; tsize < nr + nr / 3 + 1; tsize *= 2)
        ;
    tslot = nr < 0xffff ? 2 : 4;
    if (tab_off + (size_t)tsize * tslot > p->hdr_off)
    {
        logf("lib: precompute: artists' albums unchecked (%lu artists)",
             (unsigned long)nr);
        tsize = 0;
    }
    if (!nr || ar_off + (size_t)nr * sizeof(struct rec) > p->hdr_off)
    {
        logf("lib: precompute: no room for %lu artists' albums",
             (unsigned long)nr);
        goto empty;
    }
    /* the artist reps to the last third of the records' room, then the
     * records from the front: record i (12 B at 12i) ends before rep
     * i + 1 (4 B at 8R + 4(i + 1)) */
    if (!pre_load(p, LIB_ARTISTS, LIBFILE_ROWS, sizeof(int32_t),
                  ar_off + (size_t)nr * 8, nr))
        return false;
    for (i = 0; i < nr; i++)
    {
        int32_t rep = ((int32_t *)(bx_base(&p->c) + ar_off + nr * 8))[i];
        struct rec t;

        if (!pre_step(ctx, p))
            return false;
        key_prefix(label_of(ctx, G_ARTIST, rep), &t.k1, &t.k2);
        t.idx = rep;
        recs_at(&p->c, ar_off)[i] = t;
    }
    if (tsize)
        memset(bx_base(&p->c) + tab_off, 0, (size_t)tsize * tslot);
    for (i = 0; tsize && i < nr; i++)
    {
        uint32_t k = ui_casefold_hash(label_of(ctx, G_ARTIST,
                         recs_at(&p->c, ar_off)[i].idx)) & (tsize - 1);

        while (pre_tab_get(p, tab_off, tslot, k))
            k = (k + 1) & (tsize - 1);
        pre_tab_set(p, tab_off, tslot, k, i + 1);
    }
    for (j = 0, bad = 0; j < na; j++)
    {
        int32_t rep = ((struct libfile_album *)bx_base(&p->c))[j].rep;
        const int32_t *s;
        int t;

        if (!pre_step(ctx, p))
            return false;
        ART(j) = pre_artist_of(ctx, p, ar_off, nr, rep);
        if (ART(j) != -1)
        {
            bad += ART(j) < 0;
            continue;
        }
        /* no row: checked once per artist identity (a run of one
         * compilation artist's albums costs one walk) */
        s = tagcache_ui_seeks_ctx(ctx, rep, NULL);
        if (!s)
            continue;
        t = artist_tag(ctx, &p->c, s);
        if (s[t] == none_seek && t == none_tag)
            continue;
        if (tsize && pre_artist_any(ctx, p, ar_off, tab_off, tsize, tslot,
                                    rep))
            bad++;
        none_seek = s[t];
        none_tag = t;
    }
    if (bad)
    {
        /* a safety net for a broken invariant, never an expected path:
         * the readers scan */
        UPDATE_TRACE("lib precompute artist albums unassigned=%lu",
                     (unsigned long)bad);
        goto empty;
    }
    /* counting sort by artist (stable: list order within an artist) */
    memset(&END(0), 0, (size_t)nr * sizeof(uint32_t));
    for (j = 0; j < na; j++)
        if (ART(j) >= 0)
            END(ART(j))++;
    for (i = 0, run = 0; i < nr; i++)
    {
        run += END(i);
        END(i) = run - END(i);          /* the group's start, as a cursor */
    }
    for (j = 0; j < na; j++)
        if (ART(j) >= 0)
            PERM(END(ART(j))++) = j;    /* the cursors end at group ends */
    /* the merges lib_open() makes, marked art = -1 */
    for (i = 0, run = 0; i < nr; run = END(i++))
    {
        int32_t keep = -1;

        for (j = run; j < END(i); j++)
        {
            int32_t rep = ((struct libfile_album *)bx_base(&p->c))
                          [PERM(j)].rep;

            if (!pre_step(ctx, p))
                return false;
            if (keep >= 0 && same_group(ctx, G_ALBUM, keep, rep))
                ART(PERM(j)) = -1;
            else
                keep = rep;
        }
    }

    if (!pre_sec_begin(p, LIB_ARTISTS, LIBFILE_ALBUMS, sizeof(uint32_t)))
        return false;
    for (i = 0, j = 0, run = 0; i <= nr; i++)
    {
        uint32_t v = LIBFILE_ARTALB_HDR(nr) + LIBFILE_ARTALB_WORDS * run;

        if (!pre_put(p, &v, sizeof(v)))
            return false;
        for (; i < nr && j < END(i); j++)
            run += ART(PERM(j)) >= 0;
    }
    for (j = 0; j < (nr ? END(nr - 1) : 0); j++)
    {
        const struct libfile_album *e;
        struct libfile_artalb o;

        if (!pre_step(ctx, p))
            return false;
        if (ART(PERM(j)) < 0)
            continue;
        e = (const struct libfile_album *)bx_base(&p->c) + PERM(j);
        o.rep = e->rep;
        o.count = e->count;
        o.key_lo = e->key_lo;
        o.key_hi = e->key_hi;
        if (!pre_put(p, &o, sizeof(o)))
            return false;
    }
#undef END
#undef PERM
#undef ART
    pre_sec_end(p);
    return true;

empty:  /* the readers scan; pre_current() takes the file */
    if (!pre_sec_begin(p, LIB_ARTISTS, LIBFILE_ALBUMS, sizeof(uint32_t)))
        return false;
    pre_sec_end(p);
    return true;
}

/* LIBFILE_CAT_ALBUMTAB: albums, books and shows, sorted by representative,
 * assembled from their columns (read back) at the start of the block. */
static bool pre_albumtab(struct tagcache_context *ctx, struct pre *p)
{
    static const int cats[] = { LIB_ALBUMS, LIB_AUDIOBOOKS, LIB_PODCASTS
#ifdef HAVE_BCMFW_VIDEO
                                , LIB_MOVIES, LIB_TVSHOWS
#endif
    };
    static const uint32_t flags[] = {
        0, LIBFILE_ALB_BOOK, LIBFILE_ALB_PODCAST
#ifdef HAVE_BCMFW_VIDEO
        , LIBFILE_ALB_VIDEO, LIBFILE_ALB_VIDEO | LIBFILE_ALB_TV
#endif
    };
    size_t a = 0, idx_off, room = p->hdr_off;
    unsigned c, i;

    for (c = 0; c < ARRAYLEN(cats); c++)
    {
        struct libfile_hdr *hd = pre_hdr(p);
        for (i = 0; i < hd->nsec; i++)
            if (hd->sec[i].cat == cats[c] && hd->sec[i].col == LIBFILE_ROWS)
                a += hd->sec[i].count;
    }
    idx_off = ALIGN_UP(a * sizeof(struct libfile_album), 4);
    if (idx_off + a * sizeof(struct rec) > room)
    {
        logf("lib: precompute: no room for %ld albums", (long)a);
        return true;            /* no table: counts/keys of filtered views
                                 * stay unknown */
    }
    if (!pre_flush(p))
        return false;
    a = 0;
    for (c = 0; c < ARRAYLEN(cats); c++)
    {
        const struct libfile_sec *rs = pre_sec(pre_hdr(p), cats[c],
                                               LIBFILE_ROWS, 4);
        size_t n = rs ? rs->count : 0;

        if (!n)
            continue;
#ifdef HAVE_BCMFW_VIDEO
        /* a movie is one file: no COUNT or MTIME sections (3.3) */
        if (cats[c] == LIB_MOVIES)
        {
            if (!pre_readback(p, cats[c], LIBFILE_ROWS, a,
                              offsetof(struct libfile_album, rep))
                || !pre_readback(p, cats[c], LIBFILE_KEY, a,
                                 offsetof(struct libfile_album, key_lo)))
                return false;
            for (i = 0; i < n; i++)
            {
                struct libfile_album *e =
                    (struct libfile_album *)bx_base(&p->c) + a + i;
                const int32_t *s = tagcache_ui_seeks_ctx(ctx, e->rep, NULL);

                e->count = 1;
                e->mtime = s ? (uint32_t)s[tag_mtime] : 0;
                e->flags = flags[c];    /* keyed by its path, not folder */
            }
            a += n;
            continue;
        }
#endif
        if (!pre_readback(p, cats[c], LIBFILE_ROWS, a,
                          offsetof(struct libfile_album, rep))
            || !pre_readback(p, cats[c], LIBFILE_KEY, a,
                             offsetof(struct libfile_album, key_lo))
            || !pre_readback(p, cats[c], LIBFILE_COUNT, a,
                             offsetof(struct libfile_album, count))
            || !pre_readback(p, cats[c], LIBFILE_MTIME, a,
                             offsetof(struct libfile_album, mtime)))
            return false;
        for (i = 0; i < n; i++)
        {
            struct libfile_album *e =
                (struct libfile_album *)bx_base(&p->c) + a + i;
            const int32_t *s = tagcache_ui_seeks_ctx(ctx, e->rep, NULL);

            e->flags = flags[c];
            if (s && !str_of(ctx, s, tag_album))
                e->flags |= LIBFILE_ALB_DIRKEY;
        }
#ifdef HAVE_BCMFW_VIDEO
        if (cats[c] == LIB_TVSHOWS)
            pre_tv_reps(ctx, p, a, n);
#endif
        a += n;
    }
    if (lseek(p->fd, p->pos, SEEK_SET) != (off_t)p->pos)
        return false;

    for (i = 0; i < a; i++)
    {
        struct rec *r = &recs_at(&p->c, idx_off)[i];
        r->k1 = ((struct libfile_album *)bx_base(&p->c))[i].rep;
        r->k2 = 0;
        r->idx = i;
    }
    p->c.group = G_NONE;
    p->c.order = -1;
    if (!sort_recs(ctx, &p->c, idx_off, a))
        return false;
    if (!pre_sec_begin(p, LIBFILE_CAT_ALBUMTAB, 0,
                       sizeof(struct libfile_album)))
        return false;
    for (i = 0; i < a; i++)
    {
        struct libfile_album e;

        if (!pre_step(ctx, p))
            return false;
        e = ((struct libfile_album *)bx_base(&p->c))
            [recs_at(&p->c, idx_off)[i].idx];
        if (!pre_put(p, &e, sizeof(e)))
            return false;
    }
    pre_sec_end(p);
    return pre_panels(ctx, p, a) && pre_artist_albums(ctx, p, idx_off);
}

#ifdef LIB_P11
/* -- Phase 11: Search, On-The-Go views, library stats ------------------- */

/* A track view of n rows (filled by the caller), stacked on the pool. */
static int track_view(int n, int cat, int cls)
{
    struct view *v;
    int h, i;

    pool_sync();
    h = slot_new(sizeof(struct view) + (size_t)n * sizeof(int32_t));
    if (h <= 0)
        return -1;
    v = get_view(h);
    memset(v, 0, sizeof(*v));
    view_layout(v, n, 0);
    v->cat = cat;
    v->group = G_NONE;
    v->order = O_TITLE;
    v->cls = cls;
    v->sub = SUB_NONE;
    v->generation = tagcache_ui_generation();
    v->count = n;
    v->live_row = -1;
    for (i = 0; i < LIB_CACHE_ROWS; i++)
        v->cache_row[i] = -1;
    slots[h - 1].count = n;
    return h;
}

/* (Re)starts the label caches of view v after its rows changed. */
static void view_caches_reset(struct view *v)
{
    int i;

    for (i = 0; i < LIB_CACHE_ROWS; i++)
        v->cache_row[i] = -1;
    if (v->off_sub)
    {
        struct subcache *sc = view_ext(v, v->off_sub);
        for (i = 0; i < LIB_CACHE_ROWS; i++)
            sc->row[i] = -1;
    }
}

static int lib_row_tracks_held(int view, int idx)
{
    struct lib_filter f = LIB_FILTER_ANY;
    struct view *v = get_view(view);
    const char *src;
    int t, rep, h;

    if (!v || idx < 0 || idx >= v->count || !view_fresh(v))
        return -1;
    t = row_track(v, idx);
    if (t >= 0)
    {
        h = track_view(1, LIB_SONGS, C_ANY);
        if (h > 0)
            get_view(h)->rows[0] = t;
        return h;
    }
    if (v->cat == LIB_OTG)
        return -1;
    if (v->cat == LIB_SEARCH)
    {
        uint32_t r = v->rows[idx];
        rep = SRCH_ROW_IDX(r);
        switch (SRCH_ROW_KIND(r))
        {
        case SRCH_ARTIST:   f.artist = rep; break;
        case SRCH_ALBUM:
        case SRCH_BOOK:     f.album = rep; break;
        default:            return -1;
        }
    }
    else
    {
        rep = v->rows[idx];
        switch (v->group)
        {
        case G_ARTIST:   f.artist = rep; break;
        case G_ALBUM:    f.album = rep; break;
        case G_GENRE:    f.genre = rep; break;
        default:         f.composer = rep; break;
        }
    }
    h = build(LIB_SONGS, &f, &src);
    return h > 0 ? h : -1;
}

int lib_row_tracks(int view, int idx)
{
    struct tagcache_context *held = tagcache_context_acquire();
    int result = lib_row_tracks_held(view, idx);
    tagcache_context_release(held);
    return result;
}

/* -- Search (docs/phase11-brief.md 3.1) --
 * Sources, each already in ui_groupkey() order (#47): the artist, album and
 * book rows of the precompute file (read once at entry into one pool
 * slot, 'srows'), and the title tag file walked in stored order (songs
 * and episodes; book chapters are not searched, their books are). A
 * k-way merge on 8-byte key prefixes writes SRCH_ROW(kind, idx) rows
 * into THE result view, the newest slot of the pool, sized to what is
 * left of the pool while audio runs (never grown then), or to every
 * candidate while stopped. Statics: ~56 B (UI thread only). */
#define SRCH_GROUPS 3
struct srows {
    uint32_t n[SRCH_GROUPS];    /* rows of each group kind */
    uint32_t pad;
    int32_t  rows[];            /* artists, then albums, then books */
};
static const int8_t srch_cat[SRCH_GROUPS] = {
    LIB_ARTISTS, LIB_ALBUMS, LIB_AUDIOBOOKS
};
static const int8_t srch_gkind[SRCH_GROUPS] = {
    SRCH_ARTIST, SRCH_ALBUM, SRCH_BOOK
};
static const int8_t srch_ggroup[SRCH_GROUPS] = { G_ARTIST, G_ALBUM, G_ALBUM };
/* the result view's fixed part: header, second-line cache, 256 rows */
#define SRCH_VIEW_MIN (sizeof(struct view) + sizeof(struct subcache) + 1024)

static struct {
    int rows_h, view_h;         /* pool slots (0: none) */
    int gen;                    /* tagcache generation they are for */
    bool partial;               /* group kinds skipped (no file, no room) */
    bool capped;                /* the last build dropped rows */
    struct srch_query last;     /* the query the view holds */
} srch = { 0, 0, -1, false, false, { "", 0 } };

static struct srows *srows_get(void)
{
    return srch.rows_h > 0 ? (struct srows *)(void *)get_view(srch.rows_h)
                           : NULL;
}

void lib_search_end(void)
{
    if (srch.view_h > 0)
        slot_free(srch.view_h);
    if (srch.rows_h > 0)
        slot_free(srch.rows_h);
    srch.view_h = srch.rows_h = 0;
    srch.gen = -1;
    srch.partial = srch.capped = false;
    srch.last.len = 0;
    srch.last.folded[0] = '\0';
}

static int lib_search_begin_held(void)
{
    struct libfile_sec sec[SRCH_GROUPS];
    const struct libfile_hdr *hd;
    size_t need = sizeof(struct srows), off;
    int th, fd, k, h;
    int reads UNUSED_ATTR = 0;

    lib_search_end();
    srch.gen = tagcache_ui_generation();
    srch.partial = true;
    if (tagcache_ui_count() < 0)
        return -1;
    pool_sync();
    th = slot_new(LIBFILE_HDR_SIZE);
    if (th <= 0)
        return -1;
    fd = pre_ui_open(th);
    if (fd < 0)
    {
        slot_free(th);
        UPDATE_TRACE("search begin rows=0,0,0 bytes=0 reads=%d partial=1",
                     1);
        return -1;
    }
    reads++;
    hd = (const struct libfile_hdr *)get_view(th);
    memset(sec, 0, sizeof(sec));
    for (k = 0; k < SRCH_GROUPS; k++)
    {
        const struct libfile_sec *p = pre_sec(hd, srch_cat[k], LIBFILE_ROWS,
                                              sizeof(int32_t));
        if (p)
            sec[k] = *p;
    }
    slot_free(th);
    /* what fits: while audio runs only the pool's slack, leaving the
     * result view its minimum (P6 Rev 1: no growth while playing) */
    srch.partial = false;
    for (k = 0; k < SRCH_GROUPS; k++)
    {
        size_t b = (size_t)sec[k].count * sizeof(int32_t);

        if (audio_status() && SLOT_ALIGN(need + b) + SRCH_VIEW_MIN
            > (size_t)(pool.size - pool.used))
        {
            sec[k].count = 0;
            srch.partial = true;
            continue;
        }
        need += b;
    }
    h = slot_new(need);
    if (h <= 0)
    {
        close(fd);
        srch.partial = true;
        return -1;
    }
    off = sizeof(struct srows);
    for (k = 0; k < SRCH_GROUPS; k++)
    {
        uint32_t n = sec[k].count;

        if (n && !pre_ui_read(fd, h, off, &sec[k]))
        {
            for (; k < SRCH_GROUPS; k++)    /* the rest would be shifted */
                ((struct srows *)(void *)get_view(h))->n[k] = 0;
            srch.partial = true;
            break;
        }
        reads += n > 0;
        ((struct srows *)(void *)get_view(h))->n[k] = n;
        off += (size_t)n * sizeof(int32_t);
    }
    close(fd);
    srch.rows_h = h;
    {
        struct srows *r UNUSED_ATTR = srows_get();
        UPDATE_TRACE("search begin rows=%lu,%lu,%lu bytes=%lu reads=%d "
                     "partial=%d", (unsigned long)r->n[0],
                     (unsigned long)r->n[1], (unsigned long)r->n[2],
                     (unsigned long)off, reads, (int)srch.partial);
    }
    return h;
}

int lib_search_begin(void)
{
    struct tagcache_context *held = tagcache_context_acquire();
    int result = lib_search_begin_held();
    tagcache_context_release(held);
    return result;
}

/* One merge source: its next matching row (head) and that row's key. */
struct srch_src {
    int8_t   kind;              /* of the head */
    int8_t   live;              /* 0: exhausted */
    int8_t   g;                 /* group source 0..2, or -1: the titles */
    uint32_t pos, end;          /* group: rows; titles: walk position */
    int32_t  head;
    uint32_t k1, k2;
};

struct srch_run {
    const struct srch_query *q;
    bool (*cancel)(void);
    int gen, steps, yields;
};

/* Every YIELD_EVERY entries: a yield; false to abort (a new key, or the
 * database changed: its pointers died). */
static bool srch_step(struct srch_run *r)
{
    if (++r->steps < YIELD_EVERY)
        return true;
    r->steps = 0;
    r->yields++;
    yield();
    return tagcache_ui_generation() == r->gen && tagcache_ui_count() >= 0
        && !(r->cancel && r->cancel());
}

/* Advances s to its next match: 1 found, 0 exhausted, -1 abort. */
static int srch_advance(struct srch_run *r, struct srch_src *s)
{
    for (;;)
    {
        const char *label;
        int32_t idx, flag;
        int kind;

        if (!srch_step(r))
            return -1;
        if (s->g >= 0)
        {
            struct srows *sr = srows_get();
            if (!sr || s->pos >= s->end)
                break;
            idx = sr->rows[s->pos++];
            label = label_of(tagcache_context_active(), srch_ggroup[(int)s->g], idx);
            if (!label || label == lib_unknown_artist
                || label == lib_unknown_album)
                continue;
            kind = srch_gkind[(int)s->g];
        }
        else
        {
            const int32_t *sk;
            int32_t pos = s->pos;

            if (!tagcache_ui_walk(tag_title, &pos, &idx))
                break;
            s->pos = pos;
            sk = tagcache_ui_seeks(idx, &flag);
            if (!sk || (flag & TAGCACHE_UI_AUDIOBOOK))
                continue;
            if (flag & TAGCACHE_UI_ANY_VIDEO)
                continue;       /* 3.2a ⚠️ T9: no video in Search */
            label = str_of(tagcache_context_active(), sk, tag_title);
            if (!label)
                continue;
            kind = (flag & TAGCACHE_UI_PODCAST) ? SRCH_EPISODE : SRCH_SONG;
        }
        if (!srch_match(label, r->q))
            continue;
        s->head = idx;
        s->kind = kind;
        key_prefix(label, &s->k1, &s->k2);
        return 1;
    }
    s->live = 0;
    return 0;
}

/* The result view's header, after its n rows are in place. */
static void srch_finish(int h, int n)
{
    struct view *v = get_view(h);

    v->cat = LIB_SEARCH;
    v->group = G_NONE;
    v->order = O_TITLE;
    v->cls = C_ANY;
    v->sub = SUB_NONE;          /* per row: lib_sublabel() */
    v->by_author = 0;
    v->generation = srch.gen;
    v->count = n;
    v->live_row = -1;
    v->off_names = 0;
    slot_resize(h, view_layout(v, n, X_SUB));
    v = get_view(h);
    view_caches_reset(v);
    slots[h - 1].count = n;
}

/* Rebuilds the result view from every source. Returns its rows, or < 0
 * (-1 aborted, -2 no memory). */
static int srch_build(struct srch_run *r, int *counts, bool *capped)
{
    struct srch_src src[SRCH_GROUPS + 1];
    struct srows *sr = srows_get();
    size_t want, room;
    int h, cap, n = 0, i, k;
    uint32_t start = 0;

    if (srch.view_h > 0)
        slot_free(srch.view_h);
    srch.view_h = 0;
    pool_sync();
    want = SRCH_VIEW_MIN + (size_t)tagcache_ui_count() * sizeof(int32_t);
    for (k = 0; sr && k < SRCH_GROUPS; k++)
        want += (size_t)sr->n[k] * sizeof(int32_t);
    if (!r->q->len)
        want = SRCH_VIEW_MIN;           /* nothing to list */
    if (audio_status())
    {
        room = pool.handle > 0 ? (size_t)(pool.size - pool.used) & ~7u : 0;
        want = MIN(want, room);
    }
    if (want < SRCH_VIEW_MIN)
        return -2;
    h = slot_new(want);
    if (h <= 0)
        return -2;
    srch.view_h = h;
    cap = (want - sizeof(struct view) - sizeof(struct subcache))
          / sizeof(int32_t);

    /* sources, in tie order: artists, albums, titles, books */
    memset(src, 0, sizeof(src));
    for (i = 0, k = 0; i < SRCH_GROUPS + 1; i++)
    {
        struct srch_src *s = &src[i];

        s->live = 1;
        if (i == 2)
        {
            s->g = -1;          /* the title walk */
            continue;
        }
        s->g = k;
        s->pos = start;
        s->end = start + (sr ? sr->n[k] : 0);
        start = s->end;
        k++;
    }
    for (i = 0; r->q->len && i < SRCH_GROUPS + 1; i++)
        if (srch_advance(r, &src[i]) < 0)
            goto abort;
    for (;;)
    {
        struct srch_src *best = NULL;

        for (i = 0; r->q->len && i < SRCH_GROUPS + 1; i++)
        {
            struct srch_src *s = &src[i];
            if (s->live && (!best || s->k1 < best->k1
                            || (s->k1 == best->k1 && s->k2 < best->k2)))
                best = s;
        }
        if (!best)
            break;
        if (n == cap)
        {
            *capped = true;
            break;
        }
        get_view(h)->rows[n++] = SRCH_ROW(best->kind, best->head);
        counts[(int)best->kind]++;
        if (srch_advance(r, best) < 0)
            goto abort;
    }
    srch_finish(h, n);
    return n;
abort:
    srch_finish(h, n);          /* a consistent (partial) view */
    return -1;
}

/* Refines the result view in place: only rows matching q stay. */
static int srch_refine(struct srch_run *r, int *counts)
{
    int h = srch.view_h, n = slots[h - 1].count, i, kept = 0;

    for (i = 0; i < n; i++)
    {
        uint32_t row;
        const char *label;

        if (!srch_step(r))
        {
            srch_finish(h, kept);
            return -1;
        }
        row = (uint32_t)get_view(h)->rows[i];
        label = search_label(row);
        if (label && srch_match(label, r->q))
        {
            get_view(h)->rows[kept++] = row;
            counts[SRCH_ROW_KIND(row)]++;
        }
    }
    srch_finish(h, kept);
    return kept;
}

static int lib_search_held(const struct srch_query *q, int view, bool *capped,
               bool (*cancel)(void))
{
    unsigned long c0 UNUSED_ATTR = cpu_us();
    int counts[SRCH_KINDS];
    struct srch_run r;
    bool refine;
    int n;

    *capped = false;
    if (tagcache_ui_count() < 0)
        return -1;                      /* "Updating Library..." */
    if (srch.gen != tagcache_ui_generation())
    {
        lib_search_begin();             /* a reload: the ROWS again */
        view = -1;
    }
    memset(counts, 0, sizeof(counts));
    memset(&r, 0, sizeof(r));
    r.q = q;
    r.cancel = cancel;
    r.gen = tagcache_ui_generation();
    refine = view > 0 && view == srch.view_h && get_view(view)
             && !srch.capped && srch_extends(&srch.last, q);
    cpu_boost(true);
    if (refine)
        n = srch_refine(&r, counts);
    else
        n = srch_build(&r, counts, capped);
    cpu_boost(false);
    if (n < 0)
    {
        srch.last.len = 0;              /* next time: from the sources */
        srch.capped = false;
        UPDATE_TRACE("search q=%d aborted=%d yields=%d", q->len, n,
                     r.yields);
        return n;
    }
    if (srch.partial && q->len)
        *capped = true;                 /* group kinds missing */
    srch.last = *q;
    srch.capped = *capped;
    UPDATE_TRACE("search q=%d kind_n=%d,%d,%d,%d,%d n=%d us=%lu yields=%d "
                 "refine=%d cap=%d", q->len, counts[SRCH_ARTIST],
                 counts[SRCH_ALBUM], counts[SRCH_SONG], counts[SRCH_BOOK],
                 counts[SRCH_EPISODE], n, cpu_us() - c0, r.yields,
                 (int)refine, *capped ? n : 0);
    return srch.view_h;
}

int lib_search(const struct srch_query *q, int view, bool *capped,
               bool (*cancel)(void))
{
    struct tagcache_context *held = tagcache_context_acquire();
    int result = lib_search_held(q, view, capped, cancel);
    tagcache_context_release(held);
    return result;
}

/* lib_play() of a Search row: a song or an episode plays the result's
 * rows of its kind from there; an artist or album row its songs; a book
 * row is opened, not played (its page). */
static int search_play(int view, int idx, bool shuffle)
{
    struct lib_filter f = LIB_FILTER_ANY;
    struct view *v = get_view(view);
    uint32_t row = (uint32_t)v->rows[idx];
    unsigned kind = SRCH_ROW_KIND(row);
    int rep = SRCH_ROW_IDX(row);
    unsigned long elapsed = 0, offset = 0;
    const char *src;
    int i, n = 0, h, start = 0, rc;

    if (kind == SRCH_SONG || kind == SRCH_EPISODE)
    {
        for (i = 0; i < v->count; i++)
            n += SRCH_ROW_KIND((uint32_t)v->rows[i]) == kind;
        h = track_view(n, kind == SRCH_SONG ? LIB_SONGS : LIB_PODCASTS,
                       kind == SRCH_SONG ? C_MUSIC : C_PODCAST);
        if (h <= 0)
            return -1;
        v = get_view(view);
        for (i = 0, n = 0; i < v->count; i++)
        {
            uint32_t r = (uint32_t)v->rows[i];
            if (SRCH_ROW_KIND(r) != kind)
                continue;
            if (i == idx)
                start = n;
            get_view(h)->rows[n++] = SRCH_ROW_IDX(r);
        }
        if (kind == SRCH_EPISODE)
        {
            long e = stat_of(rep, tag_lastelapsed);
            long o = stat_of(rep, tag_lastoffset);
            elapsed = e > 0 ? e : 0;
            offset = (e > 0 && o > 0) ? o : 0;
        }
    }
    else if (kind == SRCH_ARTIST || kind == SRCH_ALBUM)
    {
        if (kind == SRCH_ARTIST)
            f.artist = rep;
        else
            f.album = rep;
        h = build(LIB_SONGS, &f, &src);
        if (h <= 0)
            return -1;
    }
    else
        return -1;
    rc = play_view(h, start, shuffle, elapsed, offset, NULL);
    grp_book = false;
    if (rc == 0)
        lib_shuffle_note_book(false);
    slot_free(h);
    return rc;
}

/* -- On-The-Go views (otg.c) -- */
static int lib_otg_view_new_held(int n, size_t names)
{
    struct view *v;
    size_t size;
    int h;

    if (tagcache_ui_count() >= 0)
        pool_sync();
    size = sizeof(struct view) + (size_t)n * sizeof(int32_t)
           + sizeof(struct subcache) + 8;
    h = slot_new(size + names);
    if (h <= 0)
        return -1;
    v = get_view(h);
    memset(v, 0, sizeof(*v));
    v->off_names = view_layout(v, n, X_SUB);
    v->cat = LIB_OTG;
    v->group = G_NONE;
    v->order = O_TITLE;
    v->cls = C_ANY;
    v->sub = SUB_TRACK_ARTIST;
    v->generation = tagcache_ui_generation();
    v->count = n;
    v->live_row = -1;
    view_caches_reset(v);
    memset(v->rows, 0xff, (size_t)n * sizeof(int32_t));
    *((char *)v + v->off_names) = '\0';  /* offset 0: "" (the +8 above) */
    slots[h - 1].count = n;
    return h;
}

int lib_otg_view_new(int n, size_t names)
{
    struct tagcache_context *held = tagcache_context_acquire();
    int result = lib_otg_view_new_held(n, names);
    tagcache_context_release(held);
    return result;
}

int32_t *lib_view_rows(int view)
{
    struct view *v = get_view(view);
    return v ? v->rows : NULL;
}

char *lib_otg_names(int view, size_t *cap)
{
    struct view *v = get_view(view);

    if (!v || v->cat != LIB_OTG)
        return NULL;
    *cap = slots[view - 1].size - v->off_names;
    return (char *)v + v->off_names;
}

/* -- library stats (About; brief 4.3) -- */
static struct {
    volatile uint32_t seq;      /* odd while written; 0: never */
    struct lib_stats s;
} stats;

/* Compiler barrier: the fields are written between the two seq stores */
#define STATS_BARRIER() __asm__ __volatile__("" ::: "memory")

/* Tagcache thread: a precompute file was written or found current. */
static void stats_publish(struct tagcache_context *ctx, const struct libfile_stats *f)
{
    if (ctx != tagcache_context_active())
        return;
    stats.seq++;
    STATS_BARRIER();
    stats.s.songs = f->songs;
    stats.s.episodes = f->episodes;
    stats.s.books = f->books;
    stats.s.audio_mib = f->audio_mib;
    stats.s.gen = tagcache_ui_generation_ctx(ctx);
    STATS_BARRIER();
    stats.seq++;
}

bool lib_stats(struct lib_stats *out)
{
    int tries;

    for (tries = 0; tries < 8; tries++)
    {
        uint32_t a = stats.seq;

        STATS_BARRIER();
        if (a == 0)
            return false;               /* no precompute yet */
        if (a & 1)
            continue;                   /* being written */
        *out = stats.s;
        STATS_BARRIER();
        if (stats.seq == a)
            return true;
    }
    return false;
}

#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a: the video totals, as stats (written on the tagcache thread,
 * read on the UI thread through the same kind of sequence counter) */
static struct {
    volatile uint32_t seq;      /* odd while written; 0: never */
    struct libfile_vstats s;
} vstats;

static void vstats_publish(const struct libfile_vstats *f)
{
    vstats.seq++;
    STATS_BARRIER();
    vstats.s = *f;
    STATS_BARRIER();
    vstats.seq++;
}

bool lib_vstats(struct libfile_vstats *out)
{
    int tries;

    for (tries = 0; tries < 8; tries++)
    {
        uint32_t a = vstats.seq;

        STATS_BARRIER();
        if (a == 0)
            return false;               /* no precompute yet */
        if (a & 1)
            continue;                   /* being written */
        *out = vstats.s;
        STATS_BARRIER();
        if (vstats.seq == a)
            return true;
    }
    return false;
}
#endif

/* The counts from the RAM database, one pass. 'step' yields (and says
 * whether to go on) every so often. Video builds: also the video totals
 * into *vs (if not NULL): a video is in neither songs nor audio_mib; a
 * video podcast episode counts in episodes (and vpodcasts), never in
 * videos (owner T5); video_mib covers every video file. */
#ifdef HAVE_BCMFW_VIDEO
static bool stats_walk(struct tagcache_context *snapshot,
                       struct libfile_stats *st, struct libfile_vstats *vs,
                       void *ctx, bool (*step)(void *ctx, int *steps))
#else
static bool stats_walk(struct tagcache_context *snapshot,
                       struct libfile_stats *st, void *ctx,
                       bool (*step)(void *ctx, int *steps))
#endif
{
    uint64_t bits = 0;
    int i, n = tagcache_ui_count_ctx(snapshot), steps = 0;
#ifdef HAVE_BCMFW_VIDEO
    struct libfile_vstats vdummy;
    uint64_t vbits = 0;

    if (!vs)
        vs = &vdummy;
    memset(vs, 0, sizeof(*vs));
#endif

    memset(st, 0, sizeof(*st));
    for (i = 0; i < n; i++)
    {
        const int32_t *s;
        int32_t flag;

        if (!step(ctx, &steps))
            return false;
        s = tagcache_ui_seeks_ctx(snapshot, i, &flag);
        if (!s)
            continue;
#ifndef HAVE_BCMFW_VIDEO
        if (flag & TAGCACHE_UI_VIDEO)
            continue;
#endif
        if (flag & TAGCACHE_UI_VIDEO_OTHER)
            continue;           /* neither a song nor a playable video */
#ifdef HAVE_BCMFW_VIDEO
        if (flag & TAGCACHE_UI_VIDEO)
        {
            if (flag & TAGCACHE_UI_PODCAST)
            {
                st->episodes++;
                vs->vpodcasts++;
            }
            else if (flag & TAGCACHE_UI_TV)
                vs->tv++;
            else
                vs->movies++;
            if (s[tag_bitrate] > 0 && s[tag_length] > 0)
                vbits += (uint64_t)s[tag_bitrate] * (uint32_t)s[tag_length];
            continue;
        }
#endif
        if (flag & TAGCACHE_UI_PODCAST)
            st->episodes++;
        else if (flag & TAGCACHE_UI_AUDIOBOOK)
            st->books++;
        else
            st->songs++;
        if (s[tag_bitrate] > 0 && s[tag_length] > 0)
            bits += (uint64_t)s[tag_bitrate] * (uint32_t)s[tag_length];
    }
    st->audio_mib = (uint32_t)(bits >> 23);   /* kbit/s x ms = bits */
#ifdef HAVE_BCMFW_VIDEO
    vs->videos = vs->movies + vs->tv;
    vs->video_mib = (uint32_t)(vbits >> 23);
#endif
    return true;
}

static bool stats_live_step(void *ctx, int *steps)
{
    if (++*steps < YIELD_EVERY)
        return true;
    *steps = 0;
    yield();
    return tagcache_ui_generation() == *(int *)ctx
        && tagcache_ui_count() >= 0;
}

bool lib_stats_live(struct lib_stats *out)
{
    unsigned long c0 UNUSED_ATTR = cpu_us();
    struct libfile_stats st;
    int gen = tagcache_ui_generation();

#ifdef HAVE_BCMFW_VIDEO
    if (tagcache_ui_count() < 0
        || !stats_walk(tagcache_context_active(), &st, NULL, &gen, stats_live_step))
        return false;
#else
    if (tagcache_ui_count() < 0
        || !stats_walk(tagcache_context_active(), &st, &gen, stats_live_step))
        return false;
#endif
    out->songs = st.songs;
    out->episodes = st.episodes;
    out->books = st.books;
    out->audio_mib = st.audio_mib;
    out->gen = gen;
    UPDATE_TRACE("lib stats live n=%d us=%lu", tagcache_ui_count(),
                 cpu_us() - c0);
    return true;
}

/* Precompute (tagcache thread): the STATS section. */
static bool pre_stats_step(void *ctx, int *steps)
{
    (void)steps;
    struct pre *p = ctx;
    return pre_step(p->tc, p);
}

#ifdef HAVE_BCMFW_VIDEO
static bool pre_stats(struct pre *p, struct libfile_stats *st,
                      struct libfile_vstats *vs)
{
    if (!stats_walk(p->tc, st, vs, p, pre_stats_step))
        return false;
    if (!pre_sec_begin(p, LIBFILE_CAT_STATS, 0, sizeof(*st))
        || !pre_put(p, st, sizeof(*st)))
        return false;
    pre_sec_end(p);
    if (!pre_sec_begin(p, LIBFILE_CAT_VSTATS, 0, sizeof(*vs))
        || !pre_put(p, vs, sizeof(*vs)))
        return false;
    pre_sec_end(p);
    UPDATE_TRACE("lib vstats videos=%ld tv=%ld movies=%ld vpodcasts=%ld "
                 "mib=%lu", (long)vs->videos, (long)vs->tv,
                 (long)vs->movies, (long)vs->vpodcasts,
                 (unsigned long)vs->video_mib);
    return true;
}
#else
static bool pre_stats(struct pre *p, struct libfile_stats *st)
{
    if (!stats_walk(p->tc, st, p, pre_stats_step))
        return false;
    if (!pre_sec_begin(p, LIBFILE_CAT_STATS, 0, sizeof(*st))
        || !pre_put(p, st, sizeof(*st)))
        return false;
    pre_sec_end(p);
    return true;
}
#endif

/* The STATS section of a current file: published, or false if the file
 * has none (one from before P11: it is rewritten) or it is damaged. */
static bool pre_stats_read(struct tagcache_context *ctx, int fd, const struct libfile_hdr *h)
{
    const struct libfile_sec *sec = pre_sec(h, LIBFILE_CAT_STATS, 0,
                                            sizeof(struct libfile_stats));
    struct libfile_stats st;
    struct libfile_sum_state s;

    if (!sec || sec->count != 1
        || lseek(fd, sec->offset, SEEK_SET) != (off_t)sec->offset
        || read(fd, &st, sizeof(st)) != (ssize_t)sizeof(st))
        return false;
    libfile_sum_init(&s);
    libfile_sum_add(&s, &st, sizeof(st));
    if (libfile_sum_end(&s) != sec->sum)
        return false;
    stats_publish(ctx, &st);
    return true;
}

#ifdef HAVE_BCMFW_VIDEO
/* The VSTATS section of a current file (a video build's always has it):
 * published, or false (the file is redone) */
static bool pre_vstats_read(struct tagcache_context *ctx, int fd,
                            const struct libfile_hdr *h)
{
    const struct libfile_sec *sec = pre_sec(h, LIBFILE_CAT_VSTATS, 0,
                                            sizeof(struct libfile_vstats));
    struct libfile_vstats vs;
    struct libfile_sum_state s;

    if (!sec || sec->count != 1
        || lseek(fd, sec->offset, SEEK_SET) != (off_t)sec->offset
        || read(fd, &vs, sizeof(vs)) != (ssize_t)sizeof(vs))
        return false;
    libfile_sum_init(&s);
    libfile_sum_add(&s, &vs, sizeof(vs));
    if (libfile_sum_end(&s) != sec->sum)
        return false;
    if (ctx == tagcache_context_active())
        vstats_publish(&vs);
    return true;
}
#endif
#endif /* LIB_P11 */

/* Whether the file on disk is the one for the database in RAM. Reads the
 * header into block 'hh' (at least LIBFILE_HDR_SIZE bytes). */
static bool pre_current(struct tagcache_context *ctx, int hh)
{
    struct libfile_hdr *h;
    char path[MAX_PATH];
    bool ok;
    int fd;

    pre_path(ctx, path, sizeof(path), LIBFILE_NAME);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    core_pin(hh);
    h = core_get_data(hh);
    ok = read(fd, h, LIBFILE_HDR_SIZE) == (ssize_t)LIBFILE_HDR_SIZE
        && pre_hdr_ok(ctx, h);
#ifdef LIB_P11
    /* P11: the stats (one 16 B read); a file without them is redone */
    ok = ok && pre_stats_read(ctx, fd, h);
#ifdef HAVE_BCMFW_VIDEO
    ok = ok && pre_vstats_read(ctx, fd, h);  /* 14a: one 20 B read */
#endif
#endif
    /* #58: a file with an album table but without the panel set's book
     * count (col 1) is from before the book panels: redone once */
    /* #100: and one without the pool order after it, once */
    ok = ok && (!pre_sec(h, LIBFILE_CAT_ALBUMTAB, 0,
                         sizeof(struct libfile_album))
                || pre_pool_ok(pre_sec(h, LIBFILE_CAT_PANELSET, 1,
                                       sizeof(uint32_t))));
    /* #47: rows grouped by older rules (ASCII-only case) are redone
     * once */
    ok = ok && (h->reserved == LIBFILE_GROUPING
#ifdef SIMULATOR
                || getenv("PROKBOX_SIM_ASCII_ARTKEY")   /* as before #47 */
#endif
                );
    /* #47: an album table without the artists' albums is from before
     * the per-artist ranges: redone once (an empty section counts) */
    ok = ok && (!pre_sec(h, LIBFILE_CAT_ALBUMTAB, 0,
                         sizeof(struct libfile_album))
                || pre_sec(h, LIB_ARTISTS, LIBFILE_ALBUMS, sizeof(uint32_t)));
    /* #59: books without their author order are from before Sort By */
    ok = ok && (!pre_sec(h, LIB_AUDIOBOOKS, LIBFILE_ROWS, sizeof(int32_t))
                || pre_sec(h, LIBFILE_CAT_BOOKS_AUTHOR, LIBFILE_ROWS,
                           sizeof(int32_t)));
    if (ok && ctx == tagcache_context_active())
        pre_note_counts(h);
    core_unpin(hh);
    close(fd);
    return ok;
}

void lib_committed(void)
{
    artcache_build_request();
#ifdef LIB_BOOKS
    seed_owed = true;                   /* books.dat's migration (M2) */
    np_pb_ab_wake();
#endif
}

#ifdef HAVE_BCMFW_VIDEO
/* Scratch of a .vix pass outside a precompute: ledger, entries, index
 * build (vixpass.c needs >= 4 KB plus 32 B per video) */
#define VIX_SCRATCH(n)  (8192 + (size_t)(n) / 8 * 32)

/* The owed .vix pass (3.4), in the reserved precompute block if there is
 * one (idle between precomputes), else a transient block, which needs
 * audio stopped (checked just before the allocation, no yield between)
 * and is given back to the audio buffer afterwards. 'h' > 0: a block
 * the caller holds (the precompute's own). */
static int vix_due(struct tagcache_context *ctx, int h, size_t size, bool (*stop)(void))
{
    if (ctx != tagcache_context_active())
        return LIB_PRE_DONE;
    bool own = false;
    int rc;

    if (audio_status() != 0)
        return LIB_PRE_WAIT;            /* no I/O while audio plays */
    if (h <= 0 && pre_res_h > 0)
    {
        h = pre_res_h;
        size = pre_bytes(pre_res_entries);
    }
    if (h <= 0)
    {
        int n = tagcache_ui_count_ctx(ctx);

        size = VIX_SCRATCH(n > 0 ? n : 0);
        h = core_alloc(size);           /* audio is stopped (above) */
        if (h <= 0)
        {
            UPDATE_TRACE("lib alloc failed what=%s bytes=%ld", "vixpass",
                         (long)size);
            return LIB_PRE_WAIT;
        }
        own = true;
    }
    core_pin(h);                        /* file I/O throughout */
    rc = vixpass_run(core_get_data(h), size, stop);
    core_unpin(h);
    if (own)
    {
        core_free(h);
        audio_buffer_regrow();
    }
    if (rc != LIB_PRE_WAIT)
        vix_owed_gen = -1;
    return rc;
}

/* Marks the pass owed for the file just written or found current */
static void vix_owe(struct tagcache_context *ctx, bool force)
{
    vix_owed_gen = tagcache_ui_generation_ctx(ctx);
    vix_owed_files = pre_files;
    vix_owed_forced = force;
}
#endif

int lib_precompute_ctx(struct tagcache_context *ctx, bool force, bool (*stop)(void))
{
    static const int order[] = {
        LIB_ARTISTS, LIB_ALBUMS, LIB_COMPILATIONS, LIB_SONGS, LIB_GENRES,
        LIB_COMPOSERS, LIB_AUDIOBOOKS, LIB_PODCASTS,
#ifdef HAVE_BCMFW_VIDEO
        LIB_MOVIES, LIB_TVSHOWS,
#endif
    };
    unsigned long t0 UNUSED_ATTR = now_us(), c0 UNUSED_ATTR = cpu_us();
    char tmp[MAX_PATH], dst[MAX_PATH];
    struct libfile_sum_state s;
    struct libfile_hdr *hd;
    struct pre p;
    unsigned i;
    int n = tagcache_ui_count_ctx(ctx), shows = 0;
    bool ok;
#ifdef LIB_P11
    struct libfile_stats st;
#endif
#ifdef HAVE_BCMFW_VIDEO
    struct libfile_vstats vst;
#endif

    if (n < 0)
        return LIB_PRE_DONE;    /* nothing to compute from; the next load
                                 * asks again */
#ifdef HAVE_BCMFW_VIDEO
    /* the file is current and only its .vix pass waits (for a stop): no
     * header read per retry. A forced request after an unforced run is
     * a new one (the file was found damaged): it rewrites first */
    if (vix_owed_gen == tagcache_ui_generation_ctx(ctx)
        && vix_owed_files == pre_files && (!force || vix_owed_forced))
        return vix_due(ctx, 0, 0, stop);
#endif
    memset(&p, 0, sizeof(p));
    p.tc = ctx;
    p.size = pre_bytes(n);
    pre_events = stop;
    /* outgrown (or none: a first build): while stopped, reserve anew
     * for this size rather than hold the old block and a transient one
     * (H1 end review); the block is then there during playback too */
    if (n > pre_res_entries && audio_status() == 0)
        pre_reserve(n);
    pre_transient = pre_res_h <= 0 || n > pre_res_entries;
    /* a transient block would come out of the audio buffer (a gap):
     * only while stopped, or in a paused window (#88: free memory the
     * window made, the floor kept; PLAY then no longer stops it). Its
     * own tenant (P16): freed before this returns, ended by the caller
     * (tagcache.c ui_precompute_run()), never by a commit's end */
    if (pre_transient && audio_status() != 0
        && !((maint_active() || maint_window_open())
             && maint_take(MAINT_LISTS, p.size + 64)))
    {
        /* but the file may well be current (a boot that resumed music):
         * the art builder checks its stamp itself, and works during
         * playback, so tell it once per database load, not per retry
         * (a request restarts its pass) */
        static int wait_told = -1;

        if (wait_told != tagcache_ui_generation_ctx(ctx))
        {
            wait_told = tagcache_ui_generation_ctx(ctx);
            UPDATE_TRACE("lib precompute wait n=%d reserved=%ld", n,
                         pre_res_entries);
            if (ctx == tagcache_context_active())
                lib_committed();
        }
        return LIB_PRE_WAIT;
    }
    if (!force)
    {
        /* the header check needs 544 B: no big transient block for it */
        int hh = pre_transient ? core_alloc(LIBFILE_HDR_SIZE) : pre_res_h;
        bool cur = hh > 0 && pre_current(ctx, hh);

        if (pre_transient && hh > 0)
            core_free(hh);
        if (cur)
        {
            UPDATE_TRACE("lib precompute current n=%d", n);
            if (ctx == tagcache_context_active())
                lib_committed();
#ifdef HAVE_BCMFW_VIDEO
            vix_owe(ctx, false);
            return vix_due(ctx, 0, 0, stop);
#else
            return LIB_PRE_DONE;
#endif
        }
    }
    if (tagcache_context_published(ctx))
    {
        /* A published generation's prepared lists are immutable. Ask the
         * worker to clone it; never invalidate its manifest in place. */
        tagcache_ui_precompute_request();
        return LIB_PRE_WAIT;
    }
    p.c.h = pre_transient ? core_alloc(p.size) : pre_res_h;
    if (p.c.h <= 0)
    {
        UPDATE_TRACE("lib alloc failed what=%s bytes=%ld", "precompute",
                     (long)p.size);
        return LIB_PRE_WAIT;
    }

    p.n = n;
    p.hdr_off = p.size - PRE_WBUF - LIBFILE_HDR_SIZE;
    p.wbuf_off = p.size - PRE_WBUF;
    p.c.gen = tagcache_ui_generation_ctx(ctx);
    p.c.recs_off = 0;
    p.c.seen_off = (size_t)n * sizeof(struct rec);
    p.c.stop = pre_stop;
    p.c.memo.generation = -1;
    p.fd = -1;
    memset(pre_hdr(&p), 0, LIBFILE_HDR_SIZE);

    pre_path(ctx, tmp, sizeof(tmp), LIBFILE_TEMP);
    pre_path(ctx, dst, sizeof(dst), LIBFILE_NAME);
    p.fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC, 0666);
    ok = p.fd >= 0 && pre_write(&p, p.hdr_off, LIBFILE_HDR_SIZE);
    p.pos = LIBFILE_HDR_SIZE;
    for (i = 0; ok && i < ARRAYLEN(order); i++)
        ok = pre_cat(ctx, &p, order[i]);
    ok = ok && pre_books_author(ctx, &p);
    ok = ok && pre_albumtab(ctx, &p);
#ifdef LIB_P11
#ifdef HAVE_BCMFW_VIDEO
    ok = ok && pre_stats(&p, &st, &vst);
#else
    ok = ok && pre_stats(&p, &st);
#endif
#endif
    ok = ok && pre_flush(&p);
    if (ok)
    {
        hd = pre_hdr(&p);
        hd->magic = LIBFILE_MAGIC;
        hd->version = LIBFILE_VERSION;
        hd->sortkey_version = SORTKEY_VERSION;
        hd->reserved = LIBFILE_GROUPING;
#ifdef SIMULATOR
        if (getenv("PROKBOX_SIM_ASCII_ARTKEY"))
            hd->reserved = 0;   /* as a file from before #47 */
#endif
        hd->commitid = tagcache_ui_commitid_ctx(ctx);
        hd->entries = n;
        hd->sum = 0;
        libfile_sum_init(&s);
        libfile_sum_add(&s, hd, LIBFILE_HDR_SIZE);
        hd->sum = libfile_sum_end(&s);
        ok = lseek(p.fd, 0, SEEK_SET) == 0
            && pre_write(&p, p.hdr_off, LIBFILE_HDR_SIZE);
    }
    /* the count before the block goes (noted only if the file is kept) */
    if (ok)
    {
        const struct libfile_sec *ps = pre_sec(pre_hdr(&p), LIB_PODCASTS,
                                               LIBFILE_ROWS, sizeof(int32_t));
        shows = ps ? (int)ps->count : 0;
    }
    if (p.fd >= 0)
    {
        if (ok)
            ok = fsync(p.fd) == 0;
        ok = close(p.fd) == 0 && ok;
    }
    /* the last check: nothing changed while the file was written */
    ok = ok && tagcache_ui_generation_ctx(ctx) == p.c.gen && !pre_stop();
    /* never a rename onto an existing name (two entries for one name on
     * the device, crash-fat 1019): the table is derived, and a cut after
     * the remove leaves none, which pre_current() finds and the next
     * load writes again */
    if (ok)
        ok = (remove(dst) == 0 || errno == ENOENT) && rename(tmp, dst) == 0;
#ifdef HAVE_BCMFW_VIDEO
    {
        int vrc = LIB_PRE_DONE;

        if (ok)
        {
            if (ctx != tagcache_context_active())
            {
                if (pre_transient)
                {
                    core_free(p.c.h);
                    audio_buffer_regrow();
                }
                return LIB_PRE_DONE;
            }
            /* published first: the lists and About don't wait for the
             * .vix pass, which then runs in this block (its work is
             * done), only with audio stopped */
            pre_podcasts = shows;
            stats_publish(ctx, &st);
            vstats_publish(&vst);
            pre_files++;
            lib_committed();
            vix_owe(ctx, force);
            vrc = vix_due(ctx, p.c.h, p.size, stop);   /* WAIT while playing */
        }
        if (pre_transient)
        {
            core_free(p.c.h);
            audio_buffer_regrow();
        }
        if (ok)
        {
            UPDATE_TRACE("lib precompute done n=%d us=%lu el_ms=%lu "
                         "yields=%d bytes=%lu block=%s playing=%d", n,
                         cpu_us() - c0, (now_us() - t0) / 1000, p.c.yields,
                         (unsigned long)p.pos,
                         pre_transient ? "transient" : "reserved",
                         (int)(audio_status() != 0));
            return vrc;
        }
    }
    UPDATE_TRACE("lib precompute stopped n=%d us=%lu el_ms=%lu yields=%d "
                 "bytes=%lu block=%s playing=%d", n, cpu_us() - c0,
                 (now_us() - t0) / 1000, p.c.yields, (unsigned long)p.pos,
                 pre_transient ? "transient" : "reserved",
                 (int)(audio_status() != 0));
#else
    UPDATE_TRACE("lib precompute %s n=%d us=%lu el_ms=%lu yields=%d "
                 "bytes=%lu block=%s playing=%d", ok ? "done" : "stopped",
                 n, cpu_us() - c0, (now_us() - t0) / 1000, p.c.yields,
                 (unsigned long)p.pos, pre_transient ? "transient"
                 : "reserved", (int)(audio_status() != 0));
    if (pre_transient)
    {
        core_free(p.c.h);
        audio_buffer_regrow();  /* (playback.h) */
    }
#endif
    if (!ok)
    {
        remove(tmp);
        /* a stop or a change: again later; a write error is logged and
         * the lists keep scanning (the next load tries again) */
        if (tagcache_ui_generation_ctx(ctx) != p.c.gen || pre_stop())
            return LIB_PRE_WAIT;
        UPDATE_TRACE("lib io precompute %s", "failed");
        return LIB_PRE_ERROR;
    }
    if (ctx != tagcache_context_active())
        return LIB_PRE_DONE;
    pre_podcasts = shows;
#ifdef LIB_P11
    stats_publish(ctx, &st);
#endif
    pre_files++;
    lib_committed();
    return LIB_PRE_DONE;
}

/* Measured (2026-10-06, tests/perf/lib50k.sh: the shell simulator on an
 * Apple M3, thread CPU time; 50,000 synthetic tracks, 4,970 albums, 1,006
 * album artists, 50 books, 20 shows; RAM cache 6.93 MB). Device estimate
 * = sim CPU x 400 (scripts/check_trace.py SIM_FACTOR, uncalibrated) plus
 * the file read, which the sim cannot estimate (flash: a few MB/s through
 * FAT, ~10-60 ms for these sizes; to be measured, P10):
 *    lib_open        rows   from the file          scan (no file)
 *                           CPU    read             CPU
 *    artists        1,006   96 us   8 KB  (~38 ms)  989 us (~0.40 s)
 *    albums         4,882   95 us  78 KB  (~38 ms) 3061 us (~1.2 s)
 *    compilations      96   71 us   2 KB  (~28 ms)  243 us (~0.10 s)
 *    songs         49,000  173 us 196 KB  (~69 ms) 1842 us (~0.74 s)
 *    genres            24  119 us   0 KB  (~48 ms)  339 us (~0.14 s)
 *    composers        389   71 us   2 KB  (~28 ms) 1073 us (~0.43 s)
 *    audiobooks        50  556 us   1 KB (~0.22 s)  724 us (~0.29 s)
 *    podcasts          20  598 us   1 KB (~0.24 s)  665 us (~0.27 s)
 *  (books and shows: the progress pass over the RAM index, live, is most
 *  of it.) Filtered views (an artist's albums, an album's songs) scan the
 *  index as before (~0.16 s on the device for the scan alone); an
 *  artist's albums also look their counts and keys up in the album table
 *  (~13 small reads per row).
 *  lib_precompute() at 50k: 12.5 ms CPU in the sim (13 ms elapsed, 347
 *  yields), 426 KB written: ~5 s CPU on the device unboosted at 80 MHz
 *  (~13 s at 30 MHz) plus the write, in the background, audio stopped;
 *  work block 20 B/entry + 6.6 KB = 1.0 MB, freed at once.
 *  lib_play() (#109): the whole list as master indices, no file name
 *  read (a book reads its few), one 4 B/row payload write and one
 *  control line; the names are read when each track loads. Before #109
 *  it was a 2,000-row window with 2,000 file name reads (~10 s on a
 *  PP5020, upstream's note). lib_shuffle_all() is the library source
 *  (#27), not a queue of names.
 *  The scan's sorts are a yielding heap sort since 4b (the precompute
 *  sorts on the tagcache thread; qsort() has no context): the scan is
 *  ~15-40 % slower than with qsort() (albums 2.7 -> 3.1 ms), but it is
 *  only the fallback now. A 300-track library opens in ~0.05 ms. */

#ifdef LIB_BOOKS
/* -- Phase 6: the book page, the store's seed, the boot resume ---------- */

/* The opened book's page (ab_book.h ab_page_*; Rev 2: every book opens
 * its page). It keeps two pool slots until ab_page_close(): the book's
 * track view (the queue) and its entries: the books.pak blob (PAGE_BLOB),
 * or a single file's chapters read straight from it on a pak miss
 * (PAGE_TABLE, Rev 1 item 4), or none (PAGE_FILES: the files are the
 * entries, titles from the RAM database). While audio plays the pool
 * does not grow (pool_reserve()), so a pak miss reads a smaller table
 * ("ab view cap"). Statics: ~40 B. */
enum { PAGE_NONE, PAGE_BLOB, PAGE_TABLE, PAGE_FILES };
static struct {
    int8_t kind;
    bool resume;                /* a saved spot: "Resume" is shown */
    int child, tab;             /* pool slot handles (0: none) */
    int entries;
    uint32_t tab_off;           /* the table inside the blob */
    uint32_t blob_size;
    ab_key_t key;
} page;

static char *slot_data(int h)
{
    return h > 0 ? (char *)get_view(h) : NULL;
}

static const struct audiobook_chapters *page_tab(void)
{
    char *p = slot_data(page.tab);

    return p ? (const struct audiobook_chapters *)(p + page.tab_off) : NULL;
}

static bool page_live(void)
{
    struct view *c = page.kind ? get_view(page.child) : NULL;

    return c && view_fresh(c)
           && (page.kind == PAGE_FILES || page_tab());
}

void ab_page_close(void)
{
    if (page.tab > 0)
        slot_free(page.tab);
    if (page.child > 0)
        slot_free(page.child);
    memset(&page, 0, sizeof(page));
}

/* A single file's chapters, read from the file into the newest slot
 * (pak miss): a table capped to the pool's free room while audio plays.
 * Returns the chapter count (0: none), or -1. */
#ifdef SIMULATOR
void sim_read_count_begin(void);       /* uisimulator/common/filesystem-sim.c */
int sim_read_count_end(void);
#endif

#define AB_PAGE_SCRATCH 4096    /* 1,100 chapters: ~11 reads (warble) */

static int page_read_file(int *reads)
{
    char path[MAX_PATH];
    size_t ent = ALIGN_UP(sizeof(struct mp3entry), 8);
    size_t avail, tsize;
    struct audiobook_chapters *tab;
    struct mp3entry *id3;
    int cap = AB_MAX_CHAPTERS, pool_b = AB_CH_POOL, fd, n = 0, h;
    size_t scr = AB_PAGE_SCRATCH;
    char *p;

    if (!lib_path(page.child, 0, path, sizeof(path)))
        return -1;
    (*reads)++;
    avail = (size_t)pool.size > (size_t)pool.used + ent + 64
            ? (size_t)(pool.size - pool.used) - ent - 64 : 0;
    /* the read window (fix 2: the titles in a few dozen reads, not one
     * per chapter) lives in the same slot as the mp3entry, until the
     * slot shrinks to the table below. While audio plays the table comes
     * first: the window gets an eighth of a short room, or nothing. */
    if (audio_status() && avail < AB_CH_TABLE_SIZE + scr)
    {
        scr = (avail / 8) & ~(size_t)7;
        if (scr > 1024)
            scr = 1024;
        if (scr < AB_CH_SCRATCH_MIN + 128)
            scr = 0;
    }
    avail = avail > scr ? avail - scr : 0;
    if (audio_status() && avail < AB_CH_TABLE_SIZE)
    {
        /* entries 8 B + ~24 B of title each */
        cap = (int)(avail / 32);
        if (cap > AB_MAX_CHAPTERS)
            cap = AB_MAX_CHAPTERS;
        if (cap < 1)
            return -1;
        pool_b = (int)(avail - AB_CH_HDR_SIZE - (size_t)cap * 8);
        if (pool_b > AB_CH_POOL)
            pool_b = AB_CH_POOL;
        if (pool_b < AB_CH_POOL_MIN)
            return -1;
        UPDATE_TRACE(AB_TR_VIEW_CAP, cap);
    }
    tsize = ALIGN_UP(AB_CH_SIZE(cap, pool_b), 8);
    h = slot_new(tsize + ent + scr);
    if (h <= 0)
        return -1;
    page.tab = h;
    page.tab_off = 0;
    core_pin(pool.handle);              /* the reads below yield */
    p = slot_data(h);
    tab = (struct audiobook_chapters *)p;
    id3 = (struct mp3entry *)(p + tsize);
    ab_ch_init(tab, cap, pool_b);
    fd = open(path, O_RDONLY);
    if (fd >= 0)
    {
        /* MP4: the chapter walk needs nothing of the header walk (~240
         * reads for a long book) but the codec family, which the name
         * gives; MP3 needs get_metadata()'s CHAP count */
        unsigned int fmt = probe_file_format(path);
        bool mp4 = fmt == AFMT_MP4_AAC || fmt == AFMT_MP4_AAC_HE
                   || fmt == AFMT_MP4_ALAC;

        if (mp4)
        {
            memset(id3, 0, sizeof(*id3));
            id3->codectype = fmt;
        }
        if ((mp4 || get_metadata(id3, fd, path))
            && metadata_read_chapters_buf(fd, id3, tab,
                                          scr ? p + tsize + ent : NULL,
                                          scr) > 0)
            n = tab->chapter_count;
        close(fd);
        *reads += 2;                    /* the header walk + the titles */
    }
    core_unpin(pool.handle);
    slot_resize(h, tsize);              /* the mp3entry was scratch */
    return fd < 0 ? -1 : n;
}

static int ab_page_open_held(int view, int idx)
{
    struct lib_filter f = LIB_FILTER_ANY;
    struct view *v = get_view(view);
    struct ab_idx_rec irec;
    struct ab_blob_view bv;
    struct ab_rec rec;
    const char *src;
    uint32_t gen, mt = 0;
    ab_key_t old = 0;
    int rep, files, i, reads = 0;

    ab_page_close();
    if (!v || idx < 0 || idx >= v->count || !view_fresh(v)
        || v->cls != C_BOOK || v->group != G_ALBUM)
        return -1;
#ifdef SIMULATOR
    sim_read_count_begin();             /* L4: real read() calls */
#endif
    rep = v->rows[idx];
    f.album = rep;
    page.key = lib_art_key(view, idx);
    page.child = build(LIB_SONGS, &f, &src);
    if (page.child <= 0)
    {
        page.child = 0;
#ifdef SIMULATOR
        sim_read_count_end();
#endif
        return -1;
    }
    files = lib_count(page.child);
    if (!page.key)
        page.key = book_key_of(rep, NULL);
    for (i = 0; i < files; i++)
    {
        struct view *c = get_view(page.child);
        const int32_t *s = c ? tagcache_ui_seeks(c->rows[i], NULL) : NULL;

        if (s && (uint32_t)s[tag_mtime] > mt)
            mt = s[tag_mtime];
    }
    page.kind = PAGE_FILES;
    page.entries = files;
    if (page.key && book_rec_get(rep, page.key, &rec))
        page.resume = ab_rec_state(&rec) == AB_REC_STARTED;
    reads++;
    if (files > 0 && files <= AB_MAP_FILES_MAX
        && (ab_pak_lookup(page.key, &irec, &gen)
            /* #47: filed under the pre-#47 key until the art pass
             * rebuilds it under the new one */
            || ((old = book_key_ascii(rep, page.key))
                && ab_pak_lookup(old, &irec, &gen)))
        && ab_idx_valid(&irec, mt, files))
    {
        int h = slot_new(irec.size);

        reads += 2;
        if (h > 0)
        {
            core_pin(pool.handle);
            if (ab_pak_read(&irec, gen, slot_data(h), irec.size, &bv))
            {
                page.kind = PAGE_BLOB;
                page.tab = h;
                page.tab_off = (uint32_t)((const char *)bv.tab
                                          - slot_data(h));
                page.blob_size = irec.size;
                page.entries = bv.tab->chapter_count;
            }
            core_unpin(pool.handle);
            if (page.kind != PAGE_BLOB)
                slot_free(h);
        }
    }
    UPDATE_TRACE(AB_TR_PAK, page.kind != PAGE_BLOB ? "miss"
                 : old ? "hit-old" : "hit",
                 (unsigned long)(uint32_t)page.key);
    if (page.kind != PAGE_BLOB && files == 1)
    {
        int n = page_read_file(&reads);

        if (n > 0)
        {
            page.kind = PAGE_TABLE;
            page.entries = n;
        }
        else if (page.tab > 0)
        {
            slot_free(page.tab);
            page.tab = 0;
        }
    }
#ifdef SIMULATOR
    reads = sim_read_count_end();       /* the UI thread's own reads */
#endif
    UPDATE_TRACE(AB_TR_OPEN, (unsigned long)(uint32_t)page.key, reads);
    if (page.entries < 1)
    {
        ab_page_close();
        return -1;
    }
    return page.entries;
}

int ab_page_open(int view, int idx)
{
    struct tagcache_context *held = tagcache_context_acquire();
    int result = ab_page_open_held(view, idx);
    tagcache_context_release(held);
    return result;
}

bool ab_page_has_resume(void)
{
    return page_live() && page.resume;
}

ab_key_t ab_page_key(void)
{
    return page_live() ? page.key : 0;
}

int ab_page_entries(void)
{
    return page_live() ? page.entries : 0;
}

const char *ab_page_entry_title(int i, char *buf, size_t len)
{
    const struct audiobook_chapters *tab;

    if (!len)
        return buf;
    buf[0] = '\0';
    if (!page_live() || i < 0 || i >= page.entries)
        return buf;
    if (page.kind == PAGE_FILES)
        return lib_label(page.child, i, buf, len);
    tab = page_tab();
    ab_ch_title(tab, i, buf, (int)len);
    return buf;
}

static bool page_play(int q, unsigned long ms, bool use_store)
{
    struct ab_blob_view bv, *bvp = NULL;
    struct book_ctx bk;
    int rc, files;

    if (!page_live())
        return false;
    files = lib_count(page.child);
    memset(&bk, 0, sizeof(bk));
    bk.key = page.key;
    bk.rep = files > 0 ? get_view(page.child)->rows[0] : -1;
    bk.use_store = use_store;
    bk.entry_q = q;
    bk.entry_ms = ms;
    if (page.kind == PAGE_BLOB)
    {
        core_pin(pool.handle);          /* play_view yields */
        if (ab_blob_parse(slot_data(page.tab), page.blob_size, &bv))
            bvp = &bv;
    }
    bk.page = bvp;
    bk.map = ab_map_begin(bk.key ? bk.key : 1, files);
    rc = play_view(page.child, 0, false, 0, 0, &bk);
    if (page.kind == PAGE_BLOB)
        core_unpin(pool.handle);
    grp_book = rc == 0;
    if (rc == 0)
        lib_shuffle_note_book(true);
    return rc == 0;
}

static bool ab_page_resume_held(void)
{
    return page_play(-1, 0, true);
}

bool ab_page_resume(void)
{
    struct tagcache_context *held = tagcache_context_acquire();
    bool result = ab_page_resume_held();
    tagcache_context_release(held);
    return result;
}

static bool ab_page_play_entry_held(int i)
{
    const struct audiobook_chapters *tab;
    int q = 0;
    unsigned long ms = 0;

    if (!page_live() || i < 0 || i >= page.entries)
        return false;
    switch (page.kind)
    {
    case PAGE_FILES:
        q = i;
        break;
    case PAGE_TABLE:
        ms = ab_ch_start(page_tab(), i);
        break;
    case PAGE_BLOB:
    {
        struct ab_blob_view bv;

        if (!ab_blob_parse(slot_data(page.tab), page.blob_size, &bv))
            return false;
        tab = bv.tab;
        for (q = bv.hdr->file_count - 1; q > 0 && bv.file_first[q] > i; q--)
            ;
        ms = ab_ch_start(tab, i);
        ms = ms > bv.file_start_ms[q] ? ms - bv.file_start_ms[q] : 0;
        break;
    }
    default:
        return false;
    }
    return page_play(q, ms, false);
}

bool ab_page_play_entry(int i)
{
    struct tagcache_context *held = tagcache_context_acquire();
    bool result = ab_page_play_entry_held(i);
    tagcache_context_release(held);
    return result;
}

/* -- the tagcache migration's walk (ab_store_seed(), Rev 1 item 12) ------ */

static int lib_book_seed_begin_held(size_t bytes, void **buf)
{
    int h;

    if (!seed_owed || audio_status())
        return -1;                      /* playing: the stop's pass */
    if (lib_state() != LIB_READY || tagcache_ui_count() <= 0)
    {
        seed_owed = false;              /* the next load owes it again */
        return -1;
    }
    seed_owed = false;
    h = core_alloc(bytes);
    if (h <= 0)
        return -1;
    core_pin(h);
    *buf = core_get_data(h);
    return h;
}

int lib_book_seed_begin(size_t bytes, void **buf)
{
    struct tagcache_context *held = tagcache_context_acquire();
    int result = lib_book_seed_begin_held(bytes, buf);
    tagcache_context_release(held);
    return result;
}

static void lib_book_seed_end_held(int h)
{
    if (h > 0)
    {
        core_free(h);
        audio_buffer_regrow();
    }
}

void lib_book_seed_end(int h)
{
    struct tagcache_context *held = tagcache_context_acquire();
    lib_book_seed_end_held(h);
    tagcache_context_release(held);
}

static bool same_book(const int32_t *a, const int32_t *b)
{
    const char *x = str_of(tagcache_context_active(), a, tag_album), *y = str_of(tagcache_context_active(), b, tag_album);

    return x && y && str_eq(x, y) && same_artist(tagcache_context_active(), a, b);
}

#define SEED_BUCKETS 64                 /* 128 B of UI stack */
#define SEED_END     0xffff

/* same_book() needs the same album (str_eq(): case folded) */
static unsigned album_bucket(const int32_t *s)
{
    const char *a = str_of(tagcache_context_active(), s, tag_album);
    unsigned hsh = 0;

    if (!a)
        return SEED_BUCKETS;            /* untagged: never a book match */
    hsh = ui_casefold_hash(a);      /* #47: as str_eq() compares */
    return hsh % SEED_BUCKETS;
}

/* Books with stats (tag_lastelapsed > 0): one record per book, its most
 * recently played file (tag_lastplayed); the position in the book from
 * the RAM database. Returns the records (the caller stamps and seals),
 * or -1 (database changed, no scratch).
 *
 * Two passes over the N entries, each O(N) (M2: the old second loop was
 * books x N, ~200M instructions at 50k): the second finds each entry's
 * book through 64 album buckets chained in the records' flags. Boosted
 * except around the filename read; yields every 512 entries. */
static int lib_book_seed_held(struct ab_rec *out, int max)
{
    int gen = tagcache_ui_generation();
    int n = 0, N = tagcache_ui_count(), i, j, sh;
    uint16_t head[SEED_BUCKETS];

    sh = play_scratch_open();
    if (sh < 0)
        return -1;
    cpu_boost(true);
    for (i = 0; i < N; i++)
    {
        const int32_t *s;
        int32_t flag = 0;
        long e, o, lp;
        ab_key_t key;
        struct play_scratch *ps;
        bool got;

        if ((i & 511) == 511)
            yield();
        s = tagcache_ui_seeks(i, &flag);
        if (!s || !(flag & TAGCACHE_UI_AUDIOBOOK)
            || (flag & TAGCACHE_UI_PODCAST))
            continue;
        e = stat_of(i, tag_lastelapsed);
        if (e <= 0)
            continue;
        o = stat_of(i, tag_lastoffset);
        lp = stat_of(i, tag_lastplayed);
        if (tagcache_ui_generation() != gen)
            goto changed;
        ps = scratch_of(sh);
        cpu_boost(false);               /* never boosted on I/O */
        got = tagcache_retrieve(&ps->tcs, i, tag_filename, ps->path,
                                sizeof(ps->path));
        cpu_boost(true);
        if (!got)
            continue;
        key = book_key_of(i, ps->path);
        if (!key)
            continue;
        for (j = 0; j < n && out[j].book_key != key; j++)
            ;
        if (j < n && (long)out[j].stamp >= lp)
            continue;                   /* a later file of that book */
        if (j == n)
        {
            if (n == max)
                continue;               /* logged by the count */
            n++;
        }
        memset(&out[j], 0, sizeof(out[j]));
        out[j].book_key = key;
        out[j].file_crc = ab_path_crc(ps->path);
        out[j].file_idx = (uint16_t)i;  /* master index until below */
        out[j].elapsed_ms = e;
        out[j].offset = o > 0 ? o : 0;
        out[j].stamp = lp > 0 ? (uint32_t)lp : 0;
    }
    play_scratch_close(sh);
    sh = -1;
    if (tagcache_ui_generation() != gen)
        goto changed;
    /* the file's place in its book and the time before it (RAM only):
     * stamp = the file's master index, file_idx = files before it,
     * book_ms = their length, flags = the bucket chain */
    memset(head, 0xff, sizeof(head));
    for (j = 0; j < n; j++)
    {
        const int32_t *cs = tagcache_ui_seeks((int)out[j].file_idx, NULL);
        unsigned bk = cs ? album_bucket(cs) : SEED_BUCKETS;

        out[j].stamp = out[j].file_idx;
        out[j].file_idx = 0;
        out[j].book_ms = 0;
        out[j].flags = SEED_END;
        if (bk < SEED_BUCKETS)
        {
            out[j].flags = head[bk];
            head[bk] = (uint16_t)j;
        }
    }
    for (i = 0; n && i < N; i++)
    {
        int32_t flag = 0;
        const int32_t *s;
        unsigned bk;

        if ((i & 511) == 511)
        {
            yield();
            if (tagcache_ui_generation() != gen)
                goto changed;
        }
        s = tagcache_ui_seeks(i, &flag);
        if (!s || !(flag & TAGCACHE_UI_AUDIOBOOK))
            continue;
        bk = album_bucket(s);
        if (bk >= SEED_BUCKETS)
            continue;
        for (j = head[bk]; j != SEED_END; j = out[j].flags)
        {
            int cur = (int)out[j].stamp;
            const int32_t *cs = tagcache_ui_seeks(cur, NULL);

            if (i == cur || !cs || !same_book(s, cs)
                || cmp_track_no(tagcache_context_active(), i, cur) >= 0)
                continue;
            out[j].file_idx++;
            out[j].book_ms += s[tag_length] > 0 ? (uint32_t)s[tag_length]
                                                : 0;
        }
    }
    cpu_boost(false);
    for (j = 0; j < n; j++)
    {
        out[j].book_ms += out[j].elapsed_ms;
        out[j].flags = 0;
        out[j].stamp = 0;
    }
    return n;

changed:
    cpu_boost(false);
    if (sh >= 0)
        play_scratch_close(sh);
    return -1;
}

int lib_book_seed(struct ab_rec *out, int max)
{
    struct tagcache_context *held = tagcache_context_acquire();
    int result = lib_book_seed_held(out, max);
    tagcache_context_release(held);
    return result;
}

/* -- the boot resume (lib_queue_resumed()): the map once audio plays ---- */

void lib_book_resumed_idle(void)
{
    struct mp3entry *id3;

    if (!resumed_map_owed || !(audio_status() & AUDIO_STATUS_PLAY))
        return;
    id3 = audio_current_track();
    if (!id3 || !id3->path[0])
        return;
    resumed_map_owed = false;
    if (lib_queue_is_book(playlist_get_current()))
        ab_map_resumed(id3, playlist_get_display_index() - 1,
                       playlist_amount());
}
#endif /* LIB_BOOKS */

int lib_precompute(bool force, bool (*stop)(void))
{
    return lib_precompute_ctx(tagcache_context_active(), force, stop);
}
