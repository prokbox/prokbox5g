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
 * ProkBox (Fit-1, C2): the scheduler's aging rule with a time bound.
 * Pure, so tests/sched/ checks it (and a model of switch_thread) on the
 * host. Used by firmware/kernel/thread.c under HAVE_SCHED_BANDS.
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
#ifndef SCHED_AGE_H
#define SCHED_AGE_H

#include <stdbool.h>

/* Needs PRIORITY_REALTIME and PRIORITY_USER_INTERFACE (thread.h) */
#if !defined(PRIORITY_REALTIME) || !defined(PRIORITY_USER_INTERFACE)
#error "include thread.h (or define the priorities) first"
#endif

/* A runnable thread in the interactive band (UI, video feed, buffering,
   audio: priority <= PRIORITY_USER_INTERFACE) waits at most this many
   ticks for a slice, whatever the gap to the highest runnable thread:
   20 ms at HZ 100 (docs/fit1-brief.md section B) */
#define SCHED_AGE_TICKS     2

/* The bound for a thread at this priority: background threads (system 18,
   database 20) wait 2 more ticks per level below the UI, so a refilling
   codec keeps most of the core (18: 60 ms, 20: 100 ms) */
static inline long sched_age_limit(int priority)
{
    if (priority <= PRIORITY_USER_INTERFACE)
        return SCHED_AGE_TICKS;
    return (long)SCHED_AGE_TICKS * (priority - PRIORITY_USER_INTERFACE + 1);
}

/* May a runnable thread at `priority` have the core now? `max` is the
   highest runnable priority, `skips` the times it has been passed over
   (already counting this pass), `waited` the ticks since it last ran or
   became runnable. Realtime threads are never aged past; upstream's count
   rule (skips > gap squared) stays as the fast path for higher threads
   that yield often (polled I/O); the time bound is the guarantee. */
static inline bool sched_may_run(int priority, int max, int skips,
                                 long waited)
{
    int diff;

    if (priority <= max)
        return true;
    if (priority <= PRIORITY_REALTIME)
        return false;
    diff = priority - max;
    return skips > diff * diff || waited >= sched_age_limit(priority);
}

#endif /* SCHED_AGE_H */
