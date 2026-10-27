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

/* jscan.h's fast scan against its definition, on the Mac and in vc2sim
 * (bcmfw/tests/jscan_test.py): a buffer fed in pseudo-random chunks (0 to
 * 63 bytes, sometimes up to 1023), some of them skipped as the decoder's
 * lseek skips (ff cleared, the bytes unseen), from a random start state;
 * after every chunk the two states must agree.
 * Mac: jscantest FILE SEEDS prints the chunks that differ, summed. */
#include <stdint.h>
#include "jscan.h"

static uint32_t rnd(uint32_t *x)
{
    *x ^= *x << 13;
    *x ^= *x >> 17;
    *x ^= *x << 5;
    return *x;
}

int jscan_check(const uint8_t *buf, uint32_t len, uint32_t seed)
{
    struct jscan a, b;
    uint32_t x = seed * 2654435761u | 1, at = seed & 3, r, n;
    int bad = 0;

    r = rnd(&x);
    a.after_sos = b.after_sos = r & 1;
    a.eoi = b.eoi = r >> 1 & 1;
    a.ff = b.ff = r >> 2 & 1;
    while (at < len)
    {
        r = rnd(&x);
        n = (r >> 8 & 7) ? r & 63 : r >> 12 & 1023;
        if (n > len - at)
            n = len - at;
        if (!(r >> 24 & 15))
            a.ff = b.ff = 0;            /* a skip */
        else
        {
            jscan_ref(&a, buf + at, n);
            jscan_bytes(&b, buf + at, n);
            bad += a.after_sos != b.after_sos || a.eoi != b.eoi || a.ff != b.ff;
        }
        at += n;
    }
    return bad;
}

#ifndef __vc4__
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    static uint8_t buf[1 << 20];
    FILE *f = argc == 3 ? fopen(argv[1], "rb") : NULL;
    uint32_t len, s, seeds;
    long bad = 0;

    if (!f)
        return 2;
    len = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    seeds = strtoul(argv[2], NULL, 0);
    for (s = 1; s <= seeds; s++)
        bad += jscan_check(buf, len, s);
    printf("%ld\n", bad);
    return 0;
}
#endif
