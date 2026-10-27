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
 * Phase 9 sim commands (docs/phase9-brief.md section 4.4; owner T).
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
#ifndef SIM_CMD_P9_H
#define SIM_CMD_P9_H

#include <stdbool.h>

/* Handles these sim_script.c commands; output goes to sim.log (stderr),
 * one record per line, stable for scripts/check_trace.py:
 *
 *   alloc_dump           every live buflib (core_alloc) allocation:
 *                          p9 alloc name=<ops> size=<bytes> handle=<h> pins=<n>
 *                                   move=<move_callback>
 *                        then
 *                          p9 alloc total=<bytes> count=<n> free=<bytes>
 *                        buflib keeps no allocation names, so name= is the
 *                        symbol of the block's buflib_callbacks (dladdr;
 *                        "none" for NULL ops, the hex address when the
 *                        symbol is local); move= names the ops' move
 *                        callback the same way, which tells apart the
 *                        many statics called "ops". size= is the usable size
 *                        (block length minus buflib's header).
 *   open_count           file opens (sim_open, any thread) since boot:
 *                          p9 opens=<n>
 *   run_rock <path> [param]
 *                        only with --ipodui-plugins (HAVE_IPOD_UI_PLUGINS):
 *                        plugin_load(path, param) on the script thread,
 *                          p9 rock rc=<plugin status>
 *                        The script is blocked until the rock returns, so
 *                        only a rock that finishes without input fits;
 *                        without the plugins build it logs
 *                          p9 rock unsupported
 *                        and fails the script ("expect_rock failed").
 *   lang_set <name>|-    global_settings.lang_file = name ("-" clears),
 *                        settings_save(); "reboot" then loads it at boot:
 *                          p9 lang set '<name>'
 *   lang_load <path> <rc>
 *                        lang_core_load(path) now; fails ("expect_lang
 *                        failed") unless it returns rc (0 loaded, 2 refused):
 *                          p9 lang load rc=<rc> size=<core lang_size>
 *   lang_str <english> <expected>
 *                        str() of the English string's id must equal
 *                        expected ('_' for a space in either):
 *                          p9 lang str id=<id> '<english>' = '<text>'
 *   lang_alloc_check <path>
 *                        exactly one pinned block without callbacks whose
 *                        size is the file's core lang_size (rounded up to
 *                        buflib's unit), the language block (IPODUI_TRIM_MISC):
 *                          p9 lang alloc ok want=<n> size=<n> handle=<h> */
bool sim_cmd_p9(const char *cmd, char *argv[], int argc, int lineno);

#endif /* SIM_CMD_P9_H */
