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

/* The battery session line (battlog.h, battlog_core.h): the device side */
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "powermgmt.h"
#include "backlight.h"
#include "audio.h"
#include "ipodui/uilog.h"
#include "ipodui/battlog.h"
#include "ipodui/battlog_core.h"
#ifdef HAVE_BCMFW_VIDEO
#include "video/vplayer.h"
#endif
#if defined(HAVE_BCMFW_RESIDENT) && !defined(SIMULATOR)
#include "bcm-devtest.h"
#endif

static struct bl_acc acc;
static bool started;
static long last_tick;

static void video(struct bl_video *v)
{
    memset(v, 0, sizeof(*v));
#ifdef HAVE_BCMFW_VIDEO
    vplayer_vclk_totals(&v->mhz_ms, &v->lvl10_ms, &v->busy_ms, &v->el_ms,
                        &v->ms);
#endif
}

static unsigned xtal_level(void)
{
#if defined(HAVE_BCMFW_RESIDENT) && !defined(SIMULATOR)
    bool idle, level5;

    bcm_boot_res_now(&idle, &level5);
    return level5 ? 5 : 11;
#else
    return 11;
#endif
}

static void sample(void)
{
    long now = current_tick;
    int st = audio_status();

    bl_add(&acc, (uint32_t)(now - last_tick) * (1000 / HZ),
           is_backlight_on(true),
           (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE));
    last_tick = now;
}

static void begin(void)
{
    struct bl_video v;

    video(&v);
    bl_begin(&acc, battery_level(), battery_voltage(), &v);
    last_tick = current_tick;
    started = true;
}

void battlog_end(const char *why)
{
    struct bl_video v;
    char line[200];

    if (!started)
        return;
    sample();
    video(&v);
    bl_line(line, sizeof(line), &acc, battery_level(), battery_voltage(), &v,
            xtal_level());
    ui_log("batt", "%s at=%s", line, why);
    started = false;
}

void battlog_service(void)
{
    if (!started)
    {
        begin();
        return;
    }
    if (current_tick - last_tick < HZ)
        return;
    sample();
    if (bl_due(&acc))
    {
        battlog_end("hour");
        begin();
    }
}
