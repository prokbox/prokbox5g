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

/* The video chip's clock governor, the host's half (vclk.h) */

#include <stdio.h>
#include <string.h>
#include "vclk.h"

/* bcmfw/clock.c's bands: the crystal, then the top whole MHz of each of
   Apple's level bands (@141ba), at that level */
const uint8_t vclk_band_mhz[VCLK_BANDS] =
    { 27, 46, 59, 74, 87, 102, 116, 123, 130, 140, 147, 150 };
const uint8_t vclk_band_level[VCLK_BANDS] =
    {  0,  5,  6,  7,  8,   9,  10,  11,  12,  13,  14,  15 };

const char *const vclk_why_name[VCLK_WHYS] =
{
    "start", "floor", "burst", "up", "down", "fail",
};

/* @141ba's rows (Hz, level), ascending: the first at or above the clock */
static const struct { uint32_t hz; uint8_t level; } apple_levels[] =
{
    {  46875000,  5 }, {  59665292,  6 }, {  74840572,  7 },
    {  87268380,  8 }, { 102016607,  9 }, { 116539140, 10 },
    { 123607364, 11 }, { 130841121, 12 }, { 140183656, 13 },
    { 147075854, 14 }, { 154130133, 15 }, { 160919540, 15 },
};

unsigned vclk_level_for_hz(uint32_t hz)
{
    size_t k;

    for (k = 0; k < sizeof(apple_levels) / sizeof(apple_levels[0]); k++)
        if (hz <= apple_levels[k].hz)
            return apple_levels[k].level;
    return 15;
}

int vclk_top(uint32_t mask)
{
    int b;

    for (b = VCLK_TOP; b > 0 && !(mask >> b & 1); b--)
        ;
    return b;
}

int vclk_band_for(uint32_t mask, uint32_t mhz)
{
    int b, top = vclk_top(mask | 1);

    for (b = 0; b < top; b++)
        if (((mask | 1) >> b & 1) && vclk_band_mhz[b] >= mhz)
            return b;
    return top;
}

uint32_t vclk_model_cycles(uint32_t w, uint32_t h, uint32_t kbps,
                           uint32_t fps_milli)
{
    uint64_t bits = fps_milli ? (uint64_t)kbps * 1000000u / fps_milli : 0;

    return (uint32_t)((uint64_t)w * h * VCLK_CYC_PER_PX +
                      bits * VCLK_CYC_PER_BIT);
}

static int hex8(const char *p, uint32_t *v)
{
    int k;

    *v = 0;
    for (k = 0; k < 8; k++, p++)
    {
        if (*p >= '0' && *p <= '9')
            *v = *v << 4 | (uint32_t)(*p - '0');
        else if (*p >= 'a' && *p <= 'f')
            *v = *v << 4 | (uint32_t)(*p - 'a' + 10);
        else
            return 0;
    }
    return 1;
}

uint32_t vclk_flags_mask(const char *text, uint32_t bytes, uint32_t fnv)
{
    uint32_t mask = 1, mhz, b2, f2;
    const char *p = text;
    int b;

    while (p && *p)
    {
        const char *q = p;

        mhz = 0;
        if (q[0] == 'v' && q[1] == 'c')
        {
            for (q += 2; *q >= '0' && *q <= '9' && mhz < 1000; q++)
                mhz = mhz * 10 + (uint32_t)(*q - '0');
            if (*q == ' ' && hex8(q + 1, &b2) && q[9] == ' ' &&
                hex8(q + 10, &f2) && (q[18] == '\n' || !q[18]) &&
                b2 == bytes && f2 == fnv)
                for (b = 1; b < VCLK_BANDS; b++)
                    if (vclk_band_mhz[b] == mhz)
                        mask |= 1u << b;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return mask;
}

int vclk_flag_line(char *buf, size_t n, int band, uint32_t bytes,
                   uint32_t fnv)
{
    return snprintf(buf, n, "vc%u %08lx %08lx\n",
                    vclk_band_mhz[band < 0 || band >= VCLK_BANDS ? 0 : band],
                    (unsigned long)bytes, (unsigned long)fnv);
}

/* ---- the video session's floor ---------------------------------------- */

static void window(struct vclk_video *v, uint32_t now_us)
{
    memset(v->hist, 0, sizeof(v->hist));
    v->n = 0;
    v->max_cyc = 0;
    v->start_us = now_us;
}

void vclk_video_start(struct vclk_video *v, uint32_t mask, uint32_t frame_us,
                      uint32_t now_us)
{
    memset(v, 0, sizeof(*v));
    v->mask = mask | 1;
    v->frame_us = frame_us ? frame_us : 33333;
    v->floor = (uint8_t)vclk_top(v->mask);
    window(v, now_us);
}

/* the next verified band below b, or -1 */
static int below(uint32_t mask, int b)
{
    while (--b >= 0)
        if (mask >> b & 1)
            return b;
    return -1;
}

/* the window's FEED p95 and its longest FEED fit band b's clock */
static bool fits(const struct vclk_video *v, uint32_t p95_cyc, int b)
{
    uint64_t room = (uint64_t)v->frame_us * vclk_band_mhz[b];

    return (uint64_t)p95_cyc * 100 <= room * (100 - VCLK_MARGIN_PCT) &&
           (uint64_t)v->max_cyc * 100 <= room * VCLK_SPIKE_PCT;
}

int vclk_video_feed(struct vclk_video *v, uint32_t now_us, uint32_t work_us,
                    unsigned mhz)
{
    uint32_t cyc, bin, need, seen = 0, p95 = 0;
    int b, k;

    if (!mhz)
        return v->floor;
    cyc = work_us * mhz;
    bin = cyc / VCLK_BIN_CYC;
    v->hist[bin < VCLK_BINS ? bin : VCLK_BINS - 1]++;
    v->n++;
    if (cyc > v->max_cyc)
        v->max_cyc = cyc;
    /* a steady window: 3 s, or longer when the band below missed before */
    k = below(v->mask, v->floor);
    if (k < 0 || v->n < VCLK_MIN_SAMPLES ||
        now_us - v->start_us < ((uint32_t)VCLK_STEADY_US << v->hold[k]))
        return v->floor;
    need = v->n - v->n / 20;
    for (b = 0; b < VCLK_BINS; b++)
    {
        seen += v->hist[b];
        if (seen >= need)
        {
            p95 = (uint32_t)(b + 1) * VCLK_BIN_CYC;
            break;
        }
    }
    /* down to the lowest verified band that fits; a band that missed
       before is the last one this window may reach */
    for (b = v->floor; k >= 0 && fits(v, p95, k); k = below(v->mask, k))
    {
        b = k;
        if (v->hold[k])
            break;
    }
    if (b < v->floor)
    {
        v->floor = (uint8_t)b;
        v->drops++;
    }
    window(v, now_us);
    return v->floor;
}

int vclk_video_miss(struct vclk_video *v, uint32_t now_us, int band)
{
    int b = band > v->floor ? band : v->floor, top = vclk_top(v->mask);

    v->misses++;
    if (band >= 0 && band < VCLK_BANDS && v->hold[band] < VCLK_HOLD_MAX)
        v->hold[band]++;
    while (b < top && !(v->mask >> ++b & 1))
        ;
    if (b > v->floor)
        v->raises++;
    v->floor = (uint8_t)b;
    window(v, now_us);
    return v->floor;
}

int vclk_video_race(struct vclk_video *v, uint32_t now_us)
{
    v->floor = (uint8_t)vclk_top(v->mask);
    window(v, now_us);
    return v->floor;
}

/* ---- the governor: two layers ------------------------------------------ */

const char *const vclk_policy_name[VCLK_POLICIES] = { "top", "prop" };

unsigned vclk_level_of(const struct vclk_gov *g, int band)
{
    return band ? vclk_band_level[band] : g->xtal_level;
}

void vclk_gov_account(struct vclk_gov *g, uint32_t now_us)
{
    uint32_t ms = (now_us - g->last_us) / 1000;

    /* whole milliseconds; the rest waits for the next call */
    g->mhz_us += (uint64_t)ms * 1000 * g->mhz;
    g->lvl_us += (uint64_t)ms * 1000 * vclk_level_of(g, g->band);
    g->at_ms[g->band] += ms;
    g->last_us += ms * 1000;
}

void vclk_gov_init(struct vclk_gov *g, uint32_t mask, int band, unsigned mhz,
                   enum vclk_policy policy, unsigned xtal_level,
                   uint32_t now_us)
{
    memset(g, 0, sizeof(*g));
    g->mask = mask | 1;
    g->band = (uint8_t)(band >= 0 && band < VCLK_BANDS ? band : 0);
    g->mhz = (uint8_t)(mhz ? mhz : vclk_band_mhz[g->band]);
    g->policy = (uint8_t)(policy < VCLK_POLICIES ? policy : VCLK_POLICY_TOP);
    g->xtal_level = (uint8_t)(xtal_level ? xtal_level : 11);
    g->floor = -1;
    g->util = 1000;
    g->load = 0;                /* the first reading sets it */
    g->last_us = g->changed_us = now_us;
    g->from = g->to = g->want_band = g->band;
    g->want_mhz = g->mhz;
}

/* the lowest clock band b may run: above the band below's top */
static unsigned band_low(int b)
{
    return b ? vclk_band_mhz[b - 1] + 1u : 27u;
}

/* Layer 1: the band for the load (MHz x 1000 of work) */
static int select_band(struct vclk_gov *g, uint32_t now_us, uint32_t load,
                       uint8_t *why)
{
    int b = g->band, top = vclk_top(g->mask), k;

    *why = VCLK_WHY_UP;
    if (g->burst)
    {
        *why = VCLK_WHY_BURST;
        return top;
    }
    if (load >= (uint32_t)vclk_band_mhz[b] * VCLK_UP_PM && b < top)
    {
        while (b < top && !(g->mask >> ++b & 1))
            ;
    }
    else if (b && now_us - g->changed_us >= VCLK_DWELL_US)
    {
        k = below(g->mask, b);
        if (k >= 0 && load <= (uint32_t)vclk_band_mhz[k] * VCLK_DOWN_PM)
        {
            b = k;
            *why = VCLK_WHY_DOWN;
        }
    }
    if (g->floor >= 0)
    {
        k = vclk_band_for(g->mask, vclk_band_mhz[g->floor]);
        if (b < k)
        {
            b = k;
            *why = VCLK_WHY_FLOOR;
        }
    }
    return b;
}

/* Layer 2: the clock inside band b */
static unsigned select_mhz(const struct vclk_gov *g, int b, uint32_t load)
{
    unsigned top = vclk_band_mhz[b], low = band_low(b), f;

    if (!b || g->policy == VCLK_POLICY_TOP || g->burst || b > g->band ||
        g->util >= VCLK_UP_PM)
        return top;
    f = (load + VCLK_TARGET_PM - 1) / VCLK_TARGET_PM;
    f = f < low ? low : f > top ? top : f;
    /* down only by 10 % or more: no relock for a few MHz */
    if (b == g->band && f < g->mhz && f * 100 > g->mhz * VCLK_PROP_DOWN_PCT)
        f = g->mhz;
    return f;
}

static int decide(struct vclk_gov *g, uint32_t now_us)
{
    uint32_t load = g->load;            /* MHz x 1000 */
    uint8_t why;
    int b = select_band(g, now_us, load, &why);
    unsigned f = select_mhz(g, b, load);

    g->want_band = (uint8_t)b;
    g->want_mhz = (uint8_t)f;
    if (b != g->band)
    {
        g->from = g->band;
        g->to = (uint8_t)b;
        g->why = why;
    }
    return b != g->band || f != g->mhz;
}

int vclk_gov_meter(struct vclk_gov *g, uint32_t now_us, uint32_t busy_us,
                   uint32_t el_us)
{
    uint32_t u;

    vclk_gov_account(g, now_us);
    if (!el_us)
        return 0;
    if (busy_us > el_us)
        busy_us = el_us;
    g->busy_us += busy_us;
    g->el_us += el_us;
    /* the load in MHz of work (x 1000): up at once (a reading's own),
       down by an EWMA (1/4 a reading) */
    u = (uint32_t)((uint64_t)busy_us * 1000 / el_us) * g->mhz;
    if (u >= g->load || g->el_us == el_us)      /* up, or the first */
        g->load = u;
    else
        g->load -= (g->load - u) / 4;
    g->util = g->load / g->mhz;
    return decide(g, now_us);
}

int vclk_gov_floor(struct vclk_gov *g, uint32_t now_us, int band)
{
    vclk_gov_account(g, now_us);
    g->floor = (int8_t)(band >= 0 && band < VCLK_BANDS ? band : -1);
    return decide(g, now_us);
}

int vclk_gov_burst(struct vclk_gov *g, uint32_t now_us, bool on)
{
    vclk_gov_account(g, now_us);
    g->burst = on;
    return decide(g, now_us);
}

void vclk_gov_ran(struct vclk_gov *g, uint32_t now_us, int asked_band,
                  int band, unsigned mhz)
{
    vclk_gov_account(g, now_us);
    if (band < 0 || band >= VCLK_BANDS)
        return;
    if (!mhz)
        mhz = vclk_band_mhz[band];
    if (band != g->band)
    {
        g->band_changes++;
        if (vclk_level_of(g, band) != vclk_level_of(g, g->band))
            g->level_changes++;
        g->changed_us = now_us;
    }
    if (band != g->band || mhz != g->mhz)
        g->clock_changes++;
    if (asked_band >= 0 && asked_band < VCLK_BANDS && band != asked_band)
    {
        /* the chip fell back: that band is gone for the session */
        g->fails++;
        if (asked_band)
            g->mask &= ~(1u << asked_band);
        g->from = (uint8_t)asked_band;
        g->to = (uint8_t)band;
        g->why = VCLK_WHY_FAIL;
    }
    g->band = (uint8_t)band;
    g->mhz = (uint8_t)mhz;
}

void vclk_gov_means(const struct vclk_gov *g, uint32_t *mhz, uint32_t *lvl10,
                    uint32_t *idle_pm)
{
    uint64_t t = 0;
    int b;

    for (b = 0; b < VCLK_BANDS; b++)
        t += g->at_ms[b];
    t *= 1000;
    *mhz = t ? (uint32_t)(g->mhz_us / t) : 0;
    *lvl10 = t ? (uint32_t)(g->lvl_us * 10 / t) : 0;
    *idle_pm = g->el_us ? (uint32_t)(1000 - g->busy_us * 1000 / g->el_us) : 0;
}

int vclk_band_line(const struct vclk_gov *g, char *buf, size_t n)
{
    return snprintf(buf, n, "vclk band from=%u to=%u lvl=%u why=%s util=%lu "
                    "floor=%u", vclk_band_mhz[g->from], vclk_band_mhz[g->to],
                    vclk_level_of(g, g->to),
                    vclk_why_name[g->why < VCLK_WHYS ? g->why : 0],
                    (unsigned long)g->util,
                    g->floor >= 0 ? vclk_band_mhz[g->floor] : 0);
}

int vclk_sum_line(const struct vclk_gov *g, uint32_t misses, char *buf,
                  size_t n)
{
    uint32_t mhz, lvl10, idle;
    uint64_t t = 0;
    int b;

    for (b = 0; b < VCLK_BANDS; b++)
        t += g->at_ms[b];
    vclk_gov_means(g, &mhz, &lvl10, &idle);
    return snprintf(buf, n, "vclk sum policy=%s mhz=%lu lvl=%lu.%lu idle=%lu "
                    "bands=%lu clocks=%lu lvl_changes=%lu fails=%lu "
                    "misses=%lu s=%lu",
                    vclk_policy_name[g->policy < VCLK_POLICIES ? g->policy : 0],
                    (unsigned long)mhz, (unsigned long)lvl10 / 10,
                    (unsigned long)lvl10 % 10, (unsigned long)idle,
                    (unsigned long)g->band_changes,
                    (unsigned long)g->clock_changes,
                    (unsigned long)g->level_changes,
                    (unsigned long)g->fails, (unsigned long)misses,
                    (unsigned long)(t / 1000));
}

int vclk_time_line(const struct vclk_gov *g, char *buf, size_t n)
{
    int b, k = snprintf(buf, n, "vclk time");

    for (b = 0; b < VCLK_BANDS && k > 0 && (size_t)k < n; b++)
        if (g->at_ms[b])
            k += snprintf(buf + k, n - k, " %u=%lu", vclk_band_mhz[b],
                          (unsigned long)g->at_ms[b]);
    if (k > 0 && (size_t)k < n)
        k += snprintf(buf + k, n - k, " ms");
    return k;
}
