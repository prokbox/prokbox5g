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

/* Risk tool 1 driver (scripts/risk/arm_insn_count.sh): host-side timing
 * for apps/ipodui/scroll.c's ui_scroll_step(). Unlike sortkey/library,
 * this module's cost scales with animation frames, not library size, so
 * the "workload" here is a representative glide (a 200 px list jump,
 * the strip height in docs/hardware.md) stepped at a fixed dt=1 per
 * call, the shape ui_main.c's animation timer drives it with.
 *
 * Prints one CSV line to stdout:
 *   scroll,<glides>,<calls>,<ticks_total>,<elapsed_us>
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "ipodui/scroll.h"

int main(int argc, char **argv)
{
    int glides = argc > 1 ? atoi(argv[1]) : 10000;   /* independent glides */
    struct ui_scroll s;
    long calls = 0, ticks = 0;
    struct timespec t0, t1;
    int g;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (g = 0; g < glides; g++)
    {
        ui_scroll_init(&s, 4000);            /* ~a 50k-row offscreen strip */
        ui_scroll_set_target(&s, 200 + (g % 3000));
        while (ui_scroll_step(&s, 1))        /* dt=1: one 30fps tick each */
        {
            calls++;
            ticks++;
        }
        calls++;                              /* the final, converged step */
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    {
        double us = (double)(t1.tv_sec - t0.tv_sec) * 1e6
                  + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3;
        printf("scroll,%d,%ld,%ld,%.1f\n", glides, calls, ticks, us);
    }
    return 0;
}
