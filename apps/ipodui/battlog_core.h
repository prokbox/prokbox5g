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

#ifndef _IPODUI_BATTLOG_CORE_H_
#define _IPODUI_BATTLOG_CORE_H_

/* The battery session line (P19 PW4; docs/research/bcm2722/
 * battery-levers.md section 4, "The long-run record"): one ipodui.log line
 * a session (power-on or wake to power-off or sleep) and at most one an
 * hour, so normal use builds the record that battery life on battery is
 * correlated with over days:
 *
 *   batt session start=<%>/<mV> end=<%>/<mV> min lit=<> dark=<> play=<>
 *        vc_mhz=<> vc_lvl=<> vc_idle=<permille or ->
 *
 * lit and dark are the awake minutes with the backlight on and off; play
 * the minutes the audio played.  The video chip's figures cover the lit
 * time (it is powered while the screen is lit; dark, it is cut): the
 * governed video plays' time-weighted MHz and level (vplayer_vclk_totals)
 * and, for the rest of the lit time, the crystal (27 MHz) at its level;
 * its idle share is the governed plays' meter, "-" without one.
 *
 * Pure: battlog.c samples and writes, tests/battlog tests this. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define BL_PERIOD_MS    3600000u    /* a line at least this often */

struct bl_acc
{
    int start_pct, start_mv;
    uint32_t lit_ms, dark_ms, play_ms;
    /* the video plays' totals when the period began (vplayer.h) */
    uint64_t v_mhz_ms, v_lvl10_ms, v_busy_ms, v_el_ms, v_ms;
};

struct bl_video
{
    uint64_t mhz_ms, lvl10_ms, busy_ms, el_ms, ms;
};

static inline void bl_begin(struct bl_acc *a, int pct, int mv,
                            const struct bl_video *v)
{
    a->start_pct = pct;
    a->start_mv = mv;
    a->lit_ms = a->dark_ms = a->play_ms = 0;
    a->v_mhz_ms = v->mhz_ms;
    a->v_lvl10_ms = v->lvl10_ms;
    a->v_busy_ms = v->busy_ms;
    a->v_el_ms = v->el_ms;
    a->v_ms = v->ms;
}

static inline void bl_add(struct bl_acc *a, uint32_t ms, bool lit,
                          bool playing)
{
    if (lit)
        a->lit_ms += ms;
    else
        a->dark_ms += ms;
    if (playing)
        a->play_ms += ms;
}

static inline bool bl_due(const struct bl_acc *a)
{
    return a->lit_ms + a->dark_ms >= BL_PERIOD_MS;
}

/* The line, after ui_log's tag "batt"; xtal_level: the crystal's level
 * (11 after a start, 5 with the PW3 lever) */
static inline int bl_line(char *buf, int n, const struct bl_acc *a, int pct,
                          int mv, const struct bl_video *v,
                          unsigned xtal_level)
{
    uint64_t vms = v->ms - a->v_ms, lit = a->lit_ms, rest, mhz, lvl10;
    uint64_t el = v->el_ms - a->v_el_ms, busy = v->busy_ms - a->v_busy_ms;
    char idle[8];

    if (vms > lit)
        lit = vms;          /* the plays' clock and the shell's differ */
    rest = lit - vms;
    mhz = lit ? ((v->mhz_ms - a->v_mhz_ms) + rest * 27) / lit : 0;
    lvl10 = lit ? ((v->lvl10_ms - a->v_lvl10_ms) + rest * xtal_level * 10) /
                  lit : 0;
    if (el)
        snprintf(idle, sizeof(idle), "%lu",
                 (unsigned long)(1000 - (busy > el ? el : busy) * 1000 / el));
    else
        snprintf(idle, sizeof(idle), "-");
    return snprintf(buf, n, "session start=%d/%d end=%d/%d min lit=%lu "
                    "dark=%lu play=%lu vc_mhz=%lu vc_lvl=%lu.%lu vc_idle=%s",
                    a->start_pct, a->start_mv, pct, mv,
                    (unsigned long)(a->lit_ms / 60000),
                    (unsigned long)(a->dark_ms / 60000),
                    (unsigned long)(a->play_ms / 60000), (unsigned long)mhz,
                    (unsigned long)(lvl10 / 10), (unsigned long)(lvl10 % 10),
                    idle);
}

#endif /* _IPODUI_BATTLOG_CORE_H_ */
