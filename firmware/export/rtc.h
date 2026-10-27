/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Linus Nielsen Feltzing, Uwe Freese
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
#ifndef _RTC_H_
#define _RTC_H_

#include <stdbool.h> 
#include "system.h"
#include "config.h"
#include "time.h"

/* Macros used to convert to and from BCD, used in various rtc drivers
   this is the wrong place but misc.h is in apps... */
#define BCD2DEC(X) (((((X)>>4) & 0x0f) * 10) + ((X) & 0xf))
#define DEC2BCD(X) ((((X)/10)<<4) | ((X)%10))

#if CONFIG_RTC

/* Common functions for all targets */
void rtc_init(void);
int rtc_read_datetime(struct tm *tm);
int rtc_write_datetime(const struct tm *tm);

#ifdef HAVE_RTC_ALARM
void rtc_set_alarm(int h, int m);
void rtc_get_alarm(int *h, int *m);
void rtc_enable_alarm(bool enable);
bool rtc_check_alarm_started(bool release_alarm);
bool rtc_check_alarm_flag(void);
#ifdef HAVE_IPOD_UI
/* ProkBox P12 (docs/phase12-brief.md 3.3; W1-A1): the whole alarm register
 * image 0x11..0x17 (BCD sec, min, hour, weekday, mday, month, year; the
 * "ignore" values per field) and the wake-on-RTC bit for the next standby.
 * apps/ipodui/alarm.c's alarm_rearm() is the only caller of the set (H2).
 * 0 = done, < 0 = an I2C error. */
int  rtc_set_alarm_full(const unsigned char regs[7], bool wake);
int  rtc_get_alarm_full(unsigned char regs[7], bool *wake);
/* H6: before any power-off that may come before alarm_init() (the boot
 * guard's refusal, rtc_init()): RTCWAK if the PMU holds a valid alarm
 * still ahead; one that just passed becomes the daily match of the same
 * time (rtc_alarm_keep.h). Disk-free. */
void rtc_keep_alarm_wake(void);
#endif
#endif /* HAVE_RTC_ALARM */

#endif /* CONFIG_RTC */

#endif

