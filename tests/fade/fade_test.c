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
 * Host test for the pause/resume fade curve (apps/ipodui/fadecurve.h,
 * used by apps/pcmbuf.c; docs/phase15-brief.md sections 3.3, 7): the dB
 * table (monotonic, 25 steps of 2.5 dB to -60 dB, then mute) and the
 * index arithmetic, including a fade reversed mid-way.
 *
 *   cc -Iapps tests/fade/fade_test.c
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
#include <math.h>
#include <stdio.h>
#include "ipodui/fadecurve.h"

#define UNITY 0x10000   /* MIX_AMP_UNITY (firmware/export/pcm_mixer.h) */

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; \
    printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* pcmbuf_fade_tick()'s loop, until done (amplitude at an end) */
static int run(int *idx, int in, int max_ticks)
{
    int t = 0;
    while (t < max_ticks)
    {
        *idx = fade_next(*idx, in);
        t++;
        if (fade_amp[*idx] == 0 || fade_amp[*idx] == UNITY)
            break;
    }
    return t;
}

int main(void)
{
    int i, idx, t;

    CHECK(sizeof fade_amp / sizeof fade_amp[0] == FADE_STEPS + 1,
          "26 entries");
    CHECK(FADE_STEPS == 25, "25 ticks = 250 ms at HZ 100");
    CHECK(fade_amp[0] == UNITY, "index 0 is unity");
    CHECK(fade_amp[FADE_STEPS] == 0, "last index is mute");
    for (i = 0; i < FADE_STEPS; i++)
    {
        double want = UNITY * pow(10.0, -2.5 * i / 20.0);
        CHECK(fabs(fade_amp[i] - want) <= 0.5 + 1e-9,
              "index %d: %u, want %.1f (%.1f dB)", i, fade_amp[i], want,
              -2.5 * i);
        CHECK(fade_amp[i + 1] < fade_amp[i], "index %d not decreasing", i);
    }
    CHECK(fabs(20 * log10(fade_amp[24] / (double)UNITY) + 60.0) < 0.1,
          "index 24 is -60 dB (within rounding)");

    /* full fades: 25 ticks each way */
    idx = 0;
    CHECK(fade_ticks_left(idx, 0) == 25, "out from unity: 25 left");
    t = run(&idx, 0, 100);
    CHECK(t == 25 && idx == FADE_STEPS, "out took %d ticks", t);
    CHECK(fade_ticks_left(idx, 1) == 25, "in from mute: 25 left");
    t = run(&idx, 1, 100);
    CHECK(t == 25 && idx == 0, "in took %d ticks", t);

    /* reversal: pause, 10 ticks of fade-out, then resume: the fade-in
     * starts at index 10 (-25 dB) and takes 10 ticks, not 25 */
    idx = 0;
    run(&idx, 0, 10);
    CHECK(idx == 10, "after 10 ticks out: index %d", idx);
    CHECK(fade_ticks_left(idx, 1) == 10, "reversed: %d left, want 10",
          fade_ticks_left(idx, 1));
    t = run(&idx, 1, 100);
    CHECK(t == 10 && idx == 0, "reversed fade-in took %d ticks", t);

    /* and the other way: resume, 5 ticks in, pause again */
    idx = FADE_STEPS;
    run(&idx, 1, 5);
    CHECK(idx == 20 && fade_ticks_left(idx, 0) == 5,
          "fade-out after 5 ticks in: index %d", idx);

    /* the ends hold */
    CHECK(fade_next(0, 1) == 0 && fade_next(FADE_STEPS, 0) == FADE_STEPS,
          "ends clamp");

    /* soft_mode (pcmbuf_update_volume(): vol >> 2) keeps the curve's dB
     * steps: -12 dB on every entry, within rounding */
    for (i = 0; i < 20; i++)
        CHECK(fabs(20 * log10((fade_amp[i] >> 2) / (double)fade_amp[i]) +
                   12.04) < 0.2, "soft mode at index %d", i);

    if (!fails)
        printf("ok   fade: 25 x 2.5 dB to -60 dB then mute, 250 ms, "
               "reversal continues from the index\n");
    return fails != 0;
}
