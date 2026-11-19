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

/* The reduction of samples wider than the DSP chain takes (#318: 30..32-bit
   FLAC) to that width, with TPDF dither. */
#ifndef _FLAC_DITHER_H
#define _FLAC_DITHER_H

#include <stdint.h>

/* Reduces n samples at p, in place, by shift bits (1..8), to signed values
   of bits bits (bits + shift <= 32), with triangular (TPDF) dither: the sum
   of two independent uniform values of shift bits each, so that the mean
   and the variance of the error do not depend on the signal.

   *state is the generator (an LCG, x' = 1664525 x + 1013904223 mod 2^32),
   one step a sample: r1 is its top shift bits, r2 the shift bits below bit
   24.  Each sample x = q 2^shift + f (f the low bits, 0..2^shift-1) becomes
   q + floor((f + r1 + r2 - 2^(shift-1)) / 2^shift), clamped to bits bits.
   The mean error is -1/2 of an input LSB for every input (2^-33 of full
   scale on a 32-bit stream: a DC offset under any DAC's floor); see
   tests/flacdither for the statistics and tests/flacarm/dither.py for the
   same arithmetic written independently.  The state starts at 0 when a
   stream opens (flac.c clears the context), so a stream always gets the
   same dither: the fixed seed the tests rely on. */
static inline void flac_reduce_tpdf(int32_t *p, int n, int shift, int bits,
                                    uint32_t *state)
{
    const int32_t mask = (1 << shift) - 1;
    const int32_t half = 1 << (shift - 1);
    const int32_t hi = (int32_t)((1u << (bits - 1)) - 1);
    const int32_t lo = -hi - 1;
    uint32_t x = *state;
    int i;

    for (i = 0; i < n; i++) {
        int32_t v = p[i];
        int32_t q = v >> shift;
        int32_t t;

        x = x * 1664525u + 1013904223u;
        t = (v & mask) + (int32_t)(x >> (32 - shift))
            + (int32_t)((x >> (24 - shift)) & mask) - half;
        q += t >> shift;                /* -1, 0, 1 or 2 */
        p[i] = q > hi ? hi : q < lo ? lo : q;
    }
    *state = x;
}

#endif
