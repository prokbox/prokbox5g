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

/* The video chip's clock gate: may this play run the BCM at 150 MHz?
 *
 * Two kinds of device evidence open it, both read at every play's start
 * (vplayer.c check_fast_clock, before the session's OPEN), so a unit
 * that earns either plays at 150 MHz from its next video on, with no
 * restart:
 *   - a clock150 token (bcmfw/verified.txt, compiled into
 *     bcmvideo_clock.h): the FNV-1a of a video image that played stage 6
 *     at 150 MHz on a real unit after stage 4c passed.  It names the
 *     image, and the player loads only the image its header names, so it
 *     holds for every unit running this build: the release's only way;
 *   - (developer builds) the unit's own stage 4b and 4c pass flags
 *     (bcm_devtest_fast_clock_allowed), as before.
 * Neither: 27 MHz.  The clock switch itself is Apple's sequence, in the
 * image (bcmfw/clock.c); this only decides whether OPEN asks for it.
 * Pure, so tests/video/test_vclock.c runs it on the host. */
#ifndef VCLOCK_GATE_H
#define VCLOCK_GATE_H
#include <stdbool.h>
#include <stdint.h>

enum vclock_why
{
    VCLOCK_TOKEN,       /* the image has a clock150 token */
    VCLOCK_DEV_FLAGS,   /* developer build: the unit's 4b and 4c flags */
    VCLOCK_NONE,        /* neither: 27 MHz */
};

/* image_fnv: the FNV-1a of the image this play loads (BCMVIDEO_IMAGE_FNV);
 * tok, n: the clock150 tokens; devtest: a developer build; dev_flags: its
 * stage 4b and 4c flags found (ignored without devtest) */
static inline enum vclock_why vclock_gate(uint32_t image_fnv,
                                          const uint32_t *tok, int n,
                                          bool devtest, bool dev_flags)
{
    int k;

    for (k = 0; k < n; k++)
        if (tok[k] == image_fnv && image_fnv)
            return VCLOCK_TOKEN;
    return devtest && dev_flags ? VCLOCK_DEV_FLAGS : VCLOCK_NONE;
}

#endif /* VCLOCK_GATE_H */
