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

/* iPod UI shell: CRC-32 four bytes a step, from tables built into a caller's
 * block: the same values as firmware crc_32() and crc_32r() */
#ifndef IPODUI_CRCFAST_H
#define IPODUI_CRCFAST_H

#include <stddef.h>
#include <stdint.h>

/* p081-bootload: the library's boot check CRCs every file of the selected
 * generation (12.7 MB at 50k). Firmware's crc_32()/crc_32r() look up a
 * nibble at a time (two loads a byte); these look up a byte of each of
 * four at once ("slice-by-4": one word load and four table loads a word),
 * about twice as fast on the ARM7TDMI (from the fw-ship disassembly:
 * ~19 cycles a byte for crc_32r(), ~9 here), with byte-identical results,
 * so every stored checksum (manifests, structural sums) stays valid.
 * The tables (8 KB) are built at run time into memory the caller holds
 * for the job (no static table, no resident cost); building them takes
 * about 4,000 steps. Little-endian targets only (the 5G and the sim). */
struct crcfast {
    uint32_t r[4][256];         /* reflected, poly 0xedb88320: crc_32r() */
    uint32_t m[4][256];         /* MSB first, poly 0x04c11db7: crc_32() */
};

void crcfast_init(struct crcfast *t);

/* crc_32r(p, n, crc) */
uint32_t crcfast_r(const struct crcfast *t, const void *p, size_t n,
                   uint32_t crc);

/* crc_32(p, n, crc) */
uint32_t crcfast_m(const struct crcfast *t, const void *p, size_t n,
                   uint32_t crc);

#endif /* IPODUI_CRCFAST_H */
