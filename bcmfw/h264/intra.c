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
 * H.264 decoder: intra prediction (8.3.1.2 Intra_4x4, 8.3.3 Intra_16x16,
 * 8.3.4 chroma, 4:2:0).  Reference kernels: written for clarity first;
 * the hot ones (DC, vertical, horizontal) are short loops.
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

#if H264_VPU_INTRA
#include "vpu_kernels.h"
#define REF(name) name##_c
#else
#define REF(name) name
#endif

#if !H264_VPU_INTRA || H264_VPU_REF

/* n bytes (a multiple of 4, 4-aligned) of v */
static void fill_row(uint8_t *dst, int n, int v)
{
    uint32_t w = v * 0x01010101u;
    int i;
    for (i = 0; i < n; i += 4)
        *(uint32_t *)(dst + i) = w;
}

static void fill_block(uint8_t *dst, int stride, int n, int v)
{
    int i, j;
    uint32_t w = v * 0x01010101u;
    for (j = 0; j < n; j++)
        for (i = 0; i < n; i += 4)
            *(uint32_t *)(dst + j * stride + i) = w;
}

int REF(h264_pred4x4)(uint8_t *dst, int stride, int mode, int avail)
{
    /* t[0] = p[-1,-1], t[1..8] = p[0..7,-1]; l[0..3] = p[-1,0..3] */
    int t[9], l[4], x, y, s;
    const uint8_t *top = dst - stride;
    int has_l = avail & H264_AV_LEFT, has_t = avail & H264_AV_TOP;

    switch (mode)
    {
    case 0:                                       /* vertical */
        if (!has_t)
            return -1;
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                dst[y * stride + x] = top[x];
        return 0;
    case 1:                                       /* horizontal */
        if (!has_l)
            return -1;
        for (y = 0; y < 4; y++)
        {
            int v = dst[y * stride - 1];
            for (x = 0; x < 4; x++)
                dst[y * stride + x] = v;
        }
        return 0;
    case 2:                                       /* DC */
        s = 0;
        if (has_l && has_t)
        {
            for (x = 0; x < 4; x++)
                s += top[x] + dst[x * stride - 1];
            s = (s + 4) >> 3;
        }
        else if (has_l)
        {
            for (y = 0; y < 4; y++)
                s += dst[y * stride - 1];
            s = (s + 2) >> 2;
        }
        else if (has_t)
        {
            for (x = 0; x < 4; x++)
                s += top[x];
            s = (s + 2) >> 2;
        }
        else
            s = 128;
        fill_block(dst, stride, 4, s);
        return 0;
    }
    if (mode < 0 || mode > 8)
        return -1;

    /* the directional modes: gather the edge */
    if (has_t)
    {
        for (x = 0; x < 4; x++)
            t[1 + x] = top[x];
        if (avail & H264_AV_TOPRIGHT)
            for (x = 4; x < 8; x++)
                t[1 + x] = top[x];
        else
            for (x = 4; x < 8; x++)
                t[1 + x] = top[3];
    }
    if (has_l)
        for (y = 0; y < 4; y++)
            l[y] = dst[y * stride - 1];
    if (avail & H264_AV_TOPLEFT)
        t[0] = top[-1];

    switch (mode)
    {
    case 3:                                       /* diagonal down left */
        if (!has_t)
            return -1;
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
            {
                int v;
                if (x == 3 && y == 3)
                    v = (t[7] + 3 * t[8] + 2) >> 2;
                else
                    v = (t[1 + x + y] + 2 * t[2 + x + y] + t[3 + x + y] + 2) >> 2;
                dst[y * stride + x] = v;
            }
        return 0;
    case 7:                                       /* vertical left */
        if (!has_t)
            return -1;
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
            {
                int i = x + (y >> 1), v;
                if (!(y & 1))
                    v = (t[1 + i] + t[2 + i] + 1) >> 1;
                else
                    v = (t[1 + i] + 2 * t[2 + i] + t[3 + i] + 2) >> 2;
                dst[y * stride + x] = v;
            }
        return 0;
    case 8:                                       /* horizontal up */
        if (!has_l)
            return -1;
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
            {
                int z = x + 2 * y, i = y + (x >> 1), v;
                if (z > 5)
                    v = l[3];
                else if (z == 5)
                    v = (l[2] + 3 * l[3] + 2) >> 2;
                else if (!(z & 1))
                    v = (l[i] + l[i + 1] + 1) >> 1;
                else
                    v = (l[i] + 2 * l[i + 1] + l[i + 2 > 3 ? 3 : i + 2] + 2) >> 2;
                dst[y * stride + x] = v;
            }
        return 0;
    }

    /* modes 4, 5, 6 need left, top and top-left */
    if (!has_l || !has_t || !(avail & H264_AV_TOPLEFT))
        return -1;
    /* e[k]: the edge as one line, e[3] = p[-1,0] ... e[0] = p[-1,3],
     * e[4] = p[-1,-1], e[5..8] = p[0..3,-1] */
    {
        int e[9];
        for (y = 0; y < 4; y++)
            e[3 - y] = l[y];
        e[4] = t[0];
        for (x = 0; x < 4; x++)
            e[5 + x] = t[1 + x];
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
            {
                int v, z;
                if (mode == 4)                    /* diagonal down right */
                {
                    int c = 4 + x - y;            /* centre on the edge line */
                    v = (e[c - 1] + 2 * e[c] + e[c + 1] + 2) >> 2;
                }
                else if (mode == 5)               /* vertical right */
                {
                    z = 2 * x - y;
                    if (z >= 0 && !(z & 1))
                    {
                        int c = 5 + x - (y >> 1);
                        v = (e[c - 1] + e[c] + 1) >> 1;
                    }
                    else if (z >= 0)
                    {
                        int c = 5 + x - (y >> 1);
                        v = (e[c - 2] + 2 * e[c - 1] + e[c] + 2) >> 2;
                    }
                    else if (z == -1)
                        v = (e[3] + 2 * e[4] + e[5] + 2) >> 2;
                    else
                    {
                        int c = 4 - y;            /* p[-1,y-1], p[-1,y-2] .. */
                        v = (e[c] + 2 * e[c + 1] + e[c + 2] + 2) >> 2;
                    }
                }
                else                              /* 6: horizontal down */
                {
                    z = 2 * y - x;
                    if (z >= 0 && !(z & 1))
                    {
                        int c = 3 - y + (x >> 1);   /* p[-1, y - (x>>1)] */
                        v = (e[c + 1] + e[c] + 1) >> 1;
                    }
                    else if (z >= 0)
                    {
                        int c = 3 - y + (x >> 1);
                        v = (e[c + 2] + 2 * e[c + 1] + e[c] + 2) >> 2;
                    }
                    else if (z == -1)
                        v = (e[3] + 2 * e[4] + e[5] + 2) >> 2;
                    else
                    {
                        int c = 4 + x;            /* p[x-1,-1], p[x-2,-1] .. */
                        v = (e[c] + 2 * e[c - 1] + e[c - 2] + 2) >> 2;
                    }
                }
                dst[y * stride + x] = v;
            }
    }
    return 0;
}

int REF(h264_pred16x16)(uint8_t *dst, int stride, int mode, int avail)
{
    const uint8_t *top = dst - stride;
    int x, y, s;

    switch (mode)
    {
    case 0:                                       /* vertical */
        if (!(avail & H264_AV_TOP))
            return -1;
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x += 4)
                *(uint32_t *)(dst + y * stride + x) = *(const uint32_t *)(top + x);
        return 0;
    case 1:                                       /* horizontal */
        if (!(avail & H264_AV_LEFT))
            return -1;
        for (y = 0; y < 16; y++)
            fill_row(dst + y * stride, 16, dst[y * stride - 1]);
        return 0;
    case 2:                                       /* DC */
        s = 0;
        if ((avail & H264_AV_LEFT) && (avail & H264_AV_TOP))
        {
            for (x = 0; x < 16; x++)
                s += top[x] + dst[x * stride - 1];
            s = (s + 16) >> 5;
        }
        else if (avail & H264_AV_LEFT)
        {
            for (y = 0; y < 16; y++)
                s += dst[y * stride - 1];
            s = (s + 8) >> 4;
        }
        else if (avail & H264_AV_TOP)
        {
            for (x = 0; x < 16; x++)
                s += top[x];
            s = (s + 8) >> 4;
        }
        else
            s = 128;
        fill_block(dst, stride, 16, s);
        return 0;
    case 3:                                       /* plane */
    {
        int h = 0, v = 0, a, b, c;
        if ((avail & (H264_AV_LEFT | H264_AV_TOP | H264_AV_TOPLEFT)) !=
            (H264_AV_LEFT | H264_AV_TOP | H264_AV_TOPLEFT))
            return -1;
        for (x = 0; x < 8; x++)
        {
            h += (x + 1) * (top[8 + x] - top[6 - x]);
            v += (x + 1) * (dst[(8 + x) * stride - 1] - dst[(6 - x) * stride - 1]);
        }
        a = 16 * (dst[15 * stride - 1] + top[15]);
        b = (5 * h + 32) >> 6;
        c = (5 * v + 32) >> 6;
        for (y = 0; y < 16; y++)
        {
            int base = a + c * (y - 7) - 7 * b + 16;
            for (x = 0; x < 16; x++, base += b)
                dst[y * stride + x] = clip255(base >> 5);
        }
        return 0;
    }
    }
    return -1;
}

int REF(h264_pred_chroma8x8)(uint8_t *dst, int stride, int mode, int avail)
{
    const uint8_t *top = dst - stride;
    int has_l = avail & H264_AV_LEFT, has_t = avail & H264_AV_TOP;
    int x, y, k;

    switch (mode)
    {
    case 0:                                       /* DC, per 4x4 block */
        for (k = 0; k < 4; k++)
        {
            int bx = (k & 1) * 4, by = (k >> 1) * 4, st = 0, sl = 0, s;
            for (x = 0; x < 4; x++)
            {
                if (has_t)
                    st += top[bx + x];
                if (has_l)
                    sl += dst[(by + x) * stride - 1];
            }
            if (k == 0 || k == 3)
                s = has_l && has_t ? (st + sl + 4) >> 3
                  : has_t ? (st + 2) >> 2 : has_l ? (sl + 2) >> 2 : 128;
            else if (k == 1)
                s = has_t ? (st + 2) >> 2 : has_l ? (sl + 2) >> 2 : 128;
            else
                s = has_l ? (sl + 2) >> 2 : has_t ? (st + 2) >> 2 : 128;
            fill_block(dst + by * stride + bx, stride, 4, s);
        }
        return 0;
    case 1:                                       /* horizontal */
        if (!has_l)
            return -1;
        for (y = 0; y < 8; y++)
            fill_row(dst + y * stride, 8, dst[y * stride - 1]);
        return 0;
    case 2:                                       /* vertical */
        if (!has_t)
            return -1;
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x += 4)
                *(uint32_t *)(dst + y * stride + x) = *(const uint32_t *)(top + x);
        return 0;
    case 3:                                       /* plane */
    {
        int h = 0, v = 0, a, b, c;
        if (!has_l || !has_t || !(avail & H264_AV_TOPLEFT))
            return -1;
        for (x = 0; x < 4; x++)
        {
            h += (x + 1) * (top[4 + x] - top[2 - x]);
            v += (x + 1) * (dst[(4 + x) * stride - 1] - dst[(2 - x) * stride - 1]);
        }
        a = 16 * (dst[7 * stride - 1] + top[7]);
        b = (34 * h + 32) >> 6;
        c = (34 * v + 32) >> 6;
        for (y = 0; y < 8; y++)
        {
            int base = a + c * (y - 3) - 3 * b + 16;
            for (x = 0; x < 8; x++, base += b)
                dst[y * stride + x] = clip255(base >> 5);
        }
        return 0;
    }
    }
    return -1;
}
#endif /* C kernels */

#if H264_VPU_INTRA
/* The vector kernels (gen_vpu.py): availability is checked here, and
 * each mode and neighbour case has its own straight-line kernel. */
#define AV_LT (H264_AV_LEFT | H264_AV_TOP)

static h264_vpred_fn *const vdc4[4] = {
    h264_vpred4_dc_none, h264_vpred4_dc_l, h264_vpred4_dc_t, h264_vpred4_dc_lt
};
static h264_vpred_fn *const vdc16[4] = {
    h264_vpred16_dc_none, h264_vpred16_dc_l, h264_vpred16_dc_t,
    h264_vpred16_dc_lt
};
static h264_vpred_fn *const vdc8[4] = {
    h264_vpred8_dc_none, h264_vpred8_dc_l, h264_vpred8_dc_t, h264_vpred8_dc_lt
};

int h264_pred4x4(uint8_t *dst, int stride, int mode, int avail)
{
#if H264_VPU_RUNTIME
    if (H264_USE_C(H264_OFF_INTRA))
        return h264_pred4x4_c(dst, stride, mode, avail);
#endif
    const uint8_t *top = dst - stride;
    int has_l = avail & H264_AV_LEFT, has_t = avail & H264_AV_TOP;

    switch (mode)
    {
    case 0:
        if (!has_t)
            return -1;
        h264_vpred4_v(dst, stride, 0, 0);
        return 0;
    case 1:
        if (!has_l)
            return -1;
        h264_vpred4_h(dst, stride, 0, 0);
        return 0;
    case 2:
        vdc4[avail & AV_LT](dst, stride, 0, 0);
        return 0;
    case 3:                      /* e[9..13]: see gen_vpu.py */
    case 7:
        if (!has_t)
            return -1;
        if (avail & H264_AV_TOPRIGHT)
            (mode == 3 ? h264_vpred4_ddl : h264_vpred4_vl)(dst, stride,
                                                           top[7], 0x2000);
        else
            (mode == 3 ? h264_vpred4_ddl : h264_vpred4_vl)(dst, stride,
                                                           top[3], 0x3e00);
        return 0;
    case 4:
    case 5:
    case 6:
        if (!has_l || !has_t || !(avail & H264_AV_TOPLEFT))
            return -1;
        (mode == 4 ? h264_vpred4_ddr : mode == 5 ? h264_vpred4_vr
                   : h264_vpred4_hd)(dst, stride, 0, 0);
        return 0;
    case 8:
        if (!has_l)
            return -1;
        h264_vpred4_hu(dst, stride, dst[3 * stride - 1], 0);
        return 0;
    }
    return -1;
}

int h264_pred16x16(uint8_t *dst, int stride, int mode, int avail)
{
#if H264_VPU_RUNTIME
    if (H264_USE_C(H264_OFF_INTRA) ||
        (mode == 3 && H264_BAD(H264_BAD_PRED16_PLANE)))
        return h264_pred16x16_c(dst, stride, mode, avail);
#endif
    switch (mode)
    {
    case 0:
        if (!(avail & H264_AV_TOP))
            return -1;
        h264_vpred16_v(dst, stride, 0, 0);
        return 0;
    case 1:
        if (!(avail & H264_AV_LEFT))
            return -1;
        h264_vpred16_h(dst, stride, 0, 0);
        return 0;
    case 2:
        vdc16[avail & AV_LT](dst, stride, 0, 0);
        return 0;
    case 3:
        if ((avail & (AV_LT | H264_AV_TOPLEFT)) != (AV_LT | H264_AV_TOPLEFT))
            return -1;
        h264_vpred16_plane(dst, stride, 0, 0);
        return 0;
    }
    return -1;
}

int h264_pred_chroma8x8(uint8_t *dst, int stride, int mode, int avail)
{
#if H264_VPU_RUNTIME
    if (H264_USE_C(H264_OFF_INTRA))
        return h264_pred_chroma8x8_c(dst, stride, mode, avail);
#endif
    switch (mode)
    {
    case 0:
        vdc8[avail & AV_LT](dst, stride, 0, 0);
        return 0;
    case 1:
        if (!(avail & H264_AV_LEFT))
            return -1;
        h264_vpred8_h(dst, stride, 0, 0);
        return 0;
    case 2:
        if (!(avail & H264_AV_TOP))
            return -1;
        h264_vpred8_v(dst, stride, 0, 0);
        return 0;
    case 3:
        if ((avail & (AV_LT | H264_AV_TOPLEFT)) != (AV_LT | H264_AV_TOPLEFT))
            return -1;
        h264_vpred8_plane(dst, stride, 0, 0);
        return 0;
    }
    return -1;
}
#endif
