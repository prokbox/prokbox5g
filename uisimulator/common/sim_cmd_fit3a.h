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
#ifndef SIM_CMD_FIT3A_H
#define SIM_CMD_FIT3A_H

#include <stdbool.h>

/* Fit-3a (docs/fit3-brief.md §3, #74): the scrollbar as an overlay that
 * never moves the text.
 *
 *   list_force_bar on|off    on: every long list's scrollbar shows and
 *                            stays (no hide); off: it hides at once.
 *                            Lets a script take the same list with and
 *                            without the bar (scripts/pngdiff.py --mask).
 *
 * Returns false for any other command. */
bool sim_cmd_fit3a(const char *cmd, char *argv[], int argc, int lineno);

#endif /* SIM_CMD_FIT3A_H */
