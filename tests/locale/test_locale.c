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

/* Host test of apps/ipodui/locale_core.c: the shell's date, time and
 * number formats per language (P20, 0.9.0). The patterns, AM/PM and the
 * month and weekday names are the ones the four .lang files ship: run.sh
 * reads them into patterns.h (gen_patterns.py), so this checks what the
 * device shows. Languages: 0 en, 1 de, 2 nl, 3 ja. */
#include <stdio.h>
#include <string.h>
#include "ipodui/locale_core.h"
#include "patterns.h"

static int checks, fails;

static void eq(const char *what, const char *got, const char *want)
{
    checks++;
    if (strcmp(got, want))
    {
        fails++;
        printf("FAIL %s: got '%s', want '%s'\n", what, got, want);
    }
}

static int cur;
static const char *mon(int m) { return lang_month[cur][m - 1]; }
static const char *wd(int d) { return lang_wday[cur][d]; }
static const char *wdf(int d) { return lang_wday_full[cur][d]; }

static struct locale_names names(int lang, bool h12)
{
    struct locale_names n;

    cur = lang;
    n.month = mon;
    n.wday = wd;
    n.wday_full = wdf;
    n.am = lang_am[lang];
    n.pm = lang_pm[lang];
    n.time12 = pat_time12[lang];
    n.time24 = pat_time24[lang];
    n.h12 = h12;
    return n;
}

static void fmt(const char *what, int lang, bool h12, const char *pat,
                const struct locale_when *w, const char *want)
{
    struct locale_names n = names(lang, h12);
    char b[96];

    locale_format(b, sizeof(b), pat, w, &n);
    eq(what, b, want);
}

/* the units of s, '|' between them (spaces dropped) */
static void wrap(const char *s, const char *want)
{
    char out[96];
    int j = 0, o = 0;

    while (s[j])
    {
        int k = locale_wrap_next(s, j), i = j;

        while (s[i] == ' ')
            i++;
        if (k <= i)
            break;
        if (o)
            out[o++] = '|';
        memcpy(out + o, s + i, k - i);
        o += k - i;
        j = k;
    }
    out[o] = '\0';
    eq(s, out, want);
}

static void order(const char *what, const char *pat, const char *want)
{
    char o[8] = { 0 };

    if (!locale_date_order(pat, o))
        strcpy(o, "none");
    eq(what, o, want);
}

int main(void)
{
    /* Friday 13 November 2026, 7:05 and 19:05 */
    const struct locale_when am = { 2026, 11, 13, 5, 7, 5 };
    const struct locale_when pm = { 2026, 11, 13, 5, 19, 5 };
    struct locale_names n;
    char b[64];

    /* the Extras clock's date (LANG_IPODUI_DATE_PANEL) */
    fmt("en panel", 0, true, pat_panel[0], &am, "Nov 13 2026");
    /* German "13. Nov. 2026" once deutsch.lang gives the months their
     * abbreviation point (an ipodui: line on LANG_MONTH_*, the
     * committee's wording); the pattern needs no change for it */
    fmt("de panel", 1, false, pat_panel[1], &am, "13. Nov 2026");
    fmt("nl panel", 2, false, pat_panel[2], &am, "13 nov 2026");
    fmt("ja panel", 3, false, pat_panel[3], &am,
        "2026\xe5\xb9\xb4" "11\xe6\x9c\x88" "13\xe6\x97\xa5");

    /* the alarm panel (LANG_IPODUI_DATE_ALARM) */
    fmt("en alarm", 0, true, pat_alarm[0], &am, "Fri 13 Nov, 7:05 AM");
    fmt("de alarm", 1, false, pat_alarm[1], &pm, "Fr 13. Nov, 19:05");
    fmt("nl alarm", 2, false, pat_alarm[2], &am, "vr 13 nov, 07:05");
    /* 11月13日（金） 7:05, and 午後7:05 on the 12-hour clock */
    fmt("ja alarm", 3, false, pat_alarm[3], &am,
        "11\xe6\x9c\x88" "13\xe6\x97\xa5\xef\xbc\x88\xe9\x87\x91\xef\xbc\x89"
        " 7:05");
    fmt("ja alarm 12h", 3, true, pat_alarm[3], &pm,
        "11\xe6\x9c\x88" "13\xe6\x97\xa5\xef\xbc\x88\xe9\x87\x91\xef\xbc\x89"
        " \xe5\x8d\x88\xe5\xbe\x8c" "7:05");

    /* the short numeric date (due dates, logs) and the rollers' order */
    fmt("en short", 0, true, pat_short[0], &am, "11/13/26");
    fmt("de short", 1, false, pat_short[1], &am, "13.11.26");
    fmt("nl short", 2, false, pat_short[2], &am, "13-11-26");
    fmt("ja short", 3, false, pat_short[3], &am, "2026/11/13");
    order("en order", pat_short[0], "mdy");
    order("de order", pat_short[1], "dmy");
    order("nl order", pat_short[2], "dmy");
    order("ja order", pat_short[3], "ymd");
    order("short order", "$j only", "none");

    /* a calendar day and month title */
    fmt("en day", 0, true, pat_day[0], &am, "Friday, Nov 13, 2026");
    fmt("de day", 1, false, pat_day[1], &am, "Freitag, 13. Nov 2026");
    fmt("en month", 0, true, pat_month[0], &am, "Nov 2026");
    fmt("en caption", 0, true, pat_caption[0], &am, "Nov 13, 2026");
    fmt("nl caption", 2, false, pat_caption[2], &am, "13 nov 2026");
    fmt("ja month", 3, false, pat_month[3], &am,
        "2026\xe5\xb9\xb4" "11\xe6\x9c\x88");

    /* the time alone, both clocks */
    fmt("en 12h", 0, true, "$t", &pm, "7:05 PM");
    fmt("en 24h", 0, false, "$t", &am, "07:05");
    fmt("de 24h", 1, false, "$t", &am, "07:05");
    fmt("ja 12h", 3, true, "$t", &am, "\xe5\x8d\x88\xe5\x89\x8d" "7:05");
    fmt("ja 24h", 3, false, "$t", &pm, "19:05");

    /* the syntax itself */
    fmt("lower", 2, false, "$_M|$_D|$M|$$|$q", &am, "nov|vr|Nov|$|$q");
    fmt("nested t", 0, true, "[$t]", &am, "[7:05 AM]");
    /* cut on a character boundary: never half a UTF-8 sequence */
    n = names(3, false);
    locale_format(b, 7, pat_panel[3], &am, &n);
    eq("cut", b, "2026");               /* 6 bytes of room: not 2/3 of 年 */
    eq("utf8 cut", locale_utf8_cut("ab\xe5\xb9\xb4", 4) == 2 ? "2" : "?",
       "2");

    /* decimals */
    locale_decimal(b, sizeof(b), 88, 2, 1, lang_dec[0]);
    eq("dec en", b, "88.2");
    locale_decimal(b, sizeof(b), 28, 1, 1, lang_dec[1]);
    eq("dec de", b, "28,1");
    locale_decimal(b, sizeof(b), 44, 1, 2, lang_dec[2]);
    eq("dec nl pad", b, "44,01");
    locale_decimal(b, sizeof(b), 88, 2, 1, lang_dec[3]);
    eq("dec ja", b, "88.2");
    locale_decimal(b, sizeof(b), 128, 0, 0, ",");
    eq("dec whole", b, "128");

    /* the clock default per language */
    eq("clock en", lang_clock[0], "12");
    eq("clock de", lang_clock[1], "24");
    eq("clock nl", lang_clock[2], "24");
    eq("clock ja", lang_clock[3], "24");

    /* value rows: the value whole, the name cut on a character */
    locale_value_row(b, 12, "K\xc3\xbcrzeste Runde", '~', "1:23.4");
    eq("row cut", b, "K\xc3\xbcr~1:23.4");
    locale_value_row(b, 10, "K\xc3\xbcrzeste Runde", '~', "1:23.4");
    eq("row cut utf8", b, "K~1:23.4");      /* not half of the u-umlaut */
    locale_value_row(b, sizeof(b), "Total", '~', "0:05.3");
    eq("row whole", b, "Total~0:05.3");

    /* wrap units (ui_wrap_next): an opening bracket keeps the whole word
     * it opens, a no-break space never breaks, kana break per character
     * with their closing marks */
    wrap("（Apple ではなく", "（Apple|で|は|な|く");
    wrap("「ProkBox」へ", "「ProkBox」|へ");
    wrap("88,2\xc2\xa0kHz an", "88,2\xc2\xa0kHz|an");
    wrap("ab cd", "ab|cd");
    wrap("です。次", "で|す。|次");
    wrap("（「あ", "（「あ");

    if (fails)
    {
        printf("FAIL: locale %d of %d checks\n", fails, checks);
        return 1;
    }
    printf("PASS: locale %d checks\n", checks);
    return 0;
}
