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
 * iPod UI shell: header bar
 *
 * The 7G-style title bar: a light silver vertical gradient with a darker
 * 1 px bottom line, the screen title left-aligned, and a right-aligned
 * cluster of hold lock, play/pause and battery. On the main menu the
 * header covers only the list half (st->width).
 *
 * The header keeps a tiny cache of what it last drew and only repaints
 * the regions (title, glyph cluster) whose content changed.
 *
 * Time in Title: ui_header_text() picks the title or the clock for the
 * moment; a swap repaints the title region only (one cross-fade frame,
 * then the new text).
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
#include "system.h"
#include <stdio.h>
#include <string.h>
#include "lcd.h"
#include "font.h"
#include "audio.h"
#include "button.h"
#include "power.h"
#include "powermgmt.h"
#include "kernel.h"
#include "settings.h"
#include "timefuncs.h"
#include "lang.h"
#include "ipodui/theme.h"
#include "ipodui/header.h"
#include "ipodui/paint.h"

/* Metrics and colours shared with the rest of the shell live in
 * ui_theme. The glyphs are pre-rendered tiles (header_tiles.h, generated
 * by apps/ipodui/art/header.py over the kit's fixed header rows: lean
 * rule 1), each drawn with one lcd_bitmap_part(), positioned against the
 * reference: on the 160 px main-menu header the battery body is x 129-152
 * (y 3-14, nub to x 154), the play glyph x 111-121, 7 px before it, and
 * the lock x 108-116 when nothing plays (fidelity audit 1 fixes #6, #7,
 * #16). The battery is the empty-glass tile with the green (red when
 * low) gel fill drawn over it, one row colour at a time. */
#include "ipodui/header_tiles.h"

#define BATT_W          24              /* body incl. the outline */
#define BATT_H          12
#define BATT_Y          HDR_BATT_GLASS_Y
#define BATT_NUB_W      2
#define BATT_FILL_W     (BATT_W - 2)
/* The fill's colour follows the charge (owner 2026-10-22), in percent
 * (UI_BATT_AMBER_PCT / UI_BATT_RED_PCT, header.h): green above 35, amber
 * at or below it, red at or below 20 (the level below which artcache
 * pauses background art work, LOW_BATTERY). On the charger it stays
 * green. A red battery always shows at least BATT_RED_MIN_PX of fill, so
 * an empty one reads red, not as a grey glass. */
#define BATT_AMBER_PCT  UI_BATT_AMBER_PCT
#define BATT_RED_PCT    UI_BATT_RED_PCT
#define BATT_RED_MIN_PX 2
#define BATT_EMPTY      LCD_RGBPACK(0xf4, 0xf4, 0xf4)   /* bolt halo */

#define PLAY_W          11              /* ink width (tiles: + glow column) */
#define LOCK_W          HDR_LOCK_W
#define LOCK_GAP_BATT   12              /* lock -> battery, nothing playing
                                         * (audit fix #16, ~) */

/* Lightning bolt for charging, one span per row: start x, length.
 * Original drawing, 6 x 8: a stroke down-left, a bar to the right, and a
 * second stroke down-left. */
static const unsigned char bolt_rows[][2] = {
    {4, 2}, {3, 2}, {2, 2}, {1, 5}, {3, 2}, {2, 2}, {1, 2}, {1, 1},
};
#define BOLT_W  6
#define BOLT_H  ((int)(sizeof(bolt_rows) / sizeof(bolt_rows[0])))

/* What was last drawn; compared to decide what to repaint. */
static struct {
    bool          valid;
    signed char   play;
    signed char   batt_px;      /* fill width in pixels, not percent */
    bool          powered;
    signed char   batt_band;    /* 0 green, 1 amber, 2 red */
    bool          charging;
    bool          hold;
    unsigned char busy;
    short         width;
    unsigned long title_hash;
} last;

static unsigned long title_hash(const char *s)
{
    unsigned long h = 5381;
    if (!s)
        return 0;
    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h;
}

static int batt_fill_px(int pct, bool charging)
{
    int px;
    if (pct > 100)
        pct = 100;
    px = pct <= 0 ? 0 : (pct * BATT_FILL_W + 50) / 100;
    if (px < 1 && pct > 0)
        px = 1;
    if (ui_batt_band(pct, charging) == 2 && px < BATT_RED_MIN_PX)
        px = BATT_RED_MIN_PX;
    return px;
}

/* Repaint the header background for columns x .. x+w-1 */
static void draw_bg(int x, int w)
{
    ui_paint_header_bg(x, w);
}

/* Play (the triangle) or pause (two bars), left ink column at x; the
 * tiles carry a 1 px glow column left of the ink */
static void draw_play(int x, int play)
{
    if (play == UI_PLAY_PLAYING)
        lcd_bitmap_part(hdr_play, 0, 0, HDR_PLAY_W, x - 1, HDR_PLAY_Y,
                        HDR_PLAY_W, HDR_PLAY_H);
    else
        lcd_bitmap_part(hdr_pause, 0, 0, HDR_PAUSE_W, x - 1, HDR_PAUSE_Y,
                        HDR_PAUSE_W, HDR_PAUSE_H);
}

static void draw_lock(int x)
{
    lcd_bitmap_part(hdr_lock, 0, 0, HDR_LOCK_W, x, HDR_LOCK_Y, HDR_LOCK_W,
                    HDR_LOCK_H);
}

/* plug, pointing right: cord, body, two prongs (original drawing),
 * one bit per column from the left, 12 x 5 */
static const unsigned short plug_rows[] = {
    0x0f0, 0x0f6, 0xff0, 0x0f6, 0x0f0,
};
#define PLUG_W  12
#define PLUG_H  ((int)(sizeof(plug_rows) / sizeof(plug_rows[0])))

/* Bolt with a 1 px light halo left and right, so it reads over both the
 * green fill and the empty part */
static void draw_bolt(int x, int y)
{
    int r;
    lcd_set_foreground(BATT_EMPTY);
    for (r = 0; r < BOLT_H; r++)
        lcd_hline(x + bolt_rows[r][0] - 1,
                  x + bolt_rows[r][0] + bolt_rows[r][1], y + r);
    lcd_set_foreground(ui_theme->header_glyph_fg);
    for (r = 0; r < BOLT_H; r++)
        lcd_hline(x + bolt_rows[r][0],
                  x + bolt_rows[r][0] + bolt_rows[r][1] - 1, y + r);
}

/* Battery at bx (outline x bx .. bx+BATT_W-1, nub, emboss line): the
 * empty-glass tile, then the fill over its first fill_px inner columns,
 * one colour per row (left end, body, right end when full) */
/* The plug (on power, not charging) in the bolt's style: a light halo
 * around the dark ink, over the fill and the empty part alike */
static void draw_plug(int x, int y)
{
    int r, c;
    lcd_set_foreground(BATT_EMPTY);
    for (r = -1; r <= PLUG_H; r++)
        for (c = -1; c <= PLUG_W; c++)
        {
            int rr, cc, near = 0;
            for (rr = r - 1; rr <= r + 1 && !near; rr++)
                for (cc = c - 1; cc <= c + 1; cc++)
                    if (rr >= 0 && rr < PLUG_H && cc >= 0 && cc < PLUG_W
                        && (plug_rows[rr] & (0x800 >> cc)))
                    {
                        near = 1;
                        break;
                    }
            if (near)
                lcd_drawpixel(x + c, y + r);
        }
    lcd_set_foreground(ui_theme->header_glyph_fg);
    for (r = 0; r < PLUG_H; r++)
        for (c = 0; c < PLUG_W; c++)
            if (plug_rows[r] & (0x800 >> c))
                lcd_drawpixel(x + c, y + r);
}

static void draw_battery(int bx, int fill_px, int band)
{
    const unsigned short (*f)[HDR_BATT_FILL_H] =
        band == 2 ? hdr_batt_red : band == 1 ? hdr_batt_amber
                                             : hdr_batt_green;
    int r;

    lcd_bitmap_part(hdr_batt_glass, 0, 0, HDR_BATT_GLASS_W, bx, BATT_Y,
                    HDR_BATT_GLASS_W, HDR_BATT_GLASS_H);
    if (fill_px <= 0)
        return;
    for (r = 0; r < HDR_BATT_FILL_H; r++)
    {
        int y = BATT_Y + 1 + r;

        lcd_set_foreground(f[0][r]);
        lcd_drawpixel(bx + 1, y);
        if (fill_px >= 2)
        {
            lcd_set_foreground(f[1][r]);
            lcd_hline(bx + 2, bx + MIN(fill_px, BATT_FILL_W - 1), y);
        }
        if (fill_px >= BATT_FILL_W)
        {
            lcd_set_foreground(f[2][r]);
            lcd_drawpixel(bx + BATT_FILL_W, y);
        }
    }
}

/* The right-aligned cluster, right to left: battery, play/pause, lock */
/* #84: the activity glyph (8 dots, the newest darkest, as the library
 * footer's), in the cluster's free space left of the play glyph. Hidden
 * with the lock shown (no room for both). */
static const signed char busy_pos[8][2] = {
    { 0, -4 }, { 3, -3 }, { 4, 0 }, { 3, 3 },
    { 0, 4 }, { -3, 3 }, { -4, 0 }, { -3, -3 },
};
#define BUSY_R    5
#define BUSY_GAP  7

/* the glyph's clear rectangle for a cluster whose play glyph (or the
 * battery) starts at 'right' */
static int busy_x(int right)
{
    return right - BUSY_GAP - 2 * BUSY_R - 2;
}
#define BUSY_W  (2 * BUSY_R + 3)

static int cluster_left(int width, int play)
{
    const struct ui_theme *t = ui_theme;
    int x = width - t->header_margin_r - BATT_NUB_W - BATT_W;

    return play != UI_PLAY_NONE ? x - t->header_glyph_gap - PLAY_W : x;
}

/* the rectangle the last ui_header_draw() changed (x, w; full height) */
static int push_x, push_w;

void ui_header_push_rect(int *x, int *w)
{
    *x = push_x;
    *w = push_w;
}

static void draw_busy(int right, int busy)
{
    const struct ui_theme *t = ui_theme;
    int cx = right - BUSY_GAP - BUSY_R, cy = t->header_h / 2 - 1;
    unsigned bg = t->header_rows[cy];
    int i;

    draw_bg(busy_x(right), BUSY_W);

    for (i = 0; i < 8; i++)
    {
        int age = (busy - 1 - i) & 7;

        lcd_set_foreground(ui_paint_blend(bg, t->header_glyph_fg,
                                          15 - age * 2));
        lcd_fillrect(cx + busy_pos[i][0] - 1, cy + busy_pos[i][1] - 1, 2, 2);
    }
}

static void draw_right(int width, int play, int fill_px, int band,
                       bool charging, bool powered, bool hold, int busy)
{
    const struct ui_theme *t = ui_theme;
    int bx = width - t->header_margin_r - BATT_NUB_W - BATT_W;
    int x = bx;

    draw_bg(width - t->header_side_w, t->header_side_w);
    if (last.busy)                  /* it may stick out of the cluster */
        draw_bg(busy_x(cluster_left(width, play)), BUSY_W);

    draw_battery(bx, fill_px, band);
    if (charging)
        draw_bolt(bx + (BATT_W - BOLT_W) / 2, BATT_Y + (BATT_H - BOLT_H) / 2);
    else if (powered)
        draw_plug(bx + (BATT_W - PLUG_W) / 2, BATT_Y + (BATT_H - PLUG_H) / 2);

    if (play != UI_PLAY_NONE)
    {
        x -= t->header_glyph_gap + PLAY_W;
        draw_play(x, play);
    }
    if (hold)
        draw_lock(x - (x == bx ? LOCK_GAP_BATT : t->header_glyph_gap)
                  - LOCK_W);
    else if (busy)
        draw_busy(x, busy);
}

/* Draws 'title' (left-aligned at the title position, or centred),
 * truncated with "..." before the glyphs, in colour 'fg' */
static void draw_text(int width, const char *title, bool centre, unsigned fg)
{
    const struct ui_theme *t = ui_theme;
    const int font = t->font_title;
    const int avail = width - t->header_side_w - t->header_title_x
                      - t->header_title_pad;
    char buf[96];
    int w, h;
    int x, y;

    if (!title || !*title)
        return;

    font_getstringsize((const unsigned char *)title, &w, &h, font);
    if (w > avail)
    {
        int ell_w;
        size_t n;

        font_getstringsize((const unsigned char *)"...", &ell_w, NULL, font);
        /* bytes of whole characters that fit in front of the ellipsis */
        n = font_measurestring((const unsigned char *)title,
                               sizeof(buf) - 8, avail - ell_w, &w, NULL,
                               font);
        while (n > 0 && title[n - 1] == ' ')
            n--;
        memcpy(buf, title, n);
        strcpy(buf + n, "...");
        title = buf;
        font_getstringsize((const unsigned char *)title, &w, &h, font);
    }

    /* place by baseline, not by line height: the shell fonts carry tall
     * line metrics (Bold 12 is 18 px for a 9 px cap). The viewport clips
     * any part of the cell above the header. */
    if (font == FONT_UI)
        y = (t->header_h - 1 - h) / 2;
    else
        y = t->header_baseline - font_get(font)->ascent;
    x = centre ? (width - w) / 2 : t->header_title_x;
    lcd_set_foreground(fg);
    lcd_putsxy(x, y, (const unsigned char *)title);
}

/* The title region: the background, then the text; mid cross-fade both
 * texts, half-faded into the header (a still of the 7G caught exactly
 * that, photo extras-calendars.jpg) */
static void draw_title(int width, const struct ui_header_state *st)
{
    const struct ui_theme *t = ui_theme;

    draw_bg(0, width - t->header_side_w);
    if (st->ghost)
    {
        unsigned half = ui_paint_blend(
            t->header_rows[t->header_baseline - 4], t->header_fg, 8);
        draw_text(width, st->ghost, st->centre, half);
        draw_text(width, st->title, st->centre, half);
    }
    else
        draw_text(width, st->title, st->centre, t->header_fg);
}

bool ui_header_draw(const struct ui_header_state *st, bool force)
{
    const struct ui_theme *t = ui_theme;
    struct viewport vp;
    unsigned long th = title_hash(st->title) * 31 + title_hash(st->ghost)
                       + st->centre;
    int width = st->width > 0 ? MIN(st->width, LCD_WIDTH) : LCD_WIDTH;
    int fill_px = batt_fill_px(st->battery_pct, st->charging);
    int band = ui_batt_band(st->battery_pct, st->charging);
    bool do_title, do_right, busy_only;

    force = force || !last.valid || last.width != width;
    do_title = force || last.title_hash != th;
    do_right = force || last.play != (int)st->play
                     || last.batt_px != fill_px
                     || last.batt_band != band
                     || last.charging != st->charging
                     || last.powered != st->powered
                     || last.hold != st->hold
                     || last.busy != st->busy;
    if (!do_title && !do_right)
        return false;
    push_x = 0;
    push_w = width;
    /* only the glyph moved: repaint and push its own rectangle */
    busy_only = !force && !do_title && !st->hold && !last.hold
                && last.play == (int)st->play && last.batt_px == fill_px
                && last.batt_band == band && last.charging == st->charging
                && last.powered == st->powered;

    memset(&vp, 0, sizeof(vp));
    vp.width = width;
    vp.height = t->header_h;
    vp.font = t->font_title;
    vp.drawmode = DRMODE_SOLID;
    vp.flags = VP_DEFAULT_FLAGS;
    lcd_set_viewport(&vp);

    if (do_title)
    {
        lcd_set_drawmode(DRMODE_FG);
        draw_title(width, st);
        lcd_set_drawmode(DRMODE_SOLID);
    }
    /* after the title: the glyph may sit past the cluster's own region */
    if (busy_only)
    {
        int right = cluster_left(width, st->play);

        if (st->busy)
            draw_busy(right, st->busy);
        else
            draw_bg(busy_x(right), BUSY_W);
        push_x = busy_x(right);
        push_w = BUSY_W;
    }
    else if (do_right)
        draw_right(width, st->play, fill_px, band, st->charging, st->powered,
                   st->hold,
                   st->busy);
    if (do_title && !do_right && st->busy && !st->hold)
        draw_busy(cluster_left(width, st->play), st->busy); /* the title's
                                                  clear cut into it */

    /* back to the default viewport, never a saved pointer: after the WPS
     * or USB screen that one may be a freed skin viewport */
    lcd_set_viewport(NULL);

    last.valid = true;
    last.play = st->play;
    last.batt_px = fill_px;
    last.batt_band = band;
    last.charging = st->charging;
    last.powered = st->powered;
    last.hold = st->hold;
    last.busy = st->busy;
    last.width = width;
    last.title_hash = th;
    return true;
}

void ui_header_poll(struct ui_header_state *st)
{
    int status = audio_status();

    if (status & AUDIO_STATUS_PAUSE)
        st->play = UI_PLAY_PAUSED;
    else if (status & AUDIO_STATUS_PLAY)
        st->play = UI_PLAY_PLAYING;
    else
        st->play = UI_PLAY_NONE;

    st->battery_pct = battery_level();
#if CONFIG_CHARGING >= CHARGING_MONITOR
    st->charging = charging_state();
#else
    st->charging = false;
#endif
#if CONFIG_CHARGING
    st->powered = charger_inserted();
#else
    st->powered = false;
#endif
#ifdef HAS_BUTTON_HOLD
    st->hold = button_hold();
#else
    st->hold = false;
#endif
}

/* -- Time in Title -------------------------------------------------------- */

#define TT_IDLE_TICKS   (5 * HZ / 2)    /* the title until 2.5 s idle */
#define TT_FADE_TICKS   3               /* one 30 ms cross-fade frame */

#ifdef SIMULATOR
/* uisimulator/common/sim_cmd_clock.c: the script commands "title_phase"
 * and "sim_clock" freeze the alternation and the time (golden shots) */
int sim_header_phase(void);             /* -1 auto, 0 title, 1 clock */
int sim_header_clock(void);             /* -1 the RTC, else minute of day */
#endif

static struct {
    bool off;                   /* Time in Title switched off */
    const void *screen;         /* the cycle's screen and title */
    unsigned long title_hash;
    long start;                 /* tick of the screen change or input */
    bool clock;                 /* the clock shows, else the title */
    long fade_end;              /* after current_tick: cross-fade frame */
    int minute;                 /* clock_buf's minute of the day, -1 */
    int format;                 /* and its timeformat (24 Hour Clock) */
    char clock_buf[32];         /* "15:04", "3:04 PM" (int-safe size) */
} tt = { .off = true, .minute = -1 };  /* off until the setting is applied
                                         (ui_header_set_time_in_title) */

void ui_header_set_time_in_title(bool on)
{
    tt.off = !on;
    tt.screen = NULL;           /* restart with the title */
}

bool ui_header_time_in_title(void)
{
    return !tt.off;
}

/* Formats the clock when the minute or the format changed; returns the
 * ticks to the next minute */
static int clock_update(void)
{
    struct tm *tm = get_time();
    int mod = tm->tm_hour * 60 + tm->tm_min;
    int sec = tm->tm_sec;

#ifdef SIMULATOR
    if (sim_header_clock() >= 0)
    {
        mod = sim_header_clock();
        sec = 0;
    }
#endif
    if (mod != tt.minute || global_settings.timeformat != tt.format)
    {
        int h = mod / 60, m = mod % 60;

        if (global_settings.timeformat)         /* 12 hour clock */
            snprintf(tt.clock_buf, sizeof(tt.clock_buf), "%d:%02d %s",
                     h % 12 ? h % 12 : 12, m,
                     str(h < 12 ? LANG_IPODUI_AM : LANG_IPODUI_PM));
        else
            snprintf(tt.clock_buf, sizeof(tt.clock_buf), "%02d:%02d", h, m);
        tt.minute = mod;
        tt.format = global_settings.timeformat;
    }
    return (60 - MIN(sec, 59)) * HZ;
}

int ui_header_text(struct ui_header_state *st, const void *screen,
                   const char *title, bool lit)
{
    unsigned long th = title_hash(title);
    long now = current_tick, el;
    int wake, phase = -1;
    bool want;

    st->ghost = NULL;
    if (tt.off)
    {
        st->title = title;
        return TIMEOUT_BLOCK;
    }
    if (screen != tt.screen || th != tt.title_hash)
    {
        /* a new screen (or title): the title first, switched at once
         * (the header text swaps as the slide starts) */
        tt.screen = screen;
        tt.title_hash = th;
        tt.start = now;
        tt.clock = false;
        tt.fade_end = now;
    }
    else if (!lit)
    {
        /* nobody sees it: no swap, no clock read, no wake-up */
        st->title = tt.clock ? tt.clock_buf : title;
        return TIMEOUT_BLOCK;
    }

#ifdef SIMULATOR
    phase = sim_header_phase();
#endif
    if (phase >= 0)
    {
        want = phase == 1;
        wake = TIMEOUT_BLOCK;
    }
    else
    {
        /* the clock once the wheel and buttons rest; it then stays */
        el = now - tt.start;
        want = el >= TT_IDLE_TICKS;
        wake = want ? TIMEOUT_BLOCK : TT_IDLE_TICKS - el;
    }
    if (want)
    {
        int next_min = clock_update();
        wake = wake == TIMEOUT_BLOCK ? next_min : MIN(wake, next_min);
    }

    if (want != tt.clock)
    {
        tt.clock = want;
        /* no cross-fade when frozen (golden shots) or dark */
        tt.fade_end = (phase >= 0 || !lit) ? now : now + TT_FADE_TICKS;
    }
    st->title = tt.clock ? tt.clock_buf : title;
    if (TIME_BEFORE(now, tt.fade_end))
    {
        st->ghost = tt.clock ? title : tt.clock_buf;
        wake = tt.fade_end - now;
    }
    return wake == TIMEOUT_BLOCK ? wake : MAX(wake, 1);
}

bool ui_header_note_input(void)
{
    tt.start = current_tick;
    return !tt.off && tt.clock;
}

const char *ui_header_clock(int *ticks_to_minute)
{
    int t = clock_update();
    if (ticks_to_minute)
        *ticks_to_minute = MAX(t, 1);
    return tt.clock_buf;
}

/* -- Dark header (USB, and later the other dark screens) ------------------ */

/* The battery of the 7G's dark screens (usb.png, owner photos 1165 /
 * photo 1226): a light 22 x 11 outline at x 290..311, y 5..15 (on the
 * 320 px header), a hollow 3 x 5 nub sharing its right edge, a 1 px gap
 * and a light fill. While charging a bolt is knocked out of the fill; on
 * power but not charging (full) a small plug. */
#define DBATT_X         290
#define DBATT_Y         5
#define DBATT_W         22
#define DBATT_H         11
#define DBATT_NUB_H     5
#define DBATT_FILL_W    (DBATT_W - 4)
#define DBATT_FILL_H    (DBATT_H - 4)
#define DBATT_LINE      LCD_RGBPACK(0xdd, 0xe1, 0xe3)
#define DBATT_FILL      LCD_RGBPACK(0xf4, 0xf4, 0xf4)
#define DBATT_LOW       LCD_RGBPACK(0xe0, 0x3a, 0x2a)
#define DBATT_AMBER     LCD_RGBPACK(0xf0, 0xb4, 0x28)
#define DHDR_FG         LCD_RGBPACK(0xff, 0xff, 0xff)
#define DHDR_BASELINE   15              /* "iPod" cap rows 6-15 (usb.png) */

static void draw_dark_battery(const unsigned short *rows, int pct,
                              bool charging, bool powered)
{
    const int x0 = DBATT_X, y0 = DBATT_Y;
    const int x1 = x0 + DBATT_W - 1, y1 = y0 + DBATT_H - 1;
    const int fx = x0 + 2, fy = y0 + 2;
    int ny = y0 + (DBATT_H - DBATT_NUB_H) / 2;
    int fill = pct <= 0 ? 0 : (MIN(pct, 100) * DBATT_FILL_W + 50) / 100;
    int r, c, band;

    for (r = y0; r <= y1; r++)          /* clear the body's rows */
    {
        lcd_set_foreground(rows[r]);
        lcd_hline(x0, x1 + 3, r);
    }
    lcd_set_foreground(DBATT_LINE);
    lcd_hline(x0 + 1, x1 - 1, y0);
    lcd_hline(x0 + 1, x1 - 1, y1);
    lcd_vline(x0, y0 + 1, y1 - 1);
    lcd_vline(x1, y0 + 1, y1 - 1);
    lcd_hline(x1, x1 + 2, ny);          /* nub */
    lcd_hline(x1, x1 + 2, ny + DBATT_NUB_H - 1);
    lcd_vline(x1 + 2, ny + 1, ny + DBATT_NUB_H - 2);
    for (r = 0; r < 2; r++)             /* softened outline corners */
    {
        int y = r ? y1 : y0;
        lcd_set_foreground(ui_paint_blend(rows[y], DBATT_LINE, 8));
        lcd_drawpixel(x0, y);
        lcd_drawpixel(x1, y);
    }
    if (fill < 1 && pct > 0)
        fill = 1;
    band = ui_batt_band(pct, charging);
    if (band == 2 && fill < BATT_RED_MIN_PX)
        fill = BATT_RED_MIN_PX;
    if (fill > 0)
    {
        lcd_set_foreground(band == 2 ? DBATT_LOW
                           : band == 1 ? DBATT_AMBER : DBATT_FILL);
        lcd_fillrect(fx, fy, fill, DBATT_FILL_H);
    }

    if (charging)                       /* bolt, cut out of the fill */
    {
        int bx = fx + (DBATT_FILL_W - BOLT_W) / 2;
        int by = fy + (DBATT_FILL_H - BOLT_H) / 2;
        for (r = 0; r < BOLT_H; r++)
        {
            lcd_set_foreground(rows[by + r]);
            lcd_hline(bx + bolt_rows[r][0],
                      bx + bolt_rows[r][0] + bolt_rows[r][1] - 1, by + r);
        }
    }
    else if (powered)                   /* plug, cut out of the fill */
    {
        int px = fx + (DBATT_FILL_W - PLUG_W) / 2;
        int py = fy + (DBATT_FILL_H - PLUG_H) / 2;
        for (r = 0; r < PLUG_H; r++)
        {
            lcd_set_foreground(rows[py + r]);
            for (c = 0; c < PLUG_W; c++)
                if (plug_rows[r] & (0x800 >> c))
                    lcd_drawpixel(px + c, py + r);
        }
    }
}

void ui_header_draw_dark(const unsigned short *rows, int h,
                         const char *title, int battery_pct, bool charging,
                         bool powered)
{
    struct viewport vp;
    int r;

    memset(&vp, 0, sizeof(vp));
    vp.width = LCD_WIDTH;
    vp.height = h;
    vp.font = ui_theme->font_title;
    vp.drawmode = DRMODE_SOLID;
    vp.flags = VP_DEFAULT_FLAGS;
    lcd_set_viewport(&vp);

    for (r = 0; r < h; r++)
    {
        lcd_set_foreground(rows[r]);
        lcd_hline(0, LCD_WIDTH - 1, r);
    }
    draw_dark_battery(rows, battery_pct, charging, powered);
    if (title && *title)
    {
        int w, th;
        font_getstringsize((const unsigned char *)title, &w, &th,
                           vp.font);
        lcd_set_drawmode(DRMODE_FG);
        lcd_set_foreground(DHDR_FG);
        lcd_putsxy((LCD_WIDTH - w) / 2,
                   DHDR_BASELINE - font_get(vp.font)->ascent,
                   (const unsigned char *)title);
        lcd_set_drawmode(DRMODE_SOLID);
    }
    lcd_set_viewport(NULL);
    last.valid = false;         /* the light header repaints in full next */
}
