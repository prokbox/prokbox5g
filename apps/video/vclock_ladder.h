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

/* The first-use video check's band ladder (docs/research/bcm2722/
 * first-use-check.md): the bands a unit is checked at, highest first.
 * The check stops at the first band that passes and records it in the
 * unit's token; a play then never runs the chip above it.
 *
 * Measured (sitting 4c, image 23693ff4, two vbv runs): every band from
 * 74 to 150 MHz held in both (the band reached, no fallback, no decode
 * error, no concealed block, 244-268 pictures in 8 s).  59 MHz held in
 * the second run; 46 MHz and the first run's 59 read "ran 150 MHz", the
 * bench's artifact, not the chip's: each held its band 8.3-8.5 s with no
 * fallback, then the clip's loop re-opened the session at 150 and the
 * held band, a floor to the governor, never lowered it again (vpcore.c
 * vclk_want now asks a held band outright).  Those two wait for a clean
 * vbv run before they join; the check could hardly pass there anyway
 * (its key picture, 23.9 ms at 150 MHz, would take about 61 ms at 59 and
 * 78 ms at 46, against VCHECK_PIC_MAX_US's 66.7).
 *
 * Indices into vclk.h's vclk_band_mhz[] (11 = 150 MHz).  Data only: the
 * check (vcheck.c vcheck_run) walks whatever is here, in order, within
 * its time budget (VCHECK_BUDGET_US). */
#ifndef VCLOCK_LADDER_H
#define VCLOCK_LADDER_H
#include <stdint.h>

#define VCLOCK_LADDER_PENDING   0       /* the rows below are measured */

static const uint8_t vclock_ladder[] =
{
    11,     /* 150 MHz, level 15: the clock150 token's band */
    10,     /* 147 MHz, level 14: vbv sitting 4c, held twice */
    9,      /* 140 MHz, level 13: held twice */
    8,      /* 130 MHz, level 12: held twice */
    7,      /* 123 MHz, level 11: held twice */
    6,      /* 116 MHz, level 10: held twice */
    5,      /* 102 MHz, level 9: held twice */
    4,      /* 87 MHz, level 8: held twice */
    3,      /* 74 MHz, level 7: held twice */
};
#define VCLOCK_LADDER_N ((int)(sizeof(vclock_ladder) / sizeof(vclock_ladder[0])))

#endif /* VCLOCK_LADDER_H */
