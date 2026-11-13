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

#ifndef IPODUI_BATTCELL_H
#define IPODUI_BATTCELL_H

#include <stdbool.h>

/* The fitted cell (one-build brief WP3, #205): one build for every unit,
 * so the battery is asked once, at first start (screens/battq.c), and
 * stored in the hidden setting ipodui_battery_cell (enum battcell,
 * battcurve.h; kept by Reset Settings: it is a fact about the unit, not
 * a preference). The answer sets:
 *  - the capacity (650/850 mAh for the original cell by RAM, 2,000,
 *    3,000): powermgmt's estimate and the deep-sleep timing (sleep.c);
 *  - the curve: the storage's base row (powermgmt_storage_init(), right
 *    after storage_init()) scaled by the cell's sag (battcurve_scale()),
 *    copied into the power thread's table through batcal_seed() (an
 *    IRQ-masked 22 B copy), batcal's seed for its learning.
 * Unchanged: disk-safe, shutoff, the boot guard, charging.
 * Not answered yet: the base row as it is and the original cell's
 * capacity (the safe side).
 *
 * Simulator: a scripted run (tests/ui) never asks, and counts as
 * answered "3,000 mAh" (the values every script was made with), unless
 * "# env: IPODUI_SIM_BATTQ=1" (the stored answer decides) or "=clear"
 * (the run's first boot clears it, as IPODUI_SIM_NOTICE does). */

/* apps/main.c, once, right after settings_load() (before
 * init_battery_tables(), so a battery_levels.cfg still wins): the stored
 * answer applied. */
void battcell_boot_apply(void);
/* ui_main.c, at shell start: the boot log line (cell, capacity, the
 * table in use) */
void battcell_log(void);
/* ui_main.c, at shell start, before ui_notice_boot() (the legal notice
 * comes first, the question under it): push the question when there is
 * no answer. Not over an alarm's ring (next boot then). */
void battcell_ask(void);
/* true while the question is the top screen: the main loop keeps the
 * global holds (MENU to the root, PLAY to sleep) off it, as for the
 * legal notice */
bool battcell_question_up(void);
/* The question's Select: store the answer (deferred settings write),
 * apply it, re-seed batcal; a changed answer discards the learned
 * table. The caller pops. */
void battcell_answer(int cell);

#endif /* IPODUI_BATTCELL_H */
