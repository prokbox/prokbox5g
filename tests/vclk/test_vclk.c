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

/* Host test of the video chip's clock governor (apps/video/vclk.c), with
   the stage 6 clips' FEED traces from silicon (tests/vclk/traces) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video/vclk.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)
#define PFX(l, s) (!strncmp((l), (s), strlen(s)))

#define ALL     0xFFF
#define MHZ(b)  ((uint32_t)vclk_band_mhz[(b)])

static void test_bands(void)
{
    int b;

    CHECK(vclk_level_for_hz(27000000) == 5);
    CHECK(vclk_level_for_hz(46875000) == 5);
    CHECK(vclk_level_for_hz(46875001) == 6);
    CHECK(vclk_level_for_hz(150000000) == 15);
    CHECK(vclk_level_for_hz(147075854) == 14);
    for (b = 1; b < VCLK_BANDS; b++)
    {
        uint32_t f = MHZ(b), div = f < 75 ? 2 : 1;

        CHECK(f > MHZ(b - 1) && f <= 150);
        /* Apple's level for the clock, and the top whole MHz of its band:
           a MHz more needs a higher level (but 150, the ceiling) */
        CHECK(vclk_band_level[b] == vclk_level_for_hz(f * 1000000));
        if (b < VCLK_TOP)
            CHECK(vclk_level_for_hz((f + 1) * 1000000) > vclk_band_level[b]);
        /* PLL A by Apple's rule: N fits ten bits, the VCO >= 75 MHz */
        CHECK(f * div * 4 <= 1023 && f * div >= 75);
    }
    CHECK(vclk_top(ALL) == VCLK_TOP && vclk_top(1) == 0);
    CHECK(vclk_top(0x1 | 0x20) == 5);
    CHECK(vclk_band_for(ALL, 60) == 3);             /* 74 MHz */
    CHECK(vclk_band_for(ALL, 27) == 0);
    CHECK(vclk_band_for(ALL, 200) == VCLK_TOP);
    /* unverified: the nearest verified band above */
    CHECK(vclk_band_for(0x1 | 0x20 | 0x800, 60) == 5);
    CHECK(vclk_band_for(0x1 | 0x800, 60) == VCLK_TOP);
    CHECK(vclk_band_for(0x1, 60) == 0);             /* the crystal only */
    CHECK(vclk_model_cycles(320, 240, 169, 30000) > 1100000 &&
          vclk_model_cycles(320, 240, 169, 30000) < 1300000);
}

/* a trace: cycles per picture (FEED us x MHz at 150, session 27) */
struct trace
{
    uint32_t n, cyc[400];
};

static int load(const char *path, struct trace *t)
{
    FILE *f = fopen(path, "r");
    char line[80];
    unsigned long pts, cyc;

    t->n = 0;
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f) && t->n < 400)
        if (line[0] != '#' && sscanf(line, "%lu %lu", &pts, &cyc) == 2)
            t->cyc[t->n++] = (uint32_t)cyc;
    fclose(f);
    return 0;
}

/* The trace played `loops` times through the floor and both layers, as
   vpcore does: FEEDs leave 35 ms before their picture, the chip decodes
   them in order at the clock running, a picture is late past half a
   frame; the meter read every 100 ms.  A band change costs 11 ms (the
   regulator's 10 and the lock), a clock change in a band 1 ms.  Returns
   the misses; the time-weighted clock, the band changes, the lowest band */
static uint32_t play(const struct trace *t, uint32_t frame_us, int loops,
                     uint32_t mask, enum vclk_policy pol, uint32_t *mean_mhz,
                     uint32_t *changes, int *lowest)
{
    struct vclk_video v;
    struct vclk_gov g;
    uint32_t now = 0, done = 0, misses = 0, i, total = t->n * loops;
    uint32_t lvl10, idle, busy = 0, read_at = 0;
    int band = vclk_top(mask), want;

    vclk_video_start(&v, mask, frame_us, 0);
    vclk_gov_init(&g, mask, band, 0, pol, 11, 0);
    vclk_gov_floor(&g, 0, v.floor);
    *lowest = band;
    for (i = 0; i < total; i++)
    {
        uint32_t cyc = t->cyc[i % t->n];
        uint32_t pts = 500000 + i * frame_us, release = pts - 35000;
        uint32_t start = release > done ? release : done, work;

        if (i && i % t->n == 0)
            cyc = t->cyc[1] * 2;        /* the first frame's set-up once */
        work = cyc / g.mhz;
        done = start + work;
        busy += work;
        now = done;
        if (done > pts + frame_us / 2)
        {
            misses++;
            want = vclk_gov_floor(&g, now, vclk_video_miss(&v, now, g.band));
        }
        else
            want = vclk_gov_floor(&g, now, vclk_video_feed(&v, now, work,
                                                           g.mhz));
        if (now - read_at >= 100000)
        {
            want |= vclk_gov_meter(&g, now, busy, now - read_at);
            busy = 0;
            read_at = now;
        }
        if (want)
        {
            done += g.want_band != g.band ? 11000 : 1000;
            vclk_gov_ran(&g, done, g.want_band, g.want_band, g.want_mhz);
            if (g.band < *lowest)
                *lowest = g.band;
        }
    }
    vclk_gov_account(&g, now);
    vclk_gov_means(&g, mean_mhz, &lvl10, &idle);
    *changes = g.band_changes;
    return misses;
}

static void test_video_floor(void)
{
    struct trace c1, c2;
    uint32_t mhz, ch, m, mhz_prop;
    int lowest;

    CHECK(load("tests/vclk/traces/clip1.txt", &c1) == 0 && c1.n == 240);
    CHECK(load("tests/vclk/traces/clip2.txt", &c2) == 0 && c2.n == 312);
    /* clip 1 (30 fps, 169 kbit/s): settles well below 150 with no miss;
       its I frames (1.9 M cycles) keep it at 74 MHz or more */
    m = play(&c1, 33333, 4, ALL, VCLK_POLICY_TOP, &mhz, &ch, &lowest);
    printf("     clip 1, top: %u misses, mean %u MHz, %u band changes, lowest %u MHz\n",
           m, mhz, ch, MHZ(lowest));
    CHECK(m == 0);
    CHECK(MHZ(lowest) >= 74 && MHZ(lowest) <= 102);
    CHECK(mhz < 110);
    CHECK(ch <= 10);
    /* the proportional in-band policy: no miss either, a lower clock */
    m = play(&c1, 33333, 4, ALL, VCLK_POLICY_PROP, &mhz_prop, &ch, &lowest);
    printf("     clip 1, prop: %u misses, mean %u MHz, %u band changes, lowest %u MHz\n",
           m, mhz_prop, ch, MHZ(lowest));
    CHECK(m == 0);
    CHECK(mhz_prop <= mhz);
    CHECK(ch <= 10);
    /* clip 2 (24 fps, 414 kbit/s, real footage): lower too, no miss */
    m = play(&c2, 41667, 4, ALL, VCLK_POLICY_TOP, &mhz, &ch, &lowest);
    printf("     clip 2, top: %u misses, mean %u MHz, %u band changes, lowest %u MHz\n",
           m, mhz, ch, MHZ(lowest));
    CHECK(m == 0);
    CHECK(mhz < 150 && MHZ(lowest) >= 102);
    CHECK(ch <= 8);
    m = play(&c2, 41667, 4, ALL, VCLK_POLICY_PROP, &mhz, &ch, &lowest);
    printf("     clip 2, prop: %u misses, mean %u MHz, %u band changes, lowest %u MHz\n",
           m, mhz, ch, MHZ(lowest));
    CHECK(m <= 4);              /* prop runs under the floor band's top: a few */
    /* only 150 verified (the 4b/4c flags): it never moves */
    m = play(&c1, 33333, 2, 0x1 | 0x800, VCLK_POLICY_TOP, &mhz, &ch, &lowest);
    CHECK(m == 0 && mhz == 150 && ch == 0);
    /* 87 and 150 verified: 87 is the floor clip 1 can reach */
    m = play(&c1, 33333, 4, 0x1 | 0x10 | 0x800, VCLK_POLICY_TOP, &mhz, &ch,
             &lowest);
    CHECK(m == 0 && lowest == 4);
}

static void test_video_trouble(void)
{
    struct vclk_video v;
    uint32_t t = 0, i;
    int b;

    vclk_video_start(&v, ALL, 33333, 0);
    CHECK(v.floor == VCLK_TOP);
    /* steady light frames: 1.0 M cycles, after 3 s straight to the lowest
       band that fits: 46 MHz (21.7 ms of 33.3, 65 %); 27 would be 37 ms */
    for (i = 0; i < 100; i++, t += 33333)
        b = vclk_video_feed(&v, t, 1000000 / 150, 150);
    CHECK(b == 1);
    /* a miss at 46: a band above, and 46 now waits 6 s */
    b = vclk_video_miss(&v, t, 1);
    CHECK(b == 2 && v.hold[1] == 1);
    for (i = 0; i < 100; i++, t += 33333)   /* 3.3 s: not yet */
        b = vclk_video_feed(&v, t, 1000000 / 59, 59);
    CHECK(b == 2);
    for (i = 0; i < 100; i++, t += 33333)   /* 6 s since: back to 46 */
        b = vclk_video_feed(&v, t, 1000000 / 59, 59);
    CHECK(b == 1);
    /* misses at the top band keep the top */
    v.floor = VCLK_TOP;
    CHECK(vclk_video_miss(&v, t, VCLK_TOP) == VCLK_TOP);
    /* a seek races to the top */
    v.floor = 3;
    CHECK(vclk_video_race(&v, t) == VCLK_TOP);
    /* a spike in the window keeps the floor where it is */
    vclk_video_start(&v, ALL, 33333, t);
    for (i = 0; i < 100; i++, t += 33333)
        b = vclk_video_feed(&v, t, i == 50 ? 31000 : 7000, 150);
    CHECK(b == VCLK_TOP - 0 || MHZ(b) >= 140);
}

/* the meter for a load of `load` MHz of work at the clock running */
static int meter(struct vclk_gov *g, uint32_t *t, uint32_t load)
{
    uint32_t busy = load >= g->mhz ? 100000 : 100000 * load / g->mhz;

    *t += 100000;
    return vclk_gov_meter(g, *t, busy, 100000);
}

static void ran(struct vclk_gov *g, uint32_t t)
{
    vclk_gov_ran(g, t, g->want_band, g->want_band, g->want_mhz);
}

/* layer 1: the band selector */
static void test_band_selector(void)
{
    struct vclk_gov g;
    uint32_t t = 0, i, c0;
    char line[160];

    vclk_gov_init(&g, ALL, 0, 0, VCLK_POLICY_TOP, 11, 0);
    CHECK(g.mhz == 27);
    /* the crystal saturated: up one band at a time, each at once */
    for (i = 0; i < 20 && g.band < VCLK_TOP; i++)
        if (meter(&g, &t, 200))
            ran(&g, t);
    CHECK(g.band == VCLK_TOP && g.mhz == 150);
    CHECK(g.band_changes == VCLK_TOP);       /* one band a step */
    vclk_band_line(&g, line, sizeof(line));
    CHECK(PFX(line, "vclk band from=147 to=150 lvl=15 why=up util="));
    /* 60 MHz of work: down while the band below carries it at 80 %
       (74 x 0.8 = 59.2 no; 87 x 0.8 = 69.6 yes): rests at 87 */
    for (i = 0; i < 60; i++)
        if (meter(&g, &t, 60))
            ran(&g, t);
    CHECK(g.band == 4 && g.mhz == 87);
    vclk_band_line(&g, line, sizeof(line));
    CHECK(strstr(line, "why=down") != NULL);
    /* the dwell: never two bands down within 100 ms */
    vclk_gov_init(&g, ALL, VCLK_TOP, 0, VCLK_POLICY_TOP, 11, t = 0);
    g.util = 0;
    CHECK(meter(&g, &t, 0) == 1 && g.want_band == VCLK_TOP - 1);
    ran(&g, t);
    t += 50000 - 100000;
    CHECK(meter(&g, &t, 0) == 0);           /* 50 ms later: held */
    CHECK(meter(&g, &t, 0) == 1);           /* 150 ms: down again */
    /* hysteresis: a load between the two thresholds holds the band */
    vclk_gov_init(&g, ALL, 4, 0, VCLK_POLICY_TOP, 11, t = 0);
    c0 = g.band_changes;
    for (i = 0; i < 100; i++)
        if (meter(&g, &t, i & 1 ? 81 : 62))  /* 93 % and 71 % of 87 */
            ran(&g, t);
    CHECK(g.band == 4 && g.band_changes == c0);
    /* a client's floor forces a minimum band, at once and both ways */
    CHECK(vclk_gov_floor(&g, t, 9) == 1 && g.want_band == 9 &&
          g.why == VCLK_WHY_FLOOR);
    ran(&g, t);
    CHECK(meter(&g, &t, 0) == 0 && g.band == 9);    /* idle: still 9 */
    /* released: down one band (the dwell has passed) */
    CHECK(vclk_gov_floor(&g, t, -1) == 1 && g.want_band == 8 &&
          g.why == VCLK_WHY_DOWN);
    ran(&g, t);
    /* an unverified floor: the nearest verified band above */
    vclk_gov_init(&g, 0x1 | 0x80 | 0x800, 0, 0, VCLK_POLICY_TOP, 11, t);
    CHECK(vclk_gov_floor(&g, t, 5) == 1 && g.want_band == 7);
    ran(&g, t);
    /* a burst: the top for its run, then back to the floor */
    CHECK(vclk_gov_burst(&g, t, true) == 1 && g.want_band == VCLK_TOP &&
          g.why == VCLK_WHY_BURST);
    ran(&g, t);
    CHECK(vclk_gov_burst(&g, t, false) == 0);       /* the dwell holds */
    t += 200000;
    CHECK(meter(&g, &t, 0) == 1 && g.want_band == 7);
    ran(&g, t);
    /* a failed switch: the crystal, the band dropped, the line says so */
    g.mask = ALL;
    CHECK(vclk_gov_floor(&g, t, 9) == 1 && g.want_band == 9);
    vclk_gov_ran(&g, t, 9, 0, 27);
    CHECK(g.band == 0 && !(g.mask >> 9 & 1) && g.fails == 1);
    vclk_band_line(&g, line, sizeof(line));
    CHECK(PFX(line, "vclk band from=140 to=27 lvl=11 why=fail"));
    CHECK(vclk_gov_floor(&g, t, 9) == 1 && g.want_band == 10);
}

/* layer 2: the in-band policy */
static void test_in_band(void)
{
    struct vclk_gov g;
    uint32_t t = 0, i, c0;

    /* top-of-band: always the band's top clock */
    vclk_gov_init(&g, ALL, 4, 0, VCLK_POLICY_TOP, 11, 0);
    for (i = 0; i < 10; i++)
        if (meter(&g, &t, 76))
            ran(&g, t);
    CHECK(g.band == 4 && g.mhz == 87);
    /* proportional: load / 0.8 inside the band (above 74, at most 87) */
    vclk_gov_init(&g, ALL, 4, 0, VCLK_POLICY_PROP, 11, t = 0);
    for (i = 0; i < 20; i++)
        if (meter(&g, &t, 61))              /* 61 / 0.8 = 76.25 */
            ran(&g, t);
    CHECK(g.band == 4 && g.mhz == 77);
    /* a light load: the band's bottom clock, not below (the band selector
       takes it down a band instead) */
    vclk_gov_init(&g, ALL, 4, 0, VCLK_POLICY_PROP, 11, t = 0);
    g.changed_us = 0;
    for (i = 0; i < 3; i++)
        if (meter(&g, &t, 62))              /* 77.5 MHz; 74 x 0.8 = 59 */
            ran(&g, t);
    CHECK(g.band == 4 && g.mhz >= 75 && g.mhz <= 80);
    /* down only by 10 % or more: no relock for a few MHz */
    c0 = g.clock_changes;
    for (i = 0; i < 30; i++)
        if (meter(&g, &t, i & 1 ? 63 : 61))
            ran(&g, t);
    CHECK(g.clock_changes - c0 <= 1);
    /* saturated in the band: its top first (no level change), then up */
    vclk_gov_init(&g, ALL, 4, 80, VCLK_POLICY_PROP, 11, t = 0);
    CHECK(meter(&g, &t, 200) == 1 && g.want_band == 4 && g.want_mhz == 87);
    ran(&g, t);
    for (i = 0; i < 5 && g.band == 4; i++)
        if (meter(&g, &t, 200))
            ran(&g, t);
    CHECK(g.band == 5 && g.mhz == 102);     /* a band up: its top */
    /* the crystal has one clock */
    vclk_gov_init(&g, ALL, 0, 0, VCLK_POLICY_PROP, 11, t = 0);
    CHECK(meter(&g, &t, 5) == 0 && g.mhz == 27);
}

static void test_lines(void)
{
    struct vclk_gov g;
    uint32_t mhz, lvl10, idle;
    char line[200];

    vclk_gov_init(&g, ALL, VCLK_TOP, 0, VCLK_POLICY_TOP, 11, 1000);
    vclk_gov_meter(&g, 1001000, 250000, 1000000);
    vclk_gov_ran(&g, 1001000, 4, 4, 87);
    vclk_gov_meter(&g, 3001000, 500000, 2000000);
    vclk_gov_means(&g, &mhz, &lvl10, &idle);
    /* 1 s at 150, 2 s at 87: (150 + 174) / 3 = 108; level (15 + 16) / 3 */
    CHECK(mhz == 108);
    CHECK(lvl10 == 103);
    CHECK(idle == 750);
    vclk_sum_line(&g, 2, line, sizeof(line));
    CHECK(!strcmp(line, "vclk sum policy=top mhz=108 lvl=10.3 idle=750 "
                        "bands=1 clocks=1 lvl_changes=1 fails=0 misses=2 s=3"));
    vclk_time_line(&g, line, sizeof(line));
    CHECK(!strcmp(line, "vclk time 87=2000 150=1000 ms"));
    g.policy = VCLK_POLICY_PROP;
    vclk_sum_line(&g, 0, line, sizeof(line));
    CHECK(PFX(line, "vclk sum policy=prop "));
}

static void test_flags(void)
{
    char text[200], line[40];
    int k;

    text[0] = 0;
    CHECK(vclk_flags_mask("", 1, 2) == 1);
    vclk_flag_line(line, sizeof(line), 4, 0x35624, 0xcd8e5d96);
    CHECK(!strcmp(line, "vc87 00035624 cd8e5d96\n"));
    k = 0;
    k += vclk_flag_line(text + k, sizeof(text) - k, 4, 0x35624, 0xcd8e5d96);
    k += vclk_flag_line(text + k, sizeof(text) - k, 7, 0x35624, 0xcd8e5d96);
    k += vclk_flag_line(text + k, sizeof(text) - k, 9, 0x35624, 0x11111111);
    snprintf(text + k, sizeof(text) - k, "vc100 00035624 cd8e5d96\njunk\n");
    /* 87 and 123 name this image; 140 another; 100 is no band */
    CHECK(vclk_flags_mask(text, 0x35624, 0xcd8e5d96) == (0x1 | 0x10 | 0x80));
    CHECK(vclk_flags_mask(text, 0x35628, 0xcd8e5d96) == 1);
    CHECK(vclk_flags_mask("vc87 00035624 cd8e5d9\n", 0x35624, 0xcd8e5d9) == 1);
}

int main(void)
{
    test_bands();
    test_video_floor();
    test_video_trouble();
    test_band_selector();
    test_in_band();
    test_lines();
    test_flags();
    if (fails)
        fprintf(stderr, "%d failed\n", fails);
    return fails != 0;
}
