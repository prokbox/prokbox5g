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
 * Host test for the generated hardware-EQ table (docs/phase15-brief.md
 * section 7): every field in range, Flat all zeros, and the hardware
 * presets' count. Compiled together with apps/ipodui/eq_hw_presets.c by
 * tests/eqfit/run.sh, which also checks that file equals a fresh eqfit run.
 *
 *   check <flat index>
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
#include <stdio.h>
#include <stdlib.h>
#include "ipodui/eq_hw.h"

int main(int argc, char **argv)
{
    int fail = 0, nhw = 0;
    int flat = argc > 1 ? atoi(argv[1]) : -1;

    if (sizeof eq_hw_presets / sizeof eq_hw_presets[0] != EQ_HW_PRESETS)
        return 1;
    for (int i = 0; i < EQ_HW_PRESETS; i++)
    {
        const struct eq_hw_preset *p = &eq_hw_presets[i];
        for (int b = 0; b < 5; b++)
        {
            const struct eq_hw_band *e = &p->band[b];
            if (e->gain < -12 || e->gain > 12 || e->freq > 3 || e->wide > 1 ||
                (e->wide && (b == 0 || b == 4)))
            {
                printf("FAIL: preset %d band %d out of range\n", i, b);
                fail = 1;
            }
        }
        if (p->precut > 12)
        {
            printf("FAIL: preset %d precut %d > 12\n", i, p->precut);
            fail = 1;
        }
        nhw += p->maxerr <= EQ_HW_MAXERR;
    }
    if (flat < 0 || flat >= EQ_HW_PRESETS)
    {
        printf("FAIL: no Flat preset found\n");
        fail = 1;
    }
    else
    {
        const struct eq_hw_preset *p = &eq_hw_presets[flat];
        for (int b = 0; b < 5; b++)
            if (p->band[b].gain || p->band[b].freq || p->band[b].wide)
                fail = 1;
        if (p->precut || p->maxerr)
            fail = 1;
        if (fail)
            printf("FAIL: Flat (%d) is not all zeros\n", flat);
    }
    /* every preset on the hardware (owner, 2026-10-11) */
    if (nhw != EQ_HW_PRESETS)
    {
        printf("FAIL: only %d presets fit within %d.%d dB\n", nhw,
               EQ_HW_MAXERR / 10, EQ_HW_MAXERR % 10);
        fail = 1;
    }
    if (!fail)
        printf("ok   eqfit table: 22 presets in range, Flat zeros, "
               "%d on hardware at %d.%d dB\n", nhw, EQ_HW_MAXERR / 10,
               EQ_HW_MAXERR % 10);
    return fail;
}
