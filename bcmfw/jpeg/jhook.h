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

/* bcmfw/jpeg/jhook.h: what the shell's decoder (apps/recorder/jpeg_load.c)
 * and resizer (resize.c) offer jdec (bcmfw/jobs/jdec.c) when they are
 * built with JPEG_JDEC (jpeg-spike.md sections 8 and 9).  The shell's own
 * build doesn't define it, and is unchanged; its twin of jdec will.
 *   - a bad Huffman code fails the decode (JPEG_E_BADHUFF) instead of
 *     decoding as a zero;
 *   - the rows tap: every row the decoder makes, before its own output
 *     takes it, so one decoder pass feeds several outputs;
 *   - resize_push: resize.c's scalers fed one source row at a time (the
 *     same arithmetic as resize_on_load, which pulls them);
 *   - progressive files (SOF2), bcmfw/jpeg/jprog.h. */
#ifndef JHOOK_H
#define JHOOK_H
#include <stdbool.h>
#include <stdint.h>
#include "resize.h"
#include "bmp.h"

#define JPEG_E_BADHUFF  (-13)   /* a code no table has */
#define JPEG_E_PROGMEM  (-14)   /* a progressive file's store doesn't fit */
#define JPEG_E_NOMEM    (-15)   /* the buffer is short of struct jpeg or of
                                   the decoder's rows (rows-only mode) */
/* a progressive file's coefficient store (jprog.h): its most, the same on
   both sides, so both refuse the same files (-4) */
#define PROG_EOI        0x4000  /* process_markers' flag: an EOI (progressive) */
/* struct jpeg and its alignment, at most: jdec.h JDEC_WORK_BASE
   (jpeg_load.c and jdec.c check both) */
#define JPEG_JDEC_STATE (90u * 1024)
#ifndef JPEG_PROG_MAX
#define JPEG_PROG_MAX   (2u << 20)
#endif

struct jpeg_rows_tap
{
    /* before the first row: the rows' size (the picture at the IDCT
       scale clip_jpeg_fd chose for its own output), and whether they are
       Y, Cb, Cr (1) or R, G, B (0) */
    void (*begin)(void *arg, const struct dim *rows, int yuv);
    /* row y (0, 1, ... rows->height - 1), each once, in order; at least
       rows->width pixels */
    void (*row)(void *arg, const struct img_part *row, int y);
    void *arg;
    /* 1: rows only.  The decoder makes no output of its own: bm->data is
       its work buffer alone (struct jpeg and its rows, no bitmap), every
       row goes to the tap, and the tap's consumer makes the outputs (jdec
       streams them into their destinations, jpeg-spike.md section 15).
       clip_jpeg_fd then returns 1, not the bitmap's size; a buffer short
       of struct jpeg or of the rows, JPEG_E_NOMEM */
    int only;
};
/* NULL: none (the default); set around one clip_jpeg_fd call */
void jpeg_set_rows_tap(const struct jpeg_rows_tap *tap);

/* The colour stage of a row of YCbCr pixels: n struct uint8_rgb at src (Y
 * in blue, Cb in green, Cr in red) to n RGB565 pixels at dst, dithered with
 * dy = DITHERY(row), the first pixel in a column that is a multiple of 16.
 * What it must make is the C path's: yuv_to_rgb (jpeg_common.h), then
 * output_row_8_native's dither and packing (bmp.c).  NULL (the default):
 * the decoder runs that C; the BCM image sets its vector kernel
 * (bcmfw/video/jdec_vpu.c).  Used for the rows' own size (clip_jpeg_fd's
 * unscaled rows, resize_push's same-size output) and after a resize
 * (output_row_32_native_fromyuv), when dithering. */
extern void (*jdec_yuv565)(fb_data *dst, const struct uint8_rgb *src, int n,
                           unsigned int dy);

/* The IDCT of a baseline scan's blocks whose two scales are equal and 4, 8
 * or 16 points (size): jpeg_idct4v/4h, 8v/8h or 16v/16h, four blocks a
 * call (algorithms.md section 1, jpeg-spike.md sections 13-14).  coef:
 * JDEC_IDCT_COEF halfwords, 32-byte aligned, block q's coefficient F[v][u]
 * (v vertical, both < 8) at JDEC_IDCT_AT(q) + 16 v + u; all 0 again on
 * return.  px: JDEC_IDCT_PX_SIZE bytes, 32-byte aligned, block q's pixel
 * (n, k) (row n) at JDEC_IDCT_PX(size, q, n, k), the rest undefined.
 * What it must make is the C path's, for any int16 coefficients.  NULL
 * (the default): the decoder runs that C, a block at a time; the BCM image
 * sets its vector kernel (bcmfw/video/jdec_vpu.c) when its start-up check
 * passes for every size.  Then the decoder (mcu_row_fast) sends it those
 * blocks four at a time, a batch for each component scale (the row's
 * last, fewer: the other places are 0), and puts the pixels where the C
 * would have written them (jdec_put); a block whose AC coefficients are
 * all 0 is the C's one value (jdec_fill).  It batches only a row buffer
 * whose start and row step are 32-byte aligned (jdec's own builds pad the
 * rows to 32 bytes). */
#define JDEC_IDCT_COEF      256
#define JDEC_IDCT_AT(q)     (((q) >> 1) * 128 + ((q) & 1) * 8)
#define JDEC_IDCT_PX_SIZE   1024
#define JDEC_IDCT_PX(size, q, n, k) (((q) >> 1) * ((size) == 16 ? 512 : 16) + \
                                     32 * (k) + 2 * (n) + ((q) & 1))
extern void (*jdec_idct)(uint8_t *px, int16_t *coef, int size);
/* Set with jdec_idct (all three, or none).  jdec_put: n blocks' pixels
 * from px (JDEC_IDCT_PX's places) into the row buffer: block q's pixel
 * (n, k) to to[q] + n * rowstep + 4 k, the pixels' other bytes kept
 * (to[q]'s low two bits are the component); grey (a one-component scan):
 * to bytes 0, 1 and 2 of each pixel.  The blocks in order: two may share
 * a row's bytes (Cb and Cr of one MCU).  jdec_fill: one block of the one
 * value v, the same way.  rowstep and the row buffer's start are multiples
 * of 32.  What they must make is jpeg_load.c's C put as it was
 * (bcmfw/jpeg/jidct.c jdec_put_c, jdec_fill_c); the BCM image sets its
 * vector kernels (jv_put, jv_fill: bcmfw/video/jdec_vpu.c). */
extern void (*jdec_put)(const uint8_t *px, unsigned char *const *to, int n,
                        int rowstep, int size, int grey);
extern void (*jdec_fill)(unsigned int v, unsigned char *out, int grey,
                         int rowstep, int size);
/* the C path's IDCT on one block (the check's reference): ws holds 64
   coefficients in natural order, F[v][u] at 8 v + u (the C's own layout
   made inside) */
void jdec_idct_c(int size, const int16_t *ws, unsigned char *out, int rowstep);

/* One output of resize.c fed row by row: the pixels resize_on_load(bm,
 * dither, src, rows 0..dh-1 step 1, cformat NULL, yuv) makes, given to
 * emit one output row at a time, in order (y = 0 .. dh - 1); at the
 * rows' own size, clip_jpeg_fd's unscaled rows instead (YUV to RGB, then
 * output_row_8_native) */
struct resize_push
{
    struct scaler_context ctx;      /* first: the output hook finds us */
    struct bitmap bm, row_bm;       /* the output; one row of it */
    struct dim src;
    struct img_part part;           /* the row being fed */
    int fed;                        /* the scaler took part */
    int linear;                     /* the vertical scaler: 0 area */
    int same;                       /* the rows' own size: 1 RGB, 2 YUV */
    int waiting;                    /* linear: an output row wants a row */
    uint32_t rows_in, rows_out;
    uint32_t iy, oy, mul, oye;      /* the vertical scaler's state (oye:
                                       linear's iye) */
    fb_data *row_px;
    void (*out_row)(uint32_t, void *, struct scaler_context *);
    void (*emit)(void *arg, int y, const fb_data *px);
    void *emit_arg;
};
/* buf: RESIZE_PUSH_BUF(dw) bytes, aligned to 4 */
#define RESIZE_PUSH_BUF(dw) ((uint32_t)(dw) * (3 * 16 + 2) + 4)
/* false: a size it can't take (a 1-pixel side to upscale) */
bool resize_push_init(struct resize_push *rp, int dw, int dh, bool dither,
                      const struct dim *src, unsigned char *buf,
                      unsigned int len, int yuv,
                      void (*emit)(void *arg, int y, const fb_data *px),
                      void *emit_arg);
/* the next source row (rows past src->height are ignored); false on an
   internal error */
bool resize_push_row(struct resize_push *rp, const struct img_part *row);
/* after the last source row: true if every output row was emitted */
bool resize_push_end(struct resize_push *rp);

#endif
