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

/* Host tests for apps/ipodui/library.c (Phase 4 review, F16), over the
 * fake tagcache of fake_tagcache.c (movable memory, a RAM cache that
 * moves on every yield, a recording playlist):
 *  - classification: audiobooks, podcasts and compilations land in their
 *    own lists and never in the music lists or Shuffle Songs;
 *  - grouping and order: 7G sort ("The" ignored, '#' last), album artist
 *    over track artist, same-named albums kept apart, case variants
 *    merged, disc/track order, All Songs by album;
 *  - playback: the 2,000-track cap (a window of a long list, a sample
 *    when shuffled, Shuffle Songs sampling all music), quiet insertion;
 *  - book progress computed at open (one pass for all rows), live
 *    position of the playing track, refresh of that row;
 *  - stale views after a reload, lib_find(), balanced cpu_boost and no
 *    leaked memory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "config.h"
#include "tagcache.h"
#include "ipodui/library.h"
#include "ipodui/lib_shuffle.h"
extern int fake_shuf_calls, fake_shuf_mode, fake_shuf_book;
struct playlist_info *playlist_get_current(void);
extern uint32_t fake_shuf_first;
#include "ipodui/libfile.h"
#include "ipodui/artkey.h"
#include "ipodui/artfmt.h"
#include "ipodui/sortkey.h"
#include "fake_tagcache.h"
#include "ipodui/ab_store.h"      /* #67: ab_store_pin_key() (faked) */

static int failures;

#define CHECK(cond, ...) do { \
        if (!(cond)) { \
            failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            putchar('\n'); \
        } \
    } while (0)

static const struct lib_filter any = { LIB_ANY, LIB_ANY, LIB_ANY, LIB_ANY };

static const char *label(int v, int i)
{
    static char buf[128];
    return lib_label(v, i, buf, sizeof(buf));
}

static int row_of(int v, const char *name)
{
    int i;

    for (i = 0; i < lib_count(v); i++)
        if (!strcmp(label(v, i), name))
            return i;
    return -1;
}

static void expect_list(int v, const char *what, const char *const *want,
                        int n)
{
    int i;

    CHECK(lib_count(v) == n, "%s: %d rows, want %d", what, lib_count(v), n);
    for (i = 0; i < n && i < lib_count(v); i++)
        CHECK(!strcmp(label(v, i), want[i]), "%s row %d: \"%s\", want \"%s\"",
              what, i, label(v, i), want[i]);
}

static void expect_absent(int v, const char *what, const char *name)
{
    CHECK(row_of(v, name) < 0, "%s lists \"%s\"", what, name);
}

/* -- the small library ------------------------------------------------- */

#define E(t, a, al, aa, g, c, d, n, len) \
    { t, a, al, aa, g, c, "/Music/x.mp3", d, n, len, 0, 0, 0, false, 0 }

static struct fake_entry small[] = {
    /* 0 */ E("Something", "The Beckons", "Ashen Road", "The Beckons", "Rock", "Fenn", 1, 2, 180000),
    /* 1 */ E("Come Along", "The Beckons", "Ashen Road", "The Beckons", "Rock", "Hale", 1, 1, 200000),
    /* 2 */ E("Hyperorbit", "Björvik", "Post", NULL, "Electronic", NULL, 1, 1, 300000),
    /* 3 */ E("Driftwood Holiday", "10dd", "Brisk Tourist", NULL, "Pop", NULL, 1, 1, 250000),
    /* 4 */ E("Waterfall", "ABRA", "Gold", "ABRA", "Pop", NULL, 1, 1, 160000),
    /* 5 */ E("SOS", "abra", "gold", "abra", "Pop", NULL, 1, 2, 200000),
    /* 6 */ E("Hit One", "Ann", "Hits 2000", "Various Artists", "Pop", NULL, 1, 1, 100000),
    /* 7 */ E("Hit Two", "Bob", "Hits 2000", "Various Artists", "Pop", NULL, 1, 2, 100000),
    /* 8 */ E("Chapter 1", "Melville", "Moby Dick", "Melville", "Audiobook", NULL, 1, 1, 100000),
    /* 9 */ E("Chapter 2", "Melville", "Moby Dick", "Melville", "Audiobook", NULL, 1, 2, 100000),
    /*10 */ E("Chapter 3", "Melville", "Moby Dick", "Melville", "Audiobook", NULL, 1, 3, 100000),
    /*11 */ E("Episode A", "Host", "The Show", "Host", "Podcast", NULL, 0, 0, 60000),
    /*12 */ E("Episode B", "Host", "The Show", "Host", "Podcast", NULL, 0, 0, 60000),
    /*13 */ E("Greatest Hit X", "Quill", "Greatest Hits", "Quill", "Rock", NULL, 1, 1, 100000),
    /*14 */ E("Greatest Hit Y", "ABRA", "Greatest Hits", "ABRA", "Pop", NULL, 1, 1, 100000),
    /*15 */ E("Disc Two Opener", "Quill", "Greatest Hits", "Quill", "Rock", NULL, 2, 1, 100000),
    /*16 */ E("Late Track", "Quill", "Greatest Hits", "Quill", "Rock", NULL, 1, 9, 100000),
    /*17 */ E("Gone", "Ghost", "Deleted", NULL, "Rock", NULL, 1, 1, 100000),
};
#define SMALL_N (int)(sizeof(small) / sizeof(small[0]))

static void setup_small(void)
{
    small[11].mtime = 100;              /* Episode A older */
    small[12].mtime = 200;
    small[11].path = "/Podcasts/The Show/a.mp3";
    small[17].deleted = true;
    fake_db_load(small, SMALL_N);
}

static void test_classification(void)
{
    static const char *const artists[] = {
        "ABRA", "The Beckons", "Björvik", "Quill", "10dd",
    };
    static const char *const comps[] = { "Hits 2000" };
    static const char *const books[] = { "Moby Dick" };
    static const char *const shows[] = { "The Show" };
    static const char *const genres[] = { "Electronic", "Pop", "Rock" };
    int v, i;

    setup_small();

    /* Artists: album artist (else artist), no compilations, books, shows;
     * "The" ignored, '#' after Z, case variants merged (ABRA/abra) */
    v = lib_open(LIB_ARTISTS, &any);
    expect_list(v, "artists", artists, 5);
    lib_close(v);

    v = lib_open(LIB_COMPILATIONS, NULL);
    expect_list(v, "compilations", comps, 1);
    lib_close(v);

    v = lib_open(LIB_AUDIOBOOKS, &any);
    expect_list(v, "audiobooks", books, 1);
    lib_close(v);

    v = lib_open(LIB_PODCASTS, &any);
    expect_list(v, "podcasts", shows, 1);
    lib_close(v);

    v = lib_open(LIB_GENRES, &any);
    expect_list(v, "genres", genres, 3);
    lib_close(v);

    /* Songs: every music track (compilations too), no book, no episode,
     * nothing deleted */
    v = lib_open(LIB_SONGS, &any);
    CHECK(lib_count(v) == 12, "songs: %d rows, want 12", lib_count(v));
    for (i = 8; i <= 12; i++)
        expect_absent(v, "songs", small[i].title);
    expect_absent(v, "songs", "Gone");
    CHECK(row_of(v, "Hit One") >= 0, "songs: no compilation track");
    lib_close(v);

    v = lib_open(LIB_ALBUMS, &any);
    expect_absent(v, "albums", "Moby Dick");
    expect_absent(v, "albums", "The Show");
    CHECK(row_of(v, "Hits 2000") >= 0, "albums: no compilation album");
    lib_close(v);
}

/* Video classification must filter Music even without the video player. */
static void test_music_excludes_video(void)
{
    int v;
    setup_small();
    fake_db_set_flags(2, TAGCACHE_UI_VIDEO);
    fake_db_set_flags(6, TAGCACHE_UI_VIDEO | TAGCACHE_UI_COMPILATION);
    fake_db_set_flags(7, TAGCACHE_UI_VIDEO | TAGCACHE_UI_COMPILATION);

    v = lib_open(LIB_SONGS, &any);
    CHECK(lib_count(v) == 9, "video-filtered songs: %d rows, want 9",
          lib_count(v));
    expect_absent(v, "songs", "Hyperorbit");
    expect_absent(v, "songs", "Hit One");
    expect_absent(v, "songs", "Hit Two");
    CHECK(row_of(v, "Something") >= 0, "audio-only song disappeared");
    lib_close(v);

    v = lib_open(LIB_ALBUMS, &any);
    expect_absent(v, "albums", "Post");
    expect_absent(v, "albums", "Hits 2000");
    lib_close(v);
    v = lib_open(LIB_ARTISTS, &any);
    expect_absent(v, "artists", "Björvik");
    lib_close(v);
    v = lib_open(LIB_GENRES, &any);
    expect_absent(v, "genres", "Electronic");
    lib_close(v);
    v = lib_open(LIB_COMPILATIONS, &any);
    expect_absent(v, "compilations", "Hits 2000");
    lib_close(v);
}

static void test_grouping(void)
{
    static const char *const albums[] = {
        "Ashen Road", "Brisk Tourist", "Gold", "Greatest Hits",
        "Greatest Hits", "Hits 2000", "Post",
    };
    static const char *const queen[] = {
        "Greatest Hit X", "Late Track", "Disc Two Opener",
    };
    static const char *const abra_all[] = {
        "Waterfall", "SOS", "Greatest Hit Y",
    };
    struct lib_filter f = any;
    int v, a, i, id, q = -1;

    setup_small();
    v = lib_open(LIB_ALBUMS, &any);
    /* two "Greatest Hits" (Quill, ABRA) stay apart; Gold/gold merge */
    expect_list(v, "albums", albums, 7);

    /* the Quill one: tracks by disc, then track number */
    for (i = 0; i < lib_count(v); i++)
    {
        id = lib_id(v, i);
        if (!strcmp(label(v, i), "Greatest Hits") && small[id].artist[0] == 'Q')
            q = i;
    }
    CHECK(q >= 0, "albums: no Quill Greatest Hits");
    f.album = lib_id(v, q);
    a = lib_open(LIB_SONGS, &f);
    expect_list(a, "queen album", queen, 3);
    lib_close(a);

    /* lib_find() gives the row back from the id */
    CHECK(lib_find(v, lib_id(v, q)) == q, "lib_find: %d, want %d",
          lib_find(v, lib_id(v, q)), q);
    CHECK(lib_find(v, 17) < 0, "lib_find: a deleted entry found");
    lib_close(v);

    /* an artist's All Songs: by album (Gold before Greatest Hits), then
     * track; both letter cases of the artist */
    v = lib_open(LIB_ARTISTS, &any);
    f = any;
    f.artist = lib_id(v, row_of(v, "ABRA"));
    a = lib_open(LIB_SONGS, &f);
    expect_list(a, "abra all songs", abra_all, 3);
    lib_close(a);
    /* and its albums */
    a = lib_open(LIB_ALBUMS, &f);
    CHECK(lib_count(a) == 2, "abra albums: %d, want 2", lib_count(a));
    lib_close(a);
    lib_close(v);

    /* podcast episodes: newest first */
    v = lib_open(LIB_PODCASTS, &any);
    f = any;
    f.album = lib_id(v, 0);
    a = lib_open(LIB_PODCASTS, &f);
    CHECK(lib_count(a) == 2 && !strcmp(label(a, 0), "Episode B"),
          "episodes: newest first (%d rows, first \"%s\")", lib_count(a),
          label(a, 0));
    lib_close(a);
    lib_close(v);

    /* quick-scroll letters */
    v = lib_open(LIB_ARTISTS, &any);
    CHECK(lib_letter(v, row_of(v, "The Beckons")) == 'B', "letter of The Beckons");
    CHECK(lib_letter(v, row_of(v, "10dd")) == '#', "letter of 10dd");
    lib_close(v);
}

static void test_progress(void)
{
    int v, r, e;

    setup_small();
    /* nothing played yet */
    v = lib_open(LIB_AUDIOBOOKS, &any);
    CHECK(lib_progress(v, 0) == -1, "unplayed book: %d", lib_progress(v, 0));
    lib_close(v);

    /* saved position in chapter 2: (100 s + 50 s) / 300 s */
    small[9].lastelapsed = 50000;
    small[9].lastoffset = 1234;
    fake_db_load(small, SMALL_N);
    e = fake_numeric_calls;
    v = lib_open(LIB_AUDIOBOOKS, &any);
    CHECK(lib_progress(v, 0) == 500, "book at 2/50 s: %d, want 500",
          lib_progress(v, 0));
    /* computed at open: drawing the row costs no database call */
    e = fake_numeric_calls;
    r = lib_progress(v, 0) + lib_progress(v, 0);
    CHECK(fake_numeric_calls == e, "lib_progress() of a book row read the "
          "database (%d calls)", fake_numeric_calls - e);

    /* playing chapter 3 at 10 s: the live position, after a refresh */
    fake_play(10, 10000);
    lib_progress_refresh(v);
    CHECK(lib_progress(v, 0) == 700, "book playing 3/10 s: %d, want 700",
          lib_progress(v, 0));
    /* stopped: the saved position again (chapter 3 saved at 20 s) */
    fake_play(-1, 0);
    fake_db_set_stat(10, tag_lastelapsed, 20000);
    lib_progress_refresh(v);
    CHECK(lib_progress(v, 0) == 733, "book saved 3/20 s: %d, want 733",
          lib_progress(v, 0));

    /* selecting the book resumes chapter 3 at its position */
    CHECK(lib_play(v, 0, false) == 0, "lib_play(book) failed");
    CHECK(fake_pl_n == 3 && fake_pl_start == 2 && fake_pl_elapsed == 20000,
          "book resume: %d tracks, start %d at %lu ms", fake_pl_n,
          fake_pl_start, fake_pl_elapsed);
    /* P3: a book queued alone is noted for the resume record */
    CHECK(fake_shuf_book == 1 && lib_queue_is_book(playlist_get_current()),
          "book flag %d", fake_shuf_book);
    lib_close(v);
    (void)r;
    small[9].lastelapsed = 0;
    small[9].lastoffset = 0;
}

static void test_stale(void)
{
    int v, n;
    char want[64];

    setup_small();
    v = lib_open(LIB_SONGS, &any);
    n = lib_count(v);
    strcpy(want, label(v, 0));          /* row 0 is in the label cache */
    fake_db_bump();                     /* the RAM cache was reloaded */
    CHECK(lib_count(v) == n, "stale view: count changed");
    CHECK(!strcmp(label(v, 0), want), "stale view: cached row 0 \"%s\", "
          "want \"%s\"", label(v, 0), want);
    CHECK(label(v, 5)[0] == '\0', "stale view: uncached row has a label");
    CHECK(lib_play(v, 0, false) < 0, "stale view: lib_play() played");
    CHECK(lib_find(v, 0) < 0, "stale view: lib_find() found");
    lib_close(v);

    fake_db_usable(false);              /* a commit is running */
    CHECK(lib_state() == LIB_UPDATING, "not usable: state %d", lib_state());
    v = lib_open(LIB_ARTISTS, &any);
    CHECK(v <= 0 || lib_count(v) == 0, "open while updating: %d rows",
          lib_count(v));
    if (v > 0)
        lib_close(v);
    fake_db_usable(true);
    CHECK(lib_state() == LIB_READY, "usable: state %d", lib_state());
}

/* -- the 2,000-track cap ----------------------------------------------- */

#define BIG_MUSIC 5000
#define BIG_BOOKS 100
static struct fake_entry big[BIG_MUSIC + BIG_BOOKS];
static char big_titles[BIG_MUSIC + BIG_BOOKS][16];

static void test_cap(void)
{
    int i, v;

    for (i = 0; i < BIG_MUSIC + BIG_BOOKS; i++)
    {
        bool book = i >= BIG_MUSIC;

        snprintf(big_titles[i], sizeof(big_titles[i]), "T%05d", i);
        big[i] = (struct fake_entry){ big_titles[i], book ? "Reader" : "Band",
            book ? "Book" : "Album", NULL, book ? "Audiobook" : "Rock",
            NULL, "/x.mp3", 1, i % 99 + 1, 1000, 0, 0, 0, false, 0 };
    }
    fake_db_load(big, BIG_MUSIC + BIG_BOOKS);

    /* Shuffle Songs: the whole library as a library source (#27), no
     * sample: lib_shuffle.c gets the run, 'first' none */
    fake_shuf_calls = 0;
    CHECK(lib_shuffle_all() == 0, "lib_shuffle_all failed");
    CHECK(fake_shuf_calls == 1 && fake_shuf_mode == NP_SHUFFLE_SONGS
          && fake_shuf_first == SHUF_NONE,
          "shuffle all: library source %d/%d", fake_shuf_calls,
          fake_shuf_mode);

    v = lib_open(LIB_SONGS, &any);
    CHECK(lib_count(v) == BIG_MUSIC, "songs: %d", lib_count(v));

    /* playing row 4321 in order: the last 2,000 rows, from that one */
    CHECK(lib_play(v, 4321, false) == 0, "lib_play(4321) failed");
    CHECK(fake_pl_n == 2000 && fake_pl_start == 4321 - 3000
          && fake_pl[fake_pl_start] == lib_id(v, 4321),
          "window: %d tracks, start %d", fake_pl_n, fake_pl_start);
    CHECK(fake_shuf_book == 0 && !lib_queue_is_book(playlist_get_current()),
          "music clears the book flag (%d)", fake_shuf_book);

    /* row 10 shuffled, all songs over the cap: a library source with
     * row 10 playing first (#27) */
    fake_shuf_calls = 0;
    CHECK(lib_play(v, 10, true) == 0, "lib_play(10, shuffle) failed");
    CHECK(fake_shuf_calls == 1 && fake_shuf_mode == NP_SHUFFLE_SONGS
          && fake_shuf_first == (uint32_t)lib_id(v, 10),
          "shuffled over-cap songs: library source %d, first %lu",
          fake_shuf_calls, (unsigned long)fake_shuf_first);
    lib_close(v);
}


/* -- precompute -------------------------------------------------------- */

static const char *db_dir = ".";
static char db_file[512];

static void pre_remove(void)
{
    unlink(db_file);
}

static bool pre_exists(void)
{
    return access(db_file, F_OK) == 0;
}

/* lib_id() of every row of an unfiltered category */
static int collect(int cat, int *ids, int max)
{
    int v = lib_open(cat, &any), n, i;

    n = lib_count(v);
    for (i = 0; i < n && i < max; i++)
        ids[i] = lib_id(v, i);
    lib_close(v);
    return n;
}

static bool opened_from_file(void)
{
    return strstr(fake_last_open, "src=file") != NULL;
}

static int stop_after, stop_calls;
static bool stop_soon(void)
{
    return ++stop_calls > stop_after;
}

/* every unfiltered category: the same rows from the file as from the
 * scan */
static void compare_all(const char *what)
{
    static int a[8][6000], b[8][6000];
    int na[8], nb[8], cat, i;

    pre_remove();
    fake_db_bump();
    for (cat = LIB_ARTISTS; cat <= LIB_PODCASTS; cat++)
    {
        na[cat] = collect(cat, a[cat], 6000);
        CHECK(!opened_from_file(), "%s: cat %d from a file that is gone",
              what, cat);
    }
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE,
          "%s: precompute not done", what);
    CHECK(pre_exists() && strstr(fake_last_pre, "done"),
          "%s: no file (%s)", what, fake_last_pre);
    for (cat = LIB_ARTISTS; cat <= LIB_PODCASTS; cat++)
    {
        nb[cat] = collect(cat, b[cat], 6000);
        CHECK(opened_from_file(), "%s: cat %d not from the file (%s)", what,
              cat, fake_last_open);
        CHECK(na[cat] == nb[cat], "%s: cat %d %d rows scanned, %d from "
              "the file", what, cat, na[cat], nb[cat]);
        for (i = 0; i < na[cat] && i < nb[cat] && i < 6000; i++)
            if (a[cat][i] != b[cat][i])
            {
                CHECK(0, "%s: cat %d row %d: id %d scanned, %d from the "
                      "file", what, cat, i, a[cat][i], b[cat][i]);
                break;
            }
    }
}

static void test_precompute(void)
{
    struct lib_filter f = any;
    char buf[128];
    int v, a, r, gen, builds;
    uint64_t key;

    snprintf(db_file, sizeof(db_file), "%s/%s", db_dir, LIBFILE_NAME);
    setup_small();
    pre_remove();
    fake_db_bump();

    /* before the precompute: scanned, no counts, no keys */
    v = lib_open(LIB_ALBUMS, &any);
    CHECK(!opened_from_file(), "albums from a file that does not exist");
    r = row_of(v, "Ashen Road");
    CHECK(lib_child_count(v, r) == -1 && lib_art_key(v, r) == 0,
          "scanned albums: count %d key %llx", lib_child_count(v, r),
          (unsigned long long)lib_art_key(v, r));
    /* second line: the album artist */
    CHECK(lib_sublabel(v, r, buf, sizeof(buf))
          && !strcmp(buf, "The Beckons"), "sublabel of Ashen Road: %s",
          buf);
    CHECK(!strcmp(lib_sublabel(v, row_of(v, "Hits 2000"), buf, sizeof(buf)),
                  "Various Artists"), "sublabel of Hits 2000: %s", buf);
    lib_close(v);

    /* while audio plays it waits (its block would shrink the buffer) */
    fake_play(0, 1000);
    CHECK(lib_precompute(false, NULL) == LIB_PRE_WAIT, "ran while playing");
    CHECK(!pre_exists(), "a file while playing");
    fake_play(-1, 0);

    /* stopped at the first queued event: nothing left behind */
    stop_after = 3;
    stop_calls = 0;
    CHECK(lib_precompute(false, stop_soon) == LIB_PRE_WAIT,
          "stopped precompute not waiting");
    CHECK(!pre_exists(), "a stopped precompute left a file");

    /* done: the generation changes, the art builder is asked */
    gen = lib_generation();
    builds = fake_build_requests;
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE, "precompute failed");
    CHECK(pre_exists(), "no file");
    CHECK(lib_generation() != gen, "generation unchanged");
    CHECK(fake_build_requests == builds + 1, "art build not requested");
    /* a current file is not rewritten, but the builder is asked again */
    gen = lib_generation();
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE
          && lib_generation() == gen && fake_build_requests == builds + 2,
          "current file rewritten");

    v = lib_open(LIB_ALBUMS, &any);
    CHECK(opened_from_file(), "albums not from the file: %s",
          fake_last_open);
    r = row_of(v, "Ashen Road");
    CHECK(lib_child_count(v, r) == 2, "Ashen Road: %d songs",
          lib_child_count(v, r));
    key = artcache_key("The Beckons", "The Beckons", "Ashen Road", NULL);
    CHECK(lib_art_key(v, r) == key, "Ashen Road key %llx, want %llx",
          (unsigned long long)lib_art_key(v, r), (unsigned long long)key);
    CHECK(lib_child_count(v, row_of(v, "Gold")) == 2, "Gold/gold: %d",
          lib_child_count(v, row_of(v, "Gold")));
    {   /* #49 TRACKS: every album's slice == its album-filter track list */
        struct lib_tracks lt;
        int ar, k, bad = 0;

        CHECK(lib_tracks_open(&lt) == 0 && lt.tracks_off != 0
              && (int)lt.albums == lib_count(v),
              "TRACKS section missing (%d albums)", (int)lt.albums);
        for (ar = 0; ar < lib_count(v) && lt.fd >= 0; ar++)
        {
            struct lib_filter af = { LIB_ANY, LIB_ANY, LIB_ANY, LIB_ANY };
            int sv;

            af.album = lib_id(v, ar);
            sv = lib_open(LIB_SONGS, &af);
            if ((int)lib_tracks_len(&lt, ar) != lib_count(sv)
                || (int)lib_tracks_len(&lt, ar) != lib_child_count(v, ar))
                bad++;
            for (k = 0; k < lib_count(sv); k++)
                bad += lib_tracks_entry(&lt, ar, k) != lib_id(sv, k);
            lib_close(sv);
        }
        lib_tracks_close(&lt);
        CHECK(bad == 0, "TRACKS: %d mismatches", bad);
    }
    CHECK(lib_child_count(v, row_of(v, "Hits 2000")) == 2, "Hits 2000: %d",
          lib_child_count(v, row_of(v, "Hits 2000")));
    CHECK(!strcmp(lib_sublabel(v, r, buf, sizeof(buf)), "The Beckons"),
          "file albums sublabel: %s", buf);
    lib_close(v);

    v = lib_open(LIB_ARTISTS, &any);
    CHECK(lib_child_count(v, row_of(v, "ABRA")) == 3, "ABRA: %d songs",
          lib_child_count(v, row_of(v, "ABRA")));
    CHECK(lib_child_count(v, row_of(v, "Quill")) == 3, "Quill: %d songs",
          lib_child_count(v, row_of(v, "Quill")));
    CHECK(lib_sublabel(v, 0, buf, sizeof(buf)) == NULL, "artist sublabel");
    /* an artist's albums: counts and keys from the album table */
    f.artist = lib_id(v, row_of(v, "ABRA"));
    a = lib_open(LIB_ALBUMS, &f);
    CHECK(!opened_from_file(), "a filtered view from the file");
    CHECK(lib_child_count(a, row_of(a, "Gold")) == 2
          && lib_child_count(a, row_of(a, "Greatest Hits")) == 1,
          "abra albums: Gold %d, Greatest Hits %d",
          lib_child_count(a, row_of(a, "Gold")),
          lib_child_count(a, row_of(a, "Greatest Hits")));
    CHECK(lib_art_key(a, row_of(a, "Gold"))
          == artcache_key("ABRA", "ABRA", "Gold", NULL),
          "abra Gold key");
    CHECK(lib_sublabel(a, 0, buf, sizeof(buf)) == NULL,
          "artist's albums have a sublabel");
    lib_close(a);
    /* the artist's songs: the artist under each */
    a = lib_open(LIB_SONGS, &f);
    CHECK(!strcmp(lib_sublabel(a, 0, buf, sizeof(buf)), "ABRA")
          || !strcmp(buf, "abra"), "abra song sublabel: %s", buf);
    lib_close(a);
    lib_close(v);

    /* Songs: the track artist (not "Various Artists") */
    v = lib_open(LIB_SONGS, &any);
    CHECK(opened_from_file(), "songs not from the file");
    CHECK(!strcmp(lib_sublabel(v, row_of(v, "Hit One"), buf, sizeof(buf)),
                  "Ann"), "Hit One sublabel: %s", buf);
    CHECK(!strcmp(lib_sublabel(v, row_of(v, "Hyperorbit"), buf,
                               sizeof(buf)), "Björvik"),
          "Hyperorbit sublabel: %s", buf);
    lib_close(v);
    /* an album's track list: no second line */
    v = lib_open(LIB_ALBUMS, &any);
    f = any;
    f.album = lib_id(v, row_of(v, "Ashen Road"));
    a = lib_open(LIB_SONGS, &f);
    CHECK(lib_sublabel(a, 0, buf, sizeof(buf)) == NULL,
          "album track list has a sublabel");
    lib_close(a);
    lib_close(v);

    /* books and shows */
    v = lib_open(LIB_AUDIOBOOKS, &any);
    CHECK(lib_child_count(v, 0) == 3, "Moby Dick: %d",
          lib_child_count(v, 0));
    CHECK(!lib_unplayed(v, 0), "a book is unplayed");
    {   /* P6 L2: the books builder's file list (LIB_AUDIOBOOKS TRACKS) */
        uint32_t idx[8];
        int32_t room[2];        /* ROWS searched 2 at a time */
        int n = lib_book_tracks(lib_id(v, 0), idx, 8, room, sizeof(room));

        CHECK(n == 3 && idx[0] == 8 && idx[1] == 9 && idx[2] == 10,
              "book tracks: n=%d [%d %d %d]", n, n > 0 ? (int)idx[0] : -1,
              n > 1 ? (int)idx[1] : -1, n > 2 ? (int)idx[2] : -1);
        CHECK(lib_book_tracks(lib_id(v, 0), idx, 2, room, sizeof(room)) == 3,
              "book tracks over max: the count");
        CHECK(lib_book_tracks(0, idx, 8, room, sizeof(room)) == -1,
              "book tracks of a music track");
    }
    lib_close(v);
    v = lib_open(LIB_PODCASTS, &any);
    CHECK(lib_child_count(v, 0) == 2, "The Show: %d episodes",
          lib_child_count(v, 0));
    CHECK(lib_art_key(v, 0) == artcache_key(NULL, NULL, "The Show", NULL),
          "show key: by album only");
    CHECK(lib_unplayed(v, 0), "a new show is not unplayed");
    f = any;
    f.album = lib_id(v, 0);
    a = lib_open(LIB_PODCASTS, &f);
    CHECK(lib_unplayed(a, 0) && lib_unplayed(a, 1), "new episodes played");
    fake_db_set_stat(12, tag_playcount, 1);     /* Episode B, row 0 */
    CHECK(!lib_unplayed(a, 0) && lib_unplayed(a, 1),
          "episode B played: %d %d", lib_unplayed(a, 0), lib_unplayed(a, 1));
    fake_db_set_stat(11, tag_lastelapsed, 5000); /* A started */
    CHECK(!lib_unplayed(a, 1), "a started episode is unplayed");
    lib_close(a);
    lib_close(v);
    v = lib_open(LIB_PODCASTS, &any);
    CHECK(!lib_unplayed(v, 0), "a show with every episode played");
    lib_close(v);
    fake_db_set_stat(12, tag_playcount, 0);
    fake_db_set_stat(11, tag_lastelapsed, 0);

    /* a new commit: the file is old, the lists scan again */
    fake_db_commit();
    fake_db_bump();
    v = lib_open(LIB_ALBUMS, &any);
    CHECK(!opened_from_file() && lib_child_count(v, 0) == -1,
          "old file used");
    lib_close(v);
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE, "redo failed");
    v = lib_open(LIB_ALBUMS, &any);
    CHECK(opened_from_file(), "new file not used");
    lib_close(v);

    /* a torn file: the scan, and a rebuild is asked for */
    {
        FILE *fp = fopen(db_file, "r+b");
        long size;
        int req = fake_pre_requests;

        struct libfile_hdr h;
        unsigned i;

        CHECK(fp != NULL, "cannot open %s", db_file);
        if (fp)
        {
            /* in the middle of the Songs rows */
            size = fread(&h, sizeof(h), 1, fp);
            for (i = 0; size == 1 && i < h.nsec; i++)
                if (h.sec[i].cat == LIB_SONGS
                    && h.sec[i].col == LIBFILE_ROWS)
                    break;
            CHECK(size == 1 && i < h.nsec, "no songs section");
            if (size == 1 && i < h.nsec)
            {
                fseek(fp, h.sec[i].offset + h.sec[i].count * 2, SEEK_SET);
                fputc(0x5a, fp);
                fputc(0xa5, fp);
            }
            fclose(fp);
        }
        fake_db_bump();
        for (r = LIB_ARTISTS; r <= LIB_PODCASTS; r++)
            lib_close(lib_open(r, &any));
        CHECK(fake_pre_requests > req, "torn file: no rebuild asked");
        CHECK(lib_precompute(true, NULL) == LIB_PRE_DONE, "forced redo");
        fake_db_bump();
        v = lib_open(LIB_SONGS, &any);
        CHECK(opened_from_file(), "rebuilt file not used");
        lib_close(v);
    }

    compare_all("small");
}

/* a bigger library with case variants, untagged albums, compilations,
 * books and shows: every category the same from the file as scanned */
#define MID_N 3000
static struct fake_entry mid[MID_N];
static char mid_s[MID_N][4][40];

static void test_precompute_mid(void)
{
    static const char *const genres[] = {
        "Rock", "rock", "Pop", NULL, "Jazz", "Podcast", "Audiobook",
    };
    int i, v, sum = 0, r;

    for (i = 0; i < MID_N; i++)
    {
        int al = (i * 7) % 211, ar = al % 37;
        const char *g = genres[i % 5];

        snprintf(mid_s[i][0], 40, "Song %d", (i * 31) % 997);
        snprintf(mid_s[i][1], 40, ar % 3 ? "Artist %d" : "ARTIST %d", ar);
        snprintf(mid_s[i][2], 40, al % 5 ? "Album %d" : "album %d", al);
        snprintf(mid_s[i][3], 40, "/Music/d%d/f%d.mp3", al, i);
        if (i % 97 == 0)
            g = genres[5];
        else if (i % 89 == 0)
            g = genres[6];
        mid[i] = (struct fake_entry){ mid_s[i][0], mid_s[i][1],
            al % 23 == 0 ? NULL : mid_s[i][2],
            al % 11 == 0 ? "Various Artists" : (al % 4 ? NULL : mid_s[i][1]),
            g, i % 3 ? "Comp" : NULL, mid_s[i][3], 1 + i % 2, i % 13,
            1000 + i, i, 0, 0, i % 211 == 0, 0 };
    }
    fake_db_load(mid, MID_N);
    compare_all("mid");

    /* the counts add up: every live music track is in one album row */
    v = lib_open(LIB_ALBUMS, &any);
    for (i = 0; i < lib_count(v); i++)
        sum += lib_child_count(v, i);
    r = lib_open(LIB_SONGS, &any);
    CHECK(sum == lib_count(r), "album counts sum %d, songs %d", sum,
          lib_count(r));
    lib_close(r);
    /* untagged albums are keyed by their folder */
    for (i = 0; i < lib_count(v); i++)
        CHECK(lib_art_key(v, i) != 0, "album row %d without a key", i);
    lib_close(v);
}

/* The album table as the art builder reads it (artfmt.c's artalb_check()
 * and artalb_read() over library.c's file): every key A precomputes is
 * artkey's artcache_key() over the representative's strings. Fixture: the
 * golden key tags of tests/artcache/keys.txt, one track per line. A tagged
 * album's key must equal the golden key of its representative's line; an
 * untagged one is keyed by its folder (the fake gives "/e<idx><path>"). */
static const char *keys_txt = "../tests/artcache/keys.txt";

static char *kfield(char *s)
{
    char *r = s, *w = s;

    if (!strcmp(s, "\\N"))
        return NULL;
    while (*r)
    {
        if (*r == '\\' && r[1])
        {
            r++;
            *w++ = *r == 't' ? '\t' : *r == 'n' ? '\n' : *r == 'r' ? '\r' : *r;
            r++;
        }
        else
            *w++ = *r++;
    }
    *w = '\0';
    return s;
}

#define KN 64
static struct fake_entry kent[KN];
static char kline[KN][512];
static unsigned long long kgold[KN];

static void test_albumtab_keys(void)
{
    static uint32_t page[ARTIDX_PAGE_BYTES / 4];
    struct artalb_stamp want;
    struct artalb_hdr h;
    struct artalb_rec out[64];
    FILE *f = fopen(keys_txt, "r");
    char path[600];
    int n = 0, got, i, first = 0, checked = 0, tagged = 0;

    CHECK(f != NULL, "cannot open %s", keys_txt);
    if (!f)
        return;
    while (n < KN && fgets(kline[n], sizeof(kline[n]), f))
    {
        char *fl[5], *q = kline[n];
        int k;

        kline[n][strcspn(kline[n], "\n")] = '\0';
        if (kline[n][0] == '#' || !kline[n][0])
            continue;
        for (k = 0; k < 5 && q; k++)
        {
            fl[k] = q;
            q = strchr(q, '\t');
            if (q)
                *q++ = '\0';
        }
        if (k != 5)
            continue;
        kgold[n] = strtoull(fl[4], NULL, 16);
        for (k = 0; k < 4; k++)
            fl[k] = kfield(fl[k]);
        kent[n] = (struct fake_entry){ "T", fl[1], fl[2], fl[0], NULL, NULL,
            fl[3], 1, 1, 1000, 500 + n, 0, 0, false, 0 };
        n++;
    }
    fclose(f);
    CHECK(n >= 20, "only %d fixture lines", n);
    fake_db_load(kent, n);
    fake_db_commit();
    snprintf(path, sizeof(path), "%s/%s", db_dir, LIBFILE_NAME);
    unlink(path);
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE, "fixture precompute");
    want.commitid = tagcache_ui_commitid();
    want.entries = tagcache_ui_count();
    want.sortkey_version = SORTKEY_VERSION;
    CHECK(artalb_check(path, &want, &h, page) == ARTIDX_OPENED,
          "the builder rejects library.c's album table");
    while ((got = artalb_read(path, &h, first, 64, out)) > 0)
    {
        for (i = 0; i < got; i++)
        {
            const struct artalb_rec *r = &out[i];
            const struct fake_entry *e = &kent[r->rep_idx];
            unsigned long long expect;
            char fpath[600];

            CHECK(r->rep_idx >= 0 && r->rep_idx < n, "rep %d", r->rep_idx);
            if (r->rep_idx < 0 || r->rep_idx >= n)
                continue;
            if (artcache_key(e->albumartist, e->artist, e->album, NULL))
            {
                expect = kgold[r->rep_idx];
                tagged++;
            }
            else
            {
                snprintf(fpath, sizeof(fpath), "/e%d%s", r->rep_idx,
                         e->path ? e->path : "");
                expect = artcache_key(NULL, NULL, NULL, fpath);
            }
            CHECK(r->key == expect, "album rep %d (aa=%s ar=%s al=%s): key "
                  "%016llx, artkey %016llx", r->rep_idx,
                  e->albumartist ? e->albumartist : "NULL",
                  e->artist ? e->artist : "NULL",
                  e->album ? e->album : "NULL",
                  (unsigned long long)r->key, expect);
            CHECK(r->max_mtime >= 500, "rep %d mtime %lu", r->rep_idx,
                  (unsigned long)r->max_mtime);
            CHECK(first + i == 0 || r->rep_idx > out[i ? i - 1 : 0].rep_idx
                  || i == 0, "table not in folder order");
            checked++;
        }
        first += got;
    }
    CHECK(got == 0, "album table read error");
    CHECK(checked >= 8 && tagged >= 6, "only %d albums (%d tagged) checked",
          checked, tagged);
}

/* -- H1 (#41.2): the precompute block reserved at shell start ----------- */

/* bctx stop hook: playback starts after 'n' calls (no event) */
static int start_after, start_calls;
static bool start_playing(void)
{
    if (++start_calls == start_after)
        fake_play(0, 1000);
    return false;
}

static bool pre_done_with(const char *block, int playing)
{
    char want[64];

    snprintf(want, sizeof(want), "block=%s playing=%d", block, playing);
    return strstr(fake_last_pre, "done") && strstr(fake_last_pre, want);
}

static void test_reserve(void)
{
    int blocks, allocs, v;

    snprintf(db_file, sizeof(db_file), "%s/%s", db_dir, LIBFILE_NAME);
    fake_db_load(mid, MID_N);           /* test_precompute_mid() filled it */
    fake_play(-1, 0);

    /* fresh: no database on disk at shell start, nothing reserved; the
     * precompute runs only while stopped, and then reserves its block
     * for that size (H1 end review: no transient block) */
    fake_disk_entries = -1;
    lib_reserve();                      /* (the runs above reserved one) */
    blocks = fake_blocks_used();
    lib_reserve();
    CHECK(fake_blocks_used() == blocks, "reserved without a database");
    pre_remove();
    fake_db_commit();
    fake_db_bump();
    fake_play(0, 1000);
    CHECK(lib_precompute(false, NULL) == LIB_PRE_WAIT && !pre_exists(),
          "fresh: ran while playing without a reserved block");
    fake_play(-1, 0);
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE
          && pre_done_with("reserved", 0), "fresh: stopped run: %s",
          fake_last_pre);
    CHECK(fake_blocks_used() == blocks + 1, "fresh: no block reserved");

    /* ready (also dirty: the master index header is there): reserved for
     * the database + 25 %; the precompute runs during playback without
     * allocating anything */
    fake_disk_entries = MID_N;
    lib_reserve();
    CHECK(fake_blocks_used() == blocks + 1, "ready: no block reserved");
    pre_remove();
    fake_db_commit();
    fake_db_bump();
    fake_play(0, 1000);
    allocs = fake_allocs;
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE && pre_exists()
          && pre_done_with("reserved", 1), "ready: not done while playing: "
          "%s", fake_last_pre);
    CHECK(fake_allocs == allocs, "ready: %d allocations while playing",
          fake_allocs - allocs);
    /* a current file: checked in the reserved block, no allocation */
    allocs = fake_allocs;
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE
          && strstr(fake_last_pre, "current") && fake_allocs == allocs,
          "ready: current check allocated or rewrote: %s", fake_last_pre);
    v = lib_open(LIB_ALBUMS, &any);
    CHECK(opened_from_file(), "ready: albums not from the file");
    lib_close(v);
    fake_play(-1, 0);
    /* the library grew by up to 25 %: still reserved */
    fake_disk_entries = MID_N * 4 / 5;
    lib_reserve();
    pre_remove();
    fake_db_commit();
    fake_db_bump();
    fake_play(0, 1000);
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE
          && pre_done_with("reserved", 1), "+25 %%: %s", fake_last_pre);
    /* a playback start does not stop a run in the reserved block */
    pre_remove();
    fake_db_commit();
    fake_db_bump();
    fake_play(-1, 0);
    start_after = 5;
    start_calls = 0;
    CHECK(lib_precompute(false, start_playing) == LIB_PRE_DONE
          && pre_done_with("reserved", 1), "reserved run stopped by a "
          "playback start: %s", fake_last_pre);
    fake_play(-1, 0);

    /* outgrown: the block is too small; while playing it waits (a
     * transient block would come out of the audio buffer) */
    fake_disk_entries = MID_N / 2;
    lib_reserve();
    pre_remove();
    fake_db_commit();
    fake_db_bump();
    fake_play(0, 1000);
    CHECK(lib_precompute(false, NULL) == LIB_PRE_WAIT && !pre_exists(),
          "outgrown: ran while playing");
    /* stopped: reserved anew for the new size, never the old block and a
     * transient one together; the run is then in the reserved block, so
     * a playback start does not stop it, and the audio buffer is told */
    fake_play(-1, 0);
    start_after = 5;
    start_calls = 0;
    v = fake_regrows;
    CHECK(lib_precompute(false, start_playing) == LIB_PRE_DONE
          && pre_done_with("reserved", 1), "outgrown, stopped: not "
          "reserved anew: %s", fake_last_pre);
    CHECK(fake_blocks_used() == blocks + 1, "outgrown: %d blocks held",
          fake_blocks_used() - blocks);
    CHECK(fake_regrows > v, "outgrown: audio buffer not regrown");
    fake_play(-1, 0);
    /* no memory for the new reservation: a transient block, which a
     * playback start stops (and frees) */
    fake_disk_entries = MID_N / 2;
    lib_reserve();
    pre_remove();
    fake_db_commit();
    fake_db_bump();
    fake_alloc_fail = 1;                /* pre_reserve()'s allocation */
    start_after = 5;
    start_calls = 0;
    CHECK(lib_precompute(false, start_playing) == LIB_PRE_WAIT
          && !pre_exists(), "transient run not stopped by a playback "
          "start: %s", fake_last_pre);
    CHECK(fake_blocks_used() == blocks, "transient block kept");
    fake_play(-1, 0);
    fake_alloc_fail = 1;
    v = fake_regrows;
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE
          && pre_done_with("transient", 0), "no memory, stopped: %s",
          fake_last_pre);
    CHECK(fake_regrows > v, "transient: audio buffer not regrown");
    CHECK(fake_blocks_used() == blocks, "transient block kept (2)");
    /* next boot */
    fake_disk_entries = MID_N;
    lib_reserve();
    pre_remove();
    fake_db_commit();
    fake_db_bump();
    fake_play(0, 1000);
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE
          && pre_done_with("reserved", 1), "resized: %s", fake_last_pre);
    fake_play(-1, 0);

    fake_disk_entries = -1;
    lib_reserve();                      /* frees it (end-of-test check) */
    CHECK(fake_blocks_used() == blocks, "reserved block not freed");
}

/* -- H1: the panel set (LIBFILE_CAT_PANELSET) ----------------------------- */
#define PN 400
static struct fake_entry pent[PN];
static char pname[PN][3][32];

static int read_panels(struct artalb_rec *out)
{
    static uint32_t page[ARTIDX_PAGE_BYTES / 4];
    struct artalb_stamp want;
    struct artalb_hdr h;

    want.commitid = tagcache_ui_commitid();
    want.entries = tagcache_ui_count();
    want.sortkey_version = SORTKEY_VERSION;
    if (artalb_check(db_file, &want, &h, page) != ARTIDX_OPENED)
        return -2;
    return artalb_panels(db_file, &h, out, LIBFILE_PANELS_MAX);
}

static void test_panels(void)
{
    struct artalb_rec a[LIBFILE_PANELS_MAX], b[LIBFILE_PANELS_MAX];
    int i, j, n, m, music = 0;

    snprintf(db_file, sizeof(db_file), "%s/%s", db_dir, LIBFILE_NAME);
    /* 100 albums of 4 tracks: 80 music, 10 books, 10 shows; tracks of
     * album k have mtime 1000 + k (album 79 the newest music album) */
    for (i = 0; i < PN; i++)
    {
        int al = i / 4;
        const char *g = al < 80 ? "Rock" : al < 90 ? "Audiobook" : "Podcast";

        snprintf(pname[i][0], 32, "T%d", i);
        snprintf(pname[i][1], 32, "Artist %d", al % 13);
        snprintf(pname[i][2], 32, "Album %d", al);
        pent[i] = (struct fake_entry){ pname[i][0], pname[i][1],
            pname[i][2], NULL, g, NULL, "/Music/p.mp3", 1, 1 + i % 4,
            1000, 1000 + al, 0, 0, false, 0 };
    }
    fake_db_load(pent, PN);
    /* played: album 5 (twice), then 40, then 12 last of all; a book */
    fake_db_set_stat(20, tag_lastplayed, 10);   /* album 5 */
    fake_db_set_stat(21, tag_lastplayed, 50);   /* album 5 again */
    fake_db_set_stat(160, tag_lastplayed, 60);  /* album 40 */
    fake_db_set_stat(48, tag_lastplayed, 70);   /* album 12 */
    fake_db_set_stat(330, tag_lastplayed, 99);  /* a book: never */
    fake_db_commit();
    fake_db_bump();
    pre_remove();
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE, "panels precompute");
    n = read_panels(a);
    CHECK(n == LIBFILE_PANELS_MAX, "panel set of %d, want 32", n);
    if (n < 4)
        return;
    /* #58: the last PANEL_BOOKS (8) are books (10 in the library): the
     * played one (album 82), then the newest (89, 88, ... 83); music
     * keeps the other 24, never a show */
    for (i = 0; i < n; i++)
    {
        bool book = i >= n - 8;
        int al = a[i].rep_idx / 4;

        CHECK(!!(a[i].flags & LIBFILE_ALB_BOOK) == book
              && (book ? al >= 80 && al < 90 : al < 80),
              "panel %d: album %d flags %x, want %s", i, al, a[i].flags,
              book ? "a book" : "music");
        if (book)
            CHECK(al == (i == n - 8 ? 82 : 89 - (i - (n - 7))),
                  "book panel %d: album %d", i, al);
    }
    /* recently played first, newest play first, one per album */
    CHECK(a[0].rep_idx / 4 == 12 && a[1].rep_idx / 4 == 40
          && a[2].rep_idx / 4 == 5, "played order: albums %d %d %d",
          a[0].rep_idx / 4, a[1].rep_idx / 4, a[2].rep_idx / 4);
    /* then the newest (79, 78, ...) */
    CHECK(a[3].rep_idx / 4 == 79 && a[4].rep_idx / 4 == 78,
          "newest: albums %d %d", a[3].rep_idx / 4, a[4].rep_idx / 4);
    for (i = 0; i < n; i++)
    {
        CHECK(a[i].key == artcache_key(NULL, pent[a[i].rep_idx].artist,
                                       pent[a[i].rep_idx].album, NULL),
              "panel %d key", i);
        for (j = 0; j < i; j++)
            CHECK(a[j].key != a[i].key, "panel %d repeats %d", i, j);
    }
    /* the same set for the same database */
    pre_remove();
    fake_db_bump();
    CHECK(lib_precompute(true, NULL) == LIB_PRE_DONE, "panels redo");
    m = read_panels(b);
    CHECK(m == n && !memcmp(a, b, n * sizeof(a[0])), "set changed without "
          "a database change");
    /* deleting an album outside the set leaves the set alone (the
     * builder redoes nothing), same commit epoch */
    {
        int gone = -1, al, common = 0;

        for (al = 0; al < 80 && gone < 0; al++)
        {
            for (j = 0; j < n && a[j].rep_idx / 4 != al; j++)
                ;
            if (j == n)
                gone = al;
        }
        CHECK(gone >= 0, "every album in the set");
        for (i = 0; gone >= 0 && i < 4; i++)
            pent[gone * 4 + i].deleted = true;
        fake_db_load(pent, PN);
        fake_db_set_stat(20, tag_lastplayed, 10);
        fake_db_set_stat(21, tag_lastplayed, 50);
        fake_db_set_stat(160, tag_lastplayed, 60);
        fake_db_set_stat(48, tag_lastplayed, 70);
        CHECK(lib_precompute(true, NULL) == LIB_PRE_DONE, "panels redo 2");
        m = read_panels(b);
        for (i = 0; i < m; i++)
            for (j = 0; j < n; j++)
                common += b[i].key == a[j].key;
        CHECK(m == n && common == n, "deleting album %d changed the set: "
              "%d of %d kept", gone, common, n);
        for (i = 0; gone >= 0 && i < 4; i++)
            pent[gone * 4 + i].deleted = false;
    }

    /* no books: all 32 are music */
    for (i = 0; i < PN; i++)
        if (i / 4 >= 80 && i / 4 < 90)
            pent[i].genre = "Rock";
    fake_db_load(pent, PN);
    fake_db_commit();
    fake_db_bump();
    pre_remove();
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE, "no-books precompute");
    n = read_panels(a);
    for (i = 0, j = 0; i < n; i++)
        j += !!(a[i].flags & LIBFILE_ALB_BOOK) || a[i].rep_idx / 4 >= 90;
    CHECK(n == LIBFILE_PANELS_MAX && j == 0,
          "no books: %d panels, %d books or shows", n, j);

    /* the small library: every music album, then its books, no show */
    setup_small();
    fake_db_commit();
    fake_db_bump();
    pre_remove();
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE, "small precompute");
    n = read_panels(a);
    {
        int v = lib_open(LIB_ALBUMS, &any), books;

        music = lib_count(v);
        lib_close(v);
        v = lib_open(LIB_AUDIOBOOKS, &any);
        books = lib_count(v);
        lib_close(v);
        for (i = 0, j = 0; i < n; i++)
            j += !!(a[i].flags & LIBFILE_ALB_BOOK);
        CHECK(books >= 1 && n == music + books && j == books,
              "small: %d panels (%d books), %d music albums, %d books",
              n, j, music, books);
        for (i = 0; i < n && i < music; i++)
            CHECK(!(a[i].flags & LIBFILE_ALB_BOOK), "small: panel %d is a "
                  "book before the music ends", i);
    }
}

/* -- #59: the books list by title or by author --------------------------- */

static struct fake_entry shelf[] = {
    /* 0 */ E("Part 1", "Ada Quill", "Zebra Crossing", "Ada Quill", "Audiobook", NULL, 1, 1, 1000),
    /* 1 */ E("Part 1", "Zed Marlowe", "Apple Orchard", NULL, "Audiobook", NULL, 1, 1, 1000),
    /* 2 */ E("Part 1", "Ada Quill", "Middle March", "Ada Quill", "Audiobook", NULL, 1, 1, 1000),
    /* 3 */ E("Part 2", "Ada Quill", "Middle March", "Ada Quill", "Audiobook", NULL, 1, 2, 1000),
    /* 4 */ E("Part 1", "Bo Reed", "Kettle Song", "Bo Reed", "Audiobook", NULL, 1, 1, 1000),
    /* 5 */ E("Part 1", NULL, "Nameless", NULL, "Audiobook", NULL, 1, 1, 1000),
    /* 6 */ E("Song", "Ada Quill", "Music Album", "Ada Quill", "Rock", NULL, 1, 1, 1000),
};

/* A file as written before #59: the header without the author slice
 * (its bytes stay, unreferenced), resealed. False if it had none. */
static bool strip_author_slice(void)
{
    struct libfile_hdr h;
    struct libfile_sum_state st;
    FILE *f = fopen(db_file, "r+b");
    unsigned i, j;
    bool ok;

    if (!f)
        return false;
    ok = fread(&h, sizeof(h), 1, f) == 1;
    for (i = 0, j = 0; ok && i < h.nsec; i++)
        if (h.sec[i].cat != LIBFILE_CAT_BOOKS_AUTHOR)
            h.sec[j++] = h.sec[i];
    ok = ok && j + 3 == h.nsec;
    if (ok)
    {
        memset(&h.sec[j], 0, (h.nsec - j) * sizeof(h.sec[0]));
        h.nsec = j;
        h.sum = 0;
        libfile_sum_init(&st);
        libfile_sum_add(&st, &h, sizeof(h));
        h.sum = libfile_sum_end(&st);
        ok = fseek(f, 0, SEEK_SET) == 0 && fwrite(&h, sizeof(h), 1, f) == 1;
    }
    fclose(f);
    return ok;
}

static void test_book_order(void)
{
    static const char *const by_title[] = {
        "Apple Orchard", "Kettle Song", "Middle March", "Nameless",
        "Zebra Crossing" };
    static const char *const by_author[] = {
        "Middle March", "Zebra Crossing", "Kettle Song", "Nameless",
        "Apple Orchard" };
    static const char letters[] = "AABUZ";
    int v, i, gen, pre;

    snprintf(db_file, sizeof(db_file), "%s/%s", db_dir, LIBFILE_NAME);
    fake_db_load(shelf, (int)(sizeof(shelf) / sizeof(shelf[0])));
    pre_remove();
    fake_db_bump();

    /* no file: title order from the scan, also under Author */
    lib_set_book_order(LIB_BOOKS_BY_AUTHOR);
    v = lib_open(LIB_AUDIOBOOKS, &any);
    CHECK(!opened_from_file(), "books from a file that does not exist");
    expect_list(v, "books by author, scanned", by_title, 5);
    lib_close(v);

    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE, "precompute failed");
    lib_set_book_order(LIB_BOOKS_BY_TITLE);
    v = lib_open(LIB_AUDIOBOOKS, &any);
    CHECK(opened_from_file(), "books by title not from the file");
    expect_list(v, "books by title", by_title, 5);
    CHECK(lib_letter(v, 0) == 'A' && lib_letter(v, 4) == 'Z',
          "title letters %c %c", lib_letter(v, 0), lib_letter(v, 4));
    lib_close(v);

    gen = lib_generation();
    lib_set_book_order(LIB_BOOKS_BY_AUTHOR);
    CHECK(lib_generation() != gen, "order change kept the generation");
    gen = lib_generation();
    lib_set_book_order(LIB_BOOKS_BY_AUTHOR);
    CHECK(lib_generation() == gen, "no change moved the generation");
    v = lib_open(LIB_AUDIOBOOKS, &any);
    CHECK(opened_from_file(), "books by author not from the file");
    expect_list(v, "books by author", by_author, 5);
    for (i = 0; i < 5 && i < lib_count(v); i++)
        CHECK(lib_letter(v, i) == letters[i], "author letter of row %d: "
              "%c, want %c", i, lib_letter(v, i), letters[i]);
    /* counts and keys moved with their rows */
    CHECK(lib_child_count(v, 0) == 2 && lib_child_count(v, 1) == 1,
          "author-order counts %d %d", lib_child_count(v, 0),
          lib_child_count(v, 1));
    CHECK(lib_art_key(v, 0) == artcache_key("Ada Quill", "Ada Quill",
                                            "Middle March", NULL)
          && lib_art_key(v, 4) == artcache_key(NULL, "Zed Marlowe",
                                               "Apple Orchard", NULL),
          "author-order keys follow their rows");
    lib_close(v);

    /* a file from before #59 (no author slice): Author lists the books
     * in title order from that file, which is not taken as damaged; the
     * next precompute rewrites it once, then finds it current */
    CHECK(strip_author_slice(), "could not strip the author slice");
    pre = fake_pre_requests;
    v = lib_open(LIB_AUDIOBOOKS, &any);
    CHECK(opened_from_file(), "old file: books not from the file (%s)",
          fake_last_open);
    expect_list(v, "books by author, old file", by_title, 5);
    CHECK(lib_count(v) == 5 && lib_letter(v, 4) == 'Z',
          "old file: letters by author (%c)", lib_letter(v, 4));
    lib_close(v);
    v = lib_open(LIB_ALBUMS, &any);
    CHECK(opened_from_file() && fake_pre_requests == pre,
          "old file taken as damaged (requests %d -> %d, %s)", pre,
          fake_pre_requests, fake_last_open);
    lib_close(v);
    gen = lib_generation();
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE
          && strstr(fake_last_pre, "done") && lib_generation() != gen,
          "old file not rewritten (%s)", fake_last_pre);
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE
          && strstr(fake_last_pre, "current"),
          "rewritten file not current (%s)", fake_last_pre);
    v = lib_open(LIB_AUDIOBOOKS, &any);
    expect_list(v, "books by author, rewritten file", by_author, 5);
    lib_close(v);
    lib_set_book_order(LIB_BOOKS_BY_TITLE);
}

/* -- #67: the last-played book on top (Phase 8b §3.2) ------------------- */

/* the store's answer, set by the test (ab_store.c is tests/abbook's) */
static uint64_t fake_pin;
static uint32_t fake_pin_gen;

ab_key_t ab_store_pin_key(uint32_t *gen)
{
    if (gen)
        *gen = fake_pin_gen;
    return fake_pin;
}

enum ab_pin_src ab_store_pin_src(void)
{
    return fake_pin ? AB_PIN_PENDING : AB_PIN_NONE;
}

static struct fake_entry pinshelf[] = {
    /* 0 */ E("Part 1", "Ada Quill", "Zebra Crossing", "Ada Quill", "Audiobook", NULL, 1, 1, 1000),
    /* 1 */ E("Part 1", "Zed Marlowe", "Apple Orchard", NULL, "Audiobook", NULL, 1, 1, 1000),
    /* 2 */ E("Part 1", "Ada Quill", "Middle March", "Ada Quill", "Audiobook", NULL, 1, 1, 1000),
    /* 3 */ E("Part 2", "Ada Quill", "Middle March", "Ada Quill", "Audiobook", NULL, 1, 2, 1000),
    /* 4 */ E("Part 1", "A Narrator", "Kettle Song", "Bo Reed", "Audiobook", NULL, 1, 1, 1000),
    /* 5 */ E("Part 1", "Cy Tell", NULL, NULL, "Audiobook", NULL, 1, 1, 1000),
    /* 6 */ E("Song", "Ada Quill", "Music Album", "Ada Quill", "Rock", NULL, 1, 1, 1000),
    /* 7 */ E("Ep 1", "Host", "A Show", NULL, "Podcast", NULL, 1, 1, 1000),
    /* 8 */ E("Ep 2", "Host", "B Show", NULL, "Podcast", NULL, 1, 1, 1000),
};

static void test_book_pin(void)
{
    static const char *const by_title[] = {
        "Apple Orchard", "Kettle Song", "Middle March", "Part 1",
        "Zebra Crossing" };
    static const char *const middle_first[] = {
        "Middle March", "Apple Orchard", "Kettle Song", "Part 1",
        "Zebra Crossing" };
    static const char *const kettle_first[] = {
        "Kettle Song", "Apple Orchard", "Middle March", "Part 1",
        "Zebra Crossing" };
    uint64_t middle = artcache_key("Ada Quill", "Ada Quill", "Middle March",
                                   NULL);
    /* albumartist != artist: the key artcache_key_for_id3() gives the
     * first track (album artist first; the path only without an album) */
    uint64_t kettle = artcache_key("Bo Reed", "A Narrator", "Kettle Song",
                                   "/Music/x.mp3");
    uint64_t zebra = artcache_key("Ada Quill", "Ada Quill",
                                  "Zebra Crossing", NULL);
    uint64_t folder = artcache_key(NULL, NULL, NULL, "/Music/x.mp3");
    uint32_t keys[8] = { 1, 2, 3, 4, 5, 0, 0, 0 };
    int v, i, gen;

    /* the pure search over (low, high) pairs */
    keys[6] = (uint32_t)middle;
    keys[7] = (uint32_t)(middle >> 32);
    CHECK(lib_pin_find(keys, 4, middle) == 3, "pin_find: found");
    CHECK(lib_pin_find(keys, 3, middle) == -1, "pin_find: past n");
    CHECK(lib_pin_find(keys, 4, (uint64_t)4 << 32 | 3) == 1,
          "pin_find: (low, high) order");
    CHECK(lib_pin_find(keys, 4, (uint64_t)9 << 32 | 3) == -1,
          "pin_find: equal in its low word only");
    CHECK(lib_pin_find(keys, 4, (uint64_t)3 << 32 | 4) == -1,
          "pin_find: words swapped");
    CHECK(lib_pin_find(keys, 1, (uint64_t)2 << 32 | 1) == -1,
          "pin_find: n < 2");
    CHECK(lib_pin_find(keys, 4, 0) == -1, "pin_find: key 0");
    CHECK(kettle == artcache_key("Bo Reed", "A Narrator", "Kettle Song",
                                 NULL), "kettle: the path changes the key");

    fake_db_load(pinshelf, (int)(sizeof(pinshelf) / sizeof(pinshelf[0])));
    pre_remove();
    fake_db_bump();
    lib_set_book_order(LIB_BOOKS_BY_TITLE);

    /* no book to pin */
    fake_pin = 0;
    v = lib_open(LIB_AUDIOBOOKS, &any);
    expect_list(v, "books, no pin", by_title, 5);
    CHECK(strstr(fake_last_pin, "none why=nokey") != NULL, "no pin: %s",
          fake_last_pin);
    CHECK(lib_letter(v, 0) == 'A', "no pin: letter %c", lib_letter(v, 0));
    lib_close(v);

    /* the scan path (no precompute file): keys from the RAM cache */
    fake_pin = middle;
    v = lib_open(LIB_AUDIOBOOKS, &any);
    CHECK(!opened_from_file(), "scan path: from a file");
    expect_list(v, "books pinned, scan", middle_first, 5);
    CHECK(strstr(fake_last_pin, "row=2 ") && strstr(fake_last_pin,
          "order=title path=scan") && strstr(fake_last_pin, "src=pending"),
          "scan pin trace: %s", fake_last_pin);
    CHECK(lib_letter(v, 0) == 0 && lib_letter(v, 1) == 'A'
          && lib_letter(v, 2) == 'K' && lib_letter(v, 4) == 'Z',
          "scan letters %d %c %c %c", lib_letter(v, 0), lib_letter(v, 1),
          lib_letter(v, 2), lib_letter(v, 4));
    CHECK(lib_art_key(v, 0) == middle && lib_art_key(v, 4) == zebra,
          "scan keys follow their rows");
    CHECK(lib_art_key(v, 3) == 0, "scan: the untagged book has a key");
    lib_close(v);
    /* albumartist != artist on the scan path */
    fake_pin = kettle;
    v = lib_open(LIB_AUDIOBOOKS, &any);
    expect_list(v, "kettle pinned, scan", kettle_first, 5);
    CHECK(lib_art_key(v, 0) == kettle, "scan: kettle key");
    lib_close(v);
    /* an untagged book (keyed by its folder) is never pinned */
    fake_pin = folder;
    v = lib_open(LIB_AUDIOBOOKS, &any);
    expect_list(v, "untagged pin, scan", by_title, 5);
    CHECK(strstr(fake_last_pin, "none why=absent") != NULL,
          "untagged scan: %s", fake_last_pin);
    lib_close(v);

    /* the file path: the same, counts move with their rows */
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE, "precompute failed");
    fake_pin = middle;
    v = lib_open(LIB_AUDIOBOOKS, &any);
    CHECK(opened_from_file(), "pinned books not from the file");
    expect_list(v, "books pinned, file", middle_first, 5);
    CHECK(strstr(fake_last_pin, "row=2 ") && strstr(fake_last_pin,
          "order=title path=file"), "file pin trace: %s", fake_last_pin);
    CHECK(lib_child_count(v, 0) == 2 && lib_child_count(v, 1) == 1,
          "file counts %d %d", lib_child_count(v, 0), lib_child_count(v, 1));
    CHECK(lib_art_key(v, 0) == middle && lib_art_key(v, 4) == zebra,
          "file keys follow their rows");
    CHECK(lib_letter(v, 0) == 0 && lib_letter(v, 1) == 'A',
          "file letters %d %c", lib_letter(v, 0), lib_letter(v, 1));
    lib_close(v);
    fake_pin = kettle;
    v = lib_open(LIB_AUDIOBOOKS, &any);
    expect_list(v, "kettle pinned, file", kettle_first, 5);
    lib_close(v);
    /* the untagged book: whatever key the file gave it (its folder's on
     * the device), it is never pinned */
    fake_pin = folder;
    v = lib_open(LIB_AUDIOBOOKS, &any);
    expect_list(v, "untagged pin, file", by_title, 5);
    CHECK(strstr(fake_last_pin, "pin none") != NULL, "untagged file: %s",
          fake_last_pin);
    fake_pin = lib_art_key(v, 3);
    lib_close(v);
    if (fake_pin)
    {
        v = lib_open(LIB_AUDIOBOOKS, &any);
        expect_list(v, "untagged pin by its file key", by_title, 5);
        CHECK(strstr(fake_last_pin, "none why=nokey") != NULL,
              "untagged file key: %s", fake_last_pin);
        lib_close(v);
    }
    /* a book not in the view (deleted): nothing moves */
    fake_pin = 0x123456789abcdefull;
    v = lib_open(LIB_AUDIOBOOKS, &any);
    expect_list(v, "absent pin", by_title, 5);
    CHECK(strstr(fake_last_pin, "none why=absent") != NULL, "absent: %s",
          fake_last_pin);
    lib_close(v);

    /* author order: the pinned row on top, the rest by author */
    lib_set_book_order(LIB_BOOKS_BY_AUTHOR);
    fake_pin = zebra;
    v = lib_open(LIB_AUDIOBOOKS, &any);
    CHECK(lib_count(v) == 5 && !strcmp(label(v, 0), "Zebra Crossing")
          && !strcmp(label(v, 1), "Middle March"),
          "author pin: %s, %s", label(v, 0), label(v, 1));
    CHECK(strstr(fake_last_pin, "order=author path=file") != NULL,
          "author pin trace: %s", fake_last_pin);
    CHECK(lib_letter(v, 0) == 0 && lib_letter(v, 1) == 'A',
          "author letters %d %c", lib_letter(v, 0), lib_letter(v, 1));
    CHECK(lib_art_key(v, 0) == zebra && lib_art_key(v, 1) == middle,
          "author keys follow their rows");
    lib_close(v);
    lib_set_book_order(LIB_BOOKS_BY_TITLE);

    /* Podcasts: no pin pass, no trace, every row its letter */
    fake_last_pin[0] = '\0';
    v = lib_open(LIB_PODCASTS, &any);
    CHECK(lib_count(v) == 2 && fake_last_pin[0] == '\0',
          "podcasts pinned: %s", fake_last_pin);
    for (i = 0; i < lib_count(v); i++)
        CHECK(lib_letter(v, i) != 0, "podcast row %d without a letter", i);
    lib_close(v);

    /* one book: nothing to move */
    fake_db_load(&pinshelf[2], 1);
    fake_db_bump();
    pre_remove();
    fake_pin = middle;
    v = lib_open(LIB_AUDIOBOOKS, &any);
    CHECK(lib_count(v) == 1 && strstr(fake_last_pin, "none why=small"),
          "one book: %d %s", lib_count(v), fake_last_pin);
    CHECK(lib_letter(v, 0) == 'M', "one book: letter %c", lib_letter(v, 0));
    lib_close(v);

    /* the books-only generation */
    gen = lib_generation();
    fake_pin_gen = 7;
    CHECK(lib_pin_generation() == 7, "pin generation");
    CHECK(lib_generation() == gen, "the pin moved lib_generation()");
    fake_pin = 0;
    fake_pin_gen = 0;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        db_dir = argv[1];
    if (argc > 2)
        keys_txt = argv[2];
    fake_db_dir(db_dir);
    test_classification();
    test_music_excludes_video();
    test_grouping();
    test_progress();
    test_stale();
    test_cap();
    test_precompute();
    test_precompute_mid();
    test_albumtab_keys();
    test_reserve();
    test_panels();
    test_book_order();
    test_book_pin();
    CHECK(fake_boost == 0, "cpu_boost unbalanced: %d", fake_boost);
    /* a stopped precompute reserves its block (kept for good, as at
     * shell start): free it, then only the pool (one block) may stay */
    fake_disk_entries = -1;
    lib_reserve();
    CHECK(fake_blocks_used() <= 1, "%d blocks left", fake_blocks_used());
    CHECK(fake_yields > 0, "nothing yielded (the moving RAM cache was not "
          "exercised)");
    if (failures)
    {
        printf("%d library checks failed\n", failures);
        return 1;
    }
    return 0;
}
