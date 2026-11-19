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

/* Host test: the PMU's codec rail (D1REGC1, 0x24) at init and ever after.
 * The REAL pcf50605_init() is sliced out of firmware/drivers/pcf50605.c by
 * run.sh and compiled against a recording pcf50605_write().
 * Expect: the init writes 0xf5 (Apple's 3.0 V, "ours") under HAVE_IPOD_UI
 * on the iPod Video, and 0xf0 in a stock build (EXPECT_D1 says which).
 * No write to 0x24 may carry a voltage code below the expected one. */
#include <stdio.h>
#include <stdlib.h>

#define PCF5060X_D1REGC1 0x24
#define PCF5060X_IOREGC  0x23
#define PCF5060X_D2REGC1 0x25
#define PCF5060X_D3REGC1 0x26
#define PCF5060X_DCDC1   0x1b
#define PCF5060X_DCDC2   0x1c
#define PCF5060X_DCUDC1  0x1e
#define PCF5060X_LPREGC1 0x27

static int nw, d1n, d1_last = -1, d1_min = 0xff;

int pcf50605_write(int address, unsigned char val)
{
    nw++;
    if (address == PCF5060X_D1REGC1)
    {
        d1n++;
        d1_last = val;
        if ((val & 0x1f) < d1_min)
            d1_min = val & 0x1f;
    }
    return 1;
}

#include "init_slice.c"

int main(void)
{
    int want = EXPECT_D1;

    pcf50605_init();
    printf("pmucodec: %d PMU writes, %d to D1REGC1, last 0x%02x, lowest code 0x%02x\n",
           nw, d1n, d1_last, d1_min);
    if (nw < 4 || d1n < 1)
    {
        printf("pmucodec: FAIL: the init wrote nothing it should (gate inspected nothing)\n");
        return 1;
    }
    if (d1_last != want || d1_min != (want & 0x1f))
    {
        printf("pmucodec: FAIL: D1REGC1 0x%02x, wanted 0x%02x\n", d1_last, want);
        return 1;
    }
    printf("pmucodec: PASS (D1REGC1 = 0x%02x)\n", want);
    return 0;
}
