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
 * #38 (docs/phase-nav-brief.md) sim script command "drift_phase"; see
 * sim_cmd_nav.h.
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

/* apps/ipodui/preview.h's SIMULATOR-only setter (lit-time panel phase:
 * drift framing, the Extras clock). Declared weak, with its exact
 * signature, so this file links in a stock sim, which never builds any
 * apps/ipodui file at all -- same pattern as sim_cmd_art.c's
 * sim_art_builder_idle(). apps/ipodui/preview.c now defines the same
 * symbol strongly (#ifdef SIMULATOR), which wins in the --ipodui sim. */
void ui_panel_phase_set(long ticks) __attribute__((weak));
void ui_panel_phase_set(long ticks)
{
    (void)ticks;
}
void ui_panel_phase_freeze(bool frozen) __attribute__((weak));
void ui_panel_phase_freeze(bool frozen)
{
    (void)frozen;
}

bool sim_cmd_nav(const char *cmd, char *argv[], int argc, int lineno)
{
    if (!strcmp(cmd, "drift_phase") && argc >= 2)
    {
        long ticks;
        bool frozen;

        /* drift_phase <ticks> [frozen]: live from <ticks> unless
         * "frozen"; drift_phase live|frozen: only (un)freeze */
        if (!strcmp(argv[1], "live") || !strcmp(argv[1], "frozen"))
        {
            ui_panel_phase_freeze(argv[1][0] == 'f');
            fprintf(stderr, "sim_script: drift_phase %s\n", argv[1]);
            return true;
        }
        ticks = atol(argv[1]);
        frozen = argc >= 3 && !strcmp(argv[2], "frozen");
        ui_panel_phase_freeze(frozen);
        ui_panel_phase_set(ticks);
        fprintf(stderr, "sim_script: drift_phase %ld%s\n", ticks,
                frozen ? " frozen" : "");
        return true;
    }
    (void)lineno;
    return false;
}
