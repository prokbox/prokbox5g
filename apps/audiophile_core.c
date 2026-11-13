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

/* Settings > Audiophile: the output rate and depth rules (#268, #270).
 * Pure: no kernel, no settings; host-tested by tests/audiophile/. */
#define AUDIOPHILE_CORE_ONLY
#ifndef AUDIOPHILE_HOST_TEST
#include "config.h"         /* the scope switch: HAVE_HIRATE_OUT */
#endif
#include "audiophile.h"

int audiophile_allowed_rates(bool on, int ok_mask, bool failed,
                             int battery_pct)
{
#ifdef HAVE_HIRATE_OUT
    if (!on || failed)
        return 0;
    if (battery_pct >= 0 && battery_pct < AP_BATTERY_MIN)
        return 0;
    return ok_mask & (AP_RATE_88 | AP_RATE_96);
#else
    /* 0.8.3: no Settings > Audiophile; a road-test build's saved
       "on, 88.2 and 96 passed" allows nothing */
    (void)on; (void)ok_mask; (void)failed; (void)battery_pct;
    return 0;
#endif
}

unsigned long audiophile_pick_rate(unsigned long src_hz, int allowed)
{
    /* the families: 44.1 kHz's multiples (11.025, 22.05, 88.2, 176.4,
       352.8) and the rest, 48 kHz's (8, 16, 24, 32, 96, 192, 384; any
       other rate goes with the family the guess before this rule gave
       it) */
    bool fam44 = (src_hz % 4000) != 0;
    unsigned long base = fam44 ? 44100 : 48000;
    int bit = fam44 ? AP_RATE_88 : AP_RATE_96;

    /* low and standard rates: up to the family's standard rate (never to
       88.2/96: no gain); high rates: the family's highest enabled rate,
       so a 176.4/192/352.8/384 source is a 2:1 or 4:1 (on) or a 4:1 or
       8:1 (off) whole-number step */
    if (src_hz >= 2 * base && (allowed & bit))
        return 2 * base;
    return base;
}

int audiophile_width_cut_db(int depth)
{
    /* 0.8.3: headroom for the codec's 3D (Wolfson's advice is about 6 dB
       with 3D on): 0/5/10/15 -> 0/2/4/6 dB, rounded down between */
    if (depth <= 0)
        return 0;
    if (depth > 15)
        depth = 15;
    return (depth * 2 + 2) / 5;
}

int audiophile_precut_total(int eq_db, int width_db)
{
    /* both cut the same DAC volume: the EQ's boost and the 3D's gain are
       in series, so their headroom adds, up to the codec's 12 dB precut */
    int t = (eq_db > 0 ? eq_db : 0) + (width_db > 0 ? width_db : 0);

    return t > 12 ? 12 : t;
}

int audiophile_pick_depth(int src_bits, bool crossfade, bool builtin_sink)
{
    /* crossfade mixes 16-bit frames (pcmbuf.c); another sink (USB, dock)
       takes only 16-bit frames */
    if (src_bits > 16 && !crossfade && builtin_sink)
        return 24;
    return 16;
}
