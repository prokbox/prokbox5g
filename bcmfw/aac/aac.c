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
 * bcmfw/aac/aac.c: the adapter between bcmfw and Rockbox's libfaad
 * (see aac.h).  Built with aac_config.h, like the libfaad files.
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
#include "common.h"
#include "structs.h"
#include "decoder.h"
#include "syntax.h"
#include "aac.h"

static NeAACDecHandle dec;
static uint32_t aac_flags;
static struct aac_trace trace;

void aac_set_flags(uint32_t flags)
{
    aac_flags = flags;
}

/* ---- stage 9's parse checkpoints (aac.h AAC_T_*) ---------------------- */

void aac_trace_put(uint32_t tag, uint32_t value)
{
    if (!(aac_flags & AAC_FLAG_TRACE))
        return;
    if (trace.n < AAC_TRACE_PAIRS - 1)
    {
        trace.pair[trace.n][0] = tag;
        trace.pair[trace.n][1] = value;
        trace.n++;
    }
    else
    {
        /* the last pair counts what did not fit */
        trace.pair[AAC_TRACE_PAIRS - 1][0] = AAC_T_DROPPED;
        trace.pair[AAC_TRACE_PAIRS - 1][1]++;
        trace.n = AAC_TRACE_PAIRS;
    }
}

/* FNV-1a over bytes read one at a time (ldb: silicon-checked in SDRAM by
 * stage 9's memprobe), so the hash itself cannot share a fault with the
 * copy it checks */
void aac_trace_hash(uint32_t tag, const void *p, uint32_t bytes)
{
    const volatile uint8_t *b = p;
    uint32_t h = 2166136261u;

    if (!(aac_flags & AAC_FLAG_TRACE))
        return;
    for (; bytes != 0; bytes--)
        h = (h ^ *b++) * 16777619u;
    aac_trace_put(tag, h);
}

const struct aac_trace *aac_trace_get(void)
{
    return &trace;
}

/* ---- the decoder's memcpy and memset (aac_config.h, M3) -------------- */

/* words that may alias anything: the callers' types are libfaad's */
typedef uint32_t __attribute__((may_alias)) aac_word;
typedef uint16_t __attribute__((may_alias)) aac_half;

void *aac_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    uint32_t mis = ((uint32_t)(uintptr_t)d ^ (uint32_t)(uintptr_t)s) & 3;

    if (aac_flags & AAC_FLAG_BYTE_COPY)
        mis = 1;                        /* the byte loop below */
    if (mis == 0)
    {
        aac_word *dw;
        const aac_word *sw;

        while (n && ((uintptr_t)d & 3))
        {
            *d++ = *s++;
            n--;
        }
        dw = (aac_word *)d;
        sw = (const aac_word *)s;
        for (; n >= 32; n -= 32, dw += 8, sw += 8)
        {
            uint32_t a0 = sw[0], a1 = sw[1], a2 = sw[2], a3 = sw[3];
            uint32_t a4 = sw[4], a5 = sw[5], a6 = sw[6], a7 = sw[7];

            dw[0] = a0;
            dw[1] = a1;
            dw[2] = a2;
            dw[3] = a3;
            dw[4] = a4;
            dw[5] = a5;
            dw[6] = a6;
            dw[7] = a7;
        }
        for (; n >= 4; n -= 4)
            *dw++ = *sw++;
        d = (uint8_t *)dw;
        s = (const uint8_t *)sw;
    }
    else if (mis == 2)
    {
        /* halfword-aligned blocks (the second ic_stream's copy) */
        aac_half *dh;
        const aac_half *sh;

        if (n && ((uintptr_t)d & 1))
        {
            *d++ = *s++;
            n--;
        }
        dh = (aac_half *)d;
        sh = (const aac_half *)s;
        for (; n >= 16; n -= 16, dh += 8, sh += 8)
        {
            uint16_t a0 = sh[0], a1 = sh[1], a2 = sh[2], a3 = sh[3];
            uint16_t a4 = sh[4], a5 = sh[5], a6 = sh[6], a7 = sh[7];

            dh[0] = a0;
            dh[1] = a1;
            dh[2] = a2;
            dh[3] = a3;
            dh[4] = a4;
            dh[5] = a5;
            dh[6] = a6;
            dh[7] = a7;
        }
        for (; n >= 2; n -= 2)
            *dh++ = *sh++;
        d = (uint8_t *)dh;
        s = (const uint8_t *)sh;
    }
    while (n)
    {
        *d++ = *s++;
        n--;
    }
    return dst;
}

void *aac_memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint32_t v = (uint8_t)c;
    aac_word *w;

    v |= v << 8;
    v |= v << 16;
    if (aac_flags & AAC_FLAG_BYTE_COPY)
    {
        while (n)
        {
            *d++ = (uint8_t)v;
            n--;
        }
        return dst;
    }
    while (n && ((uintptr_t)d & 3))
    {
        *d++ = (uint8_t)v;
        n--;
    }
    w = (aac_word *)d;
    for (; n >= 64; n -= 64, w += 16)
    {
        w[0] = v;
        w[1] = v;
        w[2] = v;
        w[3] = v;
        w[4] = v;
        w[5] = v;
        w[6] = v;
        w[7] = v;
        w[8] = v;
        w[9] = v;
        w[10] = v;
        w[11] = v;
        w[12] = v;
        w[13] = v;
        w[14] = v;
        w[15] = v;
    }
    for (; n >= 4; n -= 4)
        *w++ = v;
    d = (uint8_t *)w;
    while (n)
    {
        *d++ = (uint8_t)v;
        n--;
    }
    return dst;
}

int aac_open(const uint8_t *asc, uint32_t len, uint32_t *rate,
             uint32_t *channels)
{
    NeAACDecConfigurationPtr conf;
    uint32_t r = 0;
    uint8_t c = 0;

    dec = NULL;
    if (asc == NULL || len < 2)
        return AAC_ERR_ARG;
    NeAACDecHandle d = NeAACDecOpen();
    /* as Rockbox's aac.c: the output format is not used (no conversion
     * inside libfaad) */
    conf = NeAACDecGetCurrentConfiguration(d);
    conf->outputFormat = FAAD_FMT_24BIT;
    NeAACDecSetConfiguration(d, conf);
    if (NeAACDecInit2(d, (uint8_t *)asc, len, &r, &c) != 0)
        return AAC_ERR_CONFIG;
    if (d->object_type != LC || c < 1 || c > 2)
        return AAC_ERR_CONFIG;
    if (rate)
        *rate = r;
    if (channels)
        *channels = c;
    dec = d;
    return 0;
}

static inline int16_t to_s16(int32_t x)
{
    /* Rockbox's sample_output_stereo at depth 29 (dsp_sample_io.c:
     * frac_bits 29, output_scale 14), half-bit bias */
    x = (x + (1 << 13)) >> 14;
    /* M3: one unsigned test for both limits, then 32767 or -32768 from
     * the sign */
    if ((uint32_t)(x + 32768) > 0xFFFF)
        x = (x >> 31) ^ 0x7FFF;
    return (int16_t)x;
}

int aac_decode(const uint8_t *au, uint32_t len, int16_t *pcm)
{
    NeAACDecFrameInfo info;
    const int32_t *l, *r;
    uint32_t n, i;

    trace.n = 0;
    trace.pair[AAC_TRACE_PAIRS - 1][1] = 0;
    if (dec == NULL)
        return AAC_ERR_STATE;
    if (au == NULL || pcm == NULL || len == 0 || len > AAC_MAX_AU)
        return AAC_ERR_ARG;
    if (NeAACDecDecode(dec, &info, (uint8_t *)au, len) == NULL || info.error)
        return info.error ? AAC_ERR_FAAD - (int)info.error : 0;
    if (info.channels < 1 || info.channels > 2)
        return AAC_ERR_FAAD - 12;
    n = info.samples / info.channels;
    if (n > AAC_FRAME_LEN)
        return AAC_ERR_FAAD - 12;
    l = dec->time_out[0];
    r = info.channels == 2 ? dec->time_out[1] : l;
    for (i = 0; i < n; i++)
    {
        pcm[2 * i] = to_s16(l[i]);
        pcm[2 * i + 1] = to_s16(r[i]);
    }
    return (int)n;
}

void aac_reset(void)
{
    int ch;

    if (dec == NULL)
        return;
    /* NeAACDecPostSeekReset only matters to SBR; for LC, clear the
     * overlap and window history and prime again, as after aac_open */
    for (ch = 0; ch < MAX_CHANNELS; ch++)
    {
        memset(dec->fb_intermed[ch], 0, FRAME_LEN * sizeof(real_t));
        dec->window_shape_prev[ch] = 0;
    }
    dec->frame = 0;
}

const int32_t *aac_raw_output(int ch)
{
    if (dec == NULL || ch < 0 || ch > 1)
        return NULL;
    return dec->time_out[ch];
}
