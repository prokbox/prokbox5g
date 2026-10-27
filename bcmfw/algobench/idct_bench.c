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

/* The reference for vpu_islow.py: jpeg_load.c's own 8x8 islow
 * (jpeg_idct8v then jpeg_idct8h, the order and the transposed
 * workspace jdec runs), on blocks in natural order (F[v][u] at 8v + u).
 * Built on the Mac (ground truth) and by vc2cc (the scalar instruction
 * count per block in vc2sim).  Research scaffolding, not in any image. */
#include "../../apps/recorder/jpeg_load.c"

/* n blocks of 64 coefficients (natural order) -> n * 64 pixels (row-major) */
void idct8_blocks(const int16_t *in, unsigned char *out, int n)
{
    static int16_t ws[IDCT_WS_SIZE];
    static jpeg_pix_t pix[64];
    int b, k;

    for (b = 0; b < n; b++, in += 64, out += 64)
    {
        /* transposed storage, as huff_block writes it (zag + 0):
           column c, vertical frequency j at 8c + j */
        for (k = 0; k < 64; k++)
            ws[8 * (k & 7) + (k >> 3)] = in[k];
        jpeg_idct8v(ws, ws + 64);
        jpeg_idct8h(ws + 64, (unsigned char *)pix, ws + 128, 8 * JPEG_PIX_SZ);
        for (k = 0; k < 64; k++)
            out[k] = ((unsigned char *)pix)[k * JPEG_PIX_SZ];
    }
}

/* idct8_blocks' copies alone: its harness cost, to subtract */
void idct8_copies(const int16_t *in, unsigned char *out, int n)
{
    static int16_t ws[IDCT_WS_SIZE];
    static jpeg_pix_t pix[64];
    int b, k;

    for (b = 0; b < n; b++, in += 64, out += 64)
    {
        for (k = 0; k < 64; k++)
            ws[8 * (k & 7) + (k >> 3)] = in[k];
        asm volatile ("" : : "r" (ws), "r" (pix) : "memory");
        for (k = 0; k < 64; k++)
            out[k] = ((unsigned char *)pix)[k * JPEG_PIX_SZ];
    }
}

#ifdef IDCT_BENCH_MAIN
#include <stdio.h>
/* stdin: int16 blocks; stdout: 64 bytes each */
int main(void)
{
    int16_t blk[64];
    unsigned char px[64];

    while (fread(blk, 2, 64, stdin) == 64)
    {
        idct8_blocks(blk, px, 1);
        fwrite(px, 1, 64, stdout);
    }
    return 0;
}
#endif
