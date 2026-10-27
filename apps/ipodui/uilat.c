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

/* np-lag: the Now Playing latency probe (uilat.h) */
#include "config.h"

#if defined(HAVE_DEVSTATS) && !defined(SIMULATOR)
#include <stdbool.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "backlight.h"
#include "devstats.h"
#include "ipodui/fe_be.h"      /* P19 19-9: the UI thread's name */
#include "ipodui/uilog.h"
#include "ipodui/clockpol.h"
#include "ipodui/screen_stack.h"
#include "ipodui/screens/nowplaying.h"
#include "ipodui/uilat.h"

#define UILAT_WIN       (60 * HZ)
#define UILAT_STALE_US  2000000ul   /* a post older than this: not ours */

static struct {
    long t0;                        /* the window's start; 0: none */
    uint32_t codec0, main0, idle0, cidle0, us0;
    unsigned long ib_n0, ib_t0;
    bool pending;                   /* an event waits for its pixels */
    uint32_t post_us;
    uint32_t n, sum_ms, max_ms, q_sum_ms, q_max_ms, o33, o100;
    uint32_t frames, late;
} w;

static bool np_shown(void)
{
    struct ui_screen *s = ui_top();

    return s && s->ops == &ui_nowplaying_screen && is_backlight_on(true);
}

/* P19 19-9: the UI's thread: "main" inline, "ui" once split off */
static const char *ui_name(void)
{
    return fe_be_mode() == FE_INLINE ? "main" : "ui";
}

/* Sums the CPU time of the threads named 'name' (one each here) */
static uint32_t thread_us(const char *name)
{
    char buf[16];
    uint32_t us = 0;
    unsigned int i;

    for (i = 0; i < DEVSTATS_SLOTS; i++)
    {
        devstats_slot_name(i, buf, sizeof(buf));
        if (!strcmp(buf, name))
            us += devstats.t[i].cpu_us;
    }
    return us;
}

static void win_start(void)
{
    memset(&w, 0, sizeof(w));
    w.t0 = current_tick ? current_tick : 1;
    w.us0 = USEC_TIMER;
    w.codec0 = thread_us("codec");
    w.main0 = thread_us(ui_name());
    w.idle0 = devstats.idle_us[CPU];
    w.cidle0 = devstats.idle_us[COP];
    clockpol_input_stats(&w.ib_n0, &w.ib_t0);
}

void uilat_input(long btn)
{
    uint32_t now, post;

    if (!(btn & (BUTTON_SCROLL_FWD | BUTTON_SCROLL_BACK))
        || (btn & SYS_EVENT) || !np_shown())
        return;
    if (!w.t0)
        win_start();
    if (w.pending)
        return;                     /* coalesced: timed from the first */
    now = USEC_TIMER;
    post = button_wheel_post_usec();
    if ((uint32_t)(now - post) > UILAT_STALE_US)
        post = now;
    w.pending = true;
    w.post_us = post;
    w.q_sum_ms += (now - post) / 1000;
    if ((now - post) / 1000 > w.q_max_ms)
        w.q_max_ms = (now - post) / 1000;
}

void uilat_pixels(void)
{
    uint32_t ms;

    if (!w.pending)
        return;
    w.pending = false;
    ms = (uint32_t)(USEC_TIMER - w.post_us) / 1000;
    w.n++;
    w.sum_ms += ms;
    if (ms > w.max_ms)
        w.max_ms = ms;
    w.o33 += ms > 33;
    w.o100 += ms > 100;
}

void uilat_frame(bool late)
{
    if (!np_shown())
        return;
    if (!w.t0)
        win_start();
    w.frames++;
    w.late += late;
}

static unsigned pct(uint32_t part, uint32_t whole)
{
    return whole ? (unsigned)((uint64_t)part * 100 / whole) : 0;
}

void uilat_service(void)
{
    uint32_t el;
    unsigned long ib_n, ib_t;

    if (!w.t0 || current_tick - w.t0 < UILAT_WIN)
        return;
    el = USEC_TIMER - w.us0;
    clockpol_input_stats(&ib_n, &ib_t);
    ui_log("uilat", "np n=%lu avg=%lu max=%lu q=%lu/%lu o33=%lu o100=%lu",
           (unsigned long)w.n,
           (unsigned long)(w.n ? w.sum_ms / w.n : 0),
           (unsigned long)w.max_ms,
           (unsigned long)(w.n ? w.q_sum_ms / w.n : 0),
           (unsigned long)w.q_max_ms, (unsigned long)w.o33,
           (unsigned long)w.o100);
    ui_log("uilat", "cpu codec=%u%% main=%u%% idle=%u%% s=%lu ui=%s "
           "cop_idle=%u%%",
           pct(thread_us("codec") - w.codec0, el),
           pct(thread_us(ui_name()) - w.main0, el),
           pct(devstats.idle_us[CPU] - w.idle0, el),
           (unsigned long)(el / 1000000), fe_be_mode_name(fe_be_mode()),
           pct(devstats.idle_us[COP] - w.cidle0, el));
    ui_log("uilat", "fr n=%lu late=%lu ib=%lu/%lums",
           (unsigned long)w.frames, (unsigned long)w.late,
           ib_n - w.ib_n0, (ib_t - w.ib_t0) * (1000 / HZ));
    w.t0 = 0;                       /* the next NP event opens a window */
}
#endif /* HAVE_DEVSTATS && !SIMULATOR */
