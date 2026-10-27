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

#ifndef BCMFW_JMIX_H
#define BCMFW_JMIX_H

/* The pixel arithmetic of the display jobs FADE and AVG: one definition
 * for the BCM, the reference executor (jref.c) and the shell
 * (apps/ipodui/preview.c's xfade_rect() and AVG2 call these: P19 gap G5),
 * so a fallback frame and a chip frame cannot drift apart.  RGB565.
 * Header-only and static inline: no call per pixel on any core. */

#include <stdint.h>

/* One pixel d moved weight/256 of the way toward s (1..256; 256 is s
 * exactly).  Packed 0x07e0f81f form, weight in 1/32 rounded, as the
 * shell's drift has always done it: per field (d * (32 - w) + s * w + 16)
 * >> 5.  Computed with one multiply as (d << 5) + (s - d) * w, the same
 * integer mod 2^32 (the packed word is an ordinary integer, the borrows
 * of s - d between fields are linear, and no field's true sum overflows
 * it); bcmfw/algobench/fade_identity.c checks it against the two-multiply
 * form (algorithms.md section 6). */
static inline uint16_t jmix_fade_px(uint32_t d, uint32_t s, uint32_t weight)
{
    uint32_t w32 = (weight + 4) >> 3, r;

    if (weight >= 256)
        return (uint16_t)s;
    if (d == s)
        return (uint16_t)d;
    if (w32 == 0)
        w32 = 1;
    d = (d | (d << 16)) & 0x07e0f81f;
    s = (s | (s << 16)) & 0x07e0f81f;
    r = (((d << 5) + (s - d) * w32 + 0x02008010) >> 5) & 0x07e0f81f;
    return (uint16_t)(r | (r >> 16));
}

/* The average of two pixels, or of two pairs packed in a word: the
 * drift's half step (AVG2) */
#define JMIX_AVG2(a, b) (((a) & (b)) + ((((a) ^ (b)) & 0xf7def7deu) >> 1))

static inline uint16_t jmix_avg_px(uint32_t a, uint32_t b)
{
    return (uint16_t)JMIX_AVG2(a & 0xFFFF, b & 0xFFFF);
}

/* A rectangle: dst = fade(a, b, weight), pitches in pixels; dst may be a
 * (the shell's in-place step).  Equal pixels are skipped (not stored). */
static inline void jmix_fade_rect(uint16_t *dst, int dp, const uint16_t *a, int ap,
                                  const uint16_t *b, int bp, int w, int h,
                                  uint32_t weight)
{
    int x;

    for (; h > 0; h--, dst += dp, a += ap, b += bp)
        for (x = 0; x < w; x++)
        {
            uint16_t v = jmix_fade_px(a[x], b[x], weight);

            if (dst != a || v != a[x])
                dst[x] = v;
        }
}

static inline void jmix_avg_rect(uint16_t *dst, int dp, const uint16_t *a, int ap,
                                 const uint16_t *b, int bp, int w, int h)
{
    int x;

    for (; h > 0; h--, dst += dp, a += ap, b += bp)
        for (x = 0; x < w; x++)
            dst[x] = jmix_avg_px(a[x], b[x]);
}

/* The split shadow (8b-3): a pixel darkened by alpha/256, each channel
 * scaled by 256 - alpha and truncated; apps/ipodui/material.c's
 * ui_shadow_apply() calls this (gap G5, widened by review item H2) */
static inline uint16_t jmix_shade_px(uint32_t p, uint32_t alpha)
{
    uint32_t k = 256 - alpha;

    return (uint16_t)(((((p >> 11) * k) >> 8) << 11)
                      | (((((p >> 5) & 63) * k) >> 8) << 5)
                      | (((p & 31) * k) >> 8));
}

/* Columns 0..n-1 of h rows (stride in pixels) darkened by alpha[c] */
static inline void jmix_shade_cols(uint16_t *px, int stride, int h,
                                   const uint8_t *alpha, int n)
{
    int c;

    for (; h > 0; h--, px += stride)
        for (c = 0; c < n; c++)
            px[c] = jmix_shade_px(px[c], alpha[c]);
}

#endif /* BCMFW_JMIX_H */
