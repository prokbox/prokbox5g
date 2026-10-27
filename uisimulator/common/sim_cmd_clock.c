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
 * Sim script commands "title_phase", "sim_clock", "clock_format" and
 * "battery" for the header (Time in Title, battery glyph); see
 * sim_cmd_clock.h. Test-only: the
 * shell reads these hooks only in the simulator.
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
#include "settings.h"
#include "sim_cmd_clock.h"

extern const char *sim_script_file;

static int phase = -2;          /* -2: not set (title under a script) */
static int clock_min = -2;      /* -2: not set, -1: rtc, else minute */

/* The default under a script for the panels (sim_panel_clock()): */
#define PANEL_CLOCK_DEFAULT (10 * 60 + 8)

int sim_header_phase(void)
{
    if (phase == -2)
        return sim_script_file ? 0 : -1;
    return phase;
}

int sim_header_clock(void)
{
    return clock_min < 0 ? -1 : clock_min;
}

int sim_panel_clock(void)
{
    if (clock_min == -2)
        return sim_script_file ? PANEL_CLOCK_DEFAULT : -1;
    return clock_min;
}

/* sleep_warp (sim_cmd_sleep.c): a pinned clock moves on with the warp */
void sim_clock_advance(long s)
{
    static long rem;            /* seconds short of a minute */

    if (clock_min == -2 && sim_script_file)
        clock_min = PANEL_CLOCK_DEFAULT;
    if (clock_min < 0)
        return;                 /* host time: the RTC offset moves it */
    rem += s;
    clock_min = (int)((clock_min + rem / 60) % (24 * 60));
    rem %= 60;
}

bool sim_cmd_clock(const char *cmd, char *argv[], int argc, int lineno)
{
    if (!strcmp(cmd, "title_phase") && argc >= 2)
    {
        if (!strcmp(argv[1], "title"))
            phase = 0;
        else if (!strcmp(argv[1], "clock"))
            phase = 1;
        else if (!strcmp(argv[1], "auto"))
            phase = -1;
        else
        {
            fprintf(stderr, "sim_script:%d: title_phase title|clock|auto\n",
                    lineno);
            return true;
        }
        fprintf(stderr, "sim_script: title_phase %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "sim_clock") && argc >= 2)
    {
        int h, m;

        if (!strcmp(argv[1], "rtc"))
            clock_min = -1;
        else if (sscanf(argv[1], "%d:%d", &h, &m) == 2
                 && h >= 0 && h < 24 && m >= 0 && m < 60)
            clock_min = h * 60 + m;
        else
        {
            fprintf(stderr, "sim_script:%d: sim_clock HH:MM|rtc\n", lineno);
            return true;
        }
        fprintf(stderr, "sim_script: sim_clock %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "battery") && argc >= 2)
    {
        int pct = atoi(argv[1]);

        if (!strcmp(argv[1], "drain"))
            sim_battery_pin(-1);
        else if (pct >= 0 && pct <= 100)
            sim_battery_pin(pct);
        else
        {
            fprintf(stderr, "sim_script:%d: battery <0-100>|drain\n", lineno);
            return true;
        }
        fprintf(stderr, "sim_script: battery %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "clock_format") && argc >= 2)
    {
        global_settings.timeformat = atoi(argv[1]) == 12 ? 1 : 0;
        fprintf(stderr, "sim_script: clock_format %s\n",
                global_settings.timeformat ? "12" : "24");
        return true;
    }
    return false;
}
