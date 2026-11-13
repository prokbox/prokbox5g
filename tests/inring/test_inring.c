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
 * ProkBox (0.8.3 colocate): a two-cache host model of the compressed
 * ring the COP now reads itself (apps/buffering.c struct
 * buf_remote_reader, apps/codec_cop.c in_request()/in_advance(); rules in
 * docs/data-paths.md R7 item 11). The CPU (buffering) fills the ring with
 * cached stores (a bounce-buffer copy) or by DMA, writes its cache back
 * and only then publishes end; the COP reads [pos, end) through the
 * uncached alias (or cached after dropping its lines, the other arm the
 * brief asks about), and publishes pos only after the reads. The CPU
 * refills only below its applied view of pos. Positions are single-writer
 * words in uncached memory; a stale view of either is safe, which the
 * model shows by serving each read of the other core's position from a
 * random older value.
 *
 * Two write-back caches of 16-byte lines with random replacement and no
 * snooping, as tests/dspcop. Each fence is taken away in turn and the
 * model must catch it on some seed (a byte the COP read that is not the
 * file's).
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
#define NLINES  8
#define RING    64              /* words: 16 lines, so laps reuse lines */
#define TOTAL   6000            /* words the COP must read */

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

/* ---- the protocol, with each fence switchable --------------------------- */

enum {
    NO_FILL_WB      = 1,    /* buffering publishes end before writing back */
    NO_COP_DROP     = 2,    /* cached arm: the COP reads over its old lines */
    POS_BEFORE_READ = 4,    /* the COP frees bytes before it has read them */
    FILL_PAST_POS   = 8,    /* buffering refills ahead of the applied pos */
};
static unsigned faults;
static int cached_arm;      /* the COP reads cached after a drop per span */

/* the uncached words, and a short history so a reader may see an old one */
#define HIST 8
static uint32_t end_w, pos_w;
static uint32_t end_h[HIST], pos_h[HIST];
static unsigned end_n, pos_n;

static void publish(uint32_t *w, uint32_t *h, unsigned *n, uint32_t v)
{
    *w = v;
    h[(*n)++ % HIST] = v;
}

/* the other core's view: the current word, or one of its last values,
 * but never older than what this reader saw before (an uncached word read
 * twice by one core does not go back in time) */
static uint32_t view(const uint32_t *w, const uint32_t *h, unsigned n,
                     uint32_t *seen)
{
    unsigned back = rnd() % HIST;
    uint32_t v = (back == 0 || n == 0 || back >= n) ? *w
                                                    : h[(n - 1 - back) % HIST];
    if (v < *seen)
        v = *seen;
    *seen = v;
    return v;
}
static uint32_t cpu_seen_pos, cop_seen_end;

static uint32_t cpu_end;    /* the producer's own (next word to write) */
static uint32_t cop_pos;    /* the consumer's own (next word to read) */
static long bad, reads;

/* CPU: one fill chunk (buffer_handle()'s read), then remote_publish() */
static void cpu_fill(void)
{
    uint32_t pos = view(&pos_w, pos_h, pos_n, &cpu_seen_pos);    /* remote_apply() */
    uint32_t room = RING - 1 - (cpu_end - pos);
    uint32_t n = 1 + rnd() % 12;

    if (faults & FILL_PAST_POS)
        room += 4;
    if (n > room)
        n = room;
    if (n == 0)
        return;
    if (rnd() & 1)
    {
        /* a bounce-buffer copy: cached stores */
        for (uint32_t i = 0; i < n; i++)
            st(CPU, (cpu_end + i) % RING, cpu_end + i);
    }
    else
    {
        /* DMA: the driver writes back and drops first, the engine writes
         * memory */
        commit_discard(CPU);
        for (uint32_t i = 0; i < n; i++)
            mem[(cpu_end + i) % RING] = cpu_end + i;
    }
    cpu_end += n;
    if (!(faults & NO_FILL_WB))
        commit(CPU);                                /* commit_dcache() */
    publish(&end_w, end_h, &end_n, cpu_end);
}

/* COP: one request_buffer() + advance_buffer() */
static void cop_read(void)
{
    uint32_t end = view(&end_w, end_h, end_n, &cop_seen_end);
    uint32_t n = 1 + rnd() % 8;

    if (end - cop_pos < n)
        return;                 /* not buffered yet: one call, in the code */
    if (faults & POS_BEFORE_READ)
    {
        publish(&pos_w, pos_h, &pos_n, cop_pos + n);
        for (int k = rnd() % 3; k > 0; k--)
            cpu_fill();         /* the CPU runs meanwhile */
    }
    if (cached_arm && !(faults & NO_COP_DROP))
        commit_discard(COP);    /* one drop per span */
    for (uint32_t i = 0; i < n; i++)
    {
        unsigned w = (cop_pos + i) % RING;
        uint32_t v = cached_arm ? ld(COP, w) : mem[w];

        if (v != cop_pos + i)
            bad++;
        reads++;
        if (i == n / 2)
            for (int k = rnd() % 3; k > 0; k--)
                cpu_fill();     /* the CPU fills while the COP reads */
    }
    cop_pos += n;
    publish(&pos_w, pos_h, &pos_n, cop_pos);    /* in_advance(): after */
}

static int run(unsigned seed)
{
    long steps = 0;

    rng = seed;
    memset(mem, 0xee, sizeof(mem));
    memset(cache, 0, sizeof(cache));
    end_w = pos_w = cpu_end = cop_pos = 0;
    end_n = pos_n = 0;
    cpu_seen_pos = cop_seen_end = 0;
    bad = reads = 0;
    /* an earlier lap's bytes sit in both caches */
    for (unsigned w = 0; w < RING; w += LW)
    {
        (void)ld(COP, w);
        if (rnd() & 1)
            (void)ld(CPU, w);
    }
    while (cop_pos < TOTAL && steps++ < 200000)
    {
        switch (rnd() % 4)
        {
        case 0: cpu_fill(); break;
        case 1: cop_read(); break;
        case 2: noise(CPU); break;
        default: noise(COP); break;
        }
    }
    if (cop_pos < TOTAL)
        return -1;              /* stalled */
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
        printf("FAIL %s: %d of 300 seeds read a wrong word (expected %s)\n",
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

    /* the protocol as written: both read arms */
    faults = 0; cached_arm = 0;
    f |= sweep("uncached reads, stale position views", 0);
    cached_arm = 1;
    f |= sweep("cached reads after a drop per span", 0);
    /* each fence taken away */
    cached_arm = 0;
    faults = NO_FILL_WB;
    f |= sweep("no write-back before end is published", 1);
    faults = POS_BEFORE_READ;
    f |= sweep("pos published before the reads", 1);
    faults = FILL_PAST_POS;
    f |= sweep("a fill past the applied pos", 1);
    cached_arm = 1;
    faults = NO_COP_DROP;
    f |= sweep("cached reads without the drop", 1);
    printf("words read in the last run: %ld\n", reads);
    return f;
}
