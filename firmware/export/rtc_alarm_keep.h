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
 * The RTC alarm keep-alive rule (P12 H6, docs/phase12-brief.md 3.3): what
 * an early power-off does with the alarm the PMU holds. Pure; the PCF50605
 * driver (rtc_pcf50605.c) and tests/alarm both use it.
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
#ifndef RTC_ALARM_KEEP_H
#define RTC_ALARM_KEEP_H

/* rtc[]: the clock registers 0x0A..0x10, alm[]: the alarm registers
 * 0x11..0x17; both BCD in the order sec, min, hour, weekday, mday, month,
 * year (from 2000) [rockbox-derived].
 *  RTC_KEEP_NONE   nothing armed: the year at 0x99 (upstream's "off"
 *                  value, also what alarm_rearm() writes), or a field
 *                  that is neither BCD nor its "ignore" value (garbage,
 *                  never armed by us): RTCWAK stays as it is (cleared at
 *                  boot, pcf50605.c);
 *  RTC_KEEP_WAKE   a valid alarm still ahead: a daily h:m:s match (the
 *                  date fields at their ignore values) or an exact date
 *                  after the clock: set RTCWAK;
 *  RTC_KEEP_DAILY  an exact date at or before the clock (it just fired
 *                  and no firmware re-armed it): write the date fields'
 *                  ignore values (the daily match of the same time) and
 *                  set RTCWAK, so a repeating alarm rings tomorrow
 *                  instead of being lost; the next boot that reaches
 *                  alarm_init() arms the exact time again. */
enum { RTC_KEEP_NONE, RTC_KEEP_WAKE, RTC_KEEP_DAILY };

/* the date fields' "ignore" values, registers 0x14..0x17 (weekday, mday,
 * month, year): rtc_pcf50605.c alarm_disable[3..6] */
#define RTC_ALARM_DAILY_TAIL { 0x07, 0x3f, 0x1f, 0xff }

static inline int rtc_alarm_bcd_ok(unsigned char v, unsigned max)
{
    return (v & 0x0f) <= 9 && (v >> 4) <= 9
           && (unsigned)((v >> 4) * 10 + (v & 0x0f)) <= max;
}

static inline int rtc_alarm_keep(const unsigned char rtc[7],
                                 const unsigned char alm[7])
{
    int i;

    if (alm[6] == 0x99)
        return RTC_KEEP_NONE;
    if (!rtc_alarm_bcd_ok(alm[0], 59) || !rtc_alarm_bcd_ok(alm[1], 59)
        || !rtc_alarm_bcd_ok(alm[2], 23))
        return RTC_KEEP_NONE;
    if (alm[4] == 0x3f && alm[5] == 0x1f && alm[6] == 0xff)
        return RTC_KEEP_WAKE;           /* daily: always ahead */
    if (!rtc_alarm_bcd_ok(alm[4], 31) || alm[4] == 0
        || !rtc_alarm_bcd_ok(alm[5], 12) || alm[5] == 0
        || !rtc_alarm_bcd_ok(alm[6], 99))
        return RTC_KEEP_NONE;
    /* BCD orders as its value: compare year, month, day, h, m, s */
    {
        static const unsigned char order[6] = { 6, 5, 4, 2, 1, 0 };
        for (i = 0; i < 6; i++)
        {
            unsigned char a = alm[order[i]], r = rtc[order[i]];
            if (a != r)
                return a > r ? RTC_KEEP_WAKE : RTC_KEEP_DAILY;
        }
    }
    return RTC_KEEP_DAILY;              /* exactly now: it is firing */
}

#endif /* RTC_ALARM_KEEP_H */
