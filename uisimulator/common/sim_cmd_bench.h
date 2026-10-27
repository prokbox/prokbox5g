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

/* The bench session's script command in the shell simulator with the video
 * player (apps/ipodui/devbench_sim.h, DEVBENCH_SIM):
 *   bench_sim reset                 no results files, the model's defaults
 *   bench_sim set <switch> [text]   a model switch or fault (noflags,
 *                                   lowmem, putfail <n>, fail_<step id>)
 *   bench_sim clear <switch>
 *   bench_sim expect <checks...>    the newest results file: order, ended,
 *                                   verified|noverified, token|notoken,
 *                                   <id>=<pass|fail|skip|done|autoskip>;
 *                                   "expect_bench failed: ..." fails the run
 *   bench_sim dump                  the newest results file, to the log
 * In other builds the command is unknown ("bad command"). */
#ifndef SIM_CMD_BENCH_H
#define SIM_CMD_BENCH_H

#include <stdbool.h>

bool sim_cmd_bench(const char *cmd, char *argv[], int argc, int lineno);

#endif
