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
 * H.264 decoder: fractional sample interpolation kernels (8.4.2.2):
 * luma quarter-sample with the 6-tap filter (1, -5, 20, 20, -5, 1), and
 * chroma eighth-sample bilinear.  Each luma phase is built from the
 * three half-sample planes (b: horizontal, h: vertical, j: centre) and
 * the integer samples, averaged as the standard's equations say.
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

#if H264_VPU_MC
#include "vpu_kernels.h"
#define REF(name) name##_c
#else
#define REF(name) name
#endif

#if !H264_VPU_MC || H264_VPU_REF

#define TAP6(a, b, c, d, e, f) ((a) - 5 * (b) + 20 * (c) + 20 * (d) - 5 * (e) + (f))

/* b: horizontal half samples between src[x] and src[x + 1] */
static void hpel_h(uint8_t *dst, int ds, const uint8_t *src, int ss, int w, int h)
{
    int x, y;
    for (y = 0; y < h; y++, dst += ds, src += ss)
        for (x = 0; x < w; x++)
            dst[x] = clip255((TAP6(src[x - 2], src[x - 1], src[x], src[x + 1],
                                   src[x + 2], src[x + 3]) + 16) >> 5);
}

/* h: vertical half samples between rows y and y + 1 */
static void hpel_v(uint8_t *dst, int ds, const uint8_t *src, int ss, int w, int h)
{
    int x, y;
    for (y = 0; y < h; y++, dst += ds, src += ss)
        for (x = 0; x < w; x++)
            dst[x] = clip255((TAP6(src[x - 2 * ss], src[x - ss], src[x],
                                   src[x + ss], src[x + 2 * ss],
                                   src[x + 3 * ss]) + 16) >> 5);
}

/* j: the centre half sample, from unrounded horizontal intermediates */
static void hpel_hv(uint8_t *dst, int ds, const uint8_t *src, int ss, int w, int h)
{
    int16_t t[21 * 16];          /* rows -2 .. h+2 */
    int x, y;
    const uint8_t *s = src - 2 * ss;
    for (y = 0; y < h + 5; y++, s += ss)
        for (x = 0; x < w; x++)
            t[y * 16 + x] = TAP6(s[x - 2], s[x - 1], s[x], s[x + 1], s[x + 2],
                                 s[x + 3]);
    for (y = 0; y < h; y++, dst += ds)
        for (x = 0; x < w; x++)
        {
            const int16_t *c = t + (y + 2) * 16 + x;
            dst[x] = clip255((TAP6(c[-32], c[-16], c[0], c[16], c[32], c[48])
                              + 512) >> 10);
        }
}

static void avg2(uint8_t *dst, int ds, const uint8_t *a, int as,
                 const uint8_t *b, int bs, int w, int h)
{
    int x, y;
    for (y = 0; y < h; y++, dst += ds, a += as, b += bs)
        for (x = 0; x < w; x++)
            dst[x] = (a[x] + b[x] + 1) >> 1;
}

void REF(h264_mc_luma)(uint8_t *dst, int dstride, const uint8_t *src,
                       int sstride, int w, int h, int fx, int fy)
{
    uint8_t t1[16 * 16], t2[16 * 16];
    int y;

    switch (fy << 2 | fx)
    {
    case 0:                      /* G */
        for (y = 0; y < h; y++)
        {
            int x;
            for (x = 0; x < w; x++)
                dst[y * dstride + x] = src[y * sstride + x];
        }
        return;
    case 1:                      /* a = (G + b + 1) >> 1 */
    case 3:                      /* c = (H + b + 1) >> 1 */
        hpel_h(t1, 16, src, sstride, w, h);
        avg2(dst, dstride, t1, 16, src + (fx >> 1), sstride, w, h);
        return;
    case 2:                      /* b */
        hpel_h(dst, dstride, src, sstride, w, h);
        return;
    case 4:                      /* d = (G + h + 1) >> 1 */
    case 12:                     /* n = (M + h + 1) >> 1 */
        hpel_v(t1, 16, src, sstride, w, h);
        avg2(dst, dstride, t1, 16, src + (fy >> 1) * sstride, sstride, w, h);
        return;
    case 8:                      /* h */
        hpel_v(dst, dstride, src, sstride, w, h);
        return;
    case 10:                     /* j */
        hpel_hv(dst, dstride, src, sstride, w, h);
        return;
    case 6:                      /* f = (b + j + 1) >> 1 */
    case 14:                     /* q = (j + s + 1) >> 1 */
        hpel_hv(t1, 16, src, sstride, w, h);
        hpel_h(t2, 16, src + (fy >> 1) * sstride, sstride, w, h);
        avg2(dst, dstride, t1, 16, t2, 16, w, h);
        return;
    case 9:                      /* i = (h + j + 1) >> 1 */
    case 11:                     /* k = (j + m + 1) >> 1 */
        hpel_hv(t1, 16, src, sstride, w, h);
        hpel_v(t2, 16, src + (fx >> 1), sstride, w, h);
        avg2(dst, dstride, t1, 16, t2, 16, w, h);
        return;
    default:                     /* e, g, p, r: (b or s) + (h or m) */
        hpel_h(t1, 16, src + (fy >> 1) * sstride, sstride, w, h);
        hpel_v(t2, 16, src + (fx >> 1), sstride, w, h);
        avg2(dst, dstride, t1, 16, t2, 16, w, h);
        return;
    }
}

void REF(h264_mc_chroma)(uint8_t *dst, int dstride, const uint8_t *src,
                         int sstride, int w, int h, int fx, int fy)
{
    int a = (8 - fx) * (8 - fy), b = fx * (8 - fy), c = (8 - fx) * fy,
        d = fx * fy, x, y;
    if (!(fx | fy))              /* whole sample: a copy */
    {
        for (y = 0; y < h; y++, dst += dstride, src += sstride)
            for (x = 0; x < w; x++)
                dst[x] = src[x];
        return;
    }
    if (!fy || !fx)              /* one direction: (8 - f) A + f B, (x + 4) >> 3 */
    {
        int f = fx | fy, o = fx ? 1 : sstride;
        for (y = 0; y < h; y++, dst += dstride, src += sstride)
            for (x = 0; x < w; x++)
                dst[x] = ((8 - f) * src[x] + f * src[x + o] + 4) >> 3;
        return;
    }
    for (y = 0; y < h; y++, dst += dstride, src += sstride)
        for (x = 0; x < w; x++)
            dst[x] = (a * src[x] + b * src[x + 1] + c * src[x + sstride]
                      + d * src[x + sstride + 1] + 32) >> 6;
}
#endif /* C kernels */

#if H264_VPU_MC
/* The vector kernels (gen_vpu.py): one per block height and phase; they
 * compute 16 lanes and store the w the mask enables. */
static h264_vmc_fn *const vmc_luma[3][16] = H264_VMC_LUMA_TABLE;
static h264_vmc_fn *const vmc_chroma[3][4] = H264_VMC_CHROMA_TABLE;

void h264_mc_luma(uint8_t *dst, int dstride, const uint8_t *src, int sstride,
                  int w, int h, int fx, int fy)
{
#if H264_VPU_RUNTIME
    if (H264_USE_C(H264_OFF_MC) || H264_BAD(H264_BAD_MC_LUMA))
    {
        h264_mc_luma_c(dst, dstride, src, sstride, w, h, fx, fy);
        return;
    }
#endif
    vmc_luma[h >> 3][fy << 2 | fx](dst, dstride, src, sstride, (1 << w) - 1, 0);
}

void h264_mc_chroma(uint8_t *dst, int dstride, const uint8_t *src,
                    int sstride, int w, int h, int fx, int fy)
{
#if H264_VPU_RUNTIME
    if (H264_USE_C(H264_OFF_MC))
    {
        h264_mc_chroma_c(dst, dstride, src, sstride, w, h, fx, fy);
        return;
    }
#endif
    vmc_chroma[h >> 2][(fx != 0) | (fy != 0) << 1](dst, dstride, src, sstride,
                                                   (1 << w) - 1, fx | fy << 8);
}
#endif
