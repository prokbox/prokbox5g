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

/* Host test: Now Playing for a multi-file book (Phase 6 §3.4, owner Rev 2)
 *  - the fast scrub's snap set is the union of the book's file starts
 *    (ch) and the playing file's chapters (sub at sub_base), and an empty
 *    sub leaves the Phase 5 behaviour unchanged;
 *  - the map's tick bitmap, rescaled to the drawn bar (D11), sits within
 *    1 px of a direct computation and never on the bar's ends;
 *  - the counter's entry for (file, its chapter) as nowplaying.c's
 *    book_entry() asks for it (chapter -1 = a file without chapters).
 * Pure C: apps/ipodui/np_scrub.c + ab_book_core.c. */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "np_scrub.h"
#include "ab_book.h"
#include "check.h"

#define V_FAST NP_SCRUB_SNAP_V
#define V_SLOW 1

static struct ab_book_map map;  /* 10 KB: not on the stack */

static void test_union_snap(void)
{
    /* four 10-minute files; file 1 has chapters at 0, 100 s, 300 s */
    static const uint32_t files[] = { 0, 600000, 1200000, 1800000 };
    static const uint32_t local[] = { 0, 100000, 300000 };
    struct np_scrub_ctx c;
    unsigned long p;

    memset(&c, 0, sizeof(c));
    c.length_ms = 2400000;
    c.snap = true;
    c.ch.start = files;
    c.ch.stride = sizeof(uint32_t);
    c.ch.count = 4;
    c.sub.start = local;
    c.sub.stride = sizeof(uint32_t);
    c.sub.count = 3;
    c.sub_base = 600000;

    p = np_scrub_wheel(&c, 650000, 1, V_FAST);
    CHECK_EQ(p, 700000);                        /* the file's chapter 2 */
    p = np_scrub_wheel(&c, p, 1, V_FAST);
    CHECK_EQ(p, 900000);
    p = np_scrub_wheel(&c, p, 1, V_FAST);
    CHECK_EQ(p, 1200000);                       /* the next file */
    p = np_scrub_wheel(&c, p, 2, V_FAST);
    CHECK_EQ(p, 2400000);                       /* file 3, then the end */
    p = np_scrub_wheel(&c, p, 1, V_FAST);
    CHECK_EQ(p, 2400000);

    p = np_scrub_wheel(&c, 1250000, -1, V_FAST);
    CHECK_EQ(p, 1200000);
    p = np_scrub_wheel(&c, p, -3, V_FAST);
    CHECK_EQ(p, 600000);                        /* 900, 700, 600 s */
    p = np_scrub_wheel(&c, p, -2, V_FAST);
    CHECK_EQ(p, 0);

    /* slow: 1 s per click, across a file boundary, no snapping */
    p = np_scrub_wheel(&c, 1199500, 1, V_SLOW);
    CHECK_EQ(p, 1200500);

    /* no file map (a refill): the file's chapters alone still snap */
    c.ch.count = 0;
    p = np_scrub_wheel(&c, 650000, 1, V_FAST);
    CHECK_EQ(p, 700000);
    p = np_scrub_wheel(&c, 950000, 1, V_FAST);
    CHECK_EQ(p, 2400000);                       /* none after: the end */
}

static void test_p5_unchanged(void)
{
    static const uint32_t ch[] = { 0, 60000, 120000 };
    struct np_scrub_ctx a, b;
    unsigned long pa, pb;
    int i;

    memset(&a, 0, sizeof(a));
    a.length_ms = 180000;
    a.snap = true;
    a.ch.start = ch;
    a.ch.stride = sizeof(uint32_t);
    a.ch.count = 3;
    b = a;
    b.sub.stride = sizeof(uint32_t);        /* sub empty but pointed */
    b.sub_base = 5000;
    pa = pb = 30000;
    for (i = 0; i < 6; i++)
    {
        int clicks = i < 3 ? 1 : -1;
        pa = np_scrub_wheel(&a, pa, clicks, V_FAST);
        pb = np_scrub_wheel(&b, pb, clicks, V_FAST);
        CHECK_EQ(pa, pb);
    }
    CHECK_EQ(np_scrub_wheel(&a, 30000, 1, V_FAST), 60000);
    CHECK_EQ(np_scrub_wheel(&a, 60000, -1, V_FAST), 0);
    /* snap off (music): never snaps even with a sub */
    b.snap = false;
    b.sub.start = ch;
    b.sub.count = 3;
    CHECK_EQ(np_scrub_wheel(&b, 30000, 1, V_FAST),
             30000 + np_scrub_step_ms(V_FAST));
}

static void test_ticks_scale(void)
{
    static uint32_t starts[700];
    uint8_t bits[AB_TICK_BYTES], scaled[AB_TICK_BYTES], direct[AB_TICK_BYTES];
    struct np_chapters ch = { starts, sizeof(uint32_t), 0 };
    uint32_t len = 42475000;                    /* 11:47:55 */
    int w, i, n;

    srand(7);
    for (i = 0; i < 700; i++)
        starts[i] = (uint32_t)((unsigned long long)len * i / 700
                               + (unsigned)(rand() % 20000));
    ch.count = 700;
    ab_book_ticks(starts, sizeof(uint32_t), 700, len, bits);
    for (w = 120; w <= AB_TICK_RES; w += 12)
    {
        n = ab_ticks_scale(bits, w, scaled);
        np_chapter_ticks(&ch, len, w, direct);
        CHECK(n > 0);
        CHECK(!(scaled[0] & 1));                /* never on the left end */
        for (i = 0; i < w; i++)
        {
            bool s = scaled[i >> 3] & (1 << (i & 7));
            bool d0 = direct[i >> 3] & (1 << (i & 7));
            bool dl = i > 0 && (direct[(i - 1) >> 3] & (1 << ((i - 1) & 7)));
            bool dr = i + 1 < w
                      && (direct[(i + 1) >> 3] & (1 << ((i + 1) & 7)));
            if (s)
                CHECK(d0 || dl || dr);          /* within 1 px (D11) */
        }
    }
}

static void test_counter_entry(void)
{
    /* 3 files: file 0 no chapters (entry 0), file 1 four chapters
     * (entries 1-4), file 2 no chapters (entry 5) */
    memset(&map, 0, sizeof(map));
    map.file_count = 3;
    map.entries = 6;
    map.file_first[0] = 0;
    map.file_first[1] = 1;
    map.file_first[2] = 5;
    CHECK_EQ(ab_book_entry_at(&map, 0, 0), 0);
    CHECK_EQ(ab_book_entry_at(&map, 1, 0), 1);
    CHECK_EQ(ab_book_entry_at(&map, 1, 3), 4);
    CHECK_EQ(ab_book_entry_at(&map, 1, 9), 4);  /* clamped to the file */
    CHECK_EQ(ab_book_entry_at(&map, 2, 0), 5);
    CHECK_EQ(ab_book_entry_at(&map, 3, 0), -1); /* snapshot's entry then */
}

int main(void)
{
    test_union_snap();
    test_p5_unchanged();
    test_ticks_scale();
    test_counter_entry();
    CHECK_DONE("test_book");
}
