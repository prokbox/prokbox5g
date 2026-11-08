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

/* The visualiser FFT's tables (vfft.h struct vfft_tabs), host only.  On
 * the BCM they are generated at build time (as bcmfw/video/gen_viz.py
 * writes jviz_tab.h today); here libm makes them, with Python's rounding
 * (ties to even: nearbyint in the default rounding mode) and Python's
 * order of operations, so every value equals the generator's. */
#include <math.h>
#include <string.h>
#include "vfft.h"

static int16_t rnd(double v)
{
    return (int16_t)nearbyint(v);
}

void vfft_tabs_make(struct vfft_tabs *t)
{
    double h[VFFT_N], a;
    int i, k2, l, k, f;

    memset(t, 0, sizeof(*t));
    for (i = 0; i < VFFT_N; i++)
        h[i] = nearbyint(VFFT_HANN_PEAK * 0.5 *
                         (1 - cos(2 * M_PI * i / VFFT_N)));
    for (k = 0; k < VFFT_M; k++)
    {
        t->win[k] = (int16_t)h[2 * k];
        t->win[VFFT_M + k] = (int16_t)h[2 * k + 1];
    }
    for (k2 = 0; k2 < VFFT_ROWS; k2++)
        for (l = 0; l < 16; l++)
        {
            a = 2 * M_PI * l * k2 / VFFT_M;
            t->tw[16 * k2 + l] = rnd(VFFT_Q * cos(a));
            t->tw[VFFT_M + 16 * k2 + l] = rnd(-VFFT_Q * sin(a));
        }
    for (k = 0; k < VFFT_M; k++)
    {
        a = 2 * M_PI * k / VFFT_N;
        t->ws[k] = rnd(VFFT_Q * cos(a));
        t->ws[VFFT_M + k] = rnd(-VFFT_Q * sin(a));
    }
    for (k = 0; k < 8; k++)
    {
        t->w16[2 * k] = rnd(VFFT_Q * cos(2 * M_PI * k / 16));
        t->w16[2 * k + 1] = rnd(-VFFT_Q * sin(2 * M_PI * k / 16));
    }
    for (f = 0; f < 16; f++)
    {
        long v = lrint(16 * log2(1 + f / 16.0));

        t->log16[f] = (uint8_t)(v > 15 ? 15 : v);
    }
}
