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
 * iPod UI shell: the blurred-cover fill behind a fitted thumbnail
 * (see artblur.h)
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
#include <stddef.h>
#include <stdbool.h>

#include "artblur.h"

/* Lines are blurred in a buffer padded by PAD clamped samples on the left
 * and PAD + 1 on the right, so the sliding window never tests an edge. */
#define PAD             ARTBLUR_R1
#define RECIP(r)        ((65536u + (2u * (r) + 1) / 2) / (2u * (r) + 1))
#define MUL0            RECIP(ARTBLUR_R0)
#define MUL1            RECIP(ARTBLUR_R1)
#define MUL0_DARK       (MUL0 * ARTBLUR_DARK / 256)

/* 4x4 ordered dither, 0..15: the dark result never exceeds 154 + 7, so
 * adding it to the 8-bit channels before truncating to 5/6 bits never
 * overflows */
static const uint8_t bayer[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};

static void pad(uint8_t *a, int n)
{
    int i;

    for (i = 0; i < PAD; i++)
        a[i] = a[PAD];
    for (i = 0; i <= PAD; i++)
        a[PAD + n + i] = a[PAD + n - 1];
}

/* out[PAD + i] = the mean of in[PAD + i - r .. PAD + i + r], scaled by
 * mul / 65536 (mul ~ 65536 / (2r + 1)) */
static void box(const uint8_t *in, uint8_t *out, int n, int r, uint32_t mul)
{
    const uint8_t *lo = in + PAD - r, *hi = in + PAD + r + 1;
    uint32_t s = 0;
    int i;

    for (i = -r; i <= r; i++)
        s += in[PAD + i];
    out += PAD;
    for (i = 0; i < n; i++)
    {
        out[i] = (uint8_t)((s * mul + 0x8000) >> 16);
        s += *hi++;                 /* modular: the sum stays exact */
        s -= *lo++;
    }
}

/* n samples 'step' apart, in place: three passes (R0, R1, R0), the last
 * one scaled by mul_last instead of MUL0 */
static void blur_line(uint8_t *p, int n, int step, uint8_t *a, uint8_t *b,
                      uint32_t mul_last)
{
    const uint8_t *lo, *hi;
    uint32_t s = 0;
    int i;

    for (i = 0; i < n; i++)
        a[PAD + i] = p[i * step];
    pad(a, n);
    box(a, b, n, ARTBLUR_R0, MUL0);
    pad(b, n);
    box(b, a, n, ARTBLUR_R1, MUL1);
    pad(a, n);
    /* the last pass straight back to the plane */
    lo = a + PAD - ARTBLUR_R0;
    hi = a + PAD + ARTBLUR_R0 + 1;
    for (i = -ARTBLUR_R0; i <= ARTBLUR_R0; i++)
        s += a[PAD + i];
    for (i = 0; i < n; i++, p += step)
    {
        *p = (uint8_t)((s * mul_last + 0x8000) >> 16);
        s += *hi++;
        s -= *lo++;
    }
}

/* the three planes' line at p (planes n apart) */
static void blur_rgb(uint8_t *p, int n, int len, int step, uint8_t *a,
                     uint8_t *b, uint32_t mul_last)
{
    blur_line(p, len, step, a, b, mul_last);
    blur_line(p + n, len, step, a, b, mul_last);
    blur_line(p + 2 * n, len, step, a, b, mul_last);
}

void artblur_fill(uint16_t *dst, int W, int H,
                  const uint16_t *src, int sw, int sh, uint8_t *scratch,
                  void (*pause)(void))
{
    const int n = W * H, m = W > H ? W : H;
    uint8_t *pr = scratch, *pg = pr + n, *pb = pg + n;
    uint8_t *a = pb + n, *b = a + m + 2 * PAD + 1;
    /* the art's place (artcache_fit_rect(): centred) */
    const int x0 = (W - sw) / 2, x1 = x0 + sw;
    const int y0 = (H - sh) / 2, y1 = y0 + sh;
    const bool tall = sw < W;
    uint32_t xs, ys, fy;
    int x, y, i;

    if (W <= 0 || H <= 0 || sw <= 0 || sh <= 0 || sw > W || sh > H)
        return;

    /* 1: stretch (nearest; the blur hides it) into 8-bit planes */
    xs = ((uint32_t)sw << 16) / (uint32_t)W;
    ys = ((uint32_t)sh << 16) / (uint32_t)H;
    fy = ys / 2;
    for (y = 0, i = 0; y < H; y++, fy += ys)
    {
        const uint16_t *row = src + (fy >> 16) * sw;
        uint32_t fx = xs / 2;

        for (x = 0; x < W; x++, i++, fx += xs)
        {
            unsigned c = row[fx >> 16];
            unsigned r5 = c >> 11, g6 = (c >> 5) & 0x3f, b5 = c & 0x1f;

            pr[i] = (uint8_t)((r5 << 3) | (r5 >> 2));
            pg[i] = (uint8_t)((g6 << 2) | (g6 >> 4));
            pb[i] = (uint8_t)((b5 << 3) | (b5 >> 2));
        }
    }

    if (pause)
        pause();

    /* 2: along the bars first, every line, then across them only on the
     * lines the bars cover (darkened on that last pass): tall art, rows
     * then the side columns; wide art, columns then the top and bottom
     * rows. Separable, so the order does not change the result. */
    if (tall)
    {
        for (y = 0; y < H; y++)
            blur_rgb(pr + y * W, n, W, 1, a, b, MUL0);
        if (pause)
            pause();
        for (x = 0; x < W; x++)
            if (x < x0 || x >= x1)
                blur_rgb(pr + x, n, H, W, a, b, MUL0_DARK);
    }
    else
    {
        for (x = 0; x < W; x++)
            blur_rgb(pr + x, n, H, W, a, b, MUL0);
        if (pause)
            pause();
        for (y = 0; y < H; y++)
            if (y < y0 || y >= y1)
                blur_rgb(pr + y * W, n, W, 1, a, b, MUL0_DARK);
    }

    if (pause)
        pause();

    /* 3: the bars back to RGB565, dithered (the art covers the rest) */
    for (y = 0, i = 0; y < H; y++)
    {
        const uint8_t *d = bayer + ((y & 3) << 2);
        const bool row_in = y >= y0 && y < y1;

        for (x = 0; x < W; x++, i++)
        {
            unsigned t = d[x & 3];

            if (row_in && x == x0)
            {
                x = x1 - 1;         /* skip the art's span */
                i += x1 - x0 - 1;
                continue;
            }

            dst[i] = (uint16_t)((((pr[i] + (t >> 1)) >> 3) << 11)
                                | (((pg[i] + (t >> 2)) >> 2) << 5)
                                | ((pb[i] + (t >> 1)) >> 3));
        }
    }
}
