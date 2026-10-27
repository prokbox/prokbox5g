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
 * ProkBox (Fit-2): the second core's job channel, pure logic
 * (firmware/export/cop_job_core.h). No Rockbox calls: tests/copjob/ builds
 * it on the host.
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
#include "cop_job_core.h"

void copj_ring_init(struct copj_ring *r)
{
    int i;

    for (i = 0; i < COPJ_SLOTS; i++)
        r->state[i] = COPJ_FREE;
    r->head = 0;
    r->tail = 0;
}

int copj_claim(struct copj_ring *r)
{
    int slot = r->head;

    if (r->state[slot] != COPJ_FREE)
        return -1;
    r->state[slot] = COPJ_CLAIMED;
    return slot;
}

void copj_commit(struct copj_ring *r, int slot)
{
    if (slot != r->head || r->state[slot] != COPJ_CLAIMED)
        return;                     /* not the claimed slot: ignore */
    /* The payload is written before this store (uncached, in order) */
    r->state[slot] = COPJ_QUEUED;
    r->head = (uint8_t)((slot + 1) % COPJ_SLOTS);
}

int copj_take(struct copj_ring *r)
{
    int slot = r->tail;

    if (r->state[slot] != COPJ_QUEUED)
        return -1;
    r->state[slot] = COPJ_RUNNING;
    r->tail = (uint8_t)((slot + 1) % COPJ_SLOTS);
    return slot;
}

void copj_finish(struct copj_ring *r, int slot)
{
    if (slot >= 0 && slot < COPJ_SLOTS && r->state[slot] == COPJ_RUNNING)
        r->state[slot] = COPJ_DONE;
}

bool copj_is_done(const struct copj_ring *r, int slot)
{
    return slot >= 0 && slot < COPJ_SLOTS && r->state[slot] == COPJ_DONE;
}

bool copj_release(struct copj_ring *r, int slot)
{
    if (!copj_is_done(r, slot))
        return false;
    r->state[slot] = COPJ_FREE;
    return true;
}

int copj_pending(const struct copj_ring *r)
{
    int i, n = 0;

    for (i = 0; i < COPJ_SLOTS; i++)
        if (r->state[i] >= COPJ_QUEUED)
            n++;
    return n;
}

unsigned copj_cache_ops(unsigned flags, enum copj_phase phase,
                        bool cross_core)
{
    if (!cross_core)
        return 0;

    switch (phase)
    {
    case COPJ_POST:
        /* What the poster wrote must be in memory before the worker
         * reads it, code included */
        return (flags & (COPJ_IN | COPJ_CODE)) ? COPJ_C_COMMIT : 0;
    case COPJ_START:
        /* The worker may hold stale lines of it from an earlier job.
         * Write back first: a plain discard would lose the worker's own
         * dirty lines (its stack) */
        if (flags & COPJ_CODE)
            return COPJ_C_COMMIT_DISCARD_I;
        return (flags & COPJ_IN) ? COPJ_C_COMMIT_DISCARD : 0;
    case COPJ_FINISH:
        return (flags & COPJ_OUT) ? COPJ_C_COMMIT : 0;
    case COPJ_COLLECT:
        return (flags & COPJ_OUT) ? COPJ_C_COMMIT_DISCARD : 0;
    case COPJ_CALL:
        /* The handler reads the worker's memory through the uncached
         * alias, and writes through it: either way the worker's dirty
         * lines go to memory first, or a later eviction would overwrite
         * the handler's writes */
        return COPJ_C_COMMIT;
    case COPJ_RETURN:
        return (flags & COPJ_CALL_WROTE) ? COPJ_C_COMMIT_DISCARD : 0;
    }
    return 0;
}
