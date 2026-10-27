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
 * ProkBox (Fit-2): work for the second core (firmware/export/cop_job.h,
 * docs/fit2-brief.md section 4). The ring and the cache table are in
 * cop_job_core.c; this file adds the threads, semaphores and cache calls.
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
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "cop_job.h"

#if NUM_CORES > 1 && !defined(SIMULATOR)
#define CROSS 1
#else
#define CROSS 0
#endif

enum { CALL_IDLE = 0, CALL_PENDING, CALL_BUSY };

static void cache_ops(unsigned ops)
{
#if CROSS
    if (ops & COPJ_C_COMMIT_DISCARD_I)
        commit_discard_idcache();
    else if (ops & COPJ_C_COMMIT_DISCARD)
        commit_discard_dcache();
    else if (ops & COPJ_C_COMMIT)
        commit_dcache();
#else
    (void)ops;
#endif
}

static uint32_t now_us(void)
{
#if CROSS
    return USEC_TIMER;
#else
    return (uint32_t)current_tick * (1000000 / HZ);
#endif
}

bool cop_jobs_cross_core(void)
{
    return CROSS;
}

bool cop_job_in_worker(const struct cop_worker *w)
{
    return w->started && w->cross && thread_self() == w->thread_id;
}

/* Serves a pending call on the poster's thread */
static void serve_call(struct cop_worker *w)
{
    if (w->call_state != CALL_PENDING)
        return;
    w->call_state = CALL_BUSY;
    w->call_ret = w->call_fn(w->call_io);
    w->call_state = CALL_IDLE;
    semaphore_release(&w->reply);
}

static void run_slot(struct cop_worker *w, int s)
{
    struct cop_job_slot *j = &w->slot[s];

    cache_ops(copj_cache_ops(j->flags, COPJ_START, w->cross));
    j->fn(j->arg);
    cache_ops(copj_cache_ops(j->flags, COPJ_FINISH, w->cross));
    w->jobs++;
    copj_finish(&w->ring, s);
}

#if CROSS
static struct cop_worker * volatile starting SHAREDBSS_ATTR;
static struct mutex start_mtx SHAREDBSS_ATTR;
static bool start_mtx_ready SHAREDBSS_ATTR;

static void worker_thread(void)
{
    struct cop_worker *w = starting;

    semaphore_release(&w->poster);      /* the starter may go on */
    while (1)
    {
        int s;

        semaphore_wait(&w->work, TIMEOUT_BLOCK);
        while ((s = copj_take(&w->ring)) >= 0)
        {
            run_slot(w, s);
            semaphore_release(&w->poster);
        }
        if (w->stop)
            break;
    }
    /* Returning ends the thread; the kernel writes back this core's
     * cache before the slot is freed (thread-pp.c) */
}
#endif /* CROSS */

bool cop_worker_start(struct cop_worker *w, void *stack, size_t stack_size,
                      const char *name, int priority)
{
    memset(w, 0, sizeof(*w));
    copj_ring_init(&w->ring);
    semaphore_init(&w->work, COPJ_SLOTS + 1, 0);
    semaphore_init(&w->poster, 2 * COPJ_SLOTS + 2, 0);
    semaphore_init(&w->reply, 1, 0);
#if CROSS
    if (!start_mtx_ready)
    {
        mutex_init(&start_mtx);         /* first call is on the CPU, once */
        start_mtx_ready = true;
    }
    mutex_lock(&start_mtx);
    starting = w;
    w->cross = 1;
    w->thread_id = create_thread(worker_thread, stack, stack_size, 0, name
                                 IF_PRIO(, priority) IF_COP(, COP));
    if (w->thread_id == 0)
    {
        mutex_unlock(&start_mtx);
        return false;
    }
    /* Started only once the COP runs it: with an old bootloader the COP
     * never starts (cop_main), so give up rather than hang */
    if (semaphore_wait(&w->poster, HZ) != OBJ_WAIT_SUCCEEDED)
    {
        mutex_unlock(&start_mtx);
        return false;   /* the thread stays queued on a dead core */
    }
    mutex_unlock(&start_mtx);
#else
    (void)stack; (void)stack_size; (void)name; (void)priority;
#endif
    w->started = 1;
    return true;
}

void cop_worker_stop(struct cop_worker *w)
{
    if (!w->started)
        return;
#if CROSS
    w->stop = 1;
    semaphore_release(&w->work);
    thread_wait(w->thread_id);
#endif
    w->started = 0;
}

int cop_job_post(struct cop_worker *w, cop_job_fn fn, void *arg,
                 unsigned flags)
{
    int s;

    if (!w->started)
        return -1;
    while ((s = copj_claim(&w->ring)) < 0)
    {
        /* Full: wait for the oldest job, serving calls meanwhile */
        semaphore_wait(&w->poster, HZ / 10);
        serve_call(w);
    }
    w->slot[s].fn = fn;
    w->slot[s].arg = arg;
    w->slot[s].flags = (uint8_t)flags;
    cache_ops(copj_cache_ops(flags, COPJ_POST, w->cross));
    copj_commit(&w->ring, s);
    if (w->cross)
        semaphore_release(&w->work);
    else
        run_slot(w, copj_take(&w->ring));   /* inline, in order */
    return s;
}

void cop_job_wait(struct cop_worker *w, int ticket)
{
    if (ticket < 0 || ticket >= COPJ_SLOTS)
        return;
    while (!copj_is_done(&w->ring, ticket))
    {
        semaphore_wait(&w->poster, TIMEOUT_BLOCK);
        serve_call(w);
    }
    cache_ops(copj_cache_ops(w->slot[ticket].flags, COPJ_COLLECT, w->cross));
    copj_release(&w->ring, ticket);
}

intptr_t cop_job_call(struct cop_worker *w, cop_call_fn fn, intptr_t *io,
                      unsigned flags)
{
    intptr_t ret;
    uint32_t t0;
    int i;

    if (!cop_job_in_worker(w))
        return fn(io);

    cache_ops(copj_cache_ops(flags, COPJ_CALL, true));
    for (i = 0; i < COP_CALL_IO; i++)
        w->call_io[i] = io[i];
    w->call_fn = fn;
    t0 = now_us();
    w->call_state = CALL_PENDING;       /* last store: the call is ready */
    semaphore_release(&w->poster);
    semaphore_wait(&w->reply, TIMEOUT_BLOCK);
    w->call_wait_us += now_us() - t0;
    w->calls++;
    for (i = 0; i < COP_CALL_IO; i++)
        io[i] = w->call_io[i];
    ret = w->call_ret;
    cache_ops(copj_cache_ops(flags, COPJ_RETURN, true));
    return ret;
}
