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
 * Fit-3a (#74) sim script command "list_force_bar"; see sim_cmd_fit3a.h.
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
#include <string.h>
#include "config.h"
#include "sim_cmd_fit3a.h"

/* apps/ipodui/list.c's SIMULATOR-only setter. Weak, so this file links in
 * a stock sim (no apps/ipodui); list.c defines it strongly in the
 * --ipodui sim (the sim_cmd_nav.c pattern). */
void ui_list_force_bar(bool on) __attribute__((weak));
void ui_list_force_bar(bool on)
{
    (void)on;
}

bool sim_cmd_fit3a(const char *cmd, char *argv[], int argc, int lineno)
{
    if (!strcmp(cmd, "list_force_bar") && argc >= 2)
    {
        bool on = !strcmp(argv[1], "on");

        if (!on && strcmp(argv[1], "off"))
        {
            fprintf(stderr, "sim_script:%d: list_force_bar failed: want "
                    "on|off, got %s\n", lineno, argv[1]);
            return true;
        }
        ui_list_force_bar(on);
        fprintf(stderr, "sim_script: list_force_bar %s\n", argv[1]);
        return true;
    }
    return false;
}
