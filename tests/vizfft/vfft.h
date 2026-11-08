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

/* The visualiser's FFT as the VideoCore II vector unit will compute it
 * (#126, docs/design/viz-fft-vc.md): the definition and the host models.
 *
 * One frame: 512 real 16-bit samples (44.1 or 48 kHz, mono, split order:
 * x[0], x[2] .. x[510], then x[1], x[3] .. x[511], as the PP's tap writes
 * them) to 256 powers |X[k] / 256|^2, k = 0 .. 255, as 32-bit integers,
 * then the shell's bands (the largest power in each band's bins, as a
 * 1/16-octave log2).
 *
 * The FFT is a real FFT done as a 256-point complex FFT of
 * z[j] = x[2j] + i x[2j+1] plus a split step, and the 256-point FFT is a
 * four-step (Bailey) FFT laid out as 16 rows x 16 lanes so that every
 * butterfly is a whole-row vector op:
 *   window   z = (pcm * win) >> 16                    (vmulhd)
 *   step 1   16-point radix-2 DIF down the rows, all 16 lanes at once
 *   step 2   each element times W_256^(lane * row)    (the four-step twiddle)
 *   step 3   transpose, then the same 16-point DIF down the rows again
 *   split    X[k] = E[k] + W_512^k O[k] from Z[k] and Z[256 - k]
 *   power    |X|^2 as an exact 32-bit sum (the accumulator)
 * Every stage halves (CMSIS-style per-stage scaling), so the output is
 * X / 256 and nothing overflows 16 bits for a window peak of 23000.
 *
 * Two arithmetic modes, identical except in step 2 and the split:
 *   VFFT_SEP    each product floored on its own (vmulhd), the floored
 *               products added in 16 bits.  Uses only vector forms that
 *               silicon (or Apple's own code) has run: the proposal.
 *   VFFT_EXACT  each output's products summed exactly in the 32-bit
 *               accumulator and floored once: today's jviz.c arithmetic,
 *               bit for bit.  Needs four accumulator forms with a vector
 *               B operand that silicon has not run (a bench row first).
 *
 * Files: vfft_ref.c (the definition as plain integer C: what the BCM's
 * start-up check and the PP would compile), vfft_vpu.c (a model of the
 * vector register file and of every instruction the kernel issues, with
 * each instruction's form key and cost), vfft_tabs.c (the tables, host
 * only: libm). */
#ifndef VFFT_H
#define VFFT_H

#include <stdint.h>

#define VFFT_N          512             /* real samples a frame */
#define VFFT_M          256             /* complex points, bins kept */
#define VFFT_ROWS       16              /* m = 16 rows x 16 lanes */
#define VFFT_RAW_HW     512             /* raw output: 256 low halves, then
                                           256 high halves, bin k at k */
#define VFFT_HANN_PEAK  23000           /* jviz.h JVIZ_HANN_PEAK */
#define VFFT_Q          32767           /* twiddles: round(Q cos), round(-Q sin) */
#define VFFT_BANDS_MAX  32

enum vfft_mode { VFFT_SEP = 0, VFFT_EXACT = 1 };

/* The tables, every array 32-byte aligned as the vector loads need:
 *   win  the periodic Hann window in split order (evens' 256, odds' 256),
 *        round(23000 * 0.5 * (1 - cos(2 pi t / 512)))
 *   tw   step 2's W_256^(l k2) at row k2, lane l: 256 real parts, then 256
 *        imaginary (row-major, 16 a row)
 *   ws   the split's W_512^k at row k >> 4, lane k & 15 (natural order):
 *        256 real, then 256 imaginary
 *   w16  W_16^e, e < 8, as (re, im) pairs: step 1's and 3's immediates
 *   log16 jviz_l2's mantissa table: round(16 log2(1 + f / 16)), f < 16
 * Imaginary parts are round(-Q sin): W = e^(-2 pi i a), the forward FFT */
struct vfft_tabs
{
    int16_t win[VFFT_N] __attribute__((aligned(32)));
    int16_t tw[2 * VFFT_M] __attribute__((aligned(32)));
    int16_t ws[2 * VFFT_M] __attribute__((aligned(32)));
    int16_t w16[16];
    uint8_t log16[16];
};

/* vfft_tabs.c (host only): fill t; Python's rounding (ties to even), as
 * bcmfw/video/gen_viz.py makes jviz_tab.h */
void vfft_tabs_make(struct vfft_tabs *t);

/* ---- vfft_ref.c: the definition, plain integer C -------------------- */

/* raw[k] = the low half of bin k's power, raw[256 + k] its high half */
void vfft_ref(uint16_t *raw, const int16_t *pcm, const struct vfft_tabs *t,
              enum vfft_mode mode);
/* the same, the complex X[k] / 256 before the power (for the precision
 * tests): xr[k], xi[k], k < 256 */
void vfft_ref_x(int16_t *xr, int16_t *xi, const int16_t *pcm,
                const struct vfft_tabs *t, enum vfft_mode mode);
/* raw to powers p[k], k < 256 */
void vfft_power(uint32_t *p, const uint16_t *raw);
/* log2 of p in 1/16 steps (jviz_l2's definition): 0 for p < 2 */
uint32_t vfft_l2(uint32_t p, const struct vfft_tabs *t);
/* nb bands straight from raw (no unpacking pass): band b is the largest
 * power of bins e[b] .. e[b+1] - 1 as vfft_l2; an empty band 0 */
void vfft_bands(uint16_t *l2, const uint16_t *raw, const uint16_t *e,
                uint32_t nb, const struct vfft_tabs *t);

/* ---- vfft_vpu.c: the vector unit's program, instruction by instruction */

/* What the kernel cost, by phase (vfft_vpu.c's phase names) */
#define VFFT_PHASES     10
struct vfft_stats
{
    const char *name[VFFT_PHASES];
    uint32_t insns[VFFT_PHASES];        /* vector instructions issued */
    uint32_t rows[VFFT_PHASES];         /* their REP counts summed */
    uint32_t bytes[VFFT_PHASES];        /* code bytes (80-bit ALU 10, 48-bit
                                           memory 6) */
    uint32_t scalar[VFFT_PHASES];       /* scalar instructions (setup) */
};

/* Mutations (tests only): each breaks one step the way a plausible
 * generator bug would; the tests must catch every one */
enum vfft_mutation
{
    VFFT_MUT_NONE = 0,
    VFFT_MUT_LANE0,                     /* the partner's lane-0 fix skipped */
    VFFT_MUT_TWSIGN,                    /* step 2: the sine's sign flipped */
    VFFT_MUT_BREV,                      /* step 3's last stage: rows 1 and 2
                                           of the bit reversal swapped */
    VFFT_MUTATIONS
};

/* Run the kernel on the model: the same raw as vfft_ref for the same
 * mode.  st (or 0) gets the cost; forms (or 0) is called once per
 * instruction with its form key (tools/bcmre/proven_forms.txt's) and its
 * REP count */
typedef void (*vfft_form_fn)(const char *key, uint32_t rep, void *arg);
void vfft_vpu(uint16_t *raw, const int16_t *pcm, const struct vfft_tabs *t,
              enum vfft_mode mode, enum vfft_mutation mut,
              struct vfft_stats *st, vfft_form_fn forms, void *arg);

#endif /* VFFT_H */
