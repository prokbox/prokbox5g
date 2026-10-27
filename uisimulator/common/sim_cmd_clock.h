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
 * Phase 4b-2 sim script commands for the shell's Time in Title header
 * (docs/design/7g-spec.md, Global chrome), kept apart from sim_script.c
 * (docs/process.md: new sim commands get their own sim_cmd_*.c file).
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
#ifndef SIM_CMD_CLOCK_H
#define SIM_CMD_CLOCK_H

#include <stdbool.h>

/* Handles these sim_script.c commands:
 *
 *   title_phase title|clock|auto   freeze the header text on the title or
 *                                  the clock, or let it alternate (2 s /
 *                                  4 s, as on the device). A scripted run
 *                                  starts frozen on the title, so golden
 *                                  shots never depend on timing; the sim
 *                                  run by hand alternates from the start
 *   sim_clock HH:MM|rtc            the header clock shows HH:MM instead
 *                                  of the host's time (rtc: host time)
 *   clock_format 12|24             the 24 Hour Clock setting
 *                                  (global_settings.timeformat)
 *   battery <0-100>|drain          the simulated battery level. A
 *                                  scripted run starts pinned at 100 %
 *                                  (powermgmt-sim.c), so the header's
 *                                  battery glyph never depends on how
 *                                  long the run took; drain: the sim's
 *                                  own 1 %/s drain and recharge
 *
 * The shell (apps/ipodui/header.c, SIMULATOR only) reads them through
 * sim_header_phase() and sim_header_clock() (panels.c: sim_panel_clock()).
 * Returns false for any other
 * command. */
bool sim_cmd_clock(const char *cmd, char *argv[], int argc, int lineno);

/* sleep_warp: a pinned sim_clock advances by s seconds */
void sim_clock_advance(long s);

int sim_header_phase(void);     /* -1 alternate, 0 title, 1 clock */
int sim_header_clock(void);     /* -1 host time, else minute of the day */
/* The split-screen panels' clock and calendar (#38, Extras clock, the
 * calendar tile): like sim_header_clock(), but a scripted run that never
 * said "sim_clock" gets a fixed 10:08 (and the panels a fixed date, see
 * apps/ipodui/screens/panels.c), so goldens never depend on the host's
 * clock; "sim_clock rtc" opts back in to the host time. */
int sim_panel_clock(void);
void sim_battery_pin(int percent); /* powermgmt-sim.c; -1: drain */

#endif /* SIM_CMD_CLOCK_H */
