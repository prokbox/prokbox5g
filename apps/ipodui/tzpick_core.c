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

/* The time-zone city table and its time arithmetic (tzpick.h): pure, no
 * Rockbox calls, so tests/tzpick checks it on the host (the LANG ids come
 * from the build's lang.h, or the test's generated stand-in).
 *
 * Times are seconds since 2000-01-01 00:00 (the RTC's epoch in
 * alarm_core.h and P18's sleep): day 0 was a Saturday. */

#include <stdint.h>
#include <time.h>
#include "lang.h"
#include "ipodui/tzpick.h"
#include "ipodui/tzcities.h"

#define DAY_S   86400L
#define HOUR_S  3600L

/* -- the table ------------------------------------------------------------ */

static int city_ok(int city)
{
    return city >= 0 && city < TZ_CITIES ? city : UI_TZ_CITY_DEFAULT;
}

int tzp_city_count(void)          { return TZ_CITIES; }
int tzp_city_lang(int city)       { return tz_cities[city_ok(city)].lang; }
int tzp_city_offset(int city)     { return tz_cities[city_ok(city)].offset; }
int tzp_city_region(int city)     { return tz_cities[city_ok(city)].region; }
int tzp_city_dst(int city)        { return tz_cities[city_ok(city)].dst; }
int tzp_region_count(void)        { return TZ_REGIONS; }

int tzp_region_lang(int region)
{
    return tz_region_lang[region >= 0 && region < TZ_REGIONS ? region : 0];
}

/* the picker's view of the display order (tzpick.c) */
int tzp_region_first(int region)
{
    return tz_region_first[region];
}

int tzp_order(int pos)
{
    return tz_order[pos];
}

/* -- calendar arithmetic -------------------------------------------------- */

/* days since 2000-01-01 of a civil date (proleptic Gregorian, y >= 2000;
 * m may be 13 for January of the next year). The era form counts from
 * 0000-03-01, so nothing below is negative. */
static long days_from_civil(int y, int m, int d)
{
    long era, yoe, doy;

    if (m > 12)
    {
        m -= 12;
        y++;
    }
    y -= m <= 2;
    era = y / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy
           - 730425;                    /* 2000-01-01 */
}

static int year_of(long day)
{
    int y = 2000 + (int)(day / 366);

    while (days_from_civil(y + 1, 1, 1) <= day)
        y++;
    return y;
}

static int wday(long day)               /* 0 = Sunday */
{
    return (int)((day + 6) % 7);
}

/* the n-th (1..) Sunday of month m, or the last one (n = 0) */
static long sunday(int y, int m, int n)
{
    long d;

    if (n == 0)
    {
        d = days_from_civil(y, m + 1, 1) - 1;
        return d - wday(d);
    }
    d = days_from_civil(y, m, 1);
    return d + (7 - wday(d)) % 7 + 7L * (n - 1);
}

/* -- daylight saving ------------------------------------------------------ */

int tz_dst_minutes(int rule, uint32_t utc_s, int std_offset)
{
    int64_t t, a, b;                    /* 64 bit: past 2068 too */
    int y;

    /* local standard time for the local rules; UTC for the EU's */
    t = rule == TZ_DST_EU ? (int64_t)utc_s
                          : (int64_t)utc_s + std_offset * 60L;
    if (t < 0)
        return 0;                       /* before 2000: no rule */
    y = year_of((long)(t / DAY_S));
    switch (rule)
    {
        case TZ_DST_US:
            /* 02:00 standard -> 02:00 daylight, which is 01:00 standard */
            a = (int64_t)sunday(y, 3, 2) * DAY_S + 2 * HOUR_S;
            b = (int64_t)sunday(y, 11, 1) * DAY_S + 1 * HOUR_S;
            return t >= a && t < b ? TZ_DST_MINUTES : 0;
        case TZ_DST_EU:
            a = (int64_t)sunday(y, 3, 0) * DAY_S + 1 * HOUR_S;
            b = (int64_t)sunday(y, 10, 0) * DAY_S + 1 * HOUR_S;
            return t >= a && t < b ? TZ_DST_MINUTES : 0;
        case TZ_DST_SOUTH:
            /* ends 03:00 daylight (02:00 standard) on the 1st Sunday of
             * April, starts 02:00 standard on the 1st Sunday of October */
            a = (int64_t)sunday(y, 4, 1) * DAY_S + 2 * HOUR_S;
            b = (int64_t)sunday(y, 10, 1) * DAY_S + 2 * HOUR_S;
            return t < a || t >= b ? TZ_DST_MINUTES : 0;
        default:
            return 0;
    }
}

uint32_t tz_city_time(int city, int home_city, uint32_t home_rtc)
{
    const int hs = tzp_city_offset(home_city), cs = tzp_city_offset(city);
    const uint32_t guess = home_rtc - (uint32_t)(hs * 60L);
    const uint32_t utc = guess - (uint32_t)(60L *
        tz_dst_minutes(tzp_city_dst(home_city), guess, hs));

    return utc + (uint32_t)(60L * (cs + tz_dst_minutes(tzp_city_dst(city),
                                                       utc, cs)));
}

/* -- struct tm <-> seconds since 2000 ---------------------------------- */

uint32_t tz_rtc_make(const struct tm *tm)
{
    long d = days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);

    return (uint32_t)d * 86400u + (uint32_t)(tm->tm_hour * HOUR_S
                                             + tm->tm_min * 60L + tm->tm_sec);
}

void tz_rtc_split(uint32_t s, struct tm *tm)
{
    long day = (long)(s / DAY_S), rem = (long)(s % DAY_S);
    int y = year_of(day), m = 1;

    while (m < 12 && days_from_civil(y, m + 1, 1) <= day)
        m++;
    tm->tm_year = y - 1900;
    tm->tm_mon = m - 1;
    tm->tm_mday = (int)(day - days_from_civil(y, m, 1)) + 1;
    tm->tm_yday = (int)(day - days_from_civil(y, 1, 1));
    tm->tm_wday = wday(day);
    tm->tm_hour = (int)(rem / HOUR_S);
    tm->tm_min = (int)(rem / 60 % 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_isdst = 0;
}
