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

/* iPod UI shell: Extras > Calendars (P12 W3-C, MOD_CALENDARS, Class A;
 * spec extras.md, owner photo 1196). The split list (All Calendars,
 * each calendar when there are several, To Do's) with the calendar-page
 * panel; the month grid (full screen: weekday letters, six weeks, a dot
 * on each day with events, the selection on the blue gel); a day's
 * events (two-line rows: title, then the time or All Day); an event's
 * page (title, date, time, location, notes); the To Do's list.
 *
 * Keys: the wheel moves a day (across months), PREVIOUS/NEXT a month,
 * SELECT opens, a short MENU backs out (Class A). The 7G's Alarms row
 * (the event alarm sound) stays hidden until event alarms reach the alarm
 * engine (a W1-A1 source): a row that changed nothing would mislead.
 *
 * Storage (brief 2.1, Class A): only the sync-time index (calendars.idx,
 * built by MOD_XINDEX: xical_core.h). Entry: one open and one read from
 * offset 0 into the cache: the header and every table (the names, the
 * day ranges, the day bits: the month grid draws from them), and the
 * whole index when it fits (target: the state is 2.3 KB, so 4.9 KB of
 * cache resident, about 70 instances past the tables; 9.8 KB with
 * modules, about 300). Then nothing is read while
 * Calendars stays open. A larger index runs in window mode: a day, the
 * To Do's or an event reads its records and strings when it opens
 * (logged "xcal win"), never while the wheel turns.
 *
 * Memory: the module's one working block (mod_work_alloc(), from the
 * root's enter), at most 7 KB; no other allocation. */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "rbunicode.h"
#include "timefuncs.h"
#include "settings.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/anim.h"
#include "ipodui/list.h"
#include "ipodui/menu.h"
#include "ipodui/extras.h"
#include "ipodui/screen_stack.h"
#include "ipodui/modload.h"
#include "ipodui/modclass.h"
#include "ipodui/tzpick.h"
#include "ipodui/xindex.h"
#include "ipodui/xical_core.h"
#include "ipodui/screens/calendars.h"

/* The working block: 7 KB from the screen arena in a resident build,
 * 12 KB from the module arena (calendars.mod); the rest of the
 * state is the same, the cache gets the difference. */
#if defined(IPODUI_MODULES)
#define CAL_WORK_KB     12
#else
#define CAL_WORK_KB     7
#endif
#define CAL_WORK        (CAL_WORK_KB * 1024)
#define ROWS_MAX        16              /* a day's events, To Do's shown */
#define ROW_TITLE       40              /* bytes of a row's title */
#define DETAIL_MAX      360
#define DLINES_MAX      32
#define SPAN_MAX        (XCAL_TITLE_MAX + XCAL_LOC_MAX + XCAL_DESC_MAX)
#define DAY_S           86400

/* the month grid (320 x 220 under the header) */
#define WD_H            22              /* the weekday letters */
#define ROW_H           33              /* six weeks: 22 + 6 x 33 = 220 */
#define WEEKS           6
#define DOT             5
#define C_WD_BG         LCD_RGBPACK(0xe9, 0xea, 0xee)
#define C_GRID          LCD_RGBPACK(0xd2, 0xd4, 0xda)
#define C_OTHER         LCD_RGBPACK(0xa9, 0xab, 0xb1)
#define C_DOT           LCD_RGBPACK(0x55, 0x58, 0x60)
#define C_TODAY         LCD_RGBPACK(0x1f, 0x5f, 0xd0)

/* the event page */
#define MARGIN          10
#define PITCH_TITLE     20
#define PITCH_TEXT      16

enum { L_TITLE = 0, L_TEXT, L_DIM };

struct cal_work {
    /* the index: identity and layout (the header) */
    uint32_t size, rec_off, str_off, str_bytes, count;
    uint8_t ok, seq, whole;
    int32_t day0;                       /* the window's first day */
    /* the root list */
    struct ui_menu menu;
    struct ui_menu_item item[2 + XCAL_MAX_CALS];
    /* the month grid */
    int filter;                         /* -1: all calendars */
    int32_t sel, month0;                /* days since 2000 */
    char mtitle[32];
    /* a day's events or the To Do's */
    struct ui_list list;
    int nrow;
    uint8_t todo;
    struct xcal_rec row[ROWS_MAX];
    char rtitle[ROWS_MAX][ROW_TITLE];
    char ltitle[48];
    /* an event's page */
    char text[DETAIL_MAX];
    uint16_t lstart[DLINES_MAX];
    uint8_t llen[DLINES_MAX], lkind[DLINES_MAX];
    int nlines, top, rows;
    /* the cache: from offset 0, the header, the tables (always), then
     * the records and the strings when the whole index fits */
    uint8_t cache[];
};
#define CACHE_BYTES     (CAL_WORK - sizeof(struct cal_work))
_Static_assert(sizeof(struct cal_work) + sizeof(struct xidx_hdr)
               + XCAL_TAB_MAX <= CAL_WORK,
               "calendars: the header and the tables always fit");

static struct cal_work *cw;             /* the root's enter to its leave */

static const struct ui_screen_ops month_screen, day_screen, todo_screen,
                                  event_screen;

/* -- the index ------------------------------------------------------------- */

static const struct xcal_meta *meta(const struct cal_work *w)
{
    return (const struct xcal_meta *)(w->cache + sizeof(struct xidx_hdr));
}

static const uint16_t *first_tab(const struct cal_work *w)
{
    return (const uint16_t *)((const uint8_t *)meta(w) + xcal_first_off());
}

static const uint16_t *ovl_tab(const struct cal_work *w)
{
    return (const uint16_t *)((const uint8_t *)meta(w)
                              + xcal_ovl_off(meta(w)));
}

/* calendar c's day bits (row c + 1; row 0 any calendar) */
static const uint8_t *bits_row(const struct cal_work *w, int c)
{
    const struct xcal_meta *m = meta(w);

    return (const uint8_t *)m + xcal_bits_off(m) + (c + 1) * m->bm_bytes;
}

static bool read_at(int fd, uint32_t off, void *buf, uint32_t len)
{
    return lseek(fd, off, SEEK_SET) == (off_t)off
           && read(fd, buf, len) == (ssize_t)len;
}

/* Entry: one open, one read from offset 0 into the cache: the header and
 * every table; the whole index when it fits (then nothing is read again
 * while Calendars stays open) */
static void load_index(struct cal_work *w)
{
    const struct xidx_hdr *h = (const struct xidx_hdr *)w->cache;
    ssize_t got = 0;
    int fd, rc = XIDX_ESHORT;

    w->ok = w->whole = 0;
    w->seq = xindex_seq();
    if (xindex_busy())
        return;
    fd = open(XCAL_IDX, O_RDONLY);
    if (fd >= 0)
    {
        w->size = (uint32_t)ffilesize(fd);
        got = read(fd, w->cache, MIN(w->size, CACHE_BYTES));
        close(fd);
    }
    if (got >= (ssize_t)sizeof(*h))
        rc = xidx_check(h, w->size);
    if (rc == XIDX_OK && h->kind != XIDX_CAL)
        rc = XIDX_EKIND;
    if (rc == XIDX_OK && (uint32_t)got < h->rec_off)
        rc = XIDX_ESHORT;               /* the tables did not fit */
    if (rc == XIDX_OK)
        rc = xcal_meta_check(h, meta(w), w->size);
    if (rc == XIDX_OK)
        rc = xcal_tab_check(meta(w), h->rec_off - sizeof(*h));
    if (rc == XIDX_OK && (uint32_t)got == w->size)
    {
        rc = xidx_check_body(w->cache, w->size);
        w->whole = rc == XIDX_OK;
    }
    if (rc == XIDX_OK)
    {
        w->count = h->count;
        w->rec_off = h->rec_off;
        w->str_off = h->str_off;
        w->str_bytes = h->str_bytes;
        w->day0 = (int32_t)(meta(w)->day0 / DAY_S);
        w->ok = 1;
    }
    UPDATE_TRACE("xcal open n=%lu events=%u cals=%u days=%u whole=%d rc=%d "
                 "bytes=%lu", (unsigned long)(w->ok ? w->count : 0),
                 w->ok ? meta(w)->nevents : 0, w->ok ? meta(w)->ncal : 0,
                 w->ok ? meta(w)->ndays : 0, w->whole, rc,
                 (unsigned long)w->size);
}

/* a string at off of at most len bytes (with its NUL), cut on a
 * character: from the cache, or one read (fd >= 0) */
static void get_str(const struct cal_work *w, int fd, uint32_t off,
                    char *buf, uint32_t len)
{
    uint32_t n = MIN(len - 1, w->str_bytes > off ? w->str_bytes - off : 0);

    buf[0] = '\0';
    if (!n)
        return;
    if (w->whole)
        memcpy(buf, w->cache + w->str_off + off, n);
    else if (fd < 0 || !read_at(fd, w->str_off + off, buf, n))
        return;
    buf[n] = '\0';
    if (strlen(buf) == n)
    {
        /* cut: never half a character */
        while (n > 0 && ((unsigned char)buf[n - 1] & 0xc0) == 0x80)
            n--;
        if (n > 0 && ((unsigned char)buf[n - 1] & 0xc0) == 0xc0)
            n--;
        buf[n] = '\0';
    }
}

/* records [from, from + n) into out: from the cache, or one read */
static bool get_recs(const struct cal_work *w, int fd, uint32_t from,
                     uint32_t n, struct xcal_rec *out)
{
    if (w->whole)
    {
        memcpy(out, w->cache + w->rec_off + from * sizeof(*out),
               n * sizeof(*out));
        return true;
    }
    return fd >= 0 && read_at(fd, w->rec_off + from * sizeof(*out), out,
                              n * sizeof(*out));
}

/* window mode (an index larger than the cache): the file, for reads past
 * the entry; -1 in whole mode */
static int win_open(const struct cal_work *w)
{
    if (w->whole || xindex_busy())
        return -1;
    return open(XCAL_IDX, O_RDONLY);
}

/* -- formatting ------------------------------------------------------------ */

static void fmt_time(char *buf, size_t len, uint32_t rtc)
{
    int h = (int)(rtc % DAY_S / 3600), m = (int)(rtc % 3600 / 60);

    if (global_settings.timeformat)     /* 12 hour clock */
        snprintf(buf, len, "%d:%02d %s", h % 12 ? h % 12 : 12, m,
                 str(h < 12 ? LANG_IPODUI_AM : LANG_IPODUI_PM));
    else
        snprintf(buf, len, "%02d:%02d", h, m);
}

/* a day's row: "9:00 AM - 9:30 AM", or All Day */
static void fmt_span(char *buf, size_t len, const struct xcal_rec *r,
                     uint32_t d0)
{
    char a[16], b[16];

    if ((r->flags & XCAL_ALL_DAY)
        || (r->start_rtc <= d0 && r->end_rtc >= d0 + DAY_S))
    {
        strlcpy(buf, str(LANG_IPODUI_X_ALL_DAY), len);
        return;
    }
    fmt_time(a, sizeof(a), r->start_rtc);
    if (r->end_rtc <= r->start_rtc)
    {
        strlcpy(buf, a, len);
        return;
    }
    fmt_time(b, sizeof(b), r->end_rtc);
    snprintf(buf, len, "%s \xe2\x80\x93 %s", a, b);
}

static void fmt_day(char *buf, size_t len, int32_t day, bool year)
{
    struct tm tm;

    tz_rtc_split((uint32_t)day * DAY_S, &tm);
    if (year)
        snprintf(buf, len, "%s, %s %d, %d",
                 str(LANG_IPODUI_SUNDAY + tm.tm_wday),
                 str(LANG_MONTH_JANUARY + tm.tm_mon), tm.tm_mday,
                 tm.tm_year + 1900);
    else
        snprintf(buf, len, "%s, %s %d", str(LANG_IPODUI_SUNDAY + tm.tm_wday),
                 str(LANG_MONTH_JANUARY + tm.tm_mon), tm.tm_mday);
}

static int32_t today(void)
{
    return (int32_t)(tz_rtc_make(get_time()) / DAY_S);
}

/* The grey sentence of an empty list, centred */
static void draw_empty(const char *s)
{
    const struct ui_theme *t = ui_theme;
    struct viewport vp;

    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.font = t->font_small;
    vp.drawmode = DRMODE_SOLID;
    lcd_set_viewport(&vp);
    lcd_set_foreground(t->list_bg);
    lcd_fillrect(0, 0, vp.width, vp.height);
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(t->list_fg_dim);
    ui_paint_text_wrap(LCD_WIDTH / 2, vp.height / 2, LCD_WIDTH - 4 * MARGIN,
                       3, PITCH_TEXT, s);
    lcd_set_viewport(NULL);
}

/* -- the month grid -------------------------------------------------------- */

static bool has_events(const struct cal_work *w, int32_t day)
{
    int d = day - w->day0;

    if (!w->ok || d < 0 || d >= meta(w)->ndays)
        return false;
    if (w->filter >= 0)
        return (bits_row(w, w->filter)[d >> 3] >> (d & 7)) & 1;
    /* any calendar, from the day ranges alone: something starts that day,
     * or an earlier one overlaps it */
    return first_tab(w)[d] < first_tab(w)[d + 1]
           || ovl_tab(w)[d] < first_tab(w)[d];
}

static void month_set(struct cal_work *w, int32_t day)
{
    int y, m, d;
    int64_t dd = day;

    xcal_civil(dd, &y, &m, &d);
    w->month0 = (int32_t)xcal_days(y, m, 1);
    snprintf(w->mtitle, sizeof(w->mtitle), "%s %d",
             str(LANG_MONTH_JANUARY + m - 1), y);
}

/* the grid's first day: the Sunday on or before the 1st */
static int32_t grid0(const struct cal_work *w)
{
    return w->month0 - xcal_wday(w->month0);
}

static int row_y(int r)
{
    return ui_theme->header_h + WD_H + r * ROW_H;
}

static void draw_week(struct cal_work *w, int r, int32_t now)
{
    const struct ui_theme *t = ui_theme;
    int c, y = row_y(r), wdth, hgt;
    int y_month, m_month, d_month;

    xcal_civil(w->month0, &y_month, &m_month, &d_month);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(t->list_bg);
    lcd_fillrect(0, y, LCD_WIDTH, ROW_H);
    for (c = 0; c < 7; c++)
    {
        int32_t day = grid0(w) + r * 7 + c;
        int x0 = c * LCD_WIDTH / 7, x1 = (c + 1) * LCD_WIDTH / 7;
        int yy, mm, dd, cx = (x0 + x1) / 2;
        bool sel = day == w->sel;
        char num[4];

        xcal_civil(day, &yy, &mm, &dd);
        if (sel)
            ui_paint_selection(x0, y, x1 - x0 - 1, ROW_H - 1);
        lcd_set_drawmode(DRMODE_FG);
        snprintf(num, sizeof(num), "%d", dd);
        lcd_getstringsize((const unsigned char *)num, &wdth, &hgt);
        lcd_set_foreground(sel ? t->sel_fg
                           : day == now ? C_TODAY
                           : mm != m_month ? C_OTHER : t->list_fg);
        lcd_putsxy(cx - wdth / 2, y + 4, (const unsigned char *)num);
        if (has_events(w, day))
        {
            lcd_set_foreground(sel ? t->sel_fg : mm != m_month ? C_OTHER
                                                               : C_DOT);
            lcd_fillrect(cx - DOT / 2 + 1, y + ROW_H - 9, DOT - 2, DOT);
            lcd_fillrect(cx - DOT / 2, y + ROW_H - 8, DOT, DOT - 2);
        }
        lcd_set_drawmode(DRMODE_SOLID);
        lcd_set_foreground(C_GRID);
        if (c < 6)
            lcd_vline(x1 - 1, y, y + ROW_H - 1);
        lcd_hline(x0, x1 - 1, y + ROW_H - 1);
    }
}

static void month_draw_rows(struct cal_work *w, int y, int h)
{
    const struct ui_theme *t = ui_theme;
    struct viewport vp;
    int32_t now = today();
    int r;

    memset(&vp, 0, sizeof(vp));
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.font = t->font_ui;
    vp.drawmode = DRMODE_SOLID;
    lcd_set_viewport(&vp);
    for (r = 0; r < WEEKS; r++)
        if (row_y(r) < y + h && row_y(r) + ROW_H > y)
            draw_week(w, r, now);
    lcd_set_viewport(NULL);
}

static void month_draw_head(void)
{
    const struct ui_theme *t = ui_theme;
    struct viewport vp;
    int c, wdth, hgt;

    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = WD_H;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.font = t->font_small;
    vp.drawmode = DRMODE_SOLID;
    lcd_set_viewport(&vp);
    lcd_set_foreground(C_WD_BG);
    lcd_fillrect(0, 0, LCD_WIDTH, WD_H);
    lcd_set_foreground(C_GRID);
    lcd_hline(0, LCD_WIDTH - 1, WD_H - 1);
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(t->list_fg_dim);
    for (c = 0; c < 7; c++)
    {
        /* the weekday's first letter (one character of its name) */
        const unsigned char *n = (const unsigned char *)
                                 str(LANG_IPODUI_SUNDAY + c);
        ucschar_t ch;
        char one[8];
        int l = (int)(utf8decode(n, &ch) - n);

        memcpy(one, n, l);
        one[l] = '\0';
        lcd_getstringsize((const unsigned char *)one, &wdth, &hgt);
        lcd_putsxy((c * LCD_WIDTH / 7 + (c + 1) * LCD_WIDTH / 7) / 2
                   - wdth / 2, (WD_H - hgt) / 2, (const unsigned char *)one);
    }
    lcd_set_viewport(NULL);
}

static int month_enter(struct ui_screen *s, const void *arg)
{
    struct cal_work *w = cw;
    int32_t now = today();

    if (!w)
        return -1;
    s->priv = w;
    w->filter = arg ? *(const int *)arg : -1;
    if (w->ok && w->filter >= meta(w)->ncal)
        w->filter = -1;
    w->sel = now;
    if (w->ok && (now < w->day0 || now >= w->day0 + meta(w)->ndays))
        w->sel = w->day0 + meta(w)->ndays / 2;
    month_set(w, w->sel);
    s->title = w->mtitle;
    UPDATE_TRACE("xcal month filter=%d sel=%ld", w->filter, (long)w->sel);
    return 0;
}

static void month_draw(struct ui_screen *s, unsigned dirty)
{
    struct cal_work *w = s->priv;
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    if (dirty & UI_DIRTY_BODY)
    {
        month_draw_head();
        month_draw_rows(w, row_y(0), WEEKS * ROW_H);
    }
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            month_draw_rows(w, y[i], h[i]);
    }
}

/* the window's days, the grid's limits */
static int32_t clamp_day(const struct cal_work *w, int32_t day)
{
    int32_t lo = w->ok ? w->day0 : today() - 400;
    int32_t hi = w->ok ? w->day0 + meta(w)->ndays - 1 : today() + 400;

    return day < lo ? lo : day > hi ? hi : day;
}

static void month_move(struct cal_work *w, int32_t to)
{
    int32_t was = w->sel, g = grid0(w);
    int yy, mm, dd;

    to = clamp_day(w, to);
    if (to == was)
        return;
    w->sel = to;
    xcal_civil(to, &yy, &mm, &dd);
    if ((int32_t)xcal_days(yy, mm, 1) != w->month0)
    {
        month_set(w, to);
        ui_invalidate(UI_DIRTY_ALL);    /* a new month: its title */
        return;
    }
    ui_invalidate_band(row_y((was - g) / 7), ROW_H);
    ui_invalidate_band(row_y((to - g) / 7), ROW_H);
}

static enum ui_result month_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    struct cal_work *w = s->priv;
    int yy, mm, dd;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            month_move(w, w->sel + in->wheel_steps);
            return UI_HANDLED;
        case UI_KEY_PREV:
        case UI_KEY_NEXT:
            if (in->release)
                return UI_HANDLED;
            /* a month on or back, the same day where it exists */
            xcal_civil(w->sel, &yy, &mm, &dd);
            mm += in->key == UI_KEY_NEXT ? 1 : -1;
            if (mm > 12)
            {
                mm = 1;
                yy++;
            }
            if (mm < 1)
            {
                mm = 12;
                yy--;
            }
            month_move(w, (int32_t)xcal_days(yy, mm, MIN(dd,
                                                          xcal_mdays(yy, mm))));
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (in->release && !in->long_press)
                ui_push(&day_screen, NULL);
            return UI_HANDLED;
        default:
            return UI_IGNORED;          /* MENU backs out */
    }
}

static int month_tick(struct ui_screen *s)
{
    struct cal_work *w = s->priv;

    if (w->seq != xindex_seq())
        ui_pop_request(s);              /* the root reads the new index */
    return TIMEOUT_BLOCK;
}

static const struct ui_screen_ops month_screen = {
    .name    = "calendars-month",
    .enter   = month_enter,
    .leave   = NULL,
    .draw    = month_draw,
    .input   = month_input,
    .timeout = NULL,
    .tick    = month_tick,
};

/* -- a day's events and the To Do's (one list) ----------------------------- */

static const char *row_label(int idx, void *data, char *buf, size_t len)
{
    struct cal_work *w = data;

    (void)buf; (void)len;
    return idx < w->nrow ? w->rtitle[idx] : "";
}

static const char *row_sub(int idx, void *data, char *buf, size_t len)
{
    struct cal_work *w = data;
    const struct xcal_rec *r;

    if (idx >= w->nrow)
        return "";
    r = &w->row[idx];
    if (w->todo)
    {
        struct tm tm;

        if (!r->start_rtc)
            return "";
        tz_rtc_split(r->start_rtc, &tm);
        snprintf(buf, len, "%s %d/%d/%02d", str(LANG_IPODUI_X_DUE),
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_year % 100);
        return buf;
    }
    fmt_span(buf, len, r, (uint32_t)w->sel * DAY_S);
    return buf;
}

static void list_setup(struct cal_work *w)
{
    const struct ui_theme *t = ui_theme;
    struct ui_row_style st;
    struct viewport vp;

    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;
    ui_list_init(&w->list, &vp, w->nrow, row_label, w);
    memset(&st, 0, sizeof(st));
    st.rows = t->list_rows_two;
    st.text_x = t->text_pad_x;
    st.flags = UI_ROW_TWO_LINE | UI_ROW_SEP;
    ui_list_set_style(&w->list, &st);
    w->list.get_sublabel = row_sub;
}

/* the rows' titles: from the cache, or one read each (window mode) */
static void load_titles(struct cal_work *w, int fd)
{
    int i;

    for (i = 0; i < w->nrow; i++)
        get_str(w, fd, w->row[i].title_off, w->rtitle[i], ROW_TITLE);
}

/* The events overlapping the selected day (of the calendar shown): the
 * records [ovl[d], first[d + 1]), ROWS_MAX at a time */
static void load_day(struct cal_work *w)
{
    uint32_t d0 = (uint32_t)w->sel * DAY_S, from, to, k, n;
    int d = w->sel - w->day0, fd;

    w->nrow = 0;
    if (!w->ok || d < 0 || d >= meta(w)->ndays)
        return;
    from = ovl_tab(w)[d];
    to = first_tab(w)[d + 1];
    if (from >= to)
    {
        UPDATE_TRACE("xcal day d=%d n=0 range=0", d);
        return;
    }
    fd = win_open(w);
    if (!w->whole && fd < 0)
        return;
    for (k = from; k < to && w->nrow < ROWS_MAX; k += n)
    {
        struct xcal_rec *buf = &w->row[w->nrow];
        uint32_t i, keep = 0;

        n = MIN(to - k, (uint32_t)(ROWS_MAX - w->nrow));
        if (!get_recs(w, fd, k, n, buf))
            break;
        for (i = 0; i < n; i++)
        {
            const struct xcal_rec *r = &buf[i];
            uint32_t end = r->end_rtc > r->start_rtc ? r->end_rtc
                                                     : r->start_rtc + 1;

            if (r->start_rtc < d0 + DAY_S && end > d0
                && (w->filter < 0 || r->cal == w->filter))
                buf[keep++] = *r;
        }
        w->nrow += keep;
    }
    load_titles(w, fd);
    if (fd >= 0)
    {
        close(fd);
        UPDATE_TRACE("xcal win day=%d", d);
    }
    UPDATE_TRACE("xcal day d=%d n=%d range=%lu", d, w->nrow,
                 (unsigned long)(to - from));
}

static void load_todos(struct cal_work *w)
{
    uint32_t from = meta(w)->nevents, n;
    int fd;

    w->nrow = 0;
    if (!w->ok || from >= w->count)
        return;
    n = MIN(w->count - from, (uint32_t)ROWS_MAX);
    fd = win_open(w);
    if (!w->whole && fd < 0)
        return;
    if (get_recs(w, fd, from, n, w->row))
    {
        w->nrow = (int)n;
        load_titles(w, fd);
    }
    if (fd >= 0)
    {
        close(fd);
        UPDATE_TRACE("xcal win todo=%d", w->nrow);
    }
    UPDATE_TRACE("xcal todo n=%d of=%lu", w->nrow,
                 (unsigned long)(w->count - from));
}

static int day_enter(struct ui_screen *s, const void *arg)
{
    struct cal_work *w = cw;

    (void)arg;
    if (!w)
        return -1;
    s->priv = w;
    w->todo = 0;
    modclass_entry(MOD_CALENDARS);
    load_day(w);
    fmt_day(w->ltitle, sizeof(w->ltitle), w->sel, false);
    s->title = w->ltitle;
    list_setup(w);
    return 0;
}

static int todo_enter(struct ui_screen *s, const void *arg)
{
    struct cal_work *w = cw;

    (void)arg;
    if (!w)
        return -1;
    s->priv = w;
    w->todo = 1;
    modclass_entry(MOD_CALENDARS);
    load_todos(w);
    s->title = str(LANG_IPODUI_X_TODOS);
    list_setup(w);
    return 0;
}

static void list_draw(struct ui_screen *s, unsigned dirty)
{
    struct cal_work *w = s->priv;
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    if (!w->nrow)
    {
        if (dirty & UI_DIRTY_BODY)
            draw_empty(str(w->todo ? LANG_IPODUI_X_NO_TODOS
                           : w->ok && meta(w)->ncal
                             ? LANG_IPODUI_X_NO_EVENTS
                             : LANG_IPODUI_X_CALENDARS_EMPTY));
        return;
    }
    if (dirty & UI_DIRTY_BODY)
        ui_list_draw(&w->list, w->list.vp.y, w->list.vp.height);
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&w->list, y[i], h[i]);
    }
}

static enum ui_result list_input(struct ui_screen *s,
                                 const struct ui_input *in)
{
    struct cal_work *w = s->priv;
    int y[2], h[2], i, n;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            if (!w->nrow)
                return UI_HANDLED;
            n = ui_list_move(&w->list, in->wheel_steps, y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (in->release && !in->long_press && w->nrow && !w->todo)
                ui_push(&event_screen, NULL);
            return UI_HANDLED;
        default:
            return UI_IGNORED;
    }
}

static int list_tick(struct ui_screen *s)
{
    struct cal_work *w = s->priv;

    if (w->seq != xindex_seq())
    {
        ui_pop_request(s);
        return 0;
    }
    return w->nrow ? ui_list_tick(&w->list) : TIMEOUT_BLOCK;
}

static const struct ui_screen_ops day_screen = {
    .name    = "calendars-day",
    .enter   = day_enter,
    .leave   = NULL,
    .draw    = list_draw,
    .input   = list_input,
    .timeout = NULL,
    .tick    = list_tick,
};

static const struct ui_screen_ops todo_screen = {
    .name    = "calendars-todo",
    .enter   = todo_enter,
    .leave   = NULL,
    .draw    = list_draw,
    .input   = list_input,
    .timeout = NULL,
    .tick    = list_tick,
};

/* -- an event's page ------------------------------------------------------- */

/* Appends s as wrapped lines of kind k (the title in the list type, the
 * rest in the small type) */
static void add_text(struct cal_work *w, int *at, const char *s, int k)
{
    struct font *pf = font_get(k == L_TITLE ? ui_theme->font_ui
                                            : ui_theme->font_small);
    const int maxw = LCD_WIDTH - 2 * MARGIN;
    int len = (int)strlen(s), i = 0;

    if (*at + len >= DETAIL_MAX)
        len = DETAIL_MAX - 1 - *at;
    if (len <= 0)
        return;
    memcpy(w->text + *at, s, len);
    while (i < len && w->nlines < DLINES_MAX)
    {
        int j = i, wpx = 0, brk = -1;

        while (j < len && w->text[*at + j] != '\n')
        {
            ucschar_t ch;
            const unsigned char *p = (const unsigned char *)w->text + *at + j;
            int l = (int)(utf8decode(p, &ch) - p), cw_px;

            if (l <= 0 || j + l > len)
                break;
            cw_px = font_get_width(pf, ch);
            if (wpx + cw_px > maxw && j > i)
                break;
            if (ch == ' ')
                brk = j;
            wpx += cw_px;
            j += l;
        }
        if (j < len && w->text[*at + j] != '\n' && brk > i)
            j = brk;                    /* at the last space */
        w->lstart[w->nlines] = (uint16_t)(*at + i);
        w->llen[w->nlines] = (uint8_t)MIN(j - i, 255);
        w->lkind[w->nlines] = (uint8_t)k;
        w->nlines++;
        i = j;
        while (i < len && (w->text[*at + i] == ' ' || w->text[*at + i] == '\n'))
        {
            bool nl = w->text[*at + i] == '\n';

            i++;
            if (nl)
                break;
        }
    }
    *at += len;
}

static void blank_line(struct cal_work *w)
{
    if (w->nlines < DLINES_MAX)
    {
        w->lstart[w->nlines] = 0;
        w->llen[w->nlines] = 0;
        w->lkind[w->nlines++] = L_TEXT;
    }
}

static int event_enter(struct ui_screen *s, const void *arg)
{
    struct cal_work *w = cw;
    const struct xcal_rec *r;
    char line[64], span[SPAN_MAX];
    const char *title, *loc = "", *desc = "";
    int at = 0, fd, sel;
    uint32_t got;

    (void)arg;
    if (!w || !w->nrow)
        return -1;
    s->priv = w;
    sel = w->list.selected;
    r = &w->row[sel];
    w->nlines = w->top = 0;
    modclass_entry(MOD_CALENDARS);
    /* the title, then the location and the notes right after it (the
     * builder keeps them adjacent): from the cache, or one read */
    memset(span, 0, sizeof(span));
    got = MIN((uint32_t)sizeof(span) - 1,
              w->str_bytes > r->title_off ? w->str_bytes - r->title_off : 0);
    fd = win_open(w);
    if (w->whole)
        memcpy(span, w->cache + w->str_off + r->title_off, got);
    else if (fd < 0 || !read_at(fd, w->str_off + r->title_off, span, got))
        got = 0;
    if (fd >= 0)
        close(fd);
    title = span[0] ? span : w->rtitle[sel];
    if (r->loc_off && r->loc_off > r->title_off
        && r->loc_off - r->title_off < got)
    {
        loc = span + (r->loc_off - r->title_off);
        desc = loc + strlen(loc) + 1;
        if (desc >= span + got)
            desc = "";
    }
    add_text(w, &at, title, L_TITLE);
    fmt_day(line, sizeof(line), (int32_t)(r->start_rtc / DAY_S), true);
    add_text(w, &at, line, L_TEXT);
    if (r->end_rtc > r->start_rtc + DAY_S
        && (r->end_rtc - 1) / DAY_S != r->start_rtc / DAY_S)
    {
        /* several days: the last one too */
        char to[48];

        fmt_day(to, sizeof(to), (int32_t)((r->end_rtc - 1) / DAY_S), true);
        snprintf(line, sizeof(line), "\xe2\x80\x93 %s", to);
        add_text(w, &at, line, L_TEXT);
    }
    if (r->flags & XCAL_ALL_DAY)
        strlcpy(line, str(LANG_IPODUI_X_ALL_DAY), sizeof(line));
    else
    {
        fmt_time(line, sizeof(line), r->start_rtc);
        if (r->end_rtc > r->start_rtc)
        {
            char b[16];

            fmt_time(b, sizeof(b), r->end_rtc);
            strlcat(line, " \xe2\x80\x93 ", sizeof(line));
            strlcat(line, b, sizeof(line));
        }
    }
    add_text(w, &at, line, L_TEXT);
    if (*loc)
        add_text(w, &at, loc, L_DIM);
    if (*desc)
    {
        blank_line(w);
        add_text(w, &at, desc, L_TEXT);
    }
    w->rows = (LCD_HEIGHT - ui_theme->header_h - 2 * MARGIN) / PITCH_TEXT;
    s->title = w->ltitle;
    UPDATE_TRACE("xcal event lines=%d", w->nlines);
    return 0;
}

static void event_draw(struct ui_screen *s, unsigned dirty)
{
    struct cal_work *w = s->priv;
    const struct ui_theme *t = ui_theme;
    struct viewport vp;
    int i, y = MARGIN;

    if (!(dirty & (UI_DIRTY_BODY | UI_DIRTY_BAND)))
        return;
    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.font = t->font_small;
    vp.drawmode = DRMODE_SOLID;
    lcd_set_viewport(&vp);
    lcd_set_foreground(t->list_bg);
    lcd_fillrect(0, 0, vp.width, vp.height);
    lcd_set_drawmode(DRMODE_FG);
    for (i = w->top; i < w->nlines && y < vp.height - 4; i++)
    {
        char buf[256];
        int k = w->lkind[i], pitch = k == L_TITLE ? PITCH_TITLE : PITCH_TEXT;

        memcpy(buf, w->text + w->lstart[i], w->llen[i]);
        buf[w->llen[i]] = '\0';
        vp.font = k == L_TITLE ? t->font_ui : t->font_small;
        lcd_set_foreground(k == L_DIM ? t->list_fg_dim : t->list_fg);
        lcd_putsxy(MARGIN, y, (const unsigned char *)buf);
        y += pitch;
        if (k == L_TITLE && i + 1 < w->nlines && w->lkind[i + 1] != L_TITLE)
            y += 4;                     /* a gap under the title */
    }
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_viewport(NULL);
}

static enum ui_result event_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    struct cal_work *w = s->priv;
    int top = w->top, last = MAX(0, w->nlines - w->rows);

    if (in->key != UI_KEY_WHEEL)
        return in->key == UI_KEY_MENU ? UI_IGNORED : UI_HANDLED;
    top += in->wheel_steps;
    top = top < 0 ? 0 : top > last ? last : top;
    if (top != w->top)
    {
        w->top = top;
        ui_invalidate(UI_DIRTY_BODY);
    }
    return UI_HANDLED;
}

static int event_tick(struct ui_screen *s)
{
    struct cal_work *w = s->priv;

    if (w->seq != xindex_seq())
        ui_pop_request(s);
    return TIMEOUT_BLOCK;
}

static const struct ui_screen_ops event_screen = {
    .name    = "calendars-event",
    .enter   = event_enter,
    .leave   = NULL,
    .draw    = event_draw,
    .input   = event_input,
    .timeout = NULL,
    .tick    = event_tick,
};

/* -- the root: the split list ---------------------------------------------- */

static const int filt[1 + XCAL_MAX_CALS] = {
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};

/* menu.c calls it on enter and before every full draw */
static void root_refresh(void)
{
    const struct ui_preview_ops *pv = &ui_preview_x_calendars;
    struct cal_work *w = cw;
    int i, n = 0, ncal = w->ok ? meta(w)->ncal : 0;

    w->menu.title = str(LANG_IPODUI_X_CALENDARS);
    w->item[n++] = (struct ui_menu_item){ str(LANG_IPODUI_X_ALL_CALENDARS),
        &month_screen, &filt[0], NULL, NULL, pv, NULL };
    /* each calendar, when there are several */
    for (i = 0; ncal > 1 && i < ncal; i++)
        w->item[n++] = (struct ui_menu_item){ meta(w)->name[i],
            &month_screen, &filt[1 + i], NULL, NULL, pv, NULL };
    w->item[n++] = (struct ui_menu_item){ str(LANG_IPODUI_X_TODOS),
        &todo_screen, NULL, NULL, NULL, pv, NULL };
    w->menu.count = n;
}

static int root_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
    modclass_entry(MOD_CALENDARS);
    cw = mod_work_alloc(MOD_CALENDARS, CAL_WORK);
    if (!cw)
    {
        UPDATE_TRACE("xcal work refused bytes=%d", CAL_WORK);
        return -1;
    }
    memset(cw, 0, sizeof(*cw));
    load_index(cw);
    cw->menu = (struct ui_menu){ NULL, cw->item, 0, true, false,
                                 root_refresh };
    root_refresh();
    return ui_menu_screen.enter(s, &cw->menu);
}

static void root_leave(struct ui_screen *s)
{
    ui_menu_screen.leave(s);
    cw = NULL;                          /* the block goes with the module */
}

static void root_draw(struct ui_screen *s, unsigned dirty)
{
    ui_menu_screen.draw(s, dirty);
}

static enum ui_result root_input(struct ui_screen *s,
                                 const struct ui_input *in)
{
    return ui_menu_screen.input(s, in);
}

static int root_tick(struct ui_screen *s)
{
    if (cw && cw->seq != xindex_seq() && !xindex_busy())
    {
        /* a new index (the post-USB moment): read it again */
        modclass_entry(MOD_CALENDARS);
        load_index(cw);
        root_refresh();
        ui_invalidate(UI_DIRTY_ALL);
    }
    return ui_menu_screen.tick(s);
}

const struct ui_screen_ops ui_x_calendars_screen = {
    .name    = "calendars",
    .enter   = root_enter,
    .leave   = root_leave,
    .draw    = root_draw,
    .input   = root_input,
    .timeout = NULL,
    .tick    = root_tick,
};
