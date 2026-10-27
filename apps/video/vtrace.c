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

#include <stdio.h>
#include <string.h>
#include "vtrace.h"

void vtrace_init(struct vtrace *t, struct vtrace_rec *rec, uint32_t cap)
{
    memset(t, 0, sizeof(*t));
    t->rec = rec;
    t->cap = cap;
}

void vtrace_add(struct vtrace *t, const struct vpc_frame *f)
{
    struct vtrace_rec *r;
    int64_t d = f->audio_us - f->pts_us;

    /* a count below the last one: a new session (restart) began */
    t->shown_total += f->shown >= t->last.shown && t->any
                      ? f->shown - t->last.shown : f->shown;
    t->decoded_total += f->decoded >= t->last.decoded && t->any
                        ? f->decoded - t->last.decoded : f->decoded;
    t->last = *f;
    t->any = true;
    if (t->n >= t->cap)
    {
        t->lost++;
        return;
    }
    r = &t->rec[t->n++];
    r->pts_ms = f->pts_us > 0 ? (uint32_t)(f->pts_us / 1000) : 0;
    r->drift_us = d > INT32_MAX ? INT32_MAX : d < INT32_MIN ? INT32_MIN
                                                            : (int32_t)d;
    r->work_us = f->work_us;
    r->mhz = (uint16_t)f->mhz;
    r->shown = (uint16_t)f->shown;
}

void vtrace_summary(const struct vtrace *t, bool ended, bool failed,
                    struct vtrace_sum *s)
{
    uint64_t drift_sum = 0, work_sum = 0, jit_sum = 0;
    uint32_t i;

    memset(s, 0, sizeof(*s));
    s->records = t->n;
    s->lost = t->lost;
    s->mhz_lo = UINT32_MAX;
    for (i = 0; i < t->n; i++)
    {
        const struct vtrace_rec *r = &t->rec[i];
        int32_t a = r->drift_us < 0 ? -r->drift_us : r->drift_us;

        if (i == 0)
            s->first_ms = r->pts_ms;
        s->last_ms = r->pts_ms;
        work_sum += r->work_us;
        if (r->work_us > s->work_max_us)
            s->work_max_us = r->work_us;
        if (r->mhz && r->mhz < s->mhz_lo)
            s->mhz_lo = r->mhz;
        if (r->mhz > s->mhz_hi)
            s->mhz_hi = r->mhz;
        if (r->pts_ms >= t->rec[0].pts_ms + VTRACE_SETTLE_MS)
        {
            s->settled++;
            drift_sum += (uint32_t)a;
            if (a > s->drift_max_us)
                s->drift_max_us = a;
            if (i > 0 && r->pts_ms > t->rec[i - 1].pts_ms)
            {
                int32_t j = r->drift_us - t->rec[i - 1].drift_us;
                int32_t aj = j < 0 ? -j : j;
                int32_t half = (int32_t)(r->pts_ms - t->rec[i - 1].pts_ms) * 500;

                s->intervals++;
                jit_sum += (uint32_t)aj;
                if (aj > s->jitter_max_us)
                    s->jitter_max_us = aj;
                if (j > half && j >= VTRACE_HITCH_US)
                {
                    if (!s->hitches)
                        s->hitch_ms = r->pts_ms;
                    s->hitches++;
                }
            }
        }
    }
    if (s->mhz_lo == UINT32_MAX)
        s->mhz_lo = 0;
    if (t->n)
        s->work_mean_us = (uint32_t)(work_sum / t->n);
    if (s->settled)
        s->drift_mean_us = (int32_t)(drift_sum / s->settled);
    if (s->intervals)
        s->jitter_mean_us = (int32_t)(jit_sum / s->intervals);
    if (t->any)
    {
        /* the engine's and the image's totals: the latest frame */
        if (t->last.work_max_us > s->work_max_us)
            s->work_max_us = t->last.work_max_us;
        s->shown = t->shown_total;
        s->decoded = t->decoded_total;
        s->skipped = t->last.skipped;
        s->dropped_gops = t->last.dropped_gops;
        s->late_posts = t->last.late_posts;
    }
    s->pass = ended && !failed && s->settled > 0 &&
              s->drift_max_us <= VTRACE_DRIFT_US &&
              s->skipped == 0 && s->dropped_gops == 0;
}

uint32_t vtrace_work_pct(const struct vtrace *t, unsigned pct)
{
    uint32_t i, k, best = 0, need;
    bool found = false;

    if (!t->n)
        return 0;
    need = (t->n * pct + 99) / 100;         /* records at or under */
    if (!need)
        need = 1;
    for (i = 0; i < t->n; i++)
    {
        uint32_t w = t->rec[i].work_us, le = 0;

        if (found && w >= best)
            continue;
        for (k = 0; k < t->n; k++)
            le += t->rec[k].work_us <= w;
        if (le >= need)
        {
            best = w;
            found = true;
        }
    }
    return best;
}

uint32_t vtrace_frame_us(const struct vtrace *t)
{
    const struct vtrace_rec *a, *b;
    uint16_t pics;

    if (t->n < 2)
        return 0;
    a = &t->rec[0];
    b = &t->rec[t->n - 1];
    pics = (uint16_t)(b->shown - a->shown);
    if (!pics || b->pts_ms <= a->pts_ms)
        return 0;
    return (uint32_t)((uint64_t)(b->pts_ms - a->pts_ms) * 1000 / pics);
}

bool vtrace_pace_ok(const struct vtrace_sum *s, uint32_t p95_us,
                    uint32_t frame_us, uint32_t mhz)
{
    return s->pass && frame_us &&
           (uint64_t)p95_us * 100 < (uint64_t)frame_us * VTRACE_PACE_PCT &&
           s->skipped == 0 && s->dropped_gops == 0 &&
           (!mhz || (s->mhz_lo == mhz && s->mhz_hi == mhz));
}

void vtrace_lines(const struct vtrace *t, const struct vtrace_sum *s,
                  void (*line)(void *ctx, const char *text), void *ctx)
{
    char buf[192];
    uint32_t i;

    snprintf(buf, sizeof(buf), "stage 6 %s: %lu pictures shown of %lu decoded, "
             "%lu skipped, %lu GOPs dropped, %lu late posts",
             s->pass ? "PASS" : "FAIL", (unsigned long)s->shown,
             (unsigned long)s->decoded, (unsigned long)s->skipped,
             (unsigned long)s->dropped_gops, (unsigned long)s->late_posts);
    line(ctx, buf);
    snprintf(buf, sizeof(buf), "stage 6 A/V: after %d ms, max %ld us, mean %ld us "
             "over %lu pictures (bound %d us)", VTRACE_SETTLE_MS,
             (long)s->drift_max_us, (long)s->drift_mean_us,
             (unsigned long)s->settled, VTRACE_DRIFT_US);
    line(ctx, buf);
    snprintf(buf, sizeof(buf), "stage 6 pacing: %lu intervals, |jitter| max %ld us, "
             "mean %ld us; %lu hitches (first at %lu ms; > half an interval "
             "and >= %d us late)", (unsigned long)s->intervals,
             (long)s->jitter_max_us, (long)s->jitter_mean_us,
             (unsigned long)s->hitches, (unsigned long)s->hitch_ms,
             VTRACE_HITCH_US);
    line(ctx, buf);
    snprintf(buf, sizeof(buf), "stage 6 chip: %lu us a FEED (mean), %lu us max; "
             "clock %lu-%lu MHz; %lu records, %lu not kept, %lu-%lu ms",
             (unsigned long)s->work_mean_us, (unsigned long)s->work_max_us,
             (unsigned long)s->mhz_lo, (unsigned long)s->mhz_hi,
             (unsigned long)s->records, (unsigned long)s->lost,
             (unsigned long)s->first_ms, (unsigned long)s->last_ms);
    line(ctx, buf);
    /* V6 <pts ms> <drift us> <work us> <MHz> <shown> */
    for (i = 0; i < t->n; i++)
    {
        const struct vtrace_rec *r = &t->rec[i];

        snprintf(buf, sizeof(buf), "V6 %lu %ld %lu %u %u",
                 (unsigned long)r->pts_ms, (long)r->drift_us,
                 (unsigned long)r->work_us, r->mhz, r->shown);
        line(ctx, buf);
    }
}
