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
 * H.264 decoder: inverse transforms and DC scaling (8.5.10 to 8.5.12),
 * flat scaling matrices only (Baseline).
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

#if H264_VPU_IDCT
#define REF(name) name##_c
#else
#define REF(name) name
#endif

static inline int16_t sat16(int32_t v)
{
    return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

#if !H264_VPU_IDCT || H264_VPU_REF
void REF(h264_idct4_add)(uint8_t *dst, int stride, int16_t *c)
{
    int t[16], i;

    for (i = 0; i < 4; i++)              /* rows */
    {
        const int16_t *d = c + 4 * i;
        int e0 = d[0] + d[2], e1 = d[0] - d[2];
        int e2 = (d[1] >> 1) - d[3], e3 = d[1] + (d[3] >> 1);
        t[4 * i + 0] = e0 + e3;
        t[4 * i + 1] = e1 + e2;
        t[4 * i + 2] = e1 - e2;
        t[4 * i + 3] = e0 - e3;
    }
    for (i = 0; i < 4; i++)              /* columns */
    {
        int g0 = t[i] + t[8 + i], g1 = t[i] - t[8 + i];
        int g2 = (t[4 + i] >> 1) - t[12 + i], g3 = t[4 + i] + (t[12 + i] >> 1);
        dst[i] = clip255(dst[i] + ((g0 + g3 + 32) >> 6));
        dst[stride + i] = clip255(dst[stride + i] + ((g1 + g2 + 32) >> 6));
        dst[2 * stride + i] = clip255(dst[2 * stride + i] + ((g1 - g2 + 32) >> 6));
        dst[3 * stride + i] = clip255(dst[3 * stride + i] + ((g0 - g3 + 32) >> 6));
    }
    for (i = 0; i < 16; i++)
        c[i] = 0;
}

void REF(h264_idct4_dc_add)(uint8_t *dst, int stride, int16_t *c)
{
    int v = (c[0] + 32) >> 6, i, j;
    c[0] = 0;
    for (j = 0; j < 4; j++, dst += stride)
        for (i = 0; i < 4; i++)
            dst[i] = clip255(dst[i] + v);
}

#endif /* C transforms */

int h264_luma_dc_dequant(int16_t *coef, const int16_t *dc, int qp)
{
    int t[16], i, mask = 0;
    int ls = 16 * h264_dequant4[h264_qp_mod6[qp]][0], q6 = h264_qp_div6[qp];

    for (i = 0; i < 4; i++)              /* rows */
    {
        const int16_t *c = dc + 4 * i;
        int a = c[0] + c[1], b = c[0] - c[1], e = c[2] + c[3], f = c[2] - c[3];
        t[4 * i + 0] = a + e;            /* rows of the 4x4 Hadamard */
        t[4 * i + 1] = a - e;
        t[4 * i + 2] = b - f;
        t[4 * i + 3] = b + f;
    }
    for (i = 0; i < 4; i++)              /* columns */
    {
        int a = t[i] + t[4 + i], b = t[i] - t[4 + i];
        int e = t[8 + i] + t[12 + i], f = t[8 + i] - t[12 + i];
        int f4[4], j;
        f4[0] = a + e;
        f4[1] = a - e;
        f4[2] = b - f;
        f4[3] = b + f;
        for (j = 0; j < 4; j++)
        {
            int v;
            if (q6 >= 6)
                v = f4[j] * ls * (1 << (q6 - 6));
            else
                v = (f4[j] * ls + (1 << (5 - q6))) >> (6 - q6);
            v = sat16(v);
            coef[(4 * j + i) * 16] = v;
            if (v)
                mask |= 1 << (4 * j + i);
        }
    }
    return mask;
}

int h264_chroma_dc_dequant(int16_t *coef, const int16_t *dc, int qp)
{
    int ls = 16 * h264_dequant4[h264_qp_mod6[qp]][0], q6 = h264_qp_div6[qp];
    int f[4], k, mask = 0;

    f[0] = dc[0] + dc[1] + dc[2] + dc[3];
    f[1] = dc[0] - dc[1] + dc[2] - dc[3];
    f[2] = dc[0] + dc[1] - dc[2] - dc[3];
    f[3] = dc[0] - dc[1] - dc[2] + dc[3];
    for (k = 0; k < 4; k++)
    {
        int v = sat16((f[k] * ls * (1 << q6)) >> 5);
        coef[k * 16] = v;
        if (v)
            mask |= 1 << k;
    }
    return mask;
}
