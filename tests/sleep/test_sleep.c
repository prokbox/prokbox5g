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

/* Host test of Phase 18's pure sleep cores (brief §3.1, §3.2, §8):
 * apps/ipodui/sleep_wake_core.c and sleep_core.c */

#include <stdio.h>
#include <string.h>
#include "sleep_wake_core.h"
#include "ipodui/sleep_core.h"

static int fails, checks;

#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { \
            fails++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

/* A button/wheel packet: buttons (WBTN_*), position, touch */
static uint32_t pkt(uint8_t btn, unsigned pos, int touch)
{
    return WHEEL_PKT_BUTTONS | ((uint32_t)btn << WHEEL_BTN_SHIFT)
         | ((uint32_t)(pos & 0x7F) << 16) | (touch ? 0x40000000u : 0);
}

#define T0 800000000u   /* entry RTC, seconds since 2000 */

static const struct sleep_core_req base_rq = {
    .deep_after_s = 86400, .hold_wake_s = SLEEP_HOLD_WAKE_S,
    .low_mv = 3300, .poll_s = SLEEP_POLL_S,
};

/* A wheel interrupt at t seconds into the sleep, RTC read (Hold off) */
static struct sleep_core_ev ev_wheel(bool ready, uint32_t p, uint32_t t)
{
    return (struct sleep_core_ev){ .src = SRC_WHEEL, .pkt_ready = ready,
                                   .pkt = p, .rtc_ok = true, .rtc = T0 + t };
}

static struct sleep_core_ev ev_timer(uint32_t t, uint16_t mv)
{
    return (struct sleep_core_ev){ .src = SRC_TIMER, .rtc_ok = true,
                                   .rtc = T0 + t, .batt_ok = true,
                                   .mv = mv };
}

/* ---- sleep_wheel_packet() ---- */
static void test_wheel_packet(void)
{
    static const struct {
        const char *name;
        bool ready;
        uint32_t pkt;
        uint8_t prev, want_prev, want_edge;
        enum wheel_pkt want;
    } rows[] = {
        { "PLAY release after the hold", true, 0, WBTN_PLAY, 0, 0,
          PKT_VALID },
        { "bare touch", true, 0, 0, 0, 0, PKT_VALID },
        { "wheel turn", true, 0, 0, 0, 0, PKT_VALID },
        { "button still held", true, 0, WBTN_SELECT, WBTN_SELECT, 0,
          PKT_VALID },
        { "press edge", true, 0, 0, WBTN_SELECT, WBTN_SELECT, PKT_VALID },
        { "two buttons at once", true, 0, 0, WBTN_MENU | WBTN_PLAY,
          WBTN_MENU | WBTN_PLAY, PKT_VALID },
        { "second button while first held", true, 0, WBTN_LEFT,
          WBTN_LEFT | WBTN_RIGHT, WBTN_RIGHT, PKT_VALID },
        { "no packet ready (I2C completion)", false, 0, WBTN_PLAY,
          WBTN_PLAY, 0, PKT_NONE },
        { "0xFFFFFFFF", true, 0xFFFFFFFFu, 0, 0, 0, PKT_IGNORED },
        { "type 0x3A", true, 0x8000013Au, 0, 0, 0, PKT_IGNORED },
        { "type 0x3A without bit 31", true, 0x0000003Au, 0, 0, 0,
          PKT_IGNORED },
        { "out of step (bit 31 clear)", true, 0x0000011Au, 0, 0, 0,
          PKT_OUT_OF_STEP },
        { "out of step (other type)", true, 0x80000122u, 0, 0, 0,
          PKT_OUT_OF_STEP },
    };
    /* the packets the rows describe */
    uint32_t pkts[] = {
        pkt(0, 10, 1), pkt(0, 10, 1), pkt(0, 42, 1), pkt(WBTN_SELECT, 0, 0),
        pkt(WBTN_SELECT, 0, 0), pkt(WBTN_MENU | WBTN_PLAY, 0, 0),
        pkt(WBTN_LEFT | WBTN_RIGHT, 0, 0),
    };

    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++)
    {
        uint8_t prev = rows[i].prev, edge = 0xEE;
        uint32_t p = i < sizeof pkts / sizeof pkts[0] ? pkts[i]
                                                      : rows[i].pkt;
        enum wheel_pkt k = sleep_wheel_packet(rows[i].ready, p, &prev,
                                              &edge);

        CHECK(k == rows[i].want, "%s: class %d want %d", rows[i].name, k,
              rows[i].want);
        CHECK(edge == rows[i].want_edge, "%s: edge %#x want %#x",
              rows[i].name, edge, rows[i].want_edge);
        CHECK(prev == rows[i].want_prev, "%s: prev %#x want %#x",
              rows[i].name, prev, rows[i].want_prev);
    }
}

/* ---- sleep_classify(): one event from a fresh sleep ---- */
struct crow {
    const char *name;
    struct sleep_core_ev ev;
    bool hold_at_entry, vbus_at_entry;
    enum sleep_wake want;
    uint8_t want_buttons;
    uint16_t want_spurious, want_resyncs;
};

static void run_rows(const struct crow *rows, unsigned n,
                     const struct sleep_core_req *rq)
{
    for (unsigned i = 0; i < n; i++)
    {
        struct sleep_core_state st;
        enum sleep_wake w;

        sleep_core_begin(&st, rq, T0, rows[i].hold_at_entry,
                         rows[i].vbus_at_entry);
        w = sleep_classify(&st, &rows[i].ev);
        CHECK(w == rows[i].want, "%s: why %d want %d", rows[i].name, w,
              rows[i].want);
        CHECK(st.wake_buttons == rows[i].want_buttons,
              "%s: wake_buttons %#x want %#x", rows[i].name,
              st.wake_buttons, rows[i].want_buttons);
        CHECK(st.spurious == rows[i].want_spurious,
              "%s: spurious %u want %u", rows[i].name, st.spurious,
              rows[i].want_spurious);
        CHECK(st.resyncs == rows[i].want_resyncs, "%s: resyncs %u want %u",
              rows[i].name, st.resyncs, rows[i].want_resyncs);
    }
}

static void test_classify_single(void)
{
    struct sleep_core_ev both_low = ev_timer(60, 3200);
    struct sleep_core_ev hold_press = ev_wheel(true, pkt(WBTN_SELECT, 0, 0),
                                               5);
    struct sleep_core_ev vbus_press = ev_wheel(true, pkt(WBTN_SELECT, 0, 0),
                                               5);
    struct sleep_core_ev gpio_none = { .src = SRC_GPIO, .rtc_ok = true,
                                       .rtc = T0 + 5 };
    struct sleep_core_ev hold_off_early = { .src = SRC_GPIO, .rtc_ok = true,
                                            .rtc = T0 + 30 };
    struct sleep_core_ev hold_off_edge = { .src = SRC_GPIO, .rtc_ok = true,
                                           .rtc = T0 + SLEEP_HOLD_WAKE_S - 1 };
    struct sleep_core_ev hold_off_late = { .src = SRC_GPIO, .rtc_ok = true,
                                           .rtc = T0 + SLEEP_HOLD_WAKE_S };
    struct sleep_core_ev hold_on_ev = { .src = SRC_GPIO, .hold_on = true,
                                        .rtc_ok = true, .rtc = T0 + 5 };
    struct sleep_core_ev vbus_in = { .src = SRC_GPIO, .vbus = true,
                                     .rtc_ok = true, .rtc = T0 + 5 };
    struct sleep_core_ev vbus_in_hold = { .src = SRC_GPIO, .vbus = true,
                                          .hold_on = true, .rtc_ok = true,
                                          .rtc = T0 + 5 };
    struct sleep_core_ev vbus_out = { .src = SRC_GPIO, .vbus = false,
                                      .rtc_ok = true, .rtc = T0 + 5 };
    struct sleep_core_ev timer_rtc_bad = { .src = SRC_TIMER, .batt_ok = true,
                                           .mv = 3900 };
    struct sleep_core_ev timer_batt_bad = { .src = SRC_TIMER,
                                            .rtc_ok = true, .rtc = T0 + 60 };

    hold_press.hold_on = true;
    vbus_press.vbus = true;
    both_low.vbus = true;

    const struct crow rows[] = {
        { "PLAY release after the hold (prev 0 at entry: no button)",
          ev_wheel(true, pkt(0, 0, 0), 1), false, false, WAKE_NONE, 0, 1, 0 },
        { "bare touch (bit 30 only)", ev_wheel(true, pkt(0, 0, 1), 1),
          false, false, WAKE_NONE, 0, 1, 0 },
        { "wheel turn", ev_wheel(true, pkt(0, 50, 1), 1), false, false,
          WAKE_NONE, 0, 1, 0 },
        { "press edge: SELECT", ev_wheel(true, pkt(WBTN_SELECT, 0, 0), 1),
          false, false, WAKE_BUTTON, WBTN_SELECT, 0, 0 },
        { "press edge: MENU", ev_wheel(true, pkt(WBTN_MENU, 0, 0), 1),
          false, false, WAKE_BUTTON, WBTN_MENU, 0, 0 },
        { "two buttons in one packet",
          ev_wheel(true, pkt(WBTN_LEFT | WBTN_PLAY, 0, 0), 1), false, false,
          WAKE_BUTTON, WBTN_LEFT | WBTN_PLAY, 0, 0 },
        { "source 40, WHEEL_STAT bit 26 clear (I2C completion)",
          ev_wheel(false, 0, 1), false, false, WAKE_NONE, 0, 1, 0 },
        { "0xFFFFFFFF", ev_wheel(true, 0xFFFFFFFFu, 1), false, false,
          WAKE_NONE, 0, 1, 0 },
        { "type 0x3A", ev_wheel(true, 0x8000003Au, 1), false, false,
          WAKE_NONE, 0, 1, 0 },
        { "out of step: resync, no wake", ev_wheel(true, 0x12345678u, 1),
          false, false, WAKE_NONE, 0, 0, 1 },
        { "press with Hold on", hold_press, true, false, WAKE_NONE, 0, 1, 0 },
        { "GPIO interrupt with no level change", gpio_none, false, false,
          WAKE_NONE, 0, 1, 0 },
        { "Hold on while asleep: no wake", hold_on_ev, false, false,
          WAKE_NONE, 0, 0, 0 },
        { "Hold off at 30 s (inside SLEEP_HOLD_WAKE_S)", hold_off_early,
          true, false, WAKE_HOLD, 0, 0, 0 },
        { "Hold off 1 s before SLEEP_HOLD_WAKE_S", hold_off_edge, true,
          false, WAKE_HOLD, 0, 0, 0 },
        { "Hold off at SLEEP_HOLD_WAKE_S: no wake", hold_off_late, true,
          false, WAKE_NONE, 0, 0, 0 },
        { "VBUS inserted", vbus_in, false, false, WAKE_VBUS, 0, 0, 0 },
        { "VBUS inserted with Hold on (Rev 4 N8)", vbus_in_hold, true, false,
          WAKE_VBUS, 0, 0, 0 },
        { "VBUS removed: no wake", vbus_out, false, true, WAKE_NONE, 0, 0,
          0 },
        { "timer, battery fine", ev_timer(60, 3900), false, false,
          WAKE_NONE, 0, 0, 0 },
        { "timer, battery under low_mv", ev_timer(60, 3299), false, false,
          WAKE_LOWBATT, 0, 0, 0 },
        { "timer, battery at low_mv (reaches, Rev 4a)", ev_timer(60, 3300),
          false, false, WAKE_LOWBATT, 0, 0, 0 },
        { "timer, battery 1 mV over low_mv", ev_timer(60, 3301), false,
          false, WAKE_NONE, 0, 0, 0 },
        { "timer, RTC read failed: failure rule", timer_rtc_bad, false,
          false, WAKE_DEEP, 0, 0, 0 },
        { "timer, battery read failed: failure rule", timer_batt_bad, false,
          false, WAKE_DEEP, 0, 0, 0 },
        { "timer at deep_after_s", ev_timer(86400, 3900), false, false,
          WAKE_DEEP, 0, 0, 0 },
        { "timer 1 s before deep_after_s", ev_timer(86399, 3900), false,
          false, WAKE_NONE, 0, 0, 0 },
        /* simultaneous sources */
        { "LOWBATT beats VBUS", both_low, false, false, WAKE_LOWBATT, 0, 0,
          0 },
        { "VBUS beats BUTTON", vbus_press, false, false, WAKE_VBUS, 0, 0,
          0 },
    };
    run_rows(rows, sizeof rows / sizeof rows[0], &base_rq);

    /* wake_on_hold (developer): Hold-off wakes after the window too */
    struct sleep_core_req rq = base_rq;
    rq.wake_on_hold = true;
    const struct crow dev[] = {
        { "wake_on_hold: Hold off late wakes", hold_off_late, true, false,
          WAKE_HOLD, 0, 0, 0 },
    };
    run_rows(dev, 1, &rq);
    /* hold_wake_s 0: Hold-off never wakes */
    rq = base_rq;
    rq.hold_wake_s = 0;
    const struct crow never[] = {
        { "hold_wake_s 0: Hold off at 30 s does not wake", hold_off_early,
          true, false, WAKE_NONE, 0, 0, 0 },
    };
    run_rows(never, 1, &rq);
}

/* Priority with every source at once: LOWBATT > VBUS > ALARM > BUTTON >
 * HOLD > DEEP > BCM_CUT, removing the winner each time */
static void test_priority(void)
{
    static const enum sleep_wake order[] = {
        WAKE_LOWBATT, WAKE_VBUS, WAKE_ALARM, WAKE_BUTTON, WAKE_HOLD,
        WAKE_DEEP, WAKE_BCM_CUT, WAKE_NONE,
    };
    struct sleep_core_req rq = base_rq;
    struct sleep_core_ev ev = {
        .src = SRC_TIMER | SRC_WHEEL | SRC_GPIO, .pkt_ready = true,
        .pkt = pkt(WBTN_PLAY, 0, 0), .hold_on = false, .vbus = true,
        .rtc_ok = true, .rtc = T0 + 100, .batt_ok = true, .mv = 3000,
    };

    rq.alarm_rtc = T0 + 100;
    rq.deep_after_s = 100;
    rq.bcm_cut_after_s = 100;
    for (unsigned i = 0; i < sizeof order / sizeof order[0]; i++)
    {
        struct sleep_core_state st;
        enum sleep_wake w;

        sleep_core_begin(&st, &rq, T0, true, false);
        w = sleep_classify(&st, &ev);
        CHECK(w == order[i], "priority step %u: why %d want %d", i, w,
              order[i]);
        switch (order[i])           /* take the winner away */
        {
        case WAKE_LOWBATT: ev.mv = 3900; break;
        case WAKE_VBUS:    ev.vbus = false; break;
        case WAKE_ALARM:   rq.alarm_rtc = 0; break;
        case WAKE_BUTTON:  ev.pkt = pkt(0, 0, 0); break;
        case WAKE_HOLD:    ev.hold_on = true; break;
        case WAKE_DEEP:    rq.deep_after_s = 0; break;
        case WAKE_BCM_CUT: rq.bcm_cut_after_s = 0; break;
        default: break;
        }
    }
}

/* Sequences across several wakes of one sleep */
/* P18 D2 (the lab's "wheel woke it"): the wheel alone, turned through
 * two revolutions with the finger down, lifted, touched again, never
 * wakes and never counts toward the failure cap; each packet is a valid
 * packet with no press edge (PKT_VALID, edge 0), so sleep_classify()
 * takes it as no wake. The quiet-packet mute may fire (once per poll);
 * a press after the next poll still wakes. */
static void test_wheel_only(void)
{
    struct sleep_core_state st;
    struct sleep_core_ev ev;
    enum sleep_wake w = WAKE_NONE;
    int i, woke = 0, mutes = 0;
    uint8_t prev = 0, edge;

    for (i = 0; i < 2 * 96; i++)        /* the packet class alone */
    {
        enum wheel_pkt k = sleep_wheel_packet(true, pkt(0, i % 96, 1),
                                              &prev, &edge);
        CHECK(k == PKT_VALID && edge == 0 && prev == 0,
              "turn pos %d: class %d edge %#x", i % 96, k, edge);
    }

    sleep_core_begin(&st, &base_rq, T0, false, false);
    for (i = 0; i < 2 * 96 + 8; i++)
    {
        /* a turn, then the finger lifted (no touch bit), then a tap */
        uint32_t p = i < 2 * 96 ? pkt(0, i % 96, 1)
                   : pkt(0, 40, (i & 1) == 0);
        ev = ev_wheel(true, p, 1 + i / 20);
        w = sleep_classify(&st, &ev);
        if (w != WAKE_NONE)
            woke++;
        if (st.act & SACT_WHEEL_MUTE)
            mutes++;
        if (st.act & SACT_WHEEL_MUTE)
            break;                      /* power_sleep() masks it here */
    }
    CHECK(woke == 0, "the wheel alone woke %d times", woke);
    CHECK(mutes <= 1, "at most one mute per poll (%d)", mutes);
    CHECK(st.win_bad == 0, "wheel packets never count toward the cap (%u)",
          (unsigned)st.win_bad);
    CHECK(st.wake_buttons == 0, "no button recorded (%#x)", st.wake_buttons);
    w = sleep_classify(&st, &(struct sleep_core_ev){
        .src = SRC_TIMER, .rtc_ok = true, .rtc = T0 + SLEEP_POLL_S,
        .batt_ok = true, .mv = 3900 });
    CHECK(w == WAKE_NONE, "the poll after the turn: no wake (%d)", w);
    ev = ev_wheel(true, pkt(WBTN_MENU, 0, 0), SLEEP_POLL_S + 1);
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_BUTTON && st.wake_buttons == WBTN_MENU,
          "a press after the turn wakes (%d %#x)", w, st.wake_buttons);
}

static void test_sequences(void)
{
    struct sleep_core_state st;
    struct sleep_core_ev ev;
    enum sleep_wake w;

    /* hold PLAY, release PLAY while asleep, then SELECT: only SELECT
     * wakes. buttons_at_entry is 0 (the shell awaited PLAY's release),
     * but a PLAY still down at entry must not wake either */
    struct sleep_core_req rq = base_rq;
    rq.buttons_at_entry = WBTN_PLAY;
    sleep_core_begin(&st, &rq, T0, false, false);
    ev = ev_wheel(true, pkt(WBTN_PLAY, 0, 0), 1);
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_NONE, "PLAY held from entry: why %d", w);
    ev = ev_wheel(true, pkt(0, 0, 0), 2);
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_NONE, "PLAY release: why %d", w);
    ev = ev_wheel(true, pkt(WBTN_SELECT, 0, 0), 300);
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_BUTTON && st.wake_buttons == WBTN_SELECT,
          "SELECT after the release: why %d buttons %#x", w,
          st.wake_buttons);

    /* Hold on at entry, opto off; Hold off inside the window: wakes
     * (Q12), with the opto re-initialised and one edge */
    sleep_core_begin(&st, &base_rq, T0, true, false);
    ev = (struct sleep_core_ev){ .src = SRC_GPIO, .rtc_ok = true,
                                 .rtc = T0 + 120 };
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_HOLD && st.hold_edges == 1
          && (st.act & SACT_OPTO_INIT),
          "Hold off at 2 min: why %d edges %u act %#x", w, st.hold_edges,
          st.act);

    /* Hold off after the window: no wake, opto re-initialised; the next
     * press wakes at once (by interrupt, no poll) */
    sleep_core_begin(&st, &base_rq, T0, true, false);
    ev = ev_timer(SLEEP_HOLD_WAKE_S + 60, 3900);
    ev.hold_on = true;
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_NONE, "poll past the window: why %d", w);
    ev = (struct sleep_core_ev){ .src = SRC_GPIO, .rtc_ok = true,
                                 .rtc = T0 + SLEEP_HOLD_WAKE_S + 90 };
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_NONE && (st.act & SACT_OPTO_INIT)
          && st.hold_edges == 1,
          "late Hold off: why %d act %#x edges %u", w, st.act,
          st.hold_edges);
    ev = ev_wheel(true, pkt(WBTN_SELECT, 0, 0), SLEEP_HOLD_WAKE_S + 91);
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_BUTTON && st.wake_buttons == WBTN_SELECT
          && st.hold_edges == 1 && st.timer_wakes == 1,
          "press after late Hold off: why %d buttons %#x edges %u polls %u",
          w, st.wake_buttons, st.hold_edges, st.timer_wakes);

    /* Hold-off edge seen without an RTC read: the last known slept time
     * decides (here the poll at 30 s) */
    sleep_core_begin(&st, &base_rq, T0, true, false);
    ev = ev_timer(30, 3900);
    ev.hold_on = true;
    sleep_classify(&st, &ev);
    ev = (struct sleep_core_ev){ .src = SRC_GPIO };
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_HOLD, "Hold off, no RTC, last slept 30 s: why %d", w);

    /* Hold on at entry (fallback design: opto powered), presses dropped;
     * Hold on edge while asleep turns the opto off */
    sleep_core_begin(&st, &base_rq, T0, false, false);
    ev = (struct sleep_core_ev){ .src = SRC_GPIO, .hold_on = true,
                                 .rtc_ok = true, .rtc = T0 + 10 };
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_NONE && (st.act & SACT_OPTO_OFF),
          "Hold on: why %d act %#x", w, st.act);
    for (int b = 0; b < 5; b++)
    {
        ev = ev_wheel(true, pkt(1u << b, 0, 0), 11 + b);
        ev.hold_on = true;
        w = sleep_classify(&st, &ev);
        CHECK(w == WAKE_NONE, "button %d under Hold: why %d", b, w);
        ev = ev_wheel(true, pkt(0, 0, 0), 11 + b);
        ev.hold_on = true;
        sleep_classify(&st, &ev);
    }
    CHECK(st.spurious == 10, "presses+releases under Hold: spurious %u",
          st.spurious);

    /* a button held under Hold, still held after Hold off: no edge */
    sleep_core_begin(&st, &base_rq, T0, true, false);
    rq = base_rq;
    rq.hold_wake_s = 0;
    st.rq = &rq;
    ev = ev_wheel(true, pkt(WBTN_MENU, 0, 0), 5);
    ev.hold_on = true;
    sleep_classify(&st, &ev);
    ev = ev_wheel(true, pkt(WBTN_MENU, 0, 0), 6);
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_NONE, "MENU held through Hold off: why %d", w);

    /* the spurious cap: SLEEP_SPURIOUS_MAX in an hour is fine, one more
     * is the failure rule */
    sleep_core_begin(&st, &base_rq, T0, false, false);
    ev = ev_wheel(false, 0, 10);
    for (int i = 0; i < SLEEP_SPURIOUS_MAX; i++)
    {
        w = sleep_classify(&st, &ev);
        if (w != WAKE_NONE)
            break;
    }
    CHECK(w == WAKE_NONE && st.spurious == SLEEP_SPURIOUS_MAX,
          "spurious at the cap: why %d n %u", w, st.spurious);
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_DEEP, "spurious over the cap: why %d", w);

    /* Rev 4a: valid packets without a press edge never reach the cap:
     * ~10 s of turning in a pocket (about 100 packets/s) and a touch
     * storm, and presses under Hold */
    sleep_core_begin(&st, &base_rq, T0, false, false);
    for (int i = 0; i < 1000; i++)
    {
        ev = ev_wheel(true, pkt(0, (unsigned)(i * 4) % 96, 1), 10 + i / 100);
        w = sleep_classify(&st, &ev);
        if (w != WAKE_NONE)
            break;
    }
    for (int i = 0; i < 2000 && w == WAKE_NONE; i++)
    {
        ev = ev_wheel(true, pkt(0, 7, 1), 20);
        w = sleep_classify(&st, &ev);
    }
    CHECK(w == WAKE_NONE && st.spurious == 3000 && st.win_bad == 0,
          "pocket turns and touches: why %d spurious %u bad %u", w,
          st.spurious, st.win_bad);
    sleep_core_begin(&st, &base_rq, T0, true, false);
    w = WAKE_NONE;
    for (int i = 0; i < 2 * SLEEP_SPURIOUS_MAX && w == WAKE_NONE; i++)
    {
        ev = ev_wheel(true, pkt(i & 1 ? WBTN_SELECT : 0, 0, 0), 10);
        ev.hold_on = true;
        w = sleep_classify(&st, &ev);
    }
    CHECK(w == WAKE_NONE, "presses under Hold never hit the cap: why %d", w);
    /* out-of-step packets and ignored types do count */
    sleep_core_begin(&st, &base_rq, T0, false, false);
    for (int i = 0; i <= SLEEP_SPURIOUS_MAX; i++)
    {
        ev = ev_wheel(true, i & 1 ? 0x8000003Au : 0x12345678u, 10);
        w = sleep_classify(&st, &ev);
        if (w != WAKE_NONE)
            break;
    }
    CHECK(w == WAKE_DEEP && st.win_bad == SLEEP_SPURIOUS_MAX + 1,
          "out of step + 0x3A storm: why %d bad %u", w, st.win_bad);
    /* GPIO interrupts that change nothing count */
    sleep_core_begin(&st, &base_rq, T0, false, false);
    ev = (struct sleep_core_ev){ .src = SRC_GPIO, .rtc_ok = true,
                                 .rtc = T0 + 10 };
    w = WAKE_NONE;
    for (int i = 0; i <= SLEEP_SPURIOUS_MAX && w == WAKE_NONE; i++)
        w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_DEEP && st.win_bad == SLEEP_SPURIOUS_MAX + 1,
          "GPIO storm: why %d bad %u", w, st.win_bad);
    w = WAKE_NONE;

    /* ... but the count restarts each hour of RTC time */
    sleep_core_begin(&st, &base_rq, T0, false, false);
    ev = ev_wheel(false, 0, 10);
    for (int i = 0; i < SLEEP_SPURIOUS_MAX; i++)
        sleep_classify(&st, &ev);
    ev = ev_timer(SLEEP_SPURIOUS_WIN_S + 10, 3900);
    sleep_classify(&st, &ev);
    ev = ev_wheel(false, 0, SLEEP_SPURIOUS_WIN_S + 11);
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_NONE && st.spurious == SLEEP_SPURIOUS_MAX + 1,
          "spurious in a new hour: why %d n %u", w, st.spurious);

    /* the alarm: polls at 60 s, alarm at 90 s */
    rq = base_rq;
    rq.alarm_rtc = T0 + 90;
    sleep_core_begin(&st, &rq, T0, false, false);
    CHECK(sleep_poll_deadline(&rq, 0, T0) == 60, "first poll %u",
          (unsigned)sleep_poll_deadline(&rq, 0, T0));
    ev = ev_timer(60, 3900);
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_NONE, "poll at 60 s: why %d", w);
    CHECK(sleep_poll_deadline(&rq, 60, T0 + 60) == 30,
          "second poll programmed for the alarm: %u",
          (unsigned)sleep_poll_deadline(&rq, 60, T0 + 60));
    ev = ev_timer(90, 3900);
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_ALARM && st.timer_wakes == 2 && st.slept_s == 90,
          "alarm at 90 s: why %d polls %u slept %u", w, st.timer_wakes,
          (unsigned)st.slept_s);
}

static void test_deadline(void)
{
    struct sleep_core_req rq = { 0 };
    struct sleep_core_req out;

    CHECK(sleep_poll_deadline(&rq, 0, T0) == SLEEP_POLL_S,
          "poll_s 0 = SLEEP_POLL_S");
    rq.poll_s = 60;
    rq.alarm_rtc = T0 + 3600;
    rq.deep_after_s = 86400;
    CHECK(sleep_poll_deadline(&rq, 0, T0) == 60, "poll term");
    CHECK(sleep_poll_deadline(&rq, 3570, T0 + 3570) == 30, "alarm term");
    CHECK(sleep_poll_deadline(&rq, 3600, T0 + 3600) == 0, "alarm due");
    CHECK(sleep_poll_deadline(&rq, 3700, T0 + 3700) == 0, "alarm passed");
    rq.alarm_rtc = 0;
    CHECK(sleep_poll_deadline(&rq, 86390, T0 + 86390) == 10, "deep term");
    CHECK(sleep_poll_deadline(&rq, 86500, T0 + 86500) == 0, "deep passed");
    rq.bcm_cut_after_s = 1800;
    CHECK(sleep_poll_deadline(&rq, 1780, T0 + 1780) == 20, "bcm_cut term");
    rq.bcm_cut_after_s = 0;
    CHECK(sleep_poll_deadline(&rq, 1780, T0 + 1780) == 60,
          "bcm_cut 0 (already cut): no term");

    /* re-entry after WAKE_BCM_CUT at 1800 s */
    rq = base_rq;
    rq.bcm_cut_after_s = 1800;
    rq.alarm_rtc = T0 + 7200;
    sleep_req_remaining(&rq, 1800, &out);
    CHECK(out.bcm_cut_after_s == 0 && out.deep_after_s == 86400 - 1800
          && out.alarm_rtc == T0 + 7200 && out.hold_wake_s == 0
          && out.low_mv == rq.low_mv,
          "remaining after the cut: deep %u hold %u",
          (unsigned)out.deep_after_s, (unsigned)out.hold_wake_s);
    rq.hold_wake_s = 3600;
    sleep_req_remaining(&rq, 1800, &out);
    CHECK(out.hold_wake_s == 1800, "hold window shortened: %u",
          (unsigned)out.hold_wake_s);
    rq.deep_after_s = 100;
    sleep_req_remaining(&rq, 1800, &out);
    CHECK(out.deep_after_s == 1, "an elapsed deep timer stays due");
    rq.deep_after_s = 0;
    sleep_req_remaining(&rq, 1800, &out);
    CHECK(out.deep_after_s == 0, "no deep timer stays none");
}

/* ---- shell policy ---- */
static void test_deep_after(void)
{
    static const struct {
        uint32_t mah, ua, stock;
        uint32_t want;
    } rows[] = {
        { 3000, 0, 0, 86400 },          /* 60 h at 2.5 mA: capped 24 h */
        { 600, 0, 0, 43200 },           /* 30 mAh / 2.5 mA = 12 h */
        { 400, 0, 0, 28800 },           /* 20 mAh / 2.5 mA = 8 h */
        { 3000, 5000, 0, 86400 },       /* 150 / 5 = 30 h: capped */
        { 3000, 10000, 0, 54000 },      /* measured 10 mA: 15 h */
        { 600, 1000, 0, 86400 },        /* measured 1 mA: 30 h, capped */
        { 400, 10000, 0, 7200 },        /* 2 h */
        { 400, 50000, 0, SLEEP_DEEP_MIN_S }, /* 0.4 h: the floor */
        { 3000, 0, 7200, 7200 },        /* a stock timeout (Q0) wins */
        { 3000, 0, 600, SLEEP_DEEP_MIN_S }, /* ... above the floor */
        { 0, 0, 0, SLEEP_DEEP_MIN_S },
    };

    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++)
    {
        uint32_t got = sleep_deep_after_s(rows[i].mah, rows[i].ua,
                                          rows[i].stock);
        CHECK(got == rows[i].want, "deep_after(%u mAh, %u uA, %u s) = %u "
              "want %u", (unsigned)rows[i].mah, (unsigned)rows[i].ua,
              (unsigned)rows[i].stock, (unsigned)got,
              (unsigned)rows[i].want);
    }
}

static void test_low_and_tier(void)
{
    uint16_t low = sleep_low_mv(3500);

    CHECK(low == 3600, "low_mv for disk-safe 3500: %u", low);
    CHECK(!sleep_battery_low(3900, 50, low), "fine");
    CHECK(sleep_battery_low(3900, 9, low), "9 %% is low");
    CHECK(!sleep_battery_low(3900, 10, low), "10 %% is not");
    CHECK(sleep_battery_low(3599, 50, low), "3599 mV is low");
    CHECK(sleep_battery_low(3600, 50, low), "3600 mV reaches it (Rev 4a)");
    CHECK(!sleep_battery_low(3601, 50, low), "3601 mV is fine");
    CHECK(!sleep_battery_low(3900, -1, low), "unknown level, mv fine");
    CHECK(sleep_battery_low(3500, -1, low), "unknown level, mv low");
    CHECK(!sleep_battery_low(0, 50, low), "no mv reading");

    static const struct {
        const char *name;
        struct sleep_tier_in in;
        enum sleep_pick want;
    } rows[] = {
        { "normal", { 3900, 60, false, false, true, false, false, 3600 },
          SLEEP_PICK_SOFT },
        { "unsupported (stock sim)",
          { 3900, 60, false, false, false, false, false, 3600 },
          SLEEP_PICK_OFF },
        { "developer Sleep: off",
          { 3900, 60, false, false, true, true, false, 3600 },
          SLEEP_PICK_OFF },
        { "low level", { 3900, 8, false, false, true, false, false, 3600 },
          SLEEP_PICK_OFF },
        { "low mv", { 3550, 40, false, false, true, false, false, 3600 },
          SLEEP_PICK_OFF },
        { "USB host: ignored",
          { 3900, 60, false, true, true, false, false, 3600 },
          SLEEP_PICK_IGNORE },
        { "USB host beats low battery",
          { 3500, 5, false, true, true, false, false, 3600 },
          SLEEP_PICK_IGNORE },
        { "charger: ignored",
          { 3900, 60, true, false, true, false, false, 3600 },
          SLEEP_PICK_IGNORE },
        { "charger with the developer override",
          { 3900, 60, true, false, true, false, true, 3600 },
          SLEEP_PICK_SOFT },
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++)
    {
        enum sleep_pick p = sleep_tier_pick(&rows[i].in);
        CHECK(p == rows[i].want, "tier %s: %d want %d", rows[i].name, p,
              rows[i].want);
    }
}

static void test_boot_equiv(void)
{
    CHECK(SLEEP_BOOT_EQUIV_S == 21600, "SLEEP_BOOT_EQUIV_S is 6 h");
    CHECK(sleep_counts_as_boot(WAKE_BUTTON, SLEEP_BOOT_EQUIV_S),
          "button after 6 h counts");
    CHECK(!sleep_counts_as_boot(WAKE_BUTTON, SLEEP_BOOT_EQUIV_S - 1),
          "button before 6 h does not");
    CHECK(sleep_counts_as_boot(WAKE_VBUS, 30000), "VBUS after 8 h counts");
    CHECK(sleep_counts_as_boot(WAKE_ALARM, 30000), "alarm counts");
    CHECK(sleep_counts_as_boot(WAKE_HOLD, 30000), "Hold counts");
    CHECK(!sleep_counts_as_boot(WAKE_DEEP, 86400),
          "deep: the next boot counts, not the sleep");
    CHECK(!sleep_counts_as_boot(WAKE_LOWBATT, 86400), "low battery");
    CHECK(!sleep_counts_as_boot(WAKE_BCM_CUT, 86400),
          "BCM cut: the sleep goes on");
    CHECK(!sleep_counts_as_boot(WAKE_NONE, 86400), "none");
}

static void test_walk_owed(void)
{
    const uint32_t last = 800000000u;

    CHECK(SLEEP_WALK_S == 7u * 86400u, "SLEEP_WALK_DAYS is 7");
    CHECK(!sleep_walk_owed(last + 6 * 86400 + 23 * 3600, last),
          "6 d 23 h: not owed");
    CHECK(!sleep_walk_owed(last + SLEEP_WALK_S - 1, last),
          "7 d - 1 s: not owed");
    CHECK(sleep_walk_owed(last + SLEEP_WALK_S, last), "7 d: owed");
    CHECK(sleep_walk_owed(last + 30 * 86400, last), "30 d: owed");
    CHECK(!sleep_walk_owed(last - 1, last),
          "RTC 1 s behind the last walk: not owed, no underflow");
    CHECK(!sleep_walk_owed(last - 400 * 86400u, last),
          "RTC set back 400 d: not owed");
    CHECK(!sleep_walk_owed(0, last), "RTC reads 0: not owed");
    CHECK(sleep_walk_owed(last, 0), "no walk recorded: owed");
}

static void test_job_names(void)
{
    static const char *const want[SJ_COUNT] = {
        "commit", "rebuild", "walk", "chapters", "topup", "rotation",
    };
    for (int j = 0; j < SJ_COUNT; j++)
        CHECK(!strcmp(sleep_job_name(j), want[j]), "job %d: %s", j,
              sleep_job_name(j));
    CHECK(!strcmp(sleep_job_name(SJ_COUNT), "?"), "out of range");
}

static void test_swallow(void)
{
    struct sleep_swallow sw;

    /* a wake by SELECT: press, repeat/long press and release dropped,
     * the next SELECT acts */
    sleep_swallow_arm(&sw, WBTN_SELECT, WBTN_SELECT, 0);
    CHECK(sleep_swallow_event(&sw, WBTN_SELECT, false), "SELECT press");
    CHECK(sleep_swallow_event(&sw, WBTN_SELECT, false), "SELECT long");
    CHECK(sleep_swallow_event(&sw, WBTN_SELECT, true), "SELECT release");
    CHECK(!sleep_swallow_event(&sw, WBTN_SELECT, false), "next SELECT");
    CHECK(!sleep_swallow_event(&sw, WBTN_SELECT, true), "its release");

    /* a second button pressed while the first is held: not swallowed */
    sleep_swallow_arm(&sw, WBTN_MENU, WBTN_MENU, 0);
    CHECK(!sleep_swallow_event(&sw, WBTN_PLAY, false), "PLAY while MENU");
    CHECK(!sleep_swallow_event(&sw, WBTN_PLAY, true), "PLAY release");
    CHECK(sleep_swallow_event(&sw, WBTN_MENU, true), "MENU release");
    CHECK(sw.mask == 0, "disarmed");

    /* two waking buttons: each until its own release */
    sleep_swallow_arm(&sw, WBTN_LEFT | WBTN_RIGHT, WBTN_LEFT | WBTN_RIGHT, 0);
    CHECK(sleep_swallow_event(&sw, WBTN_LEFT, true), "LEFT release");
    CHECK(sleep_swallow_event(&sw, WBTN_RIGHT, false), "RIGHT repeat");
    CHECK(sleep_swallow_event(&sw, WBTN_RIGHT, true), "RIGHT release");
    CHECK(!sleep_swallow_event(&sw, WBTN_LEFT, false), "LEFT again");

    /* released while still asleep: nothing to swallow */
    sleep_swallow_arm(&sw, WBTN_SELECT, 0, 0);
    CHECK(!sleep_swallow_event(&sw, WBTN_SELECT, false),
          "SELECT already up at wake");

    /* PLAY held into the sleep (buttons_at_entry, Rev 4 N6/4a), released
     * while asleep; a wake by SELECT: PLAY's late release is dropped,
     * the next PLAY acts */
    sleep_swallow_arm(&sw, WBTN_SELECT, WBTN_SELECT, WBTN_PLAY);
    CHECK(sw.mask == (WBTN_SELECT | WBTN_PLAY), "armed SELECT+PLAY");
    CHECK(sleep_swallow_event(&sw, WBTN_PLAY, true), "PLAY's late release");
    CHECK(sleep_swallow_event(&sw, WBTN_SELECT, true), "SELECT release");
    CHECK(!sleep_swallow_event(&sw, WBTN_PLAY, false), "next PLAY acts");
    /* PLAY still held at the wake: its repeat and release dropped */
    sleep_swallow_arm(&sw, WBTN_MENU, 0, WBTN_PLAY);
    CHECK(sleep_swallow_event(&sw, WBTN_PLAY, false), "PLAY repeat");
    CHECK(sleep_swallow_event(&sw, WBTN_PLAY, true), "PLAY release");
    CHECK(sw.mask == 0, "MENU already up, PLAY released: disarmed");
}

int main(void)
{
    test_wheel_packet();
    test_classify_single();
    test_priority();
    test_sequences();
    test_wheel_only();
    test_deadline();
    test_deep_after();
    test_low_and_tier();
    test_boot_equiv();
    test_walk_owed();
    test_job_names();
    test_swallow();
    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
