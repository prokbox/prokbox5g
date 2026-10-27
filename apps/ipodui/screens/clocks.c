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

/* Extras > Clocks (P12 W1-C; docs/phase12-brief.md Q13, Q27; owner photos
 * photo 1193 clocks.jpg by day, VIDEO-2 A3 clocks-reveal-08 by night).
 *
 * The page has no header. Each clock is a full-width blue band, 80 px
 * high: the city at the left (white, Bold 15), a round analog face in the
 * middle (light by day, dark by night, from 06:00 to 18:00 local), the
 * time and "Today" / "Yesterday" / "Tomorrow" right-aligned at the right.
 * The bands stack from the top; under the last one the world map shows
 * (the Extras backdrop: our western half beside the panel's own eastern
 * half, so the reveal's map stays put). Up to six clocks: three bands
 * fill the screen and the rest scroll with the selection. With two or
 * more, the selected band is the blue one and the others a dark slate
 * (⚠️ C1 not captured).
 *
 * Keys: the wheel moves the selection (it is this screen's input, so it
 * is never the volume here, M6); SELECT opens the Clocks menu (Add, Edit,
 * Delete, Cancel: a full-width list like the hold menu); MENU backs out;
 * PLAY is the global play/pause. Add and Edit go through the resident
 * region -> city picker (tzpick.h), then straight back here.
 *
 * Times: the home clock follows the RTC; other cities through
 * tz_city_time() (their standard offset and DST rule, Q27).
 *
 * Class A (2.1): state (and the menu's list) in the module's one working
 * block (mod_work_alloc(), 1 KB, freed with the module's last screen); no storage
 * while lit (the list is a setting, "ipodui clocks", saved by
 * settings_save() at exit only if it changed, which Rockbox defers to the
 * disk's next wake); one band redraw a minute, unboosted; no threads.
 *
 * The header: none (UI_HEADER_NONE, P12 W2-W): the main loop draws and
 * pushes no header rows over the page.
 *
 * The reveal (UI_T_REVEAL, ui_main.c): reveal() draws the bands with
 * their left edge at x, clipped at the right; the backdrop under the rest
 * is the resident ui_extras_page_backdrop(), so the pop can draw it after
 * this module has gone. */

#ifndef IPODUI_IN_MODULE
#define IPODUI_IN_MODULE
#endif
/* not ownable (file): a full-screen page or takeover; the drift panel was settled on the way in (ui_panel_hide(), or the takeover's own ui_fb_settle()), and a full lcd_update() releases it (bcmjob_owned_hook) */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "settings.h"
#include "ipodui/fe_be.h"   /* P19 S1: settings saves cross */
#include "fixedpoint.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/header.h"
#include "ipodui/list.h"
#include "ipodui/usbscreen.h"
#include "ipodui/tzpick.h"
#include "ipodui/modload.h"
#include "ipodui/screens/panels.h"
#include "ipodui/screens/screens.h"
#include "ipodui/screens/clocks.h"

#if LCD_PIXELFORMAT != RGB565
#error "clocks.c composes RGB565"
#endif

/* -- layout (from photo 1193, 320 x 240) ------------------------------------ */

#define BAND_H          80
#define BANDS_VIS       (LCD_HEIGHT / BAND_H)   /* 3 */
#define TEXT_L          8               /* the city's left edge */
#define TEXT_R          296             /* the time's right edge */
#define CITY_BASE       45              /* baselines, from the band top */
#define TIME_BASE       41
#define DAY_BASE        59
#define FACE_D          72              /* the face, centred at x 160 */
#define FACE_CY         40
#define FACE_NUM_R      27              /* numeral centres */
#define FACE_HOUR_L     15              /* hands, px */
#define FACE_MIN_L      24
#define CITY_MAX_W      (LCD_WIDTH / 2 - FACE_D / 2 - TEXT_L - 8)
#define TIME_MAX_W      (TEXT_R - LCD_WIDTH / 2 - FACE_D / 2 - 8)

#define C_WHITE         LCD_RGBPACK(0xff, 0xff, 0xff)

/* the band: a blue ramp (the photo's raw ~#0F5ECC in the middle), a dark
 * line at its foot; unselected bands (two clocks or more) in slate */
static const unsigned char band_blue[3][3] = {
    { 0x48, 0x8e, 0xf0 }, { 0x14, 0x5c, 0xd0 }, { 0x0a, 0x38, 0x8c } };
static const unsigned char band_slate[3][3] = {
    { 0x5e, 0x66, 0x74 }, { 0x34, 0x3a, 0x45 }, { 0x1a, 0x1d, 0x24 } };

/* faces: fill top, fill bottom, rim, numerals, hands, hub pin */
static const unsigned short face_day[6] = {
    LCD_RGBPACK(0xf6, 0xf6, 0xfa), LCD_RGBPACK(0xd6, 0xd9, 0xe4),
    LCD_RGBPACK(0x50, 0x56, 0x64), LCD_RGBPACK(0x30, 0x32, 0x3a),
    LCD_RGBPACK(0x1c, 0x1c, 0x20), LCD_RGBPACK(0xe8, 0xe8, 0xec) };
static const unsigned short face_night[6] = {
    LCD_RGBPACK(0x2c, 0x2e, 0x36), LCD_RGBPACK(0x0e, 0x0f, 0x12),
    LCD_RGBPACK(0x9c, 0xa2, 0xb0), LCD_RGBPACK(0xe6, 0xe8, 0xee),
    LCD_RGBPACK(0xf2, 0xf2, 0xf6), LCD_RGBPACK(0x1c, 0x1c, 0x20) };

/* -- state ------------------------------------------------------------------ */

enum { PICK_NONE, PICK_ADD, PICK_EDIT };

/* the Clocks menu (below): Add, Edit, Delete, Cancel */
enum { CM_ADD, CM_EDIT, CM_DELETE, CM_CANCEL, CM__N };
static const short cm_lang[CM__N] = {
    LANG_IPODUI_X_ADD, LANG_IPODUI_X_EDIT, LANG_IPODUI_X_DELETE, LANG_CANCEL_0 };

struct cmenu {
    struct ui_list list;
    unsigned char item[CM__N];
};

struct clocks {
    unsigned char n, sel, top;          /* clocks, selected, first shown */
    unsigned char city[CLOCKS_MAX];
    unsigned char changed;              /* saved at leave */
    unsigned char pick;                 /* the picker's job (PICK_*) */
    unsigned char close_menu;           /* the menu leaves when back on top */
    unsigned char redraw;               /* band slots tick() redraws */
    int minute;                         /* the minute last drawn */
    struct cmenu menu;                  /* the Clocks menu's list */
};
/* the module's one working block (Class A, mod_work_alloc(): 1 KB) */
_Static_assert(sizeof(struct clocks) <= 1024, "clocks: work block bound");

/* the open page, for the menu's and the picker's callbacks (one page is
 * open at a time: it is a leaf of the Extras list) */
static struct clocks *cs;

static int home_city(void)
{
    int c = global_settings.ipodui_tz_city;
    return (c >= 0 && c < tzp_city_count()) ? c : UI_TZ_CITY_DEFAULT;
}

/* "3,42,79" -> the list; nothing valid: one clock, the home city */
static void list_load(struct clocks *st)
{
    const char *p = (const char *)global_settings.ipodui_clocks;

    st->n = 0;
    while (*p && st->n < CLOCKS_MAX)
    {
        char *end;
        long v = strtol(p, &end, 10);

        if (end == p)
            break;
        if (v >= 0 && v < tzp_city_count())
            st->city[st->n++] = v;
        p = *end == ',' ? end + 1 : end;
    }
    if (st->n == 0)
    {
        st->city[0] = home_city();
        st->n = 1;
    }
}

static void list_save(const struct clocks *st)
{
    char buf[sizeof(global_settings.ipodui_clocks)];
    int i, k = 0;

    buf[0] = '\0';
    for (i = 0; i < st->n; i++)
        k += snprintf(buf + k, sizeof(buf) - k, i ? ",%d" : "%d",
                      st->city[i]);
    strlcpy((char *)global_settings.ipodui_clocks, buf,
            sizeof(global_settings.ipodui_clocks));
    fe_settings_save();                    /* written at the disk's next wake */
    UPDATE_TRACE("clocks save list=%s", buf);
}

/* -- drawing ---------------------------------------------------------------- */

static inline unsigned blend32(unsigned bg, unsigned fg, unsigned a)
{
    uint32_t b = (bg | bg << 16) & 0x07e0f81f;
    uint32_t f = (fg | fg << 16) & 0x07e0f81f;
    uint32_t r = ((b * (32 - a) + f * a) >> 5) & 0x07e0f81f;
    return (r | r >> 16) & 0xffff;
}

static unsigned ramp(const unsigned char (*stop)[3], int y, int h)
{
    const int m = h / 2;
    const unsigned char *a = stop[y >= m], *b = stop[(y >= m) + 1];
    int t = y < m ? y * 256 / m : (y - m) * 256 / (h - m);

    return LCD_RGBPACK(a[0] + ((b[0] - a[0]) * t >> 8),
                       a[1] + ((b[1] - a[1]) * t >> 8),
                       a[2] + ((b[2] - a[2]) * t >> 8));
}

static int fb_stride(void)
{
    return FBADDR(0, 1) - FBADDR(0, 0);
}

/* The face, its rim and numerals: a disc of FACE_D px at (x0, y0),
 * anti-aliased against what is under it (the band) */
static void draw_face(int x0, int y0, const unsigned short *pal,
                      int hour, int min)
{
    const struct ui_theme *t = ui_theme;
    const int d = FACE_D, r2o = d * d, stride = fb_stride();
    const int xe = MIN(d, LCD_WIDTH - x0);  /* the reveal's right clip */
    int x, y, k;

    if (xe <= 0)
        return;
    for (y = 0; y < d; y++)
    {
        fb_data *row = FBADDR(x0, y0 + y);
        unsigned fill = blend32(pal[0], pal[1], y * 32 / d);

        for (x = 0; x < xe; x++)
        {
            /* distances in half pixels from the centre */
            int dx = 2 * x + 1 - d, dy = 2 * y + 1 - d;
            int q = dx * dx + dy * dy, a;

            if (q >= r2o + 2 * d)
                continue;               /* outside, past the soft edge */
            a = q <= r2o - 2 * d ? 32 : (r2o + 2 * d - q) * 32 / (4 * d);
            /* the rim: the outer 2 px */
            if (q > (d - 4) * (d - 4))
                row[x] = blend32(row[x], pal[2], a);
            else if (q > (d - 6) * (d - 6))
                row[x] = blend32(pal[2], fill,
                                 (unsigned)((d - 4) * (d - 4) - q) * 32 /
                                 ((d - 4) * (d - 4) - (d - 6) * (d - 6)));
            else
                row[x] = fill;
        }
    }
    lcd_setfont(t->font_small);
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(pal[3]);
    for (k = 1; k <= 12; k++)
    {
        char n[3];
        int a = k * 30;

        snprintf(n, sizeof(n), "%d", k);
        ui_paint_text_centre(x0 + d / 2 + (fp14_sin(a) * FACE_NUM_R >> 14),
                             y0 + d / 2 - (fp14_cos(a) * FACE_NUM_R >> 14)
                             + 4, 16, n);
    }
    ui_clock_hands(FBADDR(x0 + d / 2, y0 + d / 2), stride, hour, min,
                   FACE_HOUR_L, FACE_MIN_L, pal[4], pal[5],
                   LCD_WIDTH - 1 - (x0 + d / 2));
}

/* left-aligned text clipped to max_w ("..." when cut) */
static void text_left(int x, int base, int max_w, const char *s)
{
    int w = 0, h;

    lcd_getstringsize(s, &w, &h);
    ui_paint_text_centre(x + MIN(w, max_w) / 2, base, max_w, s);
}

static void text_right(int xr, int base, int max_w, const char *s)
{
    int w = 0, h;

    lcd_getstringsize(s, &w, &h);
    w = MIN(w, max_w);
    ui_paint_text_centre(xr - w + w / 2, base, max_w, s);
}

/* the clock's local time (home: the RTC itself) */
static void city_now(int city, const struct tm *home, struct tm *out)
{
    int hc = home_city();

    tz_rtc_split(city == hc ? tz_rtc_make(home)
                            : tz_city_time(city, hc, tz_rtc_make(home)),
                 out);
}

/* band i at row y0, its left edge at x (0 at rest; the reveal moves it) */
static void draw_band(const struct clocks *st, int i, int x, int y0,
                      const struct tm *home)
{
    const struct ui_theme *t = ui_theme;
    const unsigned char (*bg)[3] = st->n > 1 && i != st->sel ? band_slate
                                                             : band_blue;
    struct tm tm;
    char buf[24];
    int y, dd, day;

    if (x >= LCD_WIDTH)
        return;
    lcd_set_drawmode(DRMODE_SOLID);
    for (y = 0; y < BAND_H - 1; y++)
    {
        lcd_set_foreground(ramp(bg, y, BAND_H - 1));
        lcd_hline(x, LCD_WIDTH - 1, y0 + y);
    }
    lcd_set_foreground(LCD_RGBPACK(bg[2][0], bg[2][1], bg[2][2]));
    lcd_hline(x, LCD_WIDTH - 1, y0 + BAND_H - 1);

    city_now(st->city[i], home, &tm);
    draw_face(x + LCD_WIDTH / 2 - FACE_D / 2, y0 + FACE_CY - FACE_D / 2,
              tm.tm_hour >= 6 && tm.tm_hour < 18 ? face_day : face_night,
              tm.tm_hour, tm.tm_min);

    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(C_WHITE);
    lcd_setfont(t->font_ui);
    text_left(x + TEXT_L, y0 + CITY_BASE, CITY_MAX_W,
              str(tzp_city_lang(st->city[i])));
    if (global_settings.timeformat)             /* 12 hour clock */
        snprintf(buf, sizeof(buf), "%d:%02d %s",
                 tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min,
                 str(tm.tm_hour < 12 ? LANG_IPODUI_AM : LANG_IPODUI_PM));
    else
        snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    text_right(x + TEXT_R, y0 + TIME_BASE, TIME_MAX_W, buf);

    /* the day against the home clock's */
    dd = tm.tm_year != home->tm_year ? tm.tm_year - home->tm_year
                                     : tm.tm_yday - home->tm_yday;
    day = dd > 0 ? LANG_IPODUI_X_TOMORROW
                 : dd < 0 ? LANG_IPODUI_X_YESTERDAY : LANG_IPODUI_X_TODAY;
    lcd_setfont(t->font_title);
    text_right(x + TEXT_R, y0 + DAY_BASE, TIME_MAX_W, str(day));
}

/* the rows of the bands shown (the reveal's moving part) */
static int bands_h(const struct clocks *st)
{
    return MIN(st->n - st->top, BANDS_VIS) * BAND_H;
}

/* rows [y, y + h): the bands (left edge at x) and, at rest (x 0), the
 * backdrop under the last one */
static void draw_rows_at(struct clocks *st, int x, int y, int h)
{
    struct tm home = *ui_panel_time();
    int i, end = y + h, map_y = bands_h(st);

    lcd_set_viewport(NULL);
    for (i = 0; i * BAND_H < map_y; i++)
    {
        int by = i * BAND_H;
        if (by < end && by + BAND_H > y)
            draw_band(st, st->top + i, x, by, &home);
    }
    if (x == 0 && map_y < LCD_HEIGHT && end > map_y)
    {
        int y0 = MAX(y, map_y);

        ui_extras_page_backdrop(y0, end - y0, LCD_WIDTH);
    }
    st->minute = home.tm_hour * 60 + home.tm_min;
    lcd_set_drawmode(DRMODE_SOLID);
}

static void draw_rows(struct clocks *st, int y, int h)
{
    draw_rows_at(st, 0, y, h);
}

/* -- the page --------------------------------------------------------------- */

static void scroll_to_sel(struct clocks *st)
{
    if (st->sel < st->top)
        st->top = st->sel;
    else if (st->sel >= st->top + BANDS_VIS)
        st->top = st->sel - BANDS_VIS + 1;
}

static int clocks_enter(struct ui_screen *s, const void *arg)
{
    struct clocks *st = mod_work_alloc(MOD_CLOCKS, sizeof(*st));

    (void)arg;
    if (!st)
        return -1;                      /* logged by the loader */
    s->priv = st;
    s->title = str(LANG_IPODUI_X_CLOCKS);
    s->header_w = UI_HEADER_NONE;   /* no header (header.h) */
    list_load(st);
    cs = st;
    UPDATE_TRACE("clocks open n=%d", st->n);
    return 0;
}

static void clocks_leave(struct ui_screen *s)
{
    struct clocks *st = s->priv;

    if (st->changed)
        list_save(st);
    cs = NULL;
}

static void clocks_draw(struct ui_screen *s, unsigned dirty)
{
    struct clocks *st = s->priv;

    if (dirty & UI_DIRTY_BODY)
        draw_rows(st, 0, LCD_HEIGHT);
    else if (dirty & UI_DIRTY_BAND)
    {
        int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            draw_rows(st, y[i], h[i]);
    }
}

extern const struct ui_screen_ops clocks_menu_screen;

static enum ui_result clocks_input(struct ui_screen *s,
                                   const struct ui_input *in)
{
    struct clocks *st = s->priv;
    int old, top;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            old = st->sel;
            top = st->top;
            st->sel = MAX(0, MIN(st->n - 1, st->sel + in->wheel_steps));
            scroll_to_sel(st);
            /* tick() draws and pushes each band on its own (80 px each,
             * never one 160-240 px band) */
            if (st->top != top)
                st->redraw = (1 << BANDS_VIS) - 1;
            else if (st->sel != old)
                st->redraw |= 1 << (old - st->top) | 1 << (st->sel - st->top);
            return UI_HANDLED;      /* the page's input: never the volume */
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            ui_push(&clocks_menu_screen, NULL);
            return UI_HANDLED;
        default:
            return UI_IGNORED;      /* MENU backs out; PLAY is global */
    }
}

static int clocks_tick(struct ui_screen *s)
{
    struct clocks *st = s->priv;
    struct tm *tm = ui_panel_time();
    int i;

    /* a new minute: the bands again (the map does not change) */
    if (tm->tm_hour * 60 + tm->tm_min != st->minute)
        st->redraw = (1 << BANDS_VIS) - 1;
    for (i = 0; i < MIN(st->n - st->top, BANDS_VIS); i++)
        if (st->redraw & (1 << i))
        {
            draw_rows(st, i * BAND_H, BAND_H);
            ui_lcd_push("band", 0, i * BAND_H, LCD_WIDTH, BAND_H);
        }
    st->redraw = 0;
    return (60 - MIN(tm->tm_sec, 59)) * HZ;
}

/* The reveal (screen_stack.h): the bands with their left edge at x */
static int clocks_reveal(struct ui_screen *s, int x)
{
    struct clocks *st = s->priv;

    if (x < LCD_WIDTH)
        draw_rows_at(st, MAX(x, 0), 0, bands_h(st));
    return bands_h(st);
}

const struct ui_screen_ops ui_clocks_screen = {
    .name    = "clocks",
    .enter   = clocks_enter,
    .leave   = clocks_leave,
    .draw    = clocks_draw,
    .input   = clocks_input,
    .timeout = NULL,                    /* tick() keeps the minute */
    .tick    = clocks_tick,
    .reveal  = clocks_reveal,
};

/* -- the Clocks menu: Add, Edit, Delete, Cancel ----------------------------- */


static int pick_current(void *arg)
{
    (void)arg;
    return cs && cs->pick == PICK_EDIT ? cs->city[cs->sel] : -1;
}

static int pick_done(int city, void *arg)
{
    (void)arg;
    if (cs)
    {
        if (cs->pick == PICK_ADD && cs->n < CLOCKS_MAX)
        {
            cs->city[cs->n] = city;
            cs->sel = cs->n++;
            scroll_to_sel(cs);
        }
        else if (cs->pick == PICK_EDIT)
            cs->city[cs->sel] = city;
        UPDATE_TRACE("clocks %s city=%d n=%d",
                     cs->pick == PICK_ADD ? "add" : "edit", city, cs->n);
        cs->pick = PICK_NONE;
        cs->changed = 1;
        cs->close_menu = 1;             /* and the menu under the lists */
    }
    return 2;                           /* the city and region lists */
}

static const struct tzpick clocks_pick = {
    LANG_IPODUI_X_CLOCKS, pick_current, pick_done, NULL };

static const char *cm_label(int row, void *data, char *buf, size_t len)
{
    const struct cmenu *m = data;

    (void)buf; (void)len;
    return str(cm_lang[m->item[row]]);
}

static int cm_enter(struct ui_screen *s, const void *arg)
{
    const struct ui_theme *t = ui_theme;
    struct cmenu *m = cs ? &cs->menu : NULL;
    struct viewport vp;
    int n = 0;

    (void)arg;
    if (!m)
        return -1;
    s->priv = m;
    s->title = str(LANG_IPODUI_X_CLOCKS);
    if (cs->n < CLOCKS_MAX)
        m->item[n++] = CM_ADD;
    m->item[n++] = CM_EDIT;
    if (cs->n > 1)
        m->item[n++] = CM_DELETE;
    m->item[n++] = CM_CANCEL;
    memset(&vp, 0, sizeof(vp));
    vp.y = t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - t->header_h;
    ui_list_init(&m->list, &vp, n, cm_label, m);
    UPDATE_TRACE("clocks menu n=%d items=%d", cs->n, n);
    return 0;
}

static void cm_draw(struct ui_screen *s, unsigned dirty)
{
    struct cmenu *m = s->priv;
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    if (dirty & UI_DIRTY_BODY)
        ui_list_draw(&m->list, m->list.vp.y, m->list.vp.height);
    else if (dirty & UI_DIRTY_BAND)
    {
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            ui_list_draw(&m->list, y[i], h[i]);
    }
}

static enum ui_result cm_input(struct ui_screen *s, const struct ui_input *in)
{
    struct cmenu *m = s->priv;
    int y[2], h[2], i, n;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            n = ui_list_move(&m->list, in->wheel_steps, y, h);
            for (i = 0; i < n; i++)
                ui_invalidate_band(y[i], h[i]);
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press || !cs)
                return UI_HANDLED;
            switch (m->item[m->list.selected])
            {
                case CM_ADD:
                case CM_EDIT:
                    cs->pick = m->item[m->list.selected] == CM_ADD
                               ? PICK_ADD : PICK_EDIT;
                    tzpick_push(&clocks_pick);
                    return UI_HANDLED;
                case CM_DELETE:
                    if (cs->n > 1)
                    {
                        UPDATE_TRACE("clocks delete city=%d n=%d",
                                     cs->city[cs->sel], cs->n - 1);
                        memmove(&cs->city[cs->sel], &cs->city[cs->sel + 1],
                                cs->n - cs->sel - 1);
                        cs->n--;
                        cs->sel = MIN(cs->sel, cs->n - 1);
                        cs->top = MIN(cs->top,
                                      MAX(0, cs->n - BANDS_VIS));
                        scroll_to_sel(cs);
                        cs->changed = 1;
                    }
                    return UI_POP;
                default:
                    return UI_POP;          /* Cancel */
            }
        default:
            return UI_IGNORED;              /* MENU pops: Cancel */
    }
}

static int cm_tick(struct ui_screen *s)
{
    struct cmenu *m = s->priv;

    if (cs && cs->close_menu)
    {
        cs->close_menu = 0;
        ui_pop_request(s);
        return TIMEOUT_BLOCK;
    }
    return ui_list_tick(&m->list);
}

const struct ui_screen_ops clocks_menu_screen = {
    .name    = "clocks-menu",
    .enter   = cm_enter,
    .leave   = NULL,                    /* its list: the page's work block */
    .draw    = cm_draw,
    .input   = cm_input,
    .timeout = NULL,
    .tick    = cm_tick,
};
