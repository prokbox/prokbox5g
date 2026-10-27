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
 * iPod UI shell
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

/* Settings > Date & Time (docs/phase11-brief.md section 3.6; frozen
 * defaults D4, D5, T1).
 *  - The list: a split screen like Settings (menu.c, one of its static
 *    slots): Date, Time, Time Zone, then 24 Hour Clock and Time in Title
 *    with blue inline On/Off values that SELECT flips.
 *  - Date and Time: dark full-screen roller screens (roller.h, U2a's
 *    widget): month / day / year in the US order for English, hour /
 *    minute. SELECT moves to the next field; SELECT on the last one or
 *    MENU sets the clock (set_time(); the RTC holds local time) and
 *    returns (D5).
 *  - Time Zone: a region list, then that region's cities (value lists,
 *    language.c), from the append-only table of tzcities.h, through the
 *    resident picker shared with Clocks (tzpick.h, P12). A new city
 *    shifts the RTC by the difference of the two standard offsets and is
 *    stored by its stable id (ipodui_tz_city); no DST row (D4).
 *  - Every clock change (Date, Time, a zone's shift) re-arms the alarms
 *    (alarm_rearm(), P12), so a clock moved past one re-arms it.
 *  - 24 Hour Clock: timeformat (0 = 24 h). Time in Title: the
 *    ipodui_time_in_title setting, applied through
 *    ui_header_set_time_in_title() (default Off, owner T1).
 * State: the list uses a menu.c slot, the rollers the screen arena;
 * nothing allocates. */

/* #106: a module tenant defines its real screen names (the core sees the
 * stand-ins, modload.h); the target's module build passes this itself */
#ifndef IPODUI_IN_MODULE
#define IPODUI_IN_MODULE
#endif
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "lang.h"
#include "settings.h"
#include "ipodui/fe_be.h"   /* P19 S1: settings saves cross */
#include "timefuncs.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/header.h"
#include "ipodui/menu.h"
#include "ipodui/preview.h"
#include "ipodui/roller.h"
#include "ipodui/usbscreen.h"
#include "ipodui/panelart.h"
#include "ipodui/tzpick.h"
#include "ipodui/alarm.h"
#include "ipodui/screens/panels.h"
#include "ipodui/screens/screens.h"

/* -- time zone ---------------------------------------------------------------- */

static int tz_city(void)
{
    int c = global_settings.ipodui_tz_city;
    return (c >= 0 && c < tzp_city_count()) ? c : UI_TZ_CITY_DEFAULT;
}

/* days since 1970-01-01 of a civil date, and back (proleptic Gregorian) */
static long days_from_civil(int y, int m, int d)
{
    long era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, int *y, int *m, int *d)
{
    long era, doe, yoe, doy, mp;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

/* The RTC moved by 'minutes' (a time zone change): pure arithmetic on
 * the local time it holds, no host or libc time zone involved */
static void rtc_shift(int minutes)
{
    struct tm tm = *get_time();
    long day = days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    long mod = tm.tm_hour * 60L + tm.tm_min + minutes;
    int y, m, d;

    day += mod >= 0 ? mod / 1440 : -((1439 - mod) / 1440);
    mod = ((mod % 1440) + 1440) % 1440;
    civil_from_days(day, &y, &m, &d);
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = mod / 60;
    tm.tm_min = mod % 60;
    set_day_of_week(&tm);
    set_day_of_year(&tm);
    set_time(&tm);
}

/* The picker is resident and shared with Clocks (tzpick.h, P12 M7): a
 * city shifts the RTC by the difference of the two standard offsets */
static int tz_current(void *arg)
{
    (void)arg;
    return tz_city();
}

static int tz_done(int id, void *arg)
{
    int old = tz_city();
    int delta = tzp_city_offset(id) - tzp_city_offset(old);

    (void)arg;
    if (id != old)
    {
        if (delta)
        {
            rtc_shift(delta);
            alarm_rearm();          /* the clock moved: the next alarm */
        }
        global_settings.ipodui_tz_city = id;
        fe_settings_save();
    }
    UPDATE_TRACE("datetime tz city=%d shift=%d", id, delta);
    return 2;                       /* the city list and the region list */
}

static const struct tzpick tz_pick = {
    LANG_IPODUI_TIME_ZONE, tz_current, tz_done, NULL };

/* -- Date and Time: the roller screens ------------------------------------- */

enum { DT_DATE, DT_TIME };

#define ROLLER_Y    104             /* the fields' row (cf. the Screen Lock
                                       combination photo) */

struct dt_roller {
    struct ui_roller r;
    unsigned char which;
    bool h12;                           /* 12 hour clock: 1-12 and AM/PM */
};
_Static_assert(sizeof(struct dt_roller) <= UI_ARENA_B_SETTING,
               "dt_roller: arena bound");

static int days_in_month(int m, int y)
{
    static const unsigned char dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31,
                                          30, 31, 30, 31 };
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : dm[(m - 1) % 12];
}

static void field(struct ui_roller *r, int i, int min, int max, int v, int dg)
{
    r->f[i].min = min;
    r->f[i].max = max;
    r->f[i].value = MIN(MAX(v, min), max);
    r->f[i].digits = dg;
    r->f[i].text = false;
}

static int roller_enter(struct ui_screen *s, const void *arg)
{
    const int *which = arg;
    struct dt_roller *d = ui_arena_alloc(sizeof(*d));
    struct tm *tm = get_time();

    if (!d)
        return -1;
    s->priv = d;
    s->header_w = UI_HEADER_DARK;
    d->which = *which;
    d->r.dark = true;
    d->r.cur = 0;
    if (d->which == DT_DATE)
    {
        /* US order for English: month, day, year */
        s->title = str(LANG_IPODUI_DATE);
        d->r.n = 3;
        field(&d->r, 0, 1, 12, tm->tm_mon + 1, 2);
        field(&d->r, 1, 1, 31, tm->tm_mday, 2);
        field(&d->r, 2, 2000, 2099, tm->tm_year + 1900, 4);
    }
    else
    {
        s->title = str(LANG_IPODUI_TIME);
        d->h12 = global_settings.timeformat != 0;
        if (d->h12)
        {
            /* as on the 7G: hour 1-12, minute, AM/PM */
            d->r.n = 3;
            field(&d->r, 0, 1, 12, tm->tm_hour % 12 ? tm->tm_hour % 12 : 12,
                  2);
            field(&d->r, 1, 0, 59, tm->tm_min, 2);
            field(&d->r, 2, 0, 1, tm->tm_hour >= 12, 2);
            d->r.f[2].text = true;
            d->r.txt[0] = str(LANG_IPODUI_AM);
            d->r.txt[1] = str(LANG_IPODUI_PM);
        }
        else
        {
            d->r.n = 2;
            field(&d->r, 0, 0, 23, tm->tm_hour, 2);
            field(&d->r, 1, 0, 59, tm->tm_min, 2);
        }
    }
    return 0;
}

static void roller_draw(struct ui_screen *s, unsigned dirty)
{
    struct dt_roller *d = s->priv;

    if (dirty & UI_DIRTY_BODY)
        ui_dark_body(UI_DARK_HDR_H, LCD_HEIGHT - UI_DARK_HDR_H);
    else if (!(dirty & UI_DIRTY_BAND))
        return;
    else
    {
        int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_dark_body(y[i], h[i]);
    }
    ui_roller_draw(&d->r, ROLLER_Y, -1);
}

static void roller_apply(const struct dt_roller *d)
{
    struct tm tm = *get_time();

    if (d->which == DT_DATE)
    {
        int y = d->r.f[2].value, m = d->r.f[0].value;
        tm.tm_year = y - 1900;
        tm.tm_mon = m - 1;
        tm.tm_mday = MIN(d->r.f[1].value, days_in_month(m, y));
    }
    else
    {
        tm.tm_hour = d->h12 ? d->r.f[0].value % 12 + 12 * d->r.f[2].value
                            : d->r.f[0].value;
        tm.tm_min = d->r.f[1].value;
        tm.tm_sec = 0;
    }
    set_day_of_week(&tm);
    set_day_of_year(&tm);
    set_time(&tm);
    alarm_rearm();                  /* the clock moved: the next alarm */
    UPDATE_TRACE("datetime set %04d-%02d-%02d %02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                 tm.tm_min);
}

static enum ui_result roller_input(struct ui_screen *s,
                                   const struct ui_input *in)
{
    struct dt_roller *d = s->priv;
    int by = 0, bh = 0;

    if (in->key == UI_KEY_PLAY)
        return UI_IGNORED;
    switch (ui_roller_input(&d->r, in, &by, &bh))
    {
        case UI_ROLLER_BUSY:
            if (bh > 0)
                ui_invalidate_band(by, bh);
            return UI_HANDLED;
        case UI_ROLLER_DONE:
        case UI_ROLLER_CANCEL:          /* MENU saves too (D5) */
        default:
            roller_apply(d);
            ui_invalidate(UI_DIRTY_HEADER);
            return UI_POP;
    }
}

static const struct ui_screen_ops roller_screen = {
    .name    = "datetime-roller",
    .enter   = roller_enter,
    .leave   = NULL,                    /* arena: released by the pop */
    .draw    = roller_draw,
    .input   = roller_input,
    .timeout = NULL,
};

static const int arg_date = DT_DATE, arg_time = DT_TIME;

/* -- the list ------------------------------------------------------------------ */

/* An item's panel: our tile provider over ui_panel_draw_tile() */
struct dt_tile {
    short title, art;
    const char *(*caption)(char *buf, size_t len);
};

static const char *cap_date(char *buf, size_t len)
{
    struct tm *tm = get_time();
    snprintf(buf, len, "%s %d, %d", str(LANG_MONTH_JANUARY + tm->tm_mon),
             tm->tm_mday, tm->tm_year + 1900);
    return buf;
}

static const char *cap_time(char *buf, size_t len)
{
    (void)buf; (void)len;
    return ui_header_clock(NULL);
}

static const char *cap_zone(char *buf, size_t len)
{
    (void)buf; (void)len;
    return str(tzp_city_lang(tz_city()));
}

static const char *cap_onoff(bool on)
{
    return str(on ? LANG_ON : LANG_OFF);
}

static const char *cap_24h(char *buf, size_t len)
{
    (void)buf; (void)len;
    return cap_onoff(global_settings.timeformat == 0);
}

static const char *cap_tit(char *buf, size_t len)
{
    (void)buf; (void)len;
    return cap_onoff(global_settings.ipodui_time_in_title);
}

static int dt_tile_draw(const struct ui_panel *p, const void *arg, long phase)
{
    const struct dt_tile *t = arg;
    char buf[48];

    (void)phase;
    ui_panel_draw_tile(p, str(t->title), t->art,
                       t->caption ? t->caption(buf, sizeof(buf)) : NULL);
    return ui_panel_tile_wake(t->art);  /* the calendar turns at midnight */
}

static const struct ui_preview_ops dt_preview = {
    "dt-tile", dt_tile_draw, NAV_REST_ICON, UI_PANEL_XFADE };

static const struct dt_tile
    t_date = { LANG_IPODUI_DATE, UI_TILE_CALENDAR, cap_date },
    t_time = { LANG_IPODUI_TIME, PANELART_TIMER, cap_time },
    t_zone = { LANG_IPODUI_TIME_ZONE, UI_TILE_CALENDAR, cap_zone },
    t_24h  = { LANG_IPODUI_24_HOUR_CLOCK, PANELART_TIMER, cap_24h },
    t_tit  = { LANG_IPODUI_TIME_IN_TITLE, PANELART_TIMER, cap_tit };

static void dt_changed(void)
{
    fe_settings_save();
    ui_invalidate(UI_DIRTY_ALL);    /* rows (refresh()) and the header */
    ui_panel_invalidate();          /* the caption */
}

static void act_24h(void)
{
    global_settings.timeformat = global_settings.timeformat ? 0 : 1;
    UPDATE_TRACE("datetime 24h=%d", (int)(global_settings.timeformat == 0));
    dt_changed();
}

static void act_tit(void)
{
    global_settings.ipodui_time_in_title = !global_settings.ipodui_time_in_title;
    ui_header_set_time_in_title(global_settings.ipodui_time_in_title);
    UPDATE_TRACE("datetime time_in_title=%d",
                 (int)global_settings.ipodui_time_in_title);
    dt_changed();
}

#define DT_ROW(target, arg, act, tile) \
    { NULL, target, arg, NULL, act, &dt_preview, &tile }

#ifdef IPODUI_MODULES
/* const template, copied at enter: a module (#106) has no writable .data */
static const struct ui_menu_item dt_items_init[] = {
#else
static struct ui_menu_item dt_items[] = {
#endif
    DT_ROW(&roller_screen, &arg_date, NULL, t_date),
    DT_ROW(&roller_screen, &arg_time, NULL, t_time),
    DT_ROW(&tzpick_screen, &tz_pick, NULL, t_zone),
    DT_ROW(NULL, NULL, act_24h, t_24h),
    DT_ROW(NULL, NULL, act_tit, t_tit),
};

#ifdef IPODUI_MODULES
static struct ui_menu_item dt_items[ARRAYLEN(dt_items_init)];
#endif

static const short dt_lang[ARRAYLEN(dt_items)] = {
    LANG_IPODUI_DATE, LANG_IPODUI_TIME, LANG_IPODUI_TIME_ZONE,
    LANG_IPODUI_24_HOUR_CLOCK, LANG_IPODUI_TIME_IN_TITLE,
};

#define DT_VALUE_LEN 48
static char dt_value[2][DT_VALUE_LEN];  /* the two inline-value rows */

/* menu.c calls it on enter and before every full draw: the labels from
 * the language, the two values inline ("Name" UI_VALUE_SEP "Value") */
static void dt_refresh(void)
{
    unsigned i;

    for (i = 0; i < ARRAYLEN(dt_items); i++)
        dt_items[i].label = str(dt_lang[i]);
    snprintf(dt_value[0], DT_VALUE_LEN, "%s%c%s", str(dt_lang[3]),
             UI_VALUE_SEP, cap_onoff(global_settings.timeformat == 0));
    snprintf(dt_value[1], DT_VALUE_LEN, "%s%c%s", str(dt_lang[4]),
             UI_VALUE_SEP, cap_onoff(global_settings.ipodui_time_in_title));
    dt_items[3].label = dt_value[0];
    dt_items[4].label = dt_value[1];
}

#ifdef IPODUI_MODULES
static struct ui_menu dt_menu;          /* filled at enter (#106) */
#else
static struct ui_menu dt_menu = {
    NULL, dt_items, ARRAYLEN(dt_items), true, false, dt_refresh
};
#endif

/* ui_datetime_menu_screen: menu.c's screen over dt_menu */
static int dt_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
#ifdef IPODUI_MODULES
    memcpy(dt_items, dt_items_init, sizeof(dt_items));
    dt_menu = (struct ui_menu){ NULL, dt_items, ARRAYLEN(dt_items), true,
                                false, dt_refresh };
#endif
    dt_menu.title = str(LANG_IPODUI_DATE_TIME);
    dt_refresh();
    return ui_menu_screen.enter(s, &dt_menu);
}

static void dt_leave(struct ui_screen *s)
{
    ui_menu_screen.leave(s);
}

static void dt_draw(struct ui_screen *s, unsigned dirty)
{
    ui_menu_screen.draw(s, dirty);
}

static enum ui_result dt_input(struct ui_screen *s, const struct ui_input *in)
{
    return ui_menu_screen.input(s, in);
}

static int dt_tick(struct ui_screen *s)
{
    return ui_menu_screen.tick(s);
}

const struct ui_screen_ops ui_datetime_menu_screen = {
    .name    = "datetime",
    .enter   = dt_enter,
    .leave   = dt_leave,
    .draw    = dt_draw,
    .input   = dt_input,
    .timeout = NULL,
    .tick    = dt_tick,
};
