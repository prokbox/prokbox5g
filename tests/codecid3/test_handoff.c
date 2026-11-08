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

/* Host test for #197: the codec slot changes hands between the cores at
 * the cop_job fences, and that is the whole of its coherence. Every other
 * object both cores touch in a COP run is uncached on both sides or one
 * core's alone (docs/design/cop-shared-state.md); this one is cached on
 * both, so its verdict rests on copj_cache_ops() (the real one, from
 * firmware/common/cop_job_core.c) being enough at every phase.
 *
 * Two caches of 16-byte lines (random replacement, never snooping) over
 * one memory, as the PP5022's. One decode, the way codec_cop.c drives it
 * (flags COPJ_CODE | COPJ_IN | COPJ_OUT; read_filebuf with
 * COPJ_CALL_WROTE):
 *  - the CPU loads the codec (image words and statics, cached), and its
 *    earlier use of the slot leaves dirty lines behind;
 *  - post; the COP starts, reads the image and statics, writes statics
 *    and its stack;
 *  - a call back: read_filebuf, the CPU writing the codec's buffer
 *    through the uncached alias; the COP reads it after;
 *  - the COP finishes; the CPU collects and reads the statics (the
 *    unload) and runs the codec itself once (a run on the CPU);
 *  - a second COP run of the same load, after the CPU's run.
 * Each check must hold over every seed with the real operations; run with
 * one phase's operation dropped (argv[1] = the phase), some check must
 * fail on some seed, so every fence is shown to matter. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cop_job_core.h"

#define LINE    16
#define WORDS   512                 /* the modelled slot, 32-bit words */
#define NLINES  6                   /* per core: small, so it evicts */
#define LW      (LINE / 4)

static uint32_t mem[WORDS];
struct line { int valid, dirty; unsigned base; uint32_t d[LW]; };
static struct line cache[2][NLINES];
static unsigned rng;
static int drop_phase = -1;

enum { CPU = 0, COP = 1 };

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

static void ops(int core, unsigned m)
{
    int i;
    for (i = 0; i < NLINES; i++)
    {
        if (m)
            wb(&cache[core][i]);
        if (m & (COPJ_C_COMMIT_DISCARD | COPJ_C_COMMIT_DISCARD_I))
            cache[core][i].valid = 0;
    }
}

static void fence(int core, unsigned flags, enum copj_phase ph)
{
    if ((int)ph == drop_phase)
        return;
    ops(core, copj_cache_ops(flags, ph, true));
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

/* The slot: image 0..127, statics 128..191, the codec's buffer 192..319
 * (read_filebuf's target), the COP's stack 448..511 (kept back) */
#define IMG     0
#define STAT    128
#define BUF     192
#define STK     448

static int run(unsigned seed)
{
    const unsigned F = COPJ_CODE | COPJ_IN | COPJ_OUT;
    int bad = 0;
    unsigned i;

    rng = seed;
    memset(mem, 0, sizeof mem);
    memset(cache, 0, sizeof cache);

    /* the slot's earlier use, then the load: both on the CPU, cached;
     * the COP's cache holds lines of the earlier codec from its last run */
    for (i = 0; i < WORDS; i += 3)
        st(CPU, i, 0xdead0000u + i);
    for (i = 0; i < WORDS; i += 5)
        get(COP, i);
    for (i = STAT; i < STAT + NLINES * LW; i += LW)  /* last: its statics */
        get(COP, i);
    for (i = IMG; i < STAT + 64; i++)
        st(CPU, i, 0x10ad0000u + i);

    fence(CPU, F, COPJ_POST);
    fence(COP, F, COPJ_START);
    for (i = IMG; i < STAT + 64; i += 7)        /* the COP sees the load */
        if (ld(COP, i) != 0x10ad0000u + i)
            bad |= 1;
    for (i = STAT; i < STAT + 64; i += 2)       /* the codec's statics */
        st(COP, i, 0xc0b00000u + i);
    for (i = STK; i < WORDS; i++)               /* its stack */
        st(COP, i, 0x57ac0000u + i);

    for (i = BUF; i < BUF + 24; i++)            /* it used its buffer */
        st(COP, i, 0xb0f0000u + i);
    /* read_filebuf: the handler on the CPU writes the buffer uncached */
    fence(COP, COPJ_CALL_WROTE, COPJ_CALL);
    for (i = BUF; i < BUF + 128; i++)
        mem[i] = 0xf11e0000u + i;
    fence(COP, COPJ_CALL_WROTE, COPJ_RETURN);
    for (i = BUF; i < BUF + 128; i += 3)
        if (ld(COP, i) != 0xf11e0000u + i)
            bad |= 2;

    for (i = STAT; i < STAT + 64; i += 2)       /* decoding goes on */
        st(COP, i, 0xc0b10000u + i);
    fence(COP, F, COPJ_FINISH);
    for (i = WORDS - 4; i < WORDS; i++)         /* the worker goes on: */
        st(COP, i, 0x57ad0000u + i);            /* one frame, one line */
    fence(CPU, F, COPJ_COLLECT);
    for (i = STAT; i < STAT + 64; i += 2)       /* unload / a CPU run */
        if (ld(CPU, i) != 0xc0b10000u + i)
            bad |= 4;
    for (i = STAT + 1; i < STAT + 64; i += 2)   /* the CPU's run writes */
        st(CPU, i, 0xc9b00000u + i);

    /* the next COP run of the same load */
    fence(CPU, F, COPJ_POST);
    fence(COP, F, COPJ_START);
    for (i = STAT; i < STAT + 64; i++)
    {
        uint32_t want = (i & 1) ? 0xc9b00000u + i : 0xc0b10000u + i;
        if (ld(COP, i) != want)
            bad |= 8;
    }
    fence(COP, F, COPJ_FINISH);
    fence(CPU, F, COPJ_COLLECT);
    return bad;
}

int main(int argc, char **argv)
{
    int lost[4] = { 0 }, runs = 0, b;
    unsigned seed;

    if (argc > 1)
        drop_phase = atoi(argv[1]);
    for (seed = 1; seed <= 2000; seed++, runs++)
    {
        int bad = run(seed * 2654435761u);
        for (b = 0; b < 4; b++)
            if (bad & (1 << b))
                lost[b]++;
    }
    printf("     %d runs%s: COP saw a stale load %d, stale read_filebuf %d; "
           "CPU saw stale statics %d; COP missed the CPU run's %d\n", runs, drop_phase >= 0 ? " (one fence dropped)" : "",
           lost[0], lost[1], lost[2], lost[3]);
    for (b = 0; b < 4; b++)
        if (lost[b])
            return 1;
    return runs == 2000 ? 0 : 1;
}
