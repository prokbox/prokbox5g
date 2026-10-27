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
 ****************************************************************************/

/* Host-jitter watcher (docs/gate-speed-brief.md section 4, option B).
 *
 * The simulator's ticks follow the wall clock, so a stalled host (other
 * simulators, a VM, Spotlight) stretches every interval the trace rules
 * measure. With SIM_LOAD_WATCH=1 (scripts/shots.sh sets it) a host thread
 * sleeps 10 ms in a loop and records how late it wakes. The log gets
 *
 *   sim_load: run jitter mean=<ms> p95=<ms> p99=<ms> max=<ms> n=<samples>
 *
 * every 2 s and at exit (the last one counts), and a
 * "sim_load: window max=<ms>" line for each second in which the host was
 * late by 30 ms or more. scripts/check_trace.py reads the run line to tell a
 * frame-interval failure caused by the host from one caused by the shell.
 * Nothing here touches the simulated system: it reads no tick, sends no
 * event, and costs one wake-up per 10 ms on a core that is idle anyway. */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PERIOD_NS   10000000L
#define BUCKETS     201             /* 0..199 ms late, the last: 200+ */
#define WINDOW_MS   1000
#define REPORT_MS   2000
#define WINDOW_WARN 30

static unsigned long hist[BUCKETS];
static unsigned long n_samples;
static unsigned long sum_ms;
static unsigned long max_ms;

static long now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static unsigned long percentile(unsigned pct)
{
    unsigned long want = (n_samples * pct + 99) / 100, cum = 0;
    unsigned i;

    for (i = 0; i < BUCKETS; i++)
    {
        cum += hist[i];
        if (cum >= want)
            return i;
    }
    return BUCKETS - 1;
}

static void report_run(void)
{
    if (!n_samples)
        return;
    fprintf(stderr, "sim_load: run jitter mean=%lu p95=%lu p99=%lu max=%lu n=%lu\n",
            sum_ms / n_samples, percentile(95), percentile(99), max_ms,
            n_samples);
    fflush(stderr);
}

static void *watcher(void *arg)
{
    long window_start = now_ns(), last_report = window_start;
    unsigned long window_max = 0;

    (void)arg;
    for (;;)
    {
        long t0 = now_ns();
        struct timespec req = { 0, PERIOD_NS };
        long late;
        unsigned long ms;

        nanosleep(&req, NULL);
        late = now_ns() - t0 - PERIOD_NS;
        ms = late > 0 ? (unsigned long)(late / 1000000L) : 0;
        hist[ms < BUCKETS ? ms : BUCKETS - 1]++;
        n_samples++;
        sum_ms += ms;
        if (ms > max_ms)
            max_ms = ms;
        if (ms > window_max)
            window_max = ms;
        if (now_ns() - window_start >= WINDOW_MS * 1000000L)
        {
            if (window_max >= WINDOW_WARN)
                fprintf(stderr, "sim_load: window max=%lu\n", window_max);
            window_max = 0;
            window_start = now_ns();
        }
        if (now_ns() - last_report >= REPORT_MS * 1000000L)
        {
            report_run();
            last_report = now_ns();
        }
    }
    return NULL;
}

/* called from sim_script_init() (a bare constructor in this file would not
 * be linked: nothing else references it in the static library) */
void sim_load_start(void)
{
    const char *e = getenv("SIM_LOAD_WATCH");
    pthread_t th;

    if (!e || strcmp(e, "1"))
        return;
    if (pthread_create(&th, NULL, watcher, NULL) == 0)
    {
        pthread_detach(th);
        atexit(report_run);
    }
}
