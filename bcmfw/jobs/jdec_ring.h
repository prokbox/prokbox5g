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

#ifndef BCMFW_JDEC_RING_H
#define BCMFW_JDEC_RING_H

/* jdec's source over an input ring (J_OP_DECODE with J_DEC_RING; S3b,
 * docs/research/bcm2722/album-render.md section 4): the PP streams the
 * file through a ring in BCM memory, so a 2.6 MB cover needs no 2.6 MB
 * slot.  Two shared words carry the indices (jobs.h J_DEC_IN_RD and
 * J_DEC_IN_WR, HP_ENC'd, pass << 12 | 64-byte units mod 4096):
 *   - the BCM publishes the pass it reads and the units it has consumed;
 *     the PP may write up to a ring's worth ahead of that;
 *   - the PP publishes the pass it writes and the units written (the last
 *     one partial), or J_DEC_ABORT.
 * jdec reads the file from byte 0 once per pass (the probe, then each
 * output, jdec.h).  A file that fits the ring streams once and later
 * passes read it again in place; a longer one asks the PP for a new pass,
 * which starts the file again at ring byte 0.  A stall of J_DEC_STALL_MS
 * with no new bytes, or an abort, is a read error (jdec: CORRUPT). */

#include <stdint.h>
#include "jdec.h"

struct jdec_ring
{
    uint8_t *buf;                   /* the ring's bytes */
    uint32_t bytes;                 /* a power of two, 4-128 KB */
    volatile uint32_t *rd_word;     /* BCM-written */
    volatile uint32_t *wr_word;     /* PP-written */
    uint32_t (*now_us)(void);       /* a 1 MHz clock (stall limit) */
    uint32_t len;                   /* the file's bytes */
    uint32_t pos;                   /* read this pass */
    uint32_t have;                  /* units written this pass (absolute) */
    uint32_t done;                  /* units consumed this pass (absolute) */
    uint32_t pass;                  /* 0 .. J_DEC_PASSES - 1 */
    uint32_t used;                  /* passes read since jdec_ring_src() */
    int err;                        /* stalled or aborted: sticky */
};

/* The ring's geometry is fine for a source (power of two, 4-128 KB) */
int jdec_ring_ok(uint32_t bytes);

/* A source over r for a file of len bytes: a new pass (the next after
 * the one *rd_word names), published at once.  r's buf, bytes, the words
 * and now_us are the caller's. */
void jdec_ring_src(struct jdec_src *src, struct jdec_ring *r, uint32_t len);

#endif
