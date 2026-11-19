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

/* 0.9.0 fiqstall (b16, #436): the PCM FIQ held off by a whole-cache
 * write-back.  Runs the REAL fiq-window block of pcm-pp.c
 * (pcm_pp_fiq_window(), with the real pcm_words_us() and pcm_tw_update():
 * run.sh extracts them) against a one-channel I2S model in microseconds:
 *
 *  - a piece of W FIFO words armed at a with f words free ends (its last
 *    word in the FIFO, the FIFO full) at a + (W - f) word times; the FIFO
 *    then lasts 16 word times; a FIQ that enters later than that found the
 *    FIFO empty (a click), as fiq_playback() counts it;
 *  - the FIQ enters at the first microsecond the core is not stalled and
 *    has the FIQ open, and arms the next piece at once;
 *  - a decode thread works for a random time, then does a whole-cache
 *    write-back: a stall of S us in which the FIQ cannot enter (the b16
 *    mechanism [hypothesis]: the vector's fetch waits for the write-back).
 *
 * Arms, at 96 kHz 24-bit, 48 kHz 24-bit, 44.1 kHz 24-bit and 44.1 kHz
 * 16-bit, pieces of 1.2-3.9 ms, stalls 100-761 us (b16's worst at 24 MHz):
 *   red:   no window (0.9.0 as shipped): FIFO empties > 0;
 *   red:   a window asked for less than the stall: FIFO empties > 0;
 *   green: the window as system-pp502x.c asks it (800 + 40 us): 0 empties,
 *          and its waits cost under 6 % of the time.
 * Plus the no-wait cases (FIQ masked, source masked, stopped, the queue
 * on, another core's FIQ) and the bound (a FIQ that never comes).
 * The sim and this model cannot show silicon stall timing: the stall is a
 * parameter here; the device log's stall= field measures it. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ICODE_ATTR
#define IBSS_ATTR
#define PCM_PP_QUEUE
#define DMA_MASK (1u << 26)
#define FIQ_STATUS 0x40

/* ---- the model's state ---- */
static uint32_t m_t;            /* now, us */
static int m_stalled;           /* a write-back runs: no FIQ entry */
static int m_fiq_open = 1;      /* the core's F bit clear */
static int m_never_fiq;         /* the bound arm: the request never comes */
static uint32_t m_int_en = DMA_MASK;
static unsigned m_core;
static uint32_t m_words;        /* the piece size in FIFO words */
static uint32_t m_empties, m_fiqs, m_late_max;

/* ---- the environment of the extracted block ---- */
static unsigned pcm_core;
#define CURRENT_CORE m_core
static uint8_t pq_on;
static struct { int state; } dma_play_data = { 1 };
#define CORE_INT_REG(reg, core) ((void)(core), m_int_en)
#define CPU_INT_EN_STAT 0
static bool interrupt_enabled(int st) { (void)st; return m_fiq_open; }
static uint32_t pcm_arm_us;
static uint16_t pcm_arm_words;
static uint8_t pcm_arm_free;
static uint32_t pcm_fiq_last_us;
static uint32_t pcm_tw_q8;
static uint32_t model_now(void);
#define USEC_TIMER model_now()

#include "fiqwin_parts.c"

static uint32_t piece_end(void)
{
    return pcm_arm_us + pcm_words_us(pcm_arm_words > pcm_arm_free ?
                                     pcm_arm_words - pcm_arm_free : 0);
}

/* the FIQ: judged as fiq_playback() judges, then the next piece armed */
static void model_fiq(void)
{
    uint32_t end = piece_end();
    uint32_t late = m_t - end;
    uint32_t cover = pcm_words_us(16);
    uint32_t free;

    m_fiqs++;
    if (late > m_late_max)
        m_late_max = late;
    if (late > cover)
        m_empties++;
    free = late >= cover ? 16 : (late * 256) / pcm_tw_q8;
    if (free > 16)
        free = 16;
    pcm_fiq_last_us = m_t;
    pcm_arm_us = m_t;
    pcm_arm_words = m_words;
    pcm_arm_free = free;
}

/* one microsecond passes; a due FIQ enters if it can */
static void model_step(void)
{
    m_t++;
    if (!m_stalled && m_fiq_open && !m_never_fiq &&
        (m_int_en & DMA_MASK) && (int32_t)(m_t - piece_end()) >= 0)
        model_fiq();
}

static uint32_t model_now(void)
{
    model_step();
    return m_t;
}

static uint32_t rnd(uint32_t lo, uint32_t hi)
{
    return lo + (uint32_t)(rand() % (int)(hi - lo + 1));
}

struct fmt { const char *name; unsigned long rate; unsigned bpf; };
static const struct fmt fmts[] = {
    { "96k/24", 96000, 8 }, { "48k/24", 48000, 8 },
    { "44.1k/24", 44100, 8 }, { "44.1k/16", 44100, 4 },
};

/* need 0: no window; else the window asked for need us */
static void run(const struct fmt *f, uint32_t piece_us, uint32_t need,
                uint32_t smin, uint32_t smax, unsigned commits,
                uint32_t *empties, uint32_t *wait_us, uint32_t *total_us)
{
    uint32_t t_start;

    pcm_tw_update(f->rate, f->bpf);
    m_words = (piece_us * 256) / pcm_tw_q8;
    m_t = 1000;
    m_empties = m_fiqs = m_late_max = 0;
    pcm_arm_us = m_t;
    pcm_arm_words = m_words;
    pcm_arm_free = 16;
    pcm_fiq_last_us = m_t;
    t_start = m_t;
    *wait_us = 0;
    for (unsigned i = 0; i < commits; i++)
    {
        uint32_t work = rnd(200, 25000), s;

        while (work--)
            model_step();
        if (need)
        {
            uint32_t t0 = m_t;

            pcm_pp_fiq_window(need);
            *wait_us += m_t - t0;
        }
        s = rnd(smin, smax);
        m_stalled = 1;
        while (s--)
            model_step();
        m_stalled = 0;
        model_step();       /* the held FIQ enters now */
    }
    *empties = m_empties;
    *total_us = m_t - t_start;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); \
                                       printf("\n"); fails++; } } while (0)

int main(void)
{
    static const uint32_t pieces[] = { 1200, 1590, 2000, 3186, 3900 };
    const unsigned commits = 3000;
    unsigned arms = 0;
    uint32_t red_total = 0, low_total = 0;
    uint32_t worst_cost_pm = 0;

    srand(436);
    for (size_t fi = 0; fi < sizeof(fmts) / sizeof(fmts[0]); fi++)
    {
        for (size_t pi = 0; pi < sizeof(pieces) / sizeof(pieces[0]); pi++)
        {
            uint32_t e, w, tot, cost_pm;

            /* red: no window */
            run(&fmts[fi], pieces[pi], 0, 100, 761, commits, &e, &w, &tot);
            red_total += e;
            /* red: a window short of the stall */
            run(&fmts[fi], pieces[pi], 300, 500, 761, commits, &e, &w, &tot);
            low_total += e;
            /* green: the window as system-pp502x.c asks it */
            run(&fmts[fi], pieces[pi], 800 + 40, 100, 761, commits,
                &e, &w, &tot);
            cost_pm = (uint32_t)((uint64_t)w * 1000 / tot);
            if (cost_pm > worst_cost_pm)
                worst_cost_pm = cost_pm;
            CHECK(e == 0, "%s piece %u us: %u FIFO empties with the window",
                  fmts[fi].name, (unsigned)pieces[pi], (unsigned)e);
            CHECK(m_fiqs > commits, "%s piece %u us: only %u FIQs ran",
                  fmts[fi].name, (unsigned)pieces[pi], (unsigned)m_fiqs);
            printf("  %-9s piece %4u us: window waits %2u.%u %% of the time\n",
                   fmts[fi].name, (unsigned)pieces[pi],
                   (unsigned)(cost_pm / 10), (unsigned)(cost_pm % 10));
            arms++;
        }
    }
    CHECK(red_total > 0, "no window: no FIFO empty (the model shows nothing)");
    CHECK(low_total > 0, "a short window: no FIFO empty (need not judged)");
    CHECK(worst_cost_pm < 60, "the window waits %u.%u %% of the time",
          (unsigned)(worst_cost_pm / 10), (unsigned)(worst_cost_pm % 10));
    printf("  red (no window): %u empties; short window: %u; arms %u x %u "
           "write-backs\n", (unsigned)red_total, (unsigned)low_total, arms,
           commits);

    /* the no-wait cases: each returns 0 with no time passed */
    pcm_tw_update(96000, 8);
    m_words = 300;
    pcm_arm_us = m_t;
    pcm_arm_words = m_words;
    pcm_arm_free = 16;
    {
        struct { const char *what; int *flag; int on; } cases[] = {
            { "the FIQ masked", &m_fiq_open, 0 },
            { "stopped", &dma_play_data.state, 0 },
        };
        for (size_t i = 0; i < 2; i++)
        {
            uint32_t t0;
            int old = *cases[i].flag, r;

            pcm_arm_us = m_t;           /* a piece just begun: 0 left */
            pcm_arm_free = (uint8_t)m_words;
            *cases[i].flag = cases[i].on;
            t0 = m_t;
            r = pcm_pp_fiq_window(5000);
            *cases[i].flag = old;
            CHECK(r == 0 && m_t == t0, "%s: waited (r=%d, %u us)",
                  cases[i].what, r, (unsigned)(m_t - t0));
        }
    }
    {
        uint32_t t0 = m_t;
        int r;

        m_int_en = 0;
        r = pcm_pp_fiq_window(5000);
        m_int_en = DMA_MASK;
        CHECK(r == 0 && m_t == t0, "the source masked: waited (r=%d)", r);
        pq_on = 1;
        r = pcm_pp_fiq_window(5000);
        pq_on = 0;
        CHECK(r == 0 && m_t == t0, "the queue on: waited (r=%d)", r);
        pcm_core = 1;
        r = pcm_pp_fiq_window(5000);
        pcm_core = 0;
        CHECK(r == 0 && m_t == t0, "another core's FIQ: waited (r=%d)", r);
    }
    /* a piece with room: no wait */
    {
        uint32_t t0;
        int r;

        pcm_arm_us = m_t;
        pcm_arm_words = 600;
        pcm_arm_free = 0;
        t0 = m_t;
        r = pcm_pp_fiq_window(840);
        CHECK(r == 0 && m_t - t0 < 5, "room left: r=%d after %u us", r,
              (unsigned)(m_t - t0));
    }
    /* the bound: a FIQ that never comes lets the caller go, counted */
    {
        uint32_t t0;
        int r;

        pcm_arm_us = m_t;
        pcm_arm_words = 20;
        pcm_arm_free = 0;
        m_never_fiq = 1;
        t0 = m_t;
        r = pcm_pp_fiq_window(840);
        m_never_fiq = 0;
        CHECK(r == 2, "a FIQ that never comes: r=%d", r);
        CHECK(m_t - t0 > 8000 && m_t - t0 < 8100,
              "the bound took %u us (8000 expected)", (unsigned)(m_t - t0));
    }
    if (fails)
    {
        printf("FAIL: %d check(s): the FIQ window (0.9.0 fiqstall)\n", fails);
        return 1;
    }
    printf("PASS: the FIQ window: %u arms, 0 FIFO empties with it, red "
           "without it\n", arms);
    return 0;
}
