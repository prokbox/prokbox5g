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
 * Sim script command "mod_delay": the module loader's stand-ins (#106).
 * Test-only.
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
/* #106: the module loader's stand-ins in a modules build (-DIPODUI_WITH_
 * MODULES): `mod_delay <ms>` makes the next module loads land that much
 * later (the stand-in, then the swap), -1 makes them fail (the error
 * page), 0 is back to at once. A no-op in a resident build. */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim_cmd_mod.h"
#include "ipodui/modload.h"

bool sim_cmd_mod(const char *cmd, char *argv[], int argc, int lineno)
{
    (void)lineno;
    if (strcmp(cmd, "mod_delay") || argc < 2)
        return false;
#ifdef IPODUI_MODULES
    mod_sim_delay(atoi(argv[1]));
    fprintf(stderr, "sim_script: mod_delay %s\n", argv[1]);
#else
    fprintf(stderr, "sim_script: mod_delay %s (resident build: ignored)\n",
            argv[1]);
#endif
    return true;
}
