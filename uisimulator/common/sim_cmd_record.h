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
 * Revision 2 (docs/phase-nav-brief.md, owner request) sim script command
 * "record", for the owner-round review videos. Kept apart from
 * sim_script.c (docs/process.md: new sim commands get their own
 * sim_cmd_*.c file). Registered from sim_script.c's run_line() via
 * sim_cmd_record(); see sim_cmd_record.c.
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
#ifndef SIM_CMD_RECORD_H
#define SIM_CMD_RECORD_H

#include <stdbool.h>

/* Handles this sim_script.c command:
 *
 *   record <ms> <fps> <name>   dumps the LCD at a steady 'fps' (clamped
 *                        1..30) for 'ms', into <shotdir>/<name>_frames/
 *                        frame%05d.bmp (scripts/make_video.sh stitches
 *                        them into a video). Frame pacing uses an
 *                        accumulator (like sleep_ms, but per frame), so a
 *                        fps that doesn't divide 1000 (24, e.g.) doesn't
 *                        drift over a long run. A second "record" with
 *                        the SAME name in the same script continues
 *                        numbering from where the first left off (for a
 *                        video built from several non-contiguous bursts,
 *                        e.g. a push and, later, its pop); a new name
 *                        starts a fresh sequence in its own directory.
 *                        Nothing here drives the backlight, input or any
 *                        other sim state: pin the clock, battery and (#38
 *                        scripts) the drift phase with their own commands
 *                        first, for a deterministic video. Logs "record
 *                        <name> N frames (<ms> ms at <fps> fps)".
 *
 * Returns false for any other command. */
bool sim_cmd_record(const char *cmd, char *argv[], int argc, int lineno);

#endif /* SIM_CMD_RECORD_H */
