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

/* Phase 18 soft sleep in the simulator; see sim_cmd_sleep.h */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <SDL.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "button-sdl.h"
#include "powermgmt.h"
#include "usb.h"
#include "settings.h"
#include "sim_cmd_clock.h"
#include "sim_cmd_sleep.h"
#ifdef HAVE_IPOD_UI
#include "pcm-internal.h"       /* pcm_is_playing() */
#include "power_sleep.h"        /* SLEEP_REFUSE_* */
#endif

long sim_rtc_offset_s;
int sim_sleep_mode = -1;

#define Y2000_EPOCH 946684800L  /* 2000-01-01 00:00:00 in Unix time */

/* powermgmt-sim.c */
void sim_battery_pin_mv(int mv);

#ifdef HAVE_IPOD_UI

#ifdef HAS_BUTTON_HOLD
extern bool hold_button_state;          /* button-sdl.c */
#endif

enum { EV_KEY, EV_TURN, EV_TOUCH, EV_WARP };

struct sev {
    uint8_t type;
    uint8_t bits;       /* EV_KEY: the button; */
    bool down;
    int32_t arg;        /* EV_WARP: seconds; EV_TURN: clicks */
};

#define RING 32
static struct sev ring[RING];
static unsigned ring_rd, ring_wr;
static volatile bool asleep;
static int vbus_raw;
static int last_why = -1;
static uint32_t sim_alarm_in;           /* alarm_in: s after entry, 0 none */
static long pll_hold_until;             /* pll_hold: a tick, 0 none */               /* -1: no wake since the expect */

static void post(struct sev e)
{
    if (ring_wr - ring_rd < RING)
        ring[ring_wr++ % RING] = e;
    else
        fprintf(stderr, "sim_sleep: event ring full, event dropped\n");
}

/* The script waits for the sleep to take its events, so a later level
 * change (hold_switch, charger) never lands inside an earlier warp */
static void drain(void)
{
    for (int i = 0; i < 2 * HZ && asleep && ring_rd != ring_wr; i++)
        sleep(1);
}

static uint8_t key_bit(int sdl_key)
{
    switch (sdl_key)
    {
    case SDLK_RETURN:   return WBTN_SELECT;
    case SDLK_RIGHT:    return WBTN_RIGHT;
    case SDLK_LEFT:     return WBTN_LEFT;
    case SDLK_SPACE:    return WBTN_PLAY;
    case SDLK_ESCAPE:   return WBTN_MENU;
    default:            return 0;
    }
}

static int bit_key(uint8_t bit)
{
    switch (bit)
    {
    case WBTN_SELECT:   return SDLK_RETURN;
    case WBTN_RIGHT:    return SDLK_RIGHT;
    case WBTN_LEFT:     return SDLK_LEFT;
    case WBTN_PLAY:     return SDLK_SPACE;
    default:            return SDLK_ESCAPE;
    }
}

bool sim_sleep_key(int sdl_key, bool down)
{
    uint8_t bit = key_bit(sdl_key);

    if (!asleep)
        return false;
    if (bit)
        post((struct sev){ .type = EV_KEY, .bits = bit, .down = down });
    return true;                        /* asleep: nothing reaches the UI */
}

bool sim_sleep_touch(void)
{
    if (!asleep)
        return false;
    post((struct sev){ .type = EV_TOUCH });
    drain();
    return true;
}

bool sim_sleep_wheel(int clicks)
{
    if (!asleep)
        return false;
    post((struct sev){ .type = EV_TURN, .arg = clicks });
    return true;
}

long sim_rtc_utc_off(void)
{
    static long utc_off;
    static bool have_off;

    if (!have_off)
    {
        time_t now = time(NULL);

        utc_off = localtime(&now)->tm_gmtoff;
        have_off = true;
    }
    return utc_off;
}

uint32_t sim_sleep_rtc(void)
{
    return (uint32_t)(time(NULL) - Y2000_EPOCH + sim_rtc_utc_off()
                      + sim_rtc_offset_s);
}

void sim_rtc_set(uint32_t rtc)
{
    sim_rtc_offset_s = (long)rtc - (long)(time(NULL) - Y2000_EPOCH)
                       - sim_rtc_utc_off();
}

static bool vbus_level(void)
{
    return vbus_raw || usb_inserted()
#if CONFIG_CHARGING
        || charger_input_state >= CHARGER_PLUGGED
#endif
        ;
}

static bool hold_level(void)
{
#ifdef HAS_BUTTON_HOLD
    return hold_button_state;
#else
    return false;
#endif
}

static const char *const why_names[] = {
    "none", "button", "vbus", "alarm", "deep", "lowbatt", "hold", "bcm_cut",
};

/* The sleep's own clock: the RTC at entry plus the warps, so slept_s
 * and the polls never depend on how long the host took */
struct vclock {
    uint32_t now;
    unsigned pos;                       /* wheel position 0-95 */
    uint8_t held;                       /* WBTN_* down now */
};

static uint32_t wheel_pkt(uint8_t held, unsigned pos, bool touch)
{
    return WHEEL_PKT_BUTTONS | ((uint32_t)held << WHEEL_BTN_SHIFT)
         | ((uint32_t)(pos % 96) << 16) | (touch ? 0x40000000u : 0);
}

static enum sleep_wake on_wheel(struct sleep_core_state *st,
                                struct vclock *vc, bool touch)
{
    struct sleep_core_ev ev = {
        .src = SRC_WHEEL, .pkt_ready = true,
        .pkt = wheel_pkt(vc->held, vc->pos, touch),
        .hold_on = hold_level(), .vbus = vbus_level(),
        .rtc_ok = true, .rtc = vc->now,
    };
    return sleep_classify(st, &ev);
}

/* Time passes: polls at the device's deadlines until a wake or the end */
static enum sleep_wake on_warp(struct sleep_core_state *st,
                               struct vclock *vc, uint32_t s)
{
    const struct sleep_core_req *rq = st->rq;

    while (s > 0)
    {
        uint32_t d = sleep_poll_deadline(rq, vc->now - st->rtc_entry,
                                         vc->now);
        struct sleep_core_ev ev;
        enum sleep_wake w;

        if (d > s)
        {
            vc->now += s;
            break;
        }
        vc->now += d;
        s -= d;
        ev = (struct sleep_core_ev){
            .src = SRC_TIMER, .hold_on = hold_level(),
            .vbus = vbus_level(), .rtc_ok = true, .rtc = vc->now,
            .batt_ok = true, .mv = (uint16_t)_battery_voltage(),
        };
        w = sleep_classify(st, &ev);
        if (w != WAKE_NONE)
            return w;
    }
    return WAKE_NONE;
}

int sim_sleep_refusal(void)
{
    if (pll_hold_until && TIME_BEFORE(current_tick, pll_hold_until))
        return SLEEP_REFUSE_BOOST;
    pll_hold_until = 0;
    if (pcm_is_playing())
        return SLEEP_REFUSE_PCM;
    return SLEEP_REFUSE_NONE;
}

void sim_sleep_run(const struct sleep_core_req *rq,
                   struct sim_sleep_result *out)
{
    struct sleep_core_state st;
    struct sleep_core_req alarm_rq;
    struct vclock vc = { .now = sim_sleep_rtc(),
                         .held = rq->buttons_at_entry & WHEEL_BTN_BITS };
    enum sleep_wake w = WAKE_NONE;
    int refused = sim_sleep_refusal();

    if (refused)
    {
        /* as power_sleep(): WAKE_DEEP with the reason, nothing slept; not
         * a wake (expect_sleep never sees it) */
        *out = (struct sim_sleep_result){ .why = WAKE_DEEP,
                                          .refused = (uint8_t)refused,
                                          .sdram_crc_ok = 2 };
        fprintf(stderr, "sim_sleep: sleep refused why=%d\n", refused);
        return;
    }

    ring_rd = ring_wr;
    if (sim_alarm_in && !rq->alarm_rtc) /* alarm_in, until P12 sets one */
    {
        alarm_rq = *rq;
        alarm_rq.alarm_rtc = vc.now + sim_alarm_in;
        rq = &alarm_rq;
    }
    sleep_core_begin(&st, rq, vc.now, hold_level(), vbus_level());
    asleep = true;
    fprintf(stderr, "sim_sleep: sleep enter deep=%lu alarm=%lu bcm_cut=%lu "
            "hold_wake=%lu low_mv=%u hold=%d\n",
            (unsigned long)rq->deep_after_s,
            (unsigned long)(rq->alarm_rtc ? rq->alarm_rtc - vc.now : 0),
            (unsigned long)rq->bcm_cut_after_s,
            (unsigned long)rq->hold_wake_s, rq->low_mv, hold_level());

    while (w == WAKE_NONE)
    {
        if (ring_rd == ring_wr)
        {
            /* levels: Hold and VBUS interrupt on the device (GPIO) */
            if (hold_level() != st.hold_on || vbus_level() != st.vbus)
            {
                struct sleep_core_ev ev = {
                    .src = SRC_GPIO, .hold_on = hold_level(),
                    .vbus = vbus_level(), .rtc_ok = true, .rtc = vc.now,
                };
                w = sleep_classify(&st, &ev);
                continue;
            }
            sleep(1);
            continue;
        }

        struct sev e = ring[ring_rd++ % RING];

        switch (e.type)
        {
        case EV_KEY:
            if (e.down)
                vc.held |= e.bits;
            else
                vc.held &= (uint8_t)~e.bits;
            w = on_wheel(&st, &vc, false);
            break;
        case EV_TURN:
            for (int n = abs(e.arg); n > 0 && w == WAKE_NONE; n--)
            {
                vc.pos = (vc.pos + (e.arg < 0 ? 92 : 4)) % 96;
                w = on_wheel(&st, &vc, true);
            }
            break;
        case EV_TOUCH:
            w = on_wheel(&st, &vc, true);
            break;
        case EV_WARP:
            w = on_warp(&st, &vc, (uint32_t)e.arg);
            break;
        }
    }

    asleep = false;
    *out = (struct sim_sleep_result){
        .why = w, .slept_s = vc.now - st.rtc_entry,
        .timer_wakes = st.timer_wakes, .spurious = st.spurious,
        .resyncs = st.resyncs, .wake_buttons = st.wake_buttons,
        .hold_edges = st.hold_edges,
        .last_mv = (uint16_t)_battery_voltage(), .refused = 0,
        .sdram_crc_ok = 2,
    };
    /* PLAY held into the sleep (Rev 4 N6) and released while asleep:
     * the driver's state catches up at the wake, as on the device. The
     * release goes first and the button tick sees it on its own: in one
     * tick with the waking press, button.c posts no release at all
     * ((btn & diff) != 0), and the swallow mask would keep PLAY */
    bool released = false;

    for (uint8_t b = WBTN_SELECT; b <= WBTN_MENU; b <<= 1)
        if (rq->buttons_at_entry & ~vc.held & b)
        {
            sim_inject_key(bit_key(b), false);
            released = true;
        }
    if (released)
        sleep(2);                       /* two button ticks */
    /* as clickwheel_sleep_exit(): the driver sees the waking buttons
     * still down, so their release reaches the shell's swallow mask */
    for (uint8_t b = WBTN_SELECT; b <= WBTN_MENU; b <<= 1)
        if (st.wake_buttons & vc.held & b)
            sim_inject_key(bit_key(b), true);
    last_why = w;
    if (w == WAKE_ALARM)
        sim_alarm_in = 0;
    fprintf(stderr, "sim_sleep: sleep wake why=%d s=%lu polls=%u "
            "spurious=%u resyncs=%u buttons=%#x holds=%u (%s)\n", w,
            (unsigned long)out->slept_s, out->timer_wakes, out->spurious,
            out->resyncs, out->wake_buttons, out->hold_edges, why_names[w]);
}

static int parse_why(const char *s)
{
    for (unsigned i = 0; i < ARRAYLEN(why_names); i++)
        if (!strcmp(s, why_names[i]))
            return i;
    if (s[0] >= '0' && s[0] <= '7' && !s[1])
        return s[0] - '0';
    return -1;
}

static void expect_sleep(const char *what, long wait_ms, int lineno)
{
    int want = -1;
    long waited = 0;
    bool ok;

    if (strcmp(what, "asleep") && strcmp(what, "awake")
        && (want = parse_why(what)) < 0)
    {
        fprintf(stderr, "sim_script:%d: expect_sleep asleep|awake|<why>\n",
                lineno);
        return;
    }
    for (;;)
    {
        if (!strcmp(what, "asleep"))
            ok = asleep;
        else if (!strcmp(what, "awake"))
            ok = !asleep;
        else
            ok = last_why == want;      /* the shell may sleep again */
        if (ok || waited >= wait_ms)
            break;
        sleep(HZ / 20);
        waited += 50;
    }
    if (ok)
        fprintf(stderr, "sim_script: expect_sleep ok: %s after %ld ms\n",
                what, waited);
    else
        fprintf(stderr, "sim_script:%d: expect_sleep failed: want %s, %s, "
                "last wake %s\n", lineno, what, asleep ? "asleep" : "awake",
                last_why < 0 ? "none" : why_names[last_why]);
    if (want >= 0 && ok)
        last_why = -1;                  /* each wake is expected once */
}

#else /* !HAVE_IPOD_UI */

bool sim_sleep_key(int sdl_key, bool down)
{
    (void)sdl_key; (void)down;
    return false;
}

bool sim_sleep_wheel(int clicks)
{
    (void)clicks;
    return false;
}

bool sim_sleep_touch(void)
{
    return false;
}

#endif /* HAVE_IPOD_UI */

bool sim_cmd_sleep(const char *cmd, char *argv[], int argc, int lineno)
{
    if (strcmp(cmd, "sleep_warp") && strcmp(cmd, "rtc_set")
        && strcmp(cmd, "vbus") && strcmp(cmd, "batt_mv")
        && strcmp(cmd, "wheel_touch") && strcmp(cmd, "expect_sleep")
        && strcmp(cmd, "sleep_expect") && strcmp(cmd, "alarm_in")
        && strcmp(cmd, "idle_poweroff") && strcmp(cmd, "sleep_mode")
        && strcmp(cmd, "power_cut") && strcmp(cmd, "pll_hold"))
        return false;
#ifndef HAVE_IPOD_UI
    (void)argv; (void)argc;
    fprintf(stderr, "sim_script:%d: %s unavailable\n", lineno, cmd);
    return true;
#else
    if (!strcmp(cmd, "power_cut"))
    {
        /* the battery dies: no shutdown path runs, nothing more is
         * written; scripts/shots.sh boots the next part ("reboot" after
         * it) on the same disk */
        fprintf(stderr, "sim_script: power_cut\n");
        fflush(NULL);
        _exit(0);
    }
    if (!strcmp(cmd, "wheel_touch"))
    {
        /* the video build's handler (sim_cmd_video.c) runs first and
         * routes through sim_sleep_touch() too */
        extern void sim_wheel_touch(void);
        bool slept = sim_sleep_touch();

        if (!slept)
            sim_wheel_touch();          /* the driver's touch count */
        fprintf(stderr, "sim_script: wheel_touch%s\n",
                slept ? " (asleep)" : "");
        return true;
    }
    if (argc < 2)
    {
        fprintf(stderr, "sim_script:%d: %s needs an argument\n", lineno, cmd);
        return true;
    }
    if (!strcmp(cmd, "sleep_warp"))
    {
        long s = atol(argv[1]);

        if (s <= 0)
        {
            fprintf(stderr, "sim_script:%d: sleep_warp <s>\n", lineno);
            return true;
        }
        sim_rtc_offset_s += s;
        sim_clock_advance(s);
        if (asleep)
        {
            post((struct sev){ .type = EV_WARP, .arg = (int32_t)s });
            drain();
        }
        fprintf(stderr, "sim_script: sleep_warp %ld%s\n", s,
                asleep ? "" : " (awake: the clock only)");
    }
    else if (!strcmp(cmd, "idle_poweroff"))
    {
        int min = atoi(argv[1]);

        global_settings.poweroff = min; /* the setting: a later
                                         * settings_apply() keeps it */
        set_poweroff_timeout(min);      /* minutes, as the setting */
        reset_poweroff_timer();
        fprintf(stderr, "sim_script: idle_poweroff %d\n", min);
    }
    else if (!strcmp(cmd, "sleep_mode"))
    {
        sim_sleep_mode = !strcmp(argv[1], "soft") ? 1
                       : !strcmp(argv[1], "off") ? 0 : -1;
        fprintf(stderr, "sim_script: sleep_mode %s\n", argv[1]);
    }
    else if (!strcmp(cmd, "pll_hold"))
    {
        long ms = atol(argv[1]);

        pll_hold_until = ms > 0 ? current_tick + ms * HZ / 1000 : 0;
        if (ms > 0 && !pll_hold_until)
            pll_hold_until = 1;
        fprintf(stderr, "sim_script: pll_hold %ld\n", ms);
    }
    else if (!strcmp(cmd, "alarm_in"))
    {
        long s = atol(argv[1]);

        sim_alarm_in = s > 0 ? (uint32_t)s : 0;
        fprintf(stderr, "sim_script: alarm_in %ld\n", s);
    }
    else if (!strcmp(cmd, "rtc_set"))
    {
        sim_rtc_set((uint32_t)atol(argv[1]));
        fprintf(stderr, "sim_script: rtc_set %s\n", argv[1]);
    }
    else if (!strcmp(cmd, "vbus"))
    {
        vbus_raw = atoi(argv[1]) != 0;
        fprintf(stderr, "sim_script: vbus %d\n", vbus_raw);
    }
    else if (!strcmp(cmd, "batt_mv"))
    {
        int mv = strcmp(argv[1], "off") ? atoi(argv[1]) : -1;

        if (mv != -1 && (mv < 2500 || mv > 4500))
        {
            fprintf(stderr, "sim_script:%d: batt_mv <2500-4500>|off\n",
                    lineno);
            return true;
        }
        sim_battery_pin_mv(mv);
        fprintf(stderr, "sim_script: batt_mv %s\n", argv[1]);
    }
    else    /* expect_sleep, sleep_expect */
        expect_sleep(argv[1], argc >= 3 ? atol(argv[2]) : 3000, lineno);
    return true;
#endif
}
