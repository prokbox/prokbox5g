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
 * The PP's second DMA engine (0x6000_8000) writing the BCM's host port:
 * one piece at a time, for the UI's screen pushes (lcd-video.c) and the
 * video player's blocks (bcm-devtest-hal.c).  docs/pp5022-reference/
 * dma.md section 6; session 37's lab: every word lands, 256 B to 64 KB.
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
#ifndef _E2_PORT_H_
#define _E2_PORT_H_

#include <stdbool.h>
#include <stdint.h>

/* Who programs the engine: one owner at a time, claimed for a piece (or
 * a run of pieces) and released after its end */
enum e2port_owner
{
    E2PORT_FREE = 0,
    E2PORT_LCD,         /* the UI's screen pushes */
    E2PORT_VIDEO,       /* the video player's blocks */
};

#define E2PORT_PIECE_MAX  0x10000   /* the port's 64 KB window */
#define E2PORT_TIMEOUT_US 100000    /* a piece that runs longer is stopped */

/* true if the engine was free (or already this owner's) and is now.
 * Cross-core under HAVE_COP_COHERENCE (an uncached owner word under a
 * corelock).  A claim is not the port lock: the caller also holds the
 * port lock for each piece (lcd-video.c, "THE PORT LOCK"). */
bool e2port_claim(enum e2port_owner who);
void e2port_release(enum e2port_owner who);

/* Can this block go by the engine: a word-aligned source in DRAM (not
 * IRAM: unproven), 4 bytes to 64 KB, a multiple of 4 */
bool e2port_fits(const void *src, uint32_t bytes);

/* One piece into the BCM at bcm_addr: the write address as two
 * halfwords, the port's write side ready, the whole data cache written
 * back, then the engine started.  The engine reads src by its uncached
 * address; the write-back here is the RUNNING core's only: pixels written
 * by the other core must have been published by it first (xcore_publish(),
 * the hand-off contract in xcore.h), and src should be 16-byte aligned at
 * both ends when another core produced it.  The caller holds the engine and makes
 * no other port access until e2port_busy() says the piece is through.
 * 0 started; -1 the port never got ready (nothing started) */
int e2port_start(uint32_t bcm_addr, const void *src, uint32_t bytes);

/* true while the started piece is still moving */
bool e2port_busy(void);

/* A piece that never ended: stop the engine (interrupts off for the two
 * writes) */
void e2port_stop(void);

/* After the piece: the port's write side ready for the next address;
 * false if it never became ready */
bool e2port_port_ready(void);

#endif /* _E2_PORT_H_ */
