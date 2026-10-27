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

/* Host shim for tests/bootscreen: a 320x240 RGB565 framebuffer standing in
 * for the LCD driver's default buffer (FBADDR in firmware/export/lcd.h).
 * GPL v2+. */
#ifndef BOOTSCREEN_SHIM_LCD_H
#define BOOTSCREEN_SHIM_LCD_H
#include <stdint.h>
typedef uint16_t fb_data;
extern fb_data host_fb[LCD_HEIGHT][LCD_WIDTH] __attribute__((aligned(4)));
#define FBADDR(x, y) (&host_fb[(y)][(x)])
#endif
