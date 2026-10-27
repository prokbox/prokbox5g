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
 * iPod UI shell: the developer Alarm lab (P12 5.4: D-A1, D-A3, D-A5, D-A6,
 * D-A8). Developer builds only. Every row logs to ipodui.log.
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
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lang.h"
#include "settings.h"             /* ID2P */
#include "../menu.h"               /* the stock do_menu(), not ipodui/menu.h */
#include "powermgmt.h"
#include "rtc.h"
#include "timefuncs.h"
#include "ipodui/alarm_ring.h"
#include "ipodui/powerlog.h"
#include "ipodui/uilog.h"

#ifdef HAVE_IPOD_UI_DEVMODE

static uint32_t lab_now(void)
{
    struct tm *tm = get_time();

    return alarm_rtc_make(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                          tm->tm_hour, tm->tm_min, tm->tm_sec);
}

/* the log reaches the disk before the power goes (input.c's SYS_POWEROFF
 * flushes too; this is the lab's own line first) */
static void lab_off(void)
{
    ui_log_flush();
    sys_poweroff();
}

/* D-A5: three cycles of the ring pattern (5.3) at one frequency, with the
 * cell's voltage before and after (the power log gives the slope) */
static int lab_beep(void *param)
{
    unsigned hz = (unsigned)(intptr_t)param;
    int mv0 = battery_voltage(), c, b;

    for (c = 0; c < 3; c++)
    {
        for (b = 0; b < 4; b++)
        {
            alarm_beep_burst(hz);
            sleep(HZ / 5);
        }
        sleep(HZ * 4 / 5);
    }
    ui_log("alarm", "lab beep hz=%u mv0=%d mv1=%d", hz, mv0,
           battery_voltage());
    return 0;
}

static char *lab_beep_name(int selected_item, void *data,
                                 char *buffer, size_t buffer_len)
{
    (void)selected_item;
    snprintf(buffer, buffer_len, str(LANG_IPODUI_X_ALAB_BEEP),
             (int)(intptr_t)data);
    return buffer;
}

/* D-A1 (and D-A6 with a charger or USB host plugged in): a real one-shot
 * alarm through the engine (alarms.dat + the PMU, exact date), at the
 * first whole minute 2 min or more ahead, then a real power-off: the dark
 * wait is not asked. The next boot logs "alarm boot armed= late=". */
static int lab_arm_off(void)
{
    uint32_t at = (lab_now() + 120 + 59) / 60 * 60;
    struct alarm a;
    int y, mo, d, h, m, s;

    alarm_rtc_split(at, &y, &mo, &d, &h, &m, &s);
    memset(&a, 0, sizeof(a));
    a.on = 1;
    a.repeat = AR_ONCE;
    a.hour = h;
    a.min = m;
    a.year = y;
    a.mon = mo;
    a.mday = d;
    a.label = 1;
    if (alarm_set(-1, &a) < 0)
    {
        ui_log("alarm", "lab arm: full");
        return 0;
    }
    ui_log("alarm", "lab arm off at=%lu next=%lu", (unsigned long)at,
           (unsigned long)alarm_next_rtc());
    lab_off();
    return 0;
}

#ifdef HAVE_RTC_ALARM
/* D-A3: the PMU armed by hand (the lab's only direct write; alarm_rearm()
 * puts the engine's image back at the next boot), then a power-off */
static void lab_arm_regs(const uint8_t regs[7], const char *what)
{
    uint8_t got[7];
    bool w = false;

    rtc_set_alarm_full(regs, true);
    rtc_get_alarm_full(got, &w);
    ui_log("alarm", "lab %s %02x:%02x:%02x %02x/%02x/%02x ok=%d", what,
           got[2], got[1], got[0], got[4], got[5], got[6],
           !memcmp(got, regs, 7) && w);
    lab_off();
}

/* tomorrow's date, 2 min ahead of now: it must not wake today */
static int lab_arm_date(void)
{
    uint8_t r[7];

    alarm_pmu_regs(lab_now() + 120 + 86400, false, r);
    lab_arm_regs(r, "date");
    return 0;
}

/* 2 min ahead with the year at 0x99 (upstream's "off"): it must never
 * wake */
static int lab_arm_never(void)
{
    uint8_t r[7];

    alarm_pmu_regs(lab_now() + 120, true, r);
    r[6] = 0x99;
    lab_arm_regs(r, "never");
    return 0;
}
#endif

/* D-A8: the power log on, then the dark wait as hold PLAY takes it; a key
 * ends it. Leave it 8 h; the log's mV slope is the drain */
static int lab_darkwait(void)
{
    if (!powerlog_is_on())
        powerlog_set(true);
    ui_log("alarm", "lab darkwait mv=%d next=%lu", battery_voltage(),
           (unsigned long)alarm_next_rtc());
    powerlog_mark("darkwait");
    alarm_dark_wait();
    return 0;
}

MENUITEM_FUNCTION_DYNTEXT_W_PARAM(alab_beep_2k, 0, lab_beep,
                                  (void *)(intptr_t)2000, lab_beep_name,
                                  NULL, (void *)(intptr_t)2000, NULL,
                                  Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT_W_PARAM(alab_beep_1k, 0, lab_beep,
                                  (void *)(intptr_t)1000, lab_beep_name,
                                  NULL, (void *)(intptr_t)1000, NULL,
                                  Icon_NOICON);
MENUITEM_FUNCTION_DYNTEXT_W_PARAM(alab_beep_500, 0, lab_beep,
                                  (void *)(intptr_t)500, lab_beep_name,
                                  NULL, (void *)(intptr_t)500, NULL,
                                  Icon_NOICON);
MENUITEM_FUNCTION(alab_arm_off, 0, ID2P(LANG_IPODUI_X_ALAB_ARM_OFF),
                  lab_arm_off, NULL, Icon_NOICON);
#ifdef HAVE_RTC_ALARM
MENUITEM_FUNCTION(alab_arm_date, 0, ID2P(LANG_IPODUI_X_ALAB_ARM_DATE),
                  lab_arm_date, NULL, Icon_NOICON);
MENUITEM_FUNCTION(alab_arm_never, 0, ID2P(LANG_IPODUI_X_ALAB_ARM_NEVER),
                  lab_arm_never, NULL, Icon_NOICON);
#define ALAB_PMU_ITEMS , &alab_arm_date, &alab_arm_never
#else
#define ALAB_PMU_ITEMS
#endif
MENUITEM_FUNCTION(alab_darkwait, 0, ID2P(LANG_IPODUI_X_ALAB_DARKWAIT),
                  lab_darkwait, NULL, Icon_NOICON);
MAKE_MENU(ipodui_dev_alarmlab_menu, ID2P(LANG_IPODUI_X_ALAB), NULL,
          Icon_NOICON, &alab_beep_2k, &alab_beep_1k, &alab_beep_500,
          &alab_arm_off ALAB_PMU_ITEMS, &alab_darkwait);

#endif /* HAVE_IPOD_UI_DEVMODE */
