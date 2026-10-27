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

/* The Extras whose module has landed: ONE line per Extra, added by its
 * package at merge (the orchestrator merges; keep the extra_id order).
 *
 *   XT_ROOT(extra, name, panel)
 *     extra  enum extra_id (extras.h)
 *     name   the module: modules/<name>_mod.c defines
 *              MOD_DESC_A(<name>, LANG_IPODUI_X_..., <work KB>);
 *              MOD_EXPORTS(<name>, n) = { &mod_desc_<name>, &<root ops>, ... };
 *            and is built as the module (modules.make, a <name>.mod
 *            manifest) or, in the simulator and a resident build, linked
 *            statically (apps/SOURCES)
 *     panel  its row's resident panel (const struct ui_preview_ops *), or
 *            NULL for the table's default (screens/panels.c)
 *
 * The header that declares the panel goes in the #include list below.
 * No Extra has landed on p12 at W1-F: every row is a placeholder. */

/* (panel headers are included by extras.c: this file is also read inside
 * roots[]'s initializer on the target, where an include cannot declare) */

XT_ROOT(XT_CLOCKS, clocks, NULL)
XT_ROOT(XT_ALARMS, alarms, NULL)
XT_ROOT(XT_STOPWATCH, stopwatch, &ui_preview_x_stopwatch)
XT_ROOT(XT_SCREENLOCK, lockset, NULL)
XT_ROOT(XT_CONTACTS, contacts, NULL)
XT_ROOT(XT_NOTES, notes, NULL)
XT_ROOT(XT_CALENDARS, calendars, NULL)
