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

/* jdec's vector kernels (jdec_kernels.S, gen_jdec.py), bit for bit the
 * decoder's C:
 *   - the colour stage (jhook.h jdec_yuv565): YCbCr to dithered RGB565 by
 *     jv_yuv16, 16 pixels a call (algorithms.md section 4);
 *   - the 4-, 8- and 16-point IDCTs (jhook.h jdec_idct): jv_idct4n, four
 *     blocks a call, through the 32-bit accumulator (algorithms.md
 *     section 1), and the put and the DC fill into the row buffer
 *     (jdec_put, jdec_fill: jv_put, jv_fill), installed only if the
 *     start-up check agrees with the C at every size.
 * Built against bcmfw/jpeg/env with the decoder, in the display-only image
 * and tests/jpeg_test.py's vc2sim build; jdec_vpu_init() installs them. */
#include <stdint.h>
#include <string.h>
#include "jenv.h"
#include "bmp.h"
#include "jhook.h"
#include "jdec_vpu.h"
#include "jpeg_common.h"   /* yuv_to_rgb: the colour check's C */

void jv_yuv16(uint16_t *dst, const struct uint8_rgb *src, const uint16_t *delta);
void jv_idct4n(uint8_t *px, int16_t *coef, const uint32_t *d);
void jv_put(const uint8_t *px, unsigned char *const *to, int n, int rowstep,
            int size, int grey);
void jv_fill(unsigned int v, unsigned char *out, int grey, int rowstep, int size);
extern const uint32_t jv_idct_d4[4], jv_idct_d8[4], jv_idct_d16[4];

/* the row's 16 dither offsets (for dither_dy), a 32-byte aligned row of
   16 results for a destination jv_yuv16 can't store to (unaligned, or a
   last part), and the 16 pixels split into Y, Cb and Cr planes at +0,
   +32 and +64 (the kernel loads each with one aligned vld8) */
static uint16_t delta[16] __attribute__((aligned(32)));
static uint32_t part[8] __attribute__((aligned(32)));
static uint8_t planes[96] __attribute__((aligned(32)));
static unsigned int dither_dy = ~0u;

/* the decoder's pixels (Y in byte 0, Cb in 1, Cr in 2 of each 4) into the
   planes: whole words, src 4-byte aligned (jpeg_load.c's row buffer and
   resize.c's px[] are) */
static void split16(const struct uint8_rgb *src)
{
    const uint32_t *w = (const uint32_t *)(const void *)src;
    int i;

    for (i = 0; i < 16; i++)
    {
        uint32_t v = w[i];

        planes[i] = (uint8_t)v;
        planes[32 + i] = (uint8_t)(v >> 8);
        planes[64 + i] = (uint8_t)(v >> 16);
    }
}

static void yuv565(fb_data *dst, const struct uint8_rgb *src, int n,
                   unsigned int dy)
{
    int i;

    if (dy != dither_dy)
    {
        for (i = 0; i < 16; i++)
            delta[i] = dither_table[i] ^ dy;
        dither_dy = dy;
    }
    for (; n > 0; n -= 16, dst += 16, src += 16)
    {
        split16(src);
        if (n >= 16 && !((uintptr_t)dst & 31))
            jv_yuv16(dst, (const struct uint8_rgb *)planes, delta);
        else
        {
            jv_yuv16((uint16_t *)part, (const struct uint8_rgb *)planes, delta);
            if (n >= 16 && !((uintptr_t)dst & 3))
                for (i = 0; i < 8; i++)
                    ((uint32_t *)dst)[i] = part[i];
            else
                for (i = 0; i < n && i < 16; i++)
                    dst[i] = ((uint16_t *)part)[i];
        }
    }
}

/* jdec_idct: the kernel with the size's descriptor (gen_jdec.py) */
static const uint32_t *idct_d(int size)
{
    return size == 4 ? jv_idct_d4 : size == 8 ? jv_idct_d8 : jv_idct_d16;
}

static void idct(uint8_t *px, int16_t *coef, int size)
{
    jv_idct4n(px, coef, idct_d(size));
}

/* The start-up check, at each size: four blocks of known coefficients
   through jv_idct4n and jv_put into a row buffer, and through the C
   (jdec_idct_c, the decoder's C IDCT, which writes the row buffer itself)
   into its twin; every byte compared, and the coefficients left 0.  Then
   jv_fill against a C fill of the same blocks.  Coefficient i
   (JDEC_IDCT_AT's place) of block 0 is a hash of i, any int16 (outside the
   4-point's 4 x 4 too: both ignore those); block 1's are the extremes
   -32768 and 32767 (the hash's sign), but its column 0 is all 32767:
   the 16-point's output 0 (every M[0][j] > 0) sums past 2^31 there, where
   C wraps and silicon's accumulator saturates (quirks #31, #32), so a
   kernel whose partial sums can leave 32 bits fails the check.  Blocks 2 and 3 have one column,
   whose pass-1 output 0 is exactly 16 << 11 and 16 << 11 - 1 before the
   shift, rounding term included, so a rounding term one off either way
   moves a row of pixels.  fail (tests only): the check fails whatever it
   finds */
static const int16_t idct_probe[3][2][8] = {
    { { 0, 1, 1, -1, 0, 3, 0, 0 }, { 4, 0, -1, -1, 0, 3, 0, 0 } },        /* 8 */
    { { -12, 25, -13, -7 }, { 12, 4, 11, -45 } },                          /* 4 */
    { { 1, 1, 2, 0, -1, 0, 0, 0 }, { -1, 1, 1, -1, 0, 1, 0, 2 } },        /* 16 */
};

static int __attribute__((noinline)) idct_coef(int si, int i)
{
    int q = (i >> 6 & 2) | (i >> 3 & 1);        /* JDEC_IDCT_AT's block */
    int v = (int16_t)((uint32_t)i * 2654435761u >> 12);

    if (q > 1)
        return i & 7 ? 0 : idct_probe[si][q - 2][i >> 4 & 7];
    if (q && !(i & 7))                          /* block 1's column 0 */
        return 0x7FFF;
    return q ? (v >> 15) ^ 0x7FFF : v;
}

/* The check's row buffer: CHK_ROWS rows of CHK_STEP bytes, every byte
   CHK_PAT(i) in word i before.  chk_at: the blocks' places by size (8, 4, 16), the
   low two bits the component.  8 points: Y and Cb in one line (a merged
   put), two more below, components 2 and 0; 16: Y and Cr at one place,
   Cb and the fourth byte at the next, so every byte left over must stay
   as it was; 4 points as the decoder's luma, two to a line.  The fill:
   the same places, blocks 2 and 3 grey at 4 and 8 points */
#define CHK_STEP    128
#define CHK_ROWS    16
#define CHK_PAT(i)  ((uint32_t)(i) * 2654435761u)     /* word i */
static const uint16_t chk_at[3][4] = {
    { 0, 1, 8 * CHK_STEP + 2 + 32, 8 * CHK_STEP + 64 },
    { 0, 16, 4 * CHK_STEP, 4 * CHK_STEP + 16 }, { 0, 64 + 1, 2, 64 + 3 } };

struct chk
{
    int16_t coef[JDEC_IDCT_COEF];
    uint8_t px[JDEC_IDCT_PX_SIZE];
    uint8_t vec[CHK_ROWS * CHK_STEP], ref[CHK_ROWS * CHK_STEP];
    unsigned char *to[4];
    int16_t ws[64];
} __attribute__((aligned(32)));

static int __attribute__((noinline)) chk_same(const uint8_t *a, const uint8_t *b)
{
    const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
    uint32_t d = 0;
    int i;

    for (i = 0; i < CHK_ROWS * CHK_STEP / 4; i++)
        d |= x[i] ^ y[i];
    return !d;
}

/* the C's put of one block to out: its pixels from src (block q's first,
   JDEC_IDCT_PX), or (src NULL) v */
static void __attribute__((noinline)) chk_put(uint8_t *out, const uint8_t *src,
                                              int v, int size, int grey)
{
    int y, x, c;

    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++)
            for (c = 0; c <= 2 * grey; c++)             /* grey: bytes 0-2 */
                out[CHK_STEP * y + 4 * x + c] = src ? src[32 * x + 2 * y] : v;
}

/* one size of the check (si: 0, 1, 2 for 8, 4 and 16 points) */
static int __attribute__((noinline)) chk_size(struct chk *k, int si)
{
    int size = si == 1 ? 4 : 8 << (si >> 1), q, i, g, at, ok;

    for (i = 0; i < JDEC_IDCT_COEF; i++)
        k->coef[i] = idct_coef(si, i);
    for (i = 0; i < CHK_ROWS * CHK_STEP / 4; i++)
        ((uint32_t *)k->vec)[i] = ((uint32_t *)k->ref)[i] = CHK_PAT(i);
    idct(k->px, k->coef, size);
    for (q = 0; q < 4; q++)
    {
        k->to[q] = k->vec + chk_at[si][q];
        for (i = 0; i < 64; i++)                /* F[v][u] at 8 v + u */
        {
            at = JDEC_IDCT_AT(q) + 16 * (i >> 3) + (i & 7);
            k->ws[i] = idct_coef(si, at);
            if (k->coef[at])
                return 0;
        }
        jdec_idct_c(size, k->ws, k->ref + chk_at[si][q], CHK_STEP);
    }
    jv_put(k->px, k->to, 4, CHK_STEP, size, 0);
    ok = chk_same(k->vec, k->ref);
    for (q = 0; q < 4; q++)
    {
        g = si != 2 && q > 1;                   /* 4 and 8 points: two grey */
        at = chk_at[si][q] & ~(3 * g);
        jv_fill(0xA5 ^ q, k->vec + at, g, CHK_STEP, size);
        chk_put(k->ref + at, NULL, 0xA5 ^ q, size, g);
    }
    return ok & chk_same(k->vec, k->ref);
}

static int idct_check(int fail)
{
    struct chk k;
    int si, ok = !fail;

    for (si = 0; si < 3; si++)
        ok &= chk_size(&k, si);
    return ok;
}

/* The colour stage's start-up check: 64 pixels (the corners of the YCbCr
   cube, the grey axis, and an LCG's), at two dither rows, through yuv565
   (whole blocks and a 5-pixel last part) against the C path's own
   arithmetic (jpeg_common.h yuv_to_rgb, then resize.c's dither and the
   RGB565 pack); every pixel compared */
#define YC_N    64
static int yuv_check(void)
{
    static struct uint8_rgb src[YC_N] __attribute__((aligned(4)));
    static const uint8_t edge[8][3] =
    {
        { 0, 0, 0 }, { 255, 255, 255 }, { 0, 255, 0 }, { 255, 0, 255 },
        { 128, 128, 128 }, { 16, 240, 16 }, { 235, 16, 240 }, { 255, 0, 0 },
    };
    uint16_t out[YC_N];
    uint32_t s = 0x2722;
    unsigned int r, g, b, d, dy;
    int i, k, ok = 1;

    for (i = 0; i < YC_N; i++)
    {
        s = s * 1103515245u + 12345u;
        src[i].blue = i < 8 ? edge[i][0] : i < 24 ? (uint8_t)(i * 11) : (uint8_t)(s >> 24);
        src[i].green = i < 8 ? edge[i][1] : i < 24 ? 128 : (uint8_t)(s >> 16);
        src[i].red = i < 8 ? edge[i][2] : i < 24 ? 128 : (uint8_t)(s >> 8);
        src[i].alpha = 0;
    }
    for (k = 0; k < 2; k++)
    {
        dy = DITHERY(k ? 5 : 0);
        dither_dy = ~0u;
        yuv565((fb_data *)out, src, YC_N - 3 * k, dy);
        for (i = 0; i < YC_N - 3 * k; i++)
        {
            yuv_to_rgb(src[i].blue, src[i].green, src[i].red, &r, &g, &b);
            d = DITHERXDY(i, dy);
            r = (31 * r + (r >> 3) + d) >> 8;
            g = (63 * g + (g >> 2) + d) >> 8;
            b = (31 * b + (b >> 3) + d) >> 8;
            ok &= out[i] == (uint16_t)(r << 11 | g << 5 | b);
        }
    }
    dither_dy = ~0u;
    return ok;
}

static int yuv_on;

int jdec_vpu_yuv(void)
{
    return yuv_on;
}

int jdec_vpu_init(int idct_on, int fail)
{
    yuv_on = yuv_check();
    jdec_yuv565 = yuv_on ? yuv565 : NULL;
    jdec_idct = idct_on && idct_check(fail) ? idct : NULL;
    jdec_put = jv_put;          /* used only with jdec_idct */
    jdec_fill = jv_fill;
    return jdec_idct != NULL;
}
