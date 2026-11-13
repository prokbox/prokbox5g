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

/* Polyphase FIR kernels for the RESAMPLE stage's fractional ratios (#271,
 * docs/design/resample-plan.md section 9). The prototype is
 * resample_fir_coefs.h: RS_FIR_TAPS taps per phase, RS_FIR_PHASES phases per
 * input sample, Q31. resample.c owns the phase arithmetic and the buffers;
 * these are the two inner loops, in C here (resample_fir.c, the reference,
 * used by the simulator and warble) and in ARM assembly on the device
 * (resample_fir_arm.S), bit-exact with each other. */
#ifndef _DSP_RESAMPLE_FIR_H
#define _DSP_RESAMPLE_FIR_H

#include <stdint.h>

#define RS_FIR_TAPS 10

/* One coefficient row for the fractional position 'frac' (Q32 of an input
 * sample): the prototype's rows j = frac >> 26 and j + 1, linearly
 * interpolated by the remaining 26 bits (as Q31):
 *     c[i] = r0[i] + (((int64)(r1[i] - r0[i]) * eta) >> 31) */
void rs_fir_row(int32_t *c, uint32_t frac);

/* n outputs (n >= 1) for one channel. tag[k] is a coefficient row's
 * address (4-byte aligned) plus, in its low two bits, how many samples x
 * moves on before output k. Output k reads RS_FIR_TAPS samples from there
 * (oldest first):
 *     x += tag[k] & 3; c = (const int32_t *)(tag[k] & ~3);
 *     dst[k] = (int32)((sum_i (int64)x[i] * c[i]) >> 31) */
void rs_fir_batch(int32_t *dst, const int32_t *x, const uintptr_t *tag,
                  int n);

#ifdef RESAMPLE_HB_STANDALONE
/* Test builds: the C reference under its own name beside the ARM code */
void rs_fir_row_c(int32_t *c, uint32_t frac);
void rs_fir_batch_c(int32_t *dst, const int32_t *x, const uintptr_t *tag,
                    int n);
#endif

#endif /* _DSP_RESAMPLE_FIR_H */
