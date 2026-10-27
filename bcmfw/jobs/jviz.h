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

#ifndef BCMFW_JVIZ_H
#define BCMFW_JVIZ_H

/* The Now Playing visualizer's engine (#126; docs/research/bcm2722/
 * visualizer.md): one definition of the spectrum and of the bars, which
 * the PP's CPU path, the reference executor's J_OP_SPECTRUM and J_OP_BARS
 * (jref.c, JREF_VIZ) and the BCM's vector kernel (viz_kernels.S,
 * gen_viz.py) all follow bit for bit.
 *
 * The spectrum of n = 256 or 512 real samples (16-bit, mono or a downmix):
 *   - the input is in split order: x[0], x[2], ..., x[n-2], then x[1],
 *     x[3], ..., x[n-1] (the PP writes its decimated downmix that way, at
 *     no cost; the real FFT wants the even and odd samples apart);
 *   - a periodic Hann window, Q15 scaled to a peak of JVIZ_HANN_PEAK so the
 *     packed complex samples stay below 2^14 in magnitude;
 *   - the real FFT as an m = n/2 point complex FFT of x[2k] + i x[2k+1]
 *     (four-step: m = rows x 16 lanes, radix-2 decimation in frequency
 *     down the rows, a Q15 twiddle per element, radix-2 along the lanes),
 *     every stage halving (per-stage scaling: the output is X / m), then
 *     the split step X[k] = E[k] + W_n^k O[k];
 *   - the power |X[k]|^2 of bins 0 .. m-1 as 32-bit values, exact from
 *     the 16-bit X (the vector unit's 32-bit accumulator).
 * Every product that is summed is summed exactly in 32 bits (wrapping as
 * the accumulator does) and rounded once, by flooring its high half.
 * Bands: the largest power in each band's bins, as a log2 in 1/16 steps
 * (jviz_l2: 0.19 dB a step, 0..511). */

#include <stdint.h>

#define JVIZ_N_MIN          256
#define JVIZ_N_MAX          512
#define JVIZ_M_MAX          (JVIZ_N_MAX / 2)
#define JVIZ_BANDS_MAX      32
#define JVIZ_HANN_PEAK      23000   /* the window's peak (Q15 of 0.70):
                                       |x[2k] + i x[2k+1]| < 2^14 with
                                       0.7 % to spare for rounding */
/* The kernel's raw output (and the C's, jviz_raw_ref): 16 rows of 16
 * halfwords, the low halves of the powers, then 16 rows of the high
 * halves; bin k = k2 + rows * k1 at row k2, lane k1 */
#define JVIZ_RAW_HW         512

/* The tables of one size (jviz_tab.h, gen_viz.py), each 32-byte aligned,
 * rows of 16 halfwords: win (rows rows of the even samples' window, then
 * the odd ones'), tw (the four-step's W_m^(n1 k2), rows rows of real
 * parts then of imaginary), ws (the split step's W_n^k at row k2, lane
 * k1, likewise) */
struct jviz_tabs
{
    const int16_t *win, *tw, *ws;
    uint32_t rows;              /* m / 16: 8 or 16 */
};

/* The tables for n samples, or 0 if n is not 256 or 512 */
const struct jviz_tabs *jviz_tabs(uint32_t n);
#ifdef JVIZ_OVL
/* The overlay's build: the 256-sample tables, derived exactly from the
 * 512-sample ones into buf (768 halfwords, 32-byte aligned; jviz.c), are
 * what jviz_tabs(256) gives until the next call.  Every path that may
 * take 256 samples calls it first, with a buffer that outlives the use
 * (jref_viz.c jref_spectrum, viz_vpu.c viz_vpu_init) */
#define JVIZ_TAB256_HW      768
void jviz_derive256(int16_t *buf);
#endif

/* The reference FFT: X[k] / m for k < m, as the vector kernel computes
 * it (pcm in split order, 32-bit aligned) */
void jviz_fft_ref(int16_t *xr, int16_t *xi, const int16_t *pcm, uint32_t n);
/* The same as the kernel's raw output (JVIZ_RAW_HW halfwords; the rows
 * past m / 16 are 0) */
void jviz_raw_ref(uint16_t *raw, const int16_t *pcm, uint32_t n);
/* The same over the tables t (rows m / 16) instead of the size's own: any
 * 16-bit values, so a check can drive every sum the kernel makes to its
 * limits (viz_vpu.c's start-up check) */
void jviz_raw_ref_t(uint16_t *raw, const int16_t *pcm, uint32_t n,
                    const struct jviz_tabs *t);
/* Raw output to the powers p[k], k < n / 2 */
void jviz_raw_power(uint32_t *p, const uint16_t *raw, uint32_t n);
/* The powers p[k], k < n / 2, of n samples (split order) by the C: the
 * PP's CPU path and the BCM's when its kernel is off */
void jviz_power_ref(uint32_t *p, const int16_t *pcm, uint32_t n);

/* log2 of p in 1/16 steps: 16 floor(log2 p) + a 4-bit mantissa's table
 * value; 0 for p of 0 or 1 */
uint32_t jviz_l2(uint32_t p);
/* nb bands: band b is the largest of p[e[b]] .. p[e[b+1] - 1] (bins of m),
 * as jviz_l2; an empty band 0 */
void jviz_bands(uint16_t *l2, const uint32_t *p, uint32_t m, const uint16_t *e,
                uint32_t nb);

/* ---- The bars (J_OP_BARS) --------------------------------------------- */
#define JVIZ_BARS_BG        0x1     /* fill the rect with bg first */
#define JVIZ_BARS_CAP       0x2     /* a peak cap of cap_h rows per bar */
#define JVIZ_BARS_ROUND     0x4     /* the top row of a bar inset 1 pixel */
#define JVIZ_BARS_FLIP      0x8     /* upside down: the rect's row r drawn
                                       at row h - 1 - r (the bars hang from
                                       its top; the background, the same) */

struct jviz_bars
{
    uint32_t n, barw, gap, cap_h, flags;
    uint16_t bot, top, cap, bg;     /* RGB565: the gradient's ends */
    const uint8_t *height, *peak;   /* pixels from the rect's bottom */
};

/* The bars in a w x h rect at px (pitch in pixels): n bars of barw
 * pixels, gap apart, centred; bar i is height[i] rows high (at most h),
 * row t from the bottom coloured bot + (top - bot) * f / 256 per channel,
 * f = t * 256 / h; its cap (JVIZ_BARS_CAP, peak[i] > 0) the cap_h rows
 * from peak[i] down, in cap; with JVIZ_BARS_FLIP, every row of that
 * frame at row h - 1 - r instead of r (a vertical mirror, by addressing
 * the rows bottom up: no second pass).  -1 if the bars don't fit the
 * width, else 0.  fill: 0, or a solid fill (the BCM's vector FILL) that returns the
 * columns it did as lo | hi << 16 (jref.h), the rest done here */
typedef uint32_t (*jviz_fill_fn)(uint16_t *d, uint32_t pitch, uint32_t w,
                                 uint32_t h, uint32_t c);
int jviz_bars_draw(uint16_t *px, uint32_t pitch, uint32_t w, uint32_t h,
                   const struct jviz_bars *b, jviz_fill_fn fill);
/* The gradient's colour at f / 256 of the way from c0 to c1 (RGB565, per
 * channel, floored): row t of a bar is jviz_mix565(bot, top, t * 256 / h)
 * as jviz_bars_draw computes it */
uint32_t jviz_mix565(uint32_t c0, uint32_t c1, uint32_t f);
/* A whole-frame bars path (the BCM's vector kernel: viz_vpu.c viz_bars):
 * jviz_bars_draw's pixels for the rect at px, x pixels into its surface's
 * row, returning 1; or 0, nothing drawn, to leave the frame to
 * jviz_bars_draw (a rect it doesn't take, or bars that don't fit) */
typedef int (*jviz_bars_fn)(uint16_t *px, uint32_t pitch, uint32_t x, uint32_t w,
                            uint32_t h, const struct jviz_bars *b);

/* ---- Scope, Radial and Spectrogram (J_OP_POLYLINE, J_OP_SEGMENTS,
 * J_OP_PALROW; proposed, sim only) ---------------------------------------
 * Plain C on every runner (the PP's CPU path, the reference executor, the
 * BCM's overlay): line work is scalar, so the BCM runs this same code and
 * no vector kernel is involved; its start-up check (jviz_draw_check)
 * guards the build against its compiler and core. */

/* The polyline: n points, point i's column (i (w - 1)) / (n - 1) of the
 * rect (floored: x implicit, spread across it), its row from element
 * e = off + i step of src: with half, e is a sample of a split-order
 * window (jviz.h's spectrum input), at index e / 2 if e is even, else
 * half + e / 2; without, at index e.  An int16 element v is row
 * h / 2 - ((v gain) >> 16) (gain Q16, at most 0xFFFF); a byte
 * (JVIZ_POLY_U8) is the row itself; either is clamped to 0 .. h - 1.
 * Column c holds the rows between its line's row and the next column's
 * (linear between the points: y0 + (y1 - y0) (c - x0) / (x1 - x0),
 * truncated as C divides), so the line is connected; a width-W line is
 * that path under a W x W square brush (rows and columns (W - 1) / 2
 * before, W / 2 after), every pixel written once in colour */
#define JVIZ_POLY_U8        0x1     /* bytes: each point's row */
#define JVIZ_POLY_W_MAX     320     /* the widest rect */
#define JVIZ_POLY_N_MAX     1024

struct jviz_poly
{
    const void *src;            /* int16 (2-byte aligned) or bytes */
    uint32_t len;               /* elements at src */
    uint32_t n, off, step, half, gain, width, flags;
    uint16_t colour;
};

/* -1 (nothing drawn): n < 2 or > JVIZ_POLY_N_MAX, width not 1-3, w over
 * JVIZ_POLY_W_MAX, h 0, or an element past len; else 0 */
int jviz_poly_draw(uint16_t *px, uint32_t pitch, uint32_t w, uint32_t h,
                   const struct jviz_poly *p);

/* The radial segments: segment i points at angle index k = (a0 + i astep)
 * mod 48 (index 0 straight up, clockwise, 7.5 degrees a step: the
 * direction (s, -c), s and c Q14 sin and cos of 2 pi k / 48, jviz_sin48);
 * from (cx, cy) in the rect, its pixel t (0 .. min(len[i], lmax) - 1) at
 * radius r = r0 + t is the point (cx + ((r s + 8192) >> 14),
 * cy + ((r -c + 8192) >> 14)) under a width x width square (as the
 * polyline's brush), coloured jviz_mix565(c0, c1, t * 256 / lmax) (the
 * bars' gradient: c0 at the base, c1 at the tip); later pixels overwrite.
 * JVIZ_SEG_RING first draws a dot of ringd at radius rring on every
 * segment's angle, in ring; JVIZ_SEG_CAP last a dot of capd at radius
 * r0 + min(peak[i], lmax) for each peak[i] > 0, in cap.  A dot of size d
 * is the d x d square at that point (the same offsets); JVIZ_SEG_ROUND
 * drops its four corners when d >= 3.  Everything is clipped to the rect */
#define JVIZ_SEG_CAP        0x1
#define JVIZ_SEG_RING       0x2
#define JVIZ_SEG_ROUND      0x4
#define JVIZ_SEG_MAX        48

struct jviz_segs
{
    uint32_t n, a0, astep, width, flags;
    uint32_t r0, lmax, rring, capd, ringd;
    int32_t cx, cy;
    uint16_t c0, c1, cap, ring;
    const uint8_t *len, *peak;
};

/* -1 (nothing drawn): n 0 or over JVIZ_SEG_MAX, a0 or astep over 47,
 * width not 1-3, lmax 0, capd or ringd over 15; else 0 */
int jviz_segs_draw(uint16_t *px, uint32_t pitch, uint32_t w, uint32_t h,
                   const struct jviz_segs *s);
/* Q14 sin of 2 pi k / 48, any k (taken mod 48) */
int32_t jviz_sin48(uint32_t k);

/* The palette row: nb cells across the rect, cell b columns
 * (b w) / nb .. ((b + 1) w) / nb - 1, every row of the rect in
 * lut[lv[b]] (lut: 256 RGB565 entries) */
#define JVIZ_PAL_MAX        96

struct jviz_pal
{
    const uint16_t *lut;
    const uint8_t *lv;
    uint32_t nb;
};

/* -1 (nothing drawn): nb 0, over JVIZ_PAL_MAX or over w; else 0 */
int jviz_pal_draw(uint16_t *px, uint32_t pitch, uint32_t w, uint32_t h,
                  const struct jviz_pal *p);

/* The three ops' start-up check (the BCM's: viz_vpu.c viz_draw_init): op
 * 0 (POLYLINE), 1 (SEGMENTS) or 2 (PALROW) draws its fixed scene on a
 * JVIZ_CHK_W x JVIZ_CHK_H surface at s (any contents: it is filled
 * first) and returns the FNV-1a of its pixels (each halfword low byte
 * first), which must equal JVIZ_CHK_SUM[op] (viz_test checks the Mac's
 * build gives these) */
#define JVIZ_CHK_W          40
#define JVIZ_CHK_H          30
#define JVIZ_CHK_POLY       0xb86b5843u
#define JVIZ_CHK_SEGS       0x3abdc3d3u
#define JVIZ_CHK_PAL        0x35eaaef0u
uint32_t jviz_draw_check(uint16_t *s, uint32_t op);

#endif /* BCMFW_JVIZ_H */
