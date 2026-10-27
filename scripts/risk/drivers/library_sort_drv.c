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

/* Risk tool 1 driver (scripts/risk/arm_insn_count.sh): host-side timing
 * and comparator-call-count measurement for apps/ipodui/library.c's
 * lib_open() sort, at the realistic library scale (docs/hardware.md
 * Planning scale), driven the same way tests/ipodui/test_library.c
 * drives it: fake_db_load() over tests/ipodui/fake_tagcache.c, then the
 * real, unmodified apps/ipodui/library.c.
 *
 * library.c's cmp_rec() compares a precomputed 8-byte sort-key prefix
 * first (library.c:466-482) and only calls the expensive full-string
 * tie-break (cmp_tie/cmp_labels, library.c:366-463) on a prefix
 * collision -- rare at this scale. To measure the real split instead of
 * assuming one, this driver supplies its OWN qsort() (a plain quicksort
 * with an insertion-sort tail), linked ahead of the host libc's, so the
 * *exact* comparator (library.c's static cmp_rec, passed in as a
 * function pointer -- never named directly, so apps/ipodui/library.c
 * stays untouched) is counted on every call via g_cmp_calls. This is
 * link-time interposition, standard on both macOS (local .o symbols
 * satisfy the reference before -lSystem's qsort is even consulted) and
 * Linux/glibc; no --wrap or dynamic tricks needed. The replacement sort
 * is a different algorithm from whatever libc/the device's commit code
 * uses, so its *absolute* comparator count is an estimate of the same
 * O(N log N) order, not a bit-exact match -- stated in
 * docs/risk-tooling.md.
 *
 * Usage: library_sort_drv <n_tracks> <n_albums>
 * Prints one CSV line to stdout per lib_open() call:
 *   libsort,<cat>,<n>,<cmp_calls>,<elapsed_us>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fake_tagcache.h"
#include "ipodui/library.h"

/* -- counting qsort, interposed ahead of libc's -------------------------*/

unsigned long g_cmp_calls;

typedef int (*cmpfn_t)(const void *, const void *);

static void swap_bytes(char *a, char *b, size_t size)
{
    char tmp[128];
    size_t off = 0;
    while (off < size)
    {
        size_t chunk = size - off < sizeof tmp ? size - off : sizeof tmp;
        memcpy(tmp, a + off, chunk);
        memcpy(a + off, b + off, chunk);
        memcpy(b + off, tmp, chunk);
        off += chunk;
    }
}

static void qsort_range(char *base, size_t lo, size_t hi, size_t size,
                        cmpfn_t cmp)
{
    while (hi - lo > 12)
    {
        size_t mid = lo + (hi - lo) / 2;
        char *plo = base + lo * size, *pmid = base + mid * size,
             *phi = base + (hi - 1) * size;
        size_t i, j;
        char *pivot;

        g_cmp_calls++;
        if (cmp(plo, pmid) > 0) swap_bytes(plo, pmid, size);
        g_cmp_calls++;
        if (cmp(pmid, phi) > 0)
        {
            swap_bytes(pmid, phi, size);
            g_cmp_calls++;
            if (cmp(plo, pmid) > 0) swap_bytes(plo, pmid, size);
        }
        swap_bytes(pmid, plo, size);
        pivot = base + lo * size;
        i = lo + 1; j = hi - 1;
        for (;;)
        {
            while (i <= j) {
                g_cmp_calls++;
                if (cmp(base + i * size, pivot) >= 0) break;
                i++;
            }
            while (i <= j) {
                g_cmp_calls++;
                if (cmp(pivot, base + j * size) >= 0) break;
                if (j == lo) break;
                j--;
            }
            if (i >= j) break;
            swap_bytes(base + i * size, base + j * size, size);
            i++;
            if (j != lo) j--;
        }
        swap_bytes(pivot, base + j * size, size);
        if (j - lo < hi - (j + 1))
        {
            qsort_range(base, lo, j, size, cmp);
            lo = j + 1;
        }
        else
        {
            qsort_range(base, j + 1, hi, size, cmp);
            hi = j;
        }
    }
    {
        size_t i;
        for (i = lo + 1; i < hi; i++)
        {
            size_t k = i;
            while (k > lo)
            {
                g_cmp_calls++;
                if (cmp(base + (k - 1) * size, base + k * size) <= 0)
                    break;
                swap_bytes(base + (k - 1) * size, base + k * size, size);
                k--;
            }
        }
    }
}

void qsort(void *base, size_t nmemb, size_t size, cmpfn_t cmp)
{
    if (nmemb > 1)
        qsort_range((char *)base, 0, nmemb, size, cmp);
}

/* -- name generation (same shape as sortkey_drv.c / synthetic_library.py) */

static const char *WORDS[] = {
    "amber","anchor","apple","arrow","aspen","autumn","basalt","beacon",
    "birch","bloom","bright","cedar","cinder","clover","cobalt","comet",
    "copper","coral","crimson","crystal","dawn","delta","drift","dune",
    "echo","ember","emerald","falcon","faded","feather","ferry","fjord",
    "forest","frost","garnet","ghost","glacier","golden","granite",
    "harbour","hazel","hollow","horizon","indigo","iron","island","ivory",
    "jade","jagged","juniper","kestrel","lantern","lunar","maple","marble",
    "meadow","mellow","mirror","misty","nectar","noble","north","oasis",
};
#define NWORDS ((int)(sizeof(WORDS) / sizeof(WORDS[0])))
#define NGENRES 8
static const char *GENRES[NGENRES] = {
    "Rock","Pop","Electronic","Jazz","Folk","Indie","Classical","Metal",
};

static unsigned rngstate = 0x5EED5EEDu;
static unsigned rnd(void)
{
    unsigned x = rngstate;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rngstate = x;
}

static char *gen_name(int words)
{
    char *buf = malloc(64);
    int len = 0, w;
    for (w = 0; w < words; w++)
    {
        const char *word = WORDS[rnd() % NWORDS];
        int n = (int)strlen(word);
        if (w)
            buf[len++] = ' ';
        buf[len] = (char)(word[0] - 'a' + 'A');
        memcpy(buf + len + 1, word + 1, (size_t)(n - 1));
        len += n;
    }
    buf[len] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    int ntracks = argc > 1 ? atoi(argv[1]) : 40000;
    int nalbums = argc > 2 ? atoi(argv[2]) : 4000;
    int tracks_per_album = ntracks / nalbums;
    int nartists = nalbums / 4 > 0 ? nalbums / 4 : 1;
    struct fake_entry *e = calloc((size_t)ntracks, sizeof *e);
    char **artists = calloc((size_t)nartists, sizeof *artists);
    int i, a, t, n = 0;
    struct timespec t0, t1;
    static const struct lib_filter any = { LIB_ANY, LIB_ANY, LIB_ANY, LIB_ANY };

    for (a = 0; a < nartists; a++)
        artists[a] = gen_name(1 + (int)(rnd() % 2));

    for (a = 0; a < nalbums && n < ntracks; a++)
    {
        char *album = gen_name(1 + (int)(rnd() % 3));
        const char *artist = artists[rnd() % (unsigned)nartists];
        const char *genre = GENRES[rnd() % NGENRES];
        for (t = 0; t < tracks_per_album && n < ntracks; t++, n++)
        {
            e[n].title = gen_name(1 + (int)(rnd() % 3));
            e[n].artist = artist;
            e[n].album = album;
            e[n].albumartist = artist;
            e[n].genre = genre;
            e[n].composer = NULL;
            e[n].path = "/Synth/x.mp3";
            e[n].disc = 1;
            e[n].track = t + 1;
            e[n].length = 200000;
        }
    }
    ntracks = n;
    fake_db_load(e, ntracks);

    for (i = 0; i < argc; i++) {}   /* argc already consumed above */

    {
        static const struct { enum lib_cat cat; const char *name; } views[] = {
            { LIB_ARTISTS, "artists" }, { LIB_ALBUMS, "albums" },
            { LIB_SONGS, "songs" }, { LIB_GENRES, "genres" },
        };
        size_t v;
        for (v = 0; v < sizeof views / sizeof views[0]; v++)
        {
            int view;
            double us;

            g_cmp_calls = 0;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            view = lib_open(views[v].cat, &any);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            us = (double)(t1.tv_sec - t0.tv_sec) * 1e6
               + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3;
            printf("libsort,%s,%d,%lu,%.1f\n", views[v].name,
                   lib_count(view), g_cmp_calls, us);
        }
    }
    return 0;
}
