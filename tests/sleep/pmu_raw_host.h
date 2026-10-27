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

/* tests/sleep: the scripted I2C bus that pmu_raw-pp502x.c runs against
 * when built with PMU_RAW_HOST_TEST (test_pmu_raw.c defines these) */
#ifndef PMU_RAW_HOST_H
#define PMU_RAW_HOST_H
#include <stdbool.h>
#include <stdint.h>
#include "sleep_wake_core.h"
unsigned char host_i2c_rd(unsigned off);
void host_i2c_wr(unsigned off, unsigned char v);
uint32_t host_usec(void);
#define RAW_RD(off)     host_i2c_rd(off)
#define RAW_WR(off, v)  host_i2c_wr((off), (unsigned char)(v))
#define RAW_USEC()      host_usec()
bool pmu_raw_battery_mv(uint16_t *mv);
bool pmu_raw_rtc_seconds(uint32_t *s);
#endif
