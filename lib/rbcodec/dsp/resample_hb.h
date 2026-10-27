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

/* Half-band polyphase IIR stages for the RESAMPLE stage (P19 19-13,
 * docs/design/resample-plan.md). Pure integer kernels: no Rockbox includes,
 * so the host and ARM bit-exactness tests build them on their own.
 *
 * A stage is two chains of first-order allpass sections in z^-2:
 *     H(z) = 0.5 * (A0(z^2) + z^-1 * A1(z^2))
 * Section k of a chain, at the chain's own (half) rate:
 *     y[n] = s_k + 2 * mulhi(a_k, x[n] - s_{k+1}),   s_k <- x[n]
 * where mulhi(a, d) = (a * d) >> 32 (64-bit product, a in Q31), s_k holds
 * the section's last input and s_{k+1} its last output (the next section's
 * last input). Integer arithmetic wraps; C and ARM agree bit for bit. */
#ifndef _DSP_RESAMPLE_HB_H
#define _DSP_RESAMPLE_HB_H

#include <stdint.h>

#define HB_MAX_SECT 4          /* sections per chain */
#define HB_STATE    (2 * (HB_MAX_SECT + 1))

/* Offsets used by the ARM code: c[0..3] chain 0, c[4..7] chain 1 (Q31),
 * k0 at 32, k1 at 36 (sections in each chain, 1..4). */
struct hb_coefs
{
    int32_t c[2 * HB_MAX_SECT];
    int32_t k0, k1;
};

/* State per stage per channel: st[0..4] chain 0, st[5..9] chain 1. */

/* x2 interpolation: n inputs (n >= 1) -> 2n outputs. dst must not
 * overlap src. */
void hb_up2(int32_t *dst, const int32_t *src, int n, int32_t *st,
            const struct hb_coefs *c);
/* /2 decimation: 2n inputs (n >= 1) -> n outputs. dst must not overlap
 * src. */
void hb_down2(int32_t *dst, const int32_t *src, int n, int32_t *st,
              const struct hb_coefs *c);

#ifdef RESAMPLE_HB_STANDALONE
/* Test builds: the C reference under its own name beside the ARM code */
void hb_up2_c(int32_t *dst, const int32_t *src, int n, int32_t *st,
              const struct hb_coefs *c);
void hb_down2_c(int32_t *dst, const int32_t *src, int n, int32_t *st,
                const struct hb_coefs *c);
#endif

#endif /* _DSP_RESAMPLE_HB_H */
