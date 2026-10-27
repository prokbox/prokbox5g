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

/* #114 sim script commands for the drive power model (filesystem-sim.c):
 *   stor_tune <hold_ms> <min_off_ms>  shorter holds, so a script sees
 *                                     several refills in seconds
 *   stor_switch on|off                the developer switch (off: standby)
 *   stor_buffer <bytes>               shrink the audio buffer (stopped)
 *   stor_save                         settings_save() (a write)
 *   stor_mark                         the base for the counts below
 *   stor_status                       "sim_script: stor on=.. ons=.."
 *   stor_expect on|off [max_ms]       the state now, or within max_ms
 *   stor_expect_cycles_ge <n>         power-ons since stor_mark
 *   stor_expect_cycles_le <n>
 * A failed check prints "sim_script:<line>: stor_... failed". */
#ifndef SIM_CMD_STORPWR_H
#define SIM_CMD_STORPWR_H

#include <stdbool.h>

bool sim_cmd_storpwr(const char *cmd, char *argv[], int argc, int lineno);

#endif
