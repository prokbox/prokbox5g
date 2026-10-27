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
 * Phase 11 sim commands (docs/phase11-brief.md section 8; the contract
 * commit's set, T adds more).
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
#ifndef SIM_CMD_P11_H
#define SIM_CMD_P11_H

#include <stdbool.h>

/* Handles these sim_script.c commands (all log "sim_script: <cmd> ..."; a
 * failure logs "expect_<what> failed", which scripts/shots.sh turns into
 * a script failure):
 *
 *   usb_eject [off]      every LUN counts as ejected by the host (the sim
 *                        has no USB stack: usb_storage_all_ejected(),
 *                        sysscreen.h, reads this flag); "off" clears it.
 *                        Typically: usb_connect, usb_eject, shot,
 *                        usb_disconnect, usb_eject off.
 *   type <chars>         drives the Search letter strip as a user would:
 *                        per character, wheel clicks (slow, no
 *                        acceleration) from the boxed letter to it, then
 *                        SELECT; a space ("%20") presses NEXT, "<" presses
 *                        PREV (delete). Letters and digits only
 *                        (SRCH_ALPHABET, search.h). Needs the strip on
 *                        screen (ui_search_strip_pos() >= 0): else, or if
 *                        the box doesn't land on the letter,
 *                        "expect_type failed".
 *   expect_queue <n> <first-path>
 *                        the current playlist has <n> entries and its
 *                        first (display order) entry's path ends with
 *                        <first-path> (%20 = space): "expect_queue ok",
 *                        else "expect_queue failed". "-" skips the path.
 *   time_in_title on|off
 *                        sets global_settings.ipodui_time_in_title and
 *                        calls ui_header_set_time_in_title() (the setting
 *                        defaults to Off since Phase 11; scripts that
 *                        need the header clock turn it on first). Logs
 *                        "time_in_title on|off". Without the shell it only
 *                        logs. A "reboot" restores the saved setting.
 *   otg_dump             the On-The-Go store (otg.h) to sim.log: the
 *                        list's lines, the idx header and its records
 *   db_rebuild_keep_otg [ms]
 *                        rebuild_db (the database from scratch, wait up
 *                        to ms, default 30000) and check the On-The-Go
 *                        files are byte-identical afterwards:
 *                        "db_rebuild_keep_otg ok lines=<n>", else
 *                        "expect_otg_kept failed" (a timeout logs
 *                        "rebuild_db timed out", also a failure). */
bool sim_cmd_p11(const char *cmd, char *argv[], int argc, int lineno);

#endif /* SIM_CMD_P11_H */
