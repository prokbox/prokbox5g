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

/* ShellCJK sparse font form (#235): the format both firmware/font.c (the
 * companion draw path) and apps/ipodui/libglyph.c (the library glyph
 * file) read, and apps/ipodui/fonts/gen_cjk.py writes. Pure C: no
 * firmware header, so the host tests compile it.
 *
 * File (little-endian):
 *   0     the 36-byte RB12 header; depth = 1 | FONT_SPARSE_DEPTH, so a
 *         stock loader refuses it; firstchar FONT_SPARSE_FIRST, size
 *         FONT_SPARSE_SIZE, bits_size count x stride, no offset or width
 *         tables;
 *   36    struct font_sparse_sub (16 B);
 *   52    the presence bitmap, FONT_SPARSE_BITMAP bytes: bit b of byte
 *         b >> 3 (LSB first) set when the code point of bit index b has a
 *         glyph (font_sparse_bit());
 *   5512  cumulative counts, FONT_SPARSE_BLOCKS x uint16: glyphs before
 *         each block of 256 bits;
 *   FONT_SPARSE_DATA  records, 'stride' bytes each, in bit order: one
 *         width byte, then the 4-bit cell (row-major at the glyph's own
 *         width, 0 = full ink, 15 = paper), zero-padded to the stride.
 * A miss is one read of one record at DATA + rank x stride.
 *
 * The windows: U+3000..U+D7A3 (kana, CJK punctuation, the ideographs,
 * Hangul), U+F900..U+FAFF (the compatibility ideographs, 﨑) and
 * U+FF00..U+FFEF (full-width ASCII, half-width kana). */
#ifndef _FONT_SPARSE_H_
#define _FONT_SPARSE_H_

#include <stdint.h>

#define FONT_SPARSE_DEPTH   0x100       /* flag in the RB12 depth field */
#define FONT_SPARSE_FIRST   0x3000
#define FONT_SPARSE_SIZE    (0xFFF0 - 0x3000)
#define FONT_SPARSE_BITS    (42916 + 512 + 240)         /* 43,668 */
#define FONT_SPARSE_BITMAP  5460        /* (BITS + 7) / 8, padded to 4 */
#define FONT_SPARSE_BLOCKS  172         /* (BITS + 255) / 256, padded */
#define FONT_SPARSE_SUBOFF  36
#define FONT_SPARSE_MAPOFF  52
#define FONT_SPARSE_CNTOFF  (FONT_SPARSE_MAPOFF + FONT_SPARSE_BITMAP)
#define FONT_SPARSE_DATA    (FONT_SPARSE_CNTOFF + 2 * FONT_SPARSE_BLOCKS)
/* the resident index: the bitmap and the counts, as in the file */
#define FONT_SPARSE_INDEX   (FONT_SPARSE_BITMAP + 2 * FONT_SPARSE_BLOCKS)
#define FONT_SPARSE_MAGIC   0x53524853u /* "SHRS" */

struct font_sparse_sub {
    uint32_t magic;             /* FONT_SPARSE_MAGIC */
    uint32_t count;             /* glyph records */
    uint32_t stride;            /* bytes per record: 1 + the cell */
    uint32_t font_id;           /* CRC-32 (zlib) of bytes 0..47, the
                                   bitmap and the records: names this
                                   exact font */
};

/* the bit index of code point c, -1 outside the windows */
static inline int font_sparse_bit(uint32_t c)
{
    if (c >= 0x3000 && c < 0xD7A4)
        return (int)(c - 0x3000);
    if (c >= 0xF900 && c < 0xFB00)
        return 42916 + (int)(c - 0xF900);
    if (c >= 0xFF00 && c < 0xFFF0)
        return 43428 + (int)(c - 0xFF00);
    return -1;
}

static inline int font_sparse_has(const uint8_t *map, int bit)
{
    return bit >= 0 && (map[bit >> 3] >> (bit & 7)) & 1;
}

/* glyphs before bit 'bit' (its record number when it is present) */
static inline int font_sparse_rank(const uint8_t *map, const uint16_t *cnt,
                                   int bit)
{
    static const uint8_t nib[16] = { 0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4 };
    int r = cnt[bit >> 8];
    int i = (bit >> 8) << 5, end = bit >> 3;
    unsigned last;

    for (; i < end; i++)
        r += nib[map[i] & 15] + nib[map[i] >> 4];
    last = map[end] & ((1u << (bit & 7)) - 1);
    return r + nib[last & 15] + nib[last >> 4];
}

/* CRC-32 as zlib computes it (reflected 0xEDB88320; start 0, chain the
 * result): the font id, and libglyph's header sum. Bitwise: headers and
 * one bitmap only. */
static inline uint32_t font_sparse_crc(uint32_t crc, const void *p,
                                       unsigned long n)
{
    const uint8_t *b = p;
    int k;

    crc = ~crc;
    while (n--)
    {
        crc ^= *b++;
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

#endif /* _FONT_SPARSE_H_ */
