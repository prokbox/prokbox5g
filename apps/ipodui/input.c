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
#include "config.h"
#include "lcd.h"
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "misc.h"
#include "logf.h"
#include "powermgmt.h"
#include "ipodui/sleep.h"
#include "ipodui/input.h"
#include "ipodui/fe_be.h"         /* P19 MF-4: the pixel fence */
#include "ipodui/screen_stack.h"
#include "ipodui/scroll.h"
#include "ipodui/anim.h"
#include "ipodui/uilog.h"
#include "ipodui/powerlog.h"
#include "ipodui/battlog.h"
#include "ipodui/batcal.h"
#include "ipodui/unlit.h"
#include "ipodui/usbbusy.h"
#include "ipodui/artcache.h"
#include "ipodui/lock.h"
#include "ipodui/clockpol.h"        /* np-lag: the input boost */
#include "ipodui/uilat.h"           /* np-lag probe */
#include "ipodui/stopwatch.h"
#include "ipodui/lib_shuffle.h"     /* lib_shuffle_settle() */
#include "ipodui/sdk/sdk_int.h"     /* sdk_poweroff() */
#include "bcmjob_pp.h"              /* P19: bcmjob_owner_event() */
#include "ipodui/art_bcm.h"         /* art_bcm_pass_note() */

#define LONG_PRESS_TICKS (HZ/2)

static long press_tick;         /* tick of the last key press */
static bool long_fired;         /* long press already reported for it */

/* Clicker (#58): upstream clicks in get_action() (action.c), which the
 * shell never calls, so the shell clicks here, for every button event
 * that reaches it: a press, a repeat (keyclick_click() honours the
 * "keyclick repeats" setting as upstream does: off, a held button clicks
 * once on its first repeat) and each wheel event (one per wheel detent,
 * before acceleration, as upstream). Releases and system events never
 * click (keyclick_click()'s own filter). The piezo (and the haptic mod
 * on the same line) is driven by piezo_button_beep() on the target; the
 * simulator has no piezo, so a trace counts the clicks there:
 *   click n=<count> btn=<hex>     one per click that keyclick_click()
 *                                 decided to sound */
#ifdef SIMULATOR
static unsigned click_n;

static bool click_trace(int action, void *data)
{
    (void)data;
    UPDATE_TRACE("click n=%u btn=%x", ++click_n, (unsigned)action);
    return true;
}
#endif

static void ui_click(long btn)
{
    if (btn & (BUTTON_REL | SYS_EVENT))
        return;
#ifdef SIMULATOR
    keyclick_set_callback(click_trace, NULL);
#endif
    keyclick_click(true, (int)btn);
}

static enum ui_key map_key(long btn)
{
    switch (btn & ~(BUTTON_REL | BUTTON_REPEAT))
    {
        case BUTTON_MENU:        return UI_KEY_MENU;
        case BUTTON_SELECT:      return UI_KEY_SELECT;
        case BUTTON_PLAY:        return UI_KEY_PLAY;
        case BUTTON_LEFT:        return UI_KEY_PREV;
        case BUTTON_RIGHT:       return UI_KEY_NEXT;
        case BUTTON_SCROLL_FWD:
        case BUTTON_SCROLL_BACK: return UI_KEY_WHEEL;
        default:                 return UI_KEY_NONE;  /* chord or unknown */
    }
}

/* Wheel velocity in clicks/s (one click = one wheel event = 15 degrees,
 * see scroll.h). The clickwheel driver reports its filtered speed in
 * degrees/s; the simulator posts bare key events, so there the speed is
 * estimated from the event spacing with the driver's 15/16 filter (and
 * its reset on a reversal or a 250 ms pause). */
#define WHEEL_DEG_PER_CLICK 15
#define WHEEL_IDLE_TICKS    (HZ/4)

static long wheel_tick;         /* tick of the last wheel event */
static int wheel_dir;
static unsigned wheel_est;      /* simulator estimate, clicks/s */
static unsigned wheel_vel;      /* the last event's velocity, clicks/s */

unsigned ui_input_wheel_speed(void)
{
    if (current_tick - wheel_tick > WHEEL_IDLE_TICKS)
        return 0;
    return wheel_vel;
}

static int wheel_steps(intptr_t data, int dir, unsigned *velocity)
{
    long now = current_tick;
    long dt = now - wheel_tick;

    if (dir != wheel_dir || dt > WHEEL_IDLE_TICKS)
        wheel_est = 0;
    else
        wheel_est = (15 * wheel_est + HZ / (dt > 0 ? dt : 1)) / 16;
    wheel_tick = now;
    wheel_dir = dir;
    *velocity = wheel_est;

#ifdef HAVE_WHEEL_ACCELERATION
    if (data & (1u << 31))
        *velocity = (data & 0xffffff) / WHEEL_DEG_PER_CLICK;
#else
    (void)data;
#endif
    wheel_vel = *velocity;
    return ui_wheel_steps(1, *velocity);
}

bool ui_input_get(struct ui_input *in, int timeout)
{
    long deadline = current_tick + timeout;

    /* P12 Screen Lock (lock.h): the locked screen up while locked, off
     * after a data host unlocked; the main loop redraws first */
    if (ui_lock_gate())
    {
        memset(in, 0, sizeof(*in));
        return false;
    }

    for (;;)
    {
        long btn;

        fe_wait_begin();            /* P19 MF-4: the COP's cache written back */
        btn = button_get_w_tmo(timeout);
        fe_wait_end();

        uilat_input(btn);       /* np-lag probe: the wheel's queue wait */

        memset(in, 0, sizeof(*in));
        in->raw = btn;

        if (btn == BUTTON_NONE)
            return false;

        if (btn & SYS_EVENT)
        {
            /* P19 S0 (MF-2): another thread asked the job driver
             * (suspend, resume, a decode's pass); this thread owns it.
             * Its time is the pass's BCM work (sitting 4c: the
             * per-minute max, art_bcm_pass_note()) */
#ifdef HAVE_ART_BCM
            uint32_t t0 = art_bcm_pass_now();

            if (bcmjob_owner_event(btn))
            {
                art_bcm_pass_note("owner", art_bcm_pass_now() - t0);
                return false;
            }
#else
            if (bcmjob_owner_event(btn))
                return false;
#endif
            if (btn == SYS_FE_FB_FENCE)
            {
                fe_fb_serve();      /* P19 MF-4: a CPU push waits */
                return false;
            }
            ui_anim_idle();     /* the handler may block (USB): unboost */
            clockpol_input_drop();  /* np-lag: the input boost too */
            if (btn == SYS_IPODUI_BACKLIGHT_OFF
                || btn == SYS_IPODUI_HOLD_CHANGED)
            {
                /* Phase 8a: the shell's own events (firmware/backlight.c) */
                if (btn == SYS_IPODUI_HOLD_CHANGED)
                    UPDATE_TRACE("hold %s", button_get_data() ? "on" : "off");
                ui_unlit_sysevent(btn, button_get_data());
                if (btn == SYS_IPODUI_BACKLIGHT_OFF)
                {
                    artcache_note_idle();   /* #58: an idle-build wake */
                    ui_usbbusy_dark();      /* 3.20: lit work only */
                }
                return false;
            }
#ifdef SYS_SLEEP_REQUEST
            if (btn == SYS_SLEEP_REQUEST)
            {
                /* P18 Q4: the idle timer (powermgmt.c); ui_main.c's
                 * loop runs it before its next draw */
                ui_sleep_request((enum ui_sleep_reason)button_get_data());
                return false;
            }
#endif
            if (btn == SYS_POWEROFF)
            {
                ui_log("power", "off batt=%d", battery_level());
                powerlog_mark("poweroff");  /* #10: written by
                                             * system_flush()'s idle call */
                lib_shuffle_settle();   /* npfix review L1: the
                                         * record matches the queue */
                sdk_poweroff();     /* P12 W4-0: a game's state record */
                ui_sw_poweroff();   /* P12: a running Stopwatch timing
                                     * stopped and logged (one write);
                                     * before the flush, so its log
                                     * lines go out with the ring */
                battlog_end("off");    /* P19 PW4: before the flush */
                ui_log_flush(); /* the ring, before clean_shutdown() */
                batcal_poweroff();  /* #70: system_flush() writes it */
            }
            if (btn == SYS_USB_CONNECTED)
            {
                /* P12 Q10: a computer for data unlocks (RAM only here;
                 * the save follows the session, lock.c) */
                ui_lock_usb_host();
                powerlog_mark("usb");       /* #10: out before the ack */
                ui_sleep_window_close();    /* P18: no boost into it */
                ui_unlit_sysevent(btn, 0);  /* leave the face first */
            }
            if (default_event_handler(btn) == SYS_USB_CONNECTED)
            {
                ui_invalidate(UI_DIRTY_ALL);  /* the USB screen drew over us */
                ui_unlit_foreign_draw();
            }
            lcd_set_viewport(NULL);   /* don't keep a handler's viewport */
            return false;
        }

        /* P18: the waking press (and PLAY held into the sleep) up to
         * its release; no click, no action */
        if (ui_sleep_swallow(btn))
        {
            if (btn & BUTTON_REL)
                long_fired = false;
            return false;
        }
        ui_click(btn);
        in->key = map_key(btn);
        in->release = (btn & BUTTON_REL) != 0;
        in->repeat = (btn & BUTTON_REPEAT) != 0;

        /* the unlit face (Phase 8a, Revision 1 M1): a key while it shows
         * dismisses it and is swallowed with its repeats and release; the
         * main loop then redraws the top screen (hence false: no input).
         * Chords and unknown codes (UI_KEY_NONE) wake it too. Wheel
         * releases carry nothing and never reach it. */
        if (!(in->key == UI_KEY_WHEEL && in->release))
        {
            struct ui_input f = *in;
            if (f.key == UI_KEY_WHEEL)
                f.wheel_steps = (btn & BUTTON_SCROLL_BACK) ? -1 : 1;
            if (ui_unlit_filter(&f))
            {
                if (in->release)
                    long_fired = false;
                return false;
            }
        }

        /* P12 Screen Lock (Q10): locked, only the combination entry,
         * hold PLAY and a system screen's keys (a ring) get through */
        if (ui_lock_key(in))
        {
            if (in->release)
                long_fired = false;
            return false;
        }

        if (in->key == UI_KEY_WHEEL)
        {
            if (!in->release)
            {
                int dir = (btn & BUTTON_SCROLL_BACK) ? -1 : 1;
                in->wheel_steps = dir * wheel_steps(button_get_data(), dir,
                                                    &in->wheel_velocity);
                return true;
            }
            /* wheel "release" carries nothing: wait for the next event */
        }
        else if (in->release)
        {
            in->long_press = long_fired;
            long_fired = false;
            return true;
        }
        else if (in->repeat)
        {
            if (!long_fired &&
                TIME_AFTER(current_tick, press_tick + LONG_PRESS_TICKS))
            {
                long_fired = true;
                in->long_press = true;
            }
            return true;
        }
        else
        {
            press_tick = current_tick;
            long_fired = false;
            return true;
        }

        if (timeout != TIMEOUT_BLOCK)
        {
            timeout = deadline - current_tick;
            if (timeout <= 0)
                return false;
        }
    }
}
