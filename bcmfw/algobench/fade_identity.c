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

/* jmix_fade_px (bcmfw/jobs/jmix.h, one multiply: (d << 5) + (s - d) * w)
 * against the two-multiply form it replaced, d * (32 - w) + s * w, on the
 * survey's grid of every 7th d, every 3rd s and every 5th weight from 1,
 * and on every weight 0..300 over a coarser grid.  Prints the cases and
 * how many differ (algorithms.md section 6). */
#include <stdio.h>
#include <stdint.h>
#include "../jobs/jmix.h"

static inline uint16_t fade2(uint32_t d, uint32_t s, uint32_t weight)
{   /* jmix_fade_px before: two multiplies */
    uint32_t w32 = (weight + 4) >> 3, iw = 32 - w32, r;

    if (weight >= 256)
        return (uint16_t)s;
    if (d == s)
        return (uint16_t)d;
    if (w32 == 0)
        w32 = 1, iw = 31;
    d = (d | (d << 16)) & 0x07e0f81f;
    s = (s | (s << 16)) & 0x07e0f81f;
    r = ((d * iw + s * w32 + 0x02008010) >> 5) & 0x07e0f81f;
    return (uint16_t)(r | (r >> 16));
}

int main(void)
{
    uint64_t bad = 0, n = 0;
    uint32_t d, s, w;

    for (d = 0; d < 65536; d += 7)
        for (s = 0; s < 65536; s += 3)
            for (w = 1; w <= 256; w += 5, n++)
                bad += jmix_fade_px(d, s, w) != fade2(d, s, w);
    for (d = 0; d < 65536; d += 61)
        for (s = 0; s < 65536; s += 59)
            for (w = 0; w <= 300; w++, n++)
                bad += jmix_fade_px(d, s, w) != fade2(d, s, w);
    printf("%llu cases, %llu differ\n", (unsigned long long)n,
           (unsigned long long)bad);
    return bad != 0;
}
