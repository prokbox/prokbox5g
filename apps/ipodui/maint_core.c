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
 * iPod UI shell: the paused maintenance window, pure policy (Phase 16-1)
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
#include "ipodui/maint_core.h"

enum maint_why maint_core_window(const struct maint_in *in,
                                 long pause_ticks)
{
    if (in->status == 0)
        return MAINT_OPEN;              /* stopped: as today, at once */
    if (!(in->status & MAINT_ST_PAUSE))
        return MAINT_NO_PLAYING;
    if (in->video)
        return MAINT_NO_VIDEO;
    if (in->fading)
        return MAINT_NO_FADE;
    if (in->paused_ticks < 0 || in->paused_ticks < pause_ticks)
        return MAINT_NO_YOUNG;
    if (!in->user_idle || !in->dark)
        return MAINT_NO_LIT;
    return MAINT_OPEN;
}

bool maint_core_fits(size_t buf, size_t bytes, size_t floor)
{
    return buf >= bytes && buf - bytes >= floor;
}

bool maint_core_take_fits(size_t buf, size_t avail, size_t bytes,
                          size_t margin, size_t floor)
{
    if (avail >= 64 && avail - 64 >= bytes)
        return true;
    return bytes <= (size_t)-1 - margin
           && maint_core_fits(buf, bytes + margin, floor);
}
