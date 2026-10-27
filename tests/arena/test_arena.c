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

/* Host test of the P11 screen arena (docs/phase11-brief.md section 4.1,
 * Rev 1 M-1) in apps/ipodui/screen_stack.c: mark/reset LIFO through
 * ui_push_t / ui_pop / ui_pop_to_root, a failed enter() resetting to its
 * mark (also after it took memory), ui_pop_to_root() from depth 8,
 * alignment and zeroing, the full arena, and the deepest reachable
 * chains of section 4.1 summed from the per-state bounds. GPL v2+ */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ipodui/screen_stack.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
        printf("\n"); } } while (0)

/* the trace (tests/ipodui/stub/debug.h with STUB_DEBUGF) */
static char last_trace[256];
static int full_traces, push_traces;
void stub_debugf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(last_trace, sizeof(last_trace), fmt, ap);
    va_end(ap);
    if (strstr(last_trace, "arena full"))
        full_traces++;
    if (strstr(last_trace, "arena push"))
        push_traces++;
}

/* -- test screens --------------------------------------------------------- */

struct t_arg { size_t a, b; int fail; };   /* two allocs, then fail? */
static void *got_a, *got_b;
static int leaves;

static int t_enter(struct ui_screen *s, const void *arg)
{
    const struct t_arg *t = arg;

    (void)s;
    got_a = got_b = NULL;
    if (!t)
        return 0;
    if (t->a && !(got_a = ui_arena_alloc(t->a)))
        return -1;
    if (t->b && !(got_b = ui_arena_alloc(t->b)))
        return -1;
    if (got_a)
        memset(got_a, 0xa5, t->a);  /* the next owner must see zeros */
    return t->fail ? -1 : 0;
}

static void t_leave(struct ui_screen *s)
{
    (void)s;
    leaves++;
}

static const struct ui_screen_ops ops_root = { .name = "root" };
static const struct ui_screen_ops ops_t = { .name = "t", .enter = t_enter,
                                            .leave = t_leave };

static unsigned char block[UI_ARENA_SIZE + UI_ARENA_ALIGN + 16];

static void reset_stack(void)
{
    ui_pop_to_root();
}

static void test_basic(void)
{
    void *p, *q;
    size_t f0;

    /* misaligned base: aligned up, still a full UI_ARENA_SIZE */
    ui_arena_init(block + 3, UI_ARENA_SIZE + UI_ARENA_ALIGN - 1);
    f0 = ui_arena_free();
    CHECK(f0 == UI_ARENA_SIZE, "free %zu", f0);
    CHECK(ui_arena_mark() == 0, "mark %u", ui_arena_mark());

    p = ui_arena_alloc(3);
    q = ui_arena_alloc(5);
    CHECK(p && q, "allocs");
    CHECK(((uintptr_t)p % UI_ARENA_ALIGN) == 0, "p aligned");
    CHECK(((uintptr_t)q % UI_ARENA_ALIGN) == 0, "q aligned");
    CHECK((unsigned char *)q - (unsigned char *)p == UI_ARENA_ALIGN,
          "q follows p at the next alignment");
    CHECK(ui_arena_free() == UI_ARENA_SIZE - 2 * UI_ARENA_ALIGN,
          "free after two %zu", ui_arena_free());
    ui_arena_reset(0);
    CHECK(ui_arena_free() == f0, "reset to 0");

    /* a bad mark (above the top) is ignored */
    p = ui_arena_alloc(16);
    ui_arena_reset(100);
    CHECK(ui_arena_mark() == 16, "bad mark ignored: top %u", ui_arena_mark());
    ui_arena_reset(0);

    /* the whole arena, then nothing */
    full_traces = 0;
    p = ui_arena_alloc(UI_ARENA_SIZE);
    CHECK(p != NULL, "whole arena");
    CHECK(ui_arena_alloc(1) == NULL, "past the end");
    CHECK(full_traces == 1, "arena full traced (%d)", full_traces);
    CHECK(strstr(last_trace, "need=1") != NULL, "trace: %s", last_trace);
    ui_arena_reset(0);
    CHECK(ui_arena_alloc(UI_ARENA_SIZE + 1) == NULL, "too big");
    ui_arena_reset(0);

    /* no arena: every alloc fails, free 0 */
    ui_arena_init(NULL, 0);
    CHECK(ui_arena_alloc(8) == NULL && ui_arena_free() == 0, "no arena");
    ui_arena_init(block, sizeof(block));
    CHECK(ui_arena_free() == UI_ARENA_SIZE, "capped at UI_ARENA_SIZE");
}

static void test_stack(void)
{
    struct t_arg a100 = { 100, 0, 0 }, a2x = { 40, 300, 0 };
    struct t_arg fail_after = { 64, 128, 1 };
    struct t_arg fail_alloc = { 64, UI_ARENA_SIZE + 1, 0 };
    uint16_t m1, m2;
    unsigned char *pa;
    int i;

    ui_arena_init(block, sizeof(block));
    CHECK(ui_push(&ops_root, NULL) == 0, "root");
    CHECK(ui_arena_mark() == 0, "root takes nothing");

    /* LIFO: each screen's memory is released with it */
    CHECK(ui_push(&ops_t, &a100) == 0, "push a100");
    m1 = ui_arena_mark();
    CHECK(m1 == 100, "top %u", m1);
    CHECK(ui_top()->arena_mark == 0, "mark recorded before enter");
    CHECK(strstr(last_trace, "arena push s=t n=100") != NULL, "trace: %s",
          last_trace);
    CHECK(ui_push(&ops_t, &a2x) == 0, "push two blocks");
    m2 = ui_arena_mark();
    CHECK(m2 == 104 + 40 + 300, "two blocks from one screen: %u", m2);
    CHECK(ui_top()->arena_mark == m1, "second mark %u", ui_top()->arena_mark);
    ui_pop();
    CHECK(ui_arena_mark() == m1, "pop resets to the mark: %u", ui_arena_mark());
    ui_pop();
    CHECK(ui_arena_mark() == 0, "back to root: %u", ui_arena_mark());

    /* a failed enter() after taking memory leaks nothing, the top screen
     * and the depth are unchanged, leave() is not called */
    CHECK(ui_push(&ops_t, &a100) == 0, "push a100");
    leaves = 0;
    CHECK(ui_push(&ops_t, &fail_after) < 0, "enter fails");
    CHECK(ui_arena_mark() == 100, "failed enter reset: %u", ui_arena_mark());
    CHECK(ui_depth() == 2, "depth %d", ui_depth());
    CHECK(leaves == 0, "no leave for a failed enter");
    full_traces = 0;
    CHECK(ui_push(&ops_t, &fail_alloc) < 0, "alloc fails");
    CHECK(full_traces == 1, "full traced");
    CHECK(ui_arena_mark() == 100, "failed alloc reset: %u", ui_arena_mark());

    /* the next owner of reused memory sees zeros */
    ui_pop();
    CHECK(ui_push(&ops_t, &a100) == 0, "push again");
    pa = got_a;
    ui_pop();
    {
        struct t_arg z = { 0, 0, 0 };
        unsigned char *p;
        CHECK(ui_push(&ops_t, &z) == 0, "push empty");
        p = ui_arena_alloc(100);
        CHECK(p == pa, "same memory");
        for (i = 0; i < 100 && p; i++)
            if (p[i])
                break;
        CHECK(i == 100, "zeroed");
        ui_pop();
    }

    /* ui_pop_to_root() from depth 8: everything goes */
    for (i = 1; i < UI_STACK_MAX; i++)
        CHECK(ui_push(&ops_t, &a100) == 0, "push %d", i);
    CHECK(ui_depth() == UI_STACK_MAX, "depth %d", ui_depth());
    CHECK(ui_arena_mark() == 100 + 104 * (UI_STACK_MAX - 2),
          "8 deep: %u", ui_arena_mark());
    CHECK(ui_push(&ops_t, &a100) < 0, "stack full");
    CHECK(ui_arena_mark() == 100 + 104 * (UI_STACK_MAX - 2),
          "a refused push takes nothing");
    leaves = 0;
    ui_pop_to_root();
    CHECK(leaves == UI_STACK_MAX - 1, "leaves %d", leaves);
    CHECK(ui_arena_mark() == 0, "pop_to_root: %u", ui_arena_mark());
    CHECK(ui_depth() == 1, "root only");

    /* the hold menu's pop-pop-push (H2: Browse Album from Now Playing) */
    {
        struct t_arg np = { 0, 0, 0 }, hold = { UI_ARENA_B_HOLD, 0, 0 },
                     br = { UI_ARENA_B_BROWSE, 0, 0 };
        struct t_arg pl = { UI_ARENA_B_PLAYLISTS, 0, 0 };
        uint16_t base;

        CHECK(ui_push(&ops_t, &pl) == 0, "playlists");
        base = ui_arena_mark();
        CHECK(ui_push(&ops_t, &np) == 0, "now playing (no arena)");
        CHECK(ui_push(&ops_t, &hold) == 0, "hold");
        ui_pop();
        ui_pop();
        CHECK(ui_arena_mark() == base, "pop-pop: %u", ui_arena_mark());
        CHECK(ui_push(&ops_t, &br) == 0, "browse album");
        CHECK(ui_arena_mark() == ((base + 7) & ~7) + UI_ARENA_B_BROWSE,
              "push after pop-pop: %u", ui_arena_mark());
        ui_pop_to_root();
    }
    reset_stack();
}

/* Section 4.1: the deepest reachable chains, from the per-state bounds,
 * plus alignment slack per level, fit the arena. */
static void test_chains(void)
{
    static const struct { const char *name; int n; int b[8]; } chains[] = {
        { "Playlists > On-The-Go > hold > Browse Album > Songs", 5,
          { UI_ARENA_B_PLAYLISTS, UI_ARENA_B_OTG, UI_ARENA_B_HOLD,
            UI_ARENA_B_BROWSE, UI_ARENA_B_BROWSE } },
        { "Music > Search > Album > Songs > hold > Browse Album", 5,
          { UI_ARENA_B_SEARCH, UI_ARENA_B_BROWSE, UI_ARENA_B_BROWSE,
            UI_ARENA_B_HOLD, UI_ARENA_B_BROWSE } },
        { "Settings > Date & Time > Time Zone region > city", 4,
          { UI_ARENA_B_SETTING, UI_ARENA_B_SETTING, UI_ARENA_B_CHOICE,
            UI_ARENA_B_CHOICE } },
        { "Settings > Legal", 2, { UI_ARENA_B_SETTING, UI_ARENA_B_LEGAL } },
        { "Audiobooks > book page > hold", 3,
          { UI_ARENA_B_BROWSE, UI_ARENA_B_BOOK_PAGE, UI_ARENA_B_HOLD } },
        /* #126: Now Playing's Visualizer over the deepest list chain,
         * and the charging screen over it (the style list over Now
         * Playing takes the view's part back first: viz.c) */
        { "Search chain > Now Playing Visualizer > Charging", 7,
          { UI_ARENA_B_SEARCH, UI_ARENA_B_BROWSE, UI_ARENA_B_BROWSE,
            UI_ARENA_B_HOLD, UI_ARENA_B_BROWSE, UI_ARENA_B_VIZ,
            UI_ARENA_B_CHARGING } },
        { "Settings > Charging (pushed anywhere) over the Search chain", 6,
          { UI_ARENA_B_SEARCH, UI_ARENA_B_BROWSE, UI_ARENA_B_BROWSE,
            UI_ARENA_B_HOLD, UI_ARENA_B_BROWSE, UI_ARENA_B_CHARGING } },
    };
    int worst = 0;
    unsigned c;

    for (c = 0; c < sizeof(chains) / sizeof(chains[0]); c++)
    {
        int i, sum = UI_STACK_MAX * UI_ARENA_ALIGN;    /* slack per level */

        for (i = 0; i < chains[c].n; i++)
            sum += chains[c].b[i];
        CHECK(sum <= UI_ARENA_SIZE, "%s: %d > %d", chains[c].name, sum,
              UI_ARENA_SIZE);
        if (sum > worst)
            worst = sum;
    }
    printf("     worst chain %d B of %d (margin %d)\n", worst, UI_ARENA_SIZE,
           UI_ARENA_SIZE - worst);

    /* and for real: the Search chain through the stack */
    ui_arena_init(block, sizeof(block));
    reset_stack();
    {
        struct t_arg s = { UI_ARENA_B_SEARCH, 0, 0 },
                     b = { UI_ARENA_B_BROWSE, 0, 0 },
                     h = { UI_ARENA_B_HOLD, 0, 0 };
        CHECK(ui_push(&ops_t, &s) == 0 && ui_push(&ops_t, &b) == 0
              && ui_push(&ops_t, &b) == 0 && ui_push(&ops_t, &h) == 0
              && ui_push(&ops_t, &b) == 0, "search chain pushes");
        CHECK(ui_arena_free() >= 3000, "margin %zu", ui_arena_free());
        reset_stack();
        CHECK(ui_arena_mark() == 0, "chain released");
    }
}

/* #126: ui_arena_pop gives back the newest block only */
static void test_pop(void)
{
    void *a, *b;

    ui_arena_init(block, sizeof(block));
    a = ui_arena_alloc(100);
    b = ui_arena_alloc(UI_ARENA_B_VIZ);
    CHECK(a && b, "allocs");
    ui_arena_pop(a, 100);                       /* not the newest: ignored */
    CHECK(ui_arena_mark() == 104 + UI_ARENA_B_VIZ, "pop of an older block");
    ui_arena_pop(b, UI_ARENA_B_VIZ);
    CHECK(ui_arena_mark() == 104, "pop %u", (unsigned)ui_arena_mark());
    ui_arena_pop(a, 100);
    CHECK(ui_arena_mark() == 0, "pop to 0");
}

int main(void)
{
    test_pop();
    test_basic();
    test_stack();
    test_chains();
    if (fails)
        printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
