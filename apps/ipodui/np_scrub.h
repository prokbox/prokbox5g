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
 * iPod UI shell: Now Playing scrub maths (wheel -> position, chapter
 * snapping, "N of M", whole-book progress and chapter ticks)
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
#ifndef _IPODUI_NP_SCRUB_H_
#define _IPODUI_NP_SCRUB_H_

/* Pure C: no Rockbox headers, no kernel calls, no statics, integer maths
 * only (host-tested by tests/nowplaying/). Times are in ms, the wheel is
 * in clicks (one clickwheel driver event, 15 degrees) and clicks/s, as
 * for ui_wheel_steps() in scroll.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Slow scrub: 1 s per click. Above NP_SCRUB_ACCEL_V clicks/s each further
 * NP_SCRUB_ACCEL_DV clicks/s doubles the step, up to 2^NP_SCRUB_ACCEL_MAX
 * (64 s per click). */
#define NP_SCRUB_BASE_MS     1000
#define NP_SCRUB_ACCEL_V     12     /* clicks/s: same knee as ui_wheel_steps */
#define NP_SCRUB_ACCEL_DV    8
#define NP_SCRUB_ACCEL_MAX   6
/* Audiobooks: at or above this wheel speed every click snaps to the next
 * (or previous) chapter start instead (owner, 2026-10-06: "a fast wheel
 * snaps between chapter checkpoints"). ~1 turn/s. Tune on the device. */
#define NP_SCRUB_SNAP_V      24

/* Chapter starts, read in place: 'count' uint32_t (ms from the start of
 * the file or the book), 'stride' bytes apart (P6 D4: uint32_t, the
 * packed table's column). This takes the start_ms column of a packed
 * struct audiobook_chapters without a copy
 *   { &ab_ch_start(ab, 0), sizeof(struct ab_ch_entry), ab->chapter_count }
 * or a plain array (stride = sizeof(uint32_t)), e.g. the cumulative
 * track starts of a multi-file book. Starts need not be sorted, and may
 * be past the end; count may be 0. The pointer is held, not copied: if
 * the table lives in a movable core_alloc buffer, pin it while it is
 * current, or re-set it (np_set_scrub()) after anything that yields. */
struct np_chapters {
    const uint32_t *start;
    size_t stride;
    int count;
};

struct np_scrub_ctx {
    unsigned long length_ms;        /* whole track / book; 0 = unknown */
    struct np_chapters ch;          /* count 0 for music */
    bool snap;                      /* audiobook: fast wheel snaps */
    /* P6 §3.4, a multi-file book (positions are book-relative): more snap
     * points, the playing file's chapters at sub_base + start (ch then
     * holds the book's file starts). count 0 (memset) = none: songs and
     * single files snap on ch alone, exactly as in Phase 5. */
    struct np_chapters sub;
    unsigned long sub_base;
};

/* i-th chapter start (no bounds check) */
static inline unsigned long np_chapter_start(const struct np_chapters *ch,
                                             int i)
{
    return *(const uint32_t *)((const char *)ch->start
                               + (size_t)i * ch->stride);
}

/* ms moved per click at 'velocity' clicks/s (slow scrub with acceleration) */
unsigned long np_scrub_step_ms(unsigned velocity);

/* New position after a wheel event of 'clicks' (signed, + = clockwise =
 * forward) at 'velocity' clicks/s, from 'pos_ms'. Clamped to
 * [0, length_ms]. With ctx->snap, at least one chapter start in range
 * (ch, or sub at sub_base: the union of both is the snap set) and
 * velocity >= NP_SCRUB_SNAP_V, each click moves to the next chapter
 * start after pos (forward), or to the start of the current chapter, or
 * the previous one when already on it (backward); when there is no
 * chapter start further in that direction the click moves to the end
 * (forward) or 0 (backward). Returns pos_ms unchanged if length_ms is 0. */
unsigned long np_scrub_wheel(const struct np_scrub_ctx *ctx,
                             unsigned long pos_ms, int clicks,
                             unsigned velocity);

/* Chapter at 'pos_ms', 0-based in time order (the rank of the latest
 * chapter start <= pos; ties by array order), for "N of M" as
 * N = result + 1, M = ch->count. Positions before the first start count
 * as chapter 0; positions past the end as the last. -1 if count is 0.
 * '*arr_idx' (may be NULL) gets the array index of that chapter. */
int np_chapter_at(const struct np_chapters *ch, unsigned long pos_ms,
                  int *arr_idx);

/* np_chapter_at() for a table sorted by start (as playback's chapter
 * table is: metadata_read_chapters() sorts it), walking from 'hint' (the
 * previous result, or any index; clamped): O(1) per call while playback
 * moves forward, O(distance) after a seek. Returns the array index, which
 * for a sorted table equals np_chapter_at()'s result (ties: the first of
 * equal starts; before the first start: 0); -1 if count is 0. The 1 Hz
 * tick uses this (Revision 1 item 4: <= 15k instructions per tick). */
int np_chapter_step(const struct np_chapters *ch, unsigned long pos_ms,
                    int hint);

/* x (0..w) of pos_ms on a bar of w pixels for a track/book of length_ms.
 * Past the end -> w; length 0 -> 0. */
int np_bar_x(unsigned long pos_ms, unsigned long length_ms, int w);

/* Chapter tick marks on a bar of w pixels: sets bit x of 'bits' (caller
 * supplies (w + 7) / 8 bytes, bit x = bits[x >> 3] & (1 << (x & 7))) for
 * every chapter start strictly inside (0, length_ms). Ticks that land on
 * the same pixel merge, so 648 chapters on a 204 px bar cost nothing
 * extra. Returns the number of distinct pixels set. */
int np_chapter_ticks(const struct np_chapters *ch, unsigned long length_ms,
                     int w, uint8_t *bits);

#endif /* _IPODUI_NP_SCRUB_H_ */
