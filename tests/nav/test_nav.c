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
 * iPod UI shell: round-trip host test of the #38 navigation contracts
 * (apps/ipodui/nav_core.c and the transition API of screen_stack.c).
 * Built and run by tests/nav/run.sh.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nav_core.h"
#include "ipodui/screen_stack.h"
#include "../nowplaying/check.h"

static const int prov_a = 1, prov_b = 2;    /* stand-in provider keys */
static const int arg_x = 3;

/* -- rest scheduling ---------------------------------------------------- */

static void test_sched(void)
{
    struct nav_panel_sched s;
    long now;

    nav_sched_init(&s);
    CHECK_EQ(nav_sched_due(&s, 0), NAV_NEVER);

    /* push onto a split screen: nothing new until the rest delay */
    nav_sched_select(&s, &prov_a, NULL, NAV_REST_ICON, 100);
    CHECK_EQ(nav_sched_due(&s, 100), NAV_REST_ICON);
    CHECK_EQ(nav_sched_due(&s, 100 + NAV_REST_ICON - 1), 1);
    CHECK_EQ(nav_sched_due(&s, 100 + NAV_REST_ICON), 0);
    CHECK_EQ(nav_sched_due(&s, 100 + 10 * NAV_REST_ICON), 0);
    nav_sched_arrived(&s);
    CHECK_EQ(nav_sched_due(&s, 1000), NAV_NEVER);

    /* the wheel passes items every 25 ticks (~250 ms): never rests, the
     * old panel stays */
    for (now = 1000; now < 1500; now += 25)
    {
        nav_sched_select(&s, (now / 25) & 1 ? &prov_b : &prov_a, &arg_x,
                         NAV_REST_ICON, now);
        CHECK(nav_sched_due(&s, now + 24) > 0);
    }
    CHECK(s.shown.ops == &prov_a && s.shown.arg == NULL);

    /* back to the shown key: nothing pending */
    nav_sched_select(&s, &prov_a, NULL, NAV_REST_ICON, 2000);
    CHECK_EQ(nav_sched_due(&s, 5000), NAV_NEVER);
    /* same provider, another arg: a different picture */
    nav_sched_select(&s, &prov_a, &arg_x, NAV_REST_COVER, 2000);
    CHECK_EQ(nav_sched_due(&s, 2000), NAV_REST_COVER);
    /* then an item without a preview: the old panel stays, nothing due */
    nav_sched_select(&s, NULL, NULL, NAV_REST_ICON, 2010);
    CHECK_EQ(nav_sched_due(&s, 9000), NAV_NEVER);
    CHECK(s.shown.ops == &prov_a && s.shown.arg == NULL);

    /* a leaf changed its value: same key again after the rest */
    nav_sched_select(&s, &prov_b, NULL, NAV_REST_ICON, 3000);
    nav_sched_arrived(&s);
    nav_sched_invalidate(&s, 3100);
    CHECK_EQ(nav_sched_due(&s, 3100), NAV_REST_ICON);
    nav_sched_arrived(&s);

    /* full repaint: due at once, no fade */
    nav_sched_forget(&s);
    CHECK_EQ(nav_sched_due(&s, 4000), 0);
    CHECK(s.shown.ops == NULL);
    nav_sched_arrived(&s);
    CHECK(s.shown.ops == &prov_b);
}

/* -- lit-time phase ----------------------------------------------------- */

static void test_phase(void)
{
    struct nav_phase ph;

    nav_phase_init(&ph, 50, true);
    CHECK_EQ(nav_phase_update(&ph, 150, true), 100);
    /* backlight off at 200: the 50 lit ticks count, the dark ones don't */
    CHECK_EQ(nav_phase_update(&ph, 200, false), 150);
    CHECK_EQ(nav_phase_update(&ph, 900, false), 150);
    /* on again at 1000 (dark until then) */
    CHECK_EQ(nav_phase_update(&ph, 1000, true), 150);
    CHECK_EQ(nav_phase_update(&ph, 1070, true), 220);
    /* a push or the WPS does not reset it: there is no reset */
    nav_phase_set(&ph, 4 * NAV_DRIFT_CYCLE + 7, 1100);
    CHECK_EQ(nav_phase_update(&ph, 1100, true), 4 * NAV_DRIFT_CYCLE + 7);
    CHECK_EQ(nav_phase_update(&ph, 1110, true), 4 * NAV_DRIFT_CYCLE + 17);
    nav_phase_set(&ph, -5, 1200);
    CHECK_EQ(ph.ticks, 0);
}

/* -- drift framing ------------------------------------------------------ */

static void test_drift(void)
{
    struct nav_drift_pos p, q;
    long ph;
    int steps = 0, pans = 0, halves = 0, last_pan = -1, last_fade = -1;
    int last_sub = -1;

    nav_drift_pos(0, &p);
    CHECK_EQ(p.cycle, 0);
    CHECK_EQ(p.fade_step, 1);
    CHECK_EQ(p.pan, 0);
    nav_drift_pos(NAV_DRIFT_FADE - 1, &p);
    CHECK_EQ(p.fade_step, NAV_DRIFT_FADE_STEPS);
    nav_drift_pos(NAV_DRIFT_FADE, &p);
    CHECK_EQ(p.fade_step, 0);
    CHECK_EQ(p.pan, 0);
    CHECK_EQ(p.half, 0);
    nav_drift_pos(NAV_DRIFT_FADE + NAV_DRIFT_HALF - 1, &p);
    CHECK_EQ(p.pan, 0);
    CHECK_EQ(p.half, 0);
    nav_drift_pos(NAV_DRIFT_FADE + NAV_DRIFT_HALF, &p);
    CHECK_EQ(p.pan, 0);
    CHECK_EQ(p.half, 1);
    nav_drift_pos(NAV_DRIFT_FADE + NAV_DRIFT_STEP, &p);
    CHECK_EQ(p.pan, 1);
    CHECK_EQ(p.half, 0);
    nav_drift_pos(NAV_DRIFT_CYCLE - 1, &p);
    CHECK_EQ(p.pan, NAV_DRIFT_PAN_MAX - 1);
    nav_drift_pos(NAV_DRIFT_CYCLE, &q);
    CHECK_EQ(q.cycle, 1);
    CHECK_EQ(q.fade_step, 1);

    /* 5-8 px/s (spec): 1 px per step */
    CHECK(NAV_HZ >= 5 * NAV_DRIFT_STEP && NAV_HZ <= 8 * NAV_DRIFT_STEP);

    /* cycle 0 pans left, cycle 1 right+down, cycle 2 up (as seen) */
    nav_drift_pos(NAV_DRIFT_FADE, &p);
    nav_drift_pos(NAV_DRIFT_CYCLE - 1, &q);
    CHECK(q.x < p.x && q.y == p.y);
    nav_drift_pos(NAV_DRIFT_CYCLE + NAV_DRIFT_FADE, &p);
    nav_drift_pos(2 * NAV_DRIFT_CYCLE - 1, &q);
    CHECK(q.x > p.x && q.y > p.y);
    nav_drift_pos(2 * NAV_DRIFT_CYCLE + NAV_DRIFT_FADE, &p);
    nav_drift_pos(3 * NAV_DRIFT_CYCLE - 1, &q);
    CHECK(q.x == p.x && q.y < p.y);

    /* the window stays inside the 300x300 source, every tick of 16 cycles;
     * wake() always lands on the next visible change */
    for (ph = 0; ph < 16L * NAV_DRIFT_CYCLE; ph++)
    {
        int w = nav_drift_wake(ph);
        nav_drift_pos(ph, &p);
        CHECK(p.x >= 0 && p.x + NAV_PANEL_W <= NAV_DRIFT_SRC);
        CHECK(p.y >= 0 && p.y + NAV_PANEL_H <= NAV_DRIFT_SRC);
        CHECK(w > 0 && w <= NAV_DRIFT_STEP);
        if (ph < NAV_DRIFT_CYCLE)
        {
            nav_drift_pos(ph + w, &q);
            CHECK(q.cycle != p.cycle || q.fade_step != p.fade_step
                  || NAV_DRIFT_SUB(&q) != NAV_DRIFT_SUB(&p));
            if (w > 1)
            {
                struct nav_drift_pos r;
                nav_drift_pos(ph + w - 1, &r);
                CHECK(r.fade_step == p.fade_step
                      && NAV_DRIFT_SUB(&r) == NAV_DRIFT_SUB(&p));
            }
            if (p.fade_step && p.fade_step != last_fade)
            {
                steps++;
                last_fade = p.fade_step;
            }
            if (p.pan != last_pan)
            {
                pans++;
                last_pan = p.pan;
            }
            if (!p.fade_step && NAV_DRIFT_SUB(&p) != last_sub)
            {
                /* half steps, one at a time: never a skipped one */
                CHECK(last_sub < 0 || NAV_DRIFT_SUB(&p) == last_sub + 1);
                halves += p.half;
                last_sub = NAV_DRIFT_SUB(&p);
            }
        }
    }
    CHECK_EQ(steps, NAV_DRIFT_FADE_STEPS);
    CHECK_EQ(pans, NAV_DRIFT_PAN_MAX);
    CHECK_EQ(halves, NAV_DRIFT_PAN_MAX);
}

/* -- drift strip plan (Rev 5 R1) ---------------------------------------- */

static void test_plan(void)
{
    struct nav_drift_in in;

    memset(&in, 0, sizeof(in));
    in.cycle = in.shown_cycle = 3;
    in.at_rest = true;
    in.storage_ok = true;
    in.panel_count = 10;
    in.t = NAV_DRIFT_PREFETCH_FROM;

    /* stopped: never prefetches, the cycle end reads directly */
    CHECK_EQ(nav_drift_plan(&in), NAV_DRIFT_IDLE);
    in.pan = 5;
    in.shown_pan = 4;
    CHECK_EQ(nav_drift_plan(&in), NAV_DRIFT_PAN);
    in.shown_pan = 5;

    /* playing: prefetch once storage is awake, past the settle time */
    in.playing = true;
    CHECK_EQ(nav_drift_plan(&in), NAV_DRIFT_PREFETCH);
    in.t = NAV_DRIFT_PREFETCH_FROM - 1;
    CHECK_EQ(nav_drift_plan(&in), NAV_DRIFT_IDLE);
    in.t = NAV_DRIFT_PREFETCH_FROM;
    in.storage_ok = false;
    CHECK_EQ(nav_drift_plan(&in), NAV_DRIFT_IDLE);   /* never wakes it */
    in.storage_ok = true;
    in.next_kept = true;
    CHECK_EQ(nav_drift_plan(&in), NAV_DRIFT_IDLE);   /* already there */

    /* not at rest (glide, slide, highlight fade): drop the frame */
    in.at_rest = false;
    in.cycle = 4;
    CHECK_EQ(nav_drift_plan(&in), NAV_DRIFT_IDLE);
    in.at_rest = true;

    /* cycle end */
    CHECK_EQ(nav_drift_plan(&in), NAV_DRIFT_FADE_STRIP);
    in.next_kept = false;               /* a glide took the strip */
    CHECK_EQ(nav_drift_plan(&in), NAV_DRIFT_FADE_DIRECT);
    in.storage_ok = false;              /* and storage sleeps */
    CHECK_EQ(nav_drift_plan(&in), NAV_DRIFT_REDWELL);
    in.storage_ok = true;
    in.panel_count = 1;                 /* one cover: always re-dwell */
    CHECK_EQ(nav_drift_plan(&in), NAV_DRIFT_REDWELL);
    in.panel_count = 0;
    CHECK_EQ(nav_drift_plan(&in), NAV_DRIFT_REDWELL);
    /* late (missed the fade window): still a fade, never a cut */
    in.panel_count = 10;
    in.cycle = 9;
    in.t = 300;
    CHECK_EQ(nav_drift_plan(&in), NAV_DRIFT_FADE_DIRECT);
}

/* -- in-place cross-fade weights ---------------------------------------- */

static void test_xfade(void)
{
    int n, a, b, k;

    for (n = 1; n <= NAV_XFADE_STEPS_MAX; n++)
        for (a = 0; a < 64; a++)
            for (b = 0; b < 64; b++)
            {
                int v = a, prev = a;
                for (k = 1; k <= n; k++)
                {
                    v = nav_xfade_chan(v, b, nav_xfade_weight(k, n));
                    /* monotonic toward the target, close to linear */
                    CHECK(b >= a ? (v >= prev && v <= b)
                                 : (v <= prev && v >= b));
                    /* within 1.25 levels of a + (b - a) * k / n */
                    CHECK(4 * abs(n * v - (n * a + (b - a) * k)) <= 5 * n);
                    prev = v;
                }
                CHECK_EQ(v, b);         /* the last step is exact */
            }
    CHECK_EQ(nav_xfade_weight(NAV_DRIFT_FADE_STEPS, NAV_DRIFT_FADE_STEPS),
             256);
    CHECK(NAV_DRIFT_FADE_STEPS <= NAV_XFADE_STEPS_MAX);
    CHECK(NAV_PANEL_XFADE_STEPS <= NAV_XFADE_STEPS_MAX);
}

/* -- transitions -------------------------------------------------------- */

static int entered;
static const void *entered_arg;
static int t_enter(struct ui_screen *s, const void *arg)
{
    (void)s;
    entered++;
    entered_arg = arg;
    return 0;
}
static const struct ui_screen_ops ops_root = { .name = "root" };
static const struct ui_screen_ops ops_a = { .name = "a", .enter = t_enter };

static void test_transitions(void)
{
    /* ui_push() == ui_push_t(UI_T_PUSH) */
    CHECK_EQ(ui_push(&ops_root, NULL), 0);
    CHECK_EQ(ui_take_transition(), 0);          /* the root just appears */
    CHECK_EQ(ui_push(&ops_a, &arg_x), 0);
    CHECK_EQ(entered, 1);
    CHECK(entered_arg == &arg_x);
    CHECK_EQ(ui_take_transition(), 1);
    CHECK_EQ(ui_transition_kind(), UI_T_PUSH);
    CHECK_EQ(ui_top()->transition, UI_T_PUSH);
    CHECK_EQ(ui_push_t(&ops_a, NULL, UI_T_PUSH), 0);
    CHECK_EQ(ui_take_transition(), 1);
    CHECK_EQ(ui_transition_kind(), UI_T_PUSH);

    /* a reveal pushes with its kind; its pop mirrors it */
    CHECK_EQ(ui_push_t(&ops_a, NULL, UI_T_REVEAL), 0);
    CHECK_EQ(ui_take_transition(), 1);
    CHECK_EQ(ui_transition_kind(), UI_T_REVEAL);
    CHECK_EQ(ui_push_t(&ops_a, NULL, UI_T_SPLIT), 0);
    CHECK_EQ(ui_transition_kind(), UI_T_SPLIT);
    ui_pop();
    CHECK_EQ(ui_take_transition(), -1);
    CHECK_EQ(ui_transition_kind(), UI_T_SPLIT);
    ui_pop();
    CHECK_EQ(ui_take_transition(), -1);
    CHECK_EQ(ui_transition_kind(), UI_T_REVEAL);
    ui_pop();
    CHECK_EQ(ui_transition_kind(), UI_T_PUSH);
    CHECK_EQ(ui_take_transition(), -1);
    CHECK_EQ(ui_take_transition(), 0);

    /* durations (spec index.md "Transitions"), ticks at 100 Hz */
    CHECK_EQ(nav_transition_ticks(UI_T_PUSH, 1, true, true), 19);
    CHECK_EQ(nav_transition_ticks(UI_T_PUSH, -1, true, true), 19);
    CHECK_EQ(nav_transition_ticks(UI_T_PUSH, 1, false, false), 19);
    CHECK_EQ(nav_transition_ticks(UI_T_PUSH, 1, true, false), 25);
    CHECK_EQ(nav_transition_ticks(UI_T_PUSH, -1, false, true), 29);
    CHECK_EQ(nav_transition_ticks(UI_T_REVEAL, 1, true, false), 25);
    CHECK_EQ(nav_transition_ticks(UI_T_REVEAL, -1, false, true), 25);
    CHECK_EQ(nav_transition_ticks(UI_T_SPLIT, 1, true, false), 21);
    CHECK_EQ(nav_transition_ticks(UI_T_SPLIT, -1, false, true), 29);
}

/* P12 W1-C: the Clocks reveal against the measured frames (VIDEO-2 A3:
 * the band's left edge at 245/183/125/81/22/0 px, frames at 40, 80,
 * 130, 170, 210 and 250 ms of 250 ms) */
static void test_reveal(void)
{
    static const int meas[] = { 245, 183, 125, 81, 22, 0 };
    static const int at[] = { 40, 80, 130, 170, 210, 250 };
    int k, out, in, out2, in2;

    nav_reveal_offsets(0, 25, 1, &out, &in);
    CHECK_EQ(out, 0);
    CHECK_EQ(in, 320);
    nav_reveal_offsets(25, 25, 1, &out, &in);
    CHECK_EQ(out, 320);
    CHECK_EQ(in, 0);
    nav_reveal_offsets(-3, 25, 1, &out, &in);
    CHECK_EQ(in, 320);
    nav_reveal_offsets(99, 25, 1, &out, &in);
    CHECK_EQ(in, 0);
    for (k = 0; k < 6; k++)
    {
        int ms = at[k];
        nav_reveal_offsets(ms, 250, 1, &out, &in);
        CHECK(in - meas[k] <= 12 && meas[k] - in <= 12);
        /* the old layer is gone by f037 (210 ms): the parallax */
        CHECK(ms < 210 || out == 320);
        /* the pop is the mirror */
        nav_reveal_offsets(250 - ms, 250, -1, &out2, &in2);
        CHECK_EQ(out2, out);
        CHECK_EQ(in2, in);
    }
    /* monotonic */
    for (k = 1, in2 = 320, out2 = 0; k <= 25; k++)
    {
        nav_reveal_offsets(k, 25, 1, &out, &in);
        CHECK(in <= in2 && out >= out2);
        in2 = in;
        out2 = out;
    }
}

int main(void)
{
    test_sched();
    test_phase();
    test_drift();
    test_plan();
    test_xfade();
    test_transitions();
    test_reveal();
    CHECK_DONE("test_nav");
}
