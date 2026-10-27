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

/* Phase 18-1: the pure half of the soft sleep; see sleep_wake_core.h */

#include "sleep_wake_core.h"

enum wheel_pkt sleep_wheel_packet(bool pkt_ready, uint32_t pkt,
                                  uint8_t *prev, uint8_t *press_edge)
{
    uint8_t btn;

    *press_edge = 0;
    if (!pkt_ready)
        return PKT_NONE;                /* an I2C completion, or noise */
    if (pkt == WHEEL_PKT_ALL_ONES || (pkt & 0xFFu) == WHEEL_PKT_TYPE_3A)
        return PKT_IGNORED;
    if ((pkt & WHEEL_PKT_MASK) != WHEEL_PKT_BUTTONS)
        return PKT_OUT_OF_STEP;
    btn = (pkt >> WHEEL_BTN_SHIFT) & WHEEL_BTN_BITS;
    *press_edge = btn & (uint8_t)~*prev;
    *prev = btn;
    return PKT_VALID;
}

void sleep_core_begin(struct sleep_core_state *st,
                      const struct sleep_core_req *rq, uint32_t rtc_entry,
                      bool hold_on, bool vbus)
{
    *st = (struct sleep_core_state){ 0 };
    st->rq = rq;
    st->rtc_entry = rtc_entry;
    st->win_start = rtc_entry;
    st->prev_buttons = rq->buttons_at_entry & WHEEL_BTN_BITS;
    st->hold_on = hold_on;
    st->vbus = vbus;
}

/* Higher wins */
static int wake_rank(enum sleep_wake w)
{
    switch (w)
    {
    case WAKE_LOWBATT:  return 7;
    case WAKE_VBUS:     return 6;
    case WAKE_ALARM:    return 5;
    case WAKE_BUTTON:   return 4;
    case WAKE_HOLD:     return 3;
    case WAKE_DEEP:     return 2;
    case WAKE_BCM_CUT:  return 1;
    default:            return 0;
    }
}

static void pick(enum sleep_wake *best, enum sleep_wake w)
{
    if (wake_rank(w) > wake_rank(*best))
        *best = w;
}

/* bad: toward the storm cap too (no valid packet, or a GPIO that
 * changed nothing); a valid packet without a press edge is not */
static void count_spurious(struct sleep_core_state *st, bool bad)
{
    if (st->spurious < UINT16_MAX)
        st->spurious++;
    if (bad && st->win_bad < UINT16_MAX)
        st->win_bad++;
}

enum sleep_wake sleep_classify(struct sleep_core_state *st,
                               const struct sleep_core_ev *ev)
{
    const struct sleep_core_req *rq = st->rq;
    enum sleep_wake best = WAKE_NONE;
    bool changed = false;

    st->act = 0;
    if (ev->rtc_ok && ev->rtc >= st->rtc_entry)
    {
        st->slept_s = ev->rtc - st->rtc_entry;
        if (ev->rtc - st->win_start >= SLEEP_SPURIOUS_WIN_S)
        {
            st->win_start = ev->rtc;
            st->win_bad = 0;
        }
    }

    if (ev->src & SRC_TIMER)
    {
        st->timer_s += ev->poll_s;
        st->quiet_pkts = 0;
    }
    if (st->timer_s > st->slept_s)
        st->slept_s = st->timer_s;      /* a frozen RTC still ends a sleep */

    if (ev->src & SRC_TIMER)
    {
        if (st->timer_wakes < UINT16_MAX)
            st->timer_wakes++;
        if (!ev->rtc_ok || (rq->low_mv && !ev->batt_ok))
            pick(&best, WAKE_DEEP);     /* raw read failed (§3.1) */
        else if (rq->low_mv && ev->mv <= rq->low_mv)
            pick(&best, WAKE_LOWBATT);
    }

    /* Hold and VBUS are levels: an edge is seen by any wake that read
     * them, the poll being the backstop (§3.1) */
    if (ev->hold_on != st->hold_on)
    {
        changed = true;
        st->hold_on = ev->hold_on;
        if (st->hold_edges < UINT8_MAX)
            st->hold_edges++;
        st->act |= ev->hold_on ? SACT_OPTO_OFF : SACT_OPTO_INIT;
        if (!ev->hold_on && (rq->wake_on_hold
                             || st->slept_s < rq->hold_wake_s))
            pick(&best, WAKE_HOLD);
    }
    if (ev->vbus != st->vbus)
    {
        changed = true;
        st->vbus = ev->vbus;
        if (ev->vbus)
            pick(&best, WAKE_VBUS);
    }
    if ((ev->src & SRC_GPIO) && !changed)
        count_spurious(st, true);

    if (ev->src & SRC_WHEEL)
    {
        uint8_t edge;

        enum wheel_pkt k = sleep_wheel_packet(ev->pkt_ready, ev->pkt,
                                              &st->prev_buttons, &edge);

        if (k == PKT_OUT_OF_STEP)
        {
            if (st->resyncs < UINT16_MAX)
                st->resyncs++;
            if (st->win_bad < UINT16_MAX)
                st->win_bad++;
            st->act |= SACT_RESYNC;
        }
        else if (k == PKT_VALID && edge && !st->hold_on)
        {
            st->wake_buttons = edge;
            pick(&best, WAKE_BUTTON);
        }
        else    /* no packet or ignored (bad); a release, touch, turn,
                 * held button or a press under Hold (valid, not bad) */
        {
            count_spurious(st, k != PKT_VALID);
            if (k == PKT_VALID && ++st->quiet_pkts == SLEEP_QUIET_PKTS_MAX)
                st->act |= SACT_WHEEL_MUTE;
        }
    }

    if (ev->rtc_ok && rq->alarm_rtc && ev->rtc >= rq->alarm_rtc)
        pick(&best, WAKE_ALARM);
    if (rq->deep_after_s && st->slept_s >= rq->deep_after_s)
        pick(&best, WAKE_DEEP);
    if (rq->bcm_cut_after_s && st->slept_s >= rq->bcm_cut_after_s)
        pick(&best, WAKE_BCM_CUT);
    if (st->win_bad > SLEEP_SPURIOUS_MAX)
        pick(&best, WAKE_DEEP);         /* an interrupt storm */

    if (best != WAKE_BUTTON)
        st->wake_buttons = 0;
    return best;
}

static uint32_t left(uint32_t total, uint32_t done)
{
    return total > done ? total - done : 0;
}

uint32_t sleep_poll_deadline(const struct sleep_core_req *rq,
                             uint32_t slept_s, uint32_t rtc_now)
{
    uint32_t d = rq->poll_s ? rq->poll_s : SLEEP_POLL_S;
    uint32_t t;

    if (rq->alarm_rtc && (t = left(rq->alarm_rtc, rtc_now)) < d)
        d = t;
    if (rq->deep_after_s && (t = left(rq->deep_after_s, slept_s)) < d)
        d = t;
    if (rq->bcm_cut_after_s
        && (t = left(rq->bcm_cut_after_s, slept_s)) < d)
        d = t;
    return d;
}

void sleep_req_remaining(const struct sleep_core_req *in, uint32_t slept_s,
                         struct sleep_core_req *out)
{
    *out = *in;
    out->bcm_cut_after_s = 0;
    if (in->deep_after_s)
    {
        out->deep_after_s = left(in->deep_after_s, slept_s);
        if (!out->deep_after_s)
            out->deep_after_s = 1;
    }
    out->hold_wake_s = left(in->hold_wake_s, slept_s);
}

uint32_t sleep_rtc_from_date(int year, int mon, int day,
                             int hour, int min, int sec)
{
    static const uint16_t before[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    int y = year - 2000;
    /* 2000 is a leap year: the leap days before this year are
     * (y + 3) / 4 (every fourth year to 2099) */
    uint32_t days = (uint32_t)y * 365 + (uint32_t)(y + 3) / 4
                  + before[mon - 1] + (uint32_t)(day - 1);

    if (mon > 2 && (y & 3) == 0)
        days++;
    return ((days * 24 + (uint32_t)hour) * 60 + (uint32_t)min) * 60
           + (uint32_t)sec;
}

static bool bcd(uint8_t v, int lo, int hi, int *out)
{
    if ((v & 0x0F) > 9 || (v >> 4) > 9)
        return false;
    *out = (v >> 4) * 10 + (v & 0x0F);
    return *out >= lo && *out <= hi;
}

bool sleep_rtc_from_bcd(const uint8_t r[7], uint32_t *s)
{
    int sec, min, hour, day, mon, year;

    if (!bcd(r[0] & 0x7F, 0, 59, &sec) || !bcd(r[1] & 0x7F, 0, 59, &min)
        || !bcd(r[2] & 0x3F, 0, 23, &hour) || !bcd(r[4] & 0x3F, 1, 31, &day)
        || !bcd(r[5] & 0x1F, 1, 12, &mon) || !bcd(r[6], 0, 99, &year))
        return false;
    *s = sleep_rtc_from_date(2000 + year, mon, day, hour, min, sec);
    return true;
}
