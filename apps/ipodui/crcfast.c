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

/* iPod UI shell: CRC-32 four bytes a step (crcfast.h) */
#include "crcfast.h"

void crcfast_init(struct crcfast *t)
{
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t r = i, m = i << 24;
        for (int k = 0; k < 8; k++)
        {
            r = (r >> 1) ^ (0xedb88320u & -(r & 1));
            m = (m << 1) ^ (0x04c11db7u & -(m >> 31));
        }
        t->r[0][i] = r;
        t->m[0][i] = m;
    }
    for (int k = 1; k < 4; k++)
        for (int i = 0; i < 256; i++)
        {
            uint32_t r = t->r[k - 1][i], m = t->m[k - 1][i];
            t->r[k][i] = (r >> 8) ^ t->r[0][r & 0xff];
            t->m[k][i] = (m << 8) ^ t->m[0][m >> 24];
        }
}

uint32_t crcfast_r(const struct crcfast *t, const void *p, size_t n,
                   uint32_t crc)
{
    const unsigned char *b = p;

    while (n && ((uintptr_t)b & 3))
    {
        crc = t->r[0][(crc ^ *b++) & 0xff] ^ (crc >> 8);
        n--;
    }
    const uint32_t *w = (const uint32_t *)b;
    for (; n >= 4; n -= 4)
    {
        /* little-endian: the word's low byte is the first in the stream */
        crc ^= *w++;
        crc = t->r[3][crc & 0xff] ^ t->r[2][(crc >> 8) & 0xff]
            ^ t->r[1][(crc >> 16) & 0xff] ^ t->r[0][crc >> 24];
    }
    b = (const unsigned char *)w;
    while (n--)
        crc = t->r[0][(crc ^ *b++) & 0xff] ^ (crc >> 8);
    return crc;
}

uint32_t crcfast_m(const struct crcfast *t, const void *p, size_t n,
                   uint32_t crc)
{
    const unsigned char *b = p;

    while (n && ((uintptr_t)b & 3))
    {
        crc = t->m[0][((crc >> 24) ^ *b++) & 0xff] ^ (crc << 8);
        n--;
    }
    const uint32_t *w = (const uint32_t *)b;
    for (; n >= 4; n -= 4)
    {
        /* little-endian: the stream's first byte is the word's low one,
         * and meets the CRC's top byte */
        uint32_t x = *w++;
        crc = t->m[3][((crc >> 24) ^ x) & 0xff]
            ^ t->m[2][((crc >> 16) ^ (x >> 8)) & 0xff]
            ^ t->m[1][((crc >> 8) ^ (x >> 16)) & 0xff]
            ^ t->m[0][(crc ^ (x >> 24)) & 0xff];
    }
    b = (const unsigned char *)w;
    while (n--)
        crc = t->m[0][((crc >> 24) ^ *b++) & 0xff] ^ (crc << 8);
    return crc;
}
