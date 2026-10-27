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
 * bcmfw video display path: tiling for the conversion and scaling
 * kernels (disp_kernels.S), and the scalar 2:1 box for sources the
 * vector scaler can't load.
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
#include "disp.h"

uint32_t disp_use_c;

static inline uint32_t clip8(int32_t q)
{
    return q < 0 ? 0 : q > 255 ? 255 : (uint32_t)q;
}

/* dispref.py, pixel by pixel */
static void convert_c(uint16_t *dst, uint32_t dpitch, const uint8_t *y,
                      const uint8_t *u, const uint8_t *v, uint32_t ystride,
                      uint32_t w, uint32_t h)
{
    uint32_t cs = ystride >> 1, r, x;

    for (r = 0; r < h; r++)
    {
        uint16_t *out = (uint16_t *)((uint8_t *)dst + r * dpitch);
        const uint8_t *yr = y + r * ystride;
        const uint8_t *ur = u + (r >> 1) * cs, *vr = v + (r >> 1) * cs;

        for (x = 0; x < w; x++)
        {
            int32_t yt = (yr[x] * 4769) >> 8;
            int32_t uu = ur[x >> 1], vv = vr[x >> 1];
            uint32_t R = clip8((yt + ((vv * 6538) >> 8) - 3567 + 8) >> 4);
            uint32_t G = clip8((yt + ((uu * -1604) >> 8)
                                + ((vv * -3330) >> 8) + 2169 + 8) >> 4);
            uint32_t B = clip8((yt + ((uu * 8262) >> 8) - 4429 + 8) >> 4);

            out[x] = (R >> 3) << 11 | (G >> 2) << 5 | (B >> 3);
        }
    }
}

void disp_convert(uint16_t *dst, uint32_t dpitch, const uint8_t *y,
                  const uint8_t *u, const uint8_t *v, uint32_t ystride,
                  uint32_t w, uint32_t h)
{
    uint32_t cs = ystride >> 1, ty, tx;

    /* a chroma stride that is not a multiple of 16 (a frame not 32 wide:
       176, 240) would move the tile kernel's vldp/vldq pairs' 16-byte line
       phase row by row, a form unknown on silicon that vc2sim's strict
       mode traps (alignment-audit.md): such frames take the C (exact
       either way; the merged image has no room for a padded copy) */
    if (disp_use_c || (cs & 15))
    {
        convert_c(dst, dpitch, y, u, v, ystride, w, h);
        return;
    }

    for (ty = 0; ty < h; ty += 16)
    {
        uint8_t *row = (uint8_t *)dst + ty * dpitch;
        const uint8_t *yr = y + ty * ystride;
        const uint8_t *ur = u + (ty >> 1) * cs, *vr = v + (ty >> 1) * cs;

        for (tx = 0; tx < w; tx += 32)
            disp_yuv_tile((uint16_t *)(row + 2 * tx), dpitch, yr + tx, ystride,
                          ur + (tx >> 1), vr + (tx >> 1));
    }
}

/* (a + b + c + d + 2) >> 2 over 2x2 blocks: n x rows outputs */
static void box2_c(uint8_t *dst, uint32_t dstride, const uint8_t *src,
                   uint32_t sstride, uint32_t n, uint32_t rows)
{
    uint32_t x, r;

    for (r = 0; r < rows; r++)
    {
        const uint8_t *a = src + 2 * r * sstride, *b = a + sstride;
        uint8_t *o = dst + r * dstride;

        for (x = 0; x < n; x++)
            o[x] = (a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2;
    }
}

void disp_scale2(uint16_t *dst, uint32_t dpitch, const uint8_t *y,
                 const uint8_t *u, const uint8_t *v, uint32_t ystride,
                 uint32_t ow, uint32_t oh, uint8_t *strip)
{
    uint32_t cs = ystride >> 1, sy, tx;
    uint8_t *sy8 = strip, *su8 = strip + 320 * 16, *sv8 = su8 + 160 * 8;
    int vec = !((( uint32_t)y | (uint32_t)u | (uint32_t)v | cs) & 31);

    for (sy = 0; sy < oh; sy += 16)
    {
        const uint8_t *ys = y + 2 * sy * ystride;
        const uint8_t *us = u + sy * cs, *vs = v + sy * cs;
        uint32_t rows = oh - sy < 16 ? oh - sy : 16;

        if (vec)
        {
            for (tx = 0; tx < ow; tx += 16)
                disp_box2_16(sy8 + tx, 320, ys + 2 * tx, ystride);
            for (tx = 0; tx < ow >> 1; tx += 16)
            {
                disp_box2_8(su8 + tx, 160, us + 2 * tx, cs);
                disp_box2_8(sv8 + tx, 160, vs + 2 * tx, cs);
            }
        }
        else
        {
            box2_c(sy8, 320, ys, ystride, ow, rows);
            box2_c(su8, 160, us, cs, ow >> 1, (rows + 1) >> 1);
            box2_c(sv8, 160, vs, cs, ow >> 1, (rows + 1) >> 1);
        }
        disp_convert((uint16_t *)((uint8_t *)dst + sy * dpitch), dpitch,
                     sy8, su8, sv8, 320, ow, rows);
    }
}
