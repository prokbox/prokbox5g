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
 *  The pixel hand-off's default (P19 MF-4, fe_fb.h): no UI on the COP in
 * this build.  The shell's fe_be.c replaces it.
 *
 * Copyright (C) 2026 prokopto-dev
 * Copyright (C) 2026 prokopto-dev
 *
 * Portions derived from Rockbox (rockbox.org).
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
#include <stddef.h>
#include "fe_fb.h"

__attribute__((weak)) int fe_fb_publish(const struct fe_fb_rect *rect,
                                        int timeout_ms)
{
    (void)rect;
    (void)timeout_ms;
    return FE_FB_LOCAL;
}
