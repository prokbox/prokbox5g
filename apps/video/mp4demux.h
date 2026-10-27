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

/* MP4/M4V demuxer for the BCM2722 video path (#60, #64;
   docs/research/bcm2722/player.md, "MP4/M4V demux").  Pure logic: no
   Rockbox headers, no allocation, no file API.  The caller supplies a
   read_at callback and a scratch buffer, so the same code runs on the PP,
   in the simulator and in the host test (tests/video/run.sh), which checks
   it byte for byte against tests/video/mp4demux.py.

   Nothing is read whole: mp4_open() walks box headers and reads the few
   small boxes it needs; the sample iterator streams the six sample-table
   boxes through windows carved from the scratch buffer. */

#ifndef MP4DEMUX_H
#define MP4DEMUX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reads len bytes at off into buf; returns the bytes read (short only at
   the end of the file) or < 0 on an error. */
typedef long (*mp4_read_fn)(void *ctx, uint64_t off, void *buf,
                            unsigned long len);

struct mp4_io
{
    mp4_read_fn read_at;
    void *ctx;
    uint64_t size;              /* file size in bytes */
};

enum mp4_err
{
    MP4_OK = 0,
    MP4_E_IO = -1,              /* read_at failed or came up short */
    MP4_E_FORMAT = -2,          /* malformed or truncated box */
    MP4_E_FRAGMENTED = -3,      /* moof/mvex: not supported */
    MP4_E_NO_MOOV = -4,
    MP4_E_NO_AVC = -5,          /* no 'vide' track with an avc1/avc3 entry */
    MP4_E_EMPTY_EDIT = -6,      /* presentation delay: not supported */
    MP4_E_BAD_AVCC = -7,
    MP4_E_TABLE = -8,           /* sample tables disagree */
    MP4_E_SPACE = -9,           /* caller's buffer too small */
};

/* A box's payload: [pos, end) in the file; end == 0 means absent. */
struct mp4_range
{
    uint64_t pos;
    uint64_t end;
};

struct mp4_movie
{
    const struct mp4_io *io;
    uint32_t width, height;
    uint32_t timescale;         /* mdhd, media ticks per second */
    uint32_t nsamples;          /* stsz's count */
    int64_t shift;              /* elst's first media_time, in ticks */
    uint8_t nal_length;         /* avcC's length size: 1, 2 or 4 */
    bool has_audio;             /* a 'soun' track exists */
    bool co64;                  /* chunk offsets are 64-bit */
    struct mp4_range avcc;      /* avcC payload */
    struct mp4_range stsz, stco, stsc, stts, ctts, stss;
};

struct mp4_sample
{
    uint64_t offset;            /* in the file */
    uint32_t size;
    int64_t pts_us;             /* (dts + cts - shift) * 1e6 / timescale */
    bool key;
};

/* A sequential reader over one box's payload, through a caller window. */
struct mp4_cursor
{
    const struct mp4_io *io;
    uint64_t pos, end;          /* file position of buf[0], payload end */
    uint8_t *buf;
    uint32_t cap, fill, at;
};

struct mp4_iter
{
    const struct mp4_movie *m;
    struct mp4_cursor sz, co, sc, ts, ct, ss;
    uint32_t index;             /* next sample number, 0-based */
    uint32_t fixed_size;        /* stsz's fixed size, 0 if per sample */
    uint32_t chunks;            /* chunk count */
    uint32_t chunk;             /* current chunk, 1-based; 0 before the first */
    uint64_t chunk_pos;         /* next sample's offset in the chunk */
    uint32_t chunk_left;        /* samples left in the current chunk */
    uint32_t sc_left;           /* stsc entries not yet read */
    uint32_t cur_first;         /* current stsc entry: first chunk, 1-based */
    uint32_t cur_per;           /*   and samples per chunk */
    uint32_t nxt_first;         /* pending entry's first chunk, 0 if none */
    uint32_t nxt_per;
    uint32_t ts_left;           /* stts entries not yet read */
    uint32_t ts_run;            /* samples left in the current stts run */
    uint32_t ts_delta;
    uint64_t dts;
    uint32_t ct_left;
    uint32_t ct_run;
    int64_t ct_off;
    bool ct_signed;
    uint32_t ss_left;
    uint32_t ss_next;           /* next sync sample, 1-based; 0 if none */
    bool all_key;               /* no stss */
};

/* The smallest scratch mp4_iter_init() accepts: six windows of 64 bytes.
   On the PP use a few KB (fewer, larger reads). */
#define MP4_SCRATCH_MIN (6 * 64)

int mp4_open(struct mp4_movie *m, const struct mp4_io *io);

int mp4_iter_init(struct mp4_iter *it, const struct mp4_movie *m,
                  void *scratch, size_t scratch_len);
/* 1 and *s filled, 0 at the end, < 0 an mp4_err. */
int mp4_iter_next(struct mp4_iter *it, struct mp4_sample *s);

/* SPS and PPS from avcC as Annex B (each with a 00 00 00 01 start code),
   ready to go in front of the first packet and after every seek.  Needs
   cap >= 2 * avcC length; returns the bytes written or < 0. */
long mp4_params(const struct mp4_movie *m, uint8_t *out, size_t cap);

/* One sample's [length][NAL]... as Annex B, in place: with a 4-byte
   length prefix the size does not change.  Other lengths need out != in
   and cap >= len + 3 * NAL count; returns the bytes written or < 0. */
long mp4_annexb(const uint8_t *in, size_t len, int nal_length,
                uint8_t *out, size_t cap);

#endif /* MP4DEMUX_H */
