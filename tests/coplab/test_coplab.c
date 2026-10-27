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
 * Host test of the COP bench rows' pure logic
 * (firmware/target/arm/ipod/video/bcm-coplab.h): the C1 window bounds,
 * C2's trial classes and verdicts, C3's sleep tallies and verdict.
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
#include <string.h>
#include "bcm-coplab.h"

static int fails;

#define CHECK(c) do { if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void t_window(void)
{
    CHECK(coplab_in_window(COPLAB_TARGET, COPLAB_BYTES));
    CHECK(coplab_in_window(COPLAB_WIN, COPLAB_WIN_SIZE));
    CHECK(!coplab_in_window(COPLAB_WIN - 4, 8));
    CHECK(!coplab_in_window(COPLAB_WIN + COPLAB_WIN_SIZE - 4, 8));
    CHECK(!coplab_in_window(COPLAB_WIN, COPLAB_WIN_SIZE + 4));
    CHECK(!coplab_in_window(COPLAB_WIN, 0));
    CHECK(!coplab_in_window(COPLAB_WIN + 2, 4));
    CHECK(!coplab_in_window(COPLAB_WIN, 6));
    /* no wrap: a huge size from inside must not pass */
    CHECK(!coplab_in_window(COPLAB_WIN + 4, 0xFFFFFFFCu));
    /* the piece and its guards fill the window exactly */
    CHECK(COPLAB_TARGET + COPLAB_BYTES + 4 * COPLAB_GUARD_WORDS ==
          COPLAB_WIN + COPLAB_WIN_SIZE);
    CHECK(COPLAB_BYTES % 4 == 0 && COPLAB_BYTES <= 0x10000);
}

static void trial(struct coplab_cache_trial *t, uint32_t seq, int pre,
                  int u1, int r, int p)
{
    uint32_t v[4];

    coplab_cache_values(t, seq);
    v[0] = t->v0; v[1] = t->v1; v[2] = t->v9; v[3] = 0xDEADBEEF;
    t->pre = v[pre];
    t->u1 = v[u1];
    t->r = v[r];
    t->p = v[p];
}

static void t_cache(void)
{
    struct coplab_cache_trial t;
    uint32_t n[COPLAB_C_N], s;
    int i;

    /* the three values differ, in both halves, every seq */
    for (s = 0; s < 70000; s += 997)
    {
        coplab_cache_values(&t, s);
        CHECK(t.v0 != t.v1 && t.v1 != t.v9 && t.v0 != t.v9);
        CHECK((t.v0 >> 16) != (t.v1 >> 16) && (t.v1 >> 16) != (t.v9 >> 16));
        CHECK((t.v0 & 0xFFFF) != (t.v1 & 0xFFFF) &&
              (t.v1 & 0xFFFF) != (t.v9 & 0xFFFF) &&
              (t.v0 & 0xFFFF) != (t.v9 & 0xFFFF));
    }
    /* the classes: 0 v0, 1 v1, 2 v9, 3 other */
    trial(&t, 1, 0, 0, 0, 2);
    CHECK(coplab_cache_class(&t) == COPLAB_C_SEPARATE);
    trial(&t, 2, 0, 0, 1, 2);
    CHECK(coplab_cache_class(&t) == COPLAB_C_SHARED);
    trial(&t, 3, 0, 0, 2, 2);
    CHECK(coplab_cache_class(&t) == COPLAB_C_MISS);
    trial(&t, 4, 0, 0, 3, 2);
    CHECK(coplab_cache_class(&t) == COPLAB_C_ODD);
    /* the voids come first, whatever r says */
    trial(&t, 5, 3, 0, 1, 2);
    CHECK(coplab_cache_class(&t) == COPLAB_C_SETUP);
    trial(&t, 6, 0, 1, 1, 2);
    CHECK(coplab_cache_class(&t) == COPLAB_C_THROUGH);
    trial(&t, 7, 0, 0, 1, 1);           /* W's line went back: v1 there */
    CHECK(coplab_cache_class(&t) == COPLAB_C_EVICT);
    for (i = 0; i < COPLAB_C_N; i++)
        CHECK(strcmp(coplab_cache_name(i), "?") != 0);
    CHECK(strcmp(coplab_cache_name(COPLAB_C_N), "?") == 0);

    /* verdicts */
    memset(n, 0, sizeof(n));
    CHECK(coplab_cache_verdict(n) == COPLAB_V_NONE);
    n[COPLAB_C_SEPARATE] = COPLAB_MIN_VALID - 1;
    n[COPLAB_C_MISS] = 10;              /* voids never count */
    CHECK(coplab_cache_verdict(n) == COPLAB_V_NONE);
    n[COPLAB_C_SEPARATE]++;
    CHECK(coplab_cache_verdict(n) == COPLAB_V_SEPARATE);
    n[COPLAB_C_SHARED] = 1;
    CHECK(coplab_cache_verdict(n) == COPLAB_V_MIXED);
    n[COPLAB_C_SEPARATE] = 0;
    n[COPLAB_C_SHARED] = COPLAB_MIN_VALID;
    CHECK(coplab_cache_verdict(n) == COPLAB_V_SHARED);
    n[COPLAB_C_ODD] = 1;
    CHECK(coplab_cache_verdict(n) == COPLAB_V_MIXED);
    for (i = 0; i < 4; i++)
        CHECK(strcmp(coplab_verdict_name(i), "?") != 0);
}

static void fill(struct coplab_sleep *s, const char *pattern, int reps)
{
    int k;
    const char *p;

    coplab_sleep_init(s);
    for (k = 0; k < reps; k++)
        for (p = pattern; *p; p++)
            coplab_sleep_add(s, *p == 'z');
}

static void t_sleep(void)
{
    struct coplab_sleep y, b, z;

    coplab_sleep_init(&z);
    CHECK(z.idle_us == ~0u && coplab_sleep_permille(&z) == 0);
    CHECK(coplab_idle_permille(&z) == ~0u);

    /* entries count awake-to-asleep edges, a run of z's once */
    fill(&b, "azzzzzzzzz", 100);        /* 90 % asleep, 100 entries */
    CHECK(b.samples == 1000 && b.asleep == 900 && b.entries == 100);
    CHECK(coplab_sleep_permille(&b) == 900);
    fill(&z, "zzzz", 3);                /* starts asleep: one entry */
    CHECK(z.entries == 1 && coplab_sleep_permille(&z) == 1000);

    /* the kernel's idle share */
    b.window_us = 1000000;
    b.idle_us = 250000;
    CHECK(coplab_idle_permille(&b) == 250);
    b.idle_us = 2000000;
    CHECK(coplab_idle_permille(&b) == 1000);
    b.window_us = 0;
    CHECK(coplab_idle_permille(&b) == ~0u);

    /* verdicts */
    fill(&y, "a", 1000);
    fill(&b, "azzzzzzzzz", 100);
    CHECK(coplab_sleep_verdict(&y, &b) == COPLAB_S_AWAKE);
    fill(&y, "aazzzzzzzz", 100);        /* 80 % under yield too */
    CHECK(coplab_sleep_verdict(&y, &b) == COPLAB_S_SLEEPS);
    fill(&y, "aaaaaaaaaz", 100);        /* 10 %: neither */
    CHECK(coplab_sleep_verdict(&y, &b) == COPLAB_S_UNCLEAR);
    fill(&y, "a", 1000);
    fill(&b, "aaaaaaaaaz", 100);        /* the control never slept */
    CHECK(coplab_sleep_verdict(&y, &b) == COPLAB_S_UNCLEAR);
    coplab_sleep_init(&b);              /* no samples */
    CHECK(coplab_sleep_verdict(&y, &b) == COPLAB_S_UNCLEAR);
}

int main(void)
{
    t_window();
    t_cache();
    t_sleep();
    if (fails)
        printf("%d failed\n", fails);
    return fails ? 1 : 0;
}
