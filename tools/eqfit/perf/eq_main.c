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
 * Bare-metal ARM driver for the software EQ's per-sample cost
 * (docs/phase15-brief.md section 3.1, lean rule 8), run by
 * tools/eqfit/perf/eq_perf.sh under qemu-system-arm and the risk tools'
 * instruction-count plugin (scripts/risk/qemu-harness/libinsn.c).
 *
 * It links the firmware's own lib/rbcodec/dsp/dsp_arm.o (build/fw-ui) and
 * calls its filter_process() and pga_process() exactly as eq_process() and
 * the PGA stage do: one second of 44.1 kHz stereo in the DSP's 128-sample
 * blocks (dsp_sample_input.c SAMPLE_BUF_COUNT), through BANDS biquads
 * (a preset with all 10 bands non-zero, e.g. Acoustic) plus the precut
 * gain pass. BANDS=0 PGA=0 is the baseline (boot and loop overhead),
 * subtracted by the script. The coefficients don't matter to the count
 * (the loops have no data-dependent branches); full-scale-ish samples do
 * matter to the cycle estimate (ARM7TDMI multiply early termination).
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

#ifndef BANDS
#define BANDS 10
#endif
#ifndef PGA
#define PGA 1
#endif

#define RATE 44100
#define BLOCK 128

/* lib/rbcodec/dsp/dsp_filter.h, dsp_core.h, dsp_proc_entry.h layouts */
struct dsp_filter {
    int32_t coefs[5];
    int32_t history[2][4];
    uint8_t shift;
};
struct dsp_buffer {
    int32_t remcount;
    int32_t *p32[2];
    uint32_t proc_mask;
    uint8_t format[12];         /* format.num_channels at byte 1 */
};
struct dsp_proc_entry {
    intptr_t data;
    void *process;
};

void filter_process(struct dsp_filter *f, int32_t *buf[], int count,
                    unsigned int channels);
void pga_process(struct dsp_proc_entry *this, struct dsp_buffer **buf_p);

static struct dsp_filter flt[10];
static int32_t left[BLOCK], right[BLOCK];
static int32_t pga_gain = 0x00800000;   /* pga_data.gain: unity, s7.24 */
volatile int32_t sink;

int main(void)
{
    int32_t *buf[2] = { left, right };
    struct dsp_buffer db = { 0, { left, right }, 0, { 0, 2 } };
    struct dsp_buffer *dbp = &db;
    struct dsp_proc_entry pe = { (intptr_t)&pga_gain, 0 };
    int i, b, done;

    for (b = 0; b < 10; b++)
    {
        flt[b].coefs[0] = 0x10000000;   /* s3.28 unity-ish */
        flt[b].coefs[1] = -0x1e000000;
        flt[b].coefs[2] = 0x0e000000;
        flt[b].coefs[3] = 0x1e000000;
        flt[b].coefs[4] = -0x0e000000;
        flt[b].shift = 4;
    }
    for (i = 0; i < BLOCK; i++)
    {
        left[i] = (int32_t)(((uint32_t)i * 0x01234567u) ^ 0x5a5a5a5au);
        right[i] = (int32_t)(((uint32_t)i * 0x07654321u) ^ 0x3c3c3c3cu);
    }
    for (done = 0; done < RATE; done += BLOCK)
    {
        int n = RATE - done < BLOCK ? RATE - done : BLOCK;
#if PGA
        db.remcount = n;
        pga_process(&pe, &dbp);
#endif
        for (b = 0; b < BANDS; b++)
            filter_process(&flt[b], buf, n, 2);
        (void)dbp; (void)pe;
    }
    sink = left[0] + right[BLOCK - 1];
    return 0;
}
