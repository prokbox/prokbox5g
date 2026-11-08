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

/*
 * The governor's idle read against its writer (firmware/export/pgov_idle.h),
 * one store at a time.  The writer's stores (core_sleep()'s entry and
 * wake, recorded from the real inline code) are replayed into every
 * position among the reader's loads and time samples; the reader is the
 * real pgov_idle_read(), run live with a hook on each access.  Every
 * schedule must give reads that never fall, never grow by more than the
 * time between them, and equal the writer's own account at the time read.
 * The old reader and writer (a re-check of the sleep stamp only) run
 * through the same harness and must be caught.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static uint32_t hook_now(void);
static uint32_t hook_ld(volatile uint32_t *p);
static void hook_st(volatile uint32_t *p, uint32_t v);
#define PGOV_IDLE_NOW()         hook_now()
#define PGOV_IDLE_LD(lv)        hook_ld(&(lv))
#define PGOV_IDLE_ST(lv, v)     hook_st(&(lv), (v))
#include "pgov_idle.h"

#define MAXSTEPS 16
#define LMAX     16             /* writer positions: before reader event 0..LMAX */
#define NREADS   3

enum { S_STORE, S_TIME };
struct step { int kind; size_t off; uint32_t v; };

static struct pgov_idle real, shadow;
static struct step steps[MAXSTEPS];
static int nsteps, recording, in_writer;
static uint32_t rec_times[MAXSTEPS];
static int rec_ntime;
static int pos[MAXSTEPS], applied;
static uint32_t T;
static int revent;

static void apply_upto(int k)
{
    while (applied < nsteps && pos[applied] <= k)
    {
        struct step *s = &steps[applied++];
        if (s->kind == S_STORE)
            *(volatile uint32_t *)((char *)&real + s->off) = s->v;
        T++;                    /* every event takes one tick */
    }
}

static void reader_event(void)
{
    apply_upto(revent++);
}

static uint32_t hook_now(void)
{
    if (in_writer)
    {
        uint32_t t = rec_times[rec_ntime++];
        if (recording)
            steps[nsteps++] = (struct step){ S_TIME, 0, t };
        return t;
    }
    reader_event();
    return T++;
}

static uint32_t hook_ld(volatile uint32_t *p)
{
    uint32_t v;

    if (in_writer)
        return *p;              /* the writer reads its own shadow */
    reader_event();
    v = *p;
    T++;
    return v;
}

static void hook_st(volatile uint32_t *p, uint32_t v)
{
    *p = v;
    if (recording)
        steps[nsteps++] = (struct step){ S_STORE,
            (size_t)((char *)p - (char *)&shadow), v };
}

/* ---- the old pair (before the fix), the same hooks ---------------------- */
static uint32_t old_enter(volatile struct pgov_idle *a)
{
    uint32_t t0 = PGOV_IDLE_NOW() | 1;
    PGOV_IDLE_ST(a->t0, t0);
    return t0;
}

static void old_leave(volatile struct pgov_idle *a, uint32_t t0)
{
    uint32_t idle = PGOV_IDLE_LD(a->idle_us);
    PGOV_IDLE_ST(a->idle_us, idle + (PGOV_IDLE_NOW() - t0));
    PGOV_IDLE_ST(a->t0, 0);
}

static uint32_t old_read(volatile struct pgov_idle *a, uint32_t *now)
{
    uint32_t t0, idle, n = PGOV_IDLE_NOW();   /* the tick's sample first */
    do
    {
        t0 = PGOV_IDLE_LD(a->t0);
        idle = PGOV_IDLE_LD(a->idle_us);
    } while (t0 != PGOV_IDLE_LD(a->t0));
    *now = n;
    return t0 ? idle + (n - t0) : idle;
}

/* ---- scenarios ------------------------------------------------------------ */
struct scen {
    const char *name;
    int legacy;                 /* the old pair */
    int asleep;                 /* starts inside a sleep: wake, then sleep */
    uint32_t base;              /* the clock at the start (wrap tests) */
};

static void init_state(const struct scen *sc, struct pgov_idle *a)
{
    a->seq = 10;
    a->idle_us = 1000;
    a->t0 = sc->asleep ? ((sc->base - 500) | 1) : 0;
}

/* Run the writer on the shadow: record its steps (times from rec_times) */
static void run_writer(const struct scen *sc)
{
    init_state(sc, (struct pgov_idle *)&shadow);
    nsteps = 0;
    rec_ntime = 0;
    recording = in_writer = 1;
    if (sc->legacy)
    {
        if (sc->asleep)
        {
            old_leave(&shadow, shadow.t0);
            (void)old_enter(&shadow);
        }
        else
            old_leave(&shadow, old_enter(&shadow));
    }
    else if (sc->asleep)
    {
        pgov_idle_leave(&shadow, shadow.seq);
        (void)pgov_idle_enter(&shadow);
    }
    else
        pgov_idle_leave(&shadow, pgov_idle_enter(&shadow));
    recording = in_writer = 0;
}

/* The writer's own account at time t (relative arithmetic, wrap-safe):
 * the base, plus every sleep interval it stamped, up to t */
static uint32_t truth(const struct scen *sc, uint32_t t)
{
    int64_t rt = (int32_t)(t - sc->base), acc = 0, start = 0;
    int open = 0;

    if (sc->asleep)
    {
        open = 1;
        start = (int32_t)(((sc->base - 500) | 1) - sc->base);
    }
    /* the writer's times, in order: legacy/new alike alternate wake/sleep
     * from the initial state */
    for (int i = 0; i < nsteps; i++)
    {
        if (steps[i].kind != S_TIME)
            continue;
        int64_t r = (int32_t)(steps[i].v - sc->base);
        if (open)
        {
            /* a wake stamp; the old writer and new one both credit
             * (stamp - t0) where t0 = start */
            if (rt > start)
                acc += (rt < r ? rt : r) - start;
            open = 0;
        }
        else
        {
            start = (int32_t)((steps[i].v | 1) - sc->base);
            open = 1;
        }
    }
    if (open && rt > start)
        acc += rt - start;
    return 1000 + (uint32_t)acc;
}

static long runs, bad, inexact;
static int first_bad_printed;

static void run_one(const struct scen *sc)
{
    uint32_t prev, prev_t, v, t;
    int ok = 1;

    /* the writer's time steps at their global positions */
    int ti = 0;
    for (int i = 0; i < nsteps; i++)
        if (steps[i].kind == S_TIME)
            rec_times[ti++] = sc->base + (uint32_t)(pos[i] + i);
    run_writer(sc);

    init_state(sc, (struct pgov_idle *)&real);
    applied = 0;
    revent = 0;
    T = sc->base;
    prev = truth(sc, sc->base);
    prev_t = sc->base;
    for (int r = 0; r <= NREADS; r++)
    {
        if (r == NREADS)
            apply_upto(1 << 30);    /* the rest, then a last read */
        v = sc->legacy ? old_read(&real, &t) : pgov_idle_read(&real, &t);
        int32_t dv = (int32_t)(v - prev), dt = (int32_t)(t - prev_t);
        if (dv < 0 || dv > dt)
            ok = 0;
        if (!sc->legacy && v != truth(sc, t))
            inexact++;
        prev = v;
        prev_t = t;
    }
    runs++;
    if (!ok)
    {
        bad++;
        if (!sc->legacy && !first_bad_printed)
        {
            first_bad_printed = 1;
            fprintf(stderr, "%s: bad schedule:", sc->name);
            for (int i = 0; i < nsteps; i++)
                fprintf(stderr, " %d", pos[i]);
            fprintf(stderr, "\n");
        }
    }
}

/* every nondecreasing assignment of the writer's steps to reader events */
static void enumerate(const struct scen *sc, int i, int lo)
{
    if (i == nsteps)
    {
        run_one(sc);
        return;
    }
    for (int p = lo; p <= LMAX; p++)
    {
        pos[i] = p;
        enumerate(sc, i + 1, p);
    }
}

int main(void)
{
    static const struct scen sc[] = {
        { "new, awake then sleep",       0, 0, 5000 },
        { "new, asleep then wake",       0, 1, 5000 },
        { "new, clock wraps",            0, 0, 0xfffffff4u },
        { "new, asleep, clock wraps",    0, 1, 0xfffffff4u },
        { "old, awake then sleep",       1, 0, 5000 },
        { "old, asleep then wake",       1, 1, 5000 },
    };
    int fails = 0;

    for (unsigned int i = 0; i < sizeof sc / sizeof sc[0]; i++)
    {
        /* the step shapes first (times are placeholders) */
        memset(rec_times, 0, sizeof rec_times);
        run_writer(&sc[i]);
        runs = bad = inexact = 0;
        first_bad_printed = 0;
        enumerate(&sc[i], 0, 0);
        printf("     %-26s %2d writer steps, %8ld schedules: %ld bad, "
               "%ld inexact\n", sc[i].name, nsteps, runs, bad, inexact);
        if (sc[i].legacy ? bad == 0 : (bad || inexact))
            fails++;
    }
    return fails ? 1 : 0;
}
