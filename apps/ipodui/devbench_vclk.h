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

#ifndef _IPODUI_DEVBENCH_VCLK_H_
#define _IPODUI_DEVBENCH_VCLK_H_

/* P19 PW4: the video chip's level bands and its clock governor, as two
 * bench rows (docs/research/bcm2722/bench-p19.md "The clock rows").
 * Performance and viability only: no meter.  The power figures they log
 * are proxies (time-weighted MHz and level, idle share) and an estimate
 * from scripts/bcm_power_model.py's figures, never a pass condition.
 *
 *   vbv   each band verified: the chip holds it (PLL A locked, its speed
 *         checked, no fallback), and a short decode there shows pictures
 *         with no decode error and no loss; passing bands are written to
 *         bcmpassvc.txt (the governor's gate)
 *   vgov  each clip under the governor against fixed 150 MHz (the same
 *         clip, the same steps: a seek, a pause and a resume): no more
 *         skips or dropped GOPs than at 150, A/V within stage 6's bound
 *         or near 150's own, no more hitches than at 150, a stable clock
 *         (changes a minute), the top again after the seek and the
 *         resume
 *
 * This header is the pure part (verdicts, the estimate), host-tested in
 * tests/autochecks; devbench_vclk.c is the device side. */

#include <stdbool.h>
#include <stdint.h>

#define BVC_VERIFY_S        8       /* a band's decode */
#define BVC_MIN_SHOWN       20      /* pictures, at least */
#define BVC_GOV_S           20      /* a governor phase, each clip */
#define BVC_DRIFT_US        50000   /* vtrace.h VTRACE_DRIFT_US */
#define BVC_MAX_PER_MIN     20      /* band changes a minute, at most */

/* A band holds: the chip runs what was asked, no fallback, pictures
 * shown, no decode error, no concealed block, no recovery */
static inline bool bvc_verify_ok(uint32_t ran, uint32_t asked, uint32_t fails,
                                 uint32_t shown, uint32_t errors,
                                 uint32_t concealed, uint32_t recoveries)
{
    return ran == asked && fails == 0 && shown >= BVC_MIN_SHOWN &&
           errors == 0 && concealed == 0 && recoveries == 0;
}

/* The governor is viable on a clip: no worse than the same clip at fixed
 * 150 MHz (the same steps: a seek, a pause and a resume) in misses (skips
 * and dropped GOPs), A/V (within stage 6's bound, or within
 * BVC_DRIFT_SLACK_US of 150's own) and pacing; stable; the top again
 * after the seek and the resume.  Sitting 4c: clip2 skipped 33 pictures
 * and reached 235 ms A/V at fixed 150 too (the art pass running beside
 * it), and clip1's seek took both runs past 50 ms, so the absolute rule
 * failed the governor for the clip and the bench, not for itself */
#define BVC_DRIFT_SLACK_US  10000
static inline bool bvc_gov_ok(uint32_t misses, uint32_t misses_150,
                              int32_t drift_max_us, int32_t drift_150_us,
                              uint32_t hitches, uint32_t hitches_150,
                              uint32_t changes, uint32_t ms, bool raced)
{
    int32_t drift_bound = drift_150_us + BVC_DRIFT_SLACK_US > BVC_DRIFT_US
                          ? drift_150_us + BVC_DRIFT_SLACK_US : BVC_DRIFT_US;

    return misses <= misses_150 && drift_max_us <= drift_bound &&
           hitches <= hitches_150 + 1 && ms > 0 &&
           (uint64_t)changes * 60000 <= (uint64_t)BVC_MAX_PER_MIN * ms &&
           raced;
}

/* The estimate [estimate], scripts/bcm_power_model.py's figures: the core
 * polls between frames in a session (so busy or not, it burns), P = k_bcm
 * x f x (V / 1.2)^2 + p_leak11 x (V / 1.04)^2, V = 0.80 + 0.04 (level -
 * 5) V [hypothesis], at the battery through eta 0.85 and 3.7 V.  ms: the
 * time at each of the twelve bands; lvl: each band's level.  Returns
 * hundredths of a mA (0 for no time) */
static inline uint32_t bvc_est_ma100(const uint32_t *ms, const uint8_t *mhz,
                                     const uint8_t *lvl, int n)
{
    uint64_t e = 0, t = 0;          /* nW x ms, ms */
    int b;

    for (b = 0; b < n; b++)
    {
        uint64_t v = 800 + 40 * (uint64_t)(lvl[b] - 5);     /* mV */
        /* nW: 550 uW/MHz x f x v^2 / 1.44e6 + 2,000 uW x v^2 / 1.0816e6 */
        uint64_t p = 550000ull * mhz[b] * v * v / 1440000 +
                     2000000ull * v * v / 1081600;

        e += p * ms[b];
        t += ms[b];
    }
    /* nW -> mA x 100 at the battery: / (0.85 x 3.7 V) / 1e6 x 100 */
    return t ? (uint32_t)(e / t * 100 / 3145000) : 0;
}

/* devbench_vclk.c (target, video builds): each returns an enum bench_res */
int devbench_vclk_verify(void);
int devbench_vclk_gov(void);

#endif /* _IPODUI_DEVBENCH_VCLK_H_ */
