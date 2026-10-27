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
/* Built as a module (IPODUI_IN_MODULE): module.lds puts this table at
 * the image start. In the simulator (static modules) it is
 * mod_exports_settings. Slot 0 is the module header; then modload.h's
 * MOD_SET_* slots. */
#include "config.h"
#include "lang.h"
#include "ipodui/modload.h"
#include "ipodui/screen_stack.h"
#include "ipodui/screens/screens.h"
#include "ipodui/screens/checklist.h"

#ifndef IPODUI_IN_MODULE
/* the real screens, under their own names (modload.h renames them for
 * the core's callers) */
#undef ui_legal_screen
#undef ui_datetime_menu_screen
#undef ui_checklist_screen
extern const struct ui_screen_ops ui_legal_screen, ui_datetime_menu_screen,
    ui_checklist_screen;
#endif

MOD_DESC_A(settings, LANG_SETTINGS, 0);

MOD_EXPORTS(settings, MOD_SET__N) = {
    [MOD_SLOT_DESC]     = &mod_desc_settings,
    [MOD_SET_LEGAL]     = &ui_legal_screen,
    [MOD_SET_DATETIME]  = &ui_datetime_menu_screen,
    [MOD_SET_CHECKLIST] = &ui_checklist_screen,
};
