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

/* The video player's engine (see vplay.h). */

#include <string.h>
#include "vplay.h"

#define RESYNC  INT64_MIN       /* next_sync_us: set on the next tick */

void vp_open(struct vp *p, const struct vp_ops *ops, uint32_t nsamples,
             const struct vp_key *keys, uint32_t nkeys, int64_t lead_us)
{
    struct vp_sample a, b;

    memset(p, 0, sizeof(*p));
    p->ops = ops;
    p->keys = keys;
    p->nkeys = nkeys;
    p->nsamples = nsamples;
    p->lead_us = lead_us;
    p->speed = 100;
    p->params = true;
    p->catchup_until_us = INT64_MIN;
    p->next_sync_us = RESYNC;

    p->frame_us = 33333;
    if (nsamples > 1 && ops->sample(ops->ctx, 0, &a) == 0 &&
        ops->sample(ops->ctx, 1, &b) == 0 && b.pts_us > a.pts_us)
        p->frame_us = b.pts_us - a.pts_us;
    p->drop_pts_us = INT64_MIN;
}

int64_t vp_next_due_us(struct vp *p, int64_t audio_us)
{
    struct vp_sample s;

    if (p->paused || p->posted >= p->pushed ||
        p->ops->sample(p->ops->ctx, p->posted, &s) < 0)
        return INT64_MAX;
    /* catch-up records and the slideshow's skips go at once */
    if (s.pts_us < p->catchup_until_us || p->dropping ||
        (p->speed > 200 && !s.key))
        return 0;
    return s.pts_us - p->lead_us - audio_us;
}

uint32_t vp_key_before(const struct vp_key *keys, uint32_t nkeys,
                       int64_t target_us)
{
    uint32_t lo = 0, hi = nkeys;

    if (nkeys == 0)
        return 0;
    /* the last key with pts <= target */
    while (hi - lo > 1)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        if (keys[mid].pts_us <= target_us)
            lo = mid;
        else
            hi = mid;
    }
    return keys[lo].index;
}

uint32_t vp_seek(struct vp *p, int64_t target_us)
{
    uint32_t k = vp_key_before(p->keys, p->nkeys, target_us);

    p->ops->restart(p->ops->ctx);
    p->stats.restarts++;
    p->pushed = p->posted = k;
    p->params = true;
    p->dropping = false;
    p->synced = false;
    p->drop_pts_us = INT64_MIN;
    p->catchup_until_us = target_us;
    p->next_sync_us = RESYNC;
    p->refresh = true;
    return k;
}

void vp_catch_up(struct vp *p, int64_t target_us)
{
    p->dropping = false;
    p->synced = false;
    p->drop_pts_us = INT64_MIN;
    p->catchup_until_us = target_us;
    p->next_sync_us = RESYNC;
}

void vp_pause(struct vp *p, bool paused)
{
    p->paused = paused;
    if (!paused)
    {
        p->next_sync_us = RESYNC;   /* the status is stale after a pause */
        p->synced = false;
    }
}

void vp_set_speed(struct vp *p, int percent)
{
    p->speed = percent;
}

/* Data into the FIFO, up to VP_AHEAD_US past the audio clock. */
static bool fill(struct vp *p, int64_t audio_us)
{
    const struct vp_ops *o = p->ops;
    struct vp_sample s;

    while (p->pushed < p->nsamples)
    {
        int rc;
        if (o->sample(o->ctx, p->pushed, &s) < 0)
            return false;
        if (s.pts_us > audio_us + VP_AHEAD_US && p->pushed > p->posted)
            break;
        rc = o->push(o->ctx, p->pushed, p->params);
        if (rc == VP_AGAIN)
        {
            p->stats.push_waits++;
            break;
        }
        if (rc < 0)
            return false;
        p->params = false;
        p->pushed++;
    }
    return true;
}

/* Moves the lead so pictures go up on time.  The first status after an
   open, seek or resume corrects the whole error at once (the path from
   record to screen is a constant of the chip and panel); after that,
   every VP_SYNC_EVERY_US, small errors slew by VP_SYNC_STEP_US and large
   ones jump. */
static void remember(struct vp *p, int64_t pts_us, int64_t late_us)
{
    uint32_t i = p->recent_n++ % (sizeof(p->recent) / sizeof(p->recent[0]));
    p->recent[i].pts_us = pts_us;
    p->recent[i].late_us = late_us;
}

void vp_add_lateness(struct vp *p, int64_t pts_us, int64_t extra_us)
{
    uint32_t n = sizeof(p->recent) / sizeof(p->recent[0]), i;

    for (i = 0; i < n && i < p->recent_n; i++)
        if (p->recent[i].pts_us == pts_us)
            p->recent[i].late_us += extra_us;
}

/* How late the record for pts_us went out; false if it is not recent. */
static bool post_lateness(const struct vp *p, int64_t pts_us, int64_t *late)
{
    uint32_t n = sizeof(p->recent) / sizeof(p->recent[0]), i;
    for (i = 0; i < n && i < p->recent_n; i++)
        if (p->recent[i].pts_us == pts_us)
        {
            *late = p->recent[i].late_us;
            return true;
        }
    return false;
}

static void sync(struct vp *p, int64_t audio_us, const struct vp_status *st)
{
    int64_t err, late;

    if (!st->shown_valid || st->shown_pts_us < p->catchup_until_us ||
        st->shown_pts_us <= p->drop_pts_us)
        return;
    if (p->synced)
    {
        if (p->next_sync_us == RESYNC)
            p->next_sync_us = audio_us + VP_SYNC_EVERY_US;
        if (audio_us < p->next_sync_us)
            return;
    }
    p->next_sync_us = audio_us + VP_SYNC_EVERY_US;

    /* > 0: shown late.  Only the part the lead can fix counts: a record
       that went out late (start-up, a full BCM) is not the path's fault */
    if (!post_lateness(p, st->shown_pts_us, &late))
        return;
    err = st->shown_at_us - st->shown_pts_us - late;
    if (err > -VP_SYNC_OK_US && err < VP_SYNC_OK_US)
    {
        p->synced = true;
        return;
    }
    if (!p->synced || err >= VP_SYNC_FAR_US || err <= -VP_SYNC_FAR_US)
        p->lead_us += err;
    else
        p->lead_us += err > 0 ? VP_SYNC_STEP_US : -VP_SYNC_STEP_US;
    if (p->lead_us < VP_LEAD_MIN_US)
        p->lead_us = VP_LEAD_MIN_US;
    if (p->lead_us > VP_LEAD_MAX_US)
        p->lead_us = VP_LEAD_MAX_US;
    p->synced = true;
    p->stats.lead_changes++;
}

static void skip(struct vp *p)
{
    p->ops->skip(p->ops->ctx, p->posted);
    p->stats.skipped++;
    p->posted++;
}

bool vp_tick(struct vp *p, int64_t audio_us, const struct vp_status *st)
{
    const struct vp_ops *o = p->ops;
    uint32_t sent = 0;

    p->last_audio_us = audio_us;
    if (p->posted >= p->nsamples)
        return false;
    if (!fill(p, audio_us))
        return false;
    /* paused: nothing goes out, except after a seek (a wake, a scrub):
       the pictures up to the target, so the paused picture is the right
       one */
    if (p->paused && !p->refresh)
        return true;

    /* decode slower than real time: a picture went up far too late, so
       drop from the next sample to the next key (P frames are all
       references, so a GOP's tail is the smallest unit that can go) */
    if (st->shown_valid && !p->dropping &&
        st->shown_pts_us >= p->catchup_until_us &&
        st->shown_pts_us > p->drop_pts_us &&
        st->shown_at_us - st->shown_pts_us > VP_DROP_LATE_US)
    {
        p->dropping = true;
        p->stats.dropped_gops++;
        p->drop_pts_us = p->posted ? p->last_posted_pts_us : INT64_MIN;
    }
    else
        sync(p, audio_us, st);

    while (p->posted < p->pushed &&
           st->in_flight + sent < VP_MAX_IN_FLIGHT)
    {
        struct vp_sample s;
        if (o->sample(o->ctx, p->posted, &s) < 0)
            return false;

        /* a dropped GOP ends at the next key sample (decode order) */
        if (p->dropping)
        {
            if (!s.key)
            {
                skip(p);
                continue;
            }
            p->dropping = false;
        }
        /* above 2x only key frames, as a slideshow (player.md section 3) */
        if (p->speed > 200 && !s.key)
        {
            skip(p);
            continue;
        }
        /* after a seek: decode up to the target without showing */
        if (s.pts_us < p->catchup_until_us)
        {
            p->last_posted_pts_us = s.pts_us;
            o->post(o->ctx, p->posted++, s.pts_us, false);
            p->stats.posted++;
            sent++;
            continue;
        }
        if (p->paused)
        {
            /* the picture at the target: up, then nothing more */
            p->refresh = false;
            p->last_posted_pts_us = s.pts_us;
            o->post(o->ctx, p->posted++, s.pts_us, true);
            p->stats.posted++;
            p->stats.shown_posts++;
            break;
        }
        if (audio_us < s.pts_us - p->lead_us)
            break;
        p->refresh = false;

        /* posting a frame or more after its time (the BCM is full) */
        if (audio_us > s.pts_us - p->lead_us + p->frame_us)
            p->stats.late_posts++;

        p->last_posted_pts_us = s.pts_us;
        remember(p, s.pts_us, audio_us - (s.pts_us - p->lead_us));
        o->post(o->ctx, p->posted++, s.pts_us, true);
        p->stats.posted++;
        p->stats.shown_posts++;
        sent++;
    }
    return p->posted < p->nsamples;
}
