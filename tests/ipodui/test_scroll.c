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
 * iPod UI shell: host tests for the scroll.h contract (docs/phase3-brief.md)
 *
 * Built and run by tests/ipodui/run.sh against apps/ipodui/scroll.c, or
 * against tests/ipodui/ref_scroll.c while that does not exist yet.
 * SCROLL_H names the header to include (set by run.sh).
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
#include <stdbool.h>
#include SCROLL_H

/* Tunables. The 7G view only ever chases the selection, which moves at most
 * ~100 rows/s (25 px rows), so realistic distances are a few screens. */
#define CONV_STEPS      200     /* max steps to converge, max <= CONV_MAX_PX */
#define CONV_MAX_PX     1500    /* 60 rows of 25 px */
#define LARGE_MAX_PX    100000  /* sanity: huge list must still converge */
#define LARGE_STEPS     5000
#define WHEEL_MAX_VEL   3000u   /* clicks/s swept in the monotonic test */
#define WHEEL_MAX_CLICK 20

static int failures, checks;

#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { \
            if (failures++ < 30) { \
                printf("FAIL %s:%d: ", __func__, __LINE__); \
                printf(__VA_ARGS__); \
                printf("\n"); \
            } \
        } \
    } while (0)

static unsigned rng = 0x2545F491u;
static int rnd(int n) /* 0..n-1, deterministic xorshift */
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return n > 0 ? (int)(rng % (unsigned)n) : 0;
}

static int sign(int v) { return (v > 0) - (v < 0); }

/* One checked step: bounds, zero overshoot, honest return value.
 * Returns the step's result. */
static bool checked_step(struct ui_scroll *s, int dt, const char *what)
{
    int before = s->pos, tgt = s->target, side = sign(tgt - before);
    bool moving = ui_scroll_step(s, dt);

    CHECK(s->pos >= 0 && s->pos <= s->max,
          "%s: pos %d outside [0,%d]", what, s->pos, s->max);
    if (side > 0)
        CHECK(s->pos <= tgt, "%s: overshoot %d past target %d (down)",
              what, s->pos, tgt);
    else if (side < 0)
        CHECK(s->pos >= tgt, "%s: overshoot %d past target %d (up)",
              what, s->pos, tgt);
    CHECK(s->target == tgt, "%s: step changed target %d -> %d",
          what, tgt, s->target);
    CHECK(moving || s->pos == s->target, "%s: step returned false at "
          "pos %d, target %d", what, s->pos, s->target);
    return moving;
}

/* Steps until at rest; returns step count or -1 if over 'limit'. */
static int settle(struct ui_scroll *s, int dt, int limit, const char *what)
{
    int n = 0;
    while (checked_step(s, dt, what)) {
        if (++n > limit)
            return -1;
    }
    return n;
}

/* Move a fresh scroller to 'start' at rest through the API. */
static void at_rest(struct ui_scroll *s, int max, int start)
{
    ui_scroll_init(s, max);
    ui_scroll_set_target(s, start);
    settle(s, 4, LARGE_STEPS, "at_rest");
}

static void test_init_and_target_clamp(void)
{
    struct ui_scroll s;

    ui_scroll_init(&s, 500);
    CHECK(s.pos == 0 && s.target == 0 && s.max == 500,
          "init: pos %d target %d max %d", s.pos, s.target, s.max);
    CHECK(!ui_scroll_step(&s, 1), "init: at rest but step says moving");
    CHECK(s.pos == 0, "init: rest step moved to %d", s.pos);

    ui_scroll_set_target(&s, -40);
    CHECK(s.target == 0, "set_target(-40) -> %d, want 0", s.target);
    ui_scroll_set_target(&s, 540);
    CHECK(s.target == 500, "set_target(540) -> %d, want 500", s.target);
    ui_scroll_set_target(&s, 123);
    CHECK(s.target == 123, "set_target(123) -> %d", s.target);

    ui_scroll_init(&s, 0);
    ui_scroll_set_target(&s, 10);
    CHECK(s.target == 0 && s.max == 0, "max 0: target %d", s.target);
    CHECK(!ui_scroll_step(&s, 4), "max 0: step says moving");
}

static void conv_case(int max, int start, int target, int dt, int *worst)
{
    struct ui_scroll s;
    char what[64];
    int last, n = 0;

    snprintf(what, sizeof what, "conv max=%d %d->%d dt=%d",
             max, start, target, dt);
    at_rest(&s, max, start);
    CHECK(s.pos == start, "%s: could not reach start (at %d)", what, s.pos);
    ui_scroll_set_target(&s, target);
    last = s.pos;
    while (checked_step(&s, dt, what)) {
        /* from rest, every step moves towards the target (no wobble) */
        CHECK(abs(s.target - s.pos) <= abs(s.target - last),
              "%s: moved away %d -> %d", what, last, s.pos);
        last = s.pos;
        if (++n > CONV_STEPS)
            break;
    }
    CHECK(n <= CONV_STEPS, "%s: not converged in %d steps (pos %d)",
          what, CONV_STEPS, s.pos);
    CHECK(s.pos == target, "%s: rest at %d", what, s.pos);
    CHECK(!ui_scroll_step(&s, dt) && s.pos == target,
          "%s: moved again after rest (pos %d)", what, s.pos);
    if (n > *worst)
        *worst = n;
}

static void test_convergence(void)
{
    static const int maxes[] = { 0, 1, 24, 25, 100, 375, 750, CONV_MAX_PX };
    int worst[5] = { 0 };

    for (unsigned i = 0; i < sizeof maxes / sizeof maxes[0]; i++) {
        int m = maxes[i];
        for (int dt = 1; dt <= 4; dt++) {
            /* extremes and neighbours */
            int pts[] = { 0, 1, m / 2, m - 1, m };
            for (int a = 0; a < 5; a++)
                for (int b = 0; b < 5; b++)
                    if (pts[a] >= 0 && pts[b] >= 0 &&
                        pts[a] <= m && pts[b] <= m)
                        conv_case(m, pts[a], pts[b], dt, &worst[dt]);
            /* random pairs */
            for (int k = 0; k < 40; k++)
                conv_case(m, rnd(m + 1), rnd(m + 1), dt, &worst[dt]);
        }
    }
    printf("   convergence: worst steps dt=1..4: %d %d %d %d (limit %d)\n",
           worst[1], worst[2], worst[3], worst[4], CONV_STEPS);
}

static void test_large(void)
{
    struct ui_scroll s;
    int n;

    ui_scroll_init(&s, LARGE_MAX_PX);
    ui_scroll_set_target(&s, LARGE_MAX_PX);
    n = settle(&s, 1, LARGE_STEPS, "large down");
    CHECK(n >= 0 && s.pos == LARGE_MAX_PX, "large down: %d steps, pos %d",
          n, s.pos);
    ui_scroll_set_target(&s, 0);
    n = settle(&s, 1, LARGE_STEPS, "large up");
    CHECK(n >= 0 && s.pos == 0, "large up: %d steps, pos %d", n, s.pos);
}

static void test_set_max(void)
{
    struct ui_scroll s;

    /* at rest */
    at_rest(&s, 1000, 800);
    ui_scroll_set_max(&s, 300);
    CHECK(s.max == 300, "set_max: max %d", s.max);
    CHECK(s.pos <= 300 && s.pos >= 0, "set_max: pos %d not clamped", s.pos);
    CHECK(s.target <= 300 && s.target >= 0, "set_max: target %d", s.target);
    settle(&s, 2, CONV_STEPS, "set_max settle");
    CHECK(s.pos == s.target, "set_max: rest at %d, target %d",
          s.pos, s.target);

    /* mid-flight */
    at_rest(&s, 1000, 0);
    ui_scroll_set_target(&s, 1000);
    for (int i = 0; i < 3; i++)
        checked_step(&s, 1, "set_max flight");
    ui_scroll_set_max(&s, 50);
    CHECK(s.pos >= 0 && s.pos <= 50, "set_max flight: pos %d", s.pos);
    CHECK(s.target >= 0 && s.target <= 50, "set_max flight: target %d",
          s.target);
    CHECK(settle(&s, 1, CONV_STEPS, "set_max flight settle") >= 0,
          "set_max flight: no convergence");

    /* to zero, and growing again keeps position */
    ui_scroll_set_max(&s, 0);
    CHECK(s.pos == 0 && s.target == 0 && s.max == 0,
          "set_max(0): pos %d target %d", s.pos, s.target);
    at_rest(&s, 400, 200);
    ui_scroll_set_max(&s, 800);
    CHECK(s.pos == 200 && s.target == 200, "set_max grow moved pos %d "
          "target %d", s.pos, s.target);
}

static void test_midflight(void)
{
    for (int k = 0; k < 400; k++) {
        struct ui_scroll s;
        char what[80];
        int m = 25 + rnd(CONV_MAX_PX);
        int a = rnd(m + 1), b = rnd(m + 1), c = rnd(m + 1);
        int dt = 1 + rnd(4), pre = 1 + rnd(10), n = 0, rev = 0, dir = 0;

        snprintf(what, sizeof what, "midflight max=%d %d->%d->%d dt=%d",
                 m, a, b, c, dt);
        at_rest(&s, m, a);
        ui_scroll_set_target(&s, b);
        for (int i = 0; i < pre; i++)
            checked_step(&s, dt, what);
        ui_scroll_set_target(&s, c);
        for (;;) {
            int before = s.pos;
            bool moving = checked_step(&s, dt, what);
            int d = sign(s.pos - before);
            if (d && dir && d != dir)
                rev++;
            if (d)
                dir = d;
            if (!moving || ++n > CONV_STEPS)
                break;
        }
        CHECK(n <= CONV_STEPS && s.pos == c,
              "%s: not converged (pos %d after %d steps)", what, s.pos, n);
        CHECK(rev <= 1, "%s: %d direction reversals (oscillation)",
              what, rev);
    }
}

static void test_wheel(void)
{
    for (int c = 1; c <= WHEEL_MAX_CLICK; c++) {
        int prev = ui_wheel_steps(c, 0);
        CHECK(prev >= 1, "wheel(%d, 0) = %d < 1", c, prev);
        for (unsigned v = 1; v <= WHEEL_MAX_VEL; v++) {
            int st = ui_wheel_steps(c, v);
            if (st < 1 || st < prev) {
                CHECK(st >= 1, "wheel(%d, %u) = %d < 1", c, v, st);
                CHECK(st >= prev, "wheel(%d, %u) = %d < wheel(%d, %u) = %d",
                      c, v, st, c, v - 1, prev);
                break;
            }
            prev = st;
        }
    }
    CHECK(ui_wheel_steps(1, 0) == 1, "slow turn: wheel(1, 0) = %d, want 1",
          ui_wheel_steps(1, 0));
    printf("   wheel(1, v) for v = 0 5 10 20 40 80 160 320 clicks/s:");
    for (unsigned v = 0; v <= 320; v = v ? v * 2 : 5)
        printf(" %d", ui_wheel_steps(1, v));
    printf("\n");
}

/* ui_scroll_limit_lag: pulls pos to within 'lag' of the target from
 * either side, leaves it alone inside the window, clamps lag < 0 to 0. */
static void test_limit_lag(void)
{
    struct ui_scroll s;

    ui_scroll_init(&s, 1000);
    ui_scroll_set_target(&s, 800);
    s.pos = 100;
    ui_scroll_limit_lag(&s, 175);
    CHECK(s.pos == 625, "lag below: pos %d, want 625", s.pos);
    CHECK(s.target == 800, "lag moved the target to %d", s.target);

    ui_scroll_set_target(&s, 0);
    s.pos = 900;
    ui_scroll_limit_lag(&s, 175);
    CHECK(s.pos == 175, "lag above: pos %d, want 175", s.pos);

    ui_scroll_set_target(&s, 500);
    s.pos = 450;
    ui_scroll_limit_lag(&s, 175);
    CHECK(s.pos == 450, "inside the lag: pos %d moved, want 450", s.pos);

    s.pos = 400;
    ui_scroll_limit_lag(&s, -5);
    CHECK(s.pos == 500, "negative lag: pos %d, want the target 500", s.pos);

    /* stepping after a limit still converges without overshoot */
    ui_scroll_set_target(&s, 1000);
    s.pos = 0;
    ui_scroll_limit_lag(&s, 175);
    CHECK(s.pos == 825, "lag at max: pos %d, want 825", s.pos);
    CHECK(settle(&s, 1, CONV_STEPS, "after limit_lag") >= 0,
          "no convergence after limit_lag");
    CHECK(s.pos == 1000, "after limit_lag: rest at %d, want 1000", s.pos);
}

/* ui_scroll_snap: lands on the target at once, zero velocity, and the
 * next step reports rest. */
static void test_snap(void)
{
    struct ui_scroll s;

    ui_scroll_init(&s, 700);
    ui_scroll_set_target(&s, 600);
    checked_step(&s, 1, "pre-snap");
    CHECK(s.vel != 0, "pre-snap: no velocity");
    ui_scroll_snap(&s);
    CHECK(s.pos == 600 && s.vel == 0, "snap: pos %d vel %d, want 600 0",
          s.pos, s.vel);
    CHECK(!ui_scroll_step(&s, 1), "step after snap still moving");
    CHECK(s.pos == 600, "step after snap moved pos to %d", s.pos);

    ui_scroll_set_target(&s, 5000);     /* clamped to max */
    ui_scroll_snap(&s);
    CHECK(s.pos == 700, "snap past max: pos %d, want 700", s.pos);
}

int main(void)
{
    test_init_and_target_clamp();
    test_convergence();
    test_large();
    test_set_max();
    test_midflight();
    test_wheel();
    test_limit_lag();
    test_snap();
    printf("   %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
