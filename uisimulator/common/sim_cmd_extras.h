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
 * P12 sim extras (docs/phase12-brief.md 3.6): see sim_cmd_extras.h.
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
#ifndef SIM_CMD_EXTRAS_H
#define SIM_CMD_EXTRAS_H

#include <stdbool.h>

/* P12 sim commands (HAVE_IPOD_UI builds; stock builds report "unavailable").
 * Syntax is documented in tests/ui/README.md "Phase 12".
 *
 *   alarm_boot <s>      the NEXT boot is a power-on by the RTC alarm, <s>
 *                       seconds after the armed time (only if the PMU holds
 *                       a wake: alarm registers enabled and RTCWAK set at
 *                       power-off; else a plain boot, logged)
 *   pmu_arm HH:MM:SS [YYYY-MM-DD]   what rtc_set_alarm + rtc_enable_alarm
 *                       write (daily match, or the exact date) + RTCWAK
 *   pmu_disarm          rtc_enable_alarm(false): year 0x99, RTCWAK clear
 *   pmu_fire            latch INT1 bit 7 now (the alarm matched)
 *   pmu_dump            log the alarm registers, INT1, RTCWAK
 *   expect_pmu on|off|any [hms=HH:MM:SS] [date=daily|YYYY-MM-DD]
 *              [rtcwak=0|1] [bootwake=0|1] [int1=0|1] [next=<rtc s>]
 *   rtc_at YYYY-MM-DD HH:MM:SS      rtc_set by calendar time
 *   piezo_mark / expect_piezo <min> [max]   piezo plays since the mark
 *   piezo_test <hz> <ms>    one piezo_play_for_tick (drives the trace)
 *   press_hold <A> <B> <ms> [lead_ms]       two buttons, overlapping
 *   wheel_at <pos> <ms> [KEY | to_pos]      a finger on the ring at pos
 *                       (0..95, 0 at MENU, clockwise) for ms, then lifted
 *                       (sdk_wheel_pos(), #132); KEY: that button pressed
 *                       under it meanwhile; to_pos: it slides there
 */
bool sim_cmd_extras(const char *cmd, char *argv[], int argc, int lineno);

/* sim_script.c: the SDL key for a script key name (MENU, SELECT, ...), or -1 */
int sim_script_key(const char *name);

#endif /* SIM_CMD_EXTRAS_H */
