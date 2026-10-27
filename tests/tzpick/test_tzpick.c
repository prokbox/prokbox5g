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

/* Host test of the time-zone table and its arithmetic (P12 W1-C, Q27,
 * docs/phase12-brief.md 3.6 and tzpick.h): apps/ipodui/tzpick_core.c.
 * The DST switches of 2026 in both hemispheres, other cities' times from
 * a home city in and out of DST, the date split, the table's DST bytes. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "ipodui/tzpick.h"

static int fails;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)
#define CHECK_EQ(a, b) do { long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { fprintf(stderr, "%s:%d: %s = %ld, want %ld\n", \
    __FILE__, __LINE__, #a, _a, _b); fails++; } } while (0)

enum { NEW_YORK = 0, LOS_ANGELES = 11, PHOENIX = 15, SANTIAGO = 30,
       LONDON = 42, TOKYO = 79, SYDNEY = 85, AUCKLAND = 87 };

/* seconds since 2000 of a calendar time (the RTC's or UTC) */
static uint32_t at(int y, int mo, int d, int h, int mi, int s)
{
    struct tm tm;

    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    return tz_rtc_make(&tm);
}

static void test_calendar(void)
{
    struct tm tm;
    uint32_t s;

    CHECK_EQ(at(2000, 1, 1, 0, 0, 0), 0);
    CHECK_EQ(at(2000, 1, 2, 0, 0, 1), 86401);
    CHECK_EQ(at(2000, 3, 1, 0, 0, 0), 60L * 86400);      /* 2000 is leap */
    CHECK_EQ(at(2001, 1, 1, 0, 0, 0), 366L * 86400);
    tz_rtc_split(0, &tm);
    CHECK_EQ(tm.tm_wday, 6);                            /* a Saturday */
    CHECK_EQ(tm.tm_yday, 0);
    /* the panels' pinned date: Tuesday 6 October 2026 */
    tz_rtc_split(at(2026, 10, 6, 15, 8, 0), &tm);
    CHECK_EQ(tm.tm_wday, 2);
    CHECK_EQ(tm.tm_yday, 278);
    CHECK_EQ(tm.tm_hour, 15);
    CHECK_EQ(tm.tm_min, 8);
    /* round trips over leap days, century rules and the far end */
    {
        static const int d[][3] = { { 2024, 2, 29 }, { 2100, 2, 28 },
                                    { 2100, 3, 1 }, { 2099, 12, 31 },
                                    { 2135, 12, 31 }, { 2068, 6, 1 } };
        unsigned i;

        for (i = 0; i < sizeof(d) / sizeof(d[0]); i++)
        {
            s = at(d[i][0], d[i][1], d[i][2], 23, 59, 58);
            tz_rtc_split(s, &tm);
            CHECK_EQ(tm.tm_year + 1900, d[i][0]);
            CHECK_EQ(tm.tm_mon + 1, d[i][1]);
            CHECK_EQ(tm.tm_mday, d[i][2]);
            CHECK_EQ(tm.tm_sec, 58);
            CHECK_EQ(tz_rtc_make(&tm), s);
        }
    }
    tz_rtc_split(at(2100, 3, 1, 0, 0, 0) - 1, &tm);     /* 2100: no 29 Feb */
    CHECK_EQ(tm.tm_mday, 28);
}

/* the minutes of DST one second before and at a switch (UTC instants) */
static void edge(int rule, int std, uint32_t utc, int before, int after)
{
    CHECK_EQ(tz_dst_minutes(rule, utc - 1, std), before);
    CHECK_EQ(tz_dst_minutes(rule, utc, std), after);
}

static void test_dst(void)
{
    /* US 2026: Sunday 8 March 02:00 EST = 07:00 UTC; Sunday 1 November
     * 02:00 EDT = 06:00 UTC (New York); Los Angeles three hours later */
    edge(TZ_DST_US, -300, at(2026, 3, 8, 7, 0, 0), 0, 60);
    edge(TZ_DST_US, -300, at(2026, 11, 1, 6, 0, 0), 60, 0);
    edge(TZ_DST_US, -480, at(2026, 3, 8, 10, 0, 0), 0, 60);
    edge(TZ_DST_US, -480, at(2026, 11, 1, 9, 0, 0), 60, 0);
    /* a year whose 1 March is not a Sunday: 2027, 14 March / 7 Nov */
    edge(TZ_DST_US, -300, at(2027, 3, 14, 7, 0, 0), 0, 60);
    edge(TZ_DST_US, -300, at(2027, 11, 7, 6, 0, 0), 60, 0);
    /* EU 2026: 29 March and 25 October, 01:00 UTC for every offset */
    edge(TZ_DST_EU, 0, at(2026, 3, 29, 1, 0, 0), 0, 60);
    edge(TZ_DST_EU, 60, at(2026, 3, 29, 1, 0, 0), 0, 60);
    edge(TZ_DST_EU, 120, at(2026, 10, 25, 1, 0, 0), 60, 0);
    edge(TZ_DST_EU, 0, at(2026, 10, 25, 1, 0, 0), 60, 0);
    /* southern hemisphere 2026 (Sydney, +10:00): DST ends Sunday 5 April
     * 03:00 AEDT = 16:00 UTC on the 4th; starts Sunday 4 October 02:00
     * AEST = 16:00 UTC on the 3rd. In DST over the new year. */
    edge(TZ_DST_SOUTH, 600, at(2026, 4, 4, 16, 0, 0), 60, 0);
    edge(TZ_DST_SOUTH, 600, at(2026, 10, 3, 16, 0, 0), 0, 60);
    CHECK_EQ(tz_dst_minutes(TZ_DST_SOUTH, at(2026, 12, 31, 23, 0, 0), 600),
             60);
    CHECK_EQ(tz_dst_minutes(TZ_DST_SOUTH, at(2027, 1, 1, 1, 0, 0), 600), 60);
    CHECK_EQ(tz_dst_minutes(TZ_DST_SOUTH, at(2026, 7, 1, 0, 0, 0), 600), 0);
    /* Adelaide (+9:30) switches half an hour after Sydney in UTC */
    edge(TZ_DST_SOUTH, 570, at(2026, 10, 3, 16, 30, 0), 0, 60);
    /* no rule, and nonsense rules, are standard time */
    CHECK_EQ(tz_dst_minutes(TZ_DST_NONE, at(2026, 7, 1, 0, 0, 0), 540), 0);
    CHECK_EQ(tz_dst_minutes(TZ_DST__N, at(2026, 7, 1, 0, 0, 0), 0), 0);
    /* past 2068 (a uint32 RTC beyond a 32-bit long) */
    edge(TZ_DST_EU, 0, at(2090, 3, 26, 1, 0, 0), 0, 60);
}

static void hm(uint32_t s, int y, int mo, int d, int h, int mi, int line)
{
    struct tm tm;

    tz_rtc_split(s, &tm);
    if (tm.tm_year + 1900 != y || tm.tm_mon + 1 != mo || tm.tm_mday != d
        || tm.tm_hour != h || tm.tm_min != mi)
    {
        fprintf(stderr, "%s:%d: got %04d-%02d-%02d %02d:%02d, want "
                "%04d-%02d-%02d %02d:%02d\n", __FILE__, line,
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, y, mo, d, h, mi);
        fails++;
    }
}
#define HM(s, y, mo, d, h, mi) hm(s, y, mo, d, h, mi, __LINE__)

static void test_cities(void)
{
    uint32_t r;

    /* home New York in summer: the RTC holds EDT (UTC-4) */
    r = at(2026, 7, 1, 12, 0, 0);
    HM(tz_city_time(NEW_YORK, NEW_YORK, r), 2026, 7, 1, 12, 0);
    HM(tz_city_time(LONDON, NEW_YORK, r), 2026, 7, 1, 17, 0);     /* BST */
    HM(tz_city_time(TOKYO, NEW_YORK, r), 2026, 7, 2, 1, 0);
    HM(tz_city_time(SYDNEY, NEW_YORK, r), 2026, 7, 2, 2, 0);      /* AEST */
    HM(tz_city_time(PHOENIX, NEW_YORK, r), 2026, 7, 1, 9, 0);
    HM(tz_city_time(LOS_ANGELES, NEW_YORK, r), 2026, 7, 1, 9, 0);
    /* home New York in winter: EST */
    r = at(2026, 1, 15, 12, 0, 0);
    HM(tz_city_time(LONDON, NEW_YORK, r), 2026, 1, 15, 17, 0);
    HM(tz_city_time(SYDNEY, NEW_YORK, r), 2026, 1, 16, 4, 0);     /* AEDT */
    HM(tz_city_time(PHOENIX, NEW_YORK, r), 2026, 1, 15, 10, 0);
    /* home London in BST */
    r = at(2026, 7, 1, 12, 0, 0);
    HM(tz_city_time(NEW_YORK, LONDON, r), 2026, 7, 1, 7, 0);
    HM(tz_city_time(TOKYO, LONDON, r), 2026, 7, 1, 20, 0);
    /* home Sydney in DST (southern summer): AEDT, UTC+11 */
    r = at(2026, 1, 15, 12, 0, 0);
    HM(tz_city_time(LONDON, SYDNEY, r), 2026, 1, 15, 1, 0);
    HM(tz_city_time(NEW_YORK, SYDNEY, r), 2026, 1, 14, 20, 0);
    HM(tz_city_time(AUCKLAND, SYDNEY, r), 2026, 1, 15, 14, 0);    /* NZDT */
    /* the other city crosses its switch while home does not: London at
     * 08:00 GMT on 8 March 2026 is past New York's switch (07:00 UTC),
     * so New York is on EDT (04:00); a minute before it, on EST */
    r = at(2026, 3, 8, 8, 0, 0);
    HM(tz_city_time(NEW_YORK, LONDON, r), 2026, 3, 8, 4, 0);
    r = at(2026, 3, 8, 6, 59, 0);
    HM(tz_city_time(NEW_YORK, LONDON, r), 2026, 3, 8, 1, 59);
    /* the EU and US weeks apart: 22 March 2026, NY on EDT, London GMT */
    r = at(2026, 3, 22, 12, 0, 0);
    HM(tz_city_time(LONDON, NEW_YORK, r), 2026, 3, 22, 16, 0);
    /* both hemispheres in one view: Sydney -> Santiago in July */
    r = at(2026, 7, 1, 12, 0, 0);
    HM(tz_city_time(SANTIAGO, SYDNEY, r), 2026, 6, 30, 22, 0);
    /* an id out of range reads as the default city */
    HM(tz_city_time(9999, NEW_YORK, r), 2026, 7, 1, 12, 0);
}

static void test_table(void)
{
    int r, n = 0;

    CHECK_EQ(tzp_city_count(), 97);
    CHECK_EQ(tzp_region_count(), 8);
    CHECK_EQ(tzp_city_dst(NEW_YORK), TZ_DST_US);
    CHECK_EQ(tzp_city_dst(PHOENIX), TZ_DST_NONE);
    CHECK_EQ(tzp_city_dst(LONDON), TZ_DST_EU);
    CHECK_EQ(tzp_city_dst(TOKYO), TZ_DST_NONE);
    CHECK_EQ(tzp_city_dst(SYDNEY), TZ_DST_SOUTH);
    CHECK_EQ(tzp_city_offset(SYDNEY), 600);
    CHECK_EQ(tzp_city_offset(-1), tzp_city_offset(0));
    CHECK_EQ(tzp_region_first(0), 0);
    CHECK_EQ(tzp_region_first(tzp_region_count()), tzp_city_count());
    for (r = 0; r < tzp_region_count(); r++)
    {
        int k;
        for (k = tzp_region_first(r); k < tzp_region_first(r + 1); k++)
        {
            CHECK_EQ(tzp_city_region(tzp_order(k)), r);
            CHECK(tzp_city_dst(tzp_order(k)) < TZ_DST__N);
            n++;
        }
    }
    CHECK_EQ(n, tzp_city_count());
}

int main(void)
{
    test_calendar();
    test_dst();
    test_cities();
    test_table();
    if (fails)
    {
        fprintf(stderr, "test_tzpick: %d check(s) failed\n", fails);
        return 1;
    }
    return 0;
}
