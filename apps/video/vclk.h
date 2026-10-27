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

/* The video chip's clock governor, the host's half (P19 PW4;
   docs/research/bcm2722/battery-levers.md section 4, power-contract.md
   section 8).

   The chip (bcmfw/clock.c, HP_OP_VCLK) runs one of twelve level bands (the
   crystal, 27 MHz, or one of Apple's supply-level bands at its level, named
   by its top whole MHz) at a clock inside it, switched in Apple's order
   with its speed checked, and keeps a busy meter.  The governor lives
   here, because the merged image has no room for it (battery-levers.md
   section 4).  Two layers (chosen in scripts/vclk_gov_sim.py):

     1. the band selector (select_band): the supply level, from the load
        and the clients' floors (below);
     2. the in-band policy (select_mhz): the clock inside the band, top of
        band (the default) or proportional.

   The clients' floors act on layer 1: the video session's (struct
   vclk_video: it starts at the top; after a steady 3 s it drops to the
   lowest band whose scaled FEED p95 keeps 25 % of the frame and whose
   scaled longest FEED keeps 10 %; a miss puts it a band above the one that
   missed, and that band then waits twice as long; a seek, a resume or a
   wake puts it back at the top), and a burst's full speed (a cover
   DECODE).

   Only verified bands are used (bit b of the mask; the crystal always):
   until a band is verified, the nearest verified one above it serves.

   Pure logic, no Rockbox calls: vpcore.c calls it, tests/vclk tests it. */

#ifndef VCLK_H
#define VCLK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VCLK_BANDS          12
#define VCLK_TOP            (VCLK_BANDS - 1)    /* 150 MHz */
extern const uint8_t vclk_band_mhz[VCLK_BANDS];
extern const uint8_t vclk_band_level[VCLK_BANDS];  /* 0: the crystal's */

/* Apple's 13-row table (@141ba): the lowest level for a clock in Hz */
unsigned vclk_level_for_hz(uint32_t hz);
/* The lowest verified band at or above mhz (the top verified one if none) */
int vclk_band_for(uint32_t mask, uint32_t mhz);
int vclk_top(uint32_t mask);

/* The band-verify flags (bench row vbv; bcmpassvc.txt): a line a band,
   "vc<MHz> <bytes> <fnv>" (eight hex digits each) naming the player's
   image.  The mask of the bands whose line names this image (bit 0, the
   crystal, always); 150 MHz is the 4b and 4c flags' (the caller's) */
uint32_t vclk_flags_mask(const char *text, uint32_t bytes, uint32_t fnv);
int vclk_flag_line(char *buf, size_t n, int band, uint32_t bytes,
                   uint32_t fnv);

/* ---- the video session's floor ---------------------------------------- */

#define VCLK_STEADY_US      3000000     /* one GOP of prokbox-convert's */
#define VCLK_MIN_SAMPLES    30
#define VCLK_MARGIN_PCT     25          /* p95, scaled, <= 75 % of a frame */
#define VCLK_SPIKE_PCT      90          /* the longest, scaled, <= 90 % */
#define VCLK_HOLD_MAX       3           /* a band that missed: 3 s << 3 */
#define VCLK_BIN_CYC        13824       /* the p95's bins: 512 us at 27 MHz */
#define VCLK_BINS           256         /* to 3.5 M cycles; above, the last */

struct vclk_video
{
    uint32_t mask;
    uint8_t floor;              /* the band asked for */
    uint8_t hold[VCLK_BANDS];   /* misses at each band */
    uint32_t frame_us;
    uint32_t start_us;          /* the window's */
    uint16_t hist[VCLK_BINS];   /* FEED cycles / VCLK_BIN_CYC */
    uint32_t n;
    uint32_t max_cyc;           /* the window's longest FEED, cycles */
    uint32_t misses, raises, drops;
};

void vclk_video_start(struct vclk_video *v, uint32_t mask, uint32_t frame_us,
                      uint32_t now_us);
/* One FEED's time at the clock it ran at: the floor to ask for now (it
   may not have changed) */
int vclk_video_feed(struct vclk_video *v, uint32_t now_us, uint32_t work_us,
                    unsigned mhz);
/* A late picture, a skip or a dropped GOP at the band running */
int vclk_video_miss(struct vclk_video *v, uint32_t now_us, int band);
/* A seek, a resume or a wake: the top again */
int vclk_video_race(struct vclk_video *v, uint32_t now_us);

/* ---- the governor: two layers ------------------------------------------
 *
 * 1. The band selector picks the supply level: a verified band of Apple's
 *    13-row table.  Up one band when the work keeps saturating the band's
 *    top clock (the load, f x the busy share's EWMA, at 95 % of the top or
 *    more); down one band when the band below's top would carry the load
 *    at 80 % or less, after 100 ms at the band (the gap between 95 and 80
 *    is the hysteresis).  A client's floor (the video session's, from its
 *    FEED times and misses) forces a minimum band at once; a burst (a
 *    DECODE) the top band for its run.  The chip changes the level before
 *    a rise and after a fall, with Apple's 10 ms wait.
 * 2. The in-band policy picks the clock inside the band (no level change,
 *    only PLL A's 1 ms lock):
 *      VCLK_POLICY_TOP  the band's top clock, idle the rest (race to idle
 *                       inside the band: the voltage-aware choice, the
 *                       default);
 *      VCLK_POLICY_PROP f = load / 0.8, inside the band (above the band
 *                       below's top); up at once, down only by 10 % or
 *                       more.
 */

#define VCLK_UP_PM          950         /* load / the band's top: up */
#define VCLK_DOWN_PM        800         /* load / the band below's top: down */
#define VCLK_TARGET_PM      800         /* PROP: busy share aimed at */
#define VCLK_DWELL_US       100000      /* at a band before it goes down */
#define VCLK_PROP_DOWN_PCT  90          /* PROP: a lower clock by 10 %+ */

enum vclk_policy
{
    VCLK_POLICY_TOP = 0,
    VCLK_POLICY_PROP,
    VCLK_POLICIES
};
extern const char *const vclk_policy_name[VCLK_POLICIES];

enum vclk_why
{
    VCLK_WHY_START = 0,
    VCLK_WHY_FLOOR,             /* a client's floor */
    VCLK_WHY_BURST,             /* a burst's full speed */
    VCLK_WHY_UP,                /* the top clock saturated */
    VCLK_WHY_DOWN,              /* the band below carries it */
    VCLK_WHY_FAIL,              /* the chip fell back to the crystal */
    VCLK_WHYS
};
extern const char *const vclk_why_name[VCLK_WHYS];

struct vclk_gov
{
    uint32_t mask;              /* verified bands */
    uint8_t band;               /* running */
    uint8_t mhz;                /* its clock */
    uint8_t policy;             /* enum vclk_policy */
    uint8_t xtal_level;         /* the crystal's level (11, or 5) */
    int8_t floor;               /* a client's band, -1 none */
    bool burst;                 /* a burst asked for full speed */
    uint32_t load;              /* the work, MHz x 1000 (busy share x the
                                   clock running): up at once, down by an
                                   EWMA of 1/4 a reading */
    uint32_t util;              /* load / the clock running, permille */
    uint32_t changed_us;        /* the last band change */
    /* what to ask the chip for (vclk_gov_* returned 1) */
    uint8_t want_band, want_mhz;
    /* the last band move, for the log line */
    uint8_t from, to, why;
    /* accounting since start */
    uint32_t last_us;
    uint64_t mhz_us, lvl_us, el_us, busy_us;
    uint32_t at_ms[VCLK_BANDS];
    uint32_t band_changes, clock_changes, level_changes, fails;
};

void vclk_gov_init(struct vclk_gov *g, uint32_t mask, int band, unsigned mhz,
                   enum vclk_policy policy, unsigned xtal_level,
                   uint32_t now_us);
/* The chip's meter since the last reading (busy us over el us), a
   client's floor (a band, or -1 none), a burst: 1 when the governor wants
   another band or clock (want_band, want_mhz; a band move also sets from,
   to and why for vclk_band_line), else 0 */
int vclk_gov_meter(struct vclk_gov *g, uint32_t now_us, uint32_t busy_us,
                   uint32_t el_us);
int vclk_gov_floor(struct vclk_gov *g, uint32_t now_us, int band);
int vclk_gov_burst(struct vclk_gov *g, uint32_t now_us, bool on);
/* The chip answered: it runs band at mhz (asked_band -1: a reading; a
   band other than the one asked for: the switch failed, the band is
   dropped from the mask) */
void vclk_gov_ran(struct vclk_gov *g, uint32_t now_us, int asked_band,
                  int band, unsigned mhz);
/* time up to now into the sums */
void vclk_gov_account(struct vclk_gov *g, uint32_t now_us);
unsigned vclk_level_of(const struct vclk_gov *g, int band);

/* The log lines (n >= 120):
   "vclk band from=150 to=102 lvl=9 why=floor util=412 floor=102"
   "vclk sum policy=top mhz=96 lvl=9.4 idle=612 bands=4 clocks=4
    lvl_changes=4 fails=0 misses=0 s=42"
   "vclk time 87=1200 150=4500 ms" */
int vclk_band_line(const struct vclk_gov *g, char *buf, size_t n);
int vclk_sum_line(const struct vclk_gov *g, uint32_t misses, char *buf,
                  size_t n);
int vclk_time_line(const struct vclk_gov *g, char *buf, size_t n);
/* time-weighted MHz, level x 10 and idle permille (0 if no time) */
void vclk_gov_means(const struct vclk_gov *g, uint32_t *mhz,
                    uint32_t *lvl10, uint32_t *idle_pm);

/* The cost model, the floor's sanity check (scripts/vclk_fit.py, fitted
   on silicon): mean core cycles a frame from pixels and bits */
#define VCLK_CYC_PER_PX     9
#define VCLK_CYC_PER_BIT    94
uint32_t vclk_model_cycles(uint32_t w, uint32_t h, uint32_t kbps,
                           uint32_t fps_milli);

#endif /* VCLK_H */
