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
 * iPod UI shell: Extras > Alarms (P12, docs/phase12-brief.md 0.1, 3.3, spec
 * extras.md). A cold Class A module: the Alarms list (Create Alarm, Sleep
 * Timer, the alarms, two-line rows, a missed alarm says so inline), the
 * Create Alarm editor and its pickers, and the Sleep Timer values.
 * Owner W1-A2; the engine behind alarm.h is W1-A1's.
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
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "lang.h"
#include "settings.h"
#include "timefuncs.h"
#include "powermgmt.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/list.h"
#include "ipodui/menu.h"
#include "ipodui/modload.h"
#include "ipodui/header.h"
#include "ipodui/roller.h"
#include "ipodui/usbscreen.h"
#include "ipodui/screen_stack.h"
#include "ipodui/alarm.h"
#include "ipodui/panelart.h"
#include "ipodui/preview.h"
#include "ipodui/extras.h"           /* ui_preview_x_alarms */
#include "ipodui/screens/screens.h"

/* Behaviour. The list: Create Alarm (while there is room), Sleep Timer,
 * then the alarms. SELECT opens the editor or the Sleep Timer values.
 * The editor edits a copy; leaving it (MENU, or hold MENU) is the commit
 * point: one alarm_set(), only if something changed. Showing the list
 * calls alarm_seen() once (Q17) after copying the missed marks, so the
 * rows keep saying "Missed at ..." for this visit. Nothing else touches
 * storage; the screens never boost. */

/* -- the Create Alarm rows: change the order here (C2 may reorder) --------- */

enum e_row { E_ALARM, E_DATE, E_TIME, E_REPEAT, E_DAYS, E_ALERTS, E_LABEL,
             E_SNOOZE, E_DELETE, E_N };
static const uint8_t e_order[E_N] = {
    E_ALARM, E_DATE, E_TIME, E_REPEAT, E_DAYS, E_ALERTS, E_LABEL, E_SNOOZE,
    E_DELETE,
};

static const short repeat_lang[AR__N] = {
    LANG_IPODUI_X_REPEAT_ONCE, LANG_IPODUI_X_REPEAT_EVERY_DAY,
    LANG_IPODUI_X_REPEAT_WEEKDAYS, LANG_IPODUI_X_REPEAT_WEEKENDS,
    LANG_IPODUI_X_REPEAT_CUSTOM,
};
static const short label_lang[] = {
    LANG_IPODUI_X_LABEL_NONE, LANG_IPODUI_X_LABEL_WAKE_UP,
    LANG_IPODUI_X_LABEL_WORK, LANG_IPODUI_X_LABEL_SCHOOL,
    LANG_IPODUI_X_LABEL_CLASS, LANG_IPODUI_X_LABEL_GYM,
    LANG_IPODUI_X_LABEL_MEDICINE, LANG_IPODUI_X_LABEL_MEETING,
    LANG_IPODUI_X_LABEL_NAP, LANG_IPODUI_X_LABEL_APPOINTMENT,
};
#define LABELS ((int)ARRAYLEN(label_lang))
static const short day_lang[7] = {
    LANG_WEEKDAY_SUNDAY, LANG_WEEKDAY_MONDAY, LANG_WEEKDAY_TUESDAY,
    LANG_WEEKDAY_WEDNESDAY, LANG_WEEKDAY_THURSDAY, LANG_WEEKDAY_FRIDAY,
    LANG_WEEKDAY_SATURDAY,
};
static const unsigned char sleep_min[] = { 0, 15, 30, 60, 90, 120 };
#define SLEEPS ((int)ARRAYLEN(sleep_min))

#define L_ROWS_MAX  (2 + ALARM_MAX)
#define ROLLER_Y    104                 /* the fields' row (datetime.c) */

/* one working block per visit */
struct al_wk {
    struct ui_list list;                    /* the Alarms list */
    struct ui_menu emenu, dmenu;            /* the editor, the Custom days */
    struct ui_menu_item eitem[E_N];
    char elbl[E_N][36];
    struct ui_menu_item ditem[7];
    char dlbl[7][24];
    struct alarm ed;                        /* the copy being edited */
    int ed_idx;                             /* -1: a new alarm */
    bool dirty, deleted, leave_req;
    uint8_t missed;                         /* mask: rows that said so */
    uint32_t missed_at[ALARM_MAX];
    struct ui_roller r;                     /* the Date and Time pickers */
    uint8_t rwhich;
};
static struct al_wk *wk;                    /* per visit (.bss) */

#define WORK_BYTES  sizeof(struct al_wk)
#ifndef SIMULATOR                       /* 32-bit pointers: the target's size */
_Static_assert(sizeof(struct al_wk) <= 3072, "alarms: the module's work block");
#endif

/* -- formatting ----------------------------------------------------------- */

static void fmt_time(char *buf, size_t len, int h, int m)
{
    if (global_settings.timeformat)             /* 12 hour clock */
        snprintf(buf, len, "%d:%02d %s", h % 12 ? h % 12 : 12, m,
                 str(h < 12 ? LANG_IPODUI_AM : LANG_IPODUI_PM));
    else
        snprintf(buf, len, "%02d:%02d", h, m);
}

static void fmt_date(char *buf, size_t len, const struct alarm *a)
{
    snprintf(buf, len, "%d/%d/%02d", a->mon, a->mday, a->year % 100);
}

static uint32_t now_rtc(void)
{
    const struct tm *tm = get_time();

    return alarm_rtc_make(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                          tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static int days_in_month(int m, int y)
{
    static const unsigned char dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31,
                                          30, 31, 30, 31 };
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : dm[(m - 1) % 12];
}

/* "Mon Wed Fri": three letters a day, two once there are more than three */
static void fmt_days(char *buf, size_t len, unsigned days)
{
    int d, n = 0, w;
    size_t o = 0;

    for (d = 0; d < 7; d++)
        n += (days >> d) & 1;
    w = n > 3 ? 2 : 3;
    buf[0] = '\0';
    for (d = 0; d < 7 && o + 5 < len; d++)
        if ((days >> d) & 1)
            o += snprintf(buf + o, len - o, "%s%.*s", o ? " " : "", w,
                          str(day_lang[d]));
}

/* The repeat as a sentence part: Once, Every Day, ... or the days */
static void fmt_repeat(char *buf, size_t len, const struct alarm *a)
{
    if (a->repeat == AR_DAYS)
    {
        if ((a->days & 0x7f) == 0x7f)
            snprintf(buf, len, "%s", str(LANG_IPODUI_X_REPEAT_EVERY_DAY));
        else
            fmt_days(buf, len, a->days);
    }
    else
        snprintf(buf, len, "%s", str(repeat_lang[a->repeat % AR__N]));
}

/* -- the value lists (language.c's split list, shared) -------------------- */

static const char *vl_label_text(int idx, char *buf, size_t len, int kind)
{
    (void)len;
    switch (kind)
    {
        case 0:  return str(repeat_lang[idx % AR__N]);
        case 1:  return str(LANG_IPODUI_X_BEEP);
        case 2:  return str(label_lang[idx % LABELS]);
        default:
            if (idx == 0)
                return str(LANG_OFF);
            snprintf(buf, len, str(LANG_IPODUI_X_SLEEP_MINUTES),
                     (int)sleep_min[idx % SLEEPS]);
            return buf;
    }
}

static void touched(void)
{
    wk->dirty = true;
    ui_invalidate(UI_DIRTY_BODY);
}

/* Repeat */
static int rp_count(void) { return AR__N; }
static int rp_get(void) { return wk->ed.repeat; }
static int rp_set(int idx)
{
    wk->ed.repeat = idx;
    if (idx == AR_DAYS && !(wk->ed.days & 0x7f))
        wk->ed.days = ALARM_DAYS_WEEKDAYS;
    touched();
    return 1;
}
static const char *rp_label(int idx, char *buf, size_t len)
{
    return vl_label_text(idx, buf, len, 0);
}
static const struct ui_vlist repeat_list = {
    LANG_IPODUI_X_REPEAT, PANELART_REPEAT, rp_count, rp_get, rp_set,
    rp_label, NULL };

/* Alerts (the sound; Q3: one, the piezo) */
static int sn_count(void) { return 1; }
static int sn_get(void) { return 0; }
static int sn_set(int idx)
{
    (void)idx;
    return 1;
}
static const char *sn_label(int idx, char *buf, size_t len)
{
    return vl_label_text(idx, buf, len, 1);
}
static const struct ui_vlist alerts_list = {
    LANG_IPODUI_X_ALERTS, PANELART_SPEAKER, sn_count, sn_get, sn_set,
    sn_label, NULL };

/* Label */
static int lb_count(void) { return LABELS; }
static int lb_get(void) { return wk->ed.label; }
static int lb_set(int idx)
{
    wk->ed.label = idx;
    touched();
    return 1;
}
static const char *lb_label(int idx, char *buf, size_t len)
{
    return vl_label_text(idx, buf, len, 2);
}
static const struct ui_vlist label_list = {
    LANG_IPODUI_X_LABEL, PANELART_MENU, lb_count, lb_get, lb_set,
    lb_label, NULL };

/* Sleep Timer: the highlighted row is the smallest preset that covers
 * what is left (Off when it is not running) */
static int sl_count(void) { return SLEEPS; }
static int sl_get(void)
{
    int left = (get_sleep_timer() + 59) / 60, i;

    if (!get_sleep_timer_active() || left <= 0)
        return 0;
    for (i = 1; i < SLEEPS; i++)
        if (sleep_min[i] >= left)
            return i;
    return SLEEPS - 1;
}
static int sl_set(int idx)
{
    idx = MIN(MAX(idx, 0), SLEEPS - 1);
    set_sleeptimer_duration(sleep_min[idx]);
    UPDATE_TRACE("alarms sleep set=%d", (int)sleep_min[idx]);
    return 1;
}
static const char *sl_label(int idx, char *buf, size_t len)
{
    return vl_label_text(idx, buf, len, 3);
}
static const struct ui_vlist sleep_list = {
    LANG_IPODUI_X_SLEEP_TIMER, PANELART_TIMER, sl_count, sl_get, sl_set,
    sl_label, NULL };

/* -- Date and Time pickers ------------------------------------------------- */

enum { PK_DATE, PK_TIME };
static const int arg_date = PK_DATE, arg_time = PK_TIME;

static void field(struct ui_roller *r, int i, int min, int max, int v, int dg)
{
    r->f[i].min = min;
    r->f[i].max = max;
    r->f[i].value = MIN(MAX(v, min), max);
    r->f[i].digits = dg;
    r->f[i].text = false;
}

static int picker_enter(struct ui_screen *s, const void *arg)
{
    const struct alarm *a;
    bool h12 = global_settings.timeformat != 0;

    if (!wk || !arg)
        return -1;
    a = &wk->ed;
    s->header_w = UI_HEADER_DARK;
    wk->rwhich = *(const int *)arg;
    memset(&wk->r, 0, sizeof(wk->r));
    wk->r.dark = true;
    if (wk->rwhich == PK_DATE)
    {
        s->title = str(LANG_IPODUI_X_DATE);
        wk->r.n = 3;                            /* US order: M / D / Y */
        field(&wk->r, 0, 1, 12, a->mon, 2);
        field(&wk->r, 1, 1, 31, a->mday, 2);
        field(&wk->r, 2, 2000, 2099, a->year, 4);
    }
    else
    {
        s->title = str(LANG_IPODUI_X_TIME);
        if (h12)
        {
            wk->r.n = 3;
            field(&wk->r, 0, 1, 12, a->hour % 12 ? a->hour % 12 : 12, 2);
            field(&wk->r, 1, 0, 59, a->min, 2);
            field(&wk->r, 2, 0, 1, a->hour >= 12, 2);
            wk->r.f[2].text = true;
            wk->r.txt[0] = str(LANG_IPODUI_AM);
            wk->r.txt[1] = str(LANG_IPODUI_PM);
        }
        else
        {
            wk->r.n = 2;
            field(&wk->r, 0, 0, 23, a->hour, 2);
            field(&wk->r, 1, 0, 59, a->min, 2);
        }
    }
    return 0;
}

static void picker_draw(struct ui_screen *s, unsigned dirty)
{
    (void)s;
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
    ui_roller_draw(&wk->r, ROLLER_Y, -1);
}

static void picker_apply(void)
{
    struct alarm *a = &wk->ed;
    const struct ui_roller *r = &wk->r;

    if (wk->rwhich == PK_DATE)
    {
        a->mon = r->f[0].value;
        a->year = r->f[2].value;
        a->mday = MIN(r->f[1].value, days_in_month(a->mon, a->year));
    }
    else
    {
        a->hour = r->n == 3 ? r->f[0].value % 12 + 12 * r->f[2].value
                            : r->f[0].value;
        a->min = r->f[1].value;
    }
    wk->dirty = true;
}

static enum ui_result picker_input(struct ui_screen *s,
                                   const struct ui_input *in)
{
    int by = 0, bh = 0;

    (void)s;
    if (in->key == UI_KEY_PLAY)
        return UI_IGNORED;
    if (ui_roller_input(&wk->r, in, &by, &bh) == UI_ROLLER_BUSY)
    {
        if (bh > 0)
            ui_invalidate_band(by, bh);
        return UI_HANDLED;
    }
    picker_apply();                             /* DONE and MENU both keep it */
    return UI_POP;
}

static const struct ui_screen_ops picker_screen = {
    .name    = "alarms-picker",
    .enter   = picker_enter,
    .leave   = NULL,
    .draw    = picker_draw,
    .input   = picker_input,
    .timeout = NULL,
};

/* -- Custom days ------------------------------------------------------------ */

#define DAY_ACT(n) static void act_day##n(void) \
    { wk->ed.days ^= 1u << (n); touched(); }
DAY_ACT(0) DAY_ACT(1) DAY_ACT(2) DAY_ACT(3) DAY_ACT(4) DAY_ACT(5) DAY_ACT(6)
static void (*const day_act[7])(void) = {
    act_day0, act_day1, act_day2, act_day3, act_day4, act_day5, act_day6,
};

static void days_refresh(void)
{
    int d;

    wk->dmenu.title = str(LANG_IPODUI_X_REPEAT_CUSTOM);
    for (d = 0; d < 7; d++)
    {
        snprintf(wk->dlbl[d], sizeof(wk->dlbl[0]), "%.14s%c%s",
                 str(day_lang[d]), UI_VALUE_SEP,
                 str((wk->ed.days >> d) & 1 ? LANG_ON : LANG_OFF));
        wk->ditem[d] = (struct ui_menu_item){ wk->dlbl[d], NULL, NULL, NULL,
            day_act[d], NULL, NULL };
    }
}

static int days_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
    if (!wk)
        return -1;
    wk->dmenu = (struct ui_menu){ NULL, wk->ditem, 7, false, false,
                                  days_refresh };
    days_refresh();
    return ui_menu_screen.enter(s, &wk->dmenu);
}

static void days_leave(struct ui_screen *s)
{
    ui_menu_screen.leave(s);
}

static void days_draw(struct ui_screen *s, unsigned dirty)
{
    ui_menu_screen.draw(s, dirty);
}

static enum ui_result days_input(struct ui_screen *s,
                                 const struct ui_input *in)
{
    return ui_menu_screen.input(s, in);
}

static int days_tick(struct ui_screen *s)
{
    return ui_menu_screen.tick(s);
}

static const struct ui_screen_ops days_screen = {
    .name    = "alarms-days",
    .enter   = days_enter,
    .leave   = days_leave,
    .draw    = days_draw,
    .input   = days_input,
    .timeout = NULL,
    .tick    = days_tick,
};

/* -- the Create Alarm editor -------------------------------------------------- */

static void act_alarm(void)
{
    wk->ed.on = !wk->ed.on;
    touched();
}

static void act_snooze(void)
{
    wk->ed.snooze = !wk->ed.snooze;
    touched();
}

static void act_date(void) { ui_push(&picker_screen, &arg_date); }
static void act_time(void) { ui_push(&picker_screen, &arg_time); }
static void act_repeat(void) { ui_push(&ui_vlist_screen, &repeat_list); }
static void act_days(void) { ui_push(&days_screen, NULL); }
static void act_alerts(void) { ui_push(&ui_vlist_screen, &alerts_list); }
static void act_label(void) { ui_push(&ui_vlist_screen, &label_list); }

static void act_delete(void)
{
    wk->deleted = true;
    wk->leave_req = true;               /* the tick pops the editor */
    ui_invalidate(UI_DIRTY_BODY);
}

static void ed_refresh(void)
{
    const struct alarm *a = &wk->ed;
    int i, n = 0;
    char v[24];

    wk->emenu.title = str(wk->ed_idx < 0 ? LANG_IPODUI_X_NEW_ALARM
                                         : LANG_IPODUI_X_ALARM);
    for (i = 0; i < E_N; i++)
    {
        void (*act)(void) = NULL;
        int name = 0;

        v[0] = '\0';
        switch (e_order[i])
        {
            case E_ALARM:
                name = LANG_IPODUI_X_ALARM;
                snprintf(v, sizeof(v), "%s", str(a->on ? LANG_ON : LANG_OFF));
                act = act_alarm;
                break;
            case E_DATE:
                if (a->repeat != AR_ONCE)
                    continue;
                name = LANG_IPODUI_X_DATE;
                fmt_date(v, sizeof(v), a);
                act = act_date;
                break;
            case E_TIME:
                name = LANG_IPODUI_X_TIME;
                fmt_time(v, sizeof(v), a->hour, a->min);
                act = act_time;
                break;
            case E_REPEAT:
                name = LANG_IPODUI_X_REPEAT;
                snprintf(v, sizeof(v), "%s",
                         str(repeat_lang[a->repeat % AR__N]));
                act = act_repeat;
                break;
            case E_DAYS:
                if (a->repeat != AR_DAYS)
                    continue;
                name = LANG_IPODUI_X_REPEAT_CUSTOM;
                fmt_days(v, sizeof(v), a->days);
                act = act_days;
                break;
            case E_ALERTS:
                name = LANG_IPODUI_X_ALERTS;
                snprintf(v, sizeof(v), "%s", str(LANG_IPODUI_X_BEEP));
                act = act_alerts;
                break;
            case E_LABEL:
                name = LANG_IPODUI_X_LABEL;
                snprintf(v, sizeof(v), "%s",
                         str(label_lang[a->label % LABELS]));
                act = act_label;
                break;
            case E_SNOOZE:
                name = LANG_IPODUI_X_SNOOZE;
                snprintf(v, sizeof(v), "%s", str(a->snooze ? LANG_ON : LANG_OFF));
                act = act_snooze;
                break;
            default:                    /* E_DELETE: an existing alarm only */
                if (wk->ed_idx < 0)
                    continue;
                name = LANG_IPODUI_X_DELETE_ALARM;
                act = act_delete;
                break;
        }
        if (v[0])
            snprintf(wk->elbl[n], sizeof(wk->elbl[0]), "%.12s%c%.22s", str(name),
                     UI_VALUE_SEP, v);
        else
            snprintf(wk->elbl[n], sizeof(wk->elbl[0]), "%s", str(name));
        wk->eitem[n] = (struct ui_menu_item){ wk->elbl[n], NULL, NULL, NULL, act,
                                              NULL, NULL };
        n++;
    }
    wk->emenu.count = n;
}

/* Defaults of a new alarm: 7:00, on, Once, the next 7:00 */
static void ed_defaults(struct alarm *a)
{
    uint32_t now = now_rtc(), t;
    int y, mo, d, h, m, s;

    memset(a, 0, sizeof(*a));
    a->on = 1;
    a->repeat = AR_ONCE;
    a->snooze = 1;
    a->hour = 7;
    alarm_rtc_split(now, &y, &mo, &d, &h, &m, &s);
    t = alarm_rtc_make(y, mo, d, a->hour, a->min, 0);
    if (t <= now)
        alarm_rtc_split(t + 24 * 3600, &y, &mo, &d, &h, &m, &s);
    a->year = y; a->mon = mo; a->mday = d;
}

/* What is saved: no empty Custom, and a one-time alarm that already
 * passed moves to its next day */
static void ed_normalize(struct alarm *a)
{
    uint32_t now = now_rtc(), t;
    int y, mo, d, h, m, s;

    if (a->repeat == AR_DAYS && !(a->days & 0x7f))
        a->repeat = AR_ONCE;
    a->snoozes = 0;
    a->snooze_until = 0;
    a->missed = 0;
    a->missed_at = 0;
    if (a->repeat == AR_ONCE && a->on)
    {
        t = alarm_rtc_make(a->year, a->mon, a->mday, a->hour, a->min, 0);
        if (t <= now)
        {
            t = alarm_rtc_make(a->year, a->mon, a->mday, a->hour, a->min, 0);
            while (t <= now)
                t += 24 * 3600;
            alarm_rtc_split(t, &y, &mo, &d, &h, &m, &s);
            a->year = y; a->mon = mo; a->mday = d;
        }
    }
}

static int editor_enter(struct ui_screen *s, const void *arg)
{
    int idx;

    if (!wk || !arg)
        return -1;
    idx = *(const int *)arg;
    if (idx >= alarm_count())
        return -1;
    wk->ed_idx = idx;
    wk->dirty = wk->deleted = wk->leave_req = false;
    if (idx < 0)
        ed_defaults(&wk->ed);
    else
        wk->ed = *alarm_get(idx);
    wk->emenu = (struct ui_menu){ NULL, wk->eitem, E_N, false, false,
                                  ed_refresh };
    ed_refresh();
    UPDATE_TRACE("alarms edit idx=%d", idx);
    return ui_menu_screen.enter(s, &wk->emenu);
}

/* The commit point: one write, only for a change */
static void editor_leave(struct ui_screen *s)
{
    ui_menu_screen.leave(s);
    if (!wk)
        return;
    if (wk->deleted)
    {
        if (wk->ed_idx >= 0)
            alarm_delete(wk->ed_idx);
        UPDATE_TRACE("alarms delete idx=%d", wk->ed_idx);
    }
    else if (wk->dirty)
    {
        int idx;

        ed_normalize(&wk->ed);
        /* outside the trace: UPDATE_TRACE compiles to nothing on the
         * device, and the save must happen there too */
        idx = alarm_set(wk->ed_idx, &wk->ed);
        (void)idx;                      /* read only by the trace */
        UPDATE_TRACE("alarms save idx=%d on=%d rep=%d %02d:%02d",
                     idx, (int)wk->ed.on, (int)wk->ed.repeat,
                     (int)wk->ed.hour, (int)wk->ed.min);
    }
    wk->dirty = wk->deleted = false;
}

static void editor_draw(struct ui_screen *s, unsigned dirty)
{
    ui_menu_screen.draw(s, dirty);
}

static enum ui_result editor_input(struct ui_screen *s,
                                   const struct ui_input *in)
{
    return ui_menu_screen.input(s, in);
}

static int editor_tick(struct ui_screen *s)
{
    if (wk && wk->leave_req)
    {
        wk->leave_req = false;
        ui_pop_request(s);
        return TIMEOUT_BLOCK;
    }
    return ui_menu_screen.tick(s);
}

static const struct ui_screen_ops editor_screen = {
    .name    = "alarms-edit",
    .enter   = editor_enter,
    .leave   = editor_leave,
    .draw    = editor_draw,
    .input   = editor_input,
    .timeout = NULL,
    .tick    = editor_tick,
};

/* -- the Alarms list ---------------------------------------------------------- */

static const int idx_arg[ALARM_MAX + 1] = { -1, 0, 1, 2, 3, 4, 5, 6, 7 };

/* rows: [Create Alarm], Sleep Timer, alarm 0 .. n-1 */
static int rows_fixed(void)
{
    return alarm_count() >= ALARM_MAX ? 1 : 2;
}

static int rows_total(void)
{
    return rows_fixed() + alarm_count();
}

static bool row_is_create(int row)
{
    return rows_fixed() == 2 && row == 0;
}

static bool row_is_sleep(int row)
{
    return row == rows_fixed() - 1;
}

static const char *list_label(int row, void *data, char *buf, size_t len)
{
    const struct alarm *a;

    (void)data;
    if (row_is_create(row))
        return str(LANG_IPODUI_X_CREATE_ALARM);
    if (row_is_sleep(row))
        return str(LANG_IPODUI_X_SLEEP_TIMER);
    a = alarm_get(row - rows_fixed());
    if (!a)
        return "";
    fmt_time(buf, len, a->hour, a->min);
    return buf;
}

static const char *list_sub(int row, void *data, char *buf, size_t len)
{
    const struct alarm *a;
    char t[24], r[28];
    int i, left;

    (void)data;
    if (row_is_create(row))
        return str(LANG_IPODUI_X_NEW_ALARM);
    if (row_is_sleep(row))
    {
        left = (get_sleep_timer() + 59) / 60;
        if (!get_sleep_timer_active() || left <= 0)
            return str(LANG_IPODUI_X_INACTIVE);
        if (left == 1)
            return str(LANG_IPODUI_X_ONE_MINUTE_LEFT);
        snprintf(buf, len, str(LANG_IPODUI_X_MINUTES_LEFT), left);
        return buf;
    }
    i = row - rows_fixed();
    a = alarm_get(i);
    if (!a)
        return "";
    if (i < ALARM_MAX && ((wk->missed >> i) & 1))
    {
        int y, mo, d, h, m, s;

        alarm_rtc_split(wk->missed_at[i], &y, &mo, &d, &h, &m, &s);
        fmt_time(t, sizeof(t), h, m);
        snprintf(buf, len, str(LANG_IPODUI_X_MISSED_AT), t);
        return buf;
    }
    fmt_repeat(r, sizeof(r), a);
    if (a->label)
        snprintf(buf, len, "%s, %s", str(label_lang[a->label % LABELS]), r);
    else
        snprintf(buf, len, "%s", r);
    if (!a->on)
    {
        size_t o = strlen(buf);

        snprintf(buf + o, len - o, ", %s", str(LANG_OFF));
    }
    return buf;
}

static bool list_chev(int row, void *data)
{
    (void)row; (void)data;
    return true;
}

static int list_enter(struct ui_screen *s, const void *arg)
{
    const struct ui_theme *t = ui_theme;
    struct ui_row_style st;
    struct viewport vp;
    int i, n;
    bool any = false;

    (void)arg;
    wk = mod_work_alloc(MOD_ALARMS, WORK_BYTES);
    if (!wk)
        return -1;
    memset(wk, 0, WORK_BYTES);
    s->title = str(LANG_IPODUI_X_ALARMS);
    n = alarm_count();
    for (i = 0; i < n && i < ALARM_MAX; i++)    /* Q17: copy, then clear */
    {
        const struct alarm *a = alarm_get(i);

        if (a && a->missed)
        {
            wk->missed |= 1u << i;
            wk->missed_at[i] = a->missed_at;
            any = true;
        }
    }
    if (any)
        alarm_seen();
    /* split, like the 7G's (owner photo 1198): the list at the left,
     * the alarm clock in the panel on every row */
    s->header_w = t->split_x;
    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = t->split_x;
    vp.height = LCD_HEIGHT - t->header_h;
    ui_list_init(&wk->list, &vp, rows_total(), list_label, wk);
    memset(&st, 0, sizeof(st));
    st.rows = t->list_rows_two;
    st.text_x = t->text_pad_x;
    st.flags = UI_ROW_TWO_LINE | UI_ROW_SEP;
    ui_list_set_style(&wk->list, &st);
    wk->list.get_sublabel = list_sub;
    wk->list.has_chevron = list_chev;
    ui_panel_select_screen(s);
    UPDATE_TRACE("alarms open count=%d missed=%d", n, (int)any);
    return 0;
}

static void list_leave(struct ui_screen *s)
{
    (void)s;
    wk = NULL;                                  /* the arena takes the block */
}

static void list_draw(struct ui_screen *s, unsigned dirty)
{
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    (void)s;
    if (dirty & UI_DIRTY_BODY)
    {
        ui_list_set_count(&wk->list, rows_total());  /* an edit came back */
        ui_list_draw(&wk->list, wk->list.vp.y, wk->list.vp.height);
        ui_panel_draw_now();
    }
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&wk->list, y[i], h[i]);
    }
}

static int list_tick(struct ui_screen *s)
{
    (void)s;
    return ui_list_tick(&wk->list);
}

static enum ui_result list_input(struct ui_screen *s, const struct ui_input *in)
{
    int y[2], h[2], i, n, row;

    (void)s;
    switch (in->key)
    {
        case UI_KEY_WHEEL:
            n = ui_list_move(&wk->list, in->wheel_steps, y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            row = wk->list.selected;
            if (ui_list_gliding(&wk->list))
                ui_list_draw(&wk->list, wk->list.vp.y, wk->list.vp.height);
            if (row_is_create(row))
                ui_push(&editor_screen, &idx_arg[0]);
            else if (row_is_sleep(row))
                ui_push(&ui_vlist_screen, &sleep_list);
            else
                ui_push(&editor_screen, &idx_arg[1 + row - rows_fixed()]);
            return UI_HANDLED;
        default:
            return UI_IGNORED;                  /* MENU leaves */
    }
}

/* the panel: the same alarm clock on every row (photo 1198) */
static void list_panel(struct ui_screen *s, const struct ui_panel *p)
{
    (void)s;
    ui_preview_x_alarms.draw(p, NULL, 0);
}

const struct ui_screen_ops ui_x_alarms_screen = {
    .name    = "alarms",
    .enter   = list_enter,
    .leave   = list_leave,
    .draw    = list_draw,
    .input   = list_input,
    .timeout = NULL,
    .tick    = list_tick,
    .draw_into_panel = list_panel,
};
