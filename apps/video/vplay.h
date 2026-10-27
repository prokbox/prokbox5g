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

/* The video player's engine (#64 piece 6; docs/research/bcm2722/player.md
   sections 2-4), stage A of the host protocol: the audio is the master
   clock, sample data goes into the BCM's FIFO ahead of time, and each
   frame's FEED record is posted when the audio clock reaches its PTS minus
   a lead (the BCM decodes on the record and shows the picture when done).

   Pure logic, like audiobook_core.c: no threads, no Rockbox headers.  The
   caller calls vp_tick() on every kernel tick with the audio position and
   the BCM's status, and supplies the side effects as vp_ops: where sample
   bytes come from (bufpeek_at() or the file), the FIFO, the records, the
   session restart.  tests/video/test_vplay.c runs it against a model BCM
   and an audio clock and checks A/V sync, pause, seek, speed and drops. */

#ifndef VPLAY_H
#define VPLAY_H

#include <stdbool.h>
#include <stdint.h>

#define VP_AGAIN        1       /* push: no FIFO room or data not buffered */
#define VP_MAX_IN_FLIGHT 16     /* records the BCM holds (pipeline.md) */

#define VP_LEAD_MIN_US  10000
#define VP_LEAD_MAX_US  200000
#define VP_SYNC_EVERY_US 500000 /* drift check period */
#define VP_SYNC_OK_US   20000   /* under this: leave the lead alone */
#define VP_SYNC_STEP_US 5000    /* lead change per check */
#define VP_SYNC_FAR_US  100000  /* past this: a jump, not a slew */
#define VP_AHEAD_US     2000000 /* push data this far past the records */
/* A picture this late drops the rest of its GOP.  It must stay above any
   constant record-to-screen delay the lead has not yet absorbed: at 50 ms
   a 90 ms panel path reads as slow decoding (test_vplay, 'latency'). */
#define VP_DROP_LATE_US 150000

struct vp_sample
{
    int64_t pts_us;
    uint32_t size;
    bool key;
};

struct vp_key
{
    uint32_t index;             /* sample number */
    int64_t pts_us;
};

struct vp_ops
{
    void *ctx;
    /* Sample i's index record (a .vix window); 0 or < 0. */
    int (*sample)(void *ctx, uint32_t i, struct vp_sample *s);
    /* Copies sample i into the FIFO as Annex B, with the parameter sets in
       front when params; 0, VP_AGAIN, or < 0 to give up on the file. */
    int (*push)(void *ctx, uint32_t i, bool params);
    /* Posts sample i's FEED record; show false decodes without showing
       (pictures before a seek target, frames dropped for speed). */
    void (*post)(void *ctx, uint32_t i, int64_t pts_us, bool show);
    /* Frees sample i's FIFO bytes without decoding it (a drop). */
    void (*skip)(void *ctx, uint32_t i);
    /* Ends the BCM session and opens a new one (stage A has no SEEK). */
    void (*restart)(void *ctx);
};

/* What the BCM reports (STATUS words 11-12, and the records it holds).
   shown_at_us is the audio clock when the picture went up; the PP stamps
   it when it reads a new shown_pts_us. */
struct vp_status
{
    bool shown_valid;
    int64_t shown_pts_us;       /* PTS of the picture on screen */
    int64_t shown_at_us;        /* audio clock when it went up */
    uint32_t in_flight;
};

struct vp_stats
{
    uint32_t posted, shown_posts, skipped, dropped_gops, restarts;
    uint32_t push_waits, lead_changes, late_posts;
};

struct vp
{
    const struct vp_ops *ops;
    const struct vp_key *keys;
    uint32_t nkeys, nsamples;
    uint32_t pushed;            /* samples in the FIFO: [posted, pushed) */
    uint32_t posted;            /* next sample to post (or skip) */
    int64_t lead_us;
    int64_t frame_us;           /* nominal frame duration, from the index */
    int64_t catchup_until_us;   /* after a seek: post at once, unshown */
    int64_t next_sync_us;
    int64_t last_audio_us;
    int speed;                  /* percent, as the audio plays */
    bool params;                /* the next push carries SPS/PPS */
    bool paused;
    bool refresh;               /* after a seek: the picture at the target
                                   goes up even while paused */
    bool dropping;              /* skipping to the next key sample */
    bool synced;                /* the lead has met a status since (re)start */
    int64_t drop_pts_us;        /* last sample posted when a drop began:
                                   status for it or earlier is stale */
    int64_t last_posted_pts_us;
    /* how late each recent record went out against its pts - lead, so a
       status error can be split into posting delay and the BCM's path */
    struct { int64_t pts_us, late_us; } recent[32];
    uint32_t recent_n;
    struct vp_stats stats;
};

/* keys: the key table (sample number and PTS of every key sample, in
   order), kept by the caller for the session. */
void vp_open(struct vp *p, const struct vp_ops *ops, uint32_t nsamples,
             const struct vp_key *keys, uint32_t nkeys, int64_t lead_us);

/* Once per kernel tick (10 ms) with the audio position, edit- and
   priming-adjusted (player.md section 3).  Returns true while frames
   remain. */
bool vp_tick(struct vp *p, int64_t audio_us, const struct vp_status *st);

void vp_pause(struct vp *p, bool paused);
void vp_set_speed(struct vp *p, int percent);

/* The key sample at or before target_us (the first key if none). */
/* How long until the next record is due (us; 0 or less: now; INT64_MAX:
   nothing waits to go out), for the caller's sleep (C5) */
int64_t vp_next_due_us(struct vp *p, int64_t audio_us);

uint32_t vp_key_before(const struct vp_key *keys, uint32_t nkeys,
                       int64_t target_us);

/* Restarts the session from the key sample at or before target_us; the
   pictures between it and the target are decoded unshown.  The caller
   seeks the audio to target_us too.  Returns the key sample number. */
/* The record for pts_us went out extra_us later than the engine took
   (its post did work first: a restart's re-push); sync() allows for it. */
void vp_add_lateness(struct vp *p, int64_t pts_us, int64_t extra_us);

uint32_t vp_seek(struct vp *p, int64_t target_us);

/* No restart: the samples from where the engine is up to target_us go out
   unshown, as a seek's way from its key does (dev42: a resume a little
   past the pictures, when the key before the audio is behind the engine
   and a seek would decode the whole GOP again). */
void vp_catch_up(struct vp *p, int64_t target_us);

#endif /* VPLAY_H */
