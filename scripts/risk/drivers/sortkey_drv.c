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

/* Risk tool 1 driver (scripts/risk/arm_insn_count.sh): host-side timing and
 * shape measurement for apps/ipodui/sortkey.c's ui_sortkey(), at the
 * "realistic library" scale from docs/hardware.md Planning scale
 * (30-40k tracks, 3-4k albums). Compiled with the HOST compiler (not ARM)
 * against the real apps/ipodui/sortkey.c -- this never stands in for it.
 *
 * Generates deterministic track/album/artist-shaped names (same word-list
 * shape as tests/perf/synthetic_library.py's name(), reimplemented here in
 * C so this driver has no Python dependency) and runs the real
 * ui_sortkey() over N of them, timing it and reporting:
 *   - n, total input bytes, average input length (the per-call loop trip
 *     count that scripts/risk/arm_insn_count.sh multiplies by the
 *     hand-derived per-character ARM instruction cost -- see
 *     docs/risk-tooling.md "ARM instruction counts" for that derivation),
 *   - the fraction of keys that start with HASH_GROUP ('{', 0x7b): names
 *     that don't start with a letter (numbers, symbols), which take
 *     sortkey.c's extra memmove+prefix-insert path (rare; measured here
 *     instead of assumed),
 *   - elapsed host time (the sim-host baseline for the "corrected factor"
 *     table: device_estimate_ms / this_ms).
 *
 * Usage: sortkey_drv <n> <role>
 *   role: "title" (track titles, the default shape), "album", "artist"
 *   (same generator, different word counts: albums/artists use fewer,
 *   longer-looking phrases, matching synthetic_library.py's name(rng, k)
 *   call sites). Prints one CSV line to stdout:
 *     sortkey,<role>,<n>,<total_in>,<avg_in>,<avg_out>,<hashgroup_pct>,<elapsed_us>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ipodui/sortkey.h"

/* Word lengths/shape match tests/perf/synthetic_library.py's WORDS list
 * (short English words, 3-8 chars): what matters for this driver is the
 * length distribution, not the exact words. */
static const char *WORDS[] = {
    "amber","anchor","apple","arrow","aspen","autumn","basalt","beacon",
    "birch","bloom","bright","cedar","cinder","clover","cobalt","comet",
    "copper","coral","crimson","crystal","dawn","delta","drift","dune",
    "echo","ember","emerald","falcon","faded","feather","ferry","fjord",
    "forest","frost","garnet","ghost","glacier","golden","granite",
    "harbour","hazel","hollow","horizon","indigo","iron","island","ivory",
    "jade","jagged","juniper","kestrel","lantern","lunar","maple","marble",
    "meadow","mellow","mirror","misty","nectar","noble","north","oasis",
    "ochre","olive","onyx","opal","orbit","pale","pebble","pine","quartz",
    "quiet","raven","river","rust","saffron","sable","signal","silver",
    "slate","solar","spruce","summit","tide","timber","topaz","upper",
    "velvet","violet","willow","winter","xenon","yarrow","yellow","zephyr",
    "zero","1999","42","7th",
};
#define NWORDS ((int)(sizeof(WORDS) / sizeof(WORDS[0])))

static unsigned rngstate = 0xC0FFEE4u;
static unsigned rnd(void)
{
    unsigned x = rngstate;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rngstate = x;
}

/* One name of 'words' words, each capitalized, space-separated -- the
 * shape synthetic_library.py's name(rng, n) produces. */
static int gen_name(char *buf, int bufsize, int words)
{
    int len = 0, w;

    for (w = 0; w < words; w++)
    {
        const char *word = WORDS[rnd() % NWORDS];
        int n = (int)strlen(word);
        if (w)
        {
            if (len + 1 >= bufsize - 1)
                break;
            buf[len++] = ' ';
        }
        if (len + n >= bufsize - 1)
            n = bufsize - 1 - len;
        if (n <= 0)
            break;
        buf[len] = (char)(word[0] >= 'a' && word[0] <= 'z'
                           ? word[0] - 'a' + 'A' : word[0]);
        if (n > 1)
            memcpy(buf + len + 1, word + 1, (size_t)(n - 1));
        len += n;
    }
    buf[len] = '\0';
    return len;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 40000;
    const char *role = argc > 2 ? argv[2] : "title";
    int words_lo = 1, words_hi = 3;      /* titles: 1-3 words */
    char name[256], key[32];
    long total_in = 0, total_out = 0, hashgroup = 0;
    struct timespec t0, t1;
    int i;

    if (!strcmp(role, "album"))
        words_lo = 1, words_hi = 3;      /* same shape as synthetic_library.py */
    else if (!strcmp(role, "artist"))
        words_lo = 1, words_hi = 3;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < n; i++)
    {
        int words = words_lo + (int)(rnd() % (unsigned)(words_hi - words_lo + 1));
        int inlen = gen_name(name, sizeof name, words);
        int outlen = ui_sortkey(name, key, sizeof key);

        total_in += inlen;
        total_out += outlen;
        if (outlen > 0 && key[0] == '{')      /* HASH_GROUP, sortkey.c */
            hashgroup++;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    {
        double us = (double)(t1.tv_sec - t0.tv_sec) * 1e6
                  + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3;
        printf("sortkey,%s,%d,%ld,%.3f,%.3f,%.4f,%.1f\n",
               role, n, total_in,
               (double)total_in / n, (double)total_out / n,
               100.0 * (double)hashgroup / n, us);
    }
    return 0;
}
