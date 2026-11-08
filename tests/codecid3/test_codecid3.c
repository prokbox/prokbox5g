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

/* Host test of #196: the codec's id3 (playback.c CODEC_ID3) written by
 * both cores while the decode runs on the COP.
 *
 * A model of the PP5022's memory: SDRAM ('mem', which is also the
 * uncached alias) and a cached view of it ('cview', addresses only). The
 * CPU's data cache holds 16-byte lines, written back whole, evicted at
 * random; it never snoops. The COP reaches the entry only through the
 * uncached alias (codec_cop.c sync_fields()), so its stores go straight
 * to memory. Playback's side is modelled as it touches CODEC_ID3:
 *  - the whole entry written at a track start (id3_write), then the job
 *    post's write-back (COPJ_POST: commit, lines stay);
 *  - the codec on the COP writing length and tail_trim (vgm, shorten,
 *    asap) and elapsed (mpa's ASF stream);
 *  - the CPU writing elapsed and offset (set_elapsed, set_offset, a
 *    seek) mid-run, and reading them for the next PCM insert;
 *  - its neighbours: buflib's header in front of the scratch area and
 *    unbuffered_id3 after the entry, both written through the cache;
 *  - the slot's memory used before through the cache (the ring).
 * Every seed and every misalignment of the scratch area must keep every
 * write of both cores. Built twice by run.sh: with apps/codec_id3.c (the
 * fix) it must pass; with -DCODEC_ID3_OLD (the entry as 0.8.0-rc.4 /
 * a0ece5a839 placed it: a plain member, through the cache, no drop) the
 * same checks fail. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "cop_job.h"
#include "codec_id3.h"

#define LINE    CODEC_ID3_LINE
#define SZ      4096                /* modelled SDRAM, bytes */
#define NLINES  8                   /* the CPU's cache, lines (small: it
                                       evicts often) */

/* A stand-in for struct mp3entry: the fields #196 is about, at offsets
 * like the real ones (length and elapsed adjacent, tail_trim near,
 * offset far after the id3v2 buffer) */
#define E_SIZE      300
#define F_FREQ      40
#define F_LENGTH    60
#define F_ELAPSED   64
#define F_TAIL      72
#define F_BUF       100             /* atrac3_rm's id3v2buf copy */
#define F_OFFSET    260
#define F_LAST      (E_SIZE - 4)

static uint8_t mem[SZ] __attribute__((aligned(16)));
static uint8_t cview[SZ] __attribute__((aligned(16)));

struct line { int valid, dirty; size_t base; uint8_t d[LINE]; };
static struct line cpu[NLINES];
static unsigned rng;
static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

static unsigned rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return (rng >> 16) & 0x7fff;
}

static int is_cached(const void *p)
{
    return (const uint8_t *)p >= cview && (const uint8_t *)p < cview + SZ;
}

void *cop_uncached(const void *p)
{
    if (is_cached(p))
        return mem + ((const uint8_t *)p - cview);
    return (void *)p;
}

static void writeback(struct line *l)
{
    if (l->valid && l->dirty)
        memcpy(mem + l->base, l->d, LINE);
    l->dirty = 0;
}

static void cpu_commit(void)
{
    for (int i = 0; i < NLINES; i++)
        writeback(&cpu[i]);
}

void commit_discard_dcache(void)
{
    cpu_commit();
    for (int i = 0; i < NLINES; i++)
        cpu[i].valid = 0;
}

static struct line *cpu_line(size_t a)
{
    size_t base = a & ~(size_t)(LINE - 1);
    struct line *l;
    int i;

    for (i = 0; i < NLINES; i++)
        if (cpu[i].valid && cpu[i].base == base)
            return &cpu[i];
    l = &cpu[rnd() % NLINES];       /* random replacement */
    writeback(l);
    l->valid = 1;
    l->dirty = 0;
    l->base = base;
    memcpy(l->d, mem + base, LINE);
    return l;
}

/* A random eviction, as other work on the CPU would cause */
static void cpu_churn(void)
{
    if (rnd() % 3 == 0)
    {
        struct line *l = &cpu[rnd() % NLINES];
        writeback(l);
        l->valid = 0;
    }
}

/* The CPU: through its cache for the cached view, else straight */
static void cpu_st32(void *p, uint32_t v)
{
    if (is_cached(p))
    {
        size_t a = (uint8_t *)p - cview;
        struct line *l = cpu_line(a);
        memcpy(l->d + (a & (LINE - 1)), &v, 4);
        l->dirty = 1;
    }
    else
        memcpy(p, &v, 4);
    cpu_churn();
}

static uint32_t cpu_ld32(const void *p)
{
    uint32_t v;

    if (is_cached(p))
    {
        size_t a = (const uint8_t *)p - cview;
        struct line *l = cpu_line(a);
        memcpy(&v, l->d + (a & (LINE - 1)), 4);
    }
    else
        memcpy(&v, p, 4);
    cpu_churn();
    return v;
}

/* The COP: its view of the entry is the uncached alias (sync_fields()) */
static void cop_st32(void *entry, size_t off, uint32_t v)
{
    memcpy((uint8_t *)cop_uncached(entry) + off, &v, 4);
}

static uint32_t mem_ld32(void *entry, size_t off)
{
    uint32_t v;
    memcpy(&v, (uint8_t *)cop_uncached(entry) + off, 4);
    return v;
}

#define F(e, off) ((uint8_t *)(e) + (off))

/* playback.c's placement: the scratch area at 'scratch' (the audio
 * buffer's start, after buflib's header), the entry first in it */
#ifdef CODEC_ID3_OLD
#define SLOT_BYTES  E_SIZE
static void *place(void *slot) { return slot; }
#else
#define SLOT_BYTES  CODEC_ID3_SLOT_SIZE(E_SIZE)
static void *place(void *slot) { return codec_id3_place(slot); }
#endif

static int run(unsigned seed, size_t mis)
{
    uint8_t *scratch = cview + 256 + mis;    /* cached, as buflib gives it */
    uint8_t *hdr = scratch - 4;              /* buflib's header word */
    uint8_t *next = scratch + SLOT_BYTES;    /* unbuffered_id3 */
    void *e;
    int bad = 0, i;

    rng = seed;
    memset(mem, 0, sizeof mem);
    memset(cpu, 0, sizeof cpu);

    /* the memory's earlier use: the ring's bytes written through the
     * cache, some still dirty */
    for (i = 0; i < (int)SLOT_BYTES + 32; i += 4)
        cpu_st32(scratch - 16 + i, 0xdead0000u + i);

    /* scratch_mem_init() (the codec stopped) */
    e = place(scratch);

    /* a track start: id3_write(CODEC_ID3, ...) on the CPU */
    for (i = 0; i + 4 <= E_SIZE; i += 4)
        cpu_st32(F(e, i), 0x1d300000u + i);
    cpu_commit();                           /* COPJ_POST */

    /* the run: the codec on the COP and the CPU's callbacks interleave */
    cop_st32(e, F_FREQ, 44100);             /* shorten */
    cpu_st32(F(e, F_OFFSET), 4096);         /* set_offset */
    cop_st32(e, F_LENGTH, 180000);          /* vgm, asap, shorten */
    cpu_st32(F(e, F_ELAPSED), 1000);        /* set_elapsed */
    cpu_st32(hdr, 0xb0f1b0f1u);             /* buflib, its own word */
    cop_st32(e, F_TAIL, 4000);              /* vgm */
    cop_st32(e, F_BUF, 0xa7a7a7a7u);        /* atrac3_rm */
    cpu_st32(next, 0x0b0b0b0bu);            /* unbuffered_id3 */
    cop_st32(e, F_LAST, 0x1a571a57u);       /* the entry's last word */
    if (cpu_ld32(F(e, F_LENGTH)) != 180000) /* the CPU sees the COP's */
        bad |= 1;
    cpu_st32(F(e, F_ELAPSED), 2000);        /* a seek, mid-run */
    cop_st32(e, F_ELAPSED, 2500);           /* mpa ASF: the codec's own */
    cpu_st32(F(e, F_OFFSET), 8192);
    if (cpu_ld32(F(e, F_ELAPSED)) != 2500)  /* next PCM insert reads it */
        bad |= 2;

    commit_discard_dcache();                /* everything lands */
    if (mem_ld32(e, F_FREQ) != 44100)     bad |= 4;
    if (mem_ld32(e, F_LENGTH) != 180000)  bad |= 4;
    if (mem_ld32(e, F_TAIL) != 4000)      bad |= 4;
    if (mem_ld32(e, F_BUF) != 0xa7a7a7a7u) bad |= 4;
    if (mem_ld32(e, F_LAST) != 0x1a571a57u) bad |= 4;
    if (mem_ld32(e, F_ELAPSED) != 2500)   bad |= 8;
    if (mem_ld32(e, F_OFFSET) != 8192)    bad |= 8;
    /* the track start's words nobody wrote since: its first and last
     * lines (a neighbour's line over them) and two inside (the memory's
     * earlier use written back over them) */
    {
        static const size_t only_start[] = { 0, 8, 200, E_SIZE - 8 };
        for (i = 0; i < 4; i++)
            if (mem_ld32(e, only_start[i]) != 0x1d300000u + only_start[i])
                bad |= 8;
    }
    {
        uint32_t h, n;
        memcpy(&h, mem + (hdr - cview), 4);
        memcpy(&n, mem + (next - cview), 4);
        if (h != 0xb0f1b0f1u || n != 0x0b0b0b0bu)
            bad |= 16;                      /* the neighbours kept theirs */
    }
    return bad;
}

int main(void)
{
    int seeds = 0, lost[5] = { 0 };
    unsigned seed;
    size_t mis;

    for (mis = 0; mis < LINE; mis += 4)
        for (seed = 1; seed <= 500; seed++)
        {
            int bad = run(seed * 7919u + (unsigned)mis, mis);
            for (int b = 0; b < 5; b++)
                if (bad & (1 << b))
                    lost[b]++;
            seeds++;
        }
    printf("     %d runs (500 seeds x 4 alignments): "
           "CPU read stale COP field %d, stale elapsed %d, COP write lost %d, "
           "CPU write lost %d, neighbour lost %d\n",
           seeds, lost[0], lost[1], lost[2], lost[3], lost[4]);
    CHECK(lost[0] == 0);    /* the CPU reads what the COP wrote */
    CHECK(lost[1] == 0);    /* ... the newest elapsed, whoever wrote it */
    CHECK(lost[2] == 0);    /* no COP write lost to a CPU line */
    CHECK(lost[3] == 0);    /* no CPU write lost */
    CHECK(lost[4] == 0);    /* the neighbours' cached words survive */
    CHECK(seeds == 2000);   /* the runs happened */
    if (fails)
    {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    return 0;
}
