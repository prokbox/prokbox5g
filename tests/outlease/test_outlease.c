/***************************************************************************
 *              __________                   __    __________
 *   Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
 *   Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
 *   Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
 *   Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
 *                                               \/        \/               \/
 * Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
 * Not affiliated with or endorsed by Apple Inc.
 *
 * ProkBox (0.8.3 colocate): a two-cache host model of pcmbuf's write side
 * moving between the cores (apps/pcmbuf.c pcmbuf_lease_*(), apps/codec_cop.c
 * lease_frames(); rules in docs/data-paths.md R7 item 12). Two arms
 * alternate at random, as the device does:
 *  - the CPU owns the write side (the window calls): the COP writes the
 *    samples into a window, writes back at the call; the CPU drops its
 *    lines, copies the wrap from the guard, may mix a crossfade into
 *    published samples, writes back, then moves widx; the COP drops its
 *    lines on return (COPJ_CALL_WROTE);
 *  - the COP holds the lease: it writes, copies the wrap itself, writes
 *    back and moves widx; it writes back again when it gives the lease
 *    back with a partial chunk waiting; the CPU drops its lines when it
 *    reclaims it, and waits for the COP to let go.
 * The FIQ reads [ridx, widx) uncached and checks every sample.
 *
 * Two write-back caches of 16-byte lines with random replacement and no
 * snooping, as tests/dspcop. Each fence is taken away in turn and the
 * model must catch it on some seed.
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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LINE    16
#define LW      (LINE / 4)
#define WORDS   1024
#define NLINES  16              /* the whole ring fits: stale lines live long */
#define PR      64              /* the ring, words */
#define GUARD   8               /* past its end, words */
#define TOTAL   8000            /* samples the FIQ must play */

enum { CPU = 0, COP = 1 };

static uint32_t mem[WORDS];
struct line { int valid, dirty; unsigned base; uint32_t d[LW]; };
static struct line cache[2][NLINES];
static unsigned rng;

static unsigned rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return (rng >> 16) & 0x7fff;
}

static void wb(struct line *l)
{
    if (l->valid && l->dirty)
        memcpy(&mem[l->base], l->d, LINE);
    l->dirty = 0;
}

static void commit(int core)
{
    for (int i = 0; i < NLINES; i++)
        wb(&cache[core][i]);
}

static void commit_discard(int core)
{
    commit(core);
    for (int i = 0; i < NLINES; i++)
        cache[core][i].valid = 0;
}

static struct line *get(int core, unsigned w)
{
    unsigned base = w & ~(unsigned)(LW - 1);
    struct line *l;
    for (int i = 0; i < NLINES; i++)
        if (cache[core][i].valid && cache[core][i].base == base)
            return &cache[core][i];
    l = &cache[core][rnd() % NLINES];
    wb(l);
    l->valid = 1;
    l->dirty = 0;
    l->base = base;
    memcpy(l->d, &mem[base], LINE);
    return l;
}

static void st(int core, unsigned w, uint32_t v)
{
    struct line *l = get(core, w);
    l->d[w % LW] = v;
    l->dirty = 1;
}

static uint32_t ld(int core, unsigned w)
{
    return get(core, w)->d[w % LW];
}

static void noise(int core)
{
    int n = rnd() % 4;
    while (n--)
    {
        unsigned w = 512 + rnd() % 512;
        if (rnd() & 1)
            st(core, w, rnd());
        else
            (void)ld(core, w);
    }
}

enum {
    NO_COMMIT_WB    = 1,    /* the COP moves widx before writing back */
    NO_RELEASE_WB   = 2,    /* the COP lets go with its partial chunk dirty */
    NO_RECLAIM_DROP = 4,    /* the CPU keeps old lines of the audio */
    NO_GRANT_DROP   = 8,    /* the COP keeps old lines over the granting call */
    NO_REVOKE_WAIT  = 16,   /* the CPU takes it back while the COP writes */
};
static unsigned faults;

/* the uncached words (chunk_widx, chunk_ridx, the lease's) */
static uint32_t widx, ridx, waiting;
static int leased;                  /* the COP holds the write side */
static uint32_t next_val;           /* the next sample's value */
static long bad, played;

static unsigned ring_w(uint32_t pos) { return pos % PR; }

/* the producer writes n samples at widx + waiting (the window), through
 * its cache; past the ring's end into the guard, never wrapping before
 * the commit (get_write_buffer(): chunk_widx + pcmbuf_bytes_waiting) */
static uint32_t write_window(int core, uint32_t n)
{
    uint32_t room = PR - 1 - (widx + waiting - ridx);
    uint32_t idx = ring_w(widx) + waiting;

    if (idx >= PR + GUARD)
        return 0;               /* the guard is full: a commit first */
    if (n > room)
        n = room;
    if (n > PR + GUARD - idx)
        n = PR + GUARD - idx;
    for (uint32_t i = 0; i < n; i++)
        st(core, idx + i, next_val + i);
    next_val += n;
    waiting += n;
    return n;
}

/* commit_chunks(): the wrap copy, the write-back, widx */
static void commit_all(int core, int wb_fault)
{
    uint32_t idx = ring_w(widx);
    if (waiting == 0)
        return;
    if (idx + waiting > PR)
        for (uint32_t i = 0; i < idx + waiting - PR; i++)
            st(core, i, ld(core, PR + i));
    if (!wb_fault)
        commit(core);
    widx += waiting;
    waiting = 0;
}

/* the CPU mixes a crossfade into published, unplayed samples (it reads
 * and writes them back unchanged: what it reads must be what is there) */
static void cpu_mix(void)
{
    for (uint32_t p = ridx; p < widx && p < ridx + 6; p++)
    {
        uint32_t v = ld(CPU, ring_w(p));
        st(CPU, ring_w(p), v);
    }
    commit(CPU);    /* pcmbuf_write_complete(): back to SDRAM at once */
}

/* the FIQ: a few samples, uncached */
static void fiq(void)
{
    int n = 1 + rnd() % 6;
    while (n-- && ridx < widx)
    {
        uint32_t v = mem[ring_w(ridx)];
        if (v != ridx + 1)
            bad++;
        ridx++;
        played++;
    }
}

/* one window call: the COP writes back (the call), the CPU drops, commits
 * (maybe mixing), and may grant the lease; the COP drops on return */
static void window_call(void)
{
    commit(COP);                        /* the call's write-back */
    commit_discard(CPU);                /* codec_pcm_window(): drop */
    if (rnd() % 2 == 0)
        cpu_mix();
    commit_all(CPU, 0);
    if (rnd() % 3 == 0)
    {
        commit(CPU);                    /* pcmbuf_lease_grant() */
        leased = 1;
    }
    if (!(leased && (faults & NO_GRANT_DROP)))
        commit_discard(COP);            /* COPJ_CALL_WROTE */
}

static int needs_call;              /* the COP has no window: a call first */

static void cop_step(void)
{
    if (!leased && needs_call)
    {
        /* after the lease went back, the COP writes only into a window a
         * call gave it (lease_frames() returns to the window calls) */
        window_call();
        needs_call = 0;
    }
    write_window(COP, 1 + rnd() % 7);
    if (!leased)
    {
        if (rnd() % 2)
            window_call();
        return;
    }
    if (rnd() % 2)
        commit_all(COP, faults & NO_COMMIT_WB);  /* pcmbuf_lease_commit() */
    if (rnd() % 5 == 0)
    {
        /* pcmbuf_lease_release(): a message, a call, a low level */
        if (!(faults & NO_RELEASE_WB))
            commit(COP);
        leased = 0;
        needs_call = 1;                 /* the call path takes over */
    }
}

/* another CPU thread reclaims the lease (a skip, a stop, a track change) */
static void cpu_reclaim(void)
{
    if (!leased)
        return;
    if (faults & NO_REVOKE_WAIT)
    {
        /* the CPU goes on while the COP finishes its frame: the COP's
         * samples are in its cache, the CPU commits its own view */
        leased = 0;
        needs_call = 1;
        if (!(faults & NO_RECLAIM_DROP))
            commit_discard(CPU);
        commit_all(CPU, 0);             /* e.g. COMMIT_ALL_DATA */
        commit(COP);                    /* ... the COP writes back late */
        return;
    }
    /* the COP sees the revoke at its next window: it writes back and lets
     * go, its partial chunk left for the CPU to commit (lease.waiting) */
    if (!(faults & NO_RELEASE_WB))
        commit(COP);
    leased = 0;
    needs_call = 1;
    if (!(faults & NO_RECLAIM_DROP))
        commit_discard(CPU);
    if (rnd() % 2)
        cpu_mix();
    commit_all(CPU, 0);
    commit(CPU);
}

static int run(unsigned seed)
{
    long steps = 0;

    rng = seed;
    memset(mem, 0, sizeof(mem));
    memset(cache, 0, sizeof(cache));
    widx = ridx = waiting = 0;
    leased = 0;
    needs_call = 1;
    next_val = 1;
    bad = played = 0;
    /* an earlier lap's samples sit in both caches */
    for (unsigned w = 0; w < PR + GUARD; w += LW)
    {
        (void)ld(CPU, w);
        if (rnd() & 1)
            (void)ld(COP, w);
    }
    while (played < TOTAL && steps++ < 400000)
    {
        switch (rnd() % 6)
        {
        case 0: case 1: cop_step(); break;
        case 2: fiq(); break;
        case 3: if (rnd() % 8 == 0) cpu_reclaim(); else fiq(); break;
        case 4: noise(CPU); break;
        default: noise(COP); break;
        }
    }
    if (played < TOTAL)
        return -1;
    return bad ? 1 : 0;
}

static int sweep(const char *what, int expect_bad)
{
    int caught = 0, stalls = 0;

    for (unsigned seed = 1; seed <= 300; seed++)
    {
        int r = run(seed);
        if (r < 0)
            stalls++;
        else if (r > 0)
            caught++;
    }
    if (stalls)
    {
        printf("FAIL %s: %d seeds stalled\n", what, stalls);
        return 1;
    }
    if (expect_bad ? caught == 0 : caught != 0)
    {
        printf("FAIL %s: %d of 300 seeds played a wrong sample (expected %s)\n",
               what, caught, expect_bad ? "some" : "none");
        return 1;
    }
    printf("%-46s %s (%d/300 seeds caught)\n", what,
           expect_bad ? "caught" : "clean", caught);
    return 0;
}

int main(void)
{
    int f = 0;

    faults = 0;
    f |= sweep("the lease as written (both arms, crossfades)", 0);
    faults = NO_COMMIT_WB;
    f |= sweep("widx moved before the COP's write-back", 1);
    faults = NO_RELEASE_WB;
    f |= sweep("given back with the partial chunk dirty", 1);
    faults = NO_RECLAIM_DROP;
    f |= sweep("reclaimed over the CPU's old lines", 1);
    faults = NO_GRANT_DROP;
    f |= sweep("granted over the COP's old lines", 1);
    faults = NO_REVOKE_WAIT;
    f |= sweep("reclaimed without waiting for the COP", 1);
    printf("samples played in the last run: %ld\n", played);
    return f;
}
