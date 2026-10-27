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
 * bcmfw/aac in the video image (BCMFW_AAC, M4): where the decoder's far
 * data goes, and the 32-bit bcs/bcc probe.  The protocol side is
 * hostproto.c's HP_OP_AAC.
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
#ifndef BCMFW_AAC_IMAGE_H
#define BCMFW_AAC_IMAGE_H

#include <stdint.h>

/* Once per boot, at the first H.264 OPEN (SDRAM up, the decoder arena
 * not yet used): the tables crt0 parked at the arena's top go to SDRAM
 * (.xdata), and the channel elements (.sdbss) are cleared. */
void aac_image_place(void);

/* 1 when the tables are in SDRAM and still read back as they were copied
 * (a later diagnostic OPEN's memory test overwrites them: the image must
 * be booted again) */
int aac_image_ready(void);

/* dev42 (session 35's stage 9 failed): what the first OPEN copied, for the
 * host to check against the image file.  Returns the FNV-1a of the parked
 * copy as it was taken; *load and *bytes say where .xdata sits in the
 * load image (the image loads at 0), *now the FNV-1a of SDRAM's copy. */
uint32_t aac_image_info(uint32_t *load, uint32_t *bytes, uint32_t *now);

/* The 32-bit bcs/bcc probe (brprobe.S): AAC_PROBE_CASES compares, each
 * followed by a far bcs and a far bcc (the 32-bit forms), as the
 * decoder's unsigned compares are built.  Returns the taken bits: bcs in
 * bits 15:0, bcc in 31:16, one per case.  AAC_PROBE_WANT is what vc2sim
 * models (cmp sets C as a borrow, quirks.md #7: bcs taken when a < b
 * unsigned, bcc otherwise); bcmfw/tests/video_test.py checks it against
 * Python's arithmetic and vc2sim. */
#define AAC_PROBE_CASES 16
#define AAC_PROBE_WANT  0x9DAD6252u
uint32_t aac_image_probe(void);

#endif /* BCMFW_AAC_IMAGE_H */
