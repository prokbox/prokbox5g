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

/* The visualiser FFT's definition (vfft.h), as plain integer C: no
 * floats, no allocation, no library calls, so the same file builds for
 * the Mac, the PP and the BCM's scalar core (vc2cc).  It computes exactly
 * what the vector kernel computes (vfft_vpu.c runs the kernel instruction
 * by instruction and the tests require the two to agree on every bit),
 * but as ordinary loops over a 16 x 16 block, so it reads as the maths.
 *
 * Conventions, as the vector unit has them (isa-vector.md):
 *   - a lane holds 16 bits: every add and subtract wraps (w16);
 *   - vmulhd is the high half of a 16 x 16 product, floored (mulhd);
 *   - the accumulator sums products exactly and is read as its high half,
 *     floored (acc_hi); silicon's accumulator saturates at +-2^31
 *     (quirks #31, #32), so acc_hi saturates too (no frame reaches it:
 *     the window keeps every sum far below);
 *   - an arithmetic shift right floors.
 * The block is [row][lane]: element j of a 256-point sequence sits at row
 * j >> 4, lane j & 15. */
#include "vfft.h"

/* a lane's 16 bits */
static int32_t w16(int32_t v)
{
    return (int16_t)(uint16_t)(uint32_t)v;
}

/* vmulhd: (a b) >> 16, floored */
static int32_t mulhd(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 16);
}

/* the accumulator's high half of an exact sum, saturated to 32 bits as
 * silicon's accumulator is */
static int32_t acc_hi(int64_t s)
{
    if (s > INT32_MAX)
        s = INT32_MAX;
    if (s < INT32_MIN)
        s = INT32_MIN;
    return w16((int32_t)(s >> 16));
}

static uint32_t brev4(uint32_t i)
{
    return (i & 1) << 3 | (i & 2) << 1 | (i & 4) >> 1 | (i & 8) >> 3;
}

typedef int16_t blk[VFFT_ROWS][16];

/* b = d W_16^e / 2 (a butterfly's lower output): e 0 is a shift, e 4
 * (W = -i) a swap and a negation, any other e four products a component
 * summed exactly and floored once (Q15 twiddles: the floor of d w / 2^16
 * is d W / 2, so the stage's halving comes free).  Both modes: the
 * twiddles are immediates, and those chains are silicon-proven forms */
static void twiddle(int16_t *br, int16_t *bi, int32_t dr, int32_t di, uint32_t e,
                    const struct vfft_tabs *t)
{
    int32_t wr, wi;

    if (e == 0)
    {
        *br = (int16_t)(dr >> 1);
        *bi = (int16_t)(di >> 1);
    }
    else if (e == 4)
    {
        *br = (int16_t)(di >> 1);
        *bi = (int16_t)(w16(-dr) >> 1);
    }
    else
    {
        wr = t->w16[2 * e];
        wi = t->w16[2 * e + 1];
        *br = (int16_t)acc_hi((int64_t)dr * wr + (int64_t)di * -wi);
        *bi = (int16_t)acc_hi((int64_t)dr * wi + (int64_t)di * wr);
    }
}

/* One 16-point radix-2 decimation-in-frequency transform down the rows
 * of x, in every lane at once: stages h = 8, 4, 2 halve (a' = (a + b) / 2,
 * b' = (a - b) W / 2), the last (h = 1) writes a + b and a - b to y's rows
 * in bit-reversed order, so y is in natural order; halved (>> 1) when
 * last_half, else not (step 1 leaves that halving to step 2's twiddle) */
static void dif16(blk xr, blk xi, blk yr, blk yi, int last_half,
                  const struct vfft_tabs *t)
{
    uint32_t h, g, j, l, i, a, b;
    int32_t dr, di, s;

    for (h = 8; h >= 2; h >>= 1)
        for (g = 0; g < 16; g += 2 * h)
            for (j = 0; j < h; j++)
            {
                a = g + j;
                b = g + j + h;
                for (l = 0; l < 16; l++)
                {
                    dr = w16(xr[a][l] - xr[b][l]);
                    di = w16(xi[a][l] - xi[b][l]);
                    xr[a][l] = (int16_t)(w16(xr[a][l] + xr[b][l]) >> 1);
                    xi[a][l] = (int16_t)(w16(xi[a][l] + xi[b][l]) >> 1);
                    twiddle(&xr[b][l], &xi[b][l], dr, di, j * (8 / h), t);
                }
            }
    s = last_half ? 1 : 0;
    for (i = 0; i < 16; i += 2)
        for (l = 0; l < 16; l++)
        {
            yr[brev4(i)][l] = (int16_t)(w16(xr[i][l] + xr[i + 1][l]) >> s);
            yi[brev4(i)][l] = (int16_t)(w16(xi[i][l] + xi[i + 1][l]) >> s);
            yr[brev4(i + 1)][l] = (int16_t)(w16(xr[i][l] - xr[i + 1][l]) >> s);
            yi[brev4(i + 1)][l] = (int16_t)(w16(xi[i][l] - xi[i + 1][l]) >> s);
        }
}

void vfft_ref_x(int16_t *xr, int16_t *xi, const int16_t *pcm,
                const struct vfft_tabs *t, enum vfft_mode mode)
{
    blk zr, zi, yr, yi;
    uint32_t r, l, a, b, pa, pb;
    int32_t wr, wi, s, d, tt, u;

    /* the window: z[j] = (x[2j] w) >> 16 + i (x[2j+1] w') >> 16 */
    for (r = 0; r < 16; r++)
        for (l = 0; l < 16; l++)
        {
            zr[r][l] = (int16_t)mulhd(pcm[16 * r + l], t->win[16 * r + l]);
            zi[r][l] = (int16_t)mulhd(pcm[VFFT_M + 16 * r + l],
                                      t->win[VFFT_M + 16 * r + l]);
        }
    /* step 1: down the rows (n1 = row) for each lane (n2 = lane): y at
       row k2, lane n2 */
    dif16(zr, zi, yr, yi, 0, t);
    /* step 2: y times W_256^(n2 k2); the floor of y w / 2^16 is y W / 2,
       step 1's last halving */
    for (r = 0; r < 16; r++)
        for (l = 0; l < 16; l++)
        {
            int32_t ar = yr[r][l], ai = yi[r][l];

            wr = t->tw[16 * r + l];
            wi = t->tw[VFFT_M + 16 * r + l];
            if (mode == VFFT_EXACT)
            {
                zr[r][l] = (int16_t)acc_hi((int64_t)ar * wr + (int64_t)ai * -wi);
                zi[r][l] = (int16_t)acc_hi((int64_t)ar * wi + (int64_t)ai * wr);
            }
            else
            {
                zr[r][l] = (int16_t)w16(mulhd(ar, wr) - mulhd(ai, wi));
                zi[r][l] = (int16_t)w16(mulhd(ar, wi) + mulhd(ai, wr));
            }
        }
    /* the transpose: row n2, lane k2 */
    for (r = 0; r < 16; r++)
        for (l = 0; l < 16; l++)
        {
            yr[l][r] = zr[r][l];
            yi[l][r] = zi[r][l];
        }
    /* step 3: down the rows again (n2), halving: Z[k2 + 16 k1] at row k1,
       lane k2, which is natural order (bin k at row k >> 4, lane k & 15) */
    dif16(yr, yi, zr, zi, 1, t);
    /* the split: Z's partner Z[256 - k] is at row 15 - a, lane 16 - b,
       except in lane 0 (b = 0), where it is row (16 - a) & 15, lane 0 */
    for (a = 0; a < 16; a++)
        for (b = 0; b < 16; b++)
        {
            pa = b ? 15 - a : (16 - a) & 15;
            pb = (16 - b) & 15;
            s = w16(zr[a][b] + zr[pa][pb]);
            d = w16(zr[a][b] - zr[pa][pb]);
            tt = w16(zi[a][b] + zi[pa][pb]);
            u = w16(zi[a][b] - zi[pa][pb]);
            wr = t->ws[16 * a + b];
            wi = t->ws[VFFT_M + 16 * a + b];
            if (mode == VFFT_EXACT)
            {
                /* X = (S 2^15 + Wr T + Wi D) / 2^16 + i (U 2^15 + Wi T -
                   Wr D) / 2^16, each one exact sum floored once */
                xr[16 * a + b] = (int16_t)acc_hi((int64_t)s * 32768 +
                                                 (int64_t)tt * wr + (int64_t)d * wi);
                xi[16 * a + b] = (int16_t)acc_hi((int64_t)u * 32768 +
                                                 (int64_t)tt * wi + (int64_t)d * -wr);
            }
            else
            {
                /* the same terms, each floored on its own */
                xr[16 * a + b] = (int16_t)w16((s >> 1) + mulhd(tt, wr) + mulhd(d, wi));
                xi[16 * a + b] = (int16_t)w16((u >> 1) + mulhd(tt, wi) - mulhd(d, wr));
            }
        }
}

void vfft_ref(uint16_t *raw, const int16_t *pcm, const struct vfft_tabs *t,
              enum vfft_mode mode)
{
    int16_t xr[VFFT_M], xi[VFFT_M];
    uint32_t k;
    int64_t p;

    vfft_ref_x(xr, xi, pcm, t, mode);
    /* the power, exact in the 32-bit accumulator (saturating, as silicon's:
       only X = (-32768, -32768) could reach it, and no frame does) */
    for (k = 0; k < VFFT_M; k++)
    {
        p = (int64_t)xr[k] * xr[k] + (int64_t)xi[k] * xi[k];
        if (p > INT32_MAX)
            p = INT32_MAX;
        raw[k] = (uint16_t)p;
        raw[VFFT_M + k] = (uint16_t)(p >> 16);
    }
}

void vfft_power(uint32_t *p, const uint16_t *raw)
{
    uint32_t k;

    for (k = 0; k < VFFT_M; k++)
        p[k] = raw[k] | (uint32_t)raw[VFFT_M + k] << 16;
}

uint32_t vfft_l2(uint32_t p, const struct vfft_tabs *t)
{
    uint32_t e = 31, f;

    if (p < 2)
        return 0;
    while (!(p >> e))
        e--;
    f = e >= 4 ? p >> (e - 4) : p << (4 - e);
    return 16 * e + t->log16[f & 15];
}

void vfft_bands(uint16_t *l2, const uint16_t *raw, const uint16_t *e,
                uint32_t nb, const struct vfft_tabs *t)
{
    uint32_t b, k, hi, top, p;

    for (b = 0; b < nb; b++)
    {
        top = 0;
        hi = e[b + 1] < VFFT_M ? e[b + 1] : VFFT_M;
        for (k = e[b]; k < hi; k++)
        {
            p = raw[k] | (uint32_t)raw[VFFT_M + k] << 16;
            if (p > top)
                top = p;
        }
        l2[b] = (uint16_t)vfft_l2(top, t);
    }
}
