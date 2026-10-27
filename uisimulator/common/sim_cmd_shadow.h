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
 * Phase 8b close: sim script command "expect_shadow".
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
#ifndef SIM_CMD_SHADOW_H
#define SIM_CMD_SHADOW_H

#include <stdbool.h>

/* Handles this sim_script.c command:
 *
 *   expect_shadow <label> [tol_per_mille [min_sum]]
 *                         reads the LCD framebuffer's panel (x 160..319)
 *                         and checks the split shadow at the seam (panel
 *                         columns 0..9, alphas 92 75 59 45 33 23 15 8 4 1
 *                         of 256): the mean brightness of columns 0..4
 *                         against the plain columns 12..27 beside them is
 *                         the expected darkening within tol (default 80 =
 *                         8 %), and columns 0..2 each within twice that.
 *                         Logs "expect_shadow <label> ..." with the
 *                         values; "expect_shadow failed (...)" (a failure
 *                         for scripts/shots.sh) when the shadow is not
 *                         applied, applied twice, or too few
 *                         flat rows were found to judge.
 *
 * Returns false for any other command. */
bool sim_cmd_shadow(const char *cmd, char *argv[], int argc, int lineno);

#endif /* SIM_CMD_SHADOW_H */
