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
 * Simulator script commands: the paused maintenance window (Phase 16-1)
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
#ifndef SIM_CMD_MAINT_H
#define SIM_CMD_MAINT_H

#include <stdbool.h>

/* The paused maintenance window (Phase 16-1, #88; apps/ipodui/maint.h).
 * Handles this sim_script.c command:
 *
 *   pause_age <s>     the current pause began <s> seconds ago (the
 *                     window's 3 min threshold without the wait); a
 *                     no-op unless paused, or in a build without the
 *                     shell ("pause_age unavailable")
 *
 *   maint_try <B>     maint_take(commit, B) from the script, then a
 *                     core_alloc(B): "maint try ok= alloc= shrink=" (the
 *                     allocation must not shrink the audio buffer)
 *   expect_title      the current track (what Now Playing draws) has a
 *                     title and a length, else "expect_title failed"
 *   give_hold <ms>    the next window restart holds this long after the
 *                     buffer change (events queue up meanwhile)
 *
 * The window's own lines are "ipodui: maint ..." in sim.log (# log:).
 * Returns false for any other command. */
bool sim_cmd_maint(const char *cmd, char *argv[], int argc, int lineno);

#endif
