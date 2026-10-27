/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Björn Stenberg <bjorn@haxx.se>
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
#include <time.h>
#include <stdbool.h>
#include "debug.h"

#include "screens.h"
#include "button.h"

#include "string.h"
#include "lcd.h"

#include "power.h"

#include "ata.h" /* for volume definitions */

static bool storage_spinning = false;

#if defined(HAVE_DEVICEDATA)
#include "devicedata.h"
struct device_data_t device_data =
    {.length = DEVICE_DATA_PAYLOAD_SIZE};
#endif /* def HAVE_DEVICEDATA */

int fat_startsector(void)
{
    return 63;
}

int ata_spinup_time(void)
{
    return 100;
}

int storage_spinup_time(void)
{
    return 0;
}

int storage_init(void)
{
    return 1;
}

int storage_write_sectors(IF_MV(int drive,)
                      unsigned long start,
                      int count,
                      const void* buf)
{
    IF_MV((void)drive;)
    int i;

    for (i=0; i<count; i++ ) {
        FILE* f;
        char name[32];

        sprintf(name,"sector%lX.bin",start+i);
        f=fopen(name,"wb");
        if (f) {
            if(fwrite(buf,512,1,f) != 1) {
                fclose(f);
                return -1;
            }
            fclose(f);
        }
        else {
            return -1;
        }
    }
    return 0;
}

int storage_read_sectors(IF_MV(int drive,)
                     unsigned long start,
                     int count,
                     void* buf)
{
    IF_MV((void)drive;)
    int i;
    size_t ret;

    for (i=0; i<count; i++ ) {
        FILE* f;
        char name[32];

        DEBUGF("Reading sector %lX\n",start+i);
        sprintf(name,"sector%lX.bin",start+i);
        f=fopen(name,"rb");
        if (f) {
            ret = fread(buf,512,1,f);
            fclose(f);
            if (ret != 1)
                return -1;
        }
    }
    return 0;
}

void storage_spin(void)
{
    storage_spinning = true;
}

void storage_sleep(void)
{
}

bool storage_disk_is_active(void)
{
    return storage_spinning;
}

void storage_spindown(int s)
{
    (void)s;
    storage_spinning = false;
}

int rtc_read(int address)
{
    return address ^ 0x55;
}

int rtc_write(int address, int value)
{
    (void)address;
    (void)value;
    return 0;
}

#ifdef HAVE_RTC_ALARM
#ifdef HAVE_IPOD_UI
/* P12: the alarm calls go through the sim's PMU model (the registers,
 * INT1 and RTCWAK of uisimulator/common/sim_cmd_extras.c), mirroring
 * firmware/drivers/rtc/rtc_pcf50605.c */
#include "pcf50605.h"
bool sim_pmu_alarm_flag(void);

static int bcd2(int v) { return (v >> 4) * 10 + (v & 15); }

void rtc_get_alarm(int *h, int *m)
{
    *h = bcd2(pcf50605_read(0x13));
    *m = bcd2(pcf50605_read(0x12));
}

void rtc_set_alarm(int h, int m)
{
    unsigned char buf[3] = { 0, (unsigned char)((m / 10) << 4 | m % 10),
                             (unsigned char)((h / 10) << 4 | h % 10) };
    pcf50605_write_multiple(0x11, buf, 3);
}

void rtc_enable_alarm(bool enable)
{
    static const unsigned char ignore[4] = { 0x07, 0x3f, 0x1f, 0xff };

    if (enable)
    {
        pcf50605_write_multiple(0x14, ignore, 4);
        pcf50605_write(0x5, pcf50605_read(0x5) & ~0x80);
        pcf50605_wakeup_flags |= 0x10;
    }
    else
    {
        pcf50605_write(0x17, 0x99);
        pcf50605_wakeup_flags &= ~0x10;
    }
}

bool rtc_check_alarm_flag(void)
{
    return sim_pmu_alarm_flag();
}
#else
void rtc_get_alarm(int *h, int *m)
{
    *h = 11;
    *m = 55;
}

void rtc_set_alarm(int h, int m)
{
    (void)h;
    (void)m;
}

void rtc_enable_alarm(bool enable)
{
    (void)enable;
}

bool rtc_check_alarm_flag(void)
{
    return true;
}
#endif

extern bool sim_alarm_wakeup;
bool rtc_check_alarm_started(bool release_alarm)
{
    (void)release_alarm;
    return sim_alarm_wakeup;
}

#ifdef HAVE_IPOD_UI
/* ProkBox P12 (W1-A1): the rtc.h alarm image contract, the same calls as
 * rtc_pcf50605.c on the sim's PMU model (sim_cmd_extras.c) */
#include "rtc_alarm_keep.h"
#include "rtc.h"

int rtc_set_alarm_full(const unsigned char regs[7], bool wake)
{
    pcf50605_write_multiple(0x11, regs, 7);
    if (wake)
    {
        pcf50605_write(0x5, pcf50605_read(0x5) & ~0x80);
        pcf50605_wakeup_flags |= 0x10;
    }
    else
        pcf50605_wakeup_flags &= ~0x10;
    return 0;
}

int rtc_get_alarm_full(unsigned char regs[7], bool *wake)
{
    pcf50605_read_multiple(0x11, regs, 7);
    if (wake)
        *wake = (pcf50605_wakeup_flags & 0x10) != 0;
    return 0;
}

static unsigned char sim_bcd(int v)
{
    return (unsigned char)((v / 10) << 4 | v % 10);
}

void rtc_keep_alarm_wake(void)
{
    static const unsigned char daily[4] = RTC_ALARM_DAILY_TAIL;
    /* the model has no clock registers: the RTC the firmware reads, in
     * the 0x0A..0x10 layout */
    struct tm tm;
    unsigned char now[7], alm[7];

    rtc_read_datetime(&tm);
    now[0] = sim_bcd(tm.tm_sec);
    now[1] = sim_bcd(tm.tm_min);
    now[2] = sim_bcd(tm.tm_hour);
    now[3] = (unsigned char)tm.tm_wday;
    now[4] = sim_bcd(tm.tm_mday);
    now[5] = sim_bcd(tm.tm_mon + 1);
    now[6] = sim_bcd(tm.tm_year - 100);
    pcf50605_read_multiple(0x11, alm, 7);
    switch (rtc_alarm_keep(now, alm))
    {
    case RTC_KEEP_DAILY:
        pcf50605_write_multiple(0x14, daily, 4);
        /* fall through */
    case RTC_KEEP_WAKE:
        pcf50605_wakeup_flags |= 0x10;
        break;
    default:
        break;
    }
}
#endif
#endif

#ifdef HAVE_SPDIF_POWER
void spdif_power_enable(bool on)
{
   (void)on;
}

bool spdif_powered(void)
{
    return false;
}
#endif

#ifdef HAVE_USB_POWER
bool usb_powered_only(void)
{
    return false;
}

bool usb_charging_enable(bool on)
{
    (void)on;
    return false;
}
#endif

#if defined(HAVE_HOST_USB_AUDIO)
void usb_set_audio(int value)
{
    (void)value;
}

bool usb_audio_get_active(void)
{
    return false;
}
#endif

#ifdef HAVE_REMOTE_LCD_TICKING
void lcd_remote_emireduce(bool state)
{
    (void)state;
}
#endif

void lcd_set_contrast( int x )
{
    (void)x;
}

void cpu_sleep(bool enabled)
{
    (void)enabled;
}

#ifdef HAVE_TOUCHPAD_SENSITIVITY_SETTING
void touchpad_set_sensitivity(int level)
{
    (void)level;
}
#endif

#if defined(HAVE_TOUCHSCREEN) && !defined(HAS_BUTTON_HOLD)
void touchscreen_enable_device(bool en)
{
    (void)en;
}
#endif

#if defined(HAVE_TOUCHPAD) && !defined(HAS_BUTTON_HOLD)
void touchpad_enable_device(bool en)
{
    (void)en;
}
#endif
