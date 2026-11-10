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

/* p081-bootload: crcfast_r()/crcfast_m() against the firmware's crc_32r()
 * and crc_32() (tests/crcfast/run.sh) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crc32.h"
#include "crcfast.h"

static int fails, checks;

static void check(const char *what, uint32_t got, uint32_t want,
                  size_t off, size_t len, uint32_t seed)
{
    checks++;
    if (got != want && fails++ < 10)
        printf("FAIL %s off=%zu len=%zu seed=%08x got=%08x want=%08x\n",
               what, off, len, (unsigned)seed, (unsigned)got,
               (unsigned)want);
}

int main(void)
{
    static struct crcfast t;
    static unsigned char buf[70000];
    uint32_t seeds[] = { 0xffffffffu, 0, 0x12345678u, 0x80000001u };

    crcfast_init(&t);
    srand(245);
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (unsigned char)rand();

    /* every alignment and short length, every seed */
    for (size_t off = 0; off < 8; off++)
        for (size_t len = 0; len < 67; len++)
            for (unsigned s = 0; s < 4; s++)
            {
                check("r", crcfast_r(&t, buf + off, len, seeds[s]),
                      crc_32r(buf + off, len, seeds[s]), off, len, seeds[s]);
                check("m", crcfast_m(&t, buf + off, len, seeds[s]),
                      crc_32(buf + off, len, seeds[s]), off, len, seeds[s]);
            }
    /* long runs, and a run in pieces (the callers' chunks) */
    for (size_t off = 0; off < 4; off++)
    {
        size_t len = sizeof(buf) - 4;
        uint32_t r = 0xffffffffu, m = 0xffffffffu;
        check("r long", crcfast_r(&t, buf + off, len, ~0u),
              crc_32r(buf + off, len, ~0u), off, len, ~0u);
        check("m long", crcfast_m(&t, buf + off, len, ~0u),
              crc_32(buf + off, len, ~0u), off, len, ~0u);
        for (size_t p = 0; p < len; )
        {
            size_t k = 1 + (size_t)rand() % 9000;
            if (k > len - p)
                k = len - p;
            r = crcfast_r(&t, buf + off + p, k, r);
            m = crcfast_m(&t, buf + off + p, k, m);
            p += k;
        }
        check("r pieces", r, crc_32r(buf + off, len, ~0u), off, len, ~0u);
        check("m pieces", m, crc_32(buf + off, len, ~0u), off, len, ~0u);
    }
    /* the standard check values: CRC-32 ("123456789") 0xcbf43926 is
     * crc_32r's complemented; CRC-32/MPEG-2 0x0376e6e7 is crc_32's */
    check("r 123456789", ~crcfast_r(&t, "123456789", 9, ~0u), 0xcbf43926u,
          0, 9, ~0u);
    check("m 123456789", crcfast_m(&t, "123456789", 9, ~0u), 0x0376e6e7u,
          0, 9, ~0u);
    printf("crcfast: %d checks, %d failed\n", checks, fails);
    if (checks < 4000)
    {
        printf("FAIL too few checks\n");
        return 1;
    }
    return fails ? 1 : 0;
}
