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
 * Phase 5 (Now Playing, docs/phase5-brief.md) sim script commands.
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
#ifndef SIM_CMD_NP_H
#define SIM_CMD_NP_H

#include <stdbool.h>

/* Handles these sim_script.c commands (documented in tests/ui/README.md):
 *
 *   np_pin <ms|off> [<vol%>|off]  pins Now Playing's elapsed time and the
 *                         volume bar's percentage (np_pb_sim_pin())
 *   charger on|off        sets charger_input_state (the sim pins the
 *                         battery under a script, so nothing else moves it;
 *                         a pinned battery below 100 % reports charging
 *                         while it is on)
 *   expect_rating <path> <0-10> [wait_ms]   the file's tag_rating (stars
 *                         x 2) in the database; "expect_rating ok|failed"
 *   expect_pos <min_ms> <max_ms>   the current track's elapsed time lies
 *                         in the range; "expect_pos ok|failed"
 *   expect_albums <n> [max_runs]  the next n queue entries: one contiguous
 *                         run per album, at most max_runs runs
 *   play_files <path>...  starts a playlist of those sim paths (no
 *                         database, no shell screen push)
 *   repeat off|all|one    sets the Repeat setting
 *   dark on|off           backlight off / on (the shell is then "dark")
 *   mkdir <path>          makes a directory in the sim disk
 *
 * Returns false for any other command. */
bool sim_cmd_np(const char *cmd, char *argv[], int argc, int lineno);

#endif /* SIM_CMD_NP_H */
