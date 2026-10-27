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

#include <stdint.h>
typedef uint32_t u32;
/* today's COMMIT check (jref.c fnv_px): FNV-1a over the bytes, LE */
u32 h_fnv(const uint16_t *p, u32 n)
{
    u32 h = 0x811C9DC5u, x;
    for (x = 0; x < n; x++) {
        h = (h ^ (p[x] & 0xFF)) * 0x01000193u;
        h = (h ^ (p[x] >> 8)) * 0x01000193u;
    }
    return h;
}
/* Fletcher-32 (mod 65535) over LE halfwords, read as words, deferred
 * reduction every 180 words (360 halfwords: no 32-bit overflow) */
u32 h_fl32(const u32 *p, u32 nw)
{
    u32 s1 = 0xffff, s2 = 0xffff;
    while (nw) {
        u32 k = nw > 180 ? 180 : nw;
        nw -= k;
        do {
            u32 w = *p++;
            s1 += w & 0xffff; s2 += s1;
            s1 += w >> 16;    s2 += s1;
        } while (--k);
        s1 = (s1 & 0xffff) + (s1 >> 16);
        s2 = (s2 & 0xffff) + (s2 >> 16);
    }
    s1 = (s1 & 0xffff) + (s1 >> 16);
    s2 = (s2 & 0xffff) + (s2 >> 16);
    return s2 << 16 | s1;
}
/* Fletcher-style, words, both sums mod 2^32 (has a blind spot) */
u32 h_f64(const u32 *p, u32 nw)
{
    u32 a = 0, b = 0;
    while (nw--) { a += *p++; b += a; }
    return b ^ (a << 16 | a >> 16);
}
#define P1 2654435761u
#define P2 2246822519u
#define P3 3266489917u
#define P4 668265263u
#define P5 374761393u
#define RL(x, r) (((x) << (r)) | ((x) >> (32 - (r))))
u32 h_xxh32(const u32 *p, u32 nw)
{
    u32 v1 = P1 + P2, v2 = P2, v3 = 0, v4 = -P1, h;
    u32 n = nw;
    while (n >= 4) {
        v1 = RL(v1 + p[0] * P2, 13) * P1;
        v2 = RL(v2 + p[1] * P2, 13) * P1;
        v3 = RL(v3 + p[2] * P2, 13) * P1;
        v4 = RL(v4 + p[3] * P2, 13) * P1;
        p += 4; n -= 4;
    }
    h = RL(v1, 1) + RL(v2, 7) + RL(v3, 12) + RL(v4, 18);
    h += nw * 4;
    h ^= h >> 15; h *= P2; h ^= h >> 13; h *= P3; h ^= h >> 16;
    return h;
}
/* unrolled forms (same results) */
u32 h_fl32u(const u32 *p, u32 nw)  /* nw multiple of 4 */
{
    u32 s1 = 0xffff, s2 = 0xffff;
    while (nw) {
        u32 k = nw > 180 ? 180 : nw;
        nw -= k;
        for (; k; k -= 4, p += 4) {
            u32 w0 = p[0], w1 = p[1], w2 = p[2], w3 = p[3];
            s1 += w0 & 0xffff; s2 += s1; s1 += w0 >> 16; s2 += s1;
            s1 += w1 & 0xffff; s2 += s1; s1 += w1 >> 16; s2 += s1;
            s1 += w2 & 0xffff; s2 += s1; s1 += w2 >> 16; s2 += s1;
            s1 += w3 & 0xffff; s2 += s1; s1 += w3 >> 16; s2 += s1;
        }
        s1 = (s1 & 0xffff) + (s1 >> 16);
        s2 = (s2 & 0xffff) + (s2 >> 16);
    }
    s1 = (s1 & 0xffff) + (s1 >> 16);
    s2 = (s2 & 0xffff) + (s2 >> 16);
    return s2 << 16 | s1;
}
u32 h_f64u(const u32 *p, u32 nw)
{
    u32 a = 0, b = 0;
    for (; nw; nw -= 4, p += 4) {
        a += p[0]; b += a; a += p[1]; b += a;
        a += p[2]; b += a; a += p[3]; b += a;
    }
    return b ^ (a << 16 | a >> 16);
}
/* "F16w": Fletcher over LE 16-bit halfwords, both sums mod 2^32 (no
 * reductions); 64-bit state, folded for a 32-bit reply */
u32 h_f16w(const u32 *p, u32 nw)  /* nw multiple of 4 */
{
    u32 a = 0, b = 0;
    for (; nw; nw -= 4, p += 4) {
        u32 w0 = p[0], w1 = p[1], w2 = p[2], w3 = p[3];
        a += w0 & 0xffff; b += a; a += w0 >> 16; b += a;
        a += w1 & 0xffff; b += a; a += w1 >> 16; b += a;
        a += w2 & 0xffff; b += a; a += w2 >> 16; b += a;
        a += w3 & 0xffff; b += a; a += w3 >> 16; b += a;
    }
    return b ^ (a << 16 | a >> 16);
}
