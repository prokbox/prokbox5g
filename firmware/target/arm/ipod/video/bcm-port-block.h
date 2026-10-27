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

/* The host port's block write: whole words only, an odd last halfword by a
 * read-modify-write of its word (docs/research/bcm2722/alignment-audit.md).
 */
#ifndef BCM_PORT_BLOCK_H
#define BCM_PORT_BLOCK_H

#include <stdint.h>

/* The port's rules for a write [silicon, sitting 2, 2026-10-25, bench
 * rows ALGNP; memory-access.md section 11]:
 *  - the write address ignores bits 1:0 (PWD2, PHD2, PAD2 land 2 bytes
 *    low): a block must start on a word (the caller refuses, counted);
 *  - a lone trailing halfword is lost: an odd number of halfwords leaves
 *    the last one unwritten once a new address follows (PLH1/3/5), while a
 *    whole word lands exactly (PLW3).
 * And the PP's side: lcd_write_data()'s ldm/stm loop takes an even count
 * from a word-aligned source only (an ldm from 2 mod 4 reads from the
 * address rounded down on the ARM7TDMI).
 *
 * So n halfwords at a word address go as:
 *  - the even part: one address, then words (the fast loop from an
 *    aligned source, else each word built from two halfwords);
 *  - an odd last halfword: its word read, the low half replaced, the word
 *    written back (Apple's osos sends a lone 16-bit tail, which silicon
 *    loses; bcmjob-core.c write_row merges odd ends the same way).
 * Nothing past the block's last halfword is changed.
 *
 * The includer defines, before including this file, the port primitives
 * (all under the caller's port lock; nothing here is per-core state):
 *   int  bpb_addr(uint32_t a)          set the write address, 0 or -1
 *   void bpb_words(const uint16_t *s, uint32_t n)  n even, s word-aligned
 *   void bpb_word(uint32_t v)          one data word
 *   int  bpb_rd(uint32_t a, uint32_t *v)   a word read, 0 or -1
 *   int  bpb_wr(uint32_t a, uint32_t v)    a word write (its own address)
 * A host test (tests/bcmport) includes it against a model of the port. */
static inline int bcm_port_block(uint32_t addr, const uint16_t *src,
                                 uint32_t n)
{
    uint32_t even = n & ~1u, k, v;

    if (addr & 3)
        return -1;
    if (even)
    {
        if (bpb_addr(addr) < 0)
            return -1;
        if (!((uintptr_t)src & 3))
            bpb_words(src, even);
        else
            for (k = 0; k < even; k += 2)
                bpb_word(src[k] | (uint32_t)src[k + 1] << 16);
    }
    if (n & 1)
    {
        addr += 2 * even;
        if (bpb_rd(addr, &v) < 0 ||
            bpb_wr(addr, (v & 0xFFFF0000u) | src[even]) < 0)
            return -1;
    }
    return 0;
}

#endif /* BCM_PORT_BLOCK_H */
