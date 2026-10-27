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

/* Search at scale (docs/phase11-brief.md section 6 item 3), for
 * tests/perf/search50k.sh: apps/ipodui/library.c's lib_search() over the
 * fake tagcache of tests/ipodui with a synthetic library of 50,000
 * tracks (5,000 albums, 3,000 artists, 300 books of 3 files, 100 shows
 * of 4 episodes; titles of 2-4 common words, ~20 B), host CPU time per
 * keystroke: the first character, a refine, a delete, and the first
 * character while playing with the pool's spare only (the cap). The
 * device estimate is the ARM instruction count (tests/search/insn.sh),
 * not this host time. GPL v2+ */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "tagcache.h"
#include "ipodui/library.h"
#include "ipodui/search.h"
#include "fake_tagcache.h"

#define N 50000
static struct fake_entry e[N];
static char titles[N][48], albums[5000][24], artists[3000][24];
static char last[256];

void test_trace(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    if (!strncmp(fmt, "search q", 8))
        vsnprintf(last, sizeof(last), fmt, ap);
    va_end(ap);
}

int otg_play(int pos)
{
    (void)pos;
    return -1;
}

static const char *const words[] = {
    "love", "heart", "night", "day", "time", "home", "light", "dream",
    "fire", "rain", "blue", "road", "song", "world", "life", "girl", "baby",
    "man", "moon", "sun", "star", "river", "summer", "winter", "dance",
    "city", "soul", "gold", "wild", "free", "dark", "sweet", "lonely",
    "angel", "ghost", "happy", "hello", "hold", "heaven", "highway",
    "horizon", "island", "kiss", "last", "little", "magic", "memory",
    "midnight", "morning", "ocean", "paradise", "rock", "roll", "secret",
    "shadow", "silver", "sky", "stone", "storm", "street", "tears",
    "thunder", "tonight", "under", "wave", "wind", "young", "zero", "echo",
    "after", "again", "always", "away", "back", "better", "broken",
};
#define NW (int)(sizeof(words) / sizeof(words[0]))

static unsigned rnd(void)
{
    static unsigned s = 12345;
    s = s * 1103515245u + 12345u;
    return s >> 8;
}

static void cap(char *s)
{
    if (*s >= 'a' && *s <= 'z')
        *s -= 32;
}

static double run(const char *text, int view, int *out_view, bool *capped)
{
    struct srch_query q;
    clock_t t0;
    int v;

    srch_fold_query(text, &q);
    t0 = clock();
    v = lib_search(&q, view, capped, NULL);
    if (out_view)
        *out_view = v;
    return (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC;
}

int main(int argc, char **argv)
{
    long bytes = 0;
    bool capped;
    int i, v, held;
    double t;

    fake_db_dir(argc > 1 ? argv[1] : ".");
    for (i = 0; i < 5000; i++)
    {
        snprintf(albums[i], sizeof(albums[i]), "%s %s", words[rnd() % NW],
                 words[rnd() % NW]);
        cap(albums[i]);
    }
    for (i = 0; i < 3000; i++)
    {
        snprintf(artists[i], sizeof(artists[i]), "%s %s %d",
                 words[rnd() % NW], words[rnd() % NW], i);
        cap(artists[i]);
    }
    for (i = 0; i < N; i++)
    {
        int nw = 2 + rnd() % 3, k, a = i / 10;
        char *p = titles[i];

        for (k = 0; k < nw; k++)
            p += sprintf(p, "%s%s", k ? " " : "", words[rnd() % NW]);
        cap(titles[i]);
        bytes += strlen(titles[i]) + 1;
        e[i].title = titles[i];
        e[i].album = albums[a % 5000];
        e[i].albumartist = e[i].artist = artists[a % 3000];
        e[i].path = "/Music/x.flac";
        e[i].genre = "Rock";
        e[i].track = i % 10 + 1;
        e[i].length = 200000;
        if (i >= N - 900)               /* 300 books of 3 files */
            e[i].genre = "Audiobook";
        else if (i >= N - 1300)         /* 100 shows of 4 episodes */
            e[i].genre = "Podcast";
    }
    fake_db_load(e, N);
    if (lib_precompute(false, NULL) != LIB_PRE_DONE)
        return 1;
    printf("library: %d tracks, %ld B of titles (%.1f B each)\n", N, bytes,
           (double)bytes / N);
    lib_search_begin();
    t = run("h", -1, &v, &capped);
    printf("first char 'h': %8.0f us  %s\n", t, last);
    t = run("he", v, &v, &capped);
    printf("refine 'he':    %8.0f us  %s\n", t, last);
    t = run("hea", v, &v, &capped);
    printf("refine 'hea':   %8.0f us  %s\n", t, last);
    t = run("he", v, &v, &capped);
    printf("delete -> 'he': %8.0f us  %s\n", t, last);
    t = run("q", v, &v, &capped);
    printf("rare 'q':       %8.0f us  %s\n", t, last);
    t = run("s", v, &v, &capped);
    printf("first char 's': %8.0f us  %s\n", t, last);
    lib_search_end();

    /* playing: the pool's spare (64 KB) less a Songs view, no growth */
    fake_db_bump();
    held = lib_open(LIB_SONGS, NULL);
    fake_play(0, 1000);
    lib_search_begin();
    t = run("h", -1, &v, &capped);
    printf("playing 'h':    %8.0f us  capped=%d %s\n", t, capped, last);
    t = run("hea", v, &v, &capped);
    printf("playing 'hea':  %8.0f us  capped=%d %s\n", t, capped, last);
    lib_search_end();
    lib_close(held);
    fake_play(-1, 0);
    return 0;
}
