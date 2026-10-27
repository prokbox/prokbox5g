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

/* Host tests of Phase 11 Search and stats in apps/ipodui/library.c
 * (docs/phase11-brief.md 3.1, 3.8, 4.2, 4.3), over the fake tagcache of
 * tests/ipodui (movable memory that moves on every yield, a recording
 * playlist):
 *  - one list across kinds in sort-key order, labels and second lines by
 *    kind, unknown artists/albums and book chapters left out;
 *  - refine in place (same handle, refine=1), delete rebuilds;
 *  - the cap while playing: no pool growth, no core_alloc, the
 *    alphabetical head kept, "capped" set; a capped view is rebuilt, not
 *    refined; cancel aborts; no database -> < 0;
 *  - lib_play() of a song / episode row, lib_row_tracks();
 *  - lib_stats(): false before any precompute, the STATS section, the
 *    current-file path, lib_stats_live() equal;
 *  - the podcast dot: an episode loaded at 0 ms is not unplayed. GPL v2+ */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "tagcache.h"
#include "ipodui/library.h"
#include "ipodui/search.h"
#include "ipodui/libfile.h"
#include "fake_tagcache.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            putchar('\n'); \
        } \
    } while (0)

/* -- fakes this test adds ---------------------------------------------- */
static char last_search[256], last_begin[256];
static int otg_play_calls;

void test_trace(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    if (!strncmp(fmt, "search begin", 12))
        vsnprintf(last_begin, sizeof(last_begin), fmt, ap);
    else if (!strncmp(fmt, "search", 6))
        vsnprintf(last_search, sizeof(last_search), fmt, ap);
    va_end(ap);
    if (getenv("TEST_LIBRARY_TRACE"))
    {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        putchar('\n');
    }
}

int otg_play(int pos)
{
    (void)pos;
    otg_play_calls++;
    return 0;
}

/* -- fixture ------------------------------------------------------------ */
#define E(t, a, al, aa, g, len) \
    { t, a, al, aa, g, NULL, "/Music/x.mp3", 1, 1, len, 0, 0, 0, false, 0 }

static struct fake_entry lib[] = {
    /* 0 */ E("Haiku", "Hank", "Harbor", "Hank", "Rock", 200000),
    /* 1 */ E("Halcyon", "Orbital", "Orbital 2", "Orbital", "Dance", 300000),
    /* 2 */ E("Hey Juno", "The Beckons", "Hey Juno", "The Beckons", "Rock",
              400000),
    /* 3 */ E("The Hardest Part", "Coldplay", "X&Y", "Coldplay", "Rock",
              250000),
    /* 4 */ E("Untagged Song", NULL, NULL, NULL, NULL, 100000),
    /* 5 */ E("Chapter 1", "Hemingway", "Hills Like White Elephants",
              "Hemingway", "Audiobook", 600000),
    /* 6 */ E("Chapter 2", "Hemingway", "Hills Like White Elephants",
              "Hemingway", "Audiobook", 600000),
    /* 7 */ E("Happy Hour Ep", "Host", "Hello Show", "Host", "Podcast",
              900000),
    /* 8 */ E("Song B", "Hank", "Harbor", "Hank", "Rock", 150000),
};
#define LIB_N (int)(sizeof(lib) / sizeof(lib[0]))

static const char *label(int v, int i)
{
    static char buf[128];
    return lib_label(v, i, buf, sizeof(buf));
}

static const char *sub(int v, int i)
{
    static char buf[128];
    const char *s = lib_sublabel(v, i, buf, sizeof(buf));
    return s ? s : "(none)";
}

static void expect_rows(int v, const char *what, const char *const *labels,
                        const int *kinds, int n)
{
    int i;

    CHECK(lib_count(v) == n, "%s: %d rows, want %d", what, lib_count(v), n);
    for (i = 0; i < n && i < lib_count(v); i++)
    {
        uint32_t id = (uint32_t)lib_id(v, i);
        CHECK(!strcmp(label(v, i), labels[i]) && (int)SRCH_ROW_KIND(id)
              == kinds[i], "%s row %d: \"%s\" kind %d, want \"%s\" kind %d",
              what, i, label(v, i), (int)SRCH_ROW_KIND(id), labels[i],
              kinds[i]);
    }
}

static int search(const char *text, int view, bool *capped)
{
    struct srch_query q;

    srch_fold_query(text, &q);
    return lib_search(&q, view, capped, NULL);
}

static void test_stats_first(void)
{
    struct lib_stats st;
    struct lib_stats live;
    long long bits = 0;
    int i;

    fake_db_load(lib, LIB_N);
    for (i = 0; i < LIB_N; i++)
    {
        fake_db_set_stat(i, tag_bitrate, 256 + i);
        bits += (long long)(256 + i) * lib[i].length;
    }
    CHECK(!lib_stats(&st), "lib_stats true before any precompute");
    CHECK(lib_stats_live(&live) && live.songs == 6 && live.books == 2
          && live.episodes == 1 && live.audio_mib == (uint32_t)(bits >> 23),
          "live stats %d/%d/%d %u", live.songs, live.books, live.episodes,
          (unsigned)live.audio_mib);
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE, "precompute");
    CHECK(lib_stats(&st) && st.songs == 6 && st.books == 2
          && st.episodes == 1 && st.audio_mib == live.audio_mib
          && st.gen == live.gen, "stats %d/%d/%d %u", st.songs, st.books,
          st.episodes, (unsigned)st.audio_mib);
    /* found current: re-read from the file's STATS section */
    fake_db_bump();
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE, "precompute again");
    CHECK(lib_stats(&st) && st.songs == 6 && st.audio_mib == live.audio_mib,
          "stats from the current file");
}

static void test_search(void)
{
    static const char *const h_labels[] = {
        "Haiku", "Halcyon", "Hank", "Happy Hour Ep", "Harbor",
        "The Hardest Part", "Hey Juno", "Hey Juno",
        "Hills Like White Elephants",
    };
    static const int h_kinds[] = {
        SRCH_SONG, SRCH_SONG, SRCH_ARTIST, SRCH_EPISODE, SRCH_ALBUM,
        SRCH_SONG, SRCH_ALBUM, SRCH_SONG, SRCH_BOOK,
    };
    static const int ha_kinds[] = {
        SRCH_SONG, SRCH_SONG, SRCH_ARTIST, SRCH_EPISODE, SRCH_ALBUM,
        SRCH_SONG,
    };
    bool capped;
    int v, v2, i, allocs;

    fake_db_load(lib, LIB_N);
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE, "precompute");
    CHECK(lib_search_begin() > 0 && strstr(last_begin, "reads=4")
          && strstr(last_begin, "partial=0"), "begin: %s", last_begin);

    v = search("h", -1, &capped);
    CHECK(v > 0 && !capped, "search h: %d", v);
    expect_rows(v, "h", h_labels, h_kinds, 9);
    CHECK(strstr(last_search, "kind_n=1,2,4,1,1 n=9")
          && strstr(last_search, "refine=0 cap=0"), "trace: %s",
          last_search);
    /* second lines by kind */
    CHECK(!strcmp(sub(v, 0), "Hank"), "song: %s", sub(v, 0));
    CHECK(!strcmp(sub(v, 2), "(none)"), "artist: %s", sub(v, 2));
    CHECK(!strcmp(sub(v, 3), "Hello Show"), "episode: %s", sub(v, 3));
    CHECK(!strcmp(sub(v, 4), "Hank"), "album: %s", sub(v, 4));
    CHECK(!strcmp(sub(v, 8), "Hemingway"), "book: %s", sub(v, 8));
    CHECK(SRCH_ROW_IDX((uint32_t)lib_id(v, 2)) == 0, "artist rep");

    /* refine in place: same handle, the set shrinks */
    v2 = search("ha", v, &capped);
    CHECK(v2 == v && strstr(last_search, "refine=1"), "refine: %d %s", v2,
          last_search);
    expect_rows(v, "ha", h_labels, ha_kinds, 6);
    v2 = search("hap", v, &capped);
    CHECK(v2 == v && lib_count(v) == 1 && !strcmp(label(v, 0),
          "Happy Hour Ep"), "hap: %d rows", lib_count(v));
    /* delete: from the sources again, the same view handle */
    v2 = search("ha", v, &capped);
    CHECK(v2 > 0 && strstr(last_search, "refine=0") && lib_count(v2) == 6,
          "delete: %s", last_search);
    v = v2;
    v2 = search("ha ", v, &capped);     /* NEXT: a space, same set */
    CHECK(v2 == v && lib_count(v) == 6 && strstr(last_search, "refine=1"),
          "space: %s", last_search);
    v2 = search("zzz", v, &capped);
    CHECK(v2 > 0 && lib_count(v2) == 0, "no results");
    v = search("", v2, &capped);
    CHECK(v > 0 && lib_count(v) == 0, "empty query");

    /* playing a song row: the result's songs from there */
    v = search("h", v, &capped);
    CHECK(lib_play(v, 1, false) == 0 && fake_pl_n == 4 && fake_pl[0] == 0
          && fake_pl[1] == 1 && fake_pl[2] == 3 && fake_pl[3] == 2
          && fake_pl_start == 1, "play song row: n=%d start=%d", fake_pl_n,
          fake_pl_start);
    CHECK(lib_play(v, 3, false) == 0 && fake_pl_n == 1 && fake_pl[0] == 7,
          "play episode row");
    CHECK(lib_play(v, 8, false) < 0, "a book row is opened, not played");
    {
        char path[64];
        CHECK(lib_path(v, 0, path, sizeof(path)) && !strncmp(path, "/e0", 3),
              "path of a song row");
        CHECK(!lib_path(v, 2, path, sizeof(path)), "no path for an artist");
    }
    /* the tracks of rows (On-The-Go adds) */
    i = lib_row_tracks(v, 4);           /* album Harbor */
    CHECK(i > 0 && lib_count(i) == 2, "album tracks: %d", lib_count(i));
    lib_close(i);
    i = lib_row_tracks(v, 2);           /* artist Hank */
    CHECK(i > 0 && lib_count(i) == 2, "artist tracks: %d", lib_count(i));
    lib_close(i);
    i = lib_row_tracks(v, 0);           /* song Haiku */
    CHECK(i > 0 && lib_count(i) == 1 && lib_id(i, 0) == 0, "song track");
    lib_close(i);

    /* no database: "Updating Library..." */
    fake_db_usable(false);
    CHECK(search("h", v, &capped) < 0, "search without a database");
    fake_db_usable(true);
    v = search("h", v, &capped);        /* a reload: the ROWS again */
    CHECK(v > 0 && lib_count(v) == 9 && strstr(last_begin, "reads=4"),
          "after reload: %d %s", lib_count(v), last_begin);

    /* nothing allocated by Search while audio plays */
    fake_play(0, 1000);
    allocs = fake_allocs;
    lib_search_end();
    CHECK(lib_search_begin() > 0, "begin while playing");
    v = search("h", -1, &capped);
    CHECK(v > 0 && lib_count(v) == 9 && fake_allocs == allocs,
          "playing: %d rows, %d allocs", lib_count(v), fake_allocs - allocs);
    fake_play(-1, 0);
    lib_search_end();
    CHECK(fake_blocks_used() <= 3, "leaked blocks: %d", fake_blocks_used());
}

/* -- the cap: a big library, the pool nearly full while playing --------- */
#define BIG 20000
static struct fake_entry big[BIG];
static char big_titles[BIG][16];

static bool cancel_now(void)
{
    return true;
}

static void test_cap(void)
{
    char prev[128] = "";
    bool capped;
    int v, held[8], nh = 0, i, n, allocs;

    for (i = 0; i < BIG; i++)
    {
        snprintf(big_titles[i], sizeof(big_titles[i]), "H%05d", i);
        big[i].title = big_titles[i];
        big[i].artist = "Hank";
        big[i].album = "Harbor";
        big[i].path = "/Music/x.mp3";
        big[i].length = 1000;
    }
    fake_db_load(big, BIG);
    CHECK(lib_precompute(false, NULL) == LIB_PRE_DONE, "precompute big");
    lib_search_begin();
    v = search("h", -1, &capped);
    CHECK(v > 0 && !capped && lib_count(v) == BIG + 2, "stopped: %d rows",
          lib_count(v));
    CHECK(strstr(last_search, "yields=") && !strstr(last_search,
          "yields=0 "), "no yields: %s", last_search);
    lib_search_end();

    /* playing, the pool (sized anew by a reload: the stopped search grew
     * it) mostly taken by open lists */
    fake_db_bump();
    held[0] = lib_open(LIB_ARTISTS, NULL);  /* sizes the pool */
    fake_play(0, 1000);
    allocs = fake_allocs;
    /* a filler view takes all but ~12-20 KB of the slack (no growth
     * while playing: the first size that fits) */
    for (i = 400000; i > 0; i -= 8192)
    {
        held[1] = lib_otg_view_new(0, i);
        if (held[1] > 0)
            break;
    }
    lib_close(held[1]);
    held[1] = lib_otg_view_new(0, i > 12288 ? i - 12288 : 0);
    nh = 2;
    lib_search_begin();
    v = search("h", -1, &capped);
    n = lib_count(v);
    CHECK(v > 0 && capped && n > 0 && n < BIG && fake_allocs == allocs,
          "capped: %d rows capped=%d allocs %d", n, capped,
          fake_allocs - allocs);
    CHECK(strstr(last_search, "cap=") && !strstr(last_search, "cap=0"),
          "cap trace: %s", last_search);
    for (i = 0; i < n; i++)             /* the alphabetical head */
    {
        const char *l = label(v, i);
        CHECK(strcmp(prev, l) <= 0 || i < 2, "order at %d: %s %s", i, prev,
              l);
        snprintf(prev, sizeof(prev), "%s", l);
    }
    /* a capped view is rebuilt, never refined (refining keeps the cap) */
    v = search("h0", v, &capped);
    CHECK(strstr(last_search, "refine=0"), "refined a capped view: %s",
          last_search);
    /* a new key aborts */
    {
        struct srch_query q;
        srch_fold_query("h1", &q);
        CHECK(lib_search(&q, -1, &capped, cancel_now) < 0
              && strstr(last_search, "aborted"), "cancel: %s", last_search);
    }
    while (nh--)
        lib_close(held[nh]);
    fake_play(-1, 0);
    lib_search_end();
}

static void test_podcast_dot(void)
{
    struct lib_filter f = { LIB_ANY, LIB_ANY, LIB_ANY, LIB_ANY };
    int shows, eps;

    fake_db_load(lib, LIB_N);
    shows = lib_open(LIB_PODCASTS, NULL);
    CHECK(lib_count(shows) == 1 && lib_unplayed(shows, 0), "show dot");
    f.album = lib_id(shows, 0);
    eps = lib_open(LIB_PODCASTS, &f);
    CHECK(lib_count(eps) == 1 && lib_unplayed(eps, 0), "episode dot");
    fake_play(7, 0);                    /* just loaded, still at 0 ms */
    CHECK(!lib_unplayed(eps, 0), "a loaded episode keeps its dot");
    lib_progress_refresh(shows);
    CHECK(!lib_unplayed(shows, 0), "the show's dot after refresh");
    fake_play(-1, 0);
    lib_close(eps);
    lib_close(shows);
}

int main(int argc, char **argv)
{
    fake_db_dir(argc > 1 ? argv[1] : ".");
    test_stats_first();
    test_search();
    test_cap();
    test_podcast_dot();
    printf("   %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
