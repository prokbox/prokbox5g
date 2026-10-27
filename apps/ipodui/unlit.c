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
 * iPod UI shell: the unlit clock face (#51, Phase 8a U2;
 * docs/phase8a-brief.md §3.5, Revision 1 H2, M1, M2, M4, L6;
 * docs/design/unlit.md "stock").
 *
 * When the backlight times out (SYS_IPODUI_BACKLIGHT_OFF, posted by
 * firmware/backlight.c) the shell paints a grey clock face that reads by
 * ambient light: the time in block digits, play/pause, padlock and
 * battery. The screen stack stays intact underneath; any key, Hold off
 * or USB dismisses the face and the top screen redraws in full (the face
 * covered every pixel: the written reason lean rule 4 asks for).
 *
 * Drawing: straight into the framebuffer with lcd_fillrect, no heap, no
 * boost (unlit_core.c paints; ~190-400 rects for the first show, one
 * changed cell ~25 rects a minute). Pushes: one full lcd_update() at
 * entry, then one 320 x 68 band per minute (⚠️ U9) and a 320 x 37 band
 * when the status row changes. While the LCD sleeps lcd_update_rect()
 * returns at once; the framebuffer is still kept current, so
 * lcd_awake()'s own push shows the right time.
 *
 * Identity vs validity (unlit.h): shown_min / shown_12h / shown and
 * shown_clock are what the framebuffer holds; fb_valid is cleared by
 * anything else that draws (ui_unlit_foreign_draw()), and the next
 * redraw is then a full one.
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
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "scroll_engine.h"
#include "button.h"
#include "backlight.h"
#include "audio.h"
#include "power.h"
#include "powermgmt.h"
#include "settings.h"
#include "timefuncs.h"
#include "debug.h"
#include "logf.h"
#include "ipodui/unlit.h"
#include "ipodui/unlit_core.h"
#include "ipodui/anim.h"
#include "ipodui/screen_stack.h"
#include "ipodui/usbscreen.h"
#include "ipodui/devmode.h"
#include "ipodui/artcache.h"
#include "ipodui/screens/nowplaying.h"
#include "ipodui/preview.h"     /* ui_fb_settle() (P19) */

#ifdef SIMULATOR
int sim_header_clock(void);     /* sim_cmd_clock.c: pinned minute, or -1 */
#endif

#define SWALLOW_POLL (HZ/10)    /* buttons-up check while swallowing */

static struct ui_unlit_state st;
static bool st_ready;
static struct unlit_clock shown_clock;  /* the time row in the framebuffer */
static long dismiss_btn;        /* button bits of the dismissing press */
static long orphan_btn;         /* still held after the 1 s fallback: drop
                                   its repeats and release */

static void ready(void)
{
    if (!st_ready)
    {
        unlit_state_init(&st, current_tick);
        st_ready = true;
    }
}

/* -- drawing ------------------------------------------------------------ */

static void fill(void *ctx, int x, int y, int w, int h, int g)
{
    (void)ctx;
    lcd_set_foreground(LCD_RGBPACK(g, g, g));
    lcd_fillrect(x, y, w, h);
}

static int minute_now(int *sec)
{
    struct tm *tm = get_time();
    int m = tm->tm_hour * 60 + tm->tm_min;

    *sec = tm->tm_sec;
#ifdef SIMULATOR
    if (sim_header_clock() >= 0)        /* golden shots: pinned */
    {
        m = sim_header_clock();
        *sec = 0;
    }
#endif
    return m;
}

static void read_status(struct ui_unlit_status *s)
{
    int a = audio_status();
    int pct = battery_level();

    memset(s, 0, sizeof(*s));
    s->play = (a & AUDIO_STATUS_PAUSE) ? 2 : (a & AUDIO_STATUS_PLAY) ? 1 : 0;
#ifdef HAS_BUTTON_HOLD
    s->hold = button_hold();
#endif
#if CONFIG_CHARGING >= CHARGING_MONITOR
    s->charging = charging_state();
#endif
    s->fill_px = unlit_fill_px(pct < 0 ? 0 : pct);
}

static unsigned saved_fg;

static void draw_begin(void)
{
    lcd_set_viewport(NULL);
    lcd_set_drawmode(DRMODE_SOLID);
    saved_fg = lcd_get_foreground();
}

static void draw_end(void)
{
    lcd_set_foreground(saved_fg);   /* leave the viewport as found */
}

static void draw_full(void)
{
    int sec;
    struct unlit_clock c;

    draw_begin();
    st.shown_min = minute_now(&sec);
    st.shown_12h = global_settings.timeformat != 0;
    read_status(&st.shown);
    unlit_clock_layout(st.shown_min, st.shown_12h, &c);
    fill(NULL, 0, 0, LCD_WIDTH, LCD_HEIGHT, UNLIT_PAPER);
    unlit_paint_clock(&c, NULL, fill, NULL);
    unlit_paint_status(&st.shown, fill, NULL);
    draw_end();
    shown_clock = c;
    st.fb_valid = true;
    UPDATE_TRACE("update unlit:full%s", "");
    lcd_update();
}

static void draw_digits(void)
{
    int sec;
    struct unlit_clock c;

    draw_begin();
    st.shown_min = minute_now(&sec);
    st.shown_12h = global_settings.timeformat != 0;
    unlit_clock_layout(st.shown_min, st.shown_12h, &c);
    unlit_paint_clock(&c, &shown_clock, fill, NULL);
    draw_end();
    shown_clock = c;
    ui_lcd_push("unlit:digits", 0, UNLIT_DIGIT_Y, LCD_WIDTH, UNLIT_DIGIT_H);
}

static void draw_status(void)
{
    struct ui_unlit_status now;

    read_status(&now);
    if (!memcmp(&now, &st.shown, sizeof(now)))
        return;                     /* already on screen (Hold polled
                                     * before its event arrived) */
    st.shown = now;
    UPDATE_TRACE("unlit status play=%d hold=%d charging=%d fill=%d",
                 now.play, now.hold, now.charging, now.fill_px);
    draw_begin();
    unlit_paint_status(&st.shown, fill, NULL);
    draw_end();
    ui_lcd_push("unlit:status", 0, UNLIT_STATUS_Y, LCD_WIDTH,
                UNLIT_STATUS_H);
}

/* -- transitions -------------------------------------------------------- */

static void dismissed(const char *why)
{
    UPDATE_TRACE("unlit exit %s", why);
    (void)why;                      /* the trace is a no-op on the device */
    shown_clock.n = 0;
    ui_invalidate(UI_DIRTY_ALL);    /* one full redraw of the top screen */
    artcache_note_input();          /* a wake is user input: the art */
    ui_nowplaying_note_input();     /* builder waits, no auto-return */
}

static void apply(unsigned a, const char *why)
{
    if (a & UA_DISMISS)
        dismissed(why);
    if (a & UA_DRAW_FULL)
        draw_full();
    else
    {
        if (a & UA_DRAW_DIGITS)
            draw_digits();
        if (a & UA_DRAW_STATUS)
            draw_status();
    }
}

static void enter(void)
{
    lcd_scroll_stop();              /* L6: no marquee writes under the face */
    /* P19 screen truth (M6): the drift's panel back in the framebuffer
     * before the face's first push (the face is drawn before any tick) */
    ui_fb_settle(0, 0, LCD_WIDTH, LCD_HEIGHT);
    UPDATE_TRACE("unlit enter%s", "");
    draw_full();
}

static void tick_step(void)
{
    bool had_skip = st.skip_next;

    ui_unlit_step(&st, UE_TICK, current_tick, true);
    if (had_skip && !st.skip_next)
        UPDATE_TRACE("unlit skip-expired%s", "");
}

/* the release of the dismissing key may still be queued while
 * button_status() already reads 0: only an empty queue means "up" */
static bool buttons_up(long bits)
{
    return !(button_status() & bits) && button_queue_count() == 0;
}

static void swallow_checks(void)
{
    if (st.swallowing)
    {
        tick_step();                /* the 1 s fallback (and skip expiry) */
        if (!st.swallowing)
        {
            UPDATE_TRACE("unlit swallow-end %s", "timeout");
            orphan_btn = dismiss_btn;
        }
        else if (buttons_up(~0L))
        {
            ui_unlit_step(&st, UE_BUTTONS_UP, current_tick, true);
            UPDATE_TRACE("unlit swallow-end %s", "buttons-up");
        }
    }
    if (orphan_btn && buttons_up(orphan_btn))
        orphan_btn = 0;
}

/* -- API (unlit.h) ------------------------------------------------------ */

bool ui_unlit_active(void)
{
    return st_ready && st.face;
}

void ui_unlit_sysevent(long ev, intptr_t data)
{
    unsigned a;

    ready();
    if (ev == SYS_IPODUI_BACKLIGHT_OFF)
    {
        bool had_skip = st.skip_next;

        if (ui_usb_screen_active())
        {
            UPDATE_TRACE("unlit skip %s", "usb");
            return;
        }
        if (ipodui_stock_ui_allowed())
        {
            UPDATE_TRACE("unlit skip %s", "devmode");
            return;
        }
        if (!had_skip && ui_nowplaying_viz_face())
        {
            /* #126: on a charger, the Visualizer face instead (the
             * setting); it relit the screen */
            UPDATE_TRACE("unlit skip %s", "viz");
            return;
        }
        a = ui_unlit_step(&st, UE_BL_OFF, current_tick, is_backlight_on(true));
        if (a & UA_SKIP)
            UPDATE_TRACE("unlit skip %s", "sleep");
        else if (had_skip)
            UPDATE_TRACE("unlit skip-expired%s", "");
        if (a & UA_DRAW_FULL)
            enter();
    }
    else if (ev == SYS_IPODUI_HOLD_CHANGED)
    {
        bool was = st.face;

        /* #65: Hold on shows the face whatever the light does next
         * (the hidden "backlight on Hold" setting defaults to Off here,
         * the always-off path that never posts BACKLIGHT_OFF) */
        if (data && !was && (ui_usb_screen_active()
                             || ipodui_stock_ui_allowed()))
            return;
        a = ui_unlit_step(&st, data ? UE_HOLD_ON : UE_HOLD_OFF, current_tick,
                          is_backlight_on(true));
        if (!was && st.face)
            enter();                /* stops the marquee, full draw */
        else
            apply(a, "holdoff");
    }
    else if (ev == SYS_USB_CONNECTED)
    {
        a = ui_unlit_step(&st, UE_USB, current_tick, true);
        apply(a, "usb");
    }
}

void ui_unlit_dismiss(const char *why)
{
    unsigned a;

    if (!st_ready)
        return;
    /* the face's "leave without a key" path (as USB, Hold off) */
    a = ui_unlit_step(&st, UE_USB, current_tick, true);
    apply(a, why);
}

static inline const char *key_name(const struct ui_input *in)
{
    static const char *const names[] = {
        "none", "menu", "select", "play", "prev", "next", "wheel" };
    return names[in->key];
}

bool ui_unlit_filter(const struct ui_input *in)
{
    long bits = in->raw & ~(BUTTON_REL | BUTTON_REPEAT);
    bool wheel = in->key == UI_KEY_WHEEL;
    enum ui_unlit_ev ev;
    unsigned a;
    bool was;

    if (!st_ready)
        return false;
    tick_step();
    if (orphan_btn && !wheel && (bits & orphan_btn))
    {
        if (in->release)
            orphan_btn = 0;
        UPDATE_TRACE("unlit swallow %s", key_name(in));
        return true;
    }
    if (wheel)
        ev = UE_WHEEL;
    else if (in->release)
    {
        switch (unlit_release_route(&st, bits, dismiss_btn))
        {
            case UNLIT_REL_PASS:    /* key-less: not a key (fix M2) */
                return false;
            case UNLIT_REL_SWALLOW: /* another key's release while the
                                     * dismissing one is held */
                UPDATE_TRACE("unlit swallow %s", key_name(in));
                return true;
            default:
                break;
        }
        ev = UE_KEY_RELEASE;
    }
    else
        ev = UE_KEY;

    was = st.swallowing;
    a = ui_unlit_step(&st, ev, current_tick, is_backlight_on(true));
    if (a & UA_DISMISS)
    {
        dismiss_btn = ev == UE_KEY ? bits : 0;
        dismissed("key");
        UPDATE_TRACE("unlit swallow %s", key_name(in));
        return true;
    }
    if (was && !st.swallowing)
    {
        if (ev == UE_KEY_RELEASE)
            UPDATE_TRACE("unlit swallow-end %s", "release");
        else if (ev == UE_KEY)
        {
            /* the 1 s fallback: the held key stays dropped */
            UPDATE_TRACE("unlit swallow-end %s", "timeout");
            orphan_btn = dismiss_btn;
            if (bits & orphan_btn)
                return true;
        }
    }
    if (a & UA_SWALLOW)
    {
        UPDATE_TRACE("unlit swallow %s", key_name(in));
        return true;
    }
    return false;
}

int ui_unlit_timeout(void)
{
    int sec, t = TIMEOUT_BLOCK;

    if (!st_ready)
        return t;
    if (st.face)
    {
        minute_now(&sec);
        t = ui_unlit_ticks_to_minute(sec);
    }
    if (st.swallowing || orphan_btn)
        t = t == TIMEOUT_BLOCK ? SWALLOW_POLL : MIN(t, SWALLOW_POLL);
    return t;
}

void ui_unlit_tick(void)
{
    struct ui_unlit_status now;
    int sec, m;

    if (!st_ready)
        return;
    swallow_checks();
    if (st.skip_next)
        tick_step();
    if (!st.face)
        return;
    m = minute_now(&sec);
    if (m != st.shown_min || st.shown_12h != (global_settings.timeformat != 0))
        apply(ui_unlit_step(&st, UE_MINUTE, current_tick, true), "minute");
    read_status(&now);
    if (memcmp(&now, &st.shown, sizeof(now)))
        apply(ui_unlit_step(&st, UE_STATUS, current_tick, true), "status");
}

void ui_unlit_suppress_next(void)
{
    ready();
    ui_unlit_step(&st, UE_SLEEP_NEXT, current_tick, true);
}

void ui_unlit_foreign_draw(void)
{
    st.fb_valid = false;
}
