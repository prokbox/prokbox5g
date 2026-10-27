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

#ifndef BCMFW_JDEC_H
#define BCMFW_JDEC_H

/* One JPEG to up to JDEC_MAX_OUT RGB565 outputs: the single definition of
 * the art builder's decode (jpeg-spike.md section 8).  The CPU path (the
 * shell's decode-once, P19 W0-10) and the BCM's J_OP_DECODE both run this
 * module, so the two make the same pixels and fail the same way:
 *   - each output is the source resized to dw x dh (the shell's decoder
 *     and resizer: FORMAT_NATIVE | FORMAT_DITHER | FORMAT_RESIZE), then
 *     the window (cx, cy, cw, ch) of that, written to px (pitch pixels a
 *     row).  The sizes are the shell's policy (fill, fit, the portrait
 *     panel), computed by the caller, never here;
 *   - baseline and progressive (Huffman) files; an unsupported kind
 *     (arithmetic, 12-bit, odd subsampling, a progressive file whose
 *     coefficient store passes JDEC_PROG_MAX) and a corrupt (a bad
 *     Huffman code included) or truncated stream fail with a code, so the
 *     caller tries its next source.  After an error the outputs' pixels
 *     are unspecified (the caller drops them).
 *   - a progressive file gives the pixels of the baseline file with the
 *     same coefficients (jpeg-spike.md section 9).
 *
 * Decode-once (rev 2; jpeg-spike.md section 8, "decode-once"): one pass
 * of the decoder makes every output.
 *   1. The primary output is the largest by area, dw * dh (the first of
 *      equals: jdec_primary).  It is decoded exactly as a single decode
 *      at its size: the shell's clip_jpeg_fd(FORMAT_NATIVE | FORMAT_DITHER
 *      | FORMAT_RESIZE) at dw x dh, so its pixels are rev 1's, bit for
 *      bit.  That call picks the IDCT scale (1/8, 1/4, 1/2 or 1) for dw x
 *      dh and makes the picture's rows at that scale, the "rows" (rw x rh:
 *      the source's size times the scale, rounded down).
 *   2. Every other output is the rows resized to its dw x dh by resize.c
 *      with resize_on_load's own arithmetic (area average down, linear
 *      up, per axis; dithered; the YUV-to-RGB step after the resize, as
 *      for the primary), fed row by row from the same pass
 *      (bcmfw/jpeg/jhook.h, resize_push).  It is what resize_on_load(dw x
 *      dh, from the rows) would make, and so not, in general, what a
 *      single decode at its own size makes (that one may take a smaller
 *      IDCT scale).
 *   3. Each output's window (cx, cy, cw, ch) is then kept, as before.
 * Streamed (jpeg-spike.md section 15): no output is held whole.  The
 * decoder makes only the rows (the rows tap's rows-only mode,
 * bcmfw/jpeg/jhook.h); every output, the primary too, is resize_push over
 * them, each row of its window written to px at its pitch as it is made.
 * resize_push is resize_on_load's arithmetic and clip_jpeg_fd's unscaled
 * path, so the primary's pixels are still the single decode's.
 * The expected pixels are tests/jpeg/golden_once.txt (the art set: panel
 * 300, Now Playing 142, thumbnail 55, and the fitted box). */

#include <stdint.h>

#define JDEC_MAX_OUT        4

#define JDEC_OK             0
#define JDEC_E_UNSUPPORTED  (-1)    /* not baseline nor progressive, odd
                                       subsampling, past JDEC_PROG_MAX */
#define JDEC_E_CORRUPT      (-2)    /* no SOI, bad markers, a decoder error */
#define JDEC_E_TRUNCATED    (-3)    /* the stream ends before its EOI */
#define JDEC_E_RANGE        (-4)    /* an output's sizes or window */
#define JDEC_E_NOMEM        (-5)    /* the work area is too small */

struct jdec_out
{
    uint16_t dw, dh;                /* the resized size */
    uint16_t cx, cy, cw, ch;        /* the window of it that is kept */
    uint16_t *px;                   /* cw x ch pixels out */
    uint32_t pitch;                 /* in pixels */
};

struct jdec_info
{
    uint16_t sw, sh;                /* the source's size */
};

/* Where the file comes from: the PP's file, the BCM's input ring, or
 * memory (jdec_mem_src).  The decoder reads it from the start twice (the
 * probe, then the decode; once more per pass if a later revision needs
 * one), in order, and skips forward; never backward. */
struct jdec_src
{
    uint32_t len;                   /* the file's bytes */
    /* back to byte 0: 0, or < 0 if it can't */
    int (*start)(void *ctx);
    /* the next n bytes into buf: how many came (< n only at the end), < 0
       on an error */
    long (*fetch)(void *ctx, void *buf, uint32_t n);
    /* skip n bytes forward: 0, or < 0 */
    int (*skip)(void *ctx, uint32_t n);
    void *ctx;
};

/* A source over memory (the tests, a BCM slot) */
struct jdec_mem
{
    const uint8_t *p;
    uint32_t len, pos;
};
void jdec_mem_src(struct jdec_src *src, struct jdec_mem *m, const uint8_t *p,
                  uint32_t len);

/* The source's size and kind only (no pixels): JDEC_OK or an error */
int jdec_probe(const struct jdec_src *src, void *work, uint32_t work_bytes,
               struct jdec_info *info);

/* Every output, or the first error; work: the decoder's buffer, no
 * output held in it (streamed, above):
 *   JDEC_WORK_BASE      the decoder's state, struct jpeg with its 64 KB
 *                       read buffer, its tables and IDCT batches (about
 *                       85 KB on the BCM, 88 KB on the Mac; jpeg_load.c
 *                       checks it fits);
 *   JDEC_WORK_ROWS(dw)  its rows, one MCU row of the picture at the IDCT
 *                       scale the primary (width dw) picks: at most 16
 *                       lines of (2 dw + 16) pixels, 3 bytes each.  That
 *                       bounds every scale but 1/8, whose rows are at
 *                       most 4 lines (v = 2, scale 1/4 vertically) of a
 *                       source more than 8 dw wide: less than the bound
 *                       until the source passes 64 dw (19,200 px for the
 *                       panel), where the decoder answers NOMEM;
 *   JDEC_WORK_OUT(dw)   each output's resizer, the primary's too: three
 *                       rows of 16 bytes a pixel and one output row.
 * jdec_work(out, n) is the sum.  A progressive file also needs its
 * coefficient store, at most JDEC_PROG_MAX more (the decoder's
 * JPEG_PROG_MAX, bcmfw/jpeg/jhook.h); short of it, or of the rows, NOMEM.
 * Before streaming (rev 2) the primary's whole output sat in the work
 * area too: JDEC_WORK(dw, dh) = 2 dw dh + 12 dw + 190 KB, which sized
 * jobs.h J_DEC_WORK_MIN (the NP box: 236,592 B; since streaming it is the
 * 16:9 art set, 184,428 B; jpeg-spike.md section 15 has the before and
 * after). */
#define JDEC_PROG_MAX       (2u << 20)
#define JDEC_WORK_BASE      (90u * 1024)
#define JDEC_WORK_ROWS(dw)  ((uint32_t)(dw) * 96 + 768)
#define JDEC_WORK_OUT(dw)   ((uint32_t)(dw) * 50 + 512)
int jdec_run(const struct jdec_src *src, const struct jdec_out *out, int n,
             void *work, uint32_t work_bytes, struct jdec_info *info);
/* the primary output's index (decode-once, above) */
int jdec_primary(const struct jdec_out *out, int n);
/* the work area jdec_run needs for these outputs */
uint32_t jdec_work(const struct jdec_out *out, int n);
/* Tests only (bcmfw/jpeg/jdoracle.c): a tap that also gets every row
 * jdec_run's decoder makes, after its own outputs; NULL (the default):
 * none.  Its only field is not used.  Not in the BCM image (__vc4__). */
struct jpeg_rows_tap;
extern const struct jpeg_rows_tap *jdec_tap_also;
/* Called by the decoder about once per MCU row and per output row (its
 * yield(), bcmfw/jpeg/env/jenv.h); NULL (the default): nothing.  The BCM
 * image services its main loop there (S3c, video/jobs_bcm.c).  It must
 * not touch the decode's source, outputs or work area. */
extern void (*jdec_yield)(void);

#endif
