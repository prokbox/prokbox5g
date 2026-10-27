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
 * #38 (docs/phase-nav-brief.md) sim script command "drift_phase", kept
 * apart from sim_script.c (docs/process.md: new sim commands get their
 * own sim_cmd_*.c file). Registered from sim_script.c's run_line() via
 * sim_cmd_nav(); see sim_cmd_nav.c.
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
#ifndef SIM_CMD_NAV_H
#define SIM_CMD_NAV_H

#include <stdbool.h>

/* Handles this sim_script.c command:
 *
 *   drift_phase <ticks> [frozen]
 *   drift_phase live|frozen
 *                         A scripted run starts with the phase FROZEN
 *                         (preview.c; "# drift: live" in the script
 *                         un-freezes it from boot). <ticks> sets the
 *                         phase and lets it run from there, or keeps it
 *                         frozen there with "frozen"; live|frozen only
 *                         toggle. The original contract follows.
 *   drift_phase <ticks>   pins apps/ipodui/preview.h's panel phase (lit
 *                         ticks since shell start: drift framing, the
 *                         Extras clock) to an exact value, for
 *                         deterministic goldens and review videos. Calls
 *                         ui_panel_phase_set(), declared weak here so
 *                         this file (and so the sim it links into)
 *                         builds cleanly in a stock sim, which never
 *                         links apps/ipodui/preview.c's real (strong)
 *                         definition at all -- same pattern as
 *                         sim_cmd_art.c's sim_art_builder_idle().
 *
 * Returns false for any other command. */
bool sim_cmd_nav(const char *cmd, char *argv[], int argc, int lineno);

#endif /* SIM_CMD_NAV_H */
