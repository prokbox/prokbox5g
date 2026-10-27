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
 * Phase 6 (Audiobooks) sim script commands; see sim_cmd_ab.c.
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
#ifndef SIM_CMD_AB_H
#define SIM_CMD_AB_H

#include <stdbool.h>

/* Commands (documented in tests/ui/README.md, "Phase 6"):
 *
 *   ab_power_cut now|exit <kind> [arg]
 *   ab_touch <simpath>
 *   ab_mark <text>
 *   ab_mem_mark
 *   expect_ab_mem_same
 *   expect_ab_store <started|-1|geN> <finished|-1|geN> [wait_ms]
 *   expect_ab_store_own <started|-1|geN> <finished|-1|geN> [wait_ms]
 *   expect_ab_file <store|tmp|pak|idx|pak_tmp> present|absent [wait_ms]
 *   expect_ab_pak <books> [wait_ms]
 *   ab_pak_stat
 *   ab_pak_mark / expect_ab_pak_same <ms> / expect_ab_pak_changed <ms>
 *   ab_copy_at_exit <hostsrc> <simdst>
 *
 * Returns false for any other command. */
bool sim_cmd_ab(const char *cmd, char *argv[], int argc, int lineno);

#endif /* SIM_CMD_AB_H */
