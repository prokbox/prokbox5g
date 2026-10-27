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
 * iPod UI shell: the first-boot legal notice
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

/* The first-boot notice's boot check and acknowledgement (notice.h).
 * Core code, never a module: the check runs before any module is wanted,
 * and legal.c (a module tenant in a modules build) calls back here. */

#include "config.h"
#include <string.h>
#include <stdlib.h>
#include "system.h"
#include "settings.h"
#include "ipodui/fe_be.h"   /* P19 S1: settings saves cross */
#include "version.h"
#include "ipodui/anim.h"
#include "ipodui/screen_stack.h"
#include "ipodui/screens/screens.h"
#include "ipodui/usbscreen.h"
#include "ipodui/alarm.h"
#include "ipodui/notice.h"

#ifdef SIMULATOR
extern const char *sim_script_file;     /* sim_script.c */
#endif

static int notice_key;                  /* this build's hash, 0: no notice */
static unsigned notice_usb;             /* ui_usb_sessions() at the push */
static struct ui_screen *notice_scr;    /* its stack entry while up */

/* FNV-1a, folded to 1..0x7fffffff (the setting's range; 0 means none) */
static int build_hash(const char *v)
{
    uint32_t h = 2166136261u;

    while (*v)
        h = (h ^ (unsigned char)*v++) * 16777619u;
    h &= 0x7fffffffu;
    return h ? (int)h : 1;
}

void ui_notice_boot(void)
{
    const char *v = rbversion;

#ifdef SIMULATOR
    if (sim_script_file)
    {
        const char *e = getenv("IPODUI_SIM_NOTICE");

        if (!e)
            return;                     /* pre-acknowledged for tests */
        v = "0123456789-261012";        /* About's pinned id (about.c) */
        /* "clear": the run's first boot only (shots.sh names the parts
         * after a "reboot" .part-1.script, .part-2.script, ...) */
        if (!strcmp(e, "clear")
            && (!strstr(sim_script_file, "/.part-")
                || strstr(sim_script_file, "/.part-0.")))
        {
            /* stored too (sim tests only), so a later part of the run
             * starts from "not acknowledged" whatever an earlier run
             * left in config.cfg */
            global_settings.ipodui_notice_ack = 0;
            fe_settings_save();
        }
    }
#endif
    notice_key = build_hash(v);
    if (global_settings.ipodui_notice_ack == notice_key)
        return;
    if (alarm_ringing())
    {
        notice_key = 0;                 /* the ring first; next boot then */
        return;
    }
    notice_usb = ui_usb_sessions();
    if (ui_push_t(&ui_legal_screen, UI_LEGAL_NOTICE, UI_T_SYSTEM) < 0)
    {
        notice_key = 0;
        return;
    }
    notice_scr = ui_top();
    (void)ui_take_transition();         /* up at once, no slide */
    ui_invalidate(UI_DIRTY_ALL);
    UPDATE_TRACE("notice shown key=%d", notice_key);
}

bool ui_notice_up(void)
{
    return notice_scr && ui_top() == notice_scr;
}

void ui_notice_ack(void)
{
    notice_scr = NULL;                  /* Select is the only way out */
    if (!notice_key)
        return;
    if (ui_usb_sessions() != notice_usb)
    {
        UPDATE_TRACE("notice not acked (usb)%s", "");
        notice_key = 0;                 /* again next boot */
        return;
    }
    global_settings.ipodui_notice_ack = notice_key;
    notice_key = 0;
    fe_settings_save();                    /* with the next idle write */
    UPDATE_TRACE("notice acked%s", "");
}
