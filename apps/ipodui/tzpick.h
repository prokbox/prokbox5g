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
 * iPod UI shell: the resident time-zone city table and picker, shared by
 * Date & Time and Clocks (P12 contract, docs/phase12-brief.md 3.6, M7, Q27);
 * owner W1-C
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
#ifndef IPODUI_TZPICK_H
#define IPODUI_TZPICK_H

/* Modules cannot link to each other, so the region -> city picker and the
 * city table (tzcities.h, generated, append-only ids: tzcity.h) move out
 * of screens/datetime.c (the Settings module) into one resident object,
 * tzpick.c, used by Date & Time (MOD_SETTINGS) and Clocks (MOD_CLOCKS).
 * tzcities.h is then included by tzpick.c only.
 *
 * Q27 (owner, 2026-10-23): a DST rule per city, one byte, so other
 * cities' times include their daylight saving. The home clock follows
 * the RTC as today (Date & Time has no DST row, P11 D4): the RTC holds
 * whatever local time the owner set. */

#include <stdint.h>
#include "ipodui/tzcity.h"

enum tz_dst {
    TZ_DST_NONE = 0,
    TZ_DST_US,          /* 2nd Sunday of March 02:00 -> 1st Sunday of
                           November 02:00, local */
    TZ_DST_EU,          /* last Sunday of March -> last Sunday of October,
                           01:00 UTC */
    TZ_DST_SOUTH,       /* 1st Sunday of October 02:00 -> 1st Sunday of
                           April 03:00 daylight (02:00 standard), local
                           (south-east Australia's rule) */
    TZ_DST__N
};
#define TZ_DST_MINUTES 60

/* -- the table (tzpick.c; ids are tzcities.h rows) ---------------------- */
int tzp_city_count(void);               /* TZ_CITIES */
int tzp_city_lang(int city);            /* LANG_IPODUI_TZ_CITY_* */
int tzp_city_offset(int city);          /* standard offset, minutes */
int tzp_city_region(int city);
int tzp_city_dst(int city);             /* enum tz_dst */
int tzp_region_count(void);             /* TZ_REGIONS */
int tzp_region_lang(int region);        /* LANG_IPODUI_TZ_REGION_* */
/* W1-C addition: the display order, for the picker (tzpick.c) and any
 * other list of cities; region r lists tzp_order(tzp_region_first(r) ..
 * tzp_region_first(r + 1) - 1). The tables stay in one object. */
int tzp_region_first(int region);       /* 0..TZ_REGIONS */
int tzp_order(int pos);                 /* a city id */

/* -- the picker --------------------------------------------------------- */
/* Pushes the region list (ui_vlist_screen), then that region's cities in
 * display order. The chosen city goes to done(city, arg), whose return
 * value is the vlist set() result (how many lists to pop: 2 = both).
 * The struct must outlive the picker (a static const in the caller). */
struct tzpick {
    int title_lang;                     /* LANG_IPODUI_TIME_ZONE, or
                                           LANG_IPODUI_X_CLOCKS */
    int (*current)(void *arg);          /* city highlighted at entry, or
                                           -1 (the first of the region) */
    int (*done)(int city, void *arg);
    void *arg;
};
void tzpick_push(const struct tzpick *p);
/* W1-C addition: the region list as a screen, arg the struct tzpick, for
 * a menu row's target (a row with a target keeps its chevron, menu.c);
 * tzpick_push(p) is ui_push(&tzpick_screen, p). Its name is "vlist", as
 * the value list it forwards to. */
struct ui_screen_ops;
extern const struct ui_screen_ops tzpick_screen;

/* -- pure time arithmetic (host-testable; tzpick.c or a tzpick_core.c) -- */
/* Minutes of DST in effect under 'rule' (enum tz_dst) at the instant
 * utc_s (UTC, s since 2000) for a city of standard offset std_offset
 * (minutes): 0 or TZ_DST_MINUTES. Local rules compare utc_s + std_offset
 * with their local switch times. */
int      tz_dst_minutes(int rule, uint32_t utc_s, int std_offset);
/* Another city's local time from the home RTC time:
 *   utc   = home_rtc - home_std - home_dst(home_rtc - home_std)
 *   other = utc + city_std + city_dst(utc)
 * The brief's "local + (their offset - home offset) + DST", with the
 * home city's own DST taken out: the RTC holds DST-adjusted time when the
 * home city observes it. home_dst is evaluated at the standard-time
 * guess, so it can be an hour off only inside the home switch hour. */
uint32_t tz_city_time(int city, int home_city, uint32_t home_rtc);
/* W1-C addition: a struct tm (the RTC's local time; tm_year from 1900,
 * 2000..2135) to seconds since 2000 and back (split also fills tm_wday
 * and tm_yday; tm_isdst 0). Pure: no host or libc time zone. */
struct tm;
uint32_t tz_rtc_make(const struct tm *tm);
void     tz_rtc_split(uint32_t s, struct tm *tm);

#endif /* IPODUI_TZPICK_H */
