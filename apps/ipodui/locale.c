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

#include "config.h"
#include <string.h>
#include "lang.h"
#include "settings.h"
#include "ipodui/locale.h"

static const char *month(int mon)
{
    return str(LANG_MONTH_JANUARY + (mon - 1) % 12);
}

static const char *wday(int d)
{
    return str(LANG_WEEKDAY_SUNDAY + d % 7);
}

static const char *wday_full(int d)
{
    return str(LANG_IPODUI_SUNDAY + d % 7);
}

void ui_fmt_decimal(char *buf, size_t len, unsigned long whole,
                    unsigned long frac, int digits)
{
    locale_decimal(buf, len, whole, frac, digits,
                   str(LANG_IPODUI_DECIMAL_SEP));
}

bool ui_clock_12h(void)
{
    /* timeformat 0 is the 24-hour clock; the shell's default is 1, so a
     * 0 from before P20 was the owner's choice */
    if (global_settings.ipodui_clock_set || global_settings.timeformat == 0)
        return global_settings.timeformat != 0;
    return strcmp(str(LANG_IPODUI_CLOCK_DEFAULT), "24") != 0;
}

void ui_clock_set_12h(bool h12)
{
    global_settings.timeformat = h12 ? 1 : 0;
    global_settings.ipodui_clock_set = true;
}

static void names(struct locale_names *n)
{
    n->month = month;
    n->wday = wday;
    n->wday_full = wday_full;
    n->am = str(LANG_IPODUI_AM);
    n->pm = str(LANG_IPODUI_PM);
    n->time12 = str(LANG_IPODUI_TIME_12H);
    n->time24 = str(LANG_IPODUI_TIME_24H);
    n->h12 = ui_clock_12h();
}

/* 0 = Sunday (Sakamoto) */
static int day_of_week(int y, int m, int d)
{
    static const unsigned char t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };

    if (m < 1 || m > 12)
        return 0;
    y -= m < 3;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

void ui_fmt_date(char *buf, size_t len, int pattern, int year, int mon,
                 int mday, int wd, int hour, int min)
{
    struct locale_names n;
    struct locale_when w;

    names(&n);
    w.year = year;
    w.mon = mon;
    w.mday = mday;
    w.wday = wd >= 0 ? wd : day_of_week(year, mon, mday);
    w.hour = hour;
    w.min = min;
    locale_format(buf, len, str(pattern), &w, &n);
}

void ui_fmt_date_tm(char *buf, size_t len, int pattern, const struct tm *tm)
{
    ui_fmt_date(buf, len, pattern, tm->tm_year + 1900, tm->tm_mon + 1,
                tm->tm_mday, tm->tm_wday, tm->tm_hour, tm->tm_min);
}

void ui_fmt_time(char *buf, size_t len, int hour, int min)
{
    ui_fmt_date(buf, len, ui_clock_12h() ? LANG_IPODUI_TIME_12H
                                         : LANG_IPODUI_TIME_24H,
                2000, 1, 1, 6, hour, min);
}

void ui_date_order(char order[3])
{
    locale_date_order(str(LANG_IPODUI_DATE_SHORT), order);
}

void ui_date_slots(int *d, int *m, int *y)
{
    char o[3];
    int i;

    ui_date_order(o);
    for (i = 0; i < 3; i++)
    {
        if (o[i] == 'd')
            *d = i;
        else if (o[i] == 'm')
            *m = i;
        else
            *y = i;
    }
}

void ui_time_slots(int *h, int *mi, int *ap)
{
    const char *p = str(LANG_IPODUI_TIME_12H);
    const char *a = strstr(p, "$A"), *hr = strstr(p, "$g");
    bool first = a && hr && a < hr;

    *ap = first ? 0 : 2;
    *h = first ? 1 : 0;
    *mi = first ? 2 : 1;
}
