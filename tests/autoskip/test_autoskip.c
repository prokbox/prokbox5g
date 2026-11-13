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

/* Host test of #298, the skip over an unplayable track
 * (apps/autoskip_core.c): the verdict for a codec's end during an
 * automatic change, and a timeline model of the skip (playable, broken,
 * playable with Repeat All) that measures the silence it leaves and when
 * the audio thread learns of the PCM's track change.
 *
 * Red on the stock logic (the next codec starts only at the track-change
 * event): the model's skip gap is the poll phase plus the start cost. */

#include <stdio.h>
#include "autoskip_core.h"
#include "quiet_wait_core.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

#define HZ 100

/* a silent failure during a plain automatic change, next entry ready */
static struct as_in base(void)
{
    struct as_in in = {
        .skip_pending = true, .skip_auto = true, .silent = true,
        .silent_fails = 0, .fail_limit = 3, .next_ready = true,
    };
    return in;
}

static void test_verdicts(void)
{
    struct as_in in = base();

    CHECK(as_codec_end(&in) == AS_EARLY);

    /* nothing in progress: the change happens now (stock) */
    in.skip_pending = in.skip_auto = false;
    CHECK(as_codec_end(&in) == AS_NOW);
    in.silent = false;
    CHECK(as_codec_end(&in) == AS_NOW);

    /* paused: always deferred to the resume (stock) */
    in = base(); in.paused = true;
    CHECK(as_codec_end(&in) == AS_DEFER);
    in.skip_pending = in.skip_auto = false;
    CHECK(as_codec_end(&in) == AS_DEFER);

    /* a short track that played: its PCM is queued; one marker: defer */
    in = base(); in.silent = false;
    CHECK(as_codec_end(&in) == AS_DEFER);

    /* a new playlist or the end pending: not a plain change: defer */
    in = base(); in.skip_auto = false;
    CHECK(as_codec_end(&in) == AS_DEFER);

    /* Repeat One: the next entry is the same track: defer */
    in = base(); in.repeat_one = true;
    CHECK(as_codec_end(&in) == AS_DEFER);

    /* #281 kept: the failure that ends the run is left to the stock path,
     * which counts it and stops after the outgoing PCM (queue of 3) */
    in = base(); in.silent_fails = 1;
    CHECK(as_codec_end(&in) == AS_EARLY);       /* the 2nd in a row */
    in.silent_fails = 2;
    CHECK(as_codec_end(&in) == AS_DEFER);       /* the 3rd: ends it */
    in.fail_limit = 50; in.silent_fails = 48;
    CHECK(as_codec_end(&in) == AS_EARLY);
    in.silent_fails = 49;
    CHECK(as_codec_end(&in) == AS_DEFER);       /* SILENT_FAIL_MAX */

    /* the next entry not buffered, or single mode pausing at it */
    in = base(); in.next_ready = false;
    CHECK(as_codec_end(&in) == AS_DEFER);
    in = base(); in.next_pauses = true;
    CHECK(as_codec_end(&in) == AS_DEFER);
}

static void test_kick(void)
{
    unsigned int seen = 0;

    CHECK(!as_kick_due(0, &seen));
    CHECK(as_kick_due(1, &seen) && seen == 1);
    CHECK(!as_kick_due(1, &seen));              /* once per notice */
    CHECK(as_kick_due(3, &seen) && seen == 3);  /* two at once: one kick */
    CHECK(as_kick_due(0, &seen) && seen == 0);  /* the counter wraps */
}

/* ---- the skip's timeline -------------------------------------------------
 * Times in ms from track 1's codec end. Track 1's PCM plays out at
 * PCM_END; track 2's codec fails silently at FAIL; starting a codec costs
 * START (the device's constant 37.5 ms to the first PCM). The audio thread
 * learns of the PCM's change either at its next timed wake (stock: every
 * POLL from when the change began) or at the tick after the notice when
 * the tick kicks it. */
#define PCM_END 2000.0
#define FAIL      80.0
#define START     37.5
#define POLL     100.0
#define TICK      10.0

struct skip_out {
    double codec3_start;    /* track 3's codec starts */
    double gap;             /* silence at the skip */
    double np_lag;          /* the PCM's change -> Now Playing switch */
};

/* the first wake at or after t: the poll (phase from the wait's start)
 * or, with the kick, the tick after the notice */
static double wake_after(double t, double phase, bool kick)
{
    double p = kick ? TICK : POLL;
    double base = phase - (long)(phase / p) * p;  /* the tick's or poll's */
    double n = (t - base) / p;
    long k = (long)n;
    if (k < n)
        k++;
    return base + k * p;
}

static struct skip_out model_skip(bool kick, double phase)
{
    struct skip_out o;
    struct as_in in = base();
    struct qw_audio_in qa = { .skip_pending = true, .kicked = kick };
    double change_seen;

    /* the backstop poll, when kicked, is not what wakes it */
    CHECK(qw_audio_wait(&qa, HZ) == (kick ? HZ : HZ / 10));

    change_seen = wake_after(PCM_END, phase, kick);
    o.np_lag = change_seen - PCM_END;

    if (as_codec_end(&in) == AS_EARLY)
        o.codec3_start = FAIL;              /* at the failure */
    else
        o.codec3_start = change_seen;       /* stock: at the change */

    o.gap = o.codec3_start + START - PCM_END;
    if (o.gap < 0)
        o.gap = 0;
    return o;
}

static void test_timeline(void)
{
    double worst_gap = 0, worst_lag = 0;
    double phase;

    for (phase = 0; phase < POLL; phase += 3.7)
    {
        struct skip_out o = model_skip(true, phase);

        /* the next codec starts at the failure, not at the change */
        CHECK(o.codec3_start == FAIL);
        if (o.gap > worst_gap)
            worst_gap = o.gap;
        if (o.np_lag > worst_lag)
            worst_lag = o.np_lag;
    }
    /* gapless over the skipped track, the switch within a tick */
    CHECK(worst_gap == 0);
    CHECK(worst_lag <= TICK);
    printf("     model: skip gap max %.1f ms, Now Playing lag max %.1f ms "
           "(stock: up to %.1f / %.1f)\n", worst_gap, worst_lag,
           POLL + START, POLL);
}

int main(void)
{
    test_verdicts();
    test_kick();
    test_timeline();
    if (fails)
        fprintf(stderr, "%d check(s) failed\n", fails);
    return fails != 0;
}
