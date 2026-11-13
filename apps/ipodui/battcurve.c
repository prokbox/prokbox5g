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

/* The fitted cell's capacity, sag and curve (battcurve.h) */
#include "ipodui/battcurve.h"

int battcurve_mah(int cell, int ram_mb)
{
    switch (cell)
    {
        case BATTCELL_2000:
            return 2000;
        case BATTCELL_3000:
            return 3000;
        default:                    /* the original cell, or unanswered */
            return ram_mb >= 64 ? 850 : 650;
    }
}

int battcurve_sag(int cell, int ram_mb)
{
    switch (cell)
    {
        case BATTCELL_STOCK:
            return ram_mb >= 64 ? 30 : 35;
        case BATTCELL_2000:
            return 10;
        default:                    /* 3,000 mAh, or unanswered */
            return 0;
    }
}

void battcurve_scale(const uint16_t *base, int sag, uint16_t *out)
{
    int a = base[0], span = base[BATTCURVE_POINTS - 1] - a;
    int to = span - sag, i;

    if (span <= 0 || sag <= 0 || to <= 0)
    {
        for (i = 0; i < BATTCURVE_POINTS; i++)
            out[i] = base[i];
        return;
    }
    for (i = 0; i < BATTCURVE_POINTS; i++)
    {
        int d = base[i] - a;

        out[i] = (uint16_t)(a + (d * to + span / 2) / span);
    }
}
