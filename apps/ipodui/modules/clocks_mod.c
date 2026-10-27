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
 * iPod UI shell: module A, the cold Settings screens (#106): its export table
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
/* MOD_CLOCKS (P12 W1-C): Extras > Clocks. Built as a module
 * (IPODUI_IN_MODULE): module.lds puts this table at the image start; in
 * the simulator and a resident build it is mod_exports_clocks. Slot 0 is
 * the module header, MOD_SLOT_FIRST the root screen. Work: 1 KB (the
 * page's state and its menu list, screens/clocks.c). */
#include "config.h"
#include "lang.h"
#include "ipodui/modload.h"
#include "ipodui/screen_stack.h"
#include "ipodui/screens/clocks.h"

MOD_DESC_A(clocks, LANG_IPODUI_X_CLOCKS, 1);

MOD_EXPORTS(clocks, MOD_SLOT_FIRST + 1) = {
    [MOD_SLOT_DESC]  = &mod_desc_clocks,
    [MOD_SLOT_FIRST] = &ui_clocks_screen,
};
