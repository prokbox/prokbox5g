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
 * iPod UI shell: host tests for the dirty-band coalescing of
 * apps/ipodui/screen_stack.c (ui_invalidate_band / ui_dirty_bands).
 *
 * Built and run by tests/ipodui/run.sh with the stub config.h, system.h
 * and logf.h in tests/ipodui/stub/.
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
#include "ipodui/screen_stack.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            printf("FAIL %s:%d: ", __func__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static const struct ui_screen_ops dummy = { .name = "dummy" };

/* the leave hook sees each leaving screen while it is still live */
static struct ui_screen *gone[4];
static int ngone;
static bool gone_live_ok = true;

static void on_leave(struct ui_screen *s)
{
    if (!ui_screen_live(s))
        gone_live_ok = false;
    if (ngone < 4)
        gone[ngone++] = s;
}

/* #38 fix round: preview.c keyed its panel on a popped leaf screen and
 * called into freed data. Every leave must reach the hook, and the
 * screen must not count as live afterwards. */
static void test_leave_hook(void)
{
    struct ui_screen *a, *b;

    ui_set_leave_hook(on_leave);
    CHECK(ui_push(&dummy, NULL) == 0, "push a");
    a = ui_top();
    CHECK(ui_push(&dummy, NULL) == 0, "push b");
    b = ui_top();
    CHECK(ui_screen_live(a) && ui_screen_live(b), "pushed not live");
    ui_pop();
    CHECK(ngone == 1 && gone[0] == b, "pop: hook saw %d", ngone);
    CHECK(!ui_screen_live(b), "popped screen still live");
    CHECK(ui_screen_live(a), "parent not live");
    CHECK(ui_push(&dummy, NULL) == 0 && ui_push(&dummy, NULL) == 0,
          "push two");
    ui_pop_to_root();
    CHECK(ngone == 4, "pop to root: hook saw %d, want 4", ngone);
    CHECK(gone_live_ok, "hook ran after the screen left");
    CHECK(ui_depth() == 1 && ui_screen_live(ui_top()), "root left");
    CHECK(!ui_screen_live(NULL), "NULL live");
    ui_set_leave_hook(NULL);
    ui_clear_dirty();
}

/* Checks the pending bands against 'n' expected [y, y+h) pairs. */
static void expect(const char *what, int n, const int *want)
{
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX];
    int got = ui_dirty_bands(y, h), i;

    CHECK(got == n, "%s: %d bands, want %d", what, got, n);
    for (i = 0; i < got && i < n; i++)
        CHECK(y[i] == want[2 * i] && h[i] == want[2 * i + 1],
              "%s: band %d is y=%d h=%d, want y=%d h=%d", what, i,
              y[i], h[i], want[2 * i], want[2 * i + 1]);
    ui_clear_dirty();
}

int main(void)
{
    static const int one[] = { 20, 25 };
    static const int two[] = { 20, 25, 120, 25 };
    static const int overlap[] = { 20, 45 };
    static const int touch[] = { 20, 50 };
    static const int nearest[] = { 20, 50, 200, 25 };
    static const int chain[] = { 20, 105 };

    /* no screen yet: ignored */
    ui_invalidate_band(20, 25);
    expect("no screen", 0, NULL);

    CHECK(ui_push(&dummy, NULL) == 0, "push failed");
    ui_clear_dirty();

    ui_invalidate_band(20, 0);
    ui_invalidate_band(20, -5);
    expect("empty bands", 0, NULL);
    CHECK(!(ui_top()->dirty & UI_DIRTY_BAND), "empty band set the flag");

    ui_invalidate_band(20, 25);
    CHECK(ui_top()->dirty & UI_DIRTY_BAND, "no UI_DIRTY_BAND flag");
    expect("one", 1, one);
    CHECK(ui_top()->dirty == 0, "clear left dirty 0x%x", ui_top()->dirty);

    /* disjoint, given out of order: two bands sorted by y */
    ui_invalidate_band(120, 25);
    ui_invalidate_band(20, 25);
    expect("two disjoint", 2, two);

    /* overlapping and touching bands merge */
    ui_invalidate_band(20, 25);
    ui_invalidate_band(40, 25);
    expect("overlap", 1, overlap);
    ui_invalidate_band(45, 25);
    ui_invalidate_band(20, 25);
    expect("touch", 1, touch);

    /* a third disjoint band merges into the nearest one */
    ui_invalidate_band(20, 25);
    ui_invalidate_band(200, 25);
    ui_invalidate_band(60, 10);         /* gap 15 to the first, 130 */
    expect("nearest", 2, nearest);

    /* a band that grows into the other one coalesces both */
    ui_invalidate_band(20, 25);
    ui_invalidate_band(100, 25);
    ui_invalidate_band(40, 60);         /* 20..100 now touches 100..125 */
    expect("chain", 1, chain);

    test_leave_hook();

    printf("   %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
