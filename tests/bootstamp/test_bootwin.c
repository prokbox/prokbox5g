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

/* Host test (#487): the boot window (bootstamp_core.c boot_window_run())
 * against the hang watch's own rule (panic_record.c hang_overdue(), the
 * shell's 6 s bound), on a simulated tick clock that the watch samples at
 * every tick.
 *
 * 0.9.0 road test: a first boot in Japanese (the full CJK tier loaded
 * before the window) then the window's 3 s hold read "ui no progress
 * 6 s". The window must feed the watch on every pass, and a check that
 * really stalls must still trip it. Literal numbers (HZ 100, a 300-tick
 * hold, the 600-tick bound), not the headers' constants. */
#include <stdio.h>
#include <string.h>
#include "ipodui/bootstamp.h"
#include "panic_record.h"

#define T_HZ        100
#define T_LIMIT     600             /* hang_watch.c: the ui slot's 6 s */
#define T_PEER      200

static int fails;
#define CHECK(c, what) do { if (!(c)) { \
    printf("FAIL line %d: %s (%s)\n", __LINE__, what, #c); fails++; } } while (0)

static long now_t;
static uint32_t beats;
static struct hang_mon mon;
static long fired_at;               /* -1: the watch never fired */
static long last_beat;
static int passes;                  /* beats in the window */
static int looks;                   /* boot checks made */

/* the watcher's tick task, every tick */
static void advance(long ticks)
{
    while (ticks-- > 0)
    {
        now_t++;
        if (fired_at < 0 && hang_overdue(&mon, beats, HANG_ST_WORK, now_t,
                                         T_LIMIT, 0, T_PEER))
            fired_at = now_t;
    }
}

/* the scenario's boot check */
static int check_kind;              /* 0 waiting, 1 stall on pass 3,
                                     * 2 done on pass 4, 3 a slow walk */
static int t_check(int *pm)
{
    looks++;
    advance(1);                     /* a look costs a tick */
    switch (check_kind)
    {
    case 1:
        if (looks == 3)
            advance(700);           /* stuck inside the check: 7 s */
        return 0;
    case 2:
        return looks >= 4 ? 2 : 0;
    case 3:
        *pm = 100;                  /* 10 % after the first look: ~10 s */
        return 1;
    default:
        return 0;
    }
}
static bool event_now;
static bool t_event(void) { return event_now; }
static long t_now(void) { return now_t; }
static void t_wait(long ticks) { advance(ticks); }
static void t_beat(void) { beats++; passes++; last_beat = now_t; }
static void t_nobeat(void) { passes++; }

static void reset(int kind)
{
    now_t = 0;
    beats = 1;                      /* hang_watch_register() */
    mon.seen = beats;
    mon.since = 0;
    fired_at = -1;
    last_beat = 0;
    passes = 0;
    looks = 0;
    check_kind = kind;
    event_now = false;
}

static enum boot_window_end run(void (*beat)(void), int *st)
{
    const struct boot_window_ops ops = { t_check, t_event, t_now, t_wait,
                                         beat };

    return boot_window_run(&ops, now_t + 300, T_HZ, st);
}

int main(void)
{
    enum boot_window_end e;
    long t0;
    int st = -1;

    /* the road test's boot: 4.5 s of shell start (the CJK tier) before
     * the window, the check never done, 2 s of window work after */
    reset(0);
    advance(450);
    t0 = now_t;
    e = run(t_beat, &st);
    CHECK(e == BOOT_WIN_CAP, "the hold ends at its cap");
    CHECK(now_t - t0 >= 300 && now_t - t0 <= 300 + 5 + 1,
          "the hold is 3 s, one pass over at most");
    printf("  slow boot: hold %ld ticks, %d passes\n", now_t - t0, passes);
    advance(200);
    beats++;                        /* the shell's loop: its first pass */
    printf("  slow boot: fired=%ld\n", fired_at);
    CHECK(passes >= 50, "about one pass each 50 ms");
    CHECK(fired_at < 0, "a busy but moving boot never reads as a hang");

    /* the same boot with a window that does not feed the watch: the
     * road-test record (the red arm of this test, kept as a check that
     * the case can fail) */
    reset(0);
    advance(450);
    e = run(t_nobeat, &st);
    advance(200);
    printf("  slow boot, no beats: fired=%ld (since %ld)\n", fired_at,
           mon.since);
    CHECK(fired_at == 600, "without beats the watch fires at 6 s");

    /* a check that really stalls (7 s inside it) still trips the watch,
     * 6 s after the last pass's beat */
    reset(1);
    e = run(t_beat, &st);
    printf("  stalled check: fired=%ld last beat=%ld\n", fired_at,
           last_beat);
    CHECK(fired_at >= 0, "a real stall trips the watch");
    CHECK(fired_at - last_beat >= 600 && fired_at - last_beat <= 601,
          "6 s after the last beat (the watch sees it a tick later)");

    /* the loop's own decisions are unchanged */
    reset(2);
    e = run(t_beat, &st);
    CHECK(e == BOOT_WIN_DONE && st == 2 && passes == 4, "done on the check");
    reset(0);
    event_now = true;
    e = run(t_beat, &st);
    CHECK(e == BOOT_WIN_EVENT && passes == 1, "a key or USB ends it at once");
    reset(3);
    t0 = now_t;
    e = run(t_beat, &st);
    printf("  slow walk: %s after %ld ticks\n", boot_window_end_name(e),
           now_t - t0);
    CHECK(e == BOOT_WIN_HANDOVER, "a walk that can't end inside hands over");
    CHECK(now_t - t0 <= 60, "after half a second of walk");
    CHECK(fired_at < 0, "no hang");
    CHECK(!strcmp(boot_window_end_name(BOOT_WIN_CAP), "cap"),
          "the log's word for the cap");

    if (fails)
        printf("%d failure(s)\n", fails);
    return fails != 0;
}
