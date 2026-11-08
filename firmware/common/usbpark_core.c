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

/* AW-2: the charger-only USB park (firmware/export/usbpark_core.h) */
#include <stddef.h>
#include "usbpark_core.h"

#define TICK_AFTER(a, b)    ((long)((b) - (a)) < 0)

void usbpark_init(struct usbpark *p, long window, long window_max)
{
    p->state = USBPARK_OFF;
    p->reprobed = p->gave_up = false;
    p->late_run = 0;
    p->deadline = 0;
    p->window = window;
    p->window_max = window_max;
    p->parks = p->reprobes = p->late_hosts = 0;
}

void usbpark_vbus(struct usbpark *p, bool on, long now)
{
    p->reprobed = false;
    if (!on)
    {
        p->state = USBPARK_OFF;
        return;
    }
    p->state = USBPARK_WINDOW;
    p->deadline = now + p->window;
}

bool usbpark_host(struct usbpark *p)
{
    bool late = false;

    if (p->state == USBPARK_WINDOW && p->reprobed)
    {
        /* parked with a host on the cable: the window was too short */
        late = true;
        p->late_hosts++;
        p->window *= 2;
        if (p->window > p->window_max)
            p->window = p->window_max;
        if (++p->late_run >= USBPARK_LATE_TRIP)
            p->gave_up = true;
    }
    else if (p->state == USBPARK_WINDOW)
        p->late_run = 0;        /* a host inside a VBUS edge's window */
    if (p->state != USBPARK_OFF)
        p->state = USBPARK_HOST;
    p->reprobed = false;
    return late;
}

bool usbpark_due(const struct usbpark *p, long now)
{
    return p->state == USBPARK_WINDOW && !p->gave_up &&
           TICK_AFTER(now, p->deadline);
}

void usbpark_parked(struct usbpark *p)
{
    p->state = USBPARK_PARKED;
    p->reprobed = false;
    p->parks++;
}

bool usbpark_reprobe(struct usbpark *p, long now)
{
    if (p->state != USBPARK_PARKED)
        return false;
    p->state = USBPARK_WINDOW;
    p->reprobed = true;
    p->deadline = now + p->window;
    p->reprobes++;
    return true;
}
