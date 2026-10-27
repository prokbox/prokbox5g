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

#ifndef IPODUI_SCREENS_CLOCKS_H
#define IPODUI_SCREENS_CLOCKS_H

/* Extras > Clocks (P12 W1-C, docs/phase12-brief.md Q13, Q27): the root
 * screen of MOD_CLOCKS (export slot MOD_SLOT_FIRST at integration; linked
 * statically in the simulator and the resident build). Pushed with
 * UI_T_REVEAL from the Extras list (extras.c). */

#include "ipodui/screen_stack.h"

#define CLOCKS_MAX  6                   /* Q13 */

extern const struct ui_screen_ops ui_clocks_screen;

#endif /* IPODUI_SCREENS_CLOCKS_H */
