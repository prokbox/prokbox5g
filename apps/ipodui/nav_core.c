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
 * iPod UI shell: navigation model, pure logic (#38 contracts)
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
#include <stddef.h>
#include "nav_core.h"

/* -- panel rest scheduling ---------------------------------------------- */

static bool key_eq(const struct nav_panel_key *a, const struct nav_panel_key *b)
{
    return a->ops == b->ops && a->arg == b->arg;
}

void nav_sched_init(struct nav_panel_sched *s)
{
    s->shown.ops = s->shown.arg = NULL;
    s->want = s->shown;
    s->moved = 0;
    s->rest = 0;
    s->pending = false;
}

void nav_sched_select(struct nav_panel_sched *s, const void *ops,
                      const void *arg, int rest_ticks, long now)
{
    /* every move restarts the rest delay, even back to the shown key:
     * passing items quickly never shows them (spec main-menu.md) */
    s->moved = now;
    if (!ops)
    {
        /* no preview: the old panel stays */
        s->want = s->shown;
        s->pending = false;
        return;
    }
    s->want.ops = ops;
    s->want.arg = arg;
    s->rest = rest_ticks;
    s->pending = !key_eq(&s->want, &s->shown);
}

void nav_sched_invalidate(struct nav_panel_sched *s, long now)
{
    if (!s->want.ops)
        return;
    s->moved = now;
    s->pending = true;
}

long nav_sched_due(const struct nav_panel_sched *s, long now)
{
    long left;

    if (!s->pending)
        return NAV_NEVER;
    left = s->moved + s->rest - now;
    return left > 0 ? left : 0;
}

void nav_sched_arrived(struct nav_panel_sched *s)
{
    s->shown = s->want;
    s->pending = false;
}

void nav_sched_forget(struct nav_panel_sched *s)
{
    s->shown.ops = s->shown.arg = NULL;
    s->pending = s->want.ops != NULL;
    s->moved = 0;
    s->rest = 0;                    /* due at once */
}

/* -- panel phase -------------------------------------------------------- */

void nav_phase_init(struct nav_phase *ph, long now, bool lit)
{
    ph->ticks = 0;
    ph->last = now;
    ph->lit = lit;
}

long nav_phase_update(struct nav_phase *ph, long now, bool lit)
{
    long d = now - ph->last;

    /* 'lit' is the state from now on: the stretch since 'last' counts
     * if it was lit then (callers report each backlight change) */
    if (ph->lit && d > 0)
        ph->ticks += d;
    ph->last = now;
    ph->lit = lit;
    return ph->ticks;
}

void nav_phase_set(struct nav_phase *ph, long ticks, long now)
{
    ph->ticks = ticks < 0 ? 0 : ticks;
    ph->last = now;
}

/* -- drift -------------------------------------------------------------- */

/* Direction per cycle, x then y (seen on the device: left; right+down;
 * up). A table, so a re-dwell gets a new direction too. */
static const signed char drift_dir[8][2] = {
    { -1, 0 }, { 1, 1 }, { 0, -1 }, { 1, 0 },
    { -1, -1 }, { 0, 1 }, { 1, -1 }, { -1, 1 },
};

/* Start offset on one axis: centred, so the pan ends as far past the
 * middle as it started before it. */
static int drift_start(int range, int dir)
{
    int mid = range / 2;

    if (dir > 0)
        return mid - NAV_DRIFT_PAN_MAX / 2;
    if (dir < 0)
        return mid + NAV_DRIFT_PAN_MAX / 2;
    return mid;
}

void nav_drift_pos(long phase, struct nav_drift_pos *out)
{
    const signed char *d;
    int t;

    if (phase < 0)
        phase = 0;
    out->cycle = phase / NAV_DRIFT_CYCLE;
    t = (int)(phase % NAV_DRIFT_CYCLE);
    out->t = t;
    out->half = 0;
    if (t < NAV_DRIFT_FADE)
    {
        out->fade_step = 1 + t * NAV_DRIFT_FADE_STEPS / NAV_DRIFT_FADE;
        out->pan = 0;
    }
    else
    {
        int u = t - NAV_DRIFT_FADE;

        out->fade_step = 0;
        out->pan = u / NAV_DRIFT_STEP;
        if (out->pan >= NAV_DRIFT_PAN_MAX)
            out->pan = NAV_DRIFT_PAN_MAX;
        else
            out->half = u % NAV_DRIFT_STEP >= NAV_DRIFT_HALF;
    }
    d = drift_dir[out->cycle % 8];
    out->dx = d[0];
    out->dy = d[1];
    out->x = drift_start(NAV_DRIFT_SRC - NAV_PANEL_W, d[0]) + d[0] * out->pan;
    out->y = drift_start(NAV_DRIFT_SRC - NAV_PANEL_H, d[1]) + d[1] * out->pan;
}

int nav_drift_wake(long phase)
{
    int t, next;

    if (phase < 0)
        phase = 0;
    t = (int)(phase % NAV_DRIFT_CYCLE);
    if (t < NAV_DRIFT_FADE)
    {
        /* next fade step boundary */
        int step = NAV_DRIFT_FADE / NAV_DRIFT_FADE_STEPS;
        next = (t / step + 1) * step;
    }
    else
    {
        int u = t - NAV_DRIFT_FADE, r = u % NAV_DRIFT_STEP;

        /* the next half step */
        next = NAV_DRIFT_FADE + u - r
               + (r < NAV_DRIFT_HALF ? NAV_DRIFT_HALF : NAV_DRIFT_STEP);
    }
    if (next > NAV_DRIFT_CYCLE)
        next = NAV_DRIFT_CYCLE;
    return next - t;
}

enum nav_drift_act nav_drift_plan(const struct nav_drift_in *in)
{
    if (!in->at_rest)
        return NAV_DRIFT_IDLE;          /* drop frames; the phase runs on */

    if (in->cycle != in->shown_cycle)
    {
        /* a new framing is due (on time, or late after a glide or the
         * backlight): always by a fade, never a cut */
        if (in->panel_count <= 1)
            return NAV_DRIFT_REDWELL;
        if (in->next_kept)
            return NAV_DRIFT_FADE_STRIP;
        if (in->storage_ok)
            return NAV_DRIFT_FADE_DIRECT;
        return NAV_DRIFT_REDWELL;       /* keep the cover, new direction */
    }

    /* prefetch only matters during playback (storage wakes rarely);
     * stopped, the cycle end reads directly */
    if (in->playing && in->storage_ok && !in->next_kept
        && in->panel_count > 1 && in->t >= NAV_DRIFT_PREFETCH_FROM)
        return NAV_DRIFT_PREFETCH;

    if (in->pan != in->shown_pan)
        return NAV_DRIFT_PAN;
    return NAV_DRIFT_IDLE;
}

/* -- cross-fades -------------------------------------------------------- */

int nav_xfade_weight(int k, int n)
{
    if (n < 1)
        n = 1;
    if (k >= n)
        return 256;
    if (k < 1)
        k = 1;
    /* 1/(steps left), rounded */
    return (256 + (n - k + 1) / 2) / (n - k + 1);
}

/* -- transitions -------------------------------------------------------- */

int nav_transition_ticks(int kind, int dir, bool from_split, bool to_split)
{
    switch (kind)
    {
        case 1:                         /* UI_T_REVEAL */
            return NAV_HZ * 25 / 100;
        case 2:                         /* UI_T_SPLIT */
            return dir > 0 ? NAV_HZ * 21 / 100 : NAV_HZ * 29 / 100;
        default:                        /* UI_T_PUSH */
            if (dir > 0 && from_split && !to_split)
                return NAV_HZ * 25 / 100;
            if (dir < 0 && !from_split && to_split)
                return NAV_HZ * 29 / 100;
            return NAV_HZ * 19 / 100;
    }
}

void nav_reveal_offsets(int elapsed, int total, int dir, int *out_dx,
                        int *in_x)
{
    long t;                             /* progress into Clocks, 0..1024 */

    if (total <= 0)
        total = 1;
    if (elapsed < 0)
        elapsed = 0;
    if (elapsed > total)
        elapsed = total;
    if (dir < 0)
        elapsed = total - elapsed;
    t = (long)elapsed * 1024 / total;
    *out_dx = t * 400 / 1024 > 320 ? 320 : (int)(t * 400 / 1024);
    /* 320 (1 - t)(1 - 0.4 t), in 1/1024 steps */
    *in_x = (int)(320L * (1024 - t) * (1024 - t * 4 / 10) / (1024L * 1024));
}
