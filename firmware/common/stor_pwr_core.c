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

/* Drive power policy (#114, P18 A3); see firmware/export/stor_pwr_core.h */
#include <stddef.h>
#include "stor_pwr_core.h"

#define TICK_AFTER(a, b)    ((long)((b) - (a)) < 0)

void stor_pwr_init(struct stor_pwr *s, long now, bool on)
{
    s->changed = now;
    s->backoff = 0;
    s->on_ticks = 0;
    s->ons = s->offs = s->wasted = 0;
    s->ev_head = 0;
    s->fails = s->dirty = 0;
    s->fail_run = 0;
    s->dirty_run = s->gave_up = false;
    s->on = on;
}

long stor_pwr_hold(const struct stor_pwr *s, const struct stor_pwr_tune *t,
                   bool lit)
{
    long h = (lit ? t->hold_lit : t->hold_dark) + s->backoff;
    return h > t->hold_max ? t->hold_max : h;
}

bool stor_pwr_off_due(const struct stor_pwr *s,
                      const struct stor_pwr_tune *t, long now,
                      long last_access, bool lit)
{
    if (!s->on)
        return false;
    return TICK_AFTER(now, last_access + stor_pwr_hold(s, t, lit));
}

static uint16_t ticks_ms(long ticks, int tick_ms)
{
    unsigned long ms = (unsigned long)(ticks < 0 ? 0 : ticks)
                       * (unsigned long)tick_ms;
    return ms > 0xffff ? 0xffff : (uint16_t)ms;
}

static void record(struct stor_pwr *s, long now, uint16_t ms, bool on,
                   int reason)
{
    struct stor_pwr_event *e = &s->ev[s->ev_head % STOR_PWR_EVENTS];

    e->tick = (uint32_t)now;
    e->ms = ms;
    e->on = on;
    e->reason = (uint8_t)reason;
    s->ev_head++;               /* after the entry: a reader sees it whole */
}

long stor_pwr_note_off(struct stor_pwr *s, long now, int reason,
                       int tick_ms)
{
    long len = now - s->changed;

    if (!s->on)
        return 0;
    s->on = false;
    s->on_ticks += (uint32_t)(len < 0 ? 0 : len);
    s->changed = now;
    s->offs++;
    record(s, now, ticks_ms(len, tick_ms), false, reason);
    return len;
}

long stor_pwr_note_on(struct stor_pwr *s, const struct stor_pwr_tune *t,
                      long now, long reinit_ticks, int reason, int tick_ms)
{
    long len = now - reinit_ticks - s->changed;    /* the dark part */

    if (s->on)
        return 0;
    if (len < 0)
        len = 0;
    if (len < t->min_off)
    {
        /* a wasted cycle: hold twice as long next time (from one dark
         * hold up to hold_max) */
        long room = t->hold_max - t->hold_dark;
        s->backoff = s->backoff ? s->backoff * 2 : t->hold_dark;
        if (s->backoff > room)
            s->backoff = room > 0 ? room : 0;
        s->wasted++;
    }
    else if (len >= t->relax)
        s->backoff = 0;
    s->on = true;
    s->fail_run = 0;
    /* the on period starts when the power came back, before the re-init */
    s->changed = now - reinit_ticks;
    s->ons++;
    record(s, now, ticks_ms(reinit_ticks, tick_ms), true, reason);
    return len;
}

int stor_pwr_power_on(struct stor_pwr *s, const struct stor_pwr_tune *t,
                      const struct stor_pwr_ops *ops, int reason,
                      int tick_ms, bool *give_up)
{
    long t0 = ops->now(ops->ctx);
    int rc = ops->power_on(ops->ctx);

    if (rc)
    {
        /* one retry, from a drive powered off again */
        ops->power_off(ops->ctx);
        rc = ops->power_on(ops->ctx);
    }
    long now = ops->now(ops->ctx);
    if (!rc)
    {
        stor_pwr_note_on(s, t, now, now - t0, reason, tick_ms);
        return 0;
    }
    /* #114 H1: cut again, so the next access starts from a fresh
     * power-up rather than a drive half up */
    ops->power_off(ops->ctx);
    s->fails++;
    if (s->fail_run < 0xff)
        s->fail_run++;
    record(s, now, ticks_ms(now - t0, tick_ms), false, STOR_PWR_R_FAIL);
    if (s->fail_run >= STOR_PWR_FAIL_MAX && !s->gave_up)
    {
        s->gave_up = true;
        record(s, now, 0, true, STOR_PWR_R_GIVEUP);
        *give_up = true;
    }
    return rc;
}

bool stor_pwr_cut_safe(bool can_flush, bool flush_ok, bool wcache_on,
                       bool pm)
{
    if (!pm && !can_flush)
        return false;
    return (can_flush && flush_ok) || !wcache_on;
}

void stor_pwr_note_dirty(struct stor_pwr *s, long now)
{
    s->dirty++;
    if (!s->dirty_run)
    {
        s->dirty_run = true;
        record(s, now, 0, true, STOR_PWR_R_DIRTY);
    }
}

void stor_pwr_note_clean(struct stor_pwr *s)
{
    s->dirty_run = false;
}

uint32_t stor_pwr_on_ticks(const struct stor_pwr *s, long now)
{
    long open = s->on ? now - s->changed : 0;
    return s->on_ticks + (uint32_t)(open < 0 ? 0 : open);
}

const struct stor_pwr_event *stor_pwr_event_at(const struct stor_pwr *s,
                                               uint32_t k)
{
    if (k >= s->ev_head || s->ev_head - k > STOR_PWR_EVENTS)
        return NULL;
    return &s->ev[k % STOR_PWR_EVENTS];
}

const char *stor_pwr_reason_name(int reason)
{
    static const char *const n[STOR_PWR_R_COUNT] = {
        "idle", "access", "sleep", "usb", "api", "fail", "giveup", "dirty"
    };
    return (reason >= 0 && reason < STOR_PWR_R_COUNT) ? n[reason] : "?";
}

int stor_pwr_refill_secs(int base_s, int pwrup_s, int speed_pct)
{
    int s = base_s + pwrup_s;

    if (speed_pct > 100)        /* round up: never short at high speed */
        s = (s * speed_pct + 99) / 100;
    return s;
}
