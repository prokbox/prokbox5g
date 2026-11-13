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

/* tests/dacosr stub: the slice of the firmware the WM8758 driver uses,
   with the ipodvideo HAVE_IPOD_UI config flags it is built with */
#ifndef DACOSR_STUB_SYSTEM_H
#define DACOSR_STUB_SYSTEM_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define HAVE_IPOD_UI
#define HAVE_HIRATE_OUT
#define HAVE_PCM_24BIT_OUT
#define HAVE_DAC_STANDBY
#define HAVE_PWR_I2SGATE
#define HZ 100
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define TIME_BEFORE(a, b) ((long)(a) - (long)(b) < 0)
extern long current_tick;
extern unsigned long stub_usec;
#define USEC_TIMER stub_usec
static inline void udelay(unsigned us) { stub_usec += us; }
#endif
