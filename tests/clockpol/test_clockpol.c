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
 * Host test of the 24 MHz idle policy's decisions (Fit-1 C6,
 * apps/ipodui/clockpol_core.c): which tracks fit, the per-minute verdict
 * (underruns, boost rate, the window length) and strikes.
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
#include "ipodui/clockpol_core.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

static struct cp_track flac44(void)
{
    struct cp_track t;
    memset(&t, 0, sizeof(t));
    t.fmt = 7;                  /* any id */
    t.light = true;
    t.rate = 44100;
    t.speed_1x = true;
    t.dsp_heavy = false;
    return t;
}

static void test_fits(void)
{
    struct cp_state s;
    struct cp_track t;

    memset(&s, 0, sizeof(s));
    t = flac44();
    CHECK(cp_fits(&s, &t));
    t.rate = 48000;
    CHECK(cp_fits(&s, &t));             /* the limit itself fits */
    t.rate = 96000;
    CHECK(!cp_fits(&s, &t));            /* hi-res: 30 MHz */
    t = flac44();
    t.light = false;
    CHECK(!cp_fits(&s, &t));            /* AAC, Vorbis, Opus...: 30 MHz */
    t = flac44();
    t.speed_1x = false;
    CHECK(!cp_fits(&s, &t));            /* an audiobook at 1.5x */
    t = flac44();
    t.dsp_heavy = true;
    CHECK(!cp_fits(&s, &t));            /* software EQ or crossfeed */
}

static void test_judge(void)
{
    /* an underrun strikes at once, however short the window */
    CHECK(cp_judge(0, 0, 1) == CP_STRIKE);
    CHECK(cp_judge(5, 0, 2) == CP_STRIKE);
    /* under a minute: wait, even with boosts */
    CHECK(cp_judge(CP_WINDOW_S - 1, 100, 0) == CP_WAIT);
    /* a minute: up to CP_MAX_BOOSTS_MIN boosts is fine (a refill) */
    CHECK(cp_judge(60, 0, 0) == CP_OK);
    CHECK(cp_judge(60, CP_MAX_BOOSTS_MIN, 0) == CP_OK);
    CHECK(cp_judge(60, CP_MAX_BOOSTS_MIN + 1, 0) == CP_STRIKE);
    /* the rate, not the count, over a long window (the UI slept) */
    CHECK(cp_judge(600, 10 * CP_MAX_BOOSTS_MIN, 0) == CP_OK);
    CHECK(cp_judge(600, 10 * CP_MAX_BOOSTS_MIN + 1, 0) == CP_STRIKE);
}

static void test_strikes(void)
{
    struct cp_state s;
    struct cp_track t = flac44();
    struct cp_track u = flac44();

    memset(&s, 0, sizeof(s));
    u.fmt = 3;
    cp_strike(&s, t.fmt);
    CHECK(!cp_fits(&s, &t));            /* struck for the session */
    CHECK(cp_fits(&s, &u));             /* another format is not */
    cp_strike(&s, -1);                  /* out of range: ignored */
    cp_strike(&s, CP_FMT_BITS);
    CHECK(cp_fits(&s, &u));
    u.fmt = CP_FMT_BITS + 5;            /* untracked ids still fit */
    CHECK(cp_fits(&s, &u));
    u.fmt = CP_FMT_BITS - 1;            /* the top bit works */
    cp_strike(&s, u.fmt);
    CHECK(!cp_fits(&s, &u));
}

/* p19-battery: the lit clock's need and its 2 s release */
static void test_lit(void)
{
    struct cp_lit l = { true, 0 };
    const long rel = 200;               /* 2 s of 10 ms ticks */
    int sw, st, sk, pl;

    for (sw = 0; sw < 2; sw++)
        for (st = 0; st < 2; st++)
            for (sk = 0; sk < 2; sk++)
                for (pl = 0; pl < 2; pl++)
                    CHECK(cp_lit_need(sw, st, sk, pl) ==
                          (sw || st || (sk && pl)));
    /* the default (switch at 30) always holds */
    CHECK(cp_lit_need(true, false, false, false));
    /* a strike with nothing playing does not hold */
    CHECK(!cp_lit_need(false, false, true, false));

    /* starts held; released exactly at 2 s without a need */
    CHECK(cp_lit_step(&l, false, 0, rel));
    CHECK(cp_lit_step(&l, false, 199, rel));
    CHECK(!cp_lit_step(&l, false, 200, rel));
    /* taken at once */
    CHECK(cp_lit_step(&l, true, 300, rel));
    /* a 1.9 s gap never releases; the stream's holds re-arm it */
    CHECK(cp_lit_step(&l, false, 490, rel));
    CHECK(cp_lit_step(&l, true, 495, rel));
    CHECK(cp_lit_step(&l, false, 690, rel));
    CHECK(!cp_lit_step(&l, false, 695, rel));
    /* released stays released */
    CHECK(!cp_lit_step(&l, false, 5000, rel));
    /* tick wrap: long arithmetic from a negative start */
    l.need = false;
    CHECK(cp_lit_step(&l, true, -100, rel));
    CHECK(cp_lit_step(&l, false, 99, rel));
    CHECK(!cp_lit_step(&l, false, 100, rel));
}

/* np-lag: the input boost's condition, and its hold (cp_lit_step) */
static void test_input_boost(void)
{
    struct cp_lit h = { false, 0 };
    const long hold = 100;

    CHECK(cp_input_boost(true, true, true));
    CHECK(!cp_input_boost(false, true, true));  /* dark: no boost */
    CHECK(!cp_input_boost(true, false, true));  /* paused or stopped */
    CHECK(!cp_input_boost(true, true, false));  /* codec on the other core */
    CHECK(!cp_input_boost(false, false, false));
    /* starts released (unlike the lit clock's hold) */
    CHECK(!cp_lit_step(&h, false, 0, hold));
    CHECK(cp_lit_step(&h, true, 10, hold));
    CHECK(cp_lit_step(&h, false, 109, hold));
    CHECK(cp_lit_step(&h, true, 105, hold));    /* a second input re-arms */
    CHECK(cp_lit_step(&h, false, 204, hold));
    CHECK(!cp_lit_step(&h, false, 205, hold));
}

int main(void)
{
    test_fits();
    test_judge();
    test_strikes();
    test_lit();
    test_input_boost();
    if (fails)
        fprintf(stderr, "%d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
