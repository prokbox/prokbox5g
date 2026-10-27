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

/* Pure C on purpose: no Rockbox headers (host-tested by tests/vizcore/). */
#include "viz_core.h"

/* 2^(k/16) in Q12, k < 16: the log spacing without floats */
static const uint16_t exp2_q12[16] = {
    4096, 4277, 4467, 4664, 4871, 5087, 5312, 5547,
    5793, 6049, 6317, 6597, 6889, 7194, 7512, 7845,
};

/* f_lo * 2^(oct * i / nb) for the band edge i, oct in 1/16 octaves */
static uint32_t edge_hz(unsigned i, unsigned nb, uint32_t oct16)
{
    uint32_t s = oct16 * i / nb;            /* 1/16 octaves above f_lo */
    uint32_t f = (uint32_t)VIZ_F_LO << (s >> 4);

    return (f * exp2_q12[s & 15]) >> 12;
}

unsigned viz_edges(uint16_t *e, unsigned nb, unsigned n, unsigned rate)
{
    unsigned m = n / 2, i;
    uint32_t hi = rate / 2 < VIZ_F_HI ? rate / 2 : VIZ_F_HI, oct16 = 0;

    if (nb > VIZ_BANDS_MAX)
        nb = VIZ_BANDS_MAX;
    if (nb > m - 1)
        nb = m - 1;
    if (nb < 1 || rate == 0)
        return 0;
    /* the octaves from f_lo to hi, in 1/16 steps */
    while (oct16 < 16 * 12 && edge_hz(1, 1, oct16 + 1) <= hi)
        oct16++;
    for (i = 0; i <= nb; i++)
    {
        /* bin of the edge: f * n / rate, rounded */
        uint32_t b = (edge_hz(i, nb, oct16) * n + rate / 2) / rate;

        if (i > 0 && b <= e[i - 1])
            b = e[i - 1] + 1;               /* at least one bin a band */
        if (b < 1)
            b = 1;                          /* never the DC bin */
        e[i] = (uint16_t)b;
    }
    /* the top went past the bins: pull the edges back down, one apart */
    if (e[nb] > m)
    {
        e[nb] = (uint16_t)m;
        for (i = nb; i-- > 0;)
            if (e[i] >= e[i + 1])
                e[i] = e[i + 1] - 1;
    }
    return nb;
}

void viz_dyn_init(struct viz_dyn *d)
{
    unsigned i;

    d->top = VIZ_GAIN_FLOOR_L2 * 16;
    for (i = 0; i < VIZ_BANDS_MAX; i++)
        d->level[i] = d->peak[i] = d->pvel[i] = d->hold[i] = 0;
}

void viz_dyn_step(struct viz_dyn *d, const uint16_t *l2, unsigned nb,
                  int quiet, unsigned h, unsigned ms, uint8_t *height,
                  uint8_t *peak)
{
    int32_t tgt[VIZ_BANDS_MAX], loud = 0, floor_l2;
    int32_t hq = (int32_t)h << 8;
    unsigned i, steps = ms >= 50 ? 2 : 1, s;

    if (nb > VIZ_BANDS_MAX)
        nb = VIZ_BANDS_MAX;
    for (i = 0; i < nb; i++)
    {
        int32_t v = quiet ? 0 : (int32_t)l2[i] + (int32_t)(i * VIZ_TILT_L2) / 2;

        tgt[i] = v;
        if (v > loud)
            loud = v;
    }
    /* auto-gain: the top follows a louder band at once and falls about
     * 3 dB a second (16 units/s, in x16: 256 a second) */
    if (loud * 16 > d->top)
        d->top = loud * 16;
    else
        d->top -= (int32_t)(256 * ms / 1000) + 1;
    if (d->top < VIZ_GAIN_FLOOR_L2 * 16)
        d->top = VIZ_GAIN_FLOOR_L2 * 16;
    floor_l2 = d->top / 16 - VIZ_RANGE_L2;
    for (i = 0; i < nb; i++)
    {
        int32_t v = tgt[i] - floor_l2;

        v = v <= 0 ? 0 : v >= VIZ_RANGE_L2 ? hq : v * hq / VIZ_RANGE_L2;
        tgt[i] = v;
    }
    /* the neighbour lift: a bar is at least 2/3 of each neighbour's,
     * so one loud band draws a soft triangle (cava's monstercat) */
    for (i = 1; i < nb; i++)
        if (tgt[i] < tgt[i - 1] * 2 / 3)
            tgt[i] = tgt[i - 1] * 2 / 3;
    for (i = nb - 1; i-- > 0;)
        if (tgt[i] < tgt[i + 1] * 2 / 3)
            tgt[i] = tgt[i + 1] * 2 / 3;
    for (i = 0; i < nb; i++)
    {
        int32_t y = d->level[i];

        for (s = 0; s < steps; s++)
        {
            int32_t dlt = tgt[i] - y;

            if (dlt > 0)        /* attack: 0.625 of the way */
                y += dlt / 2 + dlt / 8;
            else                /* release: about 0.16 of the way */
                y += dlt / 8 + dlt / 32 - (dlt < 0 ? 1 : 0);
        }
        if (y < 0)
            y = 0;
        if (y > hq)
            y = hq;
        d->level[i] = (uint16_t)y;
        /* the cap: up with the bar, held, then falling faster */
        if (y >= d->peak[i])
        {
            d->peak[i] = (uint16_t)y;
            d->pvel[i] = 0;
            d->hold[i] = VIZ_PEAK_HOLD_MS;
        }
        else if (d->hold[i] > ms)
            d->hold[i] -= ms;
        else
        {
            int32_t p;

            d->hold[i] = 0;
            for (s = 0; s < steps; s++)
                d->pvel[i] += 48;           /* 0.19 px a frame, squared */
            p = (int32_t)d->peak[i] - d->pvel[i] * (int32_t)steps;
            d->peak[i] = (uint16_t)(p < y ? y : p);
        }
        height[i] = (uint8_t)((y + 128) >> 8);
        /* no cap on an empty band */
        peak[i] = d->peak[i] < 256 ? 0 : (uint8_t)((d->peak[i] + 128) >> 8);
    }
}

void viz_mirror_split(unsigned h, unsigned *bars_h, unsigned *refl_h)
{
    unsigned r = h * 3 / 10;

    *refl_h = r;
    *bars_h = h - r - (r ? 1 : 0);
}

uint16_t viz_fade565(uint16_t c, uint16_t bg)
{
    unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    unsigned R = (bg >> 11) & 31, G = (bg >> 5) & 63, B = bg & 31;

    r = (r * 3 + R * 5) >> 3;
    g = (g * 3 + G * 5) >> 3;
    b = (b * 3 + B * 5) >> 3;
    return (uint16_t)(r << 11 | g << 5 | b);
}
