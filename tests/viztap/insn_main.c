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

/* Dynamic ARM instruction counts for the visualizer's PCM tap and its
 * CPU fallback (tests/viztap/insn.sh): one workload per build (WL),
 * measured against a baseline (WL 0) that does the same setup. */
#include <stddef.h>
#include <stdint.h>
#include "viz_tap_core.h"
#include "jviz.h"

#ifndef WL
#define WL 0
#endif

static uint32_t ring[1024], frame[256];
static int16_t win[512];
static uint32_t pw[256];
static uint16_t lv[32], edges[25];
volatile int sink;

int main(void)
{
    struct viz_ring r;
    struct viz_tap_frame f;
    uint32_t i, k;

    for (i = 0; i < 256; i++)
        frame[i] = (i * 2654435761u) ^ 0x5a5a1234u;
    viz_ring_init(&r, ring, 1024, 256);
    for (k = 0; k < 4; k++)
        viz_ring_write(&r, frame, 256);
    for (i = 0; i < 512; i++)
        win[i] = (int16_t)(i * 977u);
    for (i = 0; i < 25; i++)            /* 24 bands, 1/3 octave-ish */
        edges[i] = (uint16_t)(1 + (i * i * 255u) / 576u);
    for (i = 1; i < 25; i++)
        if (edges[i] <= edges[i - 1])
            edges[i] = edges[i - 1] + 1;
#if WL == 1     /* the FIQ's share: one 256-frame mixer frame */
    viz_ring_write(&r, frame, 256);
#elif WL == 2   /* one read: 512 samples, downmix, split, peak */
    sink = viz_ring_read(&r, win, 512, 1, 0, 0, &f);
#elif WL == 3   /* the same with the normalising shift */
    sink = viz_ring_read(&r, win, 512, 1, 0, VIZ_RD_NORM, &f);
#elif WL == 4   /* the CPU fallback: SPECTRUM's work for 512 samples */
    jviz_power_ref(pw, win, 512);
    jviz_bands(lv, pw, 256, edges, 24);
#elif WL == 5   /* the same for 256 samples */
    jviz_power_ref(pw, win, 256);
    jviz_bands(lv, pw, 128, edges, 12);
#endif
    (void)f;
    sink += (int)ring[5] + win[3] + (int)pw[2] + lv[1];
    return 0;
}
