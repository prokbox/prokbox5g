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
 * iPod UI shell: host test of the drift's next-cover picker
 * (apps/ipodui/drift_pick.h; owner rule: never the same cover twice in a
 * row, including at the wrap and when the set changes order). Built and
 * run by tests/drift/run.sh.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "ipodui/drift_pick.h"
#include "../nowplaying/check.h"

#define MAXN 32

static void test_basic(void)
{
    uint64_t k[MAXN];
    int i;

    for (i = 0; i < MAXN; i++)
        k[i] = 0x1000 + i;

    /* nothing, or nothing on screen yet */
    CHECK_EQ(drift_pick_next(k, 0, -1, 0), -1);
    CHECK_EQ(drift_pick_next(k, 5, -1, 0), 0);
    /* in order, and the wrap never repeats */
    CHECK_EQ(drift_pick_next(k, 5, 0, k[0]), 1);
    CHECK_EQ(drift_pick_next(k, 5, 4, k[4]), 0);
    CHECK_EQ(drift_pick_next(k, 2, 1, k[1]), 0);
    CHECK_EQ(drift_pick_next(k, 2, 0, k[0]), 1);
    /* one cover: hold it (never a fade into itself) */
    CHECK_EQ(drift_pick_next(k, 1, 0, k[0]), -1);
    CHECK_EQ(drift_pick_next(k, 1, -1, k[0]), -1);
    /* one cover, another one on screen (a set switch): show it */
    CHECK_EQ(drift_pick_next(k, 1, 0, 0x9999), 0);
    /* the set shrank under the index: the cover on screen at index 3 is
     * at index 0 now; the next one is 1, never 0 */
    {
        uint64_t s[3] = { k[3], k[7], k[8] };
        CHECK_EQ(drift_pick_next(s, 3, 3, k[3]), 1);
        CHECK_EQ(drift_pick_next(s, 3, 2, k[3]), 1);
    }
    /* reordered so that index cur+1 is the cover on screen */
    {
        uint64_t s[4] = { k[1], k[2], k[5], k[6] };
        CHECK_EQ(drift_pick_next(s, 4, 1, k[5]), 3);  /* after k[5]'s */
    }
    /* the cover on screen left the set: the index carries on */
    {
        uint64_t s[3] = { k[1], k[2], k[3] };
        int n = drift_pick_next(s, 3, 1, k[9]);
        CHECK(n >= 0 && s[n] != k[9]);
    }
}

/* Random walks: the set grows, shrinks and reshuffles between picks, as
 * panels.pak does under a running builder; consecutive covers on screen
 * must always differ while the set has >= 2 covers, and the picker holds
 * (-1) only when the set is just the cover on screen. */
static void test_walk(void)
{
    uint64_t pool[64], set[MAXN];
    unsigned seed = 12345;
    int n = 0, cur = -1, step, repeats = 0, holds = 0, bad_holds = 0;
    uint64_t shown = 0;
    int i;

    for (i = 0; i < 64; i++)
        pool[i] = 0xabc00000ull + (uint64_t)i * 977;

    for (step = 0; step < 200000; step++)
    {
        int pick;

        seed = seed * 1103515245u + 12345u;
        if ((seed >> 16) % 7 == 0)
        {
            /* a new set: 0..32 distinct keys from the pool, any order,
             * sometimes keeping the cover on screen */
            int want = (seed >> 8) % (MAXN + 1), j;
            bool used[64];

            memset(used, 0, sizeof(used));
            n = 0;
            while (n < want)
            {
                seed = seed * 1103515245u + 12345u;
                j = (seed >> 10) % 64;
                if (!used[j])
                {
                    used[j] = true;
                    set[n++] = pool[j];
                }
            }
        }
        pick = drift_pick_next(set, n, cur, shown);
        if (pick < 0)
        {
            holds++;
            /* a hold is right only for no cover, or just the one shown */
            if (!(n == 0 || (n == 1 && set[0] == shown)))
                bad_holds++;
            continue;
        }
        if (set[pick] == shown)
            repeats++;
        shown = set[pick];
        cur = pick;
    }
    CHECK_EQ(repeats, 0);
    CHECK_EQ(bad_holds, 0);
    CHECK(holds > 0);           /* the walk did reach the 0/1 cover sets */
}

/* A stable set cycles through every cover before any comes back. */
static void test_cycle(void)
{
    uint64_t k[7];
    int seen[7] = { 0 }, i, cur = 3;

    for (i = 0; i < 7; i++)
        k[i] = 0x500 + i;
    for (i = 0; i < 7; i++)
    {
        cur = drift_pick_next(k, 7, cur, k[cur]);
        CHECK(cur >= 0);
        if (cur >= 0)
            seen[cur]++;
    }
    for (i = 0; i < 7; i++)
        CHECK_EQ(seen[i], 1);
}

int main(void)
{
    test_basic();
    test_walk();
    test_cycle();
    CHECK_DONE("test_drift");
}
