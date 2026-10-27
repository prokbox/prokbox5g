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
 *  Two-core cache-incoherence model for the boundary (P19 S1; design
 * section 7.2, sim-fidelity risk #1).
 *
 * The simulator is one core with no cache, so it can never show a missing
 * write-back or drop.  This model can: two "cores" (threads that run one
 * at a time, switching at seeded random points on every shared access),
 * each with its own direct-mapped write-back data cache over one shared
 * SDRAM, plus an uncached window onto the same SDRAM.  Dirty lines are
 * evicted at random (write back), like a real cache under pressure.
 * fe_be_core.c runs against it through FEBE_LD/FEBE_ST.  Each protocol is
 * run correct (it must never read stale data, over every seed) and with
 * one cache operation left out (the model must catch it on some seed).
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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fe_be_core.h"
#include "cop_job_core.h"

#define MEMW    1024                /* shared SDRAM, words */
#define LINES   32                  /* per core: 32 lines of 16 B */
#define LW      4                   /* words per line */

static uint32_t mem[MEMW];          /* pointers here: the uncached alias */
static uint32_t cview[MEMW];        /* pointers here: cached (address only) */

struct line { int valid, dirty; unsigned tag; uint32_t d[LW]; };
static struct line cache[2][LINES];

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int turn, alive[2];
static __thread int me;
static unsigned rng;

static unsigned rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return (rng >> 16) & 0x7fff;
}

static void switch_to_other(void)
{
    if (!alive[!me])
        return;
    turn = !me;
    pthread_cond_broadcast(&cv);
    while (turn != me)
        pthread_cond_wait(&cv, &mx);
}

static void writeback(struct line *l)
{
    if (l->valid && l->dirty)
        memcpy(&mem[l->tag * LW], l->d, sizeof(l->d));
    l->dirty = 0;
}

/* every access: maybe a switch, maybe a dirty line of this core evicted */
static void step(void)
{
    if (rnd() % 3 == 0)
        switch_to_other();
    if (rnd() % 5 == 0)
    {
        struct line *l = &cache[me][rnd() % LINES];
        writeback(l);
        l->valid = 0;
    }
}

static struct line *fill(unsigned i)
{
    unsigned tag = i / LW;
    struct line *l = &cache[me][tag % LINES];

    if (!l->valid || l->tag != tag)
    {
        writeback(l);
        l->valid = 1;
        l->dirty = 0;
        l->tag = tag;
        memcpy(l->d, &mem[tag * LW], sizeof(l->d));
    }
    return l;
}

uint32_t febe_mm_load(const volatile uint32_t *p)
{
    uint32_t v;
    step();
    if (p >= mem && p < mem + MEMW)
        v = mem[p - mem];
    else if (p >= cview && p < cview + MEMW)
    {
        unsigned i = (unsigned)(p - cview);
        v = fill(i)->d[i % LW];
    }
    else
        v = *p;                     /* thread-local, not shared */
    return v;
}

void febe_mm_store(volatile uint32_t *p, uint32_t v)
{
    step();
    if (p >= mem && p < mem + MEMW)
        mem[p - mem] = v;
    else if (p >= cview && p < cview + MEMW)
    {
        unsigned i = (unsigned)(p - cview);
        struct line *l = fill(i);
        l->d[i % LW] = v;
        l->dirty = 1;
    }
    else
        *p = v;
}

void febe_mm_yield(void)
{
    step();
}

/* this core's whole-cache operations (commit_dcache and friends) */
static void cache_ops(unsigned ops)
{
    int i;
    for (i = 0; i < LINES; i++)
    {
        struct line *l = &cache[me][i];
        if (ops & (COPJ_C_COMMIT | COPJ_C_COMMIT_DISCARD |
                   COPJ_C_COMMIT_DISCARD_I))
            writeback(l);
        if (ops & (COPJ_C_COMMIT_DISCARD | COPJ_C_COMMIT_DISCARD_I))
            l->valid = 0;
    }
}

/* ---- the runner -------------------------------------------------------- */
typedef void (*side_fn)(int variant);
struct run { side_fn fn; int core, variant; };

static void *thread_main(void *arg)
{
    struct run *r = arg;
    pthread_mutex_lock(&mx);
    me = r->core;
    while (turn != me)
        pthread_cond_wait(&cv, &mx);
    r->fn(r->variant);
    alive[me] = 0;
    turn = !me;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mx);
    return NULL;
}

static void run2(unsigned seed, side_fn be, side_fn fe, int variant)
{
    pthread_t t0, t1;
    struct run r0 = { be, 0, variant }, r1 = { fe, 1, variant };

    memset(mem, 0, sizeof(mem));
    memset(cache, 0, sizeof(cache));
    rng = seed;
    turn = (int)(seed & 1);
    alive[0] = alive[1] = 1;
    pthread_create(&t0, NULL, thread_main, &r0);
    pthread_create(&t1, NULL, thread_main, &r1);
    pthread_join(t0, NULL);
    pthread_join(t1, NULL);
}

/* wait for a condition: hand the turn over until it holds */
#define WAIT_UNTIL(c) do { while (!(c)) switch_to_other(); } while (0)

static int stale;                   /* stale reads seen in this run */

/* ---- protocol A: a be_call whose output the BE writes into an FE buffer
 * (a playlist range, a file read).  variant 1 leaves out the FE's
 * write-back-and-drop after DONE.  variant 2 leaves out the write-back
 * before the post, with the FE having written the buffer first (a dirty
 * line over what the BE then writes: the eviction bomb). */
#define SLOT    ((struct febe_slot *)&mem[0])
#define BUFW    48                  /* the FE buffer: cview[256..304) */
#define BUF0    256

static void a_be(int variant)
{
    bool bad;
    int i;
    (void)variant;
    WAIT_UNTIL(FEBE_LD(SLOT->state) == FEBE_SLOT_FILLED);
    febe_slot_take(SLOT, &bad);
    for (i = 0; i < BUFW; i++)      /* through the uncached alias */
        FEBE_ST(mem[BUF0 + i], 1000u + (unsigned)i);
    febe_slot_finish(SLOT, 1);
}

static void a_fe(int variant)
{
    uint32_t arg[1] = { BUF0 }, ret;
    int i;
    /* the FE has used the buffer: it holds lines of it, clean or dirty */
    for (i = 0; i < BUFW; i++)
    {
        if (variant == 2)
            FEBE_ST(cview[BUF0 + i], 7);
        else
            (void)FEBE_LD(cview[BUF0 + i]);
    }
    febe_slot_fill(SLOT, 1, 5, FEBE_F_WROTE, arg, 1);
    if (variant != 2)
        cache_ops(febe_ops_post(FEBE_F_WROTE, true));
    WAIT_UNTIL(febe_slot_done(SLOT));
    if (variant != 1)
        cache_ops(febe_ops_reply(FEBE_F_WROTE, true));
    febe_slot_collect(SLOT, arg, 1, &ret);
    for (i = 0; i < BUFW; i++)
        if (FEBE_LD(cview[BUF0 + i]) != 1000u + (unsigned)i)
            stale++;
    cache_ops(COPJ_C_COMMIT);       /* and in memory, after the FE parks */
    for (i = 0; i < BUFW; i++)
        if (mem[BUF0 + i] != 1000u + (unsigned)i)
            stale++;
}

/* ---- protocol B: a table by generation (a library view, an art slot).
 * The BE writes it through its cache, publishes; the FE, which had read
 * the old table, sees the new generation and reads.  variant 1: the BE
 * does not write back before publishing.  variant 2: the FE does not drop
 * before reading. */
#define GEN     ((struct febe_gen *)&mem[64])
#define TAB0    512
#define TABW    40

static void b_be(int variant)
{
    int i;
    for (i = 0; i < TABW; i++)
        FEBE_ST(cview[TAB0 + i], 500u + (unsigned)i);
    if (variant != 1)
        cache_ops(COPJ_C_COMMIT);           /* xcore_publish() */
    febe_gen_publish(GEN);
    WAIT_UNTIL(febe_gen_reclaimable(GEN, 1));   /* the old one is free */
}

static void b_fe(int variant)
{
    uint32_t seen = 0;
    int i;
    for (i = 0; i < TABW; i++)              /* the old table, cached */
        (void)FEBE_LD(cview[TAB0 + i]);
    WAIT_UNTIL(febe_gen_check(GEN, &seen));
    if (variant != 2)
        cache_ops(COPJ_C_COMMIT_DISCARD);   /* xcore_acquire() */
    for (i = 0; i < TABW; i++)
        if (FEBE_LD(cview[TAB0 + i]) != 500u + (unsigned)i)
            stale++;
    febe_gen_ack(GEN, seen);
}

/* ---- protocol C: the now record's seqlock, uncached, under the model's
 * interleavings: a torn copy is never accepted */
#define NOWSEQ  mem[128]
#define NOWW    6
#define NOW0    132

static void c_be(int variant)
{
    uint32_t v;
    int i;
    (void)variant;
    for (v = 1; v <= 60; v++)
    {
        febe_seq_write_begin(&NOWSEQ);
        for (i = 0; i < NOWW; i++)
            FEBE_ST(mem[NOW0 + i], v);
        febe_seq_write_end(&NOWSEQ);
    }
}

static void c_fe(int variant)
{
    int k, i;
    (void)variant;
    for (k = 0; k < 60; k++)
    {
        uint32_t d[NOWW];
        if (febe_seq_copy(&NOWSEQ, &mem[NOW0], d, NOWW, 1000) < 0)
            continue;
        for (i = 1; i < NOWW; i++)
            if (d[i] != d[0])
                stale++;
    }
}

static int check(const char *name, side_fn be, side_fn fe, int variant,
                 bool expect_stale)
{
    unsigned seed, hit = 0, seeds = 300;
    for (seed = 1; seed <= seeds; seed++)
    {
        stale = 0;
        run2(seed, be, fe, variant);
        if (stale)
            hit++;
    }
    if (expect_stale ? hit == 0 : hit != 0)
    {
        fprintf(stderr, "FAIL cachemodel %s v%d: stale in %u/%u seeds "
                "(expected %s)\n", name, variant, hit, seeds,
                expect_stale ? "some" : "none");
        return 1;
    }
    printf("     cachemodel %-18s v%d: stale in %3u/%u seeds (%s)\n", name,
           variant, hit, seeds, expect_stale ? "caught" : "clean");
    return 0;
}

int main(void)
{
    int f = 0;
    f += check("be_call bulk out", a_be, a_fe, 0, false);
    f += check("be_call bulk out", a_be, a_fe, 1, true);   /* no drop */
    f += check("be_call bulk out", a_be, a_fe, 2, true);   /* no clean */
    f += check("generation", b_be, b_fe, 0, false);
    f += check("generation", b_be, b_fe, 1, true);         /* no publish */
    f += check("generation", b_be, b_fe, 2, true);         /* no acquire */
    f += check("now seqlock", c_be, c_fe, 0, false);
    return f ? 1 : 0;
}
