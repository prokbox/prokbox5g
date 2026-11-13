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

/* #270 output-rate rule goldens (apps/audiophile_core.c).  Every source
 * rate the issue lists, with the high rates off, both on, and each alone.
 * The expected rates are literal numbers, not the code's constants. */
#include <stdio.h>
#include <stdbool.h>
#define AUDIOPHILE_CORE_ONLY
#include "audiophile.h"

static const struct { unsigned long src, off, both, only88, only96; } g[] = {
    /*  src      off     on(both) 88 only  96 only */
    {   8000,  48000,  48000,  48000,  48000 },
    {  11025,  44100,  44100,  44100,  44100 },
    {  16000,  48000,  48000,  48000,  48000 },
    {  22050,  44100,  44100,  44100,  44100 },
    {  24000,  48000,  48000,  48000,  48000 },
    {  32000,  48000,  48000,  48000,  48000 },
    {  44100,  44100,  44100,  44100,  44100 },
    {  48000,  48000,  48000,  48000,  48000 },
    {  88200,  44100,  88200,  88200,  44100 },
    {  96000,  48000,  96000,  48000,  96000 },
    { 176400,  44100,  88200,  88200,  44100 },
    { 192000,  48000,  96000,  48000,  96000 },
    { 352800,  44100,  88200,  88200,  44100 },
    { 384000,  48000,  96000,  48000,  96000 },
};

int main(void)
{
    int fails = 0, n = 0;
    for (unsigned i = 0; i < sizeof g / sizeof g[0]; i++) {
        const int masks[4] = { 0, AP_RATE_88 | AP_RATE_96, AP_RATE_88,
                               AP_RATE_96 };
        const unsigned long want[4] = { g[i].off, g[i].both, g[i].only88,
                                        g[i].only96 };
        for (int m = 0; m < 4; m++, n++) {
            unsigned long got = audiophile_pick_rate(g[i].src, masks[m]);
            if (got != want[m]) {
                printf("FAIL rate %lu mask %d: got %lu want %lu\n",
                       g[i].src, masks[m], got, want[m]);
                fails++;
            }
        }
    }
    /* depth: 24 only for >16-bit sources, no crossfade, the built-in sink */
    static const struct { int bits; bool xf, sink; int want; } d[] = {
        { 16, false, true, 16 }, { 0, false, true, 16 },
        { 24, false, true, 24 }, { 20, false, true, 24 },
        { 32, false, true, 24 }, { 24, true, true, 16 },
        { 24, false, false, 16 },
    };
    for (unsigned i = 0; i < sizeof d / sizeof d[0]; i++, n++) {
        int got = audiophile_pick_depth(d[i].bits, d[i].xf, d[i].sink);
        if (got != d[i].want) {
            printf("FAIL depth %d xf %d sink %d: got %d want %d\n",
                   d[i].bits, d[i].xf, d[i].sink, got, d[i].want);
            fails++;
        }
    }
    /* 0.8.3: the stereo width's headroom follows the depth (the four
       steps' literal cuts), and adds to an EQ's precut up to 12 dB */
    static const int wd[][2] = { {0,0}, {5,2}, {10,4}, {15,6}, {-1,0}, {20,6} };
    for (unsigned i = 0; i < sizeof wd / sizeof wd[0]; i++, n++) {
        int got = audiophile_width_cut_db(wd[i][0]);
        if (got != wd[i][1]) {
            printf("FAIL width cut depth %d: got %d want %d\n",
                   wd[i][0], got, wd[i][1]);
            fails++;
        }
    }
    static const int pt[][3] = { {0,0,0}, {0,6,6}, {3,2,5}, {8,6,12},
                                 {12,4,12}, {-2,4,4} };
    for (unsigned i = 0; i < sizeof pt / sizeof pt[0]; i++, n++) {
        int got = audiophile_precut_total(pt[i][0], pt[i][1]);
        if (got != pt[i][2]) {
            printf("FAIL precut eq %d width %d: got %d want %d\n",
                   pt[i][0], pt[i][1], got, pt[i][2]);
            fails++;
        }
    }
    /* The upgrade from the road-test build (0.8.3 scope switch): a saved
       "High Sample Rate on, 88.2 and 96 passed" (config.cfg "ipodui high
       rate: on", "ipodui high rate ok: 131") through the rules.  Built
       without HAVE_HIRATE_OUT (0.8.3, IPODUI_AUDIOPHILE off): nothing
       allowed, so a 96/192 kHz 24-bit source plays at 48 kHz, 24-bit.
       Built with it (0.9.0): 96 kHz, as the road-test build did. */
    {
#ifdef HAVE_HIRATE_OUT
        const int want_mask = AP_RATE_88 | AP_RATE_96;
        const unsigned long want96 = 96000, want192 = 96000, want88 = 88200;
#else
        const int want_mask = 0;
        const unsigned long want96 = 48000, want192 = 48000, want88 = 44100;
#endif
        int saved = audiophile_allowed_rates(true, 0x83, false, 80);
        const struct { unsigned long src, want; } u[] = {
            { 96000, want96 }, { 192000, want192 }, { 88200, want88 },
            { 44100, 44100 }, { 48000, 48000 },
        };

        n++;
        if (saved != want_mask) {
            printf("FAIL saved hirate on/0x83: allowed %d want %d\n",
                   saved, want_mask);
            fails++;
        }
        for (unsigned i = 0; i < sizeof u / sizeof u[0]; i++, n++) {
            unsigned long got = audiophile_pick_rate(u[i].src, saved);
            if (got != u[i].want) {
                printf("FAIL upgrade %lu Hz: output %lu want %lu\n",
                       u[i].src, got, u[i].want);
                fails++;
            }
        }
        n++;
        if (audiophile_pick_depth(24, false, true) != 24) {
            printf("FAIL upgrade: a 24-bit source not at 24-bit\n");
            fails++;
        }
#ifdef HAVE_HIRATE_OUT
        /* the rest of the 0.9.0 rule: off, a fault, a low battery */
        static const struct { bool on; int ok; bool failed; int batt;
                              int want; } a[] = {
            { false, 0x83, false, 80, 0 }, { true, 0x83, true, 80, 0 },
            { true, 0x83, false, 9, 0 }, { true, 0x83, false, 10, 3 },
            { true, 0x83, false, -1, 3 }, { true, 0x81, false, 50, 1 },
            { true, 0x80, false, 50, 0 },
        };
        for (unsigned i = 0; i < sizeof a / sizeof a[0]; i++, n++) {
            int got = audiophile_allowed_rates(a[i].on, a[i].ok, a[i].failed,
                                               a[i].batt);
            if (got != a[i].want) {
                printf("FAIL allowed on %d ok %#x failed %d batt %d: got %d "
                       "want %d\n", a[i].on, a[i].ok, a[i].failed, a[i].batt,
                       got, a[i].want);
                fails++;
            }
        }
#endif
    }
    printf("%d cases, %d failed\n", n, fails);
    return fails != 0;
}
