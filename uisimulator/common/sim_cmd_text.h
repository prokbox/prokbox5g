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
 * Simulator script commands: what the LCD drew (p19-relvideos)
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
#ifndef SIM_CMD_TEXT_H
#define SIM_CMD_TEXT_H

#include <stdbool.h>

/* sim_cmd_text.c records every distinct string the LCD draws (shell
 * builds only; list rows drawn off screen count too). Handles these
 * sim_script.c commands:
 *
 *   text_mark                 forget what was drawn so far
 *   expect_drawn <text>       a string drawn since text_mark contains
 *                             <text> (case-sensitive; the words joined by
 *                             one space), else "expect_drawn failed"
 *   expect_not_drawn <text>   no such string, else "expect_not_drawn failed"
 *
 * Past 512 distinct strings since the mark, a needle not found fails
 * either expect rather than guess. A stock build returns false (the
 * commands are unknown there). */
bool sim_cmd_text(const char *cmd, char *argv[], int argc, int lineno);

#endif
