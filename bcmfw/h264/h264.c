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
 * H.264 decoder: the arena, NAL units (7.3.1, Annex B), the bit reader's
 * slow paths, and the public entry points.
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
#include "h264_int.h"

uint32_t h264_refused_at;
uint32_t h264_error_at;

#define ALIGN(x, a) (((x) + (a) - 1) & ~(size_t)((a) - 1))

/* ---- Bit reader slow paths ------------------------------------------- */

uint32_t bs_word_tail(const uint8_t *buf, uint32_t pos, uint32_t size)
{
    uint32_t w = 0;
    int k;
    for (k = 0; k < 4; k++)
        w = w << 8 | (pos + k < size ? buf[pos + k] : 0);
    return w;
}

uint32_t bs_get_long(struct h264_bits *b, int k)
{
    uint32_t hi;
    if (k <= 25)
        return bs_get(b, k);
    hi = bs_get(b, k - 16);
    return hi << 16 | bs_get(b, 16);
}

/* ue(v) with more than 12 leading zeros.  Values above 2^32 - 2 can't be
 * represented: err is set and 0 returned. */
uint32_t bs_ue_slow(struct h264_bits *b)
{
    int lz = 0;
    while (!bs_get1(b))
    {
        if (++lz > 31 || bs_overrun(b))
        {
            b->err = 1;
            return 0;
        }
    }
    if (lz == 0)
        return 0;
    return ((1u << lz) - 1) + bs_get_long(b, lz);
}

/* ---- Arena -------------------------------------------------------------- */

static size_t frames_for(int num_ref_frames)
{
    return (num_ref_frames < 1 ? 1 : num_ref_frames) + 1;
}

size_t h264_ctx_size(int mb_w, int mb_h)
{
    size_t mbs = (size_t)mb_w * mb_h;
    return ALIGN(sizeof(struct h264_dec), 16) + 32
         + ALIGN(mbs * sizeof(struct h264_mbinfo), 16)
         + ALIGN(mbs * 16 * 2 * sizeof(int16_t), 16);
}

#if H264_VPU_RUNTIME
uint32_t h264_vpu_off;          /* H264_OFF_*: families run in C */
uint32_t h264_vpu_bad;      /* none since dev29: every kernel fitted */
#endif

size_t h264_frames_size(int mb_w, int mb_h, int num_ref_frames)
{
    return frames_for(num_ref_frames) * (size_t)H264_PAD_MBW(mb_w) * mb_h * 384
         + 64;                   /* the vector kernels read up to 32 bytes
                                    past a row: slack after the last plane */
}

size_t h264_arena_size(int mb_w, int mb_h, int num_ref_frames)
{
    return h264_ctx_size(mb_w, mb_h) + 32      /* frames 32-byte aligned */
         + h264_frames_size(mb_w, mb_h, num_ref_frames);
}

struct h264_dec *h264_open(void *arena, size_t size)
{
    uint8_t *p = arena;
    size_t pad = ALIGN((size_t)p, 32) - (size_t)p;   /* coef: 32-byte rows */
    struct h264_dec *d;

    if (!arena || size < pad + sizeof(struct h264_dec))
        return NULL;
    d = (struct h264_dec *)(p + pad);
    memset(d, 0, sizeof(*d));
    d->arena = p;
    d->arena_size = size;
    return d;
}

int h264_set_frames(struct h264_dec *d, void *frames, size_t size)
{
    if (!d || !frames || ((size_t)frames & 31))
        return H264_E_NOMEM;
    d->fbuf = frames;
    d->fbuf_size = size;
    d->layout_mb_w = d->layout_mb_h = d->layout_refs = 0;
    d->nframes = 0;
    return 0;
}

const void *h264_frame_base(const struct h264_dec *d)
{
    return d && d->nframes ? d->frames[0].y : NULL;
}

void h264_frames_arena_first(struct h264_dec *d, int on)
{
    if (d)
        d->arena_first = on;
}

/* Lay the picture buffers out for an SPS (called at an IDR). */
int h264_layout(struct h264_dec *d, const struct h264_sps *sps)
{
    size_t mbs = (size_t)sps->mb_w * sps->mb_h;
    size_t fmbs = (size_t)H264_PAD_MBW(sps->mb_w) * sps->mb_h;  /* a frame's */
    int nf = frames_for(sps->num_ref_frames);
    uint8_t *p, *end, *f0, *fend;
    int k;

    if (d->layout_mb_w == sps->mb_w && d->layout_mb_h == sps->mb_h &&
        d->layout_refs == sps->num_ref_frames)
        return 0;
    d->layout_mb_w = d->layout_mb_h = d->layout_refs = 0;
    d->nframes = 0;
    p = (uint8_t *)ALIGN((size_t)d + sizeof(*d), 16);
    end = d->arena + d->arena_size;
    d->mbi = (struct h264_mbinfo *)p;
    p += ALIGN(mbs * sizeof(struct h264_mbinfo), 16);
    d->mvs = (int16_t (*)[16][2])p;
    p += ALIGN(mbs * 16 * 2 * sizeof(int16_t), 16);
    if (d->fbuf && !(d->arena_first &&
                     ALIGN((size_t)p, 32) - (size_t)d->arena +
                     h264_frames_size(sps->mb_w, sps->mb_h,
                                      sps->num_ref_frames) <= d->arena_size))
    {
        f0 = d->fbuf;
        fend = d->fbuf + d->fbuf_size;
        if (p > end || h264_frames_size(sps->mb_w, sps->mb_h,
                                        sps->num_ref_frames) > d->fbuf_size)
            return H264_E_NOMEM;
    }
    else
    {
        f0 = (uint8_t *)ALIGN((size_t)p, 32);    /* as h264_set_frames */
        fend = end;
        if (p > end || (size_t)(f0 - d->arena) +
            h264_frames_size(sps->mb_w, sps->mb_h, sps->num_ref_frames) >
            d->arena_size)
            return H264_E_NOMEM;
    }
    p = f0;
    for (k = 0; k < nf; k++)
    {
        struct h264_frame *f = &d->frames[k];
        memset(f, 0, sizeof(*f));
        f->id = k;
        f->y = p;
        f->u = p + fmbs * 256;
        f->v = p + fmbs * 320;
        p += fmbs * 384;
    }
    if (p > fend)
        return H264_E_NOMEM;
    d->nframes = nf;
    d->mb_w = sps->mb_w;
    d->mb_h = sps->mb_h;
    d->ls = H264_PAD_MBW(sps->mb_w) * 16;
    d->cs = d->ls / 2;
    for (k = 0; k < 24; k++)
        d->blk_off[k] = k < 16 ? (k >> 2) * 4 * d->ls + (k & 3) * 4
                               : ((k >> 1) & 1) * 4 * d->cs + (k & 1) * 4;
    d->mbs = (int)mbs;
    d->mb.addr = -2;             /* mb_setup: no previous macroblock */
    d->layout_mb_w = sps->mb_w;
    d->layout_mb_h = sps->mb_h;
    d->layout_refs = sps->num_ref_frames;
    return 0;
}

/* ---- NAL units ------------------------------------------------------------ */

/* Remove emulation_prevention_three_byte (7.4.1) in place; new length. */
static int unescape(uint8_t *p, int len)
{
    int i = 0, o, zeros = 0;

    /* fast scan up to the first 00 00 03 */
    while (i + 2 < len)
    {
        if (p[i + 2] > 3)
            i += 3;
        else if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 3)
            break;
        else
            i++;
    }
    if (i + 2 >= len)
        return len;
    o = i;
    for (; i < len; i++)
    {
        uint8_t c = p[i];
        if (zeros >= 2 && c == 3)
        {
            zeros = 0;
            continue;
        }
        zeros = c ? 0 : zeros + 1;
        p[o++] = c;
    }
    return o;
}

int h264_decode_nal(struct h264_dec *d, uint8_t *nal, int len)
{
    struct h264_bits b;
    int type, ref_idc, r, last, orig = len;

    if (!d || !nal || len < 1)
        return H264_ERR(1, H264_E_BITSTREAM);
    if (nal == d->again_nal && len == d->again_len)
        len = d->again_rbsp;     /* re-submitted: already unescaped */
    else
        len = unescape(nal, len);
    d->again_nal = NULL;
    if (nal[0] & 0x80)
        r = H264_ERR(1, H264_E_BITSTREAM);   /* forbidden_zero_bit */
    else
    {
        ref_idc = nal[0] >> 5 & 3;
        type = nal[0] & 31;
        /* the RBSP stop bit: everything before it is syntax */
        last = len - 1;
        while (last > 0 && nal[last] == 0)
            last--;
        bs_init(&b, nal + 1, last);
        d->rbsp_bits = last > 0 ? last * 8 - 1 - __builtin_ctz(nal[last]) : 0;
        switch (type)
        {
        case 1:
        case 5:
            r = h264_decode_slice(d, &b, type, ref_idc);
            if (r == H264_FRAME_AGAIN)
            {
                d->again_nal = nal;      /* the caller passes it again */
                d->again_len = orig;
                d->again_rbsp = len;
            }
            break;
        case 7:
            r = h264_parse_sps(d, &b);
            break;
        case 8:
            r = h264_parse_pps(d, &b);
            break;
        case 2: case 3: case 4:   /* data partitioning (Extended profile) */
            r = H264_REFUSE(1);
            break;
        default:                  /* SEI, AUD, end of sequence, filler... */
            r = H264_OK;
            break;
        }
    }
    if (r < 0)
        d->stats.errors++;
    return r;
}

int h264_flush(struct h264_dec *d)
{
    if (!d || !d->in_picture)
        return H264_OK;
    return h264_finish_picture(d);
}

const struct h264_picture *h264_frame(const struct h264_dec *d)
{
    return &d->out;
}

uint32_t h264_ps_valid(const struct h264_dec *d, uint32_t *sps)
{
    uint32_t k, p = 0, s = 0;

    for (k = 0; k != H264_MAX_PPS; k++)
        if (d->pps[k].valid)
            p |= 1u << k;
    for (k = 0; k != H264_MAX_SPS; k++)
        if (d->sps[k].valid)
            s |= 1u << k;
    *sps = s;
    return p;
}

const struct h264_stats *h264_stats(const struct h264_dec *d)
{
    return &d->stats;
}

int h264_annexb_next(const uint8_t *buf, int len, int pos,
                     int *nal, int *nal_len, int *next)
{
    int i = pos < 0 ? 0 : pos, start, end;

    /* the next start code prefix 00 00 01 */
    for (;;)
    {
        if (i + 3 > len)
            return 0;
        if (buf[i + 2] > 1)
            i += 3;
        else if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
            break;
        else
            i++;
    }
    start = i + 3;
    for (i = start; i + 3 <= len; )
    {
        if (buf[i + 2] > 1)
            i += 3;
        else if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] <= 1)
            break;
        else
            i++;
    }
    end = i + 3 <= len ? i : len;
    *next = end;
    while (end > start && buf[end - 1] == 0)
        end--;                   /* trailing_zero_8bits */
    *nal = start;
    *nal_len = end - start;
    return 1;
}
