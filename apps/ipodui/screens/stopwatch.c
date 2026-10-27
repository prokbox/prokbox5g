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
 * iPod UI shell: Extras > Stopwatch (P12, docs/phase12-brief.md 3.4, spec
 * extras.md, owner photo 1202). A cold Class A module: the split list
 * (Resume, New Timer, Clear Logs, the logs), the dark timer screen (big
 * digits at 10 fps, the lap list) and a log's detail; owner W1-S
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
/* not ownable (file): a full-screen page or takeover; the drift panel was settled on the way in (ui_panel_hide(), or the takeover's own ui_fb_settle()), and a full lcd_update() releases it (bcmjob_owned_hook) */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "settings.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/menu.h"
#include "ipodui/modload.h"
#include "ipodui/header.h"
#include "ipodui/preview.h"
#include "ipodui/usbscreen.h"
#include "ipodui/screen_stack.h"
#include "ipodui/stopwatch.h"
#include "ipodui/caldigits.h"
#include "ipodui/screens/screens.h"

/* Behaviour (the 7G's): Extras > Stopwatch lists Resume (only while a
 * timing exists), New Timer, Clear Logs, then the logs, newest first, each
 * its date (M/D/YY) and, blue at the right, its time. The timer screen:
 * PLAY starts, stops and resumes; SELECT takes a lap while running; MENU
 * leaves, and the timing keeps running (Resume brings it back). A stop
 * writes the log (stopwatch.dat, one safe replace); nothing else here
 * touches storage, and the screen never boosts. */

#define TM_ROWS     6               /* lap rows in view */
#define TM_PITCH    24
#define DIG_Y       34
#define DIG_H       CALDIG_H
#define LIST_Y      78
#define LIST_H      (TM_ROWS * TM_PITCH)
#define DIG_ADV     22              /* digit advance (cell 21 + 1) */
#define SEP_ADV     11              /* ':' and '.' */

/* one working block per visit of the list (and the screens above it) */
struct sw_wk {
    struct ui_menu menu;                    /* the list */
    struct ui_menu dmenu;                   /* a log's detail */
    /* The list's rows and a log's detail rows share their storage: the
     * list is covered while a detail is up and menu.c rebuilds it
     * (list_refresh) before it draws again */
    union {
        struct {
            struct ui_menu_item item[3 + SW_LOGS_MAX];
            char lbl[SW_LOGS_MAX][20];      /* "12/5/20" SEP "16:36" */
        } l;
        struct {
            struct ui_menu_item item[4 + SW_LAST_LAPS];
            char lbl[4 + SW_LAST_LAPS][26];
        } d;
    } u;
    char dtitle[10];
    struct sw_logs_file logs;
    /* the timer screen */
    int off;                                /* first lap row in view */
    uint32_t shown_ds;                      /* tenths on screen */
    bool redo_digits, redo_list;            /* an input changed them */
};
static struct sw_wk *wk;                    /* per visit (.bss) */

#define WORK_BYTES  sizeof(struct sw_wk)
#ifndef SIMULATOR                       /* 32-bit pointers: the target's size */
_Static_assert(sizeof(struct sw_wk) <= 3072, "stopwatch: the module's work block");
#endif

/* -- formatting ----------------------------------------------------------- */

static void fmt_ms(char *buf, size_t len, uint32_t ms)
{
    uint32_t ds = ms / 100, s = ds / 10, h = s / 3600;

    if (h > 99)
        h = 99;
    if (h)
        snprintf(buf, len, "%u:%02u:%02u.%u", (unsigned)h,
                 (unsigned)(s / 60 % 60), (unsigned)(s % 60),
                 (unsigned)(ds % 10));
    else
        snprintf(buf, len, "%u:%02u.%u", (unsigned)(s / 60),
                 (unsigned)(s % 60), (unsigned)(ds % 10));
}

static void fmt_date(char *buf, size_t len, uint32_t rtc)
{
    int y, mo, d, h, mi;

    ui_sw_split(rtc, &y, &mo, &d, &h, &mi);
    snprintf(buf, len, "%d/%d/%02d", mo, d, y % 100);
}

static void fmt_clock(char *buf, size_t len, uint32_t rtc)
{
    int y, mo, d, h, mi;

    ui_sw_split(rtc, &y, &mo, &d, &h, &mi);
    if (global_settings.timeformat)             /* 12 hour clock */
        snprintf(buf, len, "%d:%02d %s", h % 12 ? h % 12 : 12, mi,
                 str(h < 12 ? LANG_IPODUI_AM : LANG_IPODUI_PM));
    else
        snprintf(buf, len, "%02d:%02d", h, mi);
}

/* -- the timer screen ------------------------------------------------------ */

static void paint_glyph(int x, int y, int d)
{
    const unsigned char *rle = caldig_rle + caldig_off[d];
    int px = 0, py = 0, w = caldig_w[d];

    while (py < CALDIG_H)
    {
        unsigned b = *rle++, a = b & 15;
        int run = (b >> 4) + 1;

        while (run-- > 0)
        {
            if (a)
            {
                fb_data *p = FBADDR(x + px, y + py);

                *p = ui_paint_blend(*p, LCD_RGBPACK(0xff, 0xff, 0xff), a);
            }
            if (++px == w)
            {
                px = 0;
                py++;
            }
        }
    }
}

static void paint_dots(int x, int y, bool colon)
{
    int k;

    for (k = 0; k < (colon ? 2 : 1); k++)
    {
        int dy = colon ? y + (k ? CALDIG_H * 3 / 4 : CALDIG_H / 4) - 3
                       : y + CALDIG_H - 5;

        lcd_set_foreground(LCD_RGBPACK(0xff, 0xff, 0xff));
        lcd_fillrect(x + 2, dy, 5, 5);
    }
}

/* The digits band: the body's gradient behind it, then the time centred */
static void draw_digits(uint32_t ms)
{
    char buf[16];
    const char *c;
    int w = 0, x;

    fmt_ms(buf, sizeof(buf), ms);
    for (c = buf; *c; c++)
        w += (*c == ':' || *c == '.') ? SEP_ADV : DIG_ADV;
    lcd_set_viewport(NULL);
    ui_dark_body(DIG_Y, DIG_H);
    x = (LCD_WIDTH - w) / 2;
    for (c = buf; *c; c++)
    {
        if (*c == ':' || *c == '.')
        {
            paint_dots(x, DIG_Y, *c == ':');
            x += SEP_ADV;
        }
        else
        {
            paint_glyph(x, DIG_Y, *c - '0');
            x += DIG_ADV;
        }
    }
}

/* Lap rows, newest first: row 0 is the open lap */
static int lap_rows(void)
{
    const struct sw_state *s = ui_sw_state();

    return ui_sw_active() ? s->nlaps + 1 : 0;
}

static void draw_lap_row(int r, uint32_t now_ms)
{
    const struct ui_theme *t = ui_theme;
    const struct sw_state *s = ui_sw_state();
    int y = LIST_Y + (r - wk->off) * TM_PITCH, n, tw, th, i;
    uint32_t len = 0, sum = 0;
    char lab[24], tim[16];

    if (r - wk->off < 0 || r - wk->off >= TM_ROWS || r >= lap_rows())
        return;
    for (i = 0; i < s->nlaps; i++)
        sum += s->lap_ms[i];
    if (r == 0)
    {
        n = s->nlaps + 1;
        len = now_ms > sum ? now_ms - sum : 0;
    }
    else
    {
        n = s->nlaps - r + 1;
        len = s->lap_ms[n - 1];
    }
    snprintf(lab, sizeof(lab), str(LANG_IPODUI_X_LAP_N), n);
    fmt_ms(tim, sizeof(tim), len);
    lcd_set_viewport(NULL);
    ui_dark_body(y, TM_PITCH);
    lcd_setfont(t->font_ui);
    lcd_set_drawmode(DRMODE_FG);
    font_getstringsize((unsigned char *)tim, &tw, &th, t->font_ui);
    lcd_set_foreground(LCD_RGBPACK(0xee, 0xee, 0xee));
    lcd_putsxy(14, y + (TM_PITCH - th) / 2, (unsigned char *)lab);
    lcd_set_foreground(LCD_RGBPACK(0x7f, 0xb8, 0xff));
    lcd_putsxy(LCD_WIDTH - 14 - tw, y + (TM_PITCH - th) / 2,
               (unsigned char *)tim);
    lcd_set_foreground(LCD_RGBPACK(0x3a, 0x3f, 0x48));
    lcd_hline(0, LCD_WIDTH - 1, y + TM_PITCH - 1);
    lcd_set_drawmode(DRMODE_SOLID);
}

static void draw_list(int y, int h, uint32_t now_ms)
{
    int r;

    for (r = wk->off; r < wk->off + TM_ROWS && r < lap_rows(); r++)
    {
        int ry = LIST_Y + (r - wk->off) * TM_PITCH;

        if (ry < y + h && ry + TM_PITCH > y)
            draw_lap_row(r, now_ms);
    }
}

static int timer_enter(struct ui_screen *s, const void *arg)
{
    if (!wk)
        return -1;
    if (arg && *(const char *)arg == 'n')       /* New Timer */
        ui_sw_new(&wk->logs);
    s->title = str(LANG_IPODUI_X_STOPWATCH);
    s->header_w = UI_HEADER_DARK;
    wk->off = 0;
    wk->redo_digits = wk->redo_list = false;
    UPDATE_TRACE("stopwatch timer %c run=%d",
                 arg ? *(const char *)arg : '-', (int)ui_sw_running());
    return 0;
}

static void timer_draw(struct ui_screen *s, unsigned dirty)
{
    uint32_t ms;

    (void)s;
    if (!(dirty & UI_DIRTY_BODY))
        return;
    ms = ui_sw_now_ms(true);                    /* a redraw reads the RTC */
    lcd_set_viewport(NULL);
    ui_dark_body(UI_DARK_HDR_H, LCD_HEIGHT - UI_DARK_HDR_H);
    draw_digits(ms);
    lcd_set_foreground(LCD_RGBPACK(0x3a, 0x3f, 0x48));
    lcd_hline(0, LCD_WIDTH - 1, DIG_Y + DIG_H + 10);
    draw_list(LIST_Y, LIST_H, ms);
    wk->shown_ds = ms / 100;
}

/* Everything after the first paint is drawn here and pushed in pieces of
 * its own (full width, at most 72 rows): the digits when the tenth turns
 * or an input changed them, the lap rows when an input did. */
static int timer_tick(struct ui_screen *s)
{
    bool run = ui_sw_running(), listed = false;
    uint32_t ms;
    int rem;

    (void)s;
    ms = ui_sw_now_ms(false);
    if (wk->redo_list)
    {
        wk->redo_list = false;
        listed = true;
        draw_list(LIST_Y, LIST_H, ms);
        ui_lcd_push("lap", 0, LIST_Y, LCD_WIDTH, LIST_H / 2);
        ui_lcd_push("lap", 0, LIST_Y + LIST_H / 2, LCD_WIDTH, LIST_H / 2);
    }
    if (wk->redo_digits || (run && ms / 100 != wk->shown_ds))
    {
        wk->redo_digits = false;
        draw_digits(ms);
        ui_lcd_push("digits", 0, DIG_Y, LCD_WIDTH, DIG_H);
        if (run && !listed && wk->off == 0)
        {
            draw_lap_row(0, ms);
            ui_lcd_push("lap", 0, LIST_Y, LCD_WIDTH, TM_PITCH);
        }
        wk->shown_ds = ms / 100;
    }
    if (!run)
        return TIMEOUT_BLOCK;
    rem = 100 - (int)(ms % 100);                /* ms to the next tenth */
    return MAX((rem + 9) * HZ / 1000, 1);
}

static enum ui_result timer_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    int off, max;

    (void)s;
    switch (in->key)
    {
        case UI_KEY_PLAY:
            if (in->release && !in->long_press)
            {
                ui_sw_toggle(&wk->logs);
                wk->redo_digits = wk->redo_list = true;     /* the tick */
            }
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (in->release && !in->long_press && ui_sw_running())
            {
                ui_sw_lap();
                wk->off = 0;
                wk->redo_list = true;
            }
            return UI_HANDLED;
        case UI_KEY_WHEEL:
            max = MAX(lap_rows() - TM_ROWS, 0);
            off = MIN(MAX(wk->off + in->wheel_steps, 0), max);
            if (off != wk->off)
            {
                wk->off = off;
                wk->redo_list = true;
            }
            return UI_HANDLED;
        default:
            return UI_IGNORED;                  /* MENU leaves; it runs on */
    }
}

static const struct ui_screen_ops timer_screen = {
    .name    = "stopwatch-timer",
    .enter   = timer_enter,
    .leave   = NULL,
    .draw    = timer_draw,
    .input   = timer_input,
    .timeout = NULL,
    .tick    = timer_tick,
};

/* -- a log's detail ---------------------------------------------------------- */

static void ditem(int *n, const char *name, uint32_t ms, const char *plain)
{
    char t[16];

    if (!plain)
        fmt_ms(t, sizeof(t), ms);
    snprintf(wk->u.d.lbl[*n], sizeof(wk->u.d.lbl[0]), "%.15s%c%.9s", name,
             UI_VALUE_SEP, plain ? plain : t);
    wk->u.d.item[*n] = (struct ui_menu_item){ wk->u.d.lbl[*n], NULL, NULL, NULL,
        NULL, &ui_preview_x_stopwatch, NULL };
    (*n)++;
}

static int detail_enter(struct ui_screen *s, const void *arg)
{
    const struct sw_log *l;
    char name[24];
    int n = 0, i, shown;

    if (!wk || !arg || *(const int *)arg >= wk->logs.count)
        return -1;
    l = &wk->logs.log[*(const int *)arg];
    fmt_date(wk->dtitle, sizeof(wk->dtitle), l->start_rtc);
    ditem(&n, str(LANG_IPODUI_X_TOTAL_TIME), l->total_ms, NULL);
    ditem(&n, str(LANG_IPODUI_X_SHORTEST_LAP), l->shortest, NULL);
    ditem(&n, str(LANG_IPODUI_X_LONGEST_LAP), l->longest, NULL);
    ditem(&n, str(LANG_IPODUI_X_AVERAGE_LAP), l->average, NULL);
    shown = MIN((int)l->nlaps, SW_LAST_LAPS);
    for (i = 0; i < shown; i++)             /* the last laps, newest first */
    {
        snprintf(name, sizeof(name), str(LANG_IPODUI_X_LAP_N),
                 (int)l->nlaps - i);
        ditem(&n, name, l->last[i], NULL);
    }
    wk->dmenu = (struct ui_menu){ wk->dtitle, wk->u.d.item, n, true, false,
                                  NULL };
    return ui_menu_screen.enter(s, &wk->dmenu);
}

static void detail_leave(struct ui_screen *s)
{
    ui_menu_screen.leave(s);
}

static void detail_draw(struct ui_screen *s, unsigned dirty)
{
    ui_menu_screen.draw(s, dirty);
}

static enum ui_result detail_input(struct ui_screen *s,
                                   const struct ui_input *in)
{
    return ui_menu_screen.input(s, in);
}

static int detail_tick(struct ui_screen *s)
{
    return ui_menu_screen.tick(s);
}

static const struct ui_screen_ops detail_screen = {
    .name    = "stopwatch-log",
    .enter   = detail_enter,
    .leave   = detail_leave,
    .draw    = detail_draw,
    .input   = detail_input,
    .timeout = NULL,
    .tick    = detail_tick,
};

/* -- the list ---------------------------------------------------------------- */

static const char arg_new = 'n', arg_resume = 'r';

static bool vis_resume(void)
{
    return ui_sw_active();
}

static void act_clear(void)
{
    ui_sw_logs_clear(&wk->logs);
    ui_invalidate(UI_DIRTY_BODY);               /* refresh() drops the rows */
}

static const int log_idx[SW_LOGS_MAX] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19
};
#define LOG_ACT(n) static void act_log##n(void) \
    { ui_push(&detail_screen, &log_idx[n]); }
LOG_ACT(0) LOG_ACT(1) LOG_ACT(2) LOG_ACT(3) LOG_ACT(4) LOG_ACT(5)
LOG_ACT(6) LOG_ACT(7) LOG_ACT(8) LOG_ACT(9) LOG_ACT(10) LOG_ACT(11)
LOG_ACT(12) LOG_ACT(13) LOG_ACT(14) LOG_ACT(15) LOG_ACT(16) LOG_ACT(17)
LOG_ACT(18) LOG_ACT(19)
static void (*const log_act[SW_LOGS_MAX])(void) = {
    act_log0, act_log1, act_log2, act_log3, act_log4, act_log5, act_log6,
    act_log7, act_log8, act_log9, act_log10, act_log11, act_log12,
    act_log13, act_log14, act_log15, act_log16, act_log17, act_log18,
    act_log19,
};

/* menu.c calls it on enter and before every full draw */
static void list_refresh(void)
{
    const struct ui_preview_ops *pv = &ui_preview_x_stopwatch;
    int i, n = wk->logs.count;
    char d[12], c[12];

    wk->menu.title = str(LANG_IPODUI_X_STOPWATCH);
    wk->u.l.item[0] = (struct ui_menu_item){ str(LANG_IPODUI_X_RESUME),
        &timer_screen, &arg_resume, vis_resume, NULL, pv, NULL };
    wk->u.l.item[1] = (struct ui_menu_item){ str(LANG_IPODUI_X_NEW_TIMER),
        &timer_screen, &arg_new, NULL, NULL, pv, NULL };
    wk->u.l.item[2] = (struct ui_menu_item){ str(LANG_IPODUI_X_CLEAR_LOGS),
        NULL, NULL, NULL, act_clear, pv, NULL };
    for (i = 0; i < n; i++)
    {
        fmt_date(d, sizeof(d), wk->logs.log[i].start_rtc);
        fmt_clock(c, sizeof(c), wk->logs.log[i].start_rtc);
        snprintf(wk->u.l.lbl[i], sizeof(wk->u.l.lbl[0]), "%.8s%c%.8s", d, UI_VALUE_SEP,
                 c);
        wk->u.l.item[3 + i] = (struct ui_menu_item){ wk->u.l.lbl[i], NULL, NULL,
            NULL, log_act[i], pv, NULL };
    }
    wk->menu.count = 3 + n;
}

static int root_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
    wk = mod_work_alloc(MOD_STOPWATCH, WORK_BYTES);
    if (!wk)
        return -1;
    memset(wk, 0, WORK_BYTES);
    ui_sw_logs_load(&wk->logs);
    wk->menu = (struct ui_menu){ NULL, wk->u.l.item, 3, true, false,
                                 list_refresh };
    list_refresh();
    UPDATE_TRACE("stopwatch open active=%d logs=%d", (int)ui_sw_active(),
                 (int)wk->logs.count);
    return ui_menu_screen.enter(s, &wk->menu);
}

static void root_leave(struct ui_screen *s)
{
    ui_menu_screen.leave(s);
    wk = NULL;                                  /* the arena takes the block */
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
    return ui_menu_screen.tick(s);
}

const struct ui_screen_ops ui_x_stopwatch_screen = {
    .name    = "stopwatch",
    .enter   = root_enter,
    .leave   = root_leave,
    .draw    = root_draw,
    .input   = root_input,
    .timeout = NULL,
    .tick    = root_tick,
};
