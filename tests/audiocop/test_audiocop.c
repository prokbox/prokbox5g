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

/* Host test for rc.6 (layout A): the PCM's FIQ, the mix and pcmbuf's
 * consumer on the COP, the producers on the CPU.  Two caches of 16-byte
 * lines (random replacement, never snooping) over one memory, as the
 * PP5022's (quirks 88, 90), and the real helpers of firmware/export/
 * pcm_xcore.h.  Each hazard class runs once with the rc.6 design (every
 * seed must pass) and once with one part of it taken away (some seed must
 * fail, so the model is shown able to see the hazard):
 *
 *  stale    the ring of PCM: the CPU writes laps through its cache, the
 *           COP's mix reads them.  rc.6: write back before publishing
 *           (commit_chunks()) and read through pcm_mix_src()'s uncached
 *           alias.  Broken: no write-back; or the mix reads through the
 *           COP's cache (the old lap stays there).
 *  index    chunk_ridx (the FIQ's) and chunk_widx (the CPU's) share a
 *           line.  rc.6: both uncached (XCORE_BSS).  Broken: cached, so
 *           one core's line write-back undoes the other's update.
 *  elapsed  a chunk's position: rc.6 posts it to the uncached mailbox
 *           (pcm_pos_post) and the CPU's tick takes it (pcm_pos_take),
 *           with every interleaving of the two at each store and load.
 *           Broken: the FIQ writes the playing id3's elapsed itself, in a
 *           line the CPU writes too; or a reader with no sequence check
 *           (a torn position).
 *
 * The fourth class, a FIQ calling the kernel (a corelock its own core's
 * interrupted thread holds), is checked on the built image instead:
 * scripts/cop_shared_check.py --fiq. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define UNCACHED_BASE_ADDR 0x10000000u
#define UNCACHED_ADDR(a) ((typeof (a))((uintptr_t)(a) | UNCACHED_BASE_ADDR))

static void hook(int where);
#define PCM_XCORE_HOOK(where) hook(where)
#include "pcm_xcore.h"

#define LINE    16
#define LW      (LINE / 4)
#define WORDS   1024
#define NLINES  8

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
    int i;
    for (i = 0; i < NLINES; i++)
        wb(&cache[core][i]);
}

static struct line *get(int core, unsigned w)
{
    unsigned base = w & ~(unsigned)(LW - 1);
    struct line *l;
    int i;
    for (i = 0; i < NLINES; i++)
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

/* an address: a word's byte address, or its uncached alias */
static void st(int core, uintptr_t a, uint32_t v)
{
    if (a >= UNCACHED_BASE_ADDR)
    {
        mem[(a - UNCACHED_BASE_ADDR) / 4] = v;
        return;
    }
    struct line *l = get(core, a / 4);
    l->d[(a / 4) % LW] = v;
    l->dirty = 1;
}

static uint32_t ld(int core, uintptr_t a)
{
    if (a >= UNCACHED_BASE_ADDR)
        return mem[(a - UNCACHED_BASE_ADDR) / 4];
    return get(core, a / 4)->d[(a / 4) % LW];
}

/* other traffic: each core touches unrelated memory, so lines evict */
static void noise(int core)
{
    int n = rnd() % 4;
    while (n--)
    {
        unsigned w = 512 + rnd() % 512;
        if (rnd() & 1)
            st(core, (uintptr_t)w * 4, rnd());
        else
            (void)ld(core, (uintptr_t)w * 4);
    }
}

static void reset(unsigned seed)
{
    memset(mem, 0, sizeof(mem));
    memset(cache, 0, sizeof(cache));
    rng = seed;
}

/* ---- stale: the ring ---------------------------------------------------- */

#define RING    64              /* words, at word RBASE */
#define RBASE   64              /* not at address 0: that is NULL */
#define CHUNK   16
#define LAPS    4

enum { S_OK, S_NO_COMMIT, S_CACHED_READ };

static int stale(unsigned seed, int mode)
{
    int bad = 0, lap, c, i;

    reset(seed);
    for (lap = 1; lap <= LAPS; lap++)
        for (c = 0; c < RING / CHUNK; c++)
        {
            /* the CPU's codec output: cached stores */
            for (i = 0; i < CHUNK; i++)
            {
                st(CPU, (uintptr_t)(RBASE + c * CHUNK + i) * 4,
                   (uint32_t)(lap << 16 | (c * CHUNK + i)));
                if (!(rnd() % 8))
                    noise(CPU);
            }
            if (mode != S_NO_COMMIT)
                commit(CPU);            /* commit_chunks() */
            noise(COP);
            /* the COP's mix reads the published chunk */
            for (i = 0; i < CHUNK; i++)
            {
                uintptr_t a = (uintptr_t)(RBASE + c * CHUNK + i) * 4;
                const void *p = (const void *)a;
                if (mode != S_CACHED_READ)
                    p = pcm_mix_src(p);
                if (ld(COP, (uintptr_t)p) !=
                    (uint32_t)(lap << 16 | (c * CHUNK + i)))
                    bad++;
            }
        }
    return bad;
}

/* ---- index: two indices in one line ------------------------------------- */

#define WIDX    (2u * 4)        /* bytes: word 2 */
#define RIDX    (3u * 4)        /* word 3, the same line */

static int index_upd(unsigned seed, int cached)
{
    uintptr_t w = cached ? WIDX : WIDX | UNCACHED_BASE_ADDR;
    uintptr_t r = cached ? RIDX : RIDX | UNCACHED_BASE_ADDR;
    uint32_t wv = 0, rv = 0;
    int k, bad = 0;

    reset(seed);
    for (k = 0; k < 200; k++)
    {
        if (rnd() & 1)
            st(CPU, w, ++wv);       /* commit_chunks() */
        else
            st(COP, r, ++rv);       /* pcmbuf_pcm_callback() */
        noise(CPU);
        noise(COP);
        /* each side reads the other's index */
        if (ld(COP, w) != wv && !cached)
            bad++;
        if (ld(CPU, r) != rv && !cached)
            bad++;
    }
    commit(CPU);
    commit(COP);
    if (ld(CPU, WIDX | UNCACHED_BASE_ADDR) != wv ||
        ld(CPU, RIDX | UNCACHED_BASE_ADDR) != rv)
        bad++;
    return bad;
}

/* ---- elapsed ------------------------------------------------------------ */

static struct pcm_pos_mbox mbox;     /* uncached on the device: plain here */
static unsigned seen;
static int in_hook, taken_bad, taken;
static int hook_mode;               /* 1: posts run inside a take; 2: the reverse */
static unsigned post_k;

static void check_taken(unsigned long e, off_t o, unsigned int k)
{
    taken++;
    if (e != (unsigned long)k * 1000 || o != (off_t)k * 7)
        taken_bad++;
}

static void do_take(void)
{
    unsigned long e; off_t o; unsigned int k;
    if (pcm_pos_take(&mbox, &seen, &e, &o, &k))
        check_taken(e, o, k);
}

static void hook(int where)
{
    if (in_hook || !hook_mode || (rnd() % 3))
        return;
    in_hook = 1;
    if (hook_mode == 1 && where >= 11)      /* a FIQ inside the tick */
    {
        post_k++;
        pcm_pos_post(&mbox, post_k * 1000, (off_t)post_k * 7, post_k);
    }
    else if (hook_mode == 2 && where < 11)  /* a tick read mid-post (the
                                               other core, any time) */
        do_take();
    in_hook = 0;
}

/* a reader without the sequence check: the hazard the seq guards */
static int naive_torn(unsigned seed)
{
    int k, bad = 0;
    reset(seed);
    memset(&mbox, 0, sizeof(mbox));
    for (k = 1; k < 2000; k++)
    {
        unsigned long e = mbox.elapsed;
        if (rnd() & 1)                      /* the post lands between */
            pcm_pos_post(&mbox, (unsigned long)k * 1000, (off_t)k * 7, k);
        off_t o = mbox.offset;
        unsigned int kk = mbox.key;
        if (kk && (e != (unsigned long)kk * 1000 || o != (off_t)kk * 7))
            bad++;
    }
    return bad;
}

static int elapsed_mbox(unsigned seed)
{
    int k;
    reset(seed);
    memset(&mbox, 0, sizeof(mbox));
    seen = 0; taken = taken_bad = 0; post_k = 0;
    for (k = 0; k < 4000; k++)
    {
        hook_mode = 1 + (int)(rnd() & 1);
        if (rnd() & 1)
        {
            post_k++;
            pcm_pos_post(&mbox, post_k * 1000, (off_t)post_k * 7, post_k);
        }
        else
            do_take();
    }
    hook_mode = 0;
    do_take();
    return taken_bad + (taken == 0);
}

/* the old design: the FIQ (COP) writes id3->elapsed through its cache;
   the CPU writes the id3's other fields in the same line */
static int elapsed_old(unsigned seed)
{
    const uintptr_t EL = 100 * 4, OTHER = 101 * 4;     /* one line */
    uint32_t want = 0;
    int k, bad = 0;

    reset(seed);
    for (k = 1; k < 200; k++)
    {
        if (rnd() & 1)
            st(COP, EL, want = (uint32_t)k * 1000);
        else
            st(CPU, OTHER, (uint32_t)k);
        noise(CPU);
        noise(COP);
        if (ld(CPU, EL) != want)        /* the UI's elapsed */
            bad++;
    }
    return bad;
}

int main(void)
{
    unsigned seed;
    int fails = 0, saw;

#define MUST_PASS(name, expr) do { \
        for (seed = 1; seed <= 300; seed++) \
            if (expr) { printf("FAIL %s seed %u\n", name, seed); fails++; break; } \
    } while (0)
#define MUST_CATCH(name, expr) do { \
        saw = 0; \
        for (seed = 1; seed <= 300 && !saw; seed++) \
            saw = (expr) != 0; \
        if (!saw) { printf("FAIL %s: the model no longer sees it\n", name); fails++; } \
        else printf("ok   %s: caught (seed %u)\n", name, seed - 1); \
    } while (0)

    MUST_PASS("stale: rc.6", stale(seed, S_OK));
    MUST_CATCH("stale: no write-back before publishing", stale(seed, S_NO_COMMIT));
    MUST_CATCH("stale: the mix reads through the COP's cache", stale(seed, S_CACHED_READ));
    MUST_PASS("index: rc.6 (uncached)", index_upd(seed, 0));
    MUST_CATCH("index: cached indices in one line", index_upd(seed, 1));
    MUST_PASS("elapsed: rc.6 mailbox", elapsed_mbox(seed));
    MUST_CATCH("elapsed: the FIQ writes the id3", elapsed_old(seed));
    MUST_CATCH("elapsed: a reader with no sequence check", naive_torn(seed));
    {
        /* pcm_mix_src's cases */
        int ok = pcm_mix_src(NULL) == NULL &&
            pcm_mix_src((const void *)0x00123450) == (const void *)0x10123450 &&
            pcm_mix_src((const void *)0x40001000) == (const void *)0x40001000 &&
            pcm_mix_src((const void *)0x10123450) == (const void *)0x10123450;
        if (!ok) { printf("FAIL pcm_mix_src mapping\n"); fails++; }
    }
    if (fails)
    {
        printf("%d failed\n", fails);
        return 1;
    }
    printf("ok   audio on the COP: stale, index, elapsed (300 seeds each)\n");
    return 0;
}
