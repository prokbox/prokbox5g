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

#ifndef VIZ_TAP_CORE_H
#define VIZ_TAP_CORE_H

/* The visualizer's PCM tap, the pure part (docs/research/bcm2722/
 * visualizer.md section 11): a ring of the stereo frames the mixer hands
 * the DAC, written from the PCM callback (FIQ on the PP5022) and read by
 * the visualizer, which takes the newest n samples behind the play
 * position as a mono downmix, decimated if the rate wants it, in the
 * split order SPECTRUM and jviz_power_ref() take.  No Rockbox headers:
 * host-tested by tests/viztap/.
 *
 * Lock-free, one writer and one reader:
 *  - the writer (viz_ring_write(), once per mixer frame) first publishes
 *    head = the end of the frame now playing (every frame it wrote
 *    before this one), then copies the new frame, which plays after it.
 *    head is the only word the reader reads, and the writer is its only
 *    writer (one 32-bit store: atomic on the ARM, and on the uncached
 *    alias across cores);
 *  - the reader copies [head - n*dec, head) and reads head again; if the
 *    writer could have reached the oldest frame it copied since, the read
 *    was torn and is retried once.  The ring holds n*dec + 2 frames' worth
 *    (the frame queued behind head, and one more arriving during a read),
 *    so a reader that keeps up never tears.
 * Samples are frames of two 16-bit channels in one 32-bit word, left in
 * the low half (the PP5022's I2S layout, and the simulator's little-endian
 * int16 pairs). */

#include <stdint.h>

struct viz_ring
{
    uint32_t *buf;              /* size frames (a power of 2) */
    uint32_t mask;              /* size - 1 */
    uint32_t fmax;              /* the largest frame a write brings */
    uint32_t w;                 /* frames written: the writer's only */
    volatile uint32_t head;     /* frames playing or played: the end of
                                   the frame now at the DAC; the writer's
                                   only, the reader's to read */
};

/* Ring frames for n samples at decimation dec, mixer frames of up to fmax
 * frames: the power of 2 at or above n * dec + 2 * fmax */
uint32_t viz_ring_frames(uint32_t n, uint32_t dec, uint32_t fmax);
void viz_ring_init(struct viz_ring *r, uint32_t *buf, uint32_t frames,
                   uint32_t fmax);
/* The writer: frames stereo frames of src (frames <= fmax), the next to
 * play after those already written.  Bounded: one copy, no locks. */
void viz_ring_write(struct viz_ring *r, const uint32_t *src, uint32_t frames);

/* The decimation for a mixer rate in Hz: 1 up to 64 kHz (the design
 * wants about 44.1 kHz: 512 samples = 11.6 ms, 86 Hz bins, bands to
 * 16 kHz), 2 up to 128 kHz, else 4.  The iPod Video's DAC tops out at
 * 48 kHz, so on the device it is always 1. */
uint32_t viz_tap_dec(uint32_t rate);

/* What a read found (flags; 0: a fresh window of sound) */
#define VIZ_TAP_STALE       0x01    /* head hasn't moved since the last read:
                                       paused, stopped or between frames */
#define VIZ_TAP_SILENT      0x02    /* every sample within +-VIZ_TAP_SILENT_PEAK */
#define VIZ_TAP_UNDERRUN    0x04    /* fewer than n*dec frames since open: the
                                       oldest samples are zeros */
#define VIZ_TAP_TORN        0x08    /* overwritten during the copy twice: the
                                       samples are zeros */
#define VIZ_TAP_BOOK        0x10    /* an audiobook plays: zeros (viz_tap.c) */
#define VIZ_TAP_CLOSED      0x20    /* not open (viz_tap.c) */

#define VIZ_TAP_SILENT_PEAK 2       /* digital silence or 1-LSB dither */
#define VIZ_TAP_NORM_MAX    8       /* the largest normalising shift */

struct viz_tap_frame
{
    uint32_t head;              /* the head the window ends at */
    uint32_t rate;              /* the samples' rate: the mixer's / dec */
    int16_t  peak;              /* the window's largest |sample| (before
                                   any shift) */
    uint8_t  shift;             /* the left shift applied (VIZ_RD_NORM):
                                   take 32 * shift from each band's
                                   jviz_l2 level to undo it exactly */
};

#define VIZ_RD_NORM         0x1     /* shift the samples up by whole bits,
                                       so the peak is at least half scale
                                       (at most VIZ_TAP_NORM_MAX bits) */

/* The reader: the newest n * dec frames ending at head, downmixed to
 * (L + R) / 2 (floored), each dec in a row averaged (floored), as n
 * samples in split order: out[k] = x[2k], out[n/2 + k] = x[2k + 1] for
 * k < n/2 (n even).  last_head: the head of the previous read (for
 * VIZ_TAP_STALE).  Returns the flags; f filled. */
unsigned viz_ring_read(const struct viz_ring *r, int16_t *out, uint32_t n,
                       uint32_t dec, uint32_t last_head, unsigned opts,
                       struct viz_tap_frame *f);

#endif /* VIZ_TAP_CORE_H */
