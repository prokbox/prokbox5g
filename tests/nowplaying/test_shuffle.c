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

/* Host test: apps/ipodui/shuffle_core.c (library-backed shuffle, #27). */
#include <stdlib.h>
#include <string.h>
#include "shuffle_core.h"
#include "check.h"

/* every position maps to a distinct item in [0, n) */
static int is_bijection(uint32_t n, uint32_t seed)
{
    struct shuf_perm p;
    unsigned char *seen = calloc(n ? n : 1, 1);
    uint32_t i;
    int ok = 1;

    shuf_perm_init(&p, n, seed);
    for (i = 0; i < n; i++)
    {
        uint32_t x = shuf_perm_at(&p, i);
        if (x >= n || seen[x])
        {
            ok = 0;
            break;
        }
        seen[x] = 1;
    }
    if (shuf_perm_at(&p, n) != SHUF_NONE)
        ok = 0;
    free(seen);
    return ok;
}

static void test_bijection(void)
{
    static const uint32_t sizes[] = { 1, 2, 3, 4, 5, 16, 17, 255, 256, 257,
                                      2000, 50000, 65536, 65537 };
    static const uint32_t seeds[] = { 0, 1, 0xdeadbeef, 12345 };
    size_t i, j;
    struct shuf_perm p;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        for (j = 0; j < sizeof(seeds) / sizeof(seeds[0]); j++)
            if (!is_bijection(sizes[i], seeds[j]))
            {
                fprintf(stderr, "not a bijection: n=%u seed=%u\n",
                        (unsigned)sizes[i], (unsigned)seeds[j]);
                check_fails++;
            }
    /* n = 0: empty */
    shuf_perm_init(&p, 0, 7);
    CHECK_EQ(shuf_perm_at(&p, 0), SHUF_NONE);
    /* large n: spot-check the domain stays bounded (<4n) */
    shuf_perm_init(&p, 1000000, 3);
    CHECK((1u << (2 * p.half_bits)) < 4u * 1000000u);
    CHECK(shuf_perm_at(&p, 999999) < 1000000);
}

static void test_quality(void)
{
    /* not the identity, different seeds give different orders, the same
     * seed the same order; fixed points ~1 on average */
    struct shuf_perm a, b, c;
    uint32_t i, same_ab = 0, fixed = 0, n = 50000, ascending = 0;

    shuf_perm_init(&a, n, 1);
    shuf_perm_init(&b, n, 2);
    shuf_perm_init(&c, n, 1);
    for (i = 0; i < n; i++)
    {
        uint32_t x = shuf_perm_at(&a, i);
        same_ab += (x == shuf_perm_at(&b, i));
        fixed += (x == i);
        CHECK_EQ(x, shuf_perm_at(&c, i));
        if (i > 0 && x == shuf_perm_at(&a, i - 1) + 1)
            ascending++;
    }
    CHECK(same_ab < 20);
    CHECK(fixed < 20);
    CHECK(ascending < 20);          /* no runs of library order */
    printf("   quality n=%u: %u positions equal across seeds, %u fixed "
           "points, %u ascending pairs\n", (unsigned)n, (unsigned)same_ab,
           (unsigned)fixed, (unsigned)ascending);
}

static void test_songs(void)
{
    struct shuf_songs s, t;
    unsigned char seen[2000];
    uint32_t i, e, n = 2000, order[2000];

    /* one lap: every entry once, then the end */
    memset(seen, 0, sizeof(seen));
    shuf_songs_init(&s, n, 99, SHUF_NONE);
    for (i = 0; i < n; i++)
    {
        e = shuf_songs_next(&s, false);
        CHECK(e < n);
        if (e < n)
        {
            CHECK(!seen[e]);
            seen[e] = 1;
        }
        order[i] = e;
    }
    CHECK_EQ(shuf_songs_next(&s, false), SHUF_NONE);

    /* prev walks back through the same order, next comes forward again */
    shuf_songs_init(&s, n, 99, SHUF_NONE);
    for (i = 0; i < 10; i++)
        shuf_songs_next(&s, false);
    CHECK_EQ(shuf_songs_prev(&s), order[8]);
    CHECK_EQ(shuf_songs_prev(&s), order[7]);
    CHECK_EQ(shuf_songs_next(&s, false), order[8]);
    CHECK_EQ(shuf_songs_next(&s, false), order[9]);
    CHECK_EQ(shuf_songs_next(&s, false), order[10]);
    for (i = 0; i < 20; i++)
        shuf_songs_prev(&s);
    CHECK_EQ(s.last, order[0]);
    CHECK_EQ(shuf_songs_prev(&s), SHUF_NONE);

    /* resume: the struct is the resume record */
    shuf_songs_init(&s, n, 4242, SHUF_NONE);
    for (i = 0; i < 777; i++)
        shuf_songs_next(&s, true);
    memcpy(&t, &s, sizeof(t));
    for (i = 0; i < 3000; i++)
        CHECK_EQ(shuf_songs_next(&s, true), shuf_songs_next(&t, true));

    /* 'first' plays first and is not repeated in lap 0 */
    memset(seen, 0, sizeof(seen));
    shuf_songs_init(&s, n, 5, 1234);
    CHECK_EQ(shuf_songs_next(&s, false), 1234);
    seen[1234] = 1;
    for (i = 1; i < n; i++)
    {
        e = shuf_songs_next(&s, false);
        CHECK(e < n && !seen[e]);
        if (e < n)
            seen[e] = 1;
    }
    CHECK_EQ(shuf_songs_next(&s, false), SHUF_NONE);
    /* prev from the second track returns to 'first' */
    shuf_songs_init(&s, n, 5, 1234);
    shuf_songs_next(&s, false);
    e = shuf_songs_next(&s, false);
    CHECK_EQ(shuf_songs_prev(&s), 1234);
    CHECK_EQ(shuf_songs_prev(&s), SHUF_NONE);
    CHECK_EQ(shuf_songs_next(&s, false), e);

    /* repeat: laps are fresh orders, full again, and never repeat the
     * last entry of the previous lap at the boundary */
    {
        uint32_t laps, last = SHUF_NONE, k;
        for (k = 2; k <= 5; k++)
        {
            shuf_songs_init(&s, k, 77, SHUF_NONE);
            for (laps = 0; laps < 200; laps++)
            {
                unsigned char sn[5] = { 0 };
                for (i = 0; i < k; i++)
                {
                    e = shuf_songs_next(&s, true);
                    CHECK(e < k && !sn[e]);
                    if (e < k)
                        sn[e] = 1;
                    if (i == 0 && laps > 0)
                        CHECK(e != last);
                    last = e;
                }
            }
        }
    }
    /* n = 1 with repeat: the same song forever, no hang */
    shuf_songs_init(&s, 1, 3, SHUF_NONE);
    for (i = 0; i < 5; i++)
        CHECK_EQ(shuf_songs_next(&s, true), 0);
    /* n = 0 */
    shuf_songs_init(&s, 0, 3, SHUF_NONE);
    CHECK_EQ(shuf_songs_next(&s, true), SHUF_NONE);
    CHECK_EQ(shuf_songs_prev(&s), SHUF_NONE);

    /* full library: 50,000 entries, of which every third is not music
     * (books, podcasts, deleted): the filter is the caller's skip */
    {
        uint32_t big = 50000, music = 0, got = 0;
        unsigned char *bs = calloc(big, 1);
        shuf_songs_init(&s, big, 2026, SHUF_NONE);
        while ((e = shuf_songs_next(&s, false)) != SHUF_NONE)
        {
            if (e % 3 == 2)
                continue;
            CHECK(!bs[e]);
            bs[e] = 1;
            got++;
        }
        for (i = 0; i < big; i++)
            music += (i % 3 != 2);
        CHECK_EQ(got, music);
        free(bs);
    }
}

/* albums of 0..7 tracks */
static uint32_t alen(void *ctx, uint32_t a)
{
    (void)ctx;
    return (a * 7 + 3) % 8;
}

static uint32_t zero(void *ctx, uint32_t a)
{
    (void)ctx;
    (void)a;
    return 0;
}

static void test_albums(void)
{
    struct shuf_albums s;
    uint32_t A = 500, a, t, prev_a = SHUF_NONE, prev_t = 0, total = 0;
    uint32_t albums_seen = 0, expect = 0, i;
    unsigned char *done = calloc(A, 1);
    uint32_t first_albums[8];

    shuf_albums_init(&s, A, 31337);
    while (shuf_albums_next(&s, alen, NULL, false, &a, &t))
    {
        CHECK(a < A);
        CHECK(t < alen(NULL, a));
        if (a != prev_a)
        {
            /* a new album starts at track 0, and is never revisited */
            CHECK_EQ(t, 0);
            CHECK(!done[a]);
            if (prev_a != SHUF_NONE)
                CHECK_EQ(prev_t + 1, alen(NULL, prev_a));
            done[a] = 1;
            if (albums_seen < 8)
                first_albums[albums_seen] = a;
            albums_seen++;
        }
        else
            CHECK_EQ(t, prev_t + 1);    /* tracks in album order */
        prev_a = a;
        prev_t = t;
        total++;
    }
    for (i = 0; i < A; i++)
    {
        expect += alen(NULL, i);
        CHECK(done[i] == (alen(NULL, i) > 0));
    }
    CHECK_EQ(total, expect);
    /* album order is shuffled, not library order */
    {
        int ascending = 1;
        for (i = 1; i < 8; i++)
            if (first_albums[i] != first_albums[i - 1] + 1)
                ascending = 0;
        CHECK(!ascending);
    }

    /* prev: within an album, then into the previous album's last track */
    shuf_albums_init(&s, A, 31337);
    {
        uint32_t ha[40], ht[40];
        for (i = 0; i < 40; i++)
            CHECK(shuf_albums_next(&s, alen, NULL, false, &ha[i], &ht[i]));
        for (i = 39; i-- > 0; )
        {
            CHECK(shuf_albums_prev(&s, alen, NULL, &a, &t));
            CHECK_EQ(a, ha[i]);
            CHECK_EQ(t, ht[i]);
        }
        CHECK(!shuf_albums_prev(&s, alen, NULL, &a, &t));
        CHECK(shuf_albums_next(&s, alen, NULL, false, &a, &t));
        CHECK_EQ(a, ha[1]);
        CHECK_EQ(t, ht[1]);
    }

    /* repeat: continues into a new lap */
    shuf_albums_init(&s, A, 1);
    for (i = 0; i < expect + 10; i++)
        CHECK(shuf_albums_next(&s, alen, NULL, true, &a, &t));
    CHECK_EQ(s.lap, 1);

    /* nothing playable: no hang, false */
    {
        shuf_albums_init(&s, 10, 1);
        CHECK(!shuf_albums_next(&s, zero, NULL, true, &a, &t));
        shuf_albums_init(&s, 0, 1);
        CHECK(!shuf_albums_next(&s, alen, NULL, true, &a, &t));
    }
    free(done);
}

/* Albums on an ordinary queue: groups move whole, keep their inner
 * order, every entry stays exactly once, and the order depends on the
 * seed. */
static void test_groups(void)
{
    static const uint32_t sizes[][8] = {
        { 1 }, { 3 }, { 1, 1 }, { 2, 5, 1, 4 }, { 12, 1, 9, 3, 7, 2, 11, 6 },
    };
    unsigned c;

    for (c = 0; c < sizeof(sizes) / sizeof(sizes[0]); c++)
    {
        unsigned long a[64], seen[64];
        uint8_t starts[8] = { 0 }, placed[8];
        uint32_t n = 0, g = 0, k, groups, seed;

        for (g = 0; g < 8 && sizes[c][g]; g++)
        {
            starts[n >> 3] |= (uint8_t)(1u << (n & 7));
            for (k = 0; k < sizes[c][g]; k++, n++)
                a[n] = 1000 * g + k;        /* group g, entry k */
        }
        for (seed = 1; seed < 6; seed++)
        {
            unsigned long b[64];
            memcpy(b, a, sizeof(a));
            groups = shuf_group_order(b, n, starts, placed, seed);
            CHECK_EQ(groups, g);
            memset(seen, 0, sizeof(seen));
            for (k = 0; k < n; k++)
            {
                unsigned long grp = b[k] / 1000, e = b[k] % 1000;
                CHECK(grp < g && e < sizes[c][grp]);
                CHECK(!(seen[grp] & (1ul << e)));
                seen[grp] |= 1ul << e;
                /* inside a group: entries in order, contiguous */
                if (e > 0)
                    CHECK(k > 0 && b[k - 1] == b[k] - 1);
            }
            for (k = 0; k < g; k++)
                CHECK_EQ(seen[k], (1ul << sizes[c][k]) - 1);
        }
    }
    {   /* the 8-group case is not the identity for some seed */
        unsigned long b[64];
        uint8_t starts[8] = { 0x01, 0x10, 0x00 }, placed[8];
        uint32_t i, moved = 0, seed;

        for (seed = 1; seed < 10; seed++)
        {
            for (i = 0; i < 20; i++)
                b[i] = i;
            shuf_group_order(b, 20, starts, placed, seed);
            moved += b[0] != 0;
        }
        CHECK(moved > 0);
    }
}

int main(void)
{
    test_bijection();
    test_quality();
    test_songs();
    test_albums();
    test_groups();
    printf("   sizeof: shuf_perm %zu, shuf_songs %zu, shuf_albums %zu B "
           "(host)\n", sizeof(struct shuf_perm), sizeof(struct shuf_songs),
           sizeof(struct shuf_albums));
    CHECK_DONE("test_shuffle");
}
