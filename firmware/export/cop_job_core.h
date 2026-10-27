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
 * ProkBox (Fit-2, docs/fit2-brief.md section 4): the second core's job
 * channel, pure logic with no Rockbox calls. firmware/common/cop_job.c
 * drives it; tests/copjob/ checks it on the host.
 *
 * Two things live here:
 *  - the slot ring: a poster on the CPU claims a slot, fills it and
 *    commits it; the worker on the COP takes it, runs it and finishes it;
 *    the poster collects and releases it. One poster thread and one worker
 *    thread per ring, so each index has one writer.
 *  - the hand-off rules: which cache operation each side does at each
 *    point, from what the job touches (docs/pp5022-reference/
 *    cores-and-threads.md section 4: no coherency between the cores'
 *    caches, maintenance is whole-cache).
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
#ifndef _COP_JOB_CORE_H_
#define _COP_JOB_CORE_H_

#include <stdbool.h>
#include <stdint.h>

#define COPJ_SLOTS  4           /* jobs in flight per worker */

enum copj_state {
    COPJ_FREE = 0,              /* the poster may claim it */
    COPJ_CLAIMED,               /* the poster is filling it */
    COPJ_QUEUED,                /* the worker may take it */
    COPJ_RUNNING,               /* the worker runs it */
    COPJ_DONE,                  /* the poster may collect it */
};

/* Lives in memory both cores see uncached (cop_job.c puts it in the
 * worker, which is SHAREDBSS). Byte stores are the only atomic operation
 * relied on (as Rockbox's corelock does). */
struct copj_ring {
    volatile uint8_t state[COPJ_SLOTS];
    volatile uint8_t head;      /* next slot to claim: the poster's */
    volatile uint8_t tail;      /* next slot to take: the worker's */
};

void copj_ring_init(struct copj_ring *r);
/* poster: the next slot in order if it is free, else -1 (ring full, or the
 * oldest job not yet released) */
int  copj_claim(struct copj_ring *r);
/* poster: the claimed slot is filled; the worker may take it */
void copj_commit(struct copj_ring *r, int slot);
/* worker: the oldest queued slot (now RUNNING), else -1 */
int  copj_take(struct copj_ring *r);
/* worker: the job ran */
void copj_finish(struct copj_ring *r, int slot);
/* poster: the job ran (DONE) */
bool copj_is_done(const struct copj_ring *r, int slot);
/* poster: back to FREE; false if it was not DONE (nothing changes) */
bool copj_release(struct copj_ring *r, int slot);
/* jobs committed and not yet released */
int  copj_pending(const struct copj_ring *r);

/* What a job touches (cop_job_post() flags) */
#define COPJ_IN         0x01    /* reads cached memory the poster wrote */
#define COPJ_OUT        0x02    /* writes cached memory the poster reads */
#define COPJ_CODE       0x04    /* runs code the poster loaded */
/* What a call back to the poster did (cop_job_call() flags) */
#define COPJ_CALL_WROTE 0x08    /* the handler wrote the worker's cached
                                 * memory (through cop_uncached()) */

/* The points where data changes hands */
enum copj_phase {
    COPJ_POST,                  /* poster, before the worker may take it */
    COPJ_START,                 /* worker, before running it */
    COPJ_FINISH,                /* worker, after running it */
    COPJ_COLLECT,               /* poster, after it is done */
    COPJ_CALL,                  /* worker, before a call to the poster */
    COPJ_RETURN,                /* worker, after the call returned */
};

/* Cache operations (a mask) */
#define COPJ_C_COMMIT           0x1 /* write back this core's dirty lines */
#define COPJ_C_COMMIT_DISCARD   0x2 /* ... and drop every line (data) */
#define COPJ_C_COMMIT_DISCARD_I 0x4 /* ... and drop every line (code too) */

/* What the core at that phase must do. cross_core false (the job runs on
 * the poster's core: the simulator, or no COP): nothing. */
unsigned copj_cache_ops(unsigned flags, enum copj_phase phase,
                        bool cross_core);

#endif /* _COP_JOB_CORE_H_ */
