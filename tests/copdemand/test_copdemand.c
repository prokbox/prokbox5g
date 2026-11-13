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
 * Host test: the COP's clock demand (apps/codec_place_core.c,
 * cpl_demand_*()). A closed loop: the music needs C MHz at 30 MHz and
 * k * C at 80 (SDRAM stalls cost more cycles at the higher clock); the
 * clock is 80 while the boost is asked for, else 30.
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
#include "codec_place_core.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

/* one period of 1 s of audio at demand x10 (tenths of a MHz), run at
 * mhz (the busy time is the cycles over the clock) */
static bool period_at(struct cpl_demand *d, unsigned int x10,
                      unsigned int mhz)
{
    return cpl_demand_period(d, x10 * 100000u, x10 * 100000u / mhz,
                             1000000u);
}

static bool period(struct cpl_demand *d, unsigned int x10)
{
    return period_at(d, x10, 30);
}

/* n periods of a music needing c30 (x10) at 30 MHz and c30*k100/100 at
 * 80; returns the number of hold/release changes; *first: the period of
 * the first change (-1: none) */
static int run(struct cpl_demand *d, int n, unsigned int c30,
               unsigned int k100, int *first)
{
    int i, changes = 0;

    if (first)
        *first = -1;
    for (i = 0; i < n; i++)
    {
        unsigned int x10 = d->want ? c30 * k100 / 100u : c30;

        if (period_at(d, x10, d->want ? 80 : 30))
        {
            if (first && *first < 0)
                *first = i;
            changes++;
        }
    }
    return changes;
}

static void test_units(void)
{
    struct cpl_demand d;

    cpl_demand_init(&d);
    CHECK(!d.want && !d.seeded);
    /* 21.0 MHz exactly: 21 M cycles in 1 s of audio */
    CHECK(cpl_demand_period(&d, 21000000u, 700000u, 1000000u));
    CHECK(d.last_x10 == 210 && d.ema_x10 == 210 && d.want);
    /* 20.9 seeded: no hold (the literal threshold, not the constant) */
    cpl_demand_init(&d);
    CHECK(!cpl_demand_period(&d, 20900000u, 696666u, 1000000u));
    CHECK(d.last_x10 == 209 && !d.want);
    /* no audio: nothing changes */
    CHECK(!cpl_demand_period(&d, 50000000u, 625000u, 0));
    CHECK(d.last_x10 == 209);
    /* a long period (a slow decode at 80 MHz: 3 s of work for 1 s) */
    cpl_demand_init(&d);
    CHECK(cpl_demand_period(&d, 240000000u, 3000000u, 1000000u));
    CHECK(d.last_x10 == 2400);
}

/* the light formats stay at 30 MHz for an hour */
static void test_light(void)
{
    struct cpl_demand d;
    int first;

    cpl_demand_init(&d);
    CHECK(run(&d, 3600, 80, 110, &first) == 0);     /* 16/44.1 FLAC ~8 */
    CHECK(!d.want);
    cpl_demand_init(&d);
    CHECK(run(&d, 3600, 190, 110, &first) == 0);    /* MP3, the estimate's top */
    CHECK(!d.want);
    cpl_demand_init(&d);
    CHECK(run(&d, 3600, 205, 110, &first) == 0);    /* just under the line */
}

/* a heavy format holds at once and keeps it: one change in an hour */
static void test_heavy(void)
{
    struct cpl_demand d;
    int first;

    /* Opus on silicon at 30 MHz: over 100 % busy (34 MHz) */
    cpl_demand_init(&d);
    CHECK(run(&d, 3600, 340, 110, &first) == 1);
    CHECK(first == 0 && d.want);
    /* Opus as the emulator reads it (1/1.4 of silicon): still held */
    cpl_demand_init(&d);
    CHECK(run(&d, 3600, 243, 100, &first) == 1);
    CHECK(first == 0 && d.want);
    /* the laptop emulator's Opus 128 (9b899de6c2): 55.7 % busy pinned at
     * 80 MHz = 44.6 MHz; at 30 MHz it saturates (30.0 and more). Held
     * from the first second, never let go in an hour */
    cpl_demand_init(&d);
    CHECK(run(&d, 3600, 300, 149, &first) == 1);
    CHECK(first == 0 && d.want && d.ema_x10 >= 440);
    /* the same on silicon (~1.4x: ~62 MHz at 80) */
    cpl_demand_init(&d);
    CHECK(run(&d, 3600, 420, 148, &first) == 1 && first == 0 && d.want);
    /* a demand just over the line, the same at both clocks: held */
    cpl_demand_init(&d);
    CHECK(run(&d, 3600, 211, 100, &first) == 1 && d.want);
    /* a light track's average, then a heavy one: ramps up in a few s */
    cpl_demand_init(&d);
    CHECK(run(&d, 60, 100, 110, &first) == 0);
    CHECK(run(&d, 60, 260, 110, &first) == 1);
    CHECK(first >= 1 && first <= 4);
}

/* a starving period holds at once even from a low average */
static void test_saturation(void)
{
    struct cpl_demand d;
    int i;

    cpl_demand_init(&d);
    CHECK(run(&d, 30, 80, 110, NULL) == 0);
    CHECK(period(&d, 300) && d.want);       /* 30.0: 100 % at 30 MHz */
    /* held for exactly the minimum (10 periods), then let go */
    for (i = 0; i < 9; i++)
        CHECK(!period(&d, 80));
    CHECK(period(&d, 80) && !d.want);
    /* the starving trigger is the busy share of the period's own clock
     * (review: at 24 MHz, 100 % busy reads 24.0, never 30.0): from a low
     * average, 89.7 % at 30 MHz (26.9) does not ask, 90.0 % (27.0) does */
    cpl_demand_init(&d);
    CHECK(run(&d, 30, 80, 110, NULL) == 0);
    CHECK(!period(&d, 269));
    cpl_demand_init(&d);
    CHECK(run(&d, 30, 80, 110, NULL) == 0);
    CHECK(period(&d, 270) && d.want);
    /* at 24 MHz (the PLL off): 21.6 is 90 % busy: asks at once from a low
     * average (the smoothed 11.4 is far under 21.0); 21.5 does not */
    cpl_demand_init(&d);
    CHECK(run(&d, 30, 80, 110, NULL) == 0);
    CHECK(period_at(&d, 216, 24) && d.want && d.ema_x10 < 210);
    cpl_demand_init(&d);
    CHECK(run(&d, 30, 80, 110, NULL) == 0);
    CHECK(!period_at(&d, 214, 24) && !d.want);
    /* and 24.0 at 24 MHz (100 %) at a run's first period */
    cpl_demand_init(&d);
    cpl_demand_restart(&d);
    CHECK(period_at(&d, 200, 20) && d.want);
}

/* hysteresis: let go only under 15.0, and never on a period that asks */
static void test_release(void)
{
    struct cpl_demand d;
    int first;

    /* Opus, then a FLAC album: one release, within ~15 s of the change */
    cpl_demand_init(&d);
    CHECK(run(&d, 120, 340, 110, &first) == 1);
    CHECK(run(&d, 120, 80, 110, &first) == 1);
    CHECK(!d.want && first >= 0 && first <= 15);
    /* between the lines (18 MHz at 80) within one run: the track that
     * asked keeps it (the hysteresis); a NEW run between the lines is
     * judged fresh against 21.0 and lets go at its first period (review
     * of p083-copclock, Major: the boost stuck across tracks) */
    cpl_demand_init(&d);
    CHECK(period(&d, 300));
    CHECK(run(&d, 600, 180, 100, NULL) == 0 && d.want);
    cpl_demand_restart(&d);
    CHECK(run(&d, 3600, 180, 100, &first) == 1 && first == 0 && !d.want);
    /* a new run reseeds the average: a low track lets go at once */
    cpl_demand_init(&d);
    CHECK(period(&d, 300));
    cpl_demand_restart(&d);
    CHECK(run(&d, 1, 80, 100, &first) == 1 && !d.want);
    /* the review's case: an Opus run (44.6 MHz at 80), then a light
     * album that reads 18.0 at 80 (15.0 at 30): let go within the light
     * run's first 10 s, then stays let go; the next Opus asks again */
    cpl_demand_init(&d);
    CHECK(run(&d, 180, 300, 149, NULL) == 1 && d.want);
    cpl_demand_restart(&d);
    CHECK(run(&d, 600, 150, 120, &first) == 1);
    CHECK(first >= 0 && first < 10 && !d.want);
    cpl_demand_restart(&d);
    CHECK(run(&d, 600, 150, 120, NULL) == 0 && !d.want);
    cpl_demand_restart(&d);
    CHECK(run(&d, 60, 300, 149, &first) == 1 && first == 0 && d.want);
    /* a heavy run's first period asks at once (a new Opus track) */
    cpl_demand_init(&d);
    cpl_demand_restart(&d);
    CHECK(period(&d, 250) && d.want);
    /* a heavy period is never the one that lets go, even with the
     * average low (held 10, then 25 MHz after silence-like periods) */
    cpl_demand_init(&d);
    CHECK(period(&d, 300));
    {
        int i;

        for (i = 0; i < 9; i++)
            CHECK(!period(&d, 0));
    }
    CHECK(!period(&d, 250) && d.want && d.ema_x10 < 150);
    CHECK(period(&d, 0) && !d.want);
    /* 15.0 is not under 15.0 (within the run that asked) */
    cpl_demand_init(&d);
    CHECK(period(&d, 300));
    CHECK(run(&d, 100, 150, 100, NULL) == 0 && d.want);
    CHECK(run(&d, 30, 149, 100, NULL) == 1 && !d.want);
}

/* no flapping: a demand that swings across both lines changes at most
 * once per minimum hold, and a jittery one near the hold line settles */
static void test_no_flap(void)
{
    struct cpl_demand d;
    int i, changes = 0;

    cpl_demand_init(&d);
    for (i = 0; i < 3600; i++)
        changes += period(&d, (i & 1) ? 120 : 280);  /* mean 20.0 */
    CHECK(changes <= 1);
    cpl_demand_init(&d);
    changes = 0;
    for (i = 0; i < 3600; i++)
        changes += period(&d, (i % 20) < 10 ? 300 : 60);
    CHECK(changes <= 3600 / 10);
    CHECK(changes > 0);     /* the series does reach both sides */
    /* with the closed loop (the clock follows), a C right at the line */
    cpl_demand_init(&d);
    CHECK(run(&d, 3600, 210, 120, NULL) == 1 && d.want);
}

int main(void)
{
    test_units();
    test_light();
    test_heavy();
    test_saturation();
    test_release();
    test_no_flap();
    if (fails)
        fprintf(stderr, "%d check(s) failed\n", fails);
    else
        printf("copdemand: 6 groups passed\n");
    return fails ? 1 : 0;
}
