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
 * The per-core idle accounting the band governor reads (pgov.c), written
 * by core_sleep() (thread-pp.c).  A sequence counter, odd while the
 * writer updates, makes the three words one snapshot for the reader on
 * the other core: the old reader re-checked only the sleep stamp, so a
 * read between "idle += slept" and "stamp = 0" counted the same sleep
 * twice (docs/design/pp-governor.md section 17.6).  Host-tested
 * one store at a time against every reader interleaving in
 * tests/ppgov/test_idle.c.
 */
#ifndef PGOV_IDLE_H
#define PGOV_IDLE_H

#include <stdint.h>

/* One per core, in uncached memory, one writer each (its own core,
 * IRQs off).  Uncached stores leave through the write buffer in program
 * order, as corelock assumes; volatile keeps the compiler's order, so no
 * barrier is needed on the PP5022 */
struct pgov_idle {
    uint32_t seq;       /* odd while the writer updates */
    uint32_t idle_us;   /* the time asleep, finished sleeps */
    uint32_t t0;        /* the sleep's start | 1, nonzero while asleep */
};

/* The host test replaces these with stepping hooks */
#ifndef PGOV_IDLE_NOW
#include "cpu.h"                    /* USEC_TIMER */
#define PGOV_IDLE_NOW()         USEC_TIMER
#endif
#ifndef PGOV_IDLE_LD
#define PGOV_IDLE_LD(lv)        (lv)
#endif
#ifndef PGOV_IDLE_ST
#define PGOV_IDLE_ST(lv, v)     ((lv) = (v))
#endif

/* The writer.  It never re-reads a word it stored in the same update
 * (the sequence is carried in a local), so the test can record its
 * stores and replay them one at a time.  The wake's stamp is taken after
 * the sequence goes odd: a reader whose snapshot showed the sleep still
 * running sampled its time before that, so the sleep's end is never
 * earlier than the time it was credited up to (monotonic totals). */
static inline uint32_t pgov_idle_enter(volatile struct pgov_idle *a)
{
    uint32_t s = PGOV_IDLE_LD(a->seq) + 1, t0;

    PGOV_IDLE_ST(a->seq, s);
    t0 = PGOV_IDLE_NOW() | 1;
    PGOV_IDLE_ST(a->t0, t0);
    PGOV_IDLE_ST(a->seq, s + 1);
    return s + 1;
}

static inline void pgov_idle_leave(volatile struct pgov_idle *a,
                                   uint32_t seq)
{
    uint32_t t0 = PGOV_IDLE_LD(a->t0), idle = PGOV_IDLE_LD(a->idle_us);

    PGOV_IDLE_ST(a->seq, seq + 1);
    idle += PGOV_IDLE_NOW() - t0;
    PGOV_IDLE_ST(a->idle_us, idle);
    PGOV_IDLE_ST(a->t0, 0);
    PGOV_IDLE_ST(a->seq, seq + 2);
}

/* The reader: the core's idle time so far, the sleep in progress
 * included, and the time it was read at (*now).  The time is sampled
 * inside the snapshot, so between two reads the total never falls and
 * never grows by more than the time between them.  It spins only while
 * the other core is inside an update: a few instructions with IRQs off
 * (an FIQ on that core can stretch it to the FIQ's length). */
static inline uint32_t pgov_idle_read(volatile struct pgov_idle *a,
                                      uint32_t *now)
{
    uint32_t s, t0 = 0, idle = 0, t = 0;

    do
    {
        s = PGOV_IDLE_LD(a->seq);
        if (s & 1)
            continue;               /* mid-update: the loop re-checks */
        t0 = PGOV_IDLE_LD(a->t0);
        idle = PGOV_IDLE_LD(a->idle_us);
        t = PGOV_IDLE_NOW();
    } while ((s & 1) || PGOV_IDLE_LD(a->seq) != s);
    *now = t;
    return t0 ? idle + (t - t0) : idle;
}

#endif /* PGOV_IDLE_H */
