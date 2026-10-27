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

/* Host test: apps/ipodui/np_scrub.c (Now Playing scrub maths). */
#include <stdlib.h>
#include <string.h>
#include "np_scrub.h"
#include "check.h"

/* a strided uint32_t column with start_ms not at offset 0 (the packed
 * table's entries have it at 0; this checks the stride arithmetic) */
struct fake_chapter {
    char title[4];
    uint32_t start_ms;
};

static struct np_chapters arr(const uint32_t *a, int n)
{
    struct np_chapters c = { a, sizeof(uint32_t), n };
    return c;
}

static int bits_count(const uint8_t *b, int w)
{
    int x, n = 0;
    for (x = 0; x < w; x++)
        n += (b[x >> 3] >> (x & 7)) & 1;
    return n;
}

static int bit(const uint8_t *b, int x)
{
    return (b[x >> 3] >> (x & 7)) & 1;
}

static void test_step(void)
{
    CHECK_EQ(np_scrub_step_ms(0), 1000);
    CHECK_EQ(np_scrub_step_ms(11), 1000);
    CHECK_EQ(np_scrub_step_ms(12), 2000);
    CHECK_EQ(np_scrub_step_ms(20), 4000);
    CHECK_EQ(np_scrub_step_ms(1000), 1000UL << NP_SCRUB_ACCEL_MAX);
    /* monotonic */
    {
        unsigned v;
        for (v = 1; v < 200; v++)
            CHECK(np_scrub_step_ms(v) >= np_scrub_step_ms(v - 1));
    }
}

static void test_music(void)
{
    struct np_scrub_ctx c = { .length_ms = 240000, .ch = { NULL, sizeof(uint32_t), 0 },
                              .snap = false };
    CHECK_EQ(np_scrub_wheel(&c, 10000, 3, 0), 13000);
    CHECK_EQ(np_scrub_wheel(&c, 10000, -3, 0), 7000);
    CHECK_EQ(np_scrub_wheel(&c, 1000, -5, 0), 0);          /* clamp low */
    CHECK_EQ(np_scrub_wheel(&c, 239500, 2, 0), 240000);    /* clamp high */
    CHECK_EQ(np_scrub_wheel(&c, 500000, 0, 0), 240000);    /* past end */
    CHECK_EQ(np_scrub_wheel(&c, 500000, -1, 0), 239000);
    CHECK_EQ(np_scrub_wheel(&c, 0, 1, 40), 1000UL << 4);   /* accel */
    /* fast wheel on music never snaps (no chapters) */
    CHECK_EQ(np_scrub_wheel(&c, 0, 1, 100), 64000);
    /* huge click counts saturate */
    CHECK_EQ(np_scrub_wheel(&c, 0, 0x7fffffff, 100), 240000);
    CHECK_EQ(np_scrub_wheel(&c, 240000, -0x7fffffff, 100), 0);
    /* unknown length: nothing moves */
    c.length_ms = 0;
    CHECK_EQ(np_scrub_wheel(&c, 0, 5, 0), 0);
}

static void test_book_snap(void)
{
    static const uint32_t st[] = { 0, 60000, 120000, 300000 };
    struct np_scrub_ctx c = { .length_ms = 400000, .ch = { st, sizeof(st[0]), 4 }, .snap = true };
    /* slow: continuous */
    CHECK_EQ(np_scrub_wheel(&c, 50000, 2, 5), 52000);
    /* fast forward: next start, one per click */
    CHECK_EQ(np_scrub_wheel(&c, 50000, 1, NP_SCRUB_SNAP_V), 60000);
    CHECK_EQ(np_scrub_wheel(&c, 50000, 2, NP_SCRUB_SNAP_V), 120000);
    CHECK_EQ(np_scrub_wheel(&c, 60000, 1, NP_SCRUB_SNAP_V), 120000);
    /* past the last start: to the end, then stays */
    CHECK_EQ(np_scrub_wheel(&c, 300000, 1, 50), 400000);
    CHECK_EQ(np_scrub_wheel(&c, 400000, 3, 50), 400000);
    CHECK_EQ(np_scrub_wheel(&c, 100000, 9, 50), 400000);
    /* fast back: start of the current chapter, then the previous */
    CHECK_EQ(np_scrub_wheel(&c, 130000, -1, 50), 120000);
    CHECK_EQ(np_scrub_wheel(&c, 120000, -1, 50), 60000);
    CHECK_EQ(np_scrub_wheel(&c, 130000, -2, 50), 60000);
    CHECK_EQ(np_scrub_wheel(&c, 130000, -9, 50), 0);
    CHECK_EQ(np_scrub_wheel(&c, 999999, -1, 50), 300000); /* past end */
    /* snap off (music with chapters, e.g. an m4a with chapter marks) */
    c.snap = false;
    CHECK_EQ(np_scrub_wheel(&c, 50000, 1, 50), 50000 + 32000);
}

static void test_chapter_at(void)
{
    static const uint32_t st[] = { 0, 60000, 120000, 300000 };
    struct np_chapters c = arr(st, 4);
    int ai;
    CHECK_EQ(np_chapter_at(&c, 0, &ai), 0);
    CHECK_EQ(ai, 0);
    CHECK_EQ(np_chapter_at(&c, 59999, NULL), 0);
    CHECK_EQ(np_chapter_at(&c, 60000, NULL), 1);
    CHECK_EQ(np_chapter_at(&c, 299999, NULL), 2);
    CHECK_EQ(np_chapter_at(&c, 10000000, NULL), 3);       /* past end */

    /* 0 chapters */
    c = arr(st, 0);
    CHECK_EQ(np_chapter_at(&c, 1000, &ai), -1);
    CHECK_EQ(ai, -1);

    /* 1 chapter, starting late (pre-roll): before it counts as 1 of 1 */
    {
        static const uint32_t one[] = { 5000 };
        c = arr(one, 1);
        CHECK_EQ(np_chapter_at(&c, 0, &ai), 0);
        CHECK_EQ(ai, 0);
        CHECK_EQ(np_chapter_at(&c, 9000, NULL), 0);
    }

    /* out of order: rank in time order, array index separately */
    {
        static const uint32_t ooo[] = { 120000, 0, 300000, 60000 };
        c = arr(ooo, 4);
        CHECK_EQ(np_chapter_at(&c, 70000, &ai), 1);
        CHECK_EQ(ai, 3);
        CHECK_EQ(np_chapter_at(&c, 10, &ai), 0);
        CHECK_EQ(ai, 1);
        CHECK_EQ(np_chapter_at(&c, 400000, &ai), 3);
        CHECK_EQ(ai, 2);
    }

    /* duplicate starts: first in array order wins, ranks stay distinct */
    {
        static const uint32_t dup[] = { 0, 1000, 1000, 2000 };
        c = arr(dup, 4);
        CHECK_EQ(np_chapter_at(&c, 1500, &ai), 1);
        CHECK_EQ(ai, 1);
    }

    /* strided (struct audiobook_chapters layout) */
    {
        static struct fake_chapter fc[3];
        struct np_chapters s;
        fc[0].start_ms = 0;
        fc[1].start_ms = 1000;
        fc[2].start_ms = 5000;
        s.start = &fc[0].start_ms;
        s.stride = sizeof(fc[0]);
        s.count = 3;
        CHECK_EQ(np_chapter_at(&s, 4999, NULL), 1);
        CHECK_EQ(np_chapter_at(&s, 5000, NULL), 2);
    }
}

static void test_bar_and_ticks(void)
{
    uint8_t bits[(320 + 7) / 8];
    CHECK_EQ(np_bar_x(0, 1000, 204), 0);
    CHECK_EQ(np_bar_x(500, 1000, 204), 102);
    CHECK_EQ(np_bar_x(1000, 1000, 204), 204);
    CHECK_EQ(np_bar_x(5000, 1000, 204), 204);    /* past the end */
    CHECK_EQ(np_bar_x(10, 0, 204), 0);           /* unknown length */
    /* 11:47:55 book: 42,475,000 ms, x 320 overflows 32 bits */
    CHECK_EQ(np_bar_x(42475000UL / 2, 42475000UL, 320), 160);
    CHECK_EQ(np_bar_x(42474999UL, 42475000UL, 320), 319);

    /* 0 chapters */
    {
        struct np_chapters c = arr(NULL, 0);
        memset(bits, 0xff, sizeof(bits));
        CHECK_EQ(np_chapter_ticks(&c, 1000, 204, bits), 0);
        CHECK_EQ(bits_count(bits, 204), 0);
    }
    /* 1 chapter at 0: no tick (it is the bar's start) */
    {
        static const uint32_t one[] = { 0 };
        struct np_chapters c = arr(one, 1);
        CHECK_EQ(np_chapter_ticks(&c, 1000, 204, bits), 0);
    }
    /* starts at 0, inside, at and past the end, out of order */
    {
        static const uint32_t st[] = { 500, 0, 1000, 99999, 250 };
        struct np_chapters c = arr(st, 5);
        CHECK_EQ(np_chapter_ticks(&c, 1000, 204, bits), 2);
        CHECK(bit(bits, 102));
        CHECK(bit(bits, 51));
        CHECK_EQ(bits_count(bits, 204), 2);
    }
    /* 648 chapters over 11:47:55 on a 204 px bar: ticks merge, never more
     * than w - 1 pixels, all strictly inside */
    {
        uint32_t *st = malloc(648 * sizeof(*st));
        struct np_chapters c;
        int i, n, ai;
        unsigned long len = 42475000UL;
        for (i = 0; i < 648; i++)
            st[i] = (uint32_t)((unsigned long long)len * i / 648);
        c = arr(st, 648);
        n = np_chapter_ticks(&c, len, 204, bits);
        CHECK(n > 150 && n <= 203);
        CHECK_EQ(bits_count(bits, 204), n);
        CHECK(!bit(bits, 0));
        /* "73 of 648" */
        CHECK_EQ(np_chapter_at(&c, st[72] + 1, &ai) + 1, 73);
        CHECK_EQ(ai, 72);
        /* fast scrub crosses the book in 647 clicks, chapter by chapter */
        {
            struct np_scrub_ctx sc;
            unsigned long pos = 0;
            sc.length_ms = len;
            sc.ch = c;
            sc.snap = true;
            for (i = 1; i < 648; i++)
            {
                pos = np_scrub_wheel(&sc, pos, 1, 60);
                if (pos != st[i])
                {
                    CHECK_EQ(pos, st[i]);
                    break;
                }
            }
            CHECK_EQ(np_scrub_wheel(&sc, pos, 1, 60), len);
        }
        free(st);
    }
    /* w = 0 is harmless */
    {
        static const uint32_t st[] = { 500 };
        struct np_chapters c = arr(st, 1);
        CHECK_EQ(np_chapter_ticks(&c, 1000, 0, bits), 0);
    }
}

int main(void)
{
    test_step();
    test_music();
    test_book_snap();
    test_chapter_at();
    test_bar_and_ticks();
    CHECK_DONE("test_np_scrub");
}
