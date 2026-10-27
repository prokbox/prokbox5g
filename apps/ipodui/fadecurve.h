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
 * iPod UI shell
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
#ifndef IPODUI_FADECURVE_H
#define IPODUI_FADECURVE_H

/* The pause/resume fade curve (docs/phase15-brief.md section 3.3), used by
 * apps/pcmbuf.c under HAVE_IPOD_UI and by tests/fade/. Include it from one
 * file only: the table is static.
 *
 * Equal dB steps instead of equal amplitude steps: index 0 is unity, each
 * index 2.5 dB lower, index 24 is -60 dB and FADE_STEPS (25) is mute. The
 * 100 Hz pcmbuf fade tick moves one index per tick, so a full fade is 25
 * ticks = 250 ms (brief section 9 Q3, frozen default). The state is the
 * index, so a fade reversed mid-way (pause, then a quick resume) turns
 * around where it is instead of restarting. */

#include <stdbool.h>
#include <stdint.h>

#define FADE_STEPS  25

/* MIX_AMP_UNITY (0x10000) * 10^(-2.5 * i / 20), rounded; then mute */
static const uint32_t fade_amp[FADE_STEPS + 1] = {
    65536, 49145, 36854, 27636, 20724, 15541, 11654,  8739,  6554,
     4915,  3685,  2764,  2072,  1554,  1165,   874,   655,   491,
      369,   276,   207,   155,   117,    87,    66,     0,
};

/* The index after one tick toward unity (in) or mute (out) */
static inline int fade_next(int idx, bool in)
{
    if (in)
        return idx > 0 ? idx - 1 : 0;
    return idx < FADE_STEPS ? idx + 1 : FADE_STEPS;
}

/* Ticks a fade from idx still takes to finish */
static inline int fade_ticks_left(int idx, bool in)
{
    return in ? idx : FADE_STEPS - idx;
}

#endif /* IPODUI_FADECURVE_H */
