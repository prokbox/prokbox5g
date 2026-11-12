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

#ifndef _IPODUI_PRODVER_H_
#define _IPODUI_PRODVER_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lang.h"
#include "version.h"

/* The product line for About, the first-boot notice and the diagnostics
 * report: LANG_IPODUI_VERSION_STRING ("ProkBox5g %s", localisable) with the
 * version the build took from git tags (scripts/version.sh, via
 * prokversion in version.h). full = false drops the "+g<sha>[-dirty]" build
 * metadata of a dev build (the Build row already shows the commit); the
 * diagnostics report keeps it. Always NUL-terminated, truncated to n. A scripted
 * simulator run (goldens) SIMULATOR builds only (never a target build) show the fixed "0.0.0-sim", so goldens never drift, unless IPODUI_SIM_VERSION names another (docs screenshots). Header-only: legal.c may be a
 * module and must not need a core symbol. */
static inline const char *ipodui_version_string(char *buf, size_t n, bool full)
{
    char ver[48];
    const char *pv = prokversion;
    size_t len;
    const char *plus;

#ifdef SIMULATOR
    pv = "0.0.0-sim";                /* goldens don't follow tags or commits */
    {
        /* docs screenshots only (scripts/docs_shots.sh): the release's
         * version, so the guide's first-start notice shows what the
         * release says.  Simulator builds only. */
        const char *ov = getenv("IPODUI_SIM_VERSION");

        if (ov && *ov)
            pv = ov;
    }
#endif
    len = strlen(pv);
    plus = strchr(pv, '+');

    if (!full && plus)
        len = plus - pv;
    if (len >= sizeof(ver))
        len = sizeof(ver) - 1;
    memcpy(ver, pv, len);
    ver[len] = '\0';
    snprintf(buf, n, str(LANG_IPODUI_VERSION_STRING), ver);
    return buf;
}

#endif
