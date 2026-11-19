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

/* Number, date and time formats per language (P20, 0.9.0): pure logic,
 * host-tested (tests/locale). locale.h feeds it the language's strings.
 *
 * The language system's printf can't reorder its arguments, so every
 * shell date goes through a pattern string instead (a LANG_IPODUI_DATE_*
 * or LANG_IPODUI_TIME_* string). A '$' and one letter is a field, as
 * PHP's date() letters (a convention translators may know):
 *   $j day 1-31        $d day 01-31
 *   $n month 1-12      $m month 01-12      $M month name (abbreviated)
 *   $y year 26         $Y year 2026
 *   $D weekday (abbreviated)               $l weekday (full)
 *   $G hour 0-23       $H hour 00-23       $g hour 1-12
 *   $i minute 00-59    $A AM / PM          $t the time, per the clock
 *   $$ a '$'
 * A '_' between '$' and M, D or l lower-cases the name's first letter:
 * Dutch writes "13 nov 2026" mid-line from the capitalised "Nov".
 * Anything else is copied as it stands (UTF-8 passes through). */
#ifndef IPODUI_LOCALE_CORE_H
#define IPODUI_LOCALE_CORE_H

#include <stdbool.h>
#include <stddef.h>

struct locale_when {
    int year;                   /* 2026 */
    int mon;                    /* 1-12 */
    int mday;                   /* 1-31 */
    int wday;                   /* 0 = Sunday */
    int hour, min;              /* 0-23, 0-59 */
};

struct locale_names {
    const char *(*month)(int mon);      /* 1-12, abbreviated */
    const char *(*wday)(int wday);      /* 0-6, abbreviated */
    const char *(*wday_full)(int wday); /* 0-6 */
    const char *am, *pm;
    const char *time12, *time24;        /* the patterns $t uses */
    bool h12;                           /* the 12-hour clock */
};

/* pat filled from w into buf (always terminated, cut on a character
 * boundary); returns the length written */
size_t locale_format(char *buf, size_t len, const char *pat,
                     const struct locale_when *w,
                     const struct locale_names *n);

/* The field order of a numeric date pattern, for the date rollers: the
 * first three of 'd', 'm', 'y' in pat into order[0..2]; false (order
 * month, day, year) if pat lacks one */
bool locale_date_order(const char *pat, char order[3]);

/* "88.2" / "88,2": whole, then 'digits' decimals of frac (frac < 10^digits,
 * zero-padded), joined by sep ("" or NULL: ".") */
void locale_decimal(char *buf, size_t len, unsigned long whole,
                    unsigned long frac, int digits, const char *sep);

/* A list row with an inline value (paint.h UI_VALUE_SEP): name, sep,
 * value into buf. The value is kept whole; the name takes what is left,
 * cut on a character boundary (never inside a UTF-8 sequence). */
void locale_value_row(char *buf, size_t len, const char *name, char sep,
                      const char *value);

/* The byte length of the longest prefix of s (at most n bytes) that ends
 * on a UTF-8 character boundary */
size_t locale_utf8_cut(const char *s, size_t n);

/* The byte index after the next wrap unit of s from j (spaces skipped):
 * a word, or one East Asian character with its closing marks; an opening
 * bracket keeps the whole unit it opens (paint.h ui_wrap_next()) */
int locale_wrap_next(const char *s, int j);

#endif /* IPODUI_LOCALE_CORE_H */
