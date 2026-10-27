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
 * iPod UI shell
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

/* The one copy of the wheel art (Phase 9, docs/phase9-brief.md section 0).
 * panelart.h defines its art as statics, so every file that drew the wheel
 * (about.c, panels.c) carried its own 5,073 B copy. Those two include the
 * header with the wheel names moved aside (their copies go unused and are
 * dropped) and reach this copy through the pointers below. */
#define panelart_wheel_a   panelart_wheel_a_tu
#define panelart_wheel_rgb panelart_wheel_rgb_tu
#include "ipodui/panelart.h"
#undef panelart_wheel_a
#undef panelart_wheel_rgb

const unsigned char *const panelart_wheel_a = panelart_wheel_a_tu;
const unsigned short *const panelart_wheel_rgb = panelart_wheel_rgb_tu;
