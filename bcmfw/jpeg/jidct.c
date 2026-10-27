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

/* jdec's IDCTs, block by block (bcmfw/tests/jidct_test.py): the vector
 * kernel (jhook.h jdec_idct, jv_idct4n) against the decoder's own C
 * (jdec_idct_c: jpeg_idct4v/4h, 8v/8h, 16v/16h), built with the decoder.
 *   - In vc2sim, jt_blocks(): n blocks of 64 coefficients (natural order,
 *     F[v][u] at 8 v + u), four at a time through jv_idct4n at size (4, 8
 *     or 16 points) and one at a time through the C, size * size pixels
 *     each (row-major) to vec and ref; jt_check(), the start-up check's
 *     verdict.
 *   - In vc2sim, jt_put(): the put and the fill (jv_put, jv_fill) against
 *     jdec_put_c and jdec_fill_c, jpeg_load.c's C put as it was.
 *   - On the Mac, with JIDCT_DUMP: the blocks the decoder sends the hook
 *     while it decodes FILE at W x H, written to stdout as int16, the size
 *     then the 64 coefficients (natural order), and made by the C so the
 *     decode goes on.
 * Test scaffolding, not in any image. */
#include <stdint.h>
#include <string.h>
#include "jenv.h"
#include "jhook.h"
#include "jdec.h"
#ifdef __vc4__
#include "jdec_vpu.h"
#endif

/* block q of coef (JDEC_IDCT_AT's places) in natural order */
static void natural(const int16_t *coef, int q, int16_t *ws)
{
    int i;

    for (i = 0; i < 64; i++)
        ws[i] = coef[JDEC_IDCT_AT(q) + 16 * (i >> 3) + (i & 7)];
}

/* the C's size * size pixels of block q (pixel (n, k) at 64 n + 4 k) to
   out, row-major */
static void ref_block(const int16_t *coef, int q, int size, uint8_t *out)
{
    int16_t ws[64];
    uint8_t px[16 * 64];
    int i;

    natural(coef, q, ws);
    jdec_idct_c(size, ws, px, 64);
    for (i = 0; i < size * size; i++)
        out[i] = px[64 * (i / size) + 4 * (i % size)];
}

/* jpeg_load.c's put and fill as they were in C (jhook.h jdec_put,
   jdec_fill): the vector kernels' reference here, and the Mac dump's
   hooks */
void jdec_put_c(const uint8_t *px, unsigned char *const *to, int n, int rowstep,
                int size, int grey)
{
    int q, y, x, c;

    for (q = 0; q < n; q++)
        for (y = 0; y < size; y++)
            for (x = 0; x < size; x++)
                for (c = 0; c <= 2 * grey; c++)
                    to[q][y * rowstep + 4 * x + c] = px[JDEC_IDCT_PX(size, q, y, x)];
}

void jdec_fill_c(unsigned int v, unsigned char *out, int grey, int rowstep,
                 int size)
{
    int y, x, c;

    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++)
            for (c = 0; c <= 2 * grey; c++)
                out[y * rowstep + 4 * x + c] = v;
}

#ifdef __vc4__
void jv_idct4n(uint8_t *px, int16_t *coef, const uint32_t *d);
void jv_put(const uint8_t *px, unsigned char *const *to, int n, int rowstep,
            int size, int grey);
void jv_fill(unsigned int v, unsigned char *out, int grey, int rowstep, int size);

/* The put (v < 0), or the fill of v + q: n blocks at size from px to vec + offs[q]
   by the kernel, and to ref + offs[q] by the C (vec and ref: two copies
   of a row buffer, 32-byte aligned) */
void jt_put(const uint8_t *px, uint8_t *vec, uint8_t *ref, const uint32_t *offs,
            int n, int rowstep, int size, int grey, int v)
{
    unsigned char *tv[4], *tr[4];
    int q;

    for (q = 0; q < n; q++)
    {
        tv[q] = vec + offs[q];
        tr[q] = ref + offs[q];
    }
    if (v < 0)
    {
        jv_put(px, tv, n, rowstep, size, grey);
        jdec_put_c(px, tr, n, rowstep, size, grey);
    }
    else
        for (q = 0; q < n; q++)
        {
            jv_fill(v + q, tv[q], grey, rowstep, size);
            jdec_fill_c(v + q, tr[q], grey, rowstep, size);
        }
}
extern const uint32_t jv_idct_d4[4], jv_idct_d8[4], jv_idct_d16[4];

/* the start-up check's verdict (jdec_vpu_init): 1 passed, 0 failed */
int jt_check(void)
{
    return jdec_vpu_init(1, 0);
}

/* 0, or 1 + the first group whose coefficients the kernel didn't leave 0
   (the kernel called directly, whatever the start-up check said) */
int jt_blocks(const int16_t *blocks, int n, uint8_t *vec, uint8_t *ref, int size)
{
    static int16_t coef[JDEC_IDCT_COEF] __attribute__((aligned(32)));
    static uint8_t px[JDEC_IDCT_PX_SIZE] __attribute__((aligned(32)));
    int ss = size * size, b, q, i;

    for (b = 0; b < n; b += 4)
    {
        for (q = 0; q < 4 && b + q < n; q++)
            for (i = 0; i < 64; i++)
                coef[JDEC_IDCT_AT(q) + 16 * (i >> 3) + (i & 7)] =
                    blocks[64 * (b + q) + i];
        for (q = 0; q < 4 && b + q < n; q++)
            ref_block(coef, q, size, ref + ss * (b + q));
        jv_idct4n(px, coef, size == 4 ? jv_idct_d4 : size == 8 ? jv_idct_d8
                                                               : jv_idct_d16);
        for (q = 0; q < 4 && b + q < n; q++)
            for (i = 0; i < ss; i++)
                vec[ss * (b + q) + i] = px[JDEC_IDCT_PX(size, q, i / size, i % size)];
        for (i = 0; i < JDEC_IDCT_COEF; i++)
            if (coef[i])
                return 1 + b / 4;
    }
    return 0;
}
#endif

#ifdef JIDCT_DUMP
#include <stdio.h>
#include <stdlib.h>

int jdec_one(const uint8_t *jpg, uint32_t len, uint16_t *out, uint32_t dw,
             uint32_t dh, void *work, uint32_t work_bytes);

static void dump(uint8_t *px, int16_t *coef, int size)
{
    uint8_t pix[256];
    int16_t blk[65];
    int q, i;

    for (q = 0; q < 4; q++)
    {
        blk[0] = size;
        natural(coef, q, blk + 1);
        fwrite(blk, 2, 65, stdout);
        ref_block(coef, q, size, pix);
        for (i = 0; i < size * size; i++)
            px[JDEC_IDCT_PX(size, q, i / size, i % size)] = pix[i];
    }
    memset(coef, 0, JDEC_IDCT_COEF * sizeof(*coef));
}

/* FILE W H: the blocks to stdout (with the batch's unused places, all 0) */
int main(int argc, char **argv)
{
    FILE *f;
    long len;
    uint8_t *jpg;
    uint16_t *out;
    void *work;

    if (argc != 4 || !(f = fopen(argv[1], "rb")))
        return 2;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    jpg = malloc(len);
    if (fread(jpg, 1, len, f) != (size_t)len)
        return 2;
    fclose(f);
    out = malloc(2u * atoi(argv[2]) * atoi(argv[3]));
    work = malloc(8u << 20);
    jdec_idct = dump;
    jdec_put = jdec_put_c;
    jdec_fill = jdec_fill_c;
    return jdec_one(jpg, len, out, atoi(argv[2]), atoi(argv[3]), work, 8u << 20) ? 1 : 0;
}
#endif
