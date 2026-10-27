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
 *  fe_be_core: the front end / back end boundary's pure logic (P19 S1;
 * fe_be_core.h).  No Rockbox calls: tests/febe/ builds it on the host.
 *
 * Copyright (C) 2026 prokopto-dev
 * Copyright (C) 2026 prokopto-dev
 *
 * Portions derived from Rockbox (rockbox.org).
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
#include "fe_be_core.h"
#include "cop_job_core.h"

/* ---- the seqlock ------------------------------------------------------ */

void febe_seq_write_begin(volatile uint32_t *seq)
{
    FEBE_ST(*seq, FEBE_LD(*seq) | 1u);     /* odd: a write in progress */
    FEBE_BARRIER();
}

void febe_seq_write_end(volatile uint32_t *seq)
{
    FEBE_BARRIER();
    FEBE_ST(*seq, (FEBE_LD(*seq) | 1u) + 1u);  /* even, and changed */
}

int febe_seq_copy(const volatile uint32_t *seq, const volatile uint32_t *src,
                  uint32_t *dst, unsigned nwords, unsigned max_tries)
{
    unsigned t, i;

    for (t = 1; t <= max_tries; t++)
    {
        uint32_t a = FEBE_LD(*seq), b;

        FEBE_BARRIER();
        if (a & 1u)
            continue;                   /* a write in progress */
        for (i = 0; i < nwords; i++)
            dst[i] = FEBE_LD(src[i]);
        FEBE_BARRIER();
        b = FEBE_LD(*seq);
        if (a == b)
            return (int)t;
    }
    return -1;
}

/* ---- the canary checksum --------------------------------------------- */

uint32_t febe_sum(const uint32_t *w, unsigned nwords)
{
    uint32_t h = 2166136261u;
    unsigned i;

    for (i = 0; i < nwords; i++)
    {
        uint32_t v = w[i];
        int k;
        for (k = 0; k < 4; k++)
        {
            h ^= v & 0xffu;
            h *= 16777619u;
            v >>= 8;
        }
    }
    return h ? h : 1u;
}

/* ---- the be_call slot -------------------------------------------------- */

bool febe_slot_fill(struct febe_slot *s, uint32_t seq, uint32_t kind,
                    uint32_t flags, const uint32_t *arg, unsigned nargs)
{
    uint32_t local[FEBE_ARG_WORDS];
    unsigned i;

    if (FEBE_LD(s->state) != FEBE_SLOT_FREE || nargs > FEBE_ARG_WORDS)
        return false;
    for (i = 0; i < FEBE_ARG_WORDS; i++)
    {
        local[i] = i < nargs ? arg[i] : 0;
        FEBE_ST(s->arg[i], local[i]);
    }
    FEBE_ST(s->seq, seq);
    FEBE_ST(s->kind, kind);
    FEBE_ST(s->flags, flags);
    FEBE_ST(s->ret, 0);
    FEBE_ST(s->sum, febe_sum(local, FEBE_ARG_WORDS) ^ seq ^ kind);
    FEBE_BARRIER();
    FEBE_ST(s->state, FEBE_SLOT_FILLED);    /* last: the record is whole */
    return true;
}

bool febe_slot_take(struct febe_slot *s, bool *bad)
{
    uint32_t local[FEBE_ARG_WORDS];
    unsigned i;

    *bad = false;
    if (FEBE_LD(s->state) != FEBE_SLOT_FILLED)
        return false;
    FEBE_BARRIER();
    for (i = 0; i < FEBE_ARG_WORDS; i++)
        local[i] = FEBE_LD(s->arg[i]);
    if ((febe_sum(local, FEBE_ARG_WORDS) ^ FEBE_LD(s->seq) ^
         FEBE_LD(s->kind)) != FEBE_LD(s->sum))
        *bad = true;
    FEBE_ST(s->state, FEBE_SLOT_TAKEN);
    return true;
}

void febe_slot_finish(struct febe_slot *s, uint32_t ret)
{
    if (FEBE_LD(s->state) != FEBE_SLOT_TAKEN)
        return;
    FEBE_ST(s->ret, ret);
    FEBE_BARRIER();
    FEBE_ST(s->state, FEBE_SLOT_DONE);      /* last: the results are whole */
}

bool febe_slot_done(const struct febe_slot *s)
{
    return FEBE_LD(s->state) == FEBE_SLOT_DONE;
}

bool febe_slot_collect(struct febe_slot *s, uint32_t *arg, unsigned nargs,
                       uint32_t *ret)
{
    unsigned i;

    if (FEBE_LD(s->state) != FEBE_SLOT_DONE)
        return false;
    FEBE_BARRIER();
    for (i = 0; i < nargs && i < FEBE_ARG_WORDS; i++)
        arg[i] = FEBE_LD(s->arg[i]);
    if (ret)
        *ret = FEBE_LD(s->ret);
    FEBE_BARRIER();
    FEBE_ST(s->state, FEBE_SLOT_FREE);
    return true;
}

/* The be_call is cop_job_call() turned around: the front end is the
 * "worker" that calls back, the back end the "poster" that serves.  So the
 * front end's two points are cop_job's COPJ_CALL and COPJ_RETURN. */
unsigned febe_ops_post(uint32_t flags, bool cross_core)
{
    /* Always a write-back: no dirty FE line may sit over a buffer the BE
     * or a DMA engine will write (a later eviction would overwrite it),
     * and a bulk input buffer must be in memory */
    (void)flags;
    return copj_cache_ops(0, COPJ_CALL, cross_core);
}

unsigned febe_ops_reply(uint32_t flags, bool cross_core)
{
    return copj_cache_ops((flags & FEBE_F_WROTE) ? COPJ_CALL_WROTE : 0,
                          COPJ_RETURN, cross_core);
}

/* ---- generations ------------------------------------------------------- */

void febe_gen_publish(struct febe_gen *g)
{
    FEBE_BARRIER();
    FEBE_ST(g->pub, FEBE_LD(g->pub) + 1u);
}

bool febe_gen_check(const struct febe_gen *g, uint32_t *seen)
{
    uint32_t p = FEBE_LD(g->pub);

    if (p == *seen)
        return false;
    *seen = p;
    return true;
}

void febe_gen_ack(struct febe_gen *g, uint32_t seen)
{
    FEBE_ST(g->ack, seen);
}

bool febe_gen_reclaimable(const struct febe_gen *g, uint32_t gen)
{
    return (int32_t)(FEBE_LD(g->ack) - gen) >= 0;
}
