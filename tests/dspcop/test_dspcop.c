/***************************************************************************
 *
 * Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
 * Not affiliated with or endorsed by Apple Inc.
 *
 * #282 step 4: the DSP on the COP. A model, not the firmware, of the two
 * hand-offs apps/codec_cop.c makes (docs/data-paths.md "DSP state across
 * the cores"):
 *  - the DSP's cross-core lock (dsp_xcore_lock/unlock): the holder writes
 *    its cache back when it lets go; a core taking it drops its own lines
 *    if the other core held it since. The COP runs a frame under it (reads
 *    a setting, advances a history word, writes samples); a CPU thread
 *    changes the setting or flushes the history under it. The setting and
 *    the history share one cache line, as DSP state does;
 *  - the pcmbuf window: the COP writes samples into a window of the ring
 *    (cached), the CPU publishes it (codec_pcm_window(): drop its lines,
 *    the wrap copy from the guard, write back, move the write index), and
 *    the COP drops its lines after the call (COPJ_CALL_WROTE). The PCM's
 *    FIQ reads the ring through the uncached alias.
 * Two write-back caches of 16-byte lines with random replacement and no
 * snooping, as tests/audiocop. Each fence is taken away in turn and the
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
    NO_UNLOCK_COMMIT = 1,   /* the holder lets go without writing back */
    NO_LOCK_DISCARD  = 2,   /* the taker keeps lines the other core staled */
    NO_WROTE_DISCARD = 4,   /* the COP keeps its lines over the publish */
    NO_CPU_DISCARD   = 8,   /* the CPU publishes over stale lines */
};
static unsigned faults;

static uint32_t gen[2], seen[2];     /* uncached on the device */

static void xlock(int core)
{
    if (gen[core ^ 1] != seen[core])
    {
        seen[core] = gen[core ^ 1];
        if (!(faults & NO_LOCK_DISCARD))
            commit_discard(core);
    }
}

static void xunlock(int core)
{
    if (!(faults & NO_UNLOCK_COMMIT))
        commit(core);
    gen[core]++;
}

/* DSP state: the setting and the history in one line */
#define W_GAIN  16
#define W_HIST  17

/* The ring: RING words from RBASE, the guard after it */
#define RBASE   64
#define RING    64
#define GUARD   8
#define FRAME   3               /* samples a frame: not a line multiple */
#define PUB     6               /* publish at this many */

static unsigned widx, ridx;     /* uncached on the device: plain here */
static unsigned win_start, win_used, win_room;
static uint32_t expect[RING];   /* what the COP wrote at each slot */
static uint32_t gain_now;       /* the last setting the CPU let go of */
static int gain_fresh;          /* set since the COP's last frame */
static uint32_t hist_expect;    /* what the COP must read next */
static unsigned seq;

static int bad_gain, bad_hist, bad_pcm, frames, consumed;

static unsigned ring_free(void)
{
    return (ridx + RING - widx - 1) % RING;
}

/* CPU: codec_pcm_window(): publish, then grant the next window */
static void cpu_window(void)
{
    if (win_used)
    {
        if (!(faults & NO_CPU_DISCARD))
            commit_discard(CPU);
        unsigned end = win_start + win_used;
        if (end > RING)                 /* the wrap copy, from the guard */
            for (unsigned i = 0; i < end - RING; i++)
                st(CPU, RBASE + i, ld(CPU, RBASE + RING + i));
        commit(CPU);                    /* commit_chunks() */
        widx = end % RING;
    }
    win_start = widx;
    win_used = 0;
    unsigned room = ring_free();
    if (room > RING - widx + GUARD)
        room = RING - widx + GUARD;
    win_room = room;
}

/* COP: win_call(): the call (COPJ_CALL writes back), the return
 * (COPJ_CALL_WROTE drops) */
static void cop_window(void)
{
    commit(COP);
    cpu_window();
    if (!(faults & NO_WROTE_DISCARD))
        commit_discard(COP);
}

/* COP: one dsp_process() under the lock */
static void cop_frame(void)
{
    if (win_room - win_used < FRAME)
    {
        cop_window();
        if (win_room - win_used < FRAME)
            return;                     /* full: the CPU would sleep */
    }
    xlock(COP);
    uint32_t g = ld(COP, W_GAIN);
    if (gain_fresh && g != gain_now)
        bad_gain++;
    gain_fresh = 0;
    uint32_t h = ld(COP, W_HIST);
    if (h != hist_expect)
        bad_hist++;
    st(COP, W_HIST, h + 1);
    hist_expect = h + 1;
    for (int i = 0; i < FRAME; i++)
    {
        unsigned pos = win_start + win_used;
        uint32_t v = (++seq << 8) | (g & 0xff);
        st(COP, RBASE + pos, v);        /* pos may run into the guard */
        expect[pos % RING] = v;
        win_used++;
    }
    xunlock(COP);
    frames++;
    if (win_used >= PUB)
        cop_window();
}

/* A CPU thread: a setting, or a flush (DSP_FLUSH) */
static void cpu_setting(void)
{
    xlock(CPU);
    if (rnd() & 1)
    {
        gain_now = 1 + rnd() % 200;
        st(CPU, W_GAIN, gain_now);
        gain_fresh = 1;
    }
    else
    {
        st(CPU, W_HIST, 0);
        hist_expect = 0;
    }
    xunlock(CPU);
}

/* The FIQ: reads published samples through the uncached alias */
static void fiq(void)
{
    int n = 1 + rnd() % 4;
    while (n-- && ridx != widx)
    {
        if (mem[RBASE + ridx] != expect[ridx])
            bad_pcm++;
        ridx = (ridx + 1) % RING;
        consumed++;
    }
}

static int run(unsigned seed, unsigned f)
{
    memset(mem, 0, sizeof(mem));
    memset(cache, 0, sizeof(cache));
    memset(expect, 0, sizeof(expect));
    memset(gen, 0, sizeof(gen));
    memset(seen, 0, sizeof(seen));
    rng = seed;
    faults = f;
    widx = ridx = win_start = win_used = win_room = 0;
    gain_now = 0; gain_fresh = 0; hist_expect = 0; seq = 0;
    bad_gain = bad_hist = bad_pcm = frames = consumed = 0;
    for (int step = 0; step < 3000; step++)
    {
        unsigned r = rnd() % 16;
        if (r < 7)
            cop_frame();
        else if (r < 8)
            cpu_setting();
        else if (r < 13)
            fiq();
        else
            noise(r & 1 ? CPU : COP);
    }
    return bad_gain + bad_hist + bad_pcm;
}

int main(void)
{
    int fail = 0;
    long fr = 0, co = 0;
    const unsigned seeds = 300;

    for (unsigned s = 1; s <= seeds; s++)
    {
        if (run(s, 0))
        {
            printf("FAIL seed %u: gain %d hist %d pcm %d\n",
                   s, bad_gain, bad_hist, bad_pcm);
            fail = 1;
        }
        fr += frames;
        co += consumed;
    }
    /* a model that ran nothing proves nothing */
    if (fr < 1000L * seeds / 2 || co < 1000L * seeds / 2)
    {
        printf("FAIL: too little traffic (frames %ld, samples %ld)\n", fr, co);
        fail = 1;
    }
    printf("as built: %u seeds, %ld frames, %ld samples consumed\n",
           seeds, fr, co);

    static const struct { unsigned f; const char *name; } m[] = {
        { NO_UNLOCK_COMMIT, "no write-back at unlock" },
        { NO_LOCK_DISCARD,  "no drop at lock" },
        { NO_WROTE_DISCARD, "no COP drop after the publish" },
        { NO_CPU_DISCARD,   "no CPU drop before the publish" },
    };
    for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++)
    {
        unsigned caught = 0;
        for (unsigned s = 1; s <= seeds; s++)
            if (run(s, m[i].f))
                caught++;
        printf("%-34s caught on %u/%u seeds\n", m[i].name, caught, seeds);
        if (!caught)
        {
            printf("FAIL: the model cannot see \"%s\"\n", m[i].name);
            fail = 1;
        }
    }
    return fail;
}
