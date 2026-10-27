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
 * Sim script commands "floor", "floor_watch", "floor_reserve" and
 * "floor_release": the audio-buffer floor gate (Phase 16-A, #105); see
 * sim_cmd_floor.h. Test-only.
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "config.h"
#include "core_alloc.h"
#include "kernel.h"
#include "thread.h"
#include "sim_cmd_floor.h"

extern size_t audio_buffer_size(void);
extern size_t audio_get_filebuflen(void);

/* docs/hardware.md budget rule (owner, 2026-10-20): the playing/paused
 * audio file buffer, measured at 50k tracks. */
#if MEMORYSIZE <= 32
#define FLOOR_BYTES 7500000UL
#else
#define FLOOR_BYTES 39000000UL
#endif

/* slots 0-3: the scripts' reservations; slot 4: the release line's
 * resident BCM image, held for the whole run (floor_gate.sh puts it
 * first, before playback, as the device pins it at boot) */
#define RESERVE_SLOTS 5
static int reserve_h[RESERVE_SLOTS];
static size_t reserve_n[RESERVE_SLOTS];

/* The watch: a kernel thread samples the buffer every tick (10 ms) and keeps the
 * minimum, so a transient dip (a commit's peak, a panel block, the
 * chapter table) is seen even when the script cannot be at that line at
 * that moment. Zero (no audio session yet) is counted apart. */
/* a kernel thread, not a host pthread: buflib walked from a host thread
 * while a Rockbox thread moves blocks reads a torn heap ("block len
 * wacky" panics, seen in 16-1's paused-window runs) */
static unsigned int watch_thread;
static long watch_stack[DEFAULT_STACK_SIZE / sizeof(long)];
static volatile bool watch_run;
static volatile size_t watch_min;
static volatile long watch_samples, watch_zero;
static volatile size_t watch_avail_min;
static bool watch_timeline;
#define TIMELINE_STEP 262144UL      /* a "floor_tl" line per 256 KB move */

static void watch_main(void)
{
    size_t last_avail = core_available();
    struct timespec t0;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (watch_run)
    {
        size_t n = audio_buffer_size();
        size_t av = core_available();

        if (av < watch_avail_min)
            watch_avail_min = av;
        if (watch_timeline && (av + TIMELINE_STEP <= last_avail
                               || av >= last_avail + TIMELINE_STEP))
        {
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            fprintf(stderr, "sim_script: floor_tl ms=%ld free=%lu audio=%lu\n",
                    (long)((t.tv_sec - t0.tv_sec) * 1000
                           + (t.tv_nsec - t0.tv_nsec) / 1000000),
                    (unsigned long)av, (unsigned long)n);
            last_avail = av;
        }
        watch_samples++;
        if (n == 0)
            watch_zero++;
        else if (n < watch_min)
            watch_min = n;
        sleep(1);                   /* a tick: cooperative */
    }
}

static bool verdict(const char *what, const char *label, size_t n,
                    int lineno)
{
    if (n >= FLOOR_BYTES)
    {
        fprintf(stderr, "sim_script: expect_floor ok %s %s ram_mb=%d "
                "audio=%lu floor=%lu margin=%lu\n", what, label,
                MEMORYSIZE, (unsigned long)n, FLOOR_BYTES,
                (unsigned long)n - FLOOR_BYTES);
        return true;
    }
    fprintf(stderr, "sim_script:%d: expect_floor failed: %s %s: audio file "
            "buffer %lu B < floor %lu B (%d MB unit, %lu B short; "
            "docs/hardware.md budget rules)\n", lineno, what, label,
            (unsigned long)n, FLOOR_BYTES, MEMORYSIZE,
            FLOOR_BYTES - (unsigned long)n);
    return false;
}

bool sim_cmd_floor(const char *cmd, char *argv[], int argc, int lineno)
{
    if (!strcmp(cmd, "floor") && argc >= 2)
    {
        /* floor <label>: the buffer now, checked against the floor */
        size_t n = audio_buffer_size();
        fprintf(stderr, "sim_script: floor %s ram_mb=%d audio=%lu "
                "filebuflen=%lu\n", argv[1], MEMORYSIZE, (unsigned long)n,
                (unsigned long)audio_get_filebuflen());
        if (n == 0)
            fprintf(stderr, "sim_script:%d: expect_floor failed: %s: no "
                    "audio session (start playback before measuring)\n",
                    lineno, argv[1]);
        else
            verdict("at", argv[1], n, lineno);
        return true;
    }
    if (!strcmp(cmd, "floor_watch") && argc >= 2)
    {
        if (!strcmp(argv[1], "begin") && !watch_run)
        {
            watch_min = (size_t)-1;
            watch_avail_min = (size_t)-1;
            watch_timeline = argc >= 3 && !strcmp(argv[2], "timeline");
            watch_samples = watch_zero = 0;
            watch_run = true;
            watch_thread = create_thread(watch_main, watch_stack,
                                         sizeof(watch_stack), 0, "floorwatch"
                                         IF_PRIO(, PRIORITY_BACKGROUND)
                                         IF_COP(, CPU));
            fprintf(stderr, "sim_script: floor_watch begin ram_mb=%d "
                    "floor=%lu\n", MEMORYSIZE, FLOOR_BYTES);
        }
        else if (!strcmp(argv[1], "end") && watch_run && argc >= 3)
        {
            watch_run = false;
            thread_wait(watch_thread);
            fprintf(stderr, "sim_script: floor_watch %s ram_mb=%d "
                    "min=%lu free_min=%lu samples=%ld no_session=%ld\n",
                    argv[2], MEMORYSIZE, watch_min == (size_t)-1 ? 0UL :
                    (unsigned long)watch_min, (unsigned long)watch_avail_min,
                    watch_samples, watch_zero);
            if (argc >= 4 && !strcmp(argv[3], "free"))
                ;               /* the free-heap minimum only (no audio) */
            else if (watch_min == (size_t)-1)
                fprintf(stderr, "sim_script:%d: expect_floor failed: "
                        "watch %s saw no audio session\n", lineno, argv[2]);
            else
                verdict("min over", argv[2], watch_min, lineno);
        }
        else
            fprintf(stderr, "sim_script:%d: floor_watch begin | end "
                    "<label>\n", lineno);
        return true;
    }
    if (!strcmp(cmd, "floor_reserve") && argc >= 3)
    {
        /* floor_reserve <slot 0-4> <bytes>: a real core_alloc, as the
         * shell's own reservation would be (it shrinks the audio buffer
         * the way playback.c's callback does); models the panel transient
         * (785,844), the module arena (65,536 placeholder), chap_bytes
         * (77,832) and the #92 slots where no script path reaches them */
        int s = atoi(argv[1]);
        size_t n = (size_t)strtoul(argv[2], NULL, 0);

        if (s < 0 || s >= RESERVE_SLOTS || reserve_h[s] > 0 || !n)
        {
            fprintf(stderr, "sim_script:%d: floor_reserve <0-4> <bytes> "
                    "(slot free)\n", lineno);
            return true;
        }
        reserve_h[s] = core_alloc(n);
        reserve_n[s] = n;
        fprintf(stderr, "sim_script: floor_reserve %d %lu %s\n", s,
                (unsigned long)n, reserve_h[s] > 0 ? "ok" : "refused");
        return true;
    }
    if (!strcmp(cmd, "floor_release") && argc >= 2)
    {
        int s = atoi(argv[1]);

        if (s >= 0 && s < RESERVE_SLOTS && reserve_h[s] > 0)
        {
            reserve_h[s] = core_free(reserve_h[s]);
            fprintf(stderr, "sim_script: floor_release %d %lu\n", s,
                    (unsigned long)reserve_n[s]);
        }
        return true;
    }
    return false;
}
