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

/* The bench_sim script command (sim_cmd_bench.h): handed to the shell's
 * bench model, apps/ipodui/devbench_sim.c, in a build that has it */
#include <string.h>
#include "config.h"
#include "sim_cmd_bench.h"
#ifdef HAVE_IPOD_UI
#include "ipodui/devbench_sim.h"
#endif

bool sim_cmd_bench(const char *cmd, char *argv[], int argc, int lineno)
{
    if (strcmp(cmd, "bench_sim"))
        return false;
#ifdef DEVBENCH_SIM
    return devbench_sim_cmd(argv, argc, lineno);
#else
    (void)argv; (void)argc; (void)lineno;
    return false;
#endif
}
