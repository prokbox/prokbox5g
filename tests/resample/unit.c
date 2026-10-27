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

/* Buffer-boundary test for the RESAMPLE stage's band-limited worker (P19
 * 19-13). Builds lib/rbcodec/dsp/resample.c itself (with the warble build's
 * config) and checks that every mode gives the same output, sample for
 * sample, whether its input arrives in one block or in irregular blocks of
 * 1..37 samples (odd tails held across /2 and /4 groups, the x3/x6 queue
 * refilled mid-call), mono and stereo. */
#include "../../lib/rbcodec/dsp/resample.c"
#include <stdio.h>
#include <stdlib.h>

/* DSP core stubs: resample_bl() and resample_new_delta() only */
unsigned int dsp_get_output_frequency(struct dsp_config *dsp)
{ (void)dsp; return 0; }
void dsp_proc_activate(struct dsp_config *dsp, enum dsp_proc_ids id, bool a)
{ (void)dsp; (void)id; (void)a; }
bool dsp_proc_active(struct dsp_config *dsp, enum dsp_proc_ids id)
{ (void)dsp; (void)id; return true; }
void dsp_proc_enable(struct dsp_config *dsp, enum dsp_proc_ids id, bool e)
{ (void)dsp; (void)id; (void)e; }
void dsp_proc_set_in_place(struct dsp_config *dsp, enum dsp_proc_ids id,
                           bool p)
{ (void)dsp; (void)id; (void)p; }
void dsp_proc_want_format_update(struct dsp_config *dsp,
                                 enum dsp_proc_ids id)
{ (void)dsp; (void)id; }
unsigned int dsp_get_id(const struct dsp_config *dsp)
{ (void)dsp; return CODEC_IDX_AUDIO; }

#define NIN 6000
static int32_t in[2][NIN];
static int32_t out[2][2][NIN * 6 + 64];

/* Feed NIN samples in blocks from next_len(); returns the output count */
static int run(unsigned int fin, unsigned int fout, int nch, int irregular,
               int32_t (*o)[NIN * 6 + 64])
{
    struct resample_data *data = &resample_data[CODEC_IDX_AUDIO];
    struct sample_format f = { 0 };
    int32_t buf0[RESAMPLE_BUF_COUNT], buf1[RESAMPLE_BUF_COUNT];
    int pos = 0, nout = 0, len = 1;
    unsigned int r = 7;

    f.num_channels = nch;
    f.frequency = fin;
    data->frequency_out = 0;
    resample_new_delta(data, &f, fout);
    resample_flush_data(data);
    rs_bl_flush();
    if (rs_bl.mode == RS_HERMITE)
        return -1;

    while (pos < NIN)
    {
        if (irregular)
        {
            r = r * 1103515245u + 12345u;
            len = 1 + (r >> 16) % 37;
        }
        else
        {
            len = NIN;
        }
        struct dsp_buffer src = { 0 };
        src.remcount = MIN(len, NIN - pos);
        src.p32[0] = in[0] + pos;
        src.p32[1] = in[nch - 1] + pos;
        src.format = f;
        while (src.remcount > 0)
        {
            struct dsp_buffer dst = { 0 };
            dst.p32[0] = buf0;
            dst.p32[1] = buf1;
            dst.bufcount = RESAMPLE_BUF_COUNT;
            int took = resample_bl(data, &src, &dst);
            for (int i = 0; i < dst.remcount; i++)
            {
                o[0][nout + i] = buf0[i];
                o[1][nout + i] = buf1[i];
            }
            nout += dst.remcount;
            if (took <= 0 && dst.remcount == 0)
            {
                printf("FAIL stuck at %d\n", pos);
                exit(1);
            }
            src.remcount -= took;
            src.p32[0] += took;
            src.p32[1] += took;
            pos += took;
        }
    }
    /* drain the x3/x6 queue (the stage sends it with the next buffer) */
    for (;;)
    {
        struct dsp_buffer src = { 0 }, dst = { 0 };
        src.format = f;
        dst.p32[0] = buf0;
        dst.p32[1] = buf1;
        dst.bufcount = RESAMPLE_BUF_COUNT;
        resample_bl(data, &src, &dst);
        if (dst.remcount == 0)
            break;
        for (int i = 0; i < dst.remcount; i++)
        {
            o[0][nout + i] = buf0[i];
            o[1][nout + i] = buf1[i];
        }
        nout += dst.remcount;
    }
    return nout;
}

int main(void)
{
    static const unsigned int rates[][2] = {
        { 88200, 44100 }, { 96000, 48000 }, { 176400, 44100 },
        { 192000, 48000 }, { 22050, 44100 }, { 11025, 44100 },
        { 16000, 48000 }, { 8000, 48000 }, { 32000, 48000 },
    };
    int fail = 0;
    for (int i = 0; i < NIN; i++)
    {
        in[0][i] = (int32_t)(rand() % (1 << 27)) - (1 << 26);
        in[1][i] = (int32_t)(rand() % (1 << 27)) - (1 << 26);
    }
    dsp_resample_set_quality(RESAMPLE_Q_ALL);
    for (unsigned int k = 0; k < sizeof rates / sizeof rates[0]; k++)
    {
        for (int nch = 1; nch <= 2; nch++)
        {
            int a = run(rates[k][0], rates[k][1], nch, 0, out[0]);
            int b = run(rates[k][0], rates[k][1], nch, 1, out[1]);
            int n = MIN(a, b);
            int same = n > 0 && a == b;
            for (int c = 0; same && c < nch; c++)
                same = !memcmp(out[0][c], out[1][c], n * sizeof (int32_t));
            printf("%s %6u -> %5u %s: %d outputs (%d)\n", same ? "ok  " : "FAIL",
                   rates[k][0], rates[k][1], nch == 1 ? "mono  " : "stereo",
                   b, a);
            if (!same)
                for (int i = 0; i < n; i++)
                    if (out[0][0][i] != out[1][0][i])
                    {
                        printf("     first difference at output %d\n", i);
                        break;
                    }
            fail |= !same;
        }
    }
    return fail;
}
