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

/* K-1, the parked COP (firmware/export/cop_park.h; docs/design/
 * p19-placement.md section 3.4; core-layout-decision.md section 7 step 1).
 *
 * Part A, a discrete-time model of the COP's scheduler as thread.c runs it
 * (next_tmo_check, the timeout check, core_schedule_wakeup's core_wake) and
 * the CPU's tick through the real cop_park_tick_wakes().  Run twice, the
 * pulse (park off) and K-1 (park on), on the same random work: every post
 * must run before the next tick, every timeout on the same tick in both
 * arms (the first tick at or after its deadline), and the parked COP's
 * wakes must fall to the work's rate.  The work is the COP's real waiters:
 *   - the job worker, semaphore_wait(work, TIMEOUT_BLOCK) (cop_job.c);
 *   - the split's service loop, queue_wait_w_tmo(be_queue, left)
 *     (fe_be.c);
 *   - a sleep(1) poll (fe_be.c's start wait);
 *   - a bounded semaphore_wait(HZ/10) that is usually posted first.
 *
 * Part B, two host threads running upstream's PP502x core_sleep() and
 * core_wake() handshake (thread-pp.c's asm, transcribed bit for bit onto
 * a model mailbox and PROC_CTL) with no tick at all: the CPU posts, the
 * COP sleeps whenever it finds nothing.  A lost wake-up shows as a post
 * nobody takes; the watchdog fails the test.  K-1 leans on this handshake
 * alone, where upstream also had the pulse to hide a lost one. */

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cop_park.h"

#define HZ 100
#define TIME_BEFORE(a, b) ((long)((unsigned long)(a) - (unsigned long)(b)) < 0)

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); \
                                       printf("\n"); } } while (0)

/* ---------------- Part A: the scheduler model ---------------- */

enum kind { K_JOB, K_SVC, K_POLL, K_BOUNDED, K_N };
enum st { S_RUN, S_BLK, S_BLK_TMO };

struct th
{
    enum kind kind;
    enum st st;
    long tmo;           /* tmo_tick while S_BLK_TMO */
    bool on_tmo_list;   /* registered (stale entries stay, as in thread.c) */
    long posted_at;     /* sub-tick time (us) of a pending post, or -1 */
    long due;           /* the deadline the thread armed (tick) */
    int wakes, fired;
};

#define NT K_N
struct model
{
    struct th t[NT];
    long next_tmo_check;
    long tick;          /* current_tick */
    bool asleep;
    long cop_wakes;
    long late_posts, timeouts, max_tmo_late, posts;
    long fire_log[200000];
    long fire_n;
};

/* thread.c prepare_block(): register a timeout */
static void block(struct model *m, struct th *t, int timeout)
{
    if (timeout < 0) { t->st = S_BLK; return; }
    t->st = S_BLK_TMO;
    t->tmo = m->tick + timeout;
    t->due = t->tmo;
    t->on_tmo_list = true;
    if (TIME_BEFORE(t->tmo, m->next_tmo_check))
        m->next_tmo_check = t->tmo;
}

/* thread.c check_tmo_expired_inner() */
static void tmo_check(struct model *m)
{
    long next = m->tick + 60 * HZ;
    for (int i = 0; i < NT; i++)
    {
        struct th *t = &m->t[i];
        if (!t->on_tmo_list)
            continue;
        if (t->st == S_BLK_TMO && TIME_BEFORE(m->tick, t->tmo))
        {
            if (TIME_BEFORE(t->tmo, next))
                next = t->tmo;
        }
        else
        {
            t->on_tmo_list = false;
            if (t->st == S_BLK_TMO)
            {
                long late = m->tick - t->due;
                if (late > m->max_tmo_late)
                    m->max_tmo_late = late;
                t->st = S_RUN;
                t->fired++;
                m->timeouts++;
                if (m->fire_n < (long)(sizeof m->fire_log / sizeof m->fire_log[0]))
                    m->fire_log[m->fire_n++] = (long)i << 24 | (m->tick & 0xffffff);
            }
        }
    }
    m->next_tmo_check = next;
}

static unsigned rnd(unsigned *s)
{
    *s = *s * 1103515245u + 12345u;
    return (*s >> 16) & 0x7fff;
}

/* A thread that ran blocks again, as its real loop does */
static void rearm(struct model *m, struct th *t, unsigned *seed)
{
    switch (t->kind)
    {
    case K_JOB:     block(m, t, -1); break;                     /* TIMEOUT_BLOCK */
    case K_SVC:     block(m, t, 1 + (int)(rnd(seed) % 300)); break; /* left */
    case K_POLL:    block(m, t, 1); break;                      /* sleep(1) */
    case K_BOUNDED: block(m, t, HZ / 10); break;
    default: break;
    }
}

/* The COP runs: the timeout check, then every runnable thread until none
 * is (switch_thread's loop), then it sleeps */
static void cop_run(struct model *m, long now_us, unsigned *seed)
{
    if (m->asleep)
    {
        m->asleep = false;
        m->cop_wakes++;
    }
    for (;;)
    {
        if (!TIME_BEFORE(m->tick, m->next_tmo_check))
            tmo_check(m);
        int ran = 0;
        for (int i = 0; i < NT; i++)
        {
            struct th *t = &m->t[i];
            if (t->st != S_RUN)
                continue;
            if (t->posted_at >= 0)
            {
                /* the post runs before the next tick */
                if (now_us - t->posted_at >= 1000000 / HZ)
                    m->late_posts++;
                t->posted_at = -1;
            }
            t->wakes++;
            rearm(m, t, seed);
            ran++;
        }
        if (!ran)
            break;
    }
    m->asleep = true;
}

/* thread.c core_schedule_wakeup() from the CPU: runnable, core_wake(COP) */
static void cpu_post(struct model *m, int i, long now_us, unsigned *seed)
{
    struct th *t = &m->t[i];
    if (t->st == S_RUN)
        return;
    t->st = S_RUN;              /* a stale tmo entry stays on the list */
    t->posted_at = now_us;
    m->posts++;
    cop_run(m, now_us, seed);   /* core_wake: the COP runs at once */
}


static struct model ma, mb;

static void run_arm(struct model *m, bool park, long secs, int post_per_s,
                    unsigned seed0, int mode)
{
    unsigned seed = seed0, wseed = seed0 ^ 0x5a5a;
    memset(m, 0, sizeof *m);
    for (int i = 0; i < NT; i++)
    {
        m->t[i].kind = (enum kind)i;
        m->t[i].posted_at = -1;
    }
    m->next_tmo_check = 60 * HZ;
    m->asleep = false;
    if (mode == 1)
        m->t[K_POLL].kind = K_JOB;  /* no poll: posts and bounded waits */
    if (mode == 2)
    {
        /* dark play with the decode on the CPU: only the job worker, blocked
           for ever, and nothing posted; the other waiters are absent */
        for (int i = 0; i < NT; i++)
            m->t[i].st = S_BLK;
        m->t[K_SVC].kind = K_JOB;
        m->t[K_POLL].kind = K_JOB;
        m->t[K_BOUNDED].kind = K_JOB;
    }
    else
    {
        for (int i = 0; i < NT; i++)
            m->t[i].st = S_RUN;
    }
    cop_run(m, 0, &seed);
    for (long k = 1; k <= secs * HZ; k++)
    {
        long t0 = k * (1000000 / HZ);
        /* the work between ticks: posts at random sub-tick instants (the
           same sequence in both arms: its own seed) */
        int n = post_per_s / HZ +
                ((int)(rnd(&wseed) % HZ) < post_per_s % HZ);
        for (int j = 0; j < n; j++)
        {
            int who = (int)(rnd(&wseed) % NT);
            long at = t0 - (long)(rnd(&wseed) % (1000000 / HZ));
            if (who == K_POLL)
                continue;       /* the poll is never posted */
            cpu_post(m, who, at, &seed);
        }
        /* the tick (kernel-pp.c TIMER1): current_tick++, then the rule */
        m->tick = k;
        if (cop_park_tick_wakes(park, m->tick, m->next_tmo_check))
            cop_run(m, t0, &seed);
    }
}

static void part_a(void)
{
    const long S = 600;     /* ten minutes of model time */

    /* the busy mix: about 50 posts a second over the four waiters */
    run_arm(&ma, false, S, 50, 1234, 0);
    run_arm(&mb, true, S, 50, 1234, 0);
    printf("     A busy: pulse %ld COP wakes (%.1f/s), K-1 %ld (%.1f/s); "
           "posts %ld/%ld, timeouts %ld/%ld\n",
           ma.cop_wakes, (double)ma.cop_wakes / S,
           mb.cop_wakes, (double)mb.cop_wakes / S,
           ma.posts, mb.posts, ma.timeouts, mb.timeouts);
    CHECK(ma.late_posts == 0 && mb.late_posts == 0,
          "a post waited a tick (pulse %ld, K-1 %ld)", ma.late_posts,
          mb.late_posts);
    CHECK(ma.max_tmo_late == 0 && mb.max_tmo_late == 0,
          "a timeout fired late (pulse %ld, K-1 %ld ticks)", ma.max_tmo_late,
          mb.max_tmo_late);
    CHECK(ma.timeouts == mb.timeouts && ma.fire_n == mb.fire_n &&
          !memcmp(ma.fire_log, mb.fire_log, sizeof(long) * (size_t)ma.fire_n),
          "the timeouts differ between the arms (%ld vs %ld)", ma.timeouts,
          mb.timeouts);
    CHECK(ma.posts == mb.posts, "the posts differ (%ld vs %ld)", ma.posts,
          mb.posts);
    for (int i = 0; i < NT; i++)
        CHECK(mb.t[i].wakes > 0, "waiter %d never ran under K-1", i);
    /* the poll's sleep(1) runs every tick in both arms: the parked COP's
       wakes are the work's, never more than the pulse's */
    CHECK(mb.cop_wakes <= ma.cop_wakes, "K-1 woke the COP more (%ld > %ld)",
          mb.cop_wakes, ma.cop_wakes);

    /* the same without the sleep(1) poll: about 20 posts a second to a
       worker, a service loop with its own deadlines, a bounded wait */
    run_arm(&ma, false, S, 20, 4321, 1);
    run_arm(&mb, true, S, 20, 4321, 1);
    printf("     A work: pulse %ld COP wakes (%.1f/s), K-1 %ld (%.1f/s); "
           "posts %ld, timeouts %ld/%ld\n", ma.cop_wakes,
           (double)ma.cop_wakes / S, mb.cop_wakes, (double)mb.cop_wakes / S,
           mb.posts, ma.timeouts, mb.timeouts);
    CHECK(ma.late_posts == 0 && mb.late_posts == 0 &&
          ma.max_tmo_late == 0 && mb.max_tmo_late == 0,
          "work arm: a late post or timeout");
    CHECK(ma.fire_n == mb.fire_n &&
          !memcmp(ma.fire_log, mb.fire_log, sizeof(long) * (size_t)ma.fire_n),
          "work arm: the timeouts differ between the arms");
    /* the parked COP wakes at the work's rate: one per timeout, the minute
       check, and per post at most two (the post's own wake; and, when it
       came before the thread's deadline, the tick that finds the stale
       entry thread.c leaves on the list and collects at that deadline) */
    CHECK(mb.cop_wakes <= 2 * mb.posts + mb.timeouts + S / 60 + 1,
          "work arm: K-1 woke the COP %ld times for %ld posts and %ld "
          "timeouts", mb.cop_wakes, mb.posts, mb.timeouts);
    CHECK(mb.cop_wakes * 2 < ma.cop_wakes, "work arm: K-1 saved under half "
          "the wakes (%ld vs %ld)", mb.cop_wakes, ma.cop_wakes);

    /* the dark case: nothing on the COP but a worker blocked for ever */
    run_arm(&ma, false, S, 0, 99, 2);
    run_arm(&mb, true, S, 0, 99, 2);
    printf("     A dark: pulse %ld COP wakes (%.2f/s), K-1 %ld (%.3f/s)\n",
           ma.cop_wakes, (double)ma.cop_wakes / S, mb.cop_wakes,
           (double)mb.cop_wakes / S);
    CHECK(ma.cop_wakes >= S * HZ - 1, "the pulse arm missed ticks (%ld)",
          ma.cop_wakes);
    /* upstream's once-a-minute keep-alive check, nothing else */
    CHECK(mb.cop_wakes <= S / 60 + 1, "a parked idle COP woke %ld times in "
          "%ld s (the floor: one a minute)", mb.cop_wakes, S);

    /* the rule's edges, wrap included */
    CHECK(cop_park_tick_wakes(false, 5, 100), "park off must always pulse");
    CHECK(!cop_park_tick_wakes(true, 99, 100), "a check not yet due woke it");
    CHECK(cop_park_tick_wakes(true, 100, 100), "a due check did not wake it");
    CHECK(cop_park_tick_wakes(true, (long)0x80000001L, (long)0x7fffffffL),
          "the tick's wrap lost a due check");
    CHECK(!cop_park_tick_wakes(true, (long)0x7ffffffeL, (long)0x80000001L),
          "the tick's wrap woke it early");
}

/* ---------------- Part B: the handshake, two threads ---------------- */

#define CPU 0
#define COP 1
#define PROC_SLEEP 0x80000000u

static _Atomic uint32_t mbx_stat;       /* MBX_MSG_STAT: set by SET, cleared by CLR */
static _Atomic uint32_t proc_ctl[2];
static _Atomic long posted, taken;
static _Atomic int stop;
static long b_sleeps, b_skips;

static void mbx_set(uint32_t v) { atomic_fetch_or(&mbx_stat, v); }
static void mbx_clr(uint32_t v) { atomic_fetch_and(&mbx_stat, ~v); }

/* thread-pp.c core_sleep() (PP502x asm), line for line */
static void core_sleep(unsigned core)
{
    uint32_t r0 = 4u << core;
    mbx_set(r0);                                /* intent to sleep */
    if (!(atomic_load(&mbx_stat) & (r0 << 2)))  /* no wake intent for me */
    {
        atomic_store(&proc_ctl[core], PROC_SLEEP);
        b_sleeps++;
        /* the core halts here until the other writes 0 */
        while (atomic_load(&proc_ctl[core]) & PROC_SLEEP)
            sched_yield();
        atomic_store(&proc_ctl[core], 0);
    }
    else
        b_skips++;
    mbx_clr(r0 | (r0 << 2));                    /* both intents off */
    while (atomic_load(&mbx_stat) & (r0 >> 2))  /* the waker is done */
        sched_yield();
}

/* thread-pp.c core_wake() (PP502x asm), line for line */
static void core_wake(unsigned oc)
{
    uint32_t r2 = 0x11u << oc, r1;
    mbx_set(r2);                                /* intent to wake */
    for (;;)
    {
        r1 = atomic_load(&mbx_stat) ^ 0xc;
        if (r1 & (r2 >> 2))
        {   /* it does not intend to sleep */
            r1 = atomic_load(&proc_ctl[oc]);
            break;
        }
        r1 = atomic_load(&proc_ctl[oc]);
        if (r1 & PROC_SLEEP)
            break;                              /* asleep: wake it */
        sched_yield();                          /* about to: let it */
    }
    if (r1 & PROC_SLEEP)
        atomic_store(&proc_ctl[oc], 0);
    mbx_clr(r2 >> 4);                           /* wake procedure done */
}

static void *cop_main(void *arg)
{
    (void)arg;
    while (!atomic_load(&stop))
    {
        /* switch_thread(): the run queue under its lock, then sleep */
        if (atomic_load(&taken) < atomic_load(&posted))
        {
            atomic_fetch_add(&taken, 1);
            continue;
        }
        core_sleep(COP);
    }
    return NULL;
}

static void part_b(void)
{
    const long N = 200000;
    pthread_t t;
    long last_taken = 0;
    struct timespec t0, now;

    pthread_create(&t, NULL, cop_main, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long i = 0; i < N; i++)
    {
        atomic_fetch_add(&posted, 1);       /* core_rtr_add() */
        core_wake(COP);                     /* core_schedule_wakeup() */
        if ((i & 63) == 0)
        {   /* a gap now and then, so the COP goes to sleep between posts */
            struct timespec gap = { 0, 20000 };
            nanosleep(&gap, NULL);
        }
    }
    /* no tick: only the handshake may get the last post taken */
    for (;;)
    {
        long tk = atomic_load(&taken);
        if (tk == N)
            break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - t0.tv_sec > 20 || (tk == last_taken &&
            now.tv_sec - t0.tv_sec > 10))
            break;
        last_taken = tk;
        sched_yield();
    }
    long tk = atomic_load(&taken);
    atomic_store(&stop, 1);
    core_wake(COP);
    pthread_join(t, NULL);
    printf("     B handshake: %ld posts, %ld taken, the COP slept %ld times "
           "(%ld sleeps refused by a pending wake)\n", N, tk, b_sleeps,
           b_skips);
    CHECK(tk == N, "a lost wake-up: %ld of %ld posts taken with no tick",
          tk, N);
    CHECK(b_sleeps > 0, "the COP never slept (the test did not test)");
}

int main(void)
{
    part_a();
    part_b();
    return fails ? 1 : 0;
}
