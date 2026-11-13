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

/* P19 19-1c: see apps/codec_place_core.h */
#include "codec_place_core.h"

bool cpl_full_legacy;

void cpl_init(struct cpl_state *s)
{
    s->screen = CPL_LIT;
    s->since = 0;
    s->seen = false;
    s->core = CPL_COP;
}

void cpl_observe(struct cpl_state *s, int screen, long now)
{
    if (s->seen && s->screen == screen)
        return;
    s->screen = screen;
    s->since = now;
    s->seen = true;
}

/* The core the screen state asks for; the last choice while dim */
static int by_screen(const struct cpl_state *s)
{
    if (!s->seen || s->screen == CPL_DIM)
        return s->core;
    return s->screen == CPL_DARK ? CPL_CPU : CPL_COP;
}

static int fixed(int mode)
{
    return mode == CPL_MODE_CPU ? CPL_CPU : CPL_COP;
}

int cpl_at_load(struct cpl_state *s, int mode, long now)
{
    (void)now;
    s->core = mode == CPL_MODE_SCREEN ? by_screen(s) : fixed(mode);
    return s->core;
}

int cpl_at_boundary(struct cpl_state *s, int mode, long now,
                    long hold_ticks)
{
    if (mode != CPL_MODE_SCREEN)
        s->core = fixed(mode);
    else if (s->seen && now - s->since >= hold_ticks)
        s->core = by_screen(s);
    return s->core;
}

bool cpl_cop_assist(int core)
{
    return core == CPL_COP;
}

long cpl_full_wait(size_t level, size_t wm, size_t byterate, long hz)
{
    unsigned long long head, margin, t;

    if (cpl_full_legacy || byterate == 0 || level <= wm)
        return hz / 20;
    head = level - wm;              /* above the boost watermark */
    margin = head / 2;              /* half the band stays as headroom */
    if (margin < byterate / 4)
        margin = byterate / 4;      /* never under 0.25 s of it */
    if (head <= margin)
        return hz / 20;             /* too thin: the old poll */
    t = (head - margin) * (unsigned long long)hz / byterate;
    if (t < 1)
        t = 1;
    if (t > (unsigned long long)hz)
        t = hz;
    return (long)t;
}

void cpl_demand_init(struct cpl_demand *d)
{
    d->ema_x10 = 0;
    d->last_x10 = 0;
    d->held = 0;
    d->want = false;
    d->seeded = false;
}

void cpl_demand_restart(struct cpl_demand *d)
{
    d->seeded = false;
}

bool cpl_demand_period(struct cpl_demand *d, uint32_t cycles,
                       uint32_t busy_us, uint32_t out_us)
{
    unsigned int m;
    bool was = d->want, sat;

    if (out_us >= 100000000u)
    {
        cycles /= 1000u;            /* 100 s of audio or more: in ms */
        busy_us /= 1000u;
        out_us /= 1000u;
    }
    if (out_us == 0)
        return false;
    /* cycles / us = MHz, in tenths (divide first: no 64-bit division;
     * the remainder times 10 fits, out_us being under 10^8) */
    m = (unsigned int)(cycles / out_us) * 10u
        + (unsigned int)((cycles % out_us) * 10u / out_us);
    d->last_x10 = m;
    /* starving: busy 90 % or more of the period at whatever clock it ran
     * (review: at 24 MHz, 100 % busy reads 24.0 and never reached a fixed
     * 30.0). busy / out >= 90 / 100, divided first: no overflow */
    sat = busy_us / CPL_DM_SAT_PCT >= out_us / 100u;
    if (!d->seeded)
    {
        /* a run's first period: judged fresh against the hold line, the
         * last run's answer forgotten (review of p083-copclock, Major: a
         * held Opus track's boost stuck through a light album, whose
         * demand read 15-21 at 80 MHz, between the lines). Its own clock
         * can only inflate it (SDRAM stalls), so under 21.0 here is under
         * 21.0 at 30 MHz too: the light run is not asked for again */
        d->ema_x10 = m;
        d->seeded = true;
        d->want = m >= CPL_DM_HOLD_X10 || sat;
        d->held = 0;
        return d->want != was;
    }
    else
        d->ema_x10 = (3u * d->ema_x10 + m) / 4u;   /* ~3.5 periods */
    if (!d->want)
    {
        if (d->ema_x10 >= CPL_DM_HOLD_X10 || sat)
        {
            d->want = true;
            d->held = 0;
        }
    }
    else
    {
        if (d->held < CPL_DM_MIN_HOLD)
            d->held++;
        /* let go only after the minimum hold, and never on a period that
         * would itself ask for the boost */
        if (d->held >= CPL_DM_MIN_HOLD && d->ema_x10 < CPL_DM_FREE_X10
            && m < CPL_DM_HOLD_X10 && !sat)
            d->want = false;
    }
    return d->want != was;
}
