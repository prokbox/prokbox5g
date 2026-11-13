/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Default battery capacity for ipod video
 *
 * Copyright (c) 2011 Frank Gevaerts
 * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
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

#include "config.h"
#include "system.h"

int battery_default_capacity(void)
{
#ifdef HAVE_IPOD_UI
    /* one build (WP3): the fitted cell is asked at first start and set
     * by apps/ipodui/battcell.c; this default (and an unanswered unit)
     * is the original cell by RAM, as battcurve_mah() has it */
    return probed_ramsize == 64 ? BATTERY_CAPACITY_DEFAULT_THICK
                                : BATTERY_CAPACITY_DEFAULT_THIN;
#elif IPOD_BATTERY_MAH > 0
    /* Phase 9: the stock build's fitted cell (ipodvideo-variant.h),
     * the same for both models */
    return IPOD_BATTERY_MAH;
#else
    if(probed_ramsize==64)
        return BATTERY_CAPACITY_DEFAULT_THICK;
    else
        return BATTERY_CAPACITY_DEFAULT_THIN;
#endif
}
