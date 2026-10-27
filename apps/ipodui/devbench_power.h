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

#ifndef _IPODUI_DEVBENCH_POWER_H_
#define _IPODUI_DEVBENCH_POWER_H_

/* p19-battery: the bench session's three battery levers as viability
 * rows (docs/research/bcm2722/battery-levers.md; bench-p19.md "The
 * battery rows"): does the lever work, and does it keep things smooth.
 * No power claims and no meter (owner, 2026-10-25): each runs four fixed
 * phases, A B A B, on battery, a power.csv mark at each (the voltage
 * trend is a log, not a verdict), with the screen still, and passes or
 * fails on its own read-backs; lit24's smoothness is the bench step
 * after it (lit24ui), judged in the menus.
 *
 *   slp    the resident image's idle sleep (stage 5i's gate) off / on
 *   lvl5   its core regulator at the level found (11) / at 5 (PW3)
 *   lit24  the PP's lit clock at 30 / 24 MHz
 *
 * This header is the pure part (phase plan, verdicts), host-tested in
 * tests/autochecks; devbench_power.c is the device side. */

#include <stdbool.h>
#include <stdint.h>

#define BPW_PHASES      4       /* A B A B */
#define BPW_PHASE_S     20      /* each: enough ticks and self-tests */

static inline bool bpw_phase_b(int k)
{
    return k & 1;
}

/* The idle tick: the image's timer compare 1 every 1000 us.  A B phase
 * proves the sleeping core wakes on it (open question #3) when its tick
 * count is within 10 % of the milliseconds it lasted, it slept at least
 * once, and no sleep disturbed r0-r5 */
static inline bool bpw_idle_ok(uint32_t ticks, uint32_t sleeps, uint32_t bad,
                               uint32_t ms)
{
    return ms > 0 && ticks * 10ull >= ms * 9ull && ticks * 10ull <= ms * 11ull
           && sleeps > 0 && bad == 0;
}

/* The lit phase reached what it asked: 24 MHz in B (every hold off), 30
 * or more in A.  mhz: the unboosted clock read at the phase's end */
static inline bool bpw_lit_ok(bool b, long mhz)
{
    return b ? mhz == 24 : mhz >= 30;
}

/* ---- slp: lost doorbells ---- */
/* In a B phase the PP rings the doorbell 'rings' times; the image's bells
 * counter moved by 'bells' (others' rings only add): any shortfall is a
 * lost doorbell */
static inline uint32_t bpw_bells_lost(uint32_t rings, uint32_t bells)
{
    return rings > bells ? rings - bells : 0;
}

static inline bool bpw_bells_ok(uint32_t rings, uint32_t bells, uint32_t bad)
{
    return rings > 0 && bpw_bells_lost(rings, bells) == 0 && bad == 0;
}

/* ---- slp: doorbell wakes, by each FENCE's completion time ----
 * The sleeping image also wakes on its 1 ms tick, so a lost doorbell
 * shows only as latency: woken by the bell, a FENCE completes in tens of
 * us (as in the A phases, where the chip polls); bell-blind, it waits
 * for the tick, uniform over 0-1000 us (about a quarter under 250 us).
 * A B phase proves the bell wakes the core when at least 80 % of its
 * FENCEs complete under BPW_LAT_UNDER_US, and its median is within
 * BPW_LAT_SLACK_US of the A phases' (polling) median */
#define BPW_LAT_UNDER_US    250
#define BPW_LAT_SLACK_US    200
#define BPW_LAT_MAX         128     /* timed FENCEs kept a phase */
#define BPW_LAT_MIN         8       /* fewer: nothing proven */

/* the q-th quantile (0-100) of n samples, sorted in place */
static inline uint32_t bpw_quantile(uint16_t *v, uint32_t n, uint32_t q)
{
    uint32_t i, j;

    if (!n)
        return 0;
    for (i = 1; i < n; i++)         /* insertion sort: n <= BPW_LAT_MAX */
    {
        uint16_t x = v[i];

        for (j = i; j > 0 && v[j - 1] > x; j--)
            v[j] = v[j - 1];
        v[j] = x;
    }
    return v[(n - 1) * q / 100];
}

static inline bool bpw_lat_ok(uint32_t n, uint32_t under, uint32_t p50_b,
                              uint32_t p50_a)
{
    return n >= BPW_LAT_MIN && under * 10 >= n * 8 &&
           p50_b <= p50_a + BPW_LAT_SLACK_US;
}

/* ---- lit24ui: missed frames (litmiss.h counts them) ---- */
/* The step's rule (pure; host-tested in tests/autochecks): per 1000
 * frames, B may miss at most BPW_MISS_MARGIN_PM more than A, and each
 * phase needs enough frames to mean anything (the owner did the moves) */
#define BPW_MISS_MARGIN_PM   20
#define BPW_MISS_MIN_DRIFT   30
#define BPW_MISS_MIN_SCROLL  10

static inline bool bpw_miss_enough(uint32_t drift_frames,
                                  uint32_t scroll_frames)
{
    return drift_frames >= BPW_MISS_MIN_DRIFT &&
           scroll_frames >= BPW_MISS_MIN_SCROLL;
}

/* one kind (drift or scroll): B's miss rate against A's, per 1000 */
static inline bool bpw_miss_kind_ok(uint32_t a_miss, uint32_t a_frames,
                                   uint32_t b_miss, uint32_t b_frames)
{
    uint64_t a = a_frames ? a_miss * 1000ull / a_frames : 0;
    uint64_t b = b_frames ? b_miss * 1000ull / b_frames : 0;

    return b <= a + BPW_MISS_MARGIN_PM;
}


/* devbench_power.c (target, the resident image's builds): each returns
 * an enum bench_res */
int devbench_pwr_idle(void);
int devbench_pwr_level(void);
int devbench_pwr_lit(void);

#endif /* _IPODUI_DEVBENCH_POWER_H_ */
