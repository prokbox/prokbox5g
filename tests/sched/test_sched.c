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
 * Fit-1 C2 host test: firmware/kernel/include/sched_age.h, and a model of
 * switch_thread() (firmware/kernel/thread.c) that replays the starvation
 * the device logged (S26: no video posts for 420-520 ms while pcmbuf held
 * the codec at 10) under upstream's rule, then checks the bounds the
 * fixed bands plus time-bounded aging promise (docs/fit1-brief.md B).
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

/* Values from firmware/kernel/include/thread.h (HAVE_SCHED_BANDS) */
#define PRIORITY_REALTIME        4
#define PRIORITY_BUFFERING       15
#define PRIORITY_USER_INTERFACE  16
#define PRIORITY_PLAYBACK        16
#define PRIORITY_BACKGROUND      20
#define PRIORITY_CODEC           13
#define PRIORITY_VIDEO_FEED      14
#include "sched_age.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the pure rule ---- */
static void test_rule(void)
{
    /* the highest runnable and its equals always run */
    CHECK(sched_may_run(13, 13, 0, 0), "equal runs");
    CHECK(sched_may_run(10, 13, 0, 0), "higher runs");
    /* realtime threads below the max are never aged past */
    CHECK(!sched_may_run(4, 2, 1000, 1000), "realtime not aged");
    /* interactive band: due after SCHED_AGE_TICKS, whatever the gap */
    CHECK(!sched_may_run(16, 5, 1, 1), "gap 11, 1 tick: wait");
    CHECK(sched_may_run(16, 5, 1, 2), "gap 11, 2 ticks: run");
    CHECK(sched_may_run(14, 13, 1, 2), "gap 1, 2 ticks: run");
    /* upstream's count rule is kept as the fast path */
    CHECK(sched_may_run(16, 13, 10, 0), "count 10 > 3*3");
    CHECK(!sched_may_run(16, 13, 9, 0), "count 9 = 3*3");
    /* background band: 2 more ticks per level below the UI */
    CHECK(sched_age_limit(16) == 2 && sched_age_limit(15) == 2,
          "interactive limit");
    CHECK(sched_age_limit(18) == 6, "system limit %ld", sched_age_limit(18));
    CHECK(sched_age_limit(20) == 10, "background limit %ld",
          sched_age_limit(20));
    CHECK(!sched_may_run(20, 13, 1, 9) && sched_may_run(20, 13, 1, 10),
          "background due at 10 ticks");
}

/* ---- a model of the cooperative scheduler on one core ----
   Threads run a slice and then yield (stay runnable) or block until their
   next wake. Wakes go to the run queue's tail with skip_count = base
   priority (core_rtr_add); the walk starts after the current thread if it
   is still runnable, else at the head; the chosen one becomes the head. */
#define TICK_US 10000L
#define MAXT 8

struct mthread {
    const char *name;
    int prio;
    long period_us;     /* 0: always runnable (yields after each slice) */
    long slice_us;
    /* state */
    bool runnable;
    long next_wake, ready_us;
    int skips;
    long ready_tick;
    long max_wait_us, ran_us;
};

static struct mthread T[MAXT];
static int nt;
static int q[MAXT], qn;     /* run queue, q[0] is the head */

static void q_add(int i)        { q[qn++] = i; }
static void q_remove(int i)
{
    int k, j = 0;
    for (k = 0; k < qn; k++)
        if (q[k] != i)
            q[j++] = q[k];
    qn = j;
}
static void q_make_first(int i)  /* rotate so i is the head */
{
    int k, pos = 0, tmp[MAXT];
    for (k = 0; k < qn; k++)
        if (q[k] == i)
            pos = k;
    for (k = 0; k < qn; k++)
        tmp[k] = q[(pos + k) % qn];
    for (k = 0; k < qn; k++)
        q[k] = tmp[k];
}

static void add_thread(const char *name, int prio, long period, long slice)
{
    struct mthread *t = &T[nt];
    t->name = name; t->prio = prio; t->period_us = period;
    t->slice_us = slice;
    t->runnable = true; t->next_wake = 0; t->ready_us = 0;
    t->skips = prio; t->ready_tick = 0;
    t->max_wait_us = 0; t->ran_us = 0;
    q_add(nt++);
}

static void run_model(bool bands, long dur_us)
{
    long now = 0;
    int cur = -1;

    while (now < dur_us)
    {
        int k, i, max = 99, pick = -1;
        long tick = now / TICK_US;

        /* wakes */
        for (i = 0; i < nt; i++)
            if (!T[i].runnable && T[i].next_wake <= now)
            {
                T[i].runnable = true;
                T[i].ready_us = T[i].next_wake;
                T[i].skips = T[i].prio;
                T[i].ready_tick = tick;
                q_add(i);
            }
        if (!qn)
        {
            long nw = dur_us;
            for (i = 0; i < nt; i++)
                if (!T[i].runnable && T[i].next_wake < nw)
                    nw = T[i].next_wake;
            now = nw;
            cur = -1;
            continue;
        }
        for (k = 0; k < qn; k++)
            if (T[q[k]].prio < max)
                max = T[q[k]].prio;

        /* the walk */
        {
            int start = 0, n;
            if (cur >= 0 && T[cur].runnable)
                for (k = 0; k < qn; k++)
                    if (q[k] == cur)
                        start = (k + 1) % qn;
            for (n = 0; ; n++)
            {
                struct mthread *t = &T[q[(start + n) % qn]];
                bool go;
                if (bands)
                    go = t->prio <= max ||
                         sched_may_run(t->prio, max, ++t->skips,
                                       tick - t->ready_tick);
                else
                {
                    int diff = t->prio - max;
                    go = t->prio <= max ||
                         (t->prio > PRIORITY_REALTIME &&
                          ++t->skips > diff * diff);
                }
                if (go)
                {
                    pick = q[(start + n) % qn];
                    break;
                }
            }
        }

        /* run it */
        {
            struct mthread *t = &T[pick];
            long w = now - t->ready_us;
            if (w > t->max_wait_us)
                t->max_wait_us = w;
            t->skips = 0;
            q_make_first(pick);
            now += t->slice_us;
            t->ran_us += t->slice_us;
            t->ready_us = now;
            t->ready_tick = now / TICK_US;
            if (t->period_us)
            {
                t->runnable = false;
                t->next_wake += t->period_us;
                if (t->next_wake < now)
                    t->next_wake = now;
                q_remove(pick);
            }
            cur = pick;
        }
    }
}

static void reset(void) { nt = 0; qn = 0; }

/* Upstream: pcmbuf holds the codec at 10 through a refill; the video feed
   and the UI at 16. The codec yields once per frame (get_command), so its
   slice is a frame's decode: 8-20 ms at 30-80 MHz depending on codec */
static void upstream_case(long slice, long expect_min_us)
{
    reset();
    add_thread("codec", 10, 0, slice);
    add_thread("video", PRIORITY_PLAYBACK, 33333, 1500);
    add_thread("ui", PRIORITY_USER_INTERFACE, 16667, 4000);
    run_model(false, 2000000);
    printf("upstream ladder (codec 10, slice %2ld ms): video max wait "
           "%3ld ms, ui %3ld ms\n", slice / 1000,
           T[1].max_wait_us / 1000, T[2].max_wait_us / 1000);
    CHECK(T[1].max_wait_us >= expect_min_us,
          "model should replay S26's starvation (got %ld ms)",
          T[1].max_wait_us / 1000);
}

/* Fixed bands with time-bounded aging, plus a busy database build */
static void bands_case(long slice)
{
    const long dur = 2000000;   /* a 2 s refill: the codec never blocks */
    int i, k;

    reset();
    add_thread("codec", PRIORITY_CODEC, 0, slice);
    add_thread("video", PRIORITY_VIDEO_FEED, 33333, 1500);
    add_thread("buffering", PRIORITY_BUFFERING, 100000, 3000);
    add_thread("ui", PRIORITY_USER_INTERFACE, 16667, 4000);
    add_thread("tagcache", PRIORITY_BACKGROUND, 0, 10000);
    run_model(true, dur);
    for (i = 0; i < nt; i++)
    {
        /* the bound: its limit (+1 tick of quantisation) plus one slice
           of every other thread, which may all be ahead of it */
        long bound = (sched_age_limit(T[i].prio) + 1) * TICK_US;
        for (k = 0; k < nt; k++)
            if (k != i)
                bound += T[k].slice_us;
        printf("bands (slice %2ld ms): %-9s prio %2d max wait %3ld ms "
               "(bound %3ld), cpu %4.1f %%\n", slice / 1000, T[i].name,
               T[i].prio, T[i].max_wait_us / 1000, bound / 1000,
               100.0 * T[i].ran_us / dur);
        if (i > 0)
            CHECK(T[i].max_wait_us <= bound, "%s waited %ld us > %ld",
                  T[i].name, T[i].max_wait_us, bound);
    }
    /* the codec keeps most of the core (80 MHz MP3 needs about 15 %) */
    CHECK(T[0].ran_us * 100 / dur >= 50, "codec share %ld %%",
          T[0].ran_us * 100 / dur);
    /* the feed posts at least every 2 frames even mid-refill */
    CHECK(T[1].max_wait_us <= 2 * 33333 - 1500, "video %ld us",
          T[1].max_wait_us);
}

int main(void)
{
    test_rule();
    upstream_case(8000, 150000);
    upstream_case(20000, 400000);       /* S26: 420-520 ms */
    bands_case(8000);
    bands_case(20000);

    if (fails)
    {
        printf("sched: %d FAILED\n", fails);
        return 1;
    }
    printf("sched: all passed\n");
    return 0;
}
