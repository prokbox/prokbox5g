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

/* iPod UI shell: MOD_CALENDARS's export table (P12 W3-C): slot 0 its module
 * header, slot 1 the Calendars list (its root screen, extras_roots.h).
 * Built as a module (IPODUI_IN_MODULE) or linked statically as
 * mod_exports_calendars (the simulator, a resident build). */
#include "config.h"
#include "lang.h"
#include "ipodui/modload.h"
#include "ipodui/screen_stack.h"
#include "ipodui/screens/calendars.h"

/* The working block (screens/calendars.c CAL_WORK_KB): 12 KB from the
 * module arena (calendars.mod's work); 7 KB from the screen arena in a
 * resident build */
#ifdef IPODUI_MODULES
MOD_DESC_A(calendars, LANG_IPODUI_X_CALENDARS, 12);
#else
MOD_DESC_A(calendars, LANG_IPODUI_X_CALENDARS, 7);
#endif

MOD_EXPORTS(calendars, MOD_SLOT_FIRST + 1) = {
    [MOD_SLOT_DESC]  = &mod_desc_calendars,
    [MOD_SLOT_FIRST] = &ui_x_calendars_screen,
};
