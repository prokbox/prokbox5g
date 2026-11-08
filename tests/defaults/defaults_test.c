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

/* Host test (#142): the compiled DEVICE defaults, not the simulator's.
 * Built as the shell on the device (HAVE_IPOD_UI, no SIMULATOR) and as the
 * simulator, so the sim's own backlight default cannot hide a regression. */
#include <stdio.h>
#include <stdbool.h>
#include "settings_defaults.h"

int main(void)
{
    int bad = 0;
#ifdef EXPECT_DEVICE
    if (DEFAULT_BACKLIGHT_TIMEOUT != 10) {
        printf("FAIL backlight default %d, want 10\n", DEFAULT_BACKLIGHT_TIMEOUT);
        bad++;
    }
#else /* simulator: Always On kept for the scripts */
    if (DEFAULT_BACKLIGHT_TIMEOUT != 0) { printf("FAIL sim backlight\n"); bad++; }
#endif
    if (DEFAULT_KEYCLICK <= 0 || !DEFAULT_KEYCLICK_HARDWARE) {
        printf("FAIL clicker default off (%d, hw %d), want on\n",
               DEFAULT_KEYCLICK, (int)DEFAULT_KEYCLICK_HARDWARE);
        bad++;
    }
    if (!bad)
        printf("defaults ok: backlight %d s, keyclick %d, hw %d\n",
               DEFAULT_BACKLIGHT_TIMEOUT, DEFAULT_KEYCLICK,
               (int)DEFAULT_KEYCLICK_HARDWARE);
    return bad != 0;
}
