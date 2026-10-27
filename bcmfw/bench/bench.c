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
 * bcmfw timing bench (BCMFW_BENCH builds): after the probe has reported
 * ready, time each kernel against the 1 MHz counter, run the vector and
 * scalar checks, and fill the results block for the host.  No panel, GPIO
 * output, clock or DMA access, except the clock switch of the separate
 * BCMFW_BENCH_PLL build.  docs/research/bcm2722/bringup.md, "Device
 * session 2".
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
#include "bcmfw.h"
#include "hw.h"
#include "bench/bench.h"
#include "clock.h"

#if BCMFW_BENCH

#define RES ((volatile uint32_t *)BCMFW_BENCH_RESULTS)

static uint32_t out[64] __attribute__((aligned(32)));

static void step(uint32_t v)
{
    MB(BCMFW_MB_PROBE_STEP) = v;
}

/* Every kernel BCMFW_BENCH_RUNS times; keep the fastest and slowest */
static void time_all(uint32_t at, uint32_t step_base)
{
    uint32_t k, run;

    for (k = 0; k < bench_nkernels; k++)
    {
        const struct bench_kdesc *d = &bench_kdesc[k];
        volatile uint32_t *e = RES + at + k * BE_WORDS;
        uint32_t lo = 0xFFFFFFFF, hi = 0;

        step(step_base | k);
        for (run = 0; run < BCMFW_BENCH_RUNS; run++)
        {
            uint32_t t = bench_time(k, BCMFW_BENCH_ITERS);

            if (t < lo)
                lo = t;
            if (t > hi)
                hi = t;
        }
        e[BE_TAG] = d->tag;
        e[BE_INSNS] = BCMFW_BENCH_ITERS * d->per_iter;
        e[BE_OPS] = BCMFW_BENCH_ITERS * d->ops;
        e[BE_ROWS] = BCMFW_BENCH_ITERS * d->rows;
        e[BE_MIN] = lo;
        e[BE_MAX] = hi;
        e[BE_PER_ITER] = d->per_iter;
        e[7] = 0;
    }
}

/* First mismatching word of check d in out, or BVF_NONE */
static uint32_t first_bad(const struct bench_vdesc *d)
{
    uint32_t i;

    for (i = 0; i < d->words; i++)
        if (out[i] != d->want[i])
            return i;
    return BVF_NONE;
}

static void check_vectors(void)
{
    uint32_t k, i;
    uint32_t pass[2] = { 0, 0 }, late[2] = { 0, 0 }, all[2] = { 0, 0 };

    RES[BR_FIRST_FAIL] = 0xFFFFFFFF;
    RES[BR_VREC_WORDS] = BV_WORDS;
    for (k = 0; k < bench_nvcases; k++)
    {
        const struct bench_vdesc *d = &bench_vdesc[k];
        volatile uint32_t *v = RES + BR_VREC_AT + k * BV_WORDS;
        uint32_t bad, flags = 0;

        step(BCMFW_BENCH_STEP_VEC | k);
        for (i = 0; i < 64; i++)
            out[i] = 0xA5A5A5A5;
        bench_vcase(k, d->in, out);
        /* the first words at once, before anything else reads out[] */
        for (i = 0; i < 4; i++)
            v[BV_GOT + i] = out[i];
        bad = first_bad(d);
        if (bad == BVF_NONE)
        {
            flags |= BVF_PASS;
            pass[k >> 5] |= 1u << (k & 31);
            v[BV_BAD_GOT] = v[BV_BAD_WANT] = 0;
        }
        else
        {
            v[BV_BAD_GOT] = out[bad];
            v[BV_BAD_WANT] = d->want[bad];
            if (RES[BR_FIRST_FAIL] == 0xFFFFFFFF)
            {
                RES[BR_FIRST_FAIL] = k << 16 | bad;
                RES[BR_FAIL_GOT] = out[bad];
                RES[BR_FAIL_WANT] = d->want[bad];
            }
        }
        /* again after a delay: a vector store still in flight lands */
        spin(BENCH_LATE_SPIN);
        for (i = 0; i < 4; i++)
            v[BV_LATE + i] = out[i];
        if (first_bad(d) == BVF_NONE)
        {
            flags |= BVF_LATE_PASS;
            late[k >> 5] |= 1u << (k & 31);
        }
        v[BV_FLAGS] = flags | d->words << 8 | bad << 16;
        v[BV_TAG] = d->tag;
        if (!d->info)
            all[k >> 5] |= 1u << (k & 31);
    }
    /* an informational check's result is in its record and BR_VEC_LATE */
    RES[BR_VEC_PASS] = pass[0] & all[0];
    RES[BR_VEC_ALL] = all[0];
    RES[BR_VEC_LATE] = late[0];
    RES[BR_VEC_PASS_HI] = pass[1] & all[1];
    RES[BR_VEC_ALL_HI] = all[1];
    RES[BR_VEC_LATE_HI] = late[1];
    RES[BR_VEC_COUNT] = bench_nvcases;
    RES[BR_VREC] = BR_VREC_AT;
}

/* Where does a vst16 from a row other than d00 land?  Fill the vector
 * buffer, store one tagged row to its middle, wait, and record every
 * word that changed (bench.h, BS_*). */
#define SLOC_FILL   0xA5A5A5A5

static void locate_stores(void)
{
    uint32_t k, i, n = bench_vbuf_bytes / 4, t = bench_sloc_target / 4;

    for (k = 0; k < bench_nsloc; k++)
    {
        volatile uint32_t *r = RES + BR_SLOC_AT + k * BS_WORDS;
        uint32_t changed = 0, first = 0, last = 0;

        step(BCMFW_BENCH_STEP_SLOC | k);
        for (i = 0; i < n; i++)
            bench_vbuf[i] = SLOC_FILL;
        bench_sloc(k, bench_vbuf + t);
        spin(BENCH_LATE_SPIN);
        for (i = 0; i < n; i++)
        {
            if (bench_vbuf[i] == SLOC_FILL)
                continue;
            if (!changed++)
                first = i;
            last = i;
        }
        r[BS_CHANGED] = changed;
        r[BS_FIRST] = 4 * (first - t);
        r[BS_LAST] = 4 * (last - t);
        r[BS_W_FIRST] = changed ? bench_vbuf[first] : 0;
        r[BS_W_TARGET] = bench_vbuf[t];
        r[BS_W_LAST] = changed ? bench_vbuf[last] : 0;
        r[BS_SPAN] = bench_sloc_target << 16 | (bench_vbuf_bytes - bench_sloc_target);
        r[BS_ROW] = BS_MAGIC | bench_sloc_src[k];
    }
    RES[BR_SLOC_COUNT] = bench_nsloc;
    RES[BR_SLOC] = BR_SLOC_AT;
}

#if BCMFW_BENCH_PLL
static void pll_step(uint32_t n)
{
    step(BCMFW_BENCH_STEP_PLL | n);
}

/* The core clock to 150 MHz (clock.c: Apple's start-up order and values
 * exactly; the bench compares the write sequence with vmcs.bin 1.3's) */
static int pll_150(void)
{
    struct clock_readback rb;
    int ok = clock_fast(pll_step, &rb);

    RES[BR_REG_BEFORE] = rb.reg_before;
    RES[BR_REG_AFTER] = rb.reg_after;
    RES[BR_PLLA_M] = rb.plla_m;
    RES[BR_PLLA_CTL] = rb.plla_ctl;
    RES[BR_CLK_SEL] = rb.clk_sel;
    return ok;
}
#endif

void bench_run(int timer_ok)
{
    uint32_t i, n = bench_nkernels;

    for (i = 0; i < BR_HEADER_WORDS; i++)
        RES[i] = 0;
    RES[BR_STATE] = 1;
    RES[BR_NKERNELS] = n;
    RES[BR_ITERS] = BCMFW_BENCH_ITERS;
    RES[BR_RUNS] = BCMFW_BENCH_RUNS;
    RES[BR_FLAGS] = (timer_ok ? BRF_TIMER_OK : 0)
                  | (BCMFW_BENCH_PLL ? BRF_PLL_BUILD : 0);
    RES[BR_TABLE1] = BR_HEADER_WORDS;
    RES[BR_ENTRY_WORDS] = BE_WORDS;
    RES[BR_MAGIC] = BCMFW_BENCH_MAGIC;
    MB(BCMFW_MB_BENCH) = BCMFW_BENCH_RESULTS;

    /* correctness first: it is quick, and a fault names its check */
    check_vectors();
    locate_stores();
    step(BCMFW_BENCH_STEP_SCALAR);
    RES[BR_SCALAR_ALL] = BENCH_SCALAR_ALL;
    RES[BR_SCALAR_PASS] = bench_scalar_check();
    RES[BR_PROBE_ALL] = MB(BCMFW_MB_PROBE_ALL);
    RES[BR_PROBE_PASS] = MB(BCMFW_MB_PROBE_PASS);

    time_all(BR_HEADER_WORDS, BCMFW_BENCH_STEP_TIME);
#if BCMFW_BENCH_PLL
    RES[BR_FLAGS] |= BRF_PLL_DONE | (pll_150() ? 0 : BRF_PLL_STALL);
    RES[BR_TABLE2] = BR_HEADER_WORDS + n * BE_WORDS;
    time_all(BR_HEADER_WORDS + n * BE_WORDS, BCMFW_BENCH_STEP_TIME2);
#endif
    RES[BR_STATE] = 2;
    step(BCMFW_BENCH_DONE);
}

#endif /* BCMFW_BENCH */
