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

/* The shell's numbers, dates and times in the language's own form (P20,
 * 0.9.0): the decimal mark (LANG_IPODUI_DECIMAL_SEP), the date and time
 * patterns (LANG_IPODUI_DATE_*, LANG_IPODUI_TIME_*; syntax in
 * locale_core.h) and the clock the language defaults to
 * (LANG_IPODUI_CLOCK_DEFAULT). Every shell date and time goes through
 * here: no screen formats one itself. */
#ifndef IPODUI_LOCALE_H
#define IPODUI_LOCALE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "ipodui/locale_core.h"

/* "88.2" / "88,2": whole, then 'digits' decimals of frac */
void ui_fmt_decimal(char *buf, size_t len, unsigned long whole,
                    unsigned long frac, int digits);

/* The 12-hour clock: the owner's 24 Hour Clock setting once set, the
 * language's default until then (an older 24-hour setting counts as set) */
bool ui_clock_12h(void);
/* Date & Time > 24 Hour Clock: the owner's choice, kept */
void ui_clock_set_12h(bool h12);

/* "7:05 AM", "午前7:05", "07:05": the time on the clock in use */
void ui_fmt_time(char *buf, size_t len, int hour, int min);

/* A date by pattern string id (LANG_IPODUI_DATE_*). wday < 0: worked out
 * from the date. */
void ui_fmt_date(char *buf, size_t len, int pattern, int year, int mon,
                 int mday, int wday, int hour, int min);
void ui_fmt_date_tm(char *buf, size_t len, int pattern, const struct tm *tm);

/* The date rollers' field order, from LANG_IPODUI_DATE_SHORT: 'd', 'm',
 * 'y' in order[0..2] */
void ui_date_order(char order[3]);
/* The roller slot (0-2) of the day, month and year fields */
void ui_date_slots(int *d, int *m, int *y);
/* The 12-hour time rollers' slots of hour, minute and AM/PM: the marker
 * first where LANG_IPODUI_TIME_12H puts it first (Japanese 午前 7:05) */
void ui_time_slots(int *h, int *mi, int *ap);

#endif /* IPODUI_LOCALE_H */
