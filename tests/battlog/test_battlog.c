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

/* Host test of the battery session line (apps/ipodui/battlog_core.h) */

#include <stdio.h>
#include <string.h>
#include "ipodui/battlog_core.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

int main(void)
{
    struct bl_acc a;
    struct bl_video v0 = { 0, 0, 0, 0, 0 }, v;
    char line[200];
    int k;

    /* no video: the chip on the crystal all lit time, its idle unknown */
    bl_begin(&a, 90, 4050, &v0);
    for (k = 0; k < 600; k++)
        bl_add(&a, 1000, k < 300, k >= 100 && k < 400);
    CHECK(!bl_due(&a));
    bl_line(line, sizeof(line), &a, 88, 4010, &v0, 11);
    CHECK(!strcmp(line, "session start=90/4050 end=88/4010 min lit=5 dark=5 "
                        "play=5 vc_mhz=27 vc_lvl=11.0 vc_idle=-"));
    /* level 5 on the crystal (PW3's lever) */
    bl_line(line, sizeof(line), &a, 88, 4010, &v0, 5);
    CHECK(strstr(line, "vc_lvl=5.0 vc_idle=-") != NULL);

    /* a governed video for half the lit time: 2.5 min at 87 MHz, level 8,
       its meter 40 % busy; the totals started at some earlier values */
    v0.mhz_ms = 1000;
    v0.ms = 10;
    bl_begin(&a, 80, 3900, &v0);
    for (k = 0; k < 300; k++)
        bl_add(&a, 1000, true, true);
    v = v0;
    v.mhz_ms += 87ull * 150000;
    v.lvl10_ms += 80ull * 150000;
    v.busy_ms += 60000;
    v.el_ms += 150000;
    v.ms += 150000;
    bl_line(line, sizeof(line), &a, 79, 3880, &v, 11);
    /* (87 + 27) / 2 = 57 MHz; (8 + 11) / 2 = 9.5; idle 600 permille */
    CHECK(!strcmp(line, "session start=80/3900 end=79/3880 min lit=5 dark=0 "
                        "play=5 vc_mhz=57 vc_lvl=9.5 vc_idle=600"));

    /* an hour: due */
    bl_begin(&a, 50, 3800, &v0);
    for (k = 0; k < 3599; k++)
        bl_add(&a, 1000, false, false);
    CHECK(!bl_due(&a));
    bl_add(&a, 1000, false, false);
    CHECK(bl_due(&a));
    bl_line(line, sizeof(line), &a, 49, 3790, &v0, 11);
    CHECK(strstr(line, "min lit=0 dark=60 play=0 vc_mhz=0 vc_lvl=0.0") != NULL);
    if (fails)
        fprintf(stderr, "%d failed\n", fails);
    return fails != 0;
}
