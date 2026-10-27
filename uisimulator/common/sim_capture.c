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
 * Phase 8a: sim_capture_screen(), see sim_capture.h. Dumps the current LCD
 * frame to $IPODUI_SHOT_DIR/<name>.bmp and logs "sim_script: shot <path>",
 * the line scripts/shots.sh converts to a PNG like any "shot" command. A
 * no-op when IPODUI_SHOT_DIR is unset or empty.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "config.h"
#include "screendump.h"
#include "sim_capture.h"

int sim_get_os_path(char *buffer, const char *path, size_t bufsize);

#define CAPTURE_TMP  "/.sim_capture.bmp"

void sim_capture_screen(const char *name)
{
    const char *dir = getenv("IPODUI_SHOT_DIR");
    char ospath[PATH_MAX];
    char dest[PATH_MAX + 64];

    if (!dir || !dir[0] || !name || !name[0])
        return;
    snprintf(dest, sizeof(dest), "%s/%s.bmp", dir, name);
    if (!screen_dump_to(CAPTURE_TMP)
        || sim_get_os_path(ospath, CAPTURE_TMP, sizeof(ospath)) < 0
        || rename(ospath, dest) != 0)
    {
        fprintf(stderr, "sim_script: capture %s failed\n", name);
        return;
    }
    fprintf(stderr, "sim_script: shot %s\n", dest);
}
