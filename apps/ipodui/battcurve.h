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

#ifndef IPODUI_BATTCURVE_H
#define IPODUI_BATTCURVE_H

#include <stdint.h>

/* The fitted cell (one-build brief WP3, #205): the answer to the
 * first-start battery question, stored in the hidden setting
 * ipodui_battery_cell. Pure: no Rockbox calls, so the host test
 * (tests/battcurve/) compiles it as it is. */
enum battcell {
    BATTCELL_UNSET = 0,     /* not answered yet: the safe defaults */
    BATTCELL_STOCK,         /* the original cell, by the unit's RAM */
    BATTCELL_2000,          /* a replacement, about 2,000 mAh */
    BATTCELL_3000,          /* a replacement, 3,000 mAh or more */
    BATTCELL_COUNT
};

#define BATTCURVE_POINTS 11  /* 0 %, 10 %, ... 100 % */

/* The cell's capacity in mAh. The original cell is 650 mAh on a 32 MB
 * unit (the thin 30 GB) and 850 mAh on a 64 MB unit (60/80 GB); an
 * unanswered unit counts as the original cell (deep-sleep timing on the
 * safe side). ram_mb: the probed RAM size. */
int battcurve_mah(int cell, int ram_mb);

/* The cell's sag in mV: how far its full-charge reading under the
 * player's load sits below the 3,000 mAh cell's (a smaller cell has a
 * higher internal resistance). Stock 35 (32 MB) / 30 (64 MB), 2,000 mAh
 * 10, 3,000 mAh and unanswered 0. [hypothesis]: batcal learns each
 * unit's real table from its first full run-down. */
int battcurve_sag(int cell, int ram_mb);

/* The cell's curve from the storage's base row (the owner's measured
 * 3,000 mAh rundown, powermgmt-ipod-pcf.c): 0 % stays where it is (the
 * storage's disk-safe floor + 20 mV), 100 % moves down by the sag, and
 * every point between keeps its share of the span ("the same voltage
 * drops over a tighter range"):
 *   out[p] = A + (base[p] - A) * (top - sag - A) / (top - A), rounded,
 * with A = base[0], top = base[10]. A sag of 0 copies the row. in and
 * out may be the same array. */
void battcurve_scale(const uint16_t *base, int sag, uint16_t *out);

#endif /* IPODUI_BATTCURVE_H */
