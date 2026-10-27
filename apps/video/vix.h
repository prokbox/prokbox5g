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

/* The video sample index, a sidecar written at library-update time
   (docs/research/bcm2722/player.md, "The sample index").  Pure logic like
   mp4demux.c; the file glue (where the index lives, when it is written)
   belongs to the caller.

   Format VIX1, little-endian like the PP:
     header (32 bytes): 'VIX1', version 1, sample count, PTS units per
       second (1000), width | height << 16, avcC offset, avcC length, 0
     record (12 bytes): file offset bits 0-31;
       size (bits 0-23) | flags << 24 (bit 0 key, bits 4-7 offset bits
       32-35, so files up to 64 GB); PTS in ms (a u32 in us would wrap
       after 71 minutes)
   Byte-identical to tests/video/mp4demux.py's --index output. */

#ifndef VIX_H
#define VIX_H

#include <stdint.h>
#include <stddef.h>
#include "mp4demux.h"

#define VIX_HEADER_SIZE 32
#define VIX_RECORD_SIZE 12
#define VIX_VERSION     1
#define VIX_UNITS       1000            /* PTS units per second */
#define VIX_NAME_LEN    16              /* hex digits in vix_name() */
#define VIX_SCRATCH_MIN (MP4_SCRATCH_MIN + VIX_RECORD_SIZE)

/* Writes len bytes; returns 0 or < 0. */
typedef int (*vix_write_fn)(void *ctx, const void *buf, unsigned long len);

struct vix_header
{
    uint32_t count;
    uint16_t width, height;
    uint32_t avcc_off, avcc_len;
};

/* Streams the index for m to write(); scratch serves the sample iterator
   and batches records: at least VIX_SCRATCH_MIN, and 4 KB or more for
   fewer, larger reads and writes.  Returns the bytes
   written or < 0 (an mp4_err; MP4_E_TABLE for a sample the format cannot
   hold: 16 MB or more, past 64 GB, or a negative PTS). */
long vix_write(const struct mp4_movie *m, void *scratch, size_t scratch_len,
               vix_write_fn write, void *ctx);

/* 0 and *h filled, or MP4_E_FORMAT. */
int vix_read_header(const uint8_t *buf, struct vix_header *h);

/* Record i of a window (buf holds whole records). */
void vix_read_record(const uint8_t *rec, struct mp4_sample *s);

/* The index's file name for a video: VIX_NAME_LEN hex digits of a hash
   of its path, size and mtime, so a re-copied file gets a new index and
   stale ones can be swept.  out needs VIX_NAME_LEN + 1 bytes. */
void vix_name(const char *path, uint64_t size, uint32_t mtime, char *out);

#endif /* VIX_H */
