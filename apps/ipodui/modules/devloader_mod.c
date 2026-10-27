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
 * iPod UI shell: module B, "Loader & storage" (#106): its export table
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
/* Built as a module (IPODUI_IN_MODULE): module.lds puts this table at
 * the image start. In the simulator (static modules) it is
 * mod_exports_devloader. Slot 0 is the module header; then modload.h's
 * MOD_DEV_* slots. Developer builds only. */
#include "config.h"
#include "ipodui/modload.h"
#include "ipodui/screen_stack.h"
#ifndef IPODUI_IN_MODULE
#undef ui_devloader_screen              /* the real one (modload.h) */
#endif
#include "ipodui/modules/devloader.h"

MOD_DESC_A(devloader, 0, 0);            /* developer only: no LANG title */

MOD_EXPORTS(devloader, MOD_DEV__N) = {
    [MOD_SLOT_DESC]  = &mod_desc_devloader,
    [MOD_DEV_SCREEN] = &ui_devloader_screen,
};
