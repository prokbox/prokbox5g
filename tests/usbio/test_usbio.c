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
 * iPod UI shell: host test of the USB screen's sustained-transfer detector
 * (apps/ipodui/usbscreen.h, #58). Built and run by tests/usbio/run.sh.
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
#include <stdbool.h>
#define HZ 100
#include "ipodui/usbscreen.h"
#include "../nowplaying/check.h"

/* A host that touches the disk at the ticks in busy(t) (true: an access
 * in tick t); the screen looks at every wake it asks for (and at every
 * 10 fps frame while the arrows spin). Returns the first tick the arrows
 * showed (-1: never) and the last tick they were on in *off_at. */
struct run {
    int first_on, last_on, on_ticks, looks;
};

static struct run drive(bool (*busy)(long t), long t0, long t1)
{
    struct ui_usb_io s;
    struct run r = { -1, -1, 0, 0 };
    long act = -1, t, next = t0;
    bool on = false;

    ui_usb_io_init(&s, t0);
    for (t = t0; t < t1; t++)
    {
        if (busy(t))
            act = t;
        if (t < next)
        {
            if (on)
                r.on_ticks++;
            continue;
        }
        r.looks++;
        on = ui_usb_io_step(&s, t, act, act >= 0);
        if (on)
        {
            r.on_ticks++;
            if (r.first_on < 0)
                r.first_on = t;
            r.last_on = t;
        }
        next = t + (on ? HZ / UI_USB_FPS : ui_usb_io_wake(&s, t));
    }
    return r;
}

/* macOS background reads: a sector or two every 1-2 s */
static bool bg(long t)       { return t % 130 == 7 || t % 170 == 3; }
/* a burst of background activity: 30 ms every 400 ms (Finder listing) */
static bool bg2(long t)      { return t % 40 < 3; }
/* a copy: busy from 1 s to 5 s, small gaps between files */
static bool copy(long t)     { return t >= 100 && t < 500 && t % 37 != 0; }
/* one 0.3 s read (a few MB) */
static bool short_rd(long t) { return t >= 200 && t < 230; }
static bool idle(long t)     { (void)t; return false; }

int main(void)
{
    struct run r;

    /* idle: never, and the loop polls at the old 0.5 s rate */
    r = drive(idle, 1000, 3000);
    CHECK_EQ(r.first_on, -1);
    CHECK(r.looks <= 2000 / UI_USB_IO_HOLD + 1);

    /* background reads: never spin */
    r = drive(bg, 1000, 7000);
    CHECK_EQ(r.first_on, -1);
    r = drive(bg2, 1000, 7000);
    CHECK_EQ(r.first_on, -1);

    /* a short read: never spins (below the sustained threshold) */
    r = drive(short_rd, 0, 1000);
    CHECK_EQ(r.first_on, -1);

    /* a copy: on within ~1.1 s of its start (one idle poll + 0.6 s of
     * busy slots), on through the gaps, off 0.5 s after the end */
    r = drive(copy, 0, 1000);
    CHECK(r.first_on >= 100 + UI_USB_IO_MIN * UI_USB_IO_SLOT - 10);
    CHECK(r.first_on <= 100 + UI_USB_IO_HOLD + UI_USB_IO_MIN * UI_USB_IO_SLOT
                        + UI_USB_IO_SLOT);
    CHECK(r.last_on >= 499 && r.last_on <= 499 + UI_USB_IO_HOLD + 10);
    CHECK(r.on_ticks >= 499 - r.first_on);

    CHECK_DONE("test_usbio");
}
