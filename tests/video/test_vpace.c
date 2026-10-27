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

/* The pace row's arithmetic (apps/video/vtrace.c vtrace_work_pct,
 * vtrace_frame_us, vtrace_pace_ok; devbench "vfeed"): percentiles, the
 * frame time from PTS and shown counts, and the rule p95 < 75 % of the
 * frame with nothing skipped at the wanted clock. */
#include <stdio.h>
#include <string.h>
#include "vtrace.h"

static int fails, n;

static void check(int ok, const char *what)
{
    n++;
    if (!ok)
        fails++;
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

static struct vtrace_rec rec[400];

/* k pictures at fps, FEED times base + (i % 20) * step us, at mhz */
static void fill(struct vtrace *t, int k, int fps, uint32_t base,
                 uint32_t step, uint16_t mhz)
{
    int i;

    vtrace_init(t, rec, sizeof(rec) / sizeof(rec[0]));
    for (i = 0; i < k; i++)
    {
        struct vtrace_rec *r = &rec[t->n++];

        r->pts_ms = (uint32_t)((long long)i * 1000 / fps);
        r->drift_us = 5000;
        r->work_us = base + (uint32_t)(i % 20) * step;
        r->mhz = mhz;
        r->shown = (uint16_t)(i + 1);
    }
}

int main(void)
{
    struct vtrace t;
    struct vtrace_sum s;
    uint32_t p95, fr;

    fill(&t, 0, 24, 0, 0, 150);
    check(vtrace_work_pct(&t, 95) == 0 && vtrace_frame_us(&t) == 0,
          "no records: no percentile, no frame time");

    /* 24 fps, FEEDs 10..29.5 ms (sitting 2's Sintel clip: p95 27.4 ms) */
    fill(&t, 240, 24, 10000, 1000, 150);
    p95 = vtrace_work_pct(&t, 95);
    fr = vtrace_frame_us(&t);
    check(p95 == 28000, "p95 of 10..29 ms in 1 ms steps is 28 ms");
    check(vtrace_work_pct(&t, 100) == 29000, "p100 is the longest");
    check(vtrace_work_pct(&t, 50) == 19000, "p50");
    check(fr >= 41600 && fr <= 41700, "24 fps: a 41.7 ms frame");
    vtrace_summary(&t, true, false, &s);
    s.mhz_lo = s.mhz_hi = 150;
    check(vtrace_pace_ok(&s, p95, fr, 150), "24 fps at 150 MHz: p95 28 < 31.25 ms passes");
    check(!vtrace_pace_ok(&s, p95, fr, 27), "the wrong clock fails");
    s.skipped = 1;
    check(!vtrace_pace_ok(&s, p95, fr, 150), "a skipped picture fails");
    s.skipped = 0;
    s.pass = false;
    check(!vtrace_pace_ok(&s, p95, fr, 150), "a failed summary (A/V out of bounds) fails");

    /* the same FEEDs at 30 fps: 28 ms is over 75 % of 33.3 ms */
    fill(&t, 300, 30, 10000, 1000, 150);
    p95 = vtrace_work_pct(&t, 95);
    fr = vtrace_frame_us(&t);
    vtrace_summary(&t, true, false, &s);
    check(fr >= 33300 && fr <= 33400, "30 fps: a 33.3 ms frame");
    check(!vtrace_pace_ok(&s, p95, fr, 150), "30 fps, p95 28 ms: over 25 ms, fails");
    /* light: clip 1 at 7 ms */
    fill(&t, 300, 30, 6000, 100, 150);
    vtrace_summary(&t, true, false, &s);
    check(vtrace_pace_ok(&s, vtrace_work_pct(&t, 95), vtrace_frame_us(&t), 150),
          "30 fps, 6-8 ms FEEDs: passes");
    printf("test_vpace: %d of %d passed\n", n - fails, n);
    return fails != 0;
}
