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

/* The per-picture trace of a playback, for the device test's stage 6
   (device-session-1.md, "Session 9"): one record each time the picture on
   screen changes (STATUS), the summary and verdict, and the log lines.
   Pure: no I/O, no allocation (the caller gives the records). */

#ifndef VTRACE_H
#define VTRACE_H

#include <stdbool.h>
#include <stdint.h>

/* What the core saw at one STATUS whose shown count moved */
struct vpc_frame
{
    int64_t pts_us;             /* the picture now on screen */
    int64_t audio_us;           /* the audio clock at that STATUS */
    uint32_t shown, decoded;    /* the image's counts since OPEN */
    uint32_t work_us;           /* the last FEED's time on the chip */
    uint32_t work_max_us;       /* the longest since OPEN */
    uint32_t mhz;               /* the core clock (27 or 150), 0 unknown */
    uint32_t skipped, dropped_gops, late_posts;  /* the engine's totals */
};

struct vtrace_rec
{
    uint32_t pts_ms;
    int32_t drift_us;           /* audio clock minus the picture's PTS */
    uint32_t work_us;
    uint16_t mhz;
    uint16_t shown;             /* the image's count, low 16 bits */
};

struct vtrace
{
    struct vtrace_rec *rec;
    uint32_t cap, n;
    uint32_t lost;              /* records past cap (counted, not kept) */
    struct vpc_frame last;      /* the latest frame, kept or not */
    bool any;
    /* the image counts per session (OPEN restarts them): summed here */
    uint32_t shown_total, decoded_total;
};

/* The verdict's bounds: after the start's settling, every picture within
   VTRACE_DRIFT_US of the audio, no skipped picture and no dropped GOP.
   Two seconds (sitting 4c): stage7.m4v's start reached 58 ms at 1083 ms,
   over 50 ms until 1.3 s and under 45 ms from 1.4 s on, and the owner
   saw it smooth and in sync; one second cut into that transient */
#define VTRACE_SETTLE_MS    2000
#define VTRACE_DRIFT_US     50000

struct vtrace_sum
{
    uint32_t records, lost;
    uint32_t first_ms, last_ms;
    uint32_t settled;           /* records after VTRACE_SETTLE_MS */
    int32_t drift_max_us;       /* the largest |drift| after settling */
    int32_t drift_mean_us;      /* the mean |drift| after settling */
    uint32_t work_mean_us, work_max_us;
    uint32_t mhz_lo, mhz_hi;    /* the clocks seen */
    uint32_t shown, decoded;
    uint32_t skipped, dropped_gops, late_posts;
    /* Frame pacing, after settling: between two records the screen moved
       on by (audio_i - audio_i-1) against the pictures' own (pts_i -
       pts_i-1), so the difference is drift_i - drift_i-1.  A hitch is a
       picture that came more than half its interval (and at least
       VTRACE_HITCH_US) late against the one before: a visible stutter.
       Informational: the records are taken at STATUS polls (a tick
       apart), so a few ms of it is the poll, not the screen. */
    uint32_t intervals;
    int32_t jitter_max_us, jitter_mean_us;
    uint32_t hitches;
    uint32_t hitch_ms;          /* the first hitch's picture, 0 none */
    bool pass;
};

#define VTRACE_HITCH_US     15000

void vtrace_init(struct vtrace *t, struct vtrace_rec *rec, uint32_t cap);
void vtrace_add(struct vtrace *t, const struct vpc_frame *f);

/* ended: the player reached the last picture; failed: it gave up */
void vtrace_summary(const struct vtrace *t, bool ended, bool failed,
                    struct vtrace_sum *s);

/* The pace row (devbench "vfeed"): the FEED time that pct percent of
   the records stay at or under (us; 0 without records), the clip's frame
   time from its pictures' PTS and the image's shown count (us; 0 if
   unknown), and the row's rule: the summary passes, the FEED p95 under
   VTRACE_PACE_PCT percent of the frame, nothing skipped or dropped, every
   record at mhz (0: any clock) */
#define VTRACE_PACE_PCT     75
uint32_t vtrace_work_pct(const struct vtrace *t, unsigned pct);
uint32_t vtrace_frame_us(const struct vtrace *t);
bool vtrace_pace_ok(const struct vtrace_sum *s, uint32_t p95_us,
                    uint32_t frame_us, uint32_t mhz);

/* The log lines: line(ctx, text) once per line, no newline.  The
   summary first ("stage 6 ..."), then one "V6 ..." line per record. */
void vtrace_lines(const struct vtrace *t, const struct vtrace_sum *s,
                  void (*line)(void *ctx, const char *text), void *ctx);

#endif /* VTRACE_H */
