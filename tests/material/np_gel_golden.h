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

/* tests/material/np_gel_golden.h: a FROZEN golden of Now Playing's blue
 * gel, its groove and its flat-reflection alphas, captured from main
 * (screens/nowplaying.c gel_rows() and groove_grey[], np_draw.c
 * np_flat_refl_a(), NP_TICK_TOP 0) before the material kit took them over
 * (Phase 8b brief 3.1, review M7). Never regenerate it: tests/material
 * compares the kit against it. */
#include <stdint.h>

static const uint16_t np_gel_golden_14[14] = {
    0x7d5e, 0x4c5d, 0x3bfd, 0x2bbd, 0x239d, 0x1b5c, 0x133c, 0x0b1c,
    0x137c, 0x13fd, 0x1c7d, 0x24dd, 0x2d3e, 0x1b57,
};
static const uint16_t np_gel_golden_15[15] = {
    0x7d5e, 0x4c5d, 0x3c1d, 0x2bbd, 0x239d, 0x1b7c, 0x135c, 0x133c,
    0x0b3c, 0x139c, 0x141d, 0x1c7d, 0x24dd, 0x2d3e, 0x1b57,
};
static const uint16_t np_gel_golden_19[19] = {
    0x7d5e, 0x4c5d, 0x3c1d, 0x33dd, 0x2bbd, 0x239d, 0x1b7c, 0x1b5c,
    0x133c, 0x133c, 0x0b1c, 0x137c, 0x13bc, 0x141d, 0x1c5d, 0x24bd,
    0x24fd, 0x2d3e, 0x1b57,
};
static const uint16_t np_groove_golden_14[14] = {
    0xb5b6, 0xe71c, 0xef7d, 0xef7d, 0xef7d, 0xe73c, 0xdefb, 0xd6ba,
    0xd69a, 0xdedb, 0xdefb, 0xe71c, 0xdefb, 0x9cf3,
};

struct np_refl_golden { int h, n; unsigned a[16]; };
static const struct np_refl_golden np_refl_golden[] = {
    { 14, 5, { 71, 44, 27, 10, 3, } },
    { 19, 4, { 71, 51, 23, 8, } },
    { 14, 8, { 71, 44, 27, 16, 10, 5, 2, 0, } },
    { 15, 6, { 71, 46, 28, 18, 7, 2, } },
    { 24, 10, { 71, 54, 40, 30, 22, 17, 12, 9, 4, 1, } },
    { 55, 16, { 71, 64, 56, 49, 44, 39, 34, 29, 27, 23, 20, 18, 16, 14, 8, 3, } },
    { 142, 16, { 71, 69, 67, 62, 60, 56, 54, 51, 49, 46, 44, 43, 40, 39, 24, 11, } },
    { 8, 3, { 71, 20, 4, } },
};
