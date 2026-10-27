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
 * iPod UI shell: hidden developer mode, MINIMAL scope (Phase 8a U2;
 * docs/phase8a-brief.md §3.4, Revision 1 H1, M3, M6, L1, L6, L7;
 * docs/design/devmode.md §1-3 with its MINIMAL note).
 *
 * On the About screen, SELECT + PREV held together for 3 s opens a plain
 * stock do_menu() list, "Developer": Debug (the stock debug screens) and
 * Back. Holding NEXT at boot opens the same list before the shell starts
 * (apps/main.c, the recovery path). Nothing here shows a hint or text in
 * the product UI; the chord gives one hardware click at 3 s.
 *
 * Compiled only with HAVE_IPOD_UI_DEVMODE (tools/configure --ipodui;
 * --ipodui-release leaves it out, and with it every literal: the title is
 * LANG_IPODUI_DEVELOPER, feature ipodui_devmode).
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

#ifdef HAVE_IPOD_UI_DEVMODE
#include <stdbool.h>
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "bcmjob_pp.h"          /* P19: bcmjob_owner_event() */
#include "lang.h"
#include "settings.h"             /* ID2P */
#ifdef HAVE_STORAGE_POWEROFF
#include "stor_pwr.h"
#include "audio.h"
#endif
#include "../menu.h"               /* the stock do_menu(), not ipodui/menu.h */
#include "debug_menu.h"
#include "viewport.h"
#include "screen_access.h"
#include "debug.h"
#include "logf.h"
#ifdef HAVE_HARDWARE_CLICK
#include "piezo.h"
#endif
#include "ipodui/devmode.h"
#include "ipodui/screen_stack.h"
#include "ipodui/anim.h"
#include "ipodui/unlit.h"
#include "ipodui/uilog.h"
#include "ipodui/powerlog.h"
#include "ipodui/batcal.h"
#include "ipodui/clockpol.h"
#include "ipodui/udmatest.h"
#include "ipodui/sleeplab.h"
#include "ipodui/alarm_ring.h"   /* P12: the Alarm lab */
#include "ipodui/sleep.h"
#include "ipodui/fe_be.h"
#include "ipodui/devstatus.h"      /* Status, verdicts, governor, bench */
#include "ipodui/devbench_sim.h"    /* DEVBENCH_SIM: the sim's Bench session */
#if defined(HAVE_BCMFW_DEVTEST) || defined(DEVBENCH_SIM)
#include "ipodui/devbcm.h"         /* Video chip's rows, Bench session */
#include "ipodui/drift_jobs.h"
#include "ipodui/devjobfail.h"     /* Fault injection: forced job failures */
#endif
#include "usb_storage_stats.h"
#ifndef SIMULATOR
#include "usb_drv.h"
#endif
#include <stdio.h>
#include <string.h>
#include "string-extra.h"            /* strlcat */
#include "misc.h"                  /* default_event_handler */
#include "dac_ext.h"
#include "ipodui/apath.h"
#ifdef HAVE_PANIC_RECORD
#include "panic.h"
#include "panic_record.h"
#include "powermgmt.h"             /* sys_reboot */
#endif
#include "ipodui/screens/screens.h"
#ifdef HAVE_BCMFW_RESIDENT
#include "bcm-devtest.h"           /* bcm_boot_resident_* (p14-resident) */
#endif
#ifdef HAVE_DEVSTATS
#include "devstats.h"              /* Fit-3 step 0 */
#include "storage.h"
#include "fat.h"                   /* #81: FSInfo */
#include "mv.h"
#include "splash.h"
#include "ipodui/artcache.h"       /* artcache_disk_awake() */
#ifdef IPODUI_MODULES
#include "ipodui/modules/devloader.h" /* #106: Loader & storage */
#endif
#endif

static bool armed;              /* the chord is down on About */
static bool fired;              /* the list ran for this hold */
static long arm_tick;
static bool in_devmode;         /* ipodui_devmode_run() is on the stack */

/* Advanced > System debug: the stock debug screens (devmode only) */
MENUITEM_FUNCTION(ipodui_dev_debug_item, 0,
                  ID2P(LANG_IPODUI_DEV_ROCKBOX_DEBUG), debug_menu, NULL,
                  Icon_NOICON);
MENUITEM_RETURNVALUE(ipodui_dev_back_item, ID2P(LANG_BACK),
                     GO_TO_PREVIOUS, NULL, Icon_NOICON);
/* P15 end review M2: standby's power order is unproven on the WM8758 (no
 * datasheet), so a developer switch keeps the codec on for an A/B of pops.
 * Not saved: standby is on again at the next boot. */
static int dev_standby_toggle(void)
{
    dac_standby_allow(!dac_standby_allowed());
    ui_log("dac", "standby allowed=%d", (int)dac_standby_allowed());
    return 0;
}
static char *dev_standby_name(int selected_item, void *data,
                              char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return (char *)(dac_standby_allowed() ? "DAC standby: on"
                                          : "DAC standby: off");
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_standby_item, 0, dev_standby_toggle,
                          dev_standby_name, NULL, NULL, NULL, Icon_NOICON);

#ifdef HAVE_STORAGE_POWEROFF
/* #114: the A/B of the drive's power-off (firmware/export/stor_pwr.h):
 * off when idle (the default) or kept in standby, awake and at sleep
 * entry. Saved as a marker file so a rundown keeps it across restarts
 * (powerlog_boot() reads it). Literals like the rows above. */
static int dev_stor_toggle(void)
{
    bool allow = !storage_poweroff_allowed();

    storage_set_poweroff(allow);
    if (allow)
        remove(POWERLOG_STOR_STANDBY);
    else
    {
        int fd = creat(POWERLOG_STOR_STANDBY, 0666);
        if (fd >= 0)
            close(fd);
    }
    ui_log("stor", "pwr allow=%d", allow ? 1 : 0);
    powerlog_mark(allow ? "stor off-idle" : "stor standby");
    return 0;
}
static char *dev_stor_name(int selected_item, void *data,
                           char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return (char *)(storage_poweroff_allowed()
                    ? "Drive power: off when idle"
                    : "Drive power: standby only");
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_stor_item, 0, dev_stor_toggle,
                          dev_stor_name, NULL, NULL, NULL, Icon_NOICON);
/* #114 H3: the drive-power A/B (docs/device-session-p18-0.md H3). An
 * automatic alternation, "off when idle" (A) and "standby only" (B), every
 * 30 min (or 60) for 2, 4, 6, 8 or 10 h (A B A B ..., an even count, ending
 * on B so each mode gets equal time; default 4 h), each switch a power.csv mark (ab_A1,
 * ab_B1 ... ab_end). It drives itself from the UI loop: the loop's wait is
 * cut short at the next switch (ipodui_devmode_ab_wait()), no thread, no
 * timer, no polling. Same API as the row above, but the marker file is
 * never touched, so a reboot is an abort that returns to the saved mode.
 * Needs music playing: stopped for the grace (2 min) halts the test with
 * an "ab paused" mark. The power log is on at 30 s for the duration.
 * Literals like the rows above. */
#define AB_STEP_DEF     30          /* min per switch; 60 is the other */
#define AB_GRACE_S      120
enum ab_state { AB_OFF, AB_RUN, AB_DONE, AB_HALTED, AB_NOPLAY, AB_NOREPEAT };
#ifdef SIMULATOR
long ipodui_ab_period_ms;           /* tests: "drive_ab period <ms>" */
long ipodui_ab_grace_ms;
#endif
static struct
{
    enum ab_state state;
    int hours, step_min;            /* the choice: 2/4/6/8/10 h, 30/60 min */
    int nphase;                     /* hours*60/step_min: even, ends on B */
    int phase;                      /* 0..nphase-1 running */
    long start, stop_since;         /* ticks; stop_since 0 = playing */
    bool prev_allow, prev_log_on;
    int prev_interval;
} ab;

static int ab_hours(void)
{
    return ab.hours ? ab.hours : 4;
}
static int ab_step(void)
{
    return ab.step_min ? ab.step_min : AB_STEP_DEF;
}
/* The length and the switch period, chosen before a start (the sim too) */
void ipodui_devmode_ab_config(int hours, int step_min)
{
    if (ab.state == AB_RUN)
        return;
    if (hours >= 2 && hours <= 10 && !(hours & 1))
        ab.hours = hours;
    if (step_min == 30 || step_min == 60)
        ab.step_min = step_min;
}
static long ab_period(void)
{
#ifdef SIMULATOR
    if (ipodui_ab_period_ms > 0)
        return ipodui_ab_period_ms * HZ / 1000;
#endif
    return ab_step() * 60L * HZ;
}
static long ab_grace(void)
{
#ifdef SIMULATOR
    if (ipodui_ab_grace_ms > 0)
        return ipodui_ab_grace_ms * HZ / 1000;
#endif
    return AB_GRACE_S * HZ;
}
static bool ab_playing(void)
{
    int st = audio_status();
    return (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
}
static void ab_mark(int phase)
{
    char tag[16];

    snprintf(tag, sizeof(tag), "ab_%c%d", (phase & 1) ? 'B' : 'A',
             (phase & 31) / 2 + 1);
    powerlog_mark(tag);
}
static void ab_restore(enum ab_state to, const char *mark)
{
    powerlog_mark(mark);
    ab.state = to;
    storage_set_poweroff(ab.prev_allow);
    powerlog_set_interval(ab.prev_interval);
    if (!ab.prev_log_on)
        powerlog_set(false);
    ui_log("ab", "%s allow=%d", mark, ab.prev_allow ? 1 : 0);
}
void ipodui_devmode_ab_stop(void)
{
    if (ab.state == AB_RUN)
        ab_restore(AB_OFF, "ab abort");
}
void ipodui_devmode_ab_start(void)
{
    if (ab.state == AB_RUN)
        return;
    /* a run wasted by a stopped player or a playlist that ends is the
     * failure this guards: refuse, say so on the row, no splash */
    if (!ab_playing())
    {
        ab.state = AB_NOPLAY;
        ui_log("ab", "refused: not playing");
        return;
    }
    if (global_settings.repeat_mode != REPEAT_ALL
        && global_settings.repeat_mode != REPEAT_ONE
        && global_settings.repeat_mode != REPEAT_SHUFFLE)
    {
        ab.state = AB_NOREPEAT;
        ui_log("ab", "refused: repeat off");
        return;
    }
    ab.prev_allow = storage_poweroff_allowed();
    ab.prev_log_on = powerlog_is_on();
    ab.prev_interval = powerlog_interval();
    powerlog_set_interval(30);
    powerlog_set(true);
    ab.state = AB_RUN;
    ab.nphase = ab_hours() * 60 / ab_step();    /* even: 2 h..10 h of 30
                                                 * or 60 min */
    ab.phase = 0;
    ab.start = current_tick;
    ab.stop_since = 0;
    storage_set_poweroff(true);
    ab_mark(0);
    ui_log("ab", "start h=%d step_min=%d n=%d", ab_hours(), ab_step(),
           ab.nphase);
}
/* UI thread, every main-loop pass (powerlog_service()) */
void ipodui_devmode_ab_service(void)
{
    if (ab.state != AB_RUN)
        return;
    if (ab_playing())
        ab.stop_since = 0;
    else if (!ab.stop_since)
        ab.stop_since = current_tick | 1;
    else if (current_tick - ab.stop_since >= ab_grace())
    {
        ab_restore(AB_HALTED, "ab paused");
        return;
    }
    while (ab.state == AB_RUN
           && !TIME_BEFORE(current_tick,
                           ab.start + (ab.phase + 1) * ab_period()))
    {
        if (++ab.phase >= ab.nphase)
        {
            ab_restore(AB_DONE, "ab_end");
            return;
        }
        storage_set_poweroff(!(ab.phase & 1));
        ab_mark(ab.phase);
    }
}
/* The main loop's wait: ticks to the next switch or the stop grace */
int ipodui_devmode_ab_wait(void)
{
    long t;

    if (ab.state != AB_RUN)
        return TIMEOUT_BLOCK;
    t = ab.start + (ab.phase + 1) * ab_period() - current_tick;
    if (ab.stop_since)
    {
        long g = ab.stop_since + ab_grace() - current_tick;
        if (g < t)
            t = g;
    }
    return t < 1 ? 1 : (int)t;
}
static int dev_ab_toggle(void)
{
    if (ab.state == AB_RUN)
        ipodui_devmode_ab_stop();
    else
        ipodui_devmode_ab_start();
    return 0;
}
static char *dev_ab_name(int selected_item, void *data,
                         char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    switch (ab.state)
    {
    case AB_RUN:
    {
        long m = (current_tick - ab.start) / (HZ * 60);
        snprintf(buffer, buffer_len, "Drive power A/B: running %ld:%02ld",
                 m / 60, m % 60);
        return buffer;
    }
    case AB_DONE:
        return (char *)"Drive power A/B: done";
    case AB_HALTED:
        return (char *)"Drive power A/B: halted";
    case AB_NOPLAY:
        return (char *)"Drive power A/B: start music first";
    case AB_NOREPEAT:
        return (char *)"Drive power A/B: set Repeat first";
    default:
        snprintf(buffer, buffer_len, ab_hours() == 10
                 ? "Drive power A/B (Overnight)" : "Drive power A/B (%d h)",
                 ab_hours());
        return buffer;
    }
}
/* the length and the period, before a start (a run keeps its own) */
static int dev_ab_len(void)
{
    int h = ab_hours();
    ipodui_devmode_ab_config(h >= 10 ? 2 : h + 2, 0);
    return 0;
}
static char *dev_ab_len_name(int selected_item, void *data,
                             char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    if (ab_hours() == 10)
        return (char *)"Drive power A/B length: Overnight (10 h)";
    snprintf(buffer, buffer_len, "Drive power A/B length: %d h", ab_hours());
    return buffer;
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_ab_len_item, 0, dev_ab_len,
                          dev_ab_len_name, NULL, NULL, NULL, Icon_NOICON);
static int dev_ab_step(void)
{
    ipodui_devmode_ab_config(0, ab_step() == 30 ? 60 : 30);
    return 0;
}
static char *dev_ab_step_name(int selected_item, void *data,
                              char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    snprintf(buffer, buffer_len, "Drive power A/B switch: %d min",
             ab_step());
    return buffer;
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_ab_step_item, 0, dev_ab_step,
                          dev_ab_step_name, NULL, NULL, NULL, Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_ab_item, 0, dev_ab_toggle,
                          dev_ab_name, NULL, NULL, NULL, Icon_NOICON);
#define DEV_STOR_ITEM , &ipodui_dev_stor_item
#define DEV_AB_ITEMS , &ipodui_dev_ab_len_item, &ipodui_dev_ab_step_item, \
    &ipodui_dev_ab_item
#else
#define DEV_STOR_ITEM
#define DEV_AB_ITEMS
#endif

/* Tracker #10: the power log (powerlog.c, docs/hardware.md "Rundowns and
 * A/B tests"). Literals like the rows above. The toggle persists in
 * /.rockbox/power.cfg so a rundown carries on across a restart; the
 * interval steps 5 s (A/B) -> 30 s (rundown) -> 60 s. */
static int dev_powerlog_toggle(void)
{
    powerlog_set(!powerlog_is_on());
    return 0;
}
static char *dev_powerlog_name(int selected_item, void *data,
                               char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return (char *)(powerlog_is_on() ? "Power log: on" : "Power log: off");
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_powerlog_item, 0, dev_powerlog_toggle,
                          dev_powerlog_name, NULL, NULL, NULL, Icon_NOICON);

static int dev_powerlog_every(void)
{
    int s = powerlog_interval();
    powerlog_set_interval(s < 30 ? 30 : s < 60 ? 60 : 5);
    return 0;
}
static char *dev_powerlog_every_name(int selected_item, void *data,
                                     char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    snprintf(buffer, buffer_len, "Power log every: %d s",
             powerlog_interval());
    return buffer;
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_powerlog_every_item, 0,
                          dev_powerlog_every, dev_powerlog_every_name, NULL,
                          NULL, NULL, Icon_NOICON);

#ifdef HAVE_BATTERY_LEARN
/* #70: automatic battery calibration (batcal.c, docs/batcal-brief.md
 * section 5). Two read-only status rows (the table in use, the run being
 * logged) and a reset behind a second press. Literals like the rows
 * above (a developer-mode exception to rule 8); never a file name. */
static int dev_batcal_none(void)
{
    return 0;
}
static char *dev_batcal_name(int selected_item, void *data,
                             char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    batcal_status(buffer, buffer_len);
    return buffer;
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_batcal_item, 0, dev_batcal_none,
                          dev_batcal_name, NULL, NULL, NULL, Icon_NOICON);
static char *dev_batcal_run_name(int selected_item, void *data,
                                 char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    batcal_run_status(buffer, buffer_len);
    return buffer;
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_batcal_run_item, 0, dev_batcal_none,
                          dev_batcal_run_name, NULL, NULL, NULL,
                          Icon_NOICON);
/* The confirmation is a second SELECT within 5 s (the row says so; a
 * press after the window arms again, the safe way): the
 * stock yes/no would link ~1.5 KB for this one row */
static long batcal_armed;           /* tick of the first SELECT, or 0 */
static bool batcal_arm_live(void)
{
    return batcal_armed && TIME_BEFORE(current_tick, batcal_armed + 5 * HZ);
}
static int dev_batcal_reset(void)
{
    if (batcal_arm_live())
    {
        batcal_armed = 0;
        batcal_reset();
    }
    else
        batcal_armed = current_tick | 1;
    return 0;
}
static char *dev_batcal_reset_name(int selected_item, void *data,
                                   char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return (char *)(batcal_arm_live() ? "Select again within 5 s"
                                      : "Reset battery calibration");
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_batcal_reset_item, 0, dev_batcal_reset,
                          dev_batcal_reset_name, NULL, NULL, NULL,
                          Icon_NOICON);
#define DEV_BATCAL_ITEMS , &ipodui_dev_batcal_item, \
    &ipodui_dev_batcal_reset_item
#define DEV_BATCAL_RUN_ITEM , &ipodui_dev_batcal_run_item
#else
#define DEV_BATCAL_ITEMS
#define DEV_BATCAL_RUN_ITEM
#endif

#ifdef HAVE_ATA_UDMA_FORCE
/* Fit-1 C7: the UDMA4 experiment (udmatest.c). Turning it on, and the
 * test, each take a second SELECT within 5 s (as the battery reset);
 * turning it off is one press. Literals, as the rows above. */
static long udma_armed, udmat_armed;
static bool udma_arm_live(long t)
{
    return t && TIME_BEFORE(current_tick, t + 5 * HZ);
}
static int dev_udma_toggle(void)
{
    if (udma_is_on())
        udma_set(false);
    else if (udma_arm_live(udma_armed))
    {
        udma_armed = 0;
        udma_set(true);
    }
    else
        udma_armed = current_tick | 1;
    return 0;
}
static char *dev_udma_name(int selected_item, void *data,
                           char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    if (udma_is_on())
        return (char *)"UDMA4 experiment: on";
    return (char *)(udma_arm_live(udma_armed) ? "Select again: force UDMA4"
                                      : "UDMA4 experiment: off");
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_udma_item, 0, dev_udma_toggle,
                          dev_udma_name, NULL, NULL, NULL, Icon_NOICON);
static int dev_udma_test(void)
{
    if (udma_arm_live(udmat_armed))
    {
        udmat_armed = 0;
        udma_test();                /* about a minute, playback stopped */
    }
    else
        udmat_armed = current_tick | 1;
    return 0;
}
static char *dev_udma_test_name(int selected_item, void *data,
                                char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return (char *)(udma_arm_live(udmat_armed) ? "Select again: test (1 min)"
                                       : udma_test_result());
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_udma_test_item, 0, dev_udma_test,
                          dev_udma_test_name, NULL, NULL, NULL, Icon_NOICON);
#define DEV_UDMA_ITEMS , &ipodui_dev_udma_item, &ipodui_dev_udma_test_item
#else
#define DEV_UDMA_ITEMS
#endif

/* Phase 18: soft sleep Off (default) / On; Off = today's power-off on
 * every sleep request. The Sleep lab calls power_sleep directly and is
 * independent of it. */
static int dev_softsleep_toggle(void)
{
    ui_sleep_soft_set(!ui_sleep_soft_get());
    return 0;
}
static char *dev_softsleep_name(int selected_item, void *data,
                                char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return str(ui_sleep_soft_get() ? LANG_IPODUI_SOFT_SLEEP_ON
                                   : LANG_IPODUI_SOFT_SLEEP_OFF);
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_softsleep_item, 0, dev_softsleep_toggle,
                          dev_softsleep_name, NULL, NULL, NULL, Icon_NOICON);

#ifdef HAVE_UI_THREAD_SPLIT
/* P19 S3/S8: where the shell's loop runs from the next boot: the main
 * thread (default), its own CPU thread, or the COP (refused and logged
 * until the boundary is complete; fe_be.c).  Developer text. */
static int dev_uisplit_toggle(void)
{
    fe_be_mode_set_next((enum fe_mode)((fe_be_mode_next() + 1) % 3));
    return 0;
}
static char *dev_uisplit_name(int selected_item, void *data,
                              char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    char t[48];

    snprintf(t, sizeof(t), "UI thread: %s (now %s)",
             fe_be_mode_name(fe_be_mode_next()),
             fe_be_mode_name(fe_be_mode()));
    return devstatus_with_verdict(t, "ucop2", buffer, buffer_len);
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_uisplit_item, 0, dev_uisplit_toggle,
                          dev_uisplit_name, NULL, NULL, NULL, Icon_NOICON);
#define DEV_UISPLIT_ITEM , &ipodui_dev_uisplit_item
#else
#define DEV_UISPLIT_ITEM
#endif

#ifdef IPODUI_SLEEPLAB
/* Phase 18-1: the Sleep lab (sleeplab.c), D2's device steps; each row
 * sleeps once and logs the result. The first row shows the last run's
 * outcome (developer text, as the other devmode rows): whether it
 * counted, without the log */
static int slab_result(void)     { return 0; }
static char *slab_result_name(int selected_item, void *data, char *buffer,
                              size_t buffer_len)
{
    (void)selected_item; (void)data;
    snprintf(buffer, buffer_len, "%s", sleeplab_last());
    return buffer;
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_slab_result_item, 0, slab_result,
                          slab_result_name, NULL, NULL, NULL, Icon_NOICON);
static int slab_poll_t2(void)    { return sleeplab_run(SLAB_POLL_TIMER2); }
static int slab_poll_wc(void)    { return sleeplab_run(SLAB_POLL_WAITCNT); }
static int slab_wake(void)       { return sleeplab_run(SLAB_WAKE); }
static int slab_wake_fast(void)  { return sleeplab_run(SLAB_WAKE_FAST); }
static int slab_wake_opto(void)  { return sleeplab_run(SLAB_WAKE_OPTO); }
static int slab_alarm(void)      { return sleeplab_run(SLAB_ALARM); }
static int slab_deep(void)       { return sleeplab_run(SLAB_DEEP); }
MENUITEM_FUNCTION(ipodui_slab_poll_t2_item, 0, ID2P(LANG_IPODUI_SLAB_POLL_T2),
                  slab_poll_t2, NULL, Icon_NOICON);
MENUITEM_FUNCTION(ipodui_slab_poll_wc_item, 0,
                  ID2P(LANG_IPODUI_SLAB_POLL_WAITCNT), slab_poll_wc, NULL,
                  Icon_NOICON);
MENUITEM_FUNCTION(ipodui_slab_wake_item, 0, ID2P(LANG_IPODUI_SLAB_WAKE),
                  slab_wake, NULL, Icon_NOICON);
MENUITEM_FUNCTION(ipodui_slab_wake_fast_item, 0,
                  ID2P(LANG_IPODUI_SLAB_WAKE_FAST), slab_wake_fast, NULL,
                  Icon_NOICON);
MENUITEM_FUNCTION(ipodui_slab_wake_opto_item, 0,
                  ID2P(LANG_IPODUI_SLAB_WAKE_OPTO), slab_wake_opto, NULL,
                  Icon_NOICON);
MENUITEM_FUNCTION(ipodui_slab_alarm_item, 0, ID2P(LANG_IPODUI_SLAB_ALARM),
                  slab_alarm, NULL, Icon_NOICON);
MENUITEM_FUNCTION(ipodui_slab_deep_item, 0, ID2P(LANG_IPODUI_SLAB_DEEP),
                  slab_deep, NULL, Icon_NOICON);
MAKE_MENU(ipodui_dev_sleeplab_menu, ID2P(LANG_IPODUI_SLEEPLAB), NULL,
          Icon_NOICON, &ipodui_slab_result_item,
          &ipodui_slab_poll_t2_item, &ipodui_slab_poll_wc_item,
          &ipodui_slab_wake_item, &ipodui_slab_wake_fast_item,
          &ipodui_slab_wake_opto_item, &ipodui_slab_alarm_item,
          &ipodui_slab_deep_item);
#define DEV_SLEEPLAB_ITEM &ipodui_dev_sleeplab_menu,
#else
#define DEV_SLEEPLAB_ITEM
#endif

#if defined(HAVE_USB_STATS) && !defined(SIMULATOR)
/* iPod Video normally uses the compatible page-aligned INQUIRY backing
 * object. This volatile Developer A/B changes only its allocation; it is a
 * regression tool, not an ordinary-user setting. */
static int dev_usb_inq_toggle(void)
{
    usb_storage_set_inquiry_static(!usb_storage_inquiry_static());
    ui_log("usb", "dev inquiry=%s", usb_storage_inquiry_static() ?
           "static" : "core");
    return 0;
}
static char *dev_usb_inq_name(int selected_item, void *data,
                              char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return (char *)(usb_storage_inquiry_static() ? "USB INQUIRY buffer: static"
                                                  : "USB INQUIRY buffer: core");
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_usb_inq_item, 0, dev_usb_inq_toggle,
                          dev_usb_inq_name, NULL, NULL, NULL, Icon_NOICON);
/* This switch deliberately affects a later USB session only: demand
 * batching (the default), off (plain serial reads, the earlier baseline)
 * or the overlapped worker test. It is a regression A/B for the next cable
 * insertion, never a user-facing storage preference. */
static int dev_usb_pf_toggle(void)
{
    static const char *const names[] = { "batch", "off", "worker" };
    usb_storage_set_prefetch_mode((usb_storage_prefetch_mode() + 1)
                                  % USB_PF_MODES);
    ui_log("usb", "dev prefetch-next=%s", names[usb_storage_prefetch_mode()]);
    return 0;
}
static char *dev_usb_pf_name(int selected_item, void *data,
                             char *buffer, size_t buffer_len)
{
    static const char *const names[] = {
        "USB prefetch next: batch", "USB prefetch next: off",
        "USB prefetch next: worker test" };
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return (char *)names[usb_storage_prefetch_mode()];
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_usb_pf_item, 0, dev_usb_pf_toggle,
                          dev_usb_pf_name, NULL, NULL, NULL, Icon_NOICON);
#if defined(IPOD_VIDEO)
static int dev_usb_attach_toggle(void)
{
    usb_drv_set_attach_reinit(!usb_drv_attach_reinit());
    ui_log("usb", "dev attach-reinit=%s", usb_drv_attach_reinit() ?
           "legacy" : "normal");
    return 0;
}
static char *dev_usb_attach_name(int selected_item, void *data,
                                 char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return (char *)(usb_drv_attach_reinit() ?
                  "USB ARC attach init: legacy" : "USB ARC attach init: normal");
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_usb_attach_item, 0,
                          dev_usb_attach_toggle, dev_usb_attach_name,
                          NULL, NULL, NULL, Icon_NOICON);
#define DEV_USB_ATTACH_ITEM , &ipodui_dev_usb_attach_item
#else
#define DEV_USB_ATTACH_ITEM
#endif
#define DEV_USB_INQ_ITEM , &ipodui_dev_usb_inq_item
#define DEV_USB_EXP_ITEMS , &ipodui_dev_usb_pf_item DEV_USB_ATTACH_ITEM
#else
#define DEV_USB_INQ_ITEM
#define DEV_USB_EXP_ITEMS
#endif

#ifndef SIMULATOR
/* #81: the volume's FSInfo free count. "unknown" (0xffffffff) or out of
 * range makes a host walk the whole FAT before it mounts (macOS: 250 MB
 * on this 1 TB volume, past its 20 s limit); the second SELECT within 5 s
 * counts the free clusters once (one walk of the FAT on the iPod, tens of
 * seconds) and writes FSInfo back. Literals, as the rows above. */
static long fsinfo_armed;
static char fsinfo_result[40];
static int dev_fsinfo_fix(void)
{
    if (fsinfo_armed && TIME_BEFORE(current_tick, fsinfo_armed + 5 * HZ))
    {
        long t0 = current_tick;
        unsigned long fc = 0, nf = 0, cl = 0;

        fsinfo_armed = 0;
        splash(0, "...");           /* developer row: no shell string */
        volume_recalc_free_commit(IF_MV(0));
        fat_get_fsinfo(IF_MV(0,) &fc, &nf, &cl);
        snprintf(fsinfo_result, sizeof(fsinfo_result), "FSInfo set: %lu free, %lds",
                 fc, (current_tick - t0) / HZ);
        ui_log("fat", "fsinfo recalc free=%lu next=%lu s=%ld", fc, nf,
               (current_tick - t0) / HZ);
    }
    else
        fsinfo_armed = current_tick | 1;
    return 0;
}
static char *dev_fsinfo_name(int selected_item, void *data,
                             char *buffer, size_t buffer_len)
{
    unsigned long fc = 0, nf = 0, cl = 0;

    (void)selected_item; (void)data;
    if (fsinfo_armed && TIME_BEFORE(current_tick, fsinfo_armed + 5 * HZ))
        return (char *)"Select again: count free space";
    if (fsinfo_result[0])
        return fsinfo_result;
    if (!fat_get_fsinfo(IF_MV(0,) &fc, &nf, &cl))
        return (char *)"FSInfo: no volume";
    if (fc == 0xffffffffUL || fc > cl)
        snprintf(buffer, buffer_len, "FSInfo: unknown (fix)");
    else
        snprintf(buffer, buffer_len, "FSInfo: %lu free", fc);
    return buffer;
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_fsinfo_item, 0, dev_fsinfo_fix,
                          dev_fsinfo_name, NULL, NULL, NULL, Icon_NOICON);

#define DEV_FSINFO_ITEM , &ipodui_dev_fsinfo_item
#else
#define DEV_FSINFO_ITEM
#endif

/* P15 wave 2b: the digital path, bit-perfect on the device (apath.c) */
static char *dev_apath_name(int selected_item, void *data,
                            char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return (char *)"Audio path test";
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_apath_item, 0, ui_apath_test,
                          dev_apath_name, NULL, NULL, NULL, Icon_NOICON);

#ifdef HAVE_PANIC_RECORD
/* The panic record's device test (docs/hardware.md "Panic record"): the
 * first row panics for real (the core halts; reset with MENU+SELECT), the
 * second saves the same record and restarts cleanly (a DEV_RS reset). The
 * next boot's ipodui.log shows "panic ... msg dev test ..." either way. */
static int dev_panic_freeze(void)
{
    panicf("dev test freeze t=%08lx", (unsigned long)current_tick);
    return 0;
}
static int dev_panic_restart(void)
{
    panic_record_save(0, 0, 0, 0, "dev test restart");
    sys_reboot();
    return 0;
}
MENUITEM_FUNCTION(ipodui_dev_panic_freeze_item, 0,
                  (unsigned char *)"Crash test: freeze",
                  dev_panic_freeze, NULL, Icon_NOICON);
MENUITEM_FUNCTION(ipodui_dev_panic_restart_item, 0,
                  (unsigned char *)"Crash test: restart",
                  dev_panic_restart, NULL, Icon_NOICON);
#define DEV_PANIC_ITEMS , &ipodui_dev_panic_freeze_item, \
                          &ipodui_dev_panic_restart_item
#else
#define DEV_PANIC_ITEMS
#endif

#ifdef HAVE_BCMFW_RESIDENT
/* p14-resident: the video image as the display driver, so a video starts
 * with no power cycle (default on, owner's go), and its core's sleep
 * between commands (default off: no interrupt is proven on the chip yet).
 * Saved in a file; the boot path reads it at the next boot.  The rest is
 * on Debug -> BCM boot path: status. */
static int dev_resident_toggle(void)
{
    bool idle, on = bcm_boot_resident_setting(&idle);

    bcm_boot_resident_set(!on, idle);
    return 0;
}
static char *dev_resident_name(int selected_item, void *data,
                               char *buffer, size_t buffer_len)
{
    bool idle;

    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return (char *)(bcm_boot_resident_setting(&idle) ?
                    "Resident video image: on" : "Resident video image: off");
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_resident_item, 0, dev_resident_toggle,
                          dev_resident_name, NULL, NULL, NULL, Icon_NOICON);

static int dev_res_idle_toggle(void)
{
    bool idle, on = bcm_boot_resident_setting(&idle);

    bcm_boot_resident_set(on, !idle);
    return 0;
}
static char *dev_res_idle_name(int selected_item, void *data,
                               char *buffer, size_t buffer_len)
{
    bool idle;

    (void)selected_item; (void)data;
    bcm_boot_resident_setting(&idle);
    return devstatus_with_verdict(idle ? "Video chip idle sleep: on"
                                       : "Video chip idle sleep: off",
                                  "slp", buffer, buffer_len);
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_res_idle_item, 0, dev_res_idle_toggle,
                          dev_res_idle_name, NULL, NULL, NULL, Icon_NOICON);

/* p19-battery, lever PW3: the resident image's core at Apple's level 5
 * for 27 MHz (default off until PW3 and the bench row "lvl5" pass on
 * silicon).  Same file, next boot. */
static int dev_res_level_toggle(void)
{
    bcm_boot_level5_set(!bcm_boot_level5_setting());
    return 0;
}
static char *dev_res_level_name(int selected_item, void *data,
                                char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    return devstatus_with_verdict(bcm_boot_level5_setting() ?
                                  "Video chip core level 5: on" :
                                  "Video chip core level 5: off",
                                  "lvl5", buffer, buffer_len);
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_res_level_item, 0, dev_res_level_toggle,
                          dev_res_level_name, NULL, NULL, NULL, Icon_NOICON);
#define DEV_RESIDENT_ITEMS , &ipodui_dev_resident_item, \
    &ipodui_dev_res_idle_item, &ipodui_dev_res_level_item
#else
#define DEV_RESIDENT_ITEMS
#endif

#ifdef HAVE_PLL_IDLE
/* Fit-1 C6: the 24 MHz idle policy's A/B switch (clockpol.c). Not saved:
 * on again at the next boot, like the DAC standby row. A literal like the
 * rows above. */
static int dev_clock_toggle(void)
{
    clockpol_set_enabled(!clockpol_enabled());
    return 0;
}
static char *dev_clock_name(int selected_item, void *data,
                            char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data; (void)buffer; (void)buffer_len;
    return (char *)(clockpol_enabled() ? "24 MHz idle: on"
                                       : "24 MHz idle: off");
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_clock_item, 0, dev_clock_toggle,
                          dev_clock_name, NULL, NULL, NULL, Icon_NOICON);

/* p19-battery: the lit clock (clockpol.c; docs/design/lit-24mhz.md).
 * Not saved: 30 MHz at every boot until a meter shows the saving. */
static int dev_lit_toggle(void)
{
    clockpol_lit24_set(!clockpol_lit24());
    return 0;
}
static char *dev_lit_name(int selected_item, void *data,
                          char *buffer, size_t buffer_len)
{
    (void)selected_item; (void)data;
    return devstatus_with_verdict(clockpol_lit24() ? "Lit screen: 24 MHz"
                                                   : "Lit screen: 30 MHz",
                                  "lit24ui", buffer, buffer_len);
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_lit_item, 0, dev_lit_toggle,
                          dev_lit_name, NULL, NULL, NULL, Icon_NOICON);
#define DEV_CLOCK_ITEM , &ipodui_dev_clock_item, &ipodui_dev_lit_item
#else
#define DEV_CLOCK_ITEM
#endif

#ifdef HAVE_DEVSTATS
/* Fit-3 step 0 (docs/fit3-brief.md section 3): the data-path counters as
 * per-minute rates over the window since the last log line (deltas from
 * devstats_snapshot(), never cleared; lcd_max_us is the all-time max).
 * Developer rows, literals like the rows above. Once a minute, when the
 * disk is awake anyway (S36's rule: ui_log() then writes at its idle
 * flush, so the lines never wake it; asleep, the window just grows),
 * ipodui.log gets the window's raw counts, parsed by scripts/
 * fit3_stats.py (each line fits UI_LOG_LINE; port/art/pcm only if any):
 *   f3 lcd s=S n=UPDATES kb=KB ms=MS max=MAX_US
 *   f3 lcde2 kb=KB_BY_ENGINE ms=ENGINE_MS fail=FAILS (HAVE_LCD_E2)
 *   f3 port ovl=SENDS kb=KB tick=TICK_READS poll=POLL_READS
 *   f3 art n=IMAGES kb=FILE_KB r=STORAGE_CMDS rms=OFFCORE_MS dms=ONCORE_MS
 *          bo=BOOSTS
 *   f3 pcm wb=WRITEBACKS us=US */
#define F3_WORDS (sizeof(struct devstats_fit3) / sizeof(uint32_t))
typedef char f3_all_words[sizeof(struct devstats_fit3) == F3_WORDS * 4
                          ? 1 : -1] __attribute__((unused));
static struct devstats_fit3 f3_base;
static long f3_base_tick;

/* d = now - base (lcd_max_us: now's); returns the window in ticks (>= 1) */
static long f3_delta(struct devstats_fit3 *d, struct devstats_fit3 *now)
{
    struct devstats s;
    const uint32_t *a = (const uint32_t *)&s.f3;
    const uint32_t *b = (const uint32_t *)&f3_base;
    uint32_t *o = (uint32_t *)d;
    unsigned int i;
    long t = current_tick - f3_base_tick;

    devstats_snapshot(&s);
    for (i = 0; i < F3_WORDS; i++)
        o[i] = a[i] - b[i];
    d->lcd_max_us = s.f3.lcd_max_us;
    if (now)
        *now = s.f3;
    return t > 0 ? t : 1;
}

static unsigned long f3_pm(uint32_t v, long ticks)     /* per minute */
{
    return (unsigned long)((uint64_t)v * (60 * HZ) / (unsigned long)ticks);
}

static int f3_noop(void)
{
    return 0;
}
static char *f3_lcd_name(int selected_item, void *data,
                         char *buffer, size_t buffer_len)
{
    struct devstats_fit3 d;
    long t = f3_delta(&d, NULL);

    (void)selected_item; (void)data;
    snprintf(buffer, buffer_len, "LCD/min %lu %luKB %lums max %luus",
             f3_pm(d.lcd_updates, t), f3_pm(d.lcd_bytes, t) / 1024,
             f3_pm(d.lcd_us, t) / 1000, (unsigned long)d.lcd_max_us);
    return buffer;
}
static char *f3_port_name(int selected_item, void *data,
                          char *buffer, size_t buffer_len)
{
    struct devstats_fit3 d;
    long t = f3_delta(&d, NULL);

    (void)selected_item; (void)data;
    snprintf(buffer, buffer_len, "Port/min ovl %lu %luKB rd %lu+%lu",
             f3_pm(d.ovl_updates, t), f3_pm(d.ovl_bytes, t) / 1024,
             f3_pm(d.rd_tick, t), f3_pm(d.rd_poll, t));
    return buffer;
}
static char *f3_art_name(int selected_item, void *data,
                         char *buffer, size_t buffer_len)
{
    struct devstats_fit3 d;
    long t = f3_delta(&d, NULL);

    (void)selected_item; (void)data;
    snprintf(buffer, buffer_len, "Art/min %lu %luKB r%lu %lu+%lums b%lu",
             f3_pm(d.art_builds, t), f3_pm(d.art_bytes, t) / 1024,
             f3_pm(d.art_reads, t), f3_pm(d.art_read_us, t) / 1000,
             f3_pm(d.art_dec_us, t) / 1000, f3_pm(d.art_boosts, t));
    return buffer;
}
static char *f3_pcm_name(int selected_item, void *data,
                         char *buffer, size_t buffer_len)
{
    struct devstats_fit3 d;
    long t = f3_delta(&d, NULL);

    (void)selected_item; (void)data;
    snprintf(buffer, buffer_len, "PCM wb/min %lu %luus",
             f3_pm(d.pcm_wb, t), f3_pm(d.pcm_wb_us, t));
    return buffer;
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_f3_lcd_item, 0, f3_noop,
                          f3_lcd_name, NULL, NULL, NULL, Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_f3_port_item, 0, f3_noop,
                          f3_port_name, NULL, NULL, NULL, Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_f3_art_item, 0, f3_noop,
                          f3_art_name, NULL, NULL, NULL, Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_f3_pcm_item, 0, f3_noop,
                          f3_pcm_name, NULL, NULL, NULL, Icon_NOICON);
#define DEV_F3_ITEMS , &ipodui_dev_f3_lcd_item, &ipodui_dev_f3_port_item, \
                       &ipodui_dev_f3_art_item, &ipodui_dev_f3_pcm_item

#if !defined(SIMULATOR) && (CONFIG_STORAGE & STORAGE_ATA)
/* Fit-3b delta (one build for flash and disk): the first wake from
 * standby after boot, once. storage_spinup_time() is set when the wake's
 * first transfer ends, after devstats sees the wake, so the line waits
 * for the value to change (10 s at most: then it is the same value). */
static void f3_wake1(void)
{
    static uint8_t stage;           /* 0 no sleep yet, 1 asleep, 2 woke */
    static uint32_t base;
    static int sp0;
    static long t0;

    if (stage == 3)
        return;
    if (stage == 0)
    {
        if (devstats.sleeps)
        {
            stage = 1;
            base = devstats.wake_head;
        }
        return;
    }
    if (stage == 1)
    {
        if (devstats.wake_head == base)
            return;
        stage = 2;
        sp0 = storage_spinup_time();
        t0 = current_tick;
        return;
    }
    if (storage_spinup_time() == sp0 && TIME_BEFORE(current_tick, t0 + 10*HZ))
        return;
    stage = 3;
    ui_log("storage", "wake1 spinup_ms=%d",
           storage_spinup_time() * (1000 / HZ));
}
#else
#define f3_wake1() do { } while (0)
#endif

/* UI thread, every main-loop pass (powerlog_service()) */
void ipodui_devmode_f3_service(void)
{
    struct devstats_fit3 d, now;
    long t;
    unsigned long s;

    f3_wake1();
    if (TIME_BEFORE(current_tick, f3_base_tick + 60 * HZ)
        || !artcache_disk_awake())
        return;                     /* never a wake: the window grows */
    t = f3_delta(&d, &now);
    s = (unsigned long)(t / HZ);
    ui_log("f3", "lcd s=%lu n=%lu kb=%lu ms=%lu max=%lu", s,
           (unsigned long)d.lcd_updates, (unsigned long)(d.lcd_bytes / 1024),
           (unsigned long)(d.lcd_us / 1000), (unsigned long)d.lcd_max_us);
    if (d.lcd_e2_bytes || d.lcd_e2_fails)
        ui_log("f3", "lcde2 kb=%lu ms=%lu fail=%lu",
               (unsigned long)(d.lcd_e2_bytes / 1024),
               (unsigned long)(d.lcd_e2_us / 1000),
               (unsigned long)d.lcd_e2_fails);
    if (d.ovl_updates || d.rd_tick || d.rd_poll)
        ui_log("f3", "port ovl=%lu kb=%lu tick=%lu poll=%lu",
               (unsigned long)d.ovl_updates,
               (unsigned long)(d.ovl_bytes / 1024),
               (unsigned long)d.rd_tick, (unsigned long)d.rd_poll);
    if (d.art_builds)
        ui_log("f3", "art n=%lu kb=%lu r=%lu rms=%lu dms=%lu bo=%lu",
               (unsigned long)d.art_builds,
               (unsigned long)(d.art_bytes / 1024),
               (unsigned long)d.art_reads,
               (unsigned long)(d.art_read_us / 1000),
               (unsigned long)(d.art_dec_us / 1000),
               (unsigned long)d.art_boosts);
    if (d.pcm_wb)
        ui_log("f3", "pcm wb=%lu us=%lu", (unsigned long)d.pcm_wb,
               (unsigned long)d.pcm_wb_us);
    f3_base = now;
    f3_base_tick = current_tick;
}
#else
#define DEV_F3_ITEMS
#endif /* HAVE_DEVSTATS */

/* #106: "Loader & storage", a shell screen (a module on the target). The
 * stock list can't host it: the row leaves the list and
 * ipodui_devmode_run() pushes the screen once the list is gone. Last, so
 * the other rows keep their places. */
#ifdef IPODUI_MODULES
static bool dev_loader_pending;
static int dev_loader_open(void)
{
    dev_loader_pending = true;
    return 1;                           /* MENU_FUNC_CHECK_RETVAL: leave */
}
MENUITEM_FUNCTION(ipodui_dev_loader_item, MENU_FUNC_CHECK_RETVAL,
                  (unsigned char *)"Loader & storage",
                  dev_loader_open, NULL, Icon_NOICON);
#define DEV_LOADER_ITEM , &ipodui_dev_loader_item
#else
#define DEV_LOADER_ITEM
#endif

/* ---- The layout (owner, 2026-10-26; docs/design/devmode.md section 3)
 * What is on is one screen (Status); what the owner presses is up front
 * (Bench session, the power log, the battery table); the switches, the
 * one-off checks, the labs and the video chip's rows are under Advanced.
 * A release build has none of this (HAVE_IPOD_UI_DEVMODE). */

static int dev_status(void)
{
    return devstatus_show();
}
MENUITEM_FUNCTION(ipodui_dev_status_item, 0, ID2P(LANG_IPODUI_DEV_STATUS),
                  dev_status, NULL, Icon_NOICON);

#if defined(HAVE_BCMFW_DEVTEST) || defined(DEVBENCH_SIM)
/* A function of the stock debug screens' kind (bool: USB was attached),
 * run as debug_menu() runs its rows: the theme off around it */
static void dev_run_screen(bool (*fn)(void))
{
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, false, NULL);
    fn();
    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, false);
}
#endif

#if (defined(HAVE_BCMFW_DEVTEST) && defined(HAVE_BCMFW_VIDEO) \
     && !defined(SIMULATOR)) || defined(DEVBENCH_SIM)
static int dev_bench(void)
{
    dev_run_screen(devbench_session);
    return 0;
}
MENUITEM_FUNCTION(ipodui_dev_bench_item, 0, ID2P(LANG_IPODUI_DEV_BENCH),
                  dev_bench, NULL, Icon_NOICON);
#define DEV_BENCH_ITEM &ipodui_dev_bench_item,
#else
#define DEV_BENCH_ITEM
#endif

#if defined(HAVE_PP_GOV) && defined(HAVE_PLL_IDLE)
/* The speed governor (pgov, docs/design/pp-governor.md): off -> stage 1
 * -> log only -> apply -> off, for the next start (the markers
 * clockpol_init() reads; boot code unchanged).  apply takes a second
 * SELECT within 5 s; a press after the window goes to off instead. */
static long gov_armed;              /* tick of the press that armed, or 0 */
static bool gov_arm_live(void)
{
    return gov_armed && TIME_BEFORE(current_tick, gov_armed + 5 * HZ);
}
static int dev_gov_toggle(void)
{
    enum devgov g = devgov_get();

    if (gov_armed)
    {
        bool live = gov_arm_live();

        gov_armed = 0;
        devgov_set(live ? DEVGOV_APPLY : DEVGOV_OFF);
        return 0;
    }
#ifdef HAVE_PP_GOV_DVS
    if (g == DEVGOV_LOG)
    {
        gov_armed = current_tick | 1;
        return 0;
    }
#endif
    devgov_set(g == DEVGOV_OFF ? DEVGOV_STAGE1
#ifdef HAVE_PP_GOV_DVS
               : g == DEVGOV_STAGE1 ? DEVGOV_LOG
#endif
               : DEVGOV_OFF);
    return 0;
}
static char *dev_gov_name(int selected_item, void *data,
                          char *buffer, size_t buffer_len)
{
    char t[64];

    (void)selected_item; (void)data;
    if (gov_arm_live())
        return str(LANG_IPODUI_DEV_GOV_CONFIRM);
    snprintf(t, sizeof(t), str(LANG_IPODUI_DEV_GOV),
             devgov_name(devgov_get()));
    if (gov_armed)                  /* the window passed: next is off */
        strlcat(t, str(LANG_IPODUI_DEV_GOV_NEXT_OFF), sizeof(t));
    else if (devgov_refused())
        strlcat(t, str(LANG_IPODUI_DEV_GOV_REFUSED), sizeof(t));
    return devstatus_with_verdict(t, "vgov", buffer, buffer_len);
}
MENUITEM_FUNCTION_DYNTEXT(ipodui_dev_gov_item, 0, dev_gov_toggle,
                          dev_gov_name, NULL, NULL, NULL, Icon_NOICON);
#define DEV_GOV_ITEM &ipodui_dev_gov_item,
#else
#define DEV_GOV_ITEM
#endif

#ifdef HAVE_BCMFW_DEVTEST
/* Video chip: the BCM rows (devbcm.h), stock-list tables as the debug
 * menu showed them.  A row's text is a literal or ID2P(LANG_...). */
static const struct devbcm_row *dt_rows;
static int dt_n;

static int dt_action(int btn, struct gui_synclist *lists)
{
    if (btn == ACTION_STD_OK)
    {
        int sel = gui_synclist_get_sel_pos(lists);

        if (sel >= 0 && sel < dt_n)
            dev_run_screen(dt_rows[sel].fn);
        btn = ACTION_REDRAW;
    }
    return btn;
}

static const char *dt_name(int item, void *data, char *buf, size_t len)
{
    const unsigned char *d = (const unsigned char *)dt_rows[item].desc;

    (void)data; (void)buf; (void)len;
    return P2STR(d);
}

/* One table; a row may open another (the outer one's rows come back) */
static bool dev_table(int title_id, const struct devbcm_row *rows, int n)
{
    struct simplelist_info info;
    const struct devbcm_row *prev = dt_rows;
    int prev_n = dt_n;
    bool usb;

    dt_rows = rows;
    dt_n = n;
    simplelist_info_init(&info, str(title_id), n, NULL);
    info.action_callback = dt_action;
    info.get_name = dt_name;
    usb = simplelist_show_list(&info);
    dt_rows = prev;
    dt_n = prev_n;
    return usb;
}

#ifdef HAVE_BCMFW
static const struct devbcm_row vc_boot_rows[] = { DEVBCM_BOOT_ROWS };
static bool vc_boot(void)
{
    return dev_table(LANG_IPODUI_DEV_VC_BOOT, vc_boot_rows,
                     ARRAYLEN(vc_boot_rows));
}
#if defined(HAVE_BCMFW_L1) || defined(HAVE_BCMJOB_INJECT)
#ifdef HAVE_BCMFW_L1
#define VC_L1_ROWS DEVBCM_L1_ROWS
#else
#define VC_L1_ROWS
#endif
static const struct devbcm_row vc_fault_rows[] = { VC_L1_ROWS DEVJF_ROWS };
static bool vc_faults(void)
{
    return dev_table(LANG_IPODUI_DEV_VC_FAULTS, vc_fault_rows,
                     ARRAYLEN(vc_fault_rows));
}
#define VC_FAULTS_ROW { (const char *)ID2P(LANG_IPODUI_DEV_VC_FAULTS), \
                        vc_faults },
#else
#define VC_FAULTS_ROW
#endif
#define VC_BOOT_ROWS { (const char *)ID2P(LANG_IPODUI_DEV_VC_BOOT), \
                       vc_boot }, VC_FAULTS_ROW
#else
#define VC_BOOT_ROWS
#endif
static const struct devbcm_row vc_probe_rows[] = { DEVBCM_PROBE_ROWS };
static bool vc_probes(void)
{
    return dev_table(LANG_IPODUI_DEV_VC_PROBES, vc_probe_rows,
                     ARRAYLEN(vc_probe_rows));
}
#ifdef HAVE_BCM_JOBS
#define VC_DRIFT_ROW DRIFT_JOBS_DEV_ITEM
#else
#define VC_DRIFT_ROW
#endif
static const struct devbcm_row vc_rows[] = {
    VC_BOOT_ROWS
    { (const char *)ID2P(LANG_IPODUI_DEV_VC_PROBES), vc_probes },
    DEVBCM_JOB_ROWS
    VC_DRIFT_ROW
};
static int dev_vchip(void)
{
    dev_table(LANG_IPODUI_DEV_VIDEO_CHIP, vc_rows, ARRAYLEN(vc_rows));
    return 0;
}
MENUITEM_FUNCTION(ipodui_dev_vchip_item, 0,
                  ID2P(LANG_IPODUI_DEV_VIDEO_CHIP), dev_vchip, NULL,
                  Icon_NOICON);
#define DEV_VCHIP_ITEM &ipodui_dev_vchip_item,
#else
#define DEV_VCHIP_ITEM
#endif

/* Experiments: every switch, each with its state (and its bench step's
 * verdict, read once on entry) */
static int dev_exp_cb(int action, const struct menu_item_ex *this_item,
                      struct gui_synclist *this_list)
{
    (void)this_item; (void)this_list;
    if (action == ACTION_ENTER_MENUITEM)
        devstatus_bench_load();
    return action;
}
MAKE_MENU(ipodui_dev_exp_menu, ID2P(LANG_IPODUI_DEV_EXPERIMENTS),
          dev_exp_cb, Icon_NOICON,
          DEV_GOV_ITEM
#ifdef HAVE_UI_THREAD_SPLIT
          &ipodui_dev_uisplit_item,
#endif
          &ipodui_dev_softsleep_item
          DEV_STOR_ITEM, &ipodui_dev_standby_item
          DEV_CLOCK_ITEM DEV_RESIDENT_ITEMS DEV_UDMA_ITEMS DEV_USB_EXP_ITEMS);

/* Diagnostics: one-off checks and live readings */
MAKE_MENU(ipodui_dev_diag_menu, ID2P(LANG_IPODUI_DEV_DIAGNOSTICS), NULL,
          Icon_NOICON, &ipodui_dev_apath_item DEV_PANIC_ITEMS DEV_F3_ITEMS
          DEV_LOADER_ITEM DEV_BATCAL_RUN_ITEM DEV_FSINFO_ITEM
          DEV_USB_INQ_ITEM);

/* Labs: the guided sleep, alarm and drive-power runs */
MAKE_MENU(ipodui_dev_labs_menu, ID2P(LANG_IPODUI_DEV_LABS), NULL,
          Icon_NOICON, DEV_SLEEPLAB_ITEM &ipodui_dev_alarmlab_menu
          DEV_AB_ITEMS);

MAKE_MENU(ipodui_dev_adv_menu, ID2P(LANG_IPODUI_DEV_ADVANCED), NULL,
          Icon_NOICON, &ipodui_dev_exp_menu, &ipodui_dev_diag_menu,
          &ipodui_dev_labs_menu, DEV_VCHIP_ITEM &ipodui_dev_debug_item);

MAKE_MENU(ipodui_dev_menu, ID2P(LANG_IPODUI_DEVELOPER), NULL, Icon_NOICON,
          &ipodui_dev_status_item, DEV_BENCH_ITEM
          &ipodui_dev_powerlog_item, &ipodui_dev_powerlog_every_item
          DEV_BATCAL_ITEMS, &ipodui_dev_adv_menu, &ipodui_dev_back_item);

static void click(void)
{
#if defined(HAVE_HARDWARE_CLICK) && !defined(SIMULATOR)
    piezo_button_beep(false, true);     /* one tick, whatever the setting */
#endif
}

/* The main loop's global layer, before the top screen. The chord arrives
 * in steps (devmode.md §1): the lone SELECT first, which About ignores,
 * then SELECT|LEFT, then its repeats while held. From the arm on, every
 * event up to the chord's release is consumed, so the screen never acts
 * on it; the wheel (posted apart) neither breaks nor reaches it. */
bool ipodui_devmode_feed(const struct ui_screen *top, long raw_btn)
{
    long bits = raw_btn & ~(BUTTON_REL | BUTTON_REPEAT);

    if (raw_btn == BUTTON_NONE || (raw_btn & SYS_EVENT))
        return false;
    if (armed && (!top || top->ops != &ui_about_screen))
    {
        armed = false;                  /* left About mid-chord (USB took
                                         * the release): never stick */
        UPDATE_TRACE("devmode disarm%s", "");
    }
    if (armed)
    {
        if (bits & ~(DEVMODE_CHORD | BUTTON_SCROLL_FWD | BUTTON_SCROLL_BACK))
        {
            armed = false;              /* another key: not the chord */
            return false;
        }
        if (raw_btn & BUTTON_REL)
        {
            if (!(button_status() & DEVMODE_CHORD))
                armed = false;          /* the last chord key is up */
            return true;
        }
        if (!fired && bits == DEVMODE_CHORD
            && current_tick - arm_tick >= DEVMODE_HOLD_TICKS)
        {
            fired = true;
            click();
            ipodui_devmode_run();
            armed = false;          /* the stock list took the releases */
        }
        return true;
    }
    if (!top || top->ops != &ui_about_screen)
        return false;                   /* M6: About only */
    if (bits == DEVMODE_CHORD && !(raw_btn & BUTTON_REL))
    {
        armed = true;
        fired = false;
        arm_tick = current_tick;
        UPDATE_TRACE("devmode arm%s", "");
        return true;
    }
    return false;
}

void ipodui_devmode_run(void)
{
    ui_log_init();                      /* the boot escape runs first */
    UPDATE_TRACE("devmode enter%s", "");
    in_devmode = true;                  /* M3: splashes pass from here */

    /* H1: exactly one push and one pop per screen */
    FOR_NB_SCREENS(i)
        viewportmanager_theme_enable(i, true, NULL);
    do_menu(&ipodui_dev_menu, NULL, NULL, false);
    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, false);

    in_devmode = false;
    FOR_NB_SCREENS(i)
    {
        screens[i].set_viewport(NULL);  /* the shell's own viewport */
        screens[i].scroll_stop();       /* L6: no stock marquee left */
    }
    ui_invalidate(UI_DIRTY_ALL);        /* header and body: one full redraw
                                         * (a no-op before the shell) */
    ui_unlit_foreign_draw();
    UPDATE_TRACE("devmode exit%s", "");
#ifdef IPODUI_MODULES
    if (dev_loader_pending)
    {
        dev_loader_pending = false;
        /* over the stock list's last frame: no slide, one full draw.
         * Not from the boot entry (main.c): no shell stack yet. */
        if (ui_depth() > 0 && ui_push(&ui_devloader_screen, NULL) == 0)
            ui_take_transition();
    }
#endif
}

bool ipodui_devmode_boot_requested(void)
{
    return (button_status() & BUTTON_RIGHT) != 0;   /* L7 */
}

bool ipodui_stock_ui_allowed(void)
{
    return in_devmode;
}

#endif /* HAVE_IPOD_UI_DEVMODE */
