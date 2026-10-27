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
 * iPod UI shell: module MOD_LOCKSET, Extras > Screen Lock (P12 W1-L)
 * Export table: slot 0 the header, slot 1 the root screen
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

/* Built as a module (IPODUI_IN_MODULE): module.lds puts this table at the
 * image start. Linked statically (the simulator, a resident build) it is
 * mod_exports_lockset, found through extras_roots.h. */
#include "config.h"
#include "lang.h"
#include "ipodui/modload.h"
#include "ipodui/screen_stack.h"

extern const struct ui_screen_ops ui_lockset_screen;   /* screens/lockset.c */

MOD_DESC_A(lockset, LANG_IPODUI_X_SCREEN_LOCK, 0);

MOD_EXPORTS(lockset, MOD_SLOT_FIRST + 1) = {
    [MOD_SLOT_DESC]  = &mod_desc_lockset,
    [MOD_SLOT_FIRST] = &ui_lockset_screen,
};
