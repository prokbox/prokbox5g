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

#ifndef BCMFW_JSCAN_H
#define BCMFW_JSCAN_H

/* jdec's truncation rule over the bytes it reads (#125): a stream is whole
 * if an EOI (FF D9) follows its last SOS (FF DA).  An EXIF thumbnail has
 * its own SOS and EOI before the picture's SOS, so only an EOI after the
 * last SOS counts.  The state is fed every chunk the decoder reads, in
 * file order (bytes it skips past the end of its buffer are never seen;
 * a skip clears ff: it never crosses a marker pair).
 *
 * The definition is the byte-wise step, jscan_ref(); jscan_bytes() is
 * the same function, faster: only the byte after an 0xFF can change the
 * state, so the runs between 0xFFs (nearly all of the entropy data, where
 * an 0xFF comes about once in 256 bytes) are skipped 16 bytes a turn
 * with the "has a zero byte" test of Hacker's Delight 6-1 (glibc's and
 * musl's memchr) on ~w, whose ~w - 0x01010101 is 0xFEFEFEFE - w.  That
 * test is exact as a yes/no, and the word that answers yes is walked a
 * byte at a time.  bcmfw/tests/jscan_test.py checks the two agree, on the
 * Mac and in vc2sim, and that its mutations of this file fail. */

#include <stdint.h>

struct jscan
{
    int after_sos;      /* an SOS has passed */
    int eoi;            /* an EOI has passed since the last SOS */
    int ff;             /* the last byte was 0xFF */
};

/* the definition, a byte at a time */
static inline void jscan_ref(struct jscan *s, const uint8_t *p, long n)
{
    long i;

    for (i = 0; i < n; i++)
    {
        if (s->ff && p[i] == 0xDA)
        {
            s->after_sos = 1;
            s->eoi = 0;
        }
        else if (s->ff && p[i] == 0xD9 && s->after_sos)
            s->eoi = 1;
        s->ff = p[i] == 0xFF;
    }
}

/* a word read from a byte buffer (the buffer's own type for aliasing) */
typedef uint32_t __attribute__((may_alias)) jscan_word;

/* the same, the runs between 0xFFs four words a turn */
static inline void jscan_bytes(struct jscan *s, const uint8_t *p, long n)
{
    const uint8_t *e = p + n;
    const jscan_word *w;
    uint32_t b, k = 0xFEFEFEFEu, m = 0x80808080u;
    int ff = s->ff;

    /* both constants in registers: k - w is then one three-operand sub */
    __asm__("" : "+r"(k), "+r"(m));
#define JSCAN_FF(x)     ((k - (x)) & (x))   /* & m: nonzero if an 0xFF */
    while (p < e)
    {
        if (!ff)
        {
            while (((uintptr_t)p & 3) && p < e && *p != 0xFF)
                p++;
            if (!((uintptr_t)p & 3))
            {
                w = (const jscan_word *)p;
                if (e - p >= 16)
                {
                    const jscan_word *last = (const jscan_word *)(e - 16);

                    while (!((JSCAN_FF(w[0]) | JSCAN_FF(w[1]) |
                              JSCAN_FF(w[2]) | JSCAN_FF(w[3])) & m) &&
                           (w += 4) <= last)
                        ;
                }
                while ((const uint8_t *)w + 4 <= e && !(JSCAN_FF(*w) & m))
                    w++;
                p = (const uint8_t *)w;
            }
            while (p < e && *p != 0xFF)
                p++;
            if (p == e)
                break;
        }
        b = *p++;
        if (ff && b == 0xDA)
        {
            s->after_sos = 1;
            s->eoi = 0;
        }
        else if (ff && b == 0xD9 && s->after_sos)
            s->eoi = 1;
        ff = b == 0xFF;
    }
#undef JSCAN_FF
    s->ff = ff;
}

#endif /* BCMFW_JSCAN_H */
