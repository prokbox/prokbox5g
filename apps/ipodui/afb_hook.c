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
 * iPod UI shell: the Apple fallback's resident hook (#137, afb.h):
 * the switch, the park on the USB ack path, the live flag. Developer
 * builds only; the job itself loads with MOD_XINDEX.
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
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "ipodui/uilog.h"
#include "ipodui/xindex.h"
#include "ipodui/afb.h"
#include "ipodui/afb_core.h"
#ifdef SIMULATOR
#include "ipodui/afb_fat.h"
#else
#include "file_internal.h"
#endif

static int8_t sw = -1;          /* the switch; -1 not read yet */
static int8_t left = -1;        /* a state file (links may be live) */

static void load(void)
{
    if (sw < 0)
    {
        sw = file_exists(AFB_ON);
        left = file_exists(AFB_STATE);
    }
}

bool afb_on(void)
{
    load();
    return sw > 0;
}

bool afb_wanted(void)
{
    load();
    return sw > 0 || left > 0;
}

void afb_live(bool live)
{
    load();
    left = live || file_exists(AFB_STATE);
}

void afb_set(bool on)
{
    load();
    if (on)
    {
        mkdir(AFB_DIR);
        close(open(AFB_ON, O_WRONLY | O_CREAT, 0666));
    }
    else
        remove(AFB_ON);
    sw = on;
    left = 1;                   /* the job decides (it may tear down) */
    ui_log("afb", "switch %s", on ? "on" : "off");
    xindex_sync(XIDX_AT_USB);   /* now, as after a USB session */
}

void afb_usb_park(void)
{
    long t0 = current_tick;
    int rc;
    bool want = afb_wanted();

    /* read the switch again after the session (a host may have set or
     * cleared it: the bench sheet's other way to turn it on) */
    sw = -1;
    if (!want)
        return;
#ifdef SIMULATOR
    rc = afb_vol_park();
#else
    rc = dir_exists(AFB_LINK_DIR) ? rmdir_shallow(AFB_LINK_DIR) : 1;
#endif
    /* logged, not flushed: the host owns the disk right after the ack */
    ui_log("afb", "park rc=%d ms=%ld at=usb", rc,
           (current_tick - t0) * 1000 / HZ);
}
