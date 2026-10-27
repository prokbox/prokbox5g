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
 * ProkBox (Fit-2, docs/fit2-brief.md section 4): work for the second core.
 *
 * A client (the codec first; album art, scans and the video's audio
 * fallback later) owns one worker: a thread on the COP, at the client's
 * priority band, on a stack from the client's own memory. The client's
 * thread posts jobs and waits for them. While it waits it also serves the
 * worker's calls back to it, so a job can use anything that must stay on
 * the CPU (buffering, pcmbuf, queues, peripherals) through cop_job_call().
 *
 * Rules (the brief's section 3; cop_job_core.h has the cache table):
 *  - The worker struct lives in uncached memory: declare it COP_SHARED_ATTR.
 *  - A job touches no peripheral and takes no interrupt. Peripheral work
 *    goes through cop_job_call().
 *  - Data changes hands only at post, done, call and return. Flags say
 *    what crosses (COPJ_IN, COPJ_OUT, COPJ_CODE, COPJ_CALL_WROTE) and the
 *    channel does the cache operations; nothing else is shared without
 *    one of these points.
 *  - A call handler reaches the worker's cached memory only through
 *    cop_uncached() (and says COPJ_CALL_WROTE if it wrote it); small
 *    values go through io[], which is uncached already.
 *  - One poster thread per worker; a job that calls back needs that
 *    thread in cop_job_wait().
 *  - Without a second core (the simulator) the same calls run each job
 *    on the poster's thread, call handlers directly, and do no cache
 *    operation: the client code is the same.
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
#ifndef _COP_JOB_H_
#define _COP_JOB_H_

#include "config.h"

#ifdef HAVE_COP_JOBS
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "system.h"
#include "kernel.h"
#include "cop_job_core.h"

#define COP_SHARED_ATTR SHAREDBSS_ATTR
#define COP_CALL_IO     4       /* values in and out of a call */

typedef void (*cop_job_fn)(void *arg);
typedef intptr_t (*cop_call_fn)(intptr_t *io);

struct cop_job_slot {
    cop_job_fn fn;
    void *arg;
    uint8_t flags;
};

struct cop_worker {
    struct copj_ring ring;
    struct cop_job_slot slot[COPJ_SLOTS];
    struct semaphore work;      /* the worker waits: a job, or stop */
    struct semaphore poster;    /* the poster waits: a job done, a call */
    struct semaphore reply;     /* the worker waits: its call returned */
    cop_call_fn call_fn;        /* the one call in flight */
    intptr_t call_io[COP_CALL_IO];
    intptr_t call_ret;
    volatile uint8_t call_state;
    volatile uint8_t stop;
    uint8_t cross;              /* the worker runs on the other core */
    uint8_t started;
    unsigned int thread_id;     /* the worker (0 without a second core) */
    /* counters, for the developer screen and the device A/B */
    uint32_t jobs, calls, call_wait_us;
};

/* Starts the worker on the COP (the simulator: marks it ready, no thread).
 * stack is the client's (cached memory is fine: only the worker uses it).
 * False: no second core running (an old bootloader leaves the COP
 * parked) or no thread slot; the client then runs its work itself. */
bool cop_worker_start(struct cop_worker *w, void *stack, size_t stack_size,
                      const char *name, int priority);
/* Waits for queued jobs, stops the worker and waits for its thread */
void cop_worker_stop(struct cop_worker *w);
/* A ticket for cop_job_wait(); blocks while the ring is full. -1: the
 * worker is not started (the caller runs fn itself) */
int  cop_job_post(struct cop_worker *w, cop_job_fn fn, void *arg,
                  unsigned flags);
/* Waits for that job, serving the worker's calls meanwhile */
void cop_job_wait(struct cop_worker *w, int ticket);
/* From a job: runs fn(io) on the poster's thread and returns its value;
 * io[] (COP_CALL_IO values) is copied in and back. Elsewhere: fn(io). */
intptr_t cop_job_call(struct cop_worker *w, cop_call_fn fn, intptr_t *io,
                      unsigned flags);
/* True on the worker's own thread */
bool cop_job_in_worker(const struct cop_worker *w);
/* The second core runs workers (false in the simulator) */
bool cop_jobs_cross_core(void);

/* The uncached alias of a pointer into cached SDRAM; anything else
 * (NULL, IRAM, already uncached) as is */
static inline void *cop_uncached(const void *p)
{
#if NUM_CORES > 1 && defined(UNCACHED_BASE_ADDR) && !defined(SIMULATOR)
    if (p != NULL && (uintptr_t)p < UNCACHED_BASE_ADDR)
        return (void *)UNCACHED_ADDR((uintptr_t)p);
#endif
    return (void *)p;
}

#endif /* HAVE_COP_JOBS */
#endif /* _COP_JOB_H_ */
