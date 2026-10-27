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
 * iPod UI shell: scroll physics and wheel acceleration
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
/* Pure C on purpose: no Rockbox headers (host-tested by tests/ipodui/). */
#include "scroll.h"

#define STEP_DIV        3       /* each tick covers 1/3 of the distance */
#define STEP_MAX_TICKS  64      /* dt cap: far more than any distance needs */

#define WHEEL_ACCEL_V0  12      /* clicks/s below which 1 click = 1 step */
#define WHEEL_ACCEL_DV  12      /* clicks/s per extra step */
#define WHEEL_STEPS_MAX 5

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void ui_scroll_init(struct ui_scroll *s, int max)
{
    s->pos = s->target = s->vel = 0;
    s->max = max > 0 ? max : 0;
}

void ui_scroll_set_max(struct ui_scroll *s, int max)
{
    s->max = max > 0 ? max : 0;
    s->pos = clamp(s->pos, 0, s->max);
    s->target = clamp(s->target, 0, s->max);
}

void ui_scroll_set_target(struct ui_scroll *s, int target)
{
    s->target = clamp(target, 0, s->max);
}

bool ui_scroll_step(struct ui_scroll *s, int dt)
{
    int i;

    if (dt > STEP_MAX_TICKS)
        dt = STEP_MAX_TICKS;
    s->vel = 0;
    for (i = 0; i < dt && s->pos != s->target; i++)
    {
        int d = s->target - s->pos;
        int m = (d > 0 ? d : -d);
        m = (m + STEP_DIV - 1) / STEP_DIV;      /* >= 1, <= |d| */
        s->vel = d > 0 ? m : -m;
        s->pos += s->vel;
    }
    /* pos and target are both in [0, max] and pos only moves toward the
     * target by at most the distance, so this is a no-op guard */
    s->pos = clamp(s->pos, 0, s->max);
    return s->pos != s->target;
}

void ui_scroll_limit_lag(struct ui_scroll *s, int lag)
{
    if (lag < 0)
        lag = 0;
    if (s->pos < s->target - lag)
        s->pos = s->target - lag;
    else if (s->pos > s->target + lag)
        s->pos = s->target + lag;
}

void ui_scroll_snap(struct ui_scroll *s)
{
    s->pos = s->target;
    s->vel = 0;
}

int ui_wheel_steps(int clicks, unsigned velocity)
{
    int per = 1;

    if (clicks <= 0)
        return 0;
    if (velocity >= WHEEL_ACCEL_V0)
    {
        unsigned extra = (velocity - WHEEL_ACCEL_V0) / WHEEL_ACCEL_DV;
        per = extra >= WHEEL_STEPS_MAX - 1 ? WHEEL_STEPS_MAX : 1 + (int)extra;
    }
    return clicks * per;
}
