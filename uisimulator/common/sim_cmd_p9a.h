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
#ifndef SIM_CMD_P9A_H
#define SIM_CMD_P9A_H

#include <stdbool.h>

/* Phase 9 Part 1, agent A (docs/phase9-brief.md Rev 1 H1/H2): database
 * read-backs for the trim scripts (ipodui-p9-stats-playcount, -usb-rescan,
 * -rating-persist). Sim paths write a space as %20. <tag> is one of
 * playcount, playtime, lastplayed, lastelapsed, lastoffset, rating.
 *
 *   stat_mark <path> <tag>   remember the track's current value
 *   expect_stat <path> <tag>=<lo>..<hi|max> [wait_ms]
 *                            the value is within lo..hi (polled up to
 *                            wait_ms, default 3000); logs
 *                            "expect_stat ok" or "expect_stat failed"
 *                            (-2: not in the database)
 *   expect_stat_delta <path> <tag>=<lo>..<hi|max> [wait_ms]
 *                            the same for value - the stat_mark value
 *   expect_in_db <path> [wait_ms]
 *                            the file has a database entry (default wait
 *                            30000): "expect_in_db ok|failed"
 *   expect_not_in_db <path> [wait_ms]
 *                            no live database entry for the file within
 *                            wait_ms (default 30000; #89 H2)
 *
 * A failed expect_* fails the run in scripts/shots.sh. Returns false for
 * any other command. */
bool sim_cmd_p9a(const char *cmd, char *argv[], int argc, int lineno);

#endif /* SIM_CMD_P9A_H */
