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
 * iPod UI shell: which Main Menu / Music Menu rows are shown (Phase 11)
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

/* menu.c asks ui_mm_visible() / ui_mu_visible() per row on every rebuild
 * of its visible-row map: one load, a shift and a mask (no I/O, no
 * allocation). A bit is shown when the setting has it AND the screen
 * ships (UI_MM_OFFERED / UI_MU_OFFERED, compile-time); Settings is
 * always shown. */
#include "config.h"
#include "settings.h"
#include "ipodui/menucfg.h"

bool ui_mm_visible(enum ui_mm_bit b)
{
    if (b == UI_MM_SETTINGS)
        return true;
    return (((unsigned)global_settings.ipodui_main_menu & UI_MM_OFFERED)
            >> b) & 1u;
}

bool ui_mu_visible(enum ui_mu_bit b)
{
    return (((unsigned)global_settings.ipodui_music_menu & UI_MU_OFFERED)
            >> b) & 1u;
}
