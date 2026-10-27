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
 * Host test of the Fit-3 step 0 counters (docs/fit3-brief.md §3): the
 * contract in firmware/export/devstats.h.  Each hook only adds, the
 * longest UI update is kept, and readers take deltas without clearing.
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
#include <string.h>
#include "devstats.h"

struct devstats devstats;
static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

int main(void)
{
    struct devstats_fit3 before;

    DEVSTATS_LCD(76800, 27000);
    DEVSTATS_LCD(640, 120);
    CHECK(devstats.f3.lcd_bytes == 77440);
    CHECK(devstats.f3.lcd_updates == 2);
    CHECK(devstats.f3.lcd_us == 27120);
    CHECK(devstats.f3.lcd_max_us == 27000);     /* the longest is kept */

    /* HAVE_LCD_E2: the engine's share of those pushes, apart */
    DEVSTATS_LCD_E2(65536, 5800);
    DEVSTATS_LCD_E2_FAIL();
    CHECK(devstats.f3.lcd_e2_bytes == 65536 && devstats.f3.lcd_e2_us == 5800);
    CHECK(devstats.f3.lcd_e2_fails == 1);
    CHECK(devstats.f3.lcd_bytes == 77440);      /* not counted twice */

    DEVSTATS_OVL(28160);
    CHECK(devstats.f3.ovl_bytes == 28160 && devstats.f3.ovl_updates == 1);

    DEVSTATS_RD_TICK();
    DEVSTATS_RD_POLL();
    DEVSTATS_RD_POLL();
    CHECK(devstats.f3.rd_tick == 1 && devstats.f3.rd_poll == 2);

    DEVSTATS_ART(250000, 62, 11000, 95000, 1);
    CHECK(devstats.f3.art_builds == 1 && devstats.f3.art_bytes == 250000);
    CHECK(devstats.f3.art_reads == 62 && devstats.f3.art_read_us == 11000);
    CHECK(devstats.f3.art_dec_us == 95000 && devstats.f3.art_boosts == 1);

    /* a reader's delta across more adds; nothing is ever cleared */
    before = devstats.f3;
    DEVSTATS_PCM_WB(40);
    DEVSTATS_PCM_WB(45);
    CHECK(devstats.f3.pcm_wb - before.pcm_wb == 2);
    CHECK(devstats.f3.pcm_wb_us - before.pcm_wb_us == 85);
    CHECK(devstats.f3.lcd_bytes == before.lcd_bytes);

    /* 32-bit wrap: deltas stay right across it */
    devstats.f3.lcd_bytes = 0xFFFFFF00u;
    before = devstats.f3;
    DEVSTATS_LCD(0x200, 1);
    CHECK(devstats.f3.lcd_bytes - before.lcd_bytes == 0x200);

    if (fails)
        return 1;
    return 0;
}
