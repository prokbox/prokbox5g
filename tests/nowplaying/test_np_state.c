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

/* Host test: apps/ipodui/np_state.c (Now Playing SELECT cycle). */
#include <string.h>
#include "np_state.h"
#include "check.h"

#define HZ 100
#define TO (NP_MODE_TIMEOUT_MS * HZ / 1000)       /* 400 ticks */
#define SETTLE (NP_SEEK_SETTLE_MS * HZ / 1000)    /* 25 ticks */

static struct np_state st;
static long now;
static unsigned long elapsed = 30000;

static struct np_result ev(enum np_event_type t, int clicks, unsigned v)
{
    struct np_event e;
    e.type = t;
    e.clicks = clicks;
    e.velocity = v;
    e.tick = now;
    e.elapsed_ms = elapsed;
    return np_event(&st, &e);
}
#define SELECT()      ev(NP_EV_SELECT, 0, 0)
#define WHEEL(c)      ev(NP_EV_WHEEL, (c), 0)
#define WHEELV(c, v)  ev(NP_EV_WHEEL, (c), (v))
#define TICK()        ev(NP_EV_TICK, 0, 0)

static const struct np_scrub_ctx music = {
    .length_ms = 240000, .ch = { NULL, sizeof(uint32_t), 0 }, .snap = false };
static const uint32_t book_st[] = { 0, 600000, 1200000 };
static const struct np_scrub_ctx book = {
    .length_ms = 1800000, .ch = { book_st, sizeof(uint32_t), 3 }, .snap = true };

static void track(bool ab, bool lyr, int rating, const struct np_scrub_ctx *c)
{
    np_set_track(&st, ab, lyr, 10, rating, c, now);
}

static void test_cycles(void)
{
    struct np_result r = { 0 };

    /* music, no lyrics: N -> S -> R -> Sh -> N */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    r = SELECT();
    CHECK_EQ(r.mode, NP_SCRUBBER);
    CHECK(r.mode_changed);
    CHECK(r.dirty & NP_DIRTY_ROW);
    CHECK_EQ(SELECT().mode, NP_RATING);
    CHECK_EQ(SELECT().mode, NP_SHUFFLE);
    r = SELECT();
    CHECK_EQ(r.mode, NP_NORMAL);
    CHECK(!(r.dirty & NP_DIRTY_FULL));

    /* music with lyrics: ... -> Sh -> L -> N; lyrics redraw everything */
    track(false, true, 0, &music);
    SELECT(); SELECT(); SELECT();
    r = SELECT();
    CHECK_EQ(r.mode, NP_LYRICS);
    CHECK(r.dirty & NP_DIRTY_FULL);
    r = SELECT();
    CHECK_EQ(r.mode, NP_NORMAL);
    CHECK(r.dirty & NP_DIRTY_FULL);

    /* audiobook: N -> S -> R -> N (no Shuffle) */
    track(true, false, 0, &book);
    CHECK_EQ(SELECT().mode, NP_SCRUBBER);
    CHECK_EQ(SELECT().mode, NP_RATING);
    CHECK_EQ(SELECT().mode, NP_NORMAL);

    /* audiobook with lyrics: N -> S -> R -> L -> N */
    track(true, true, 0, &book);
    SELECT(); SELECT();
    CHECK_EQ(SELECT().mode, NP_LYRICS);
    CHECK_EQ(SELECT().mode, NP_NORMAL);

    /* unknown length (stream): Scrubber skipped */
    {
        struct np_scrub_ctx z = music;
        z.length_ms = 0;
        track(false, false, 0, &z);
        CHECK_EQ(SELECT().mode, NP_RATING);
        CHECK_EQ(SELECT().mode, NP_SHUFFLE);
        CHECK_EQ(SELECT().mode, NP_NORMAL);
    }
}

static void test_timeout(void)
{
    struct np_result r = { 0 };
    int m;

    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, true, 0, &music);
    for (m = NP_SCRUBBER; m <= NP_LYRICS; m++)
    {
        int k;
        now = 1000 * m;
        for (k = NP_SCRUBBER; k <= m; k++)
            r = SELECT();
        CHECK_EQ(r.mode, m);
        now += TO - 1;
        CHECK_EQ(TICK().mode, m);
        /* a 0-click wheel event is not input; a click restarts the timer */
        WHEEL(0);
        now += 1;
        CHECK_EQ(TICK().mode, NP_NORMAL);
        for (k = NP_SCRUBBER; k <= m; k++)
            SELECT();
        now += TO - 1;
        WHEEL(1);
        now += TO - 1;
        CHECK_EQ(TICK().mode, m);
        now += 1;
        r = TICK();
        CHECK_EQ(r.mode, NP_NORMAL);
        CHECK(r.mode_changed);
    }
    /* Normal never times out (nothing to drop to) */
    now += 10 * TO;
    r = TICK();
    CHECK_EQ(r.mode, NP_NORMAL);
    CHECK(!r.mode_changed);

    /* tick wrap-around */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    now = 0x7fffffffL - 10;
    SELECT();
    now = (long)((unsigned long)now + TO);
    CHECK_EQ(TICK().mode, NP_NORMAL);
}

static void test_volume(void)
{
    struct np_result r = { 0 };

    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    now = 5000;
    r = WHEEL(-3);
    CHECK_EQ(r.volume_clicks, -3);
    CHECK(st.volume_shown);
    CHECK(r.dirty & NP_DIRTY_ROW);
    CHECK_EQ(r.mode, NP_NORMAL);
    now += NP_VOLUME_SHOW_MS * HZ / 1000 - 1;
    r = TICK();
    CHECK(st.volume_shown);
    now += 1;
    r = TICK();
    CHECK(!st.volume_shown);
    CHECK(r.dirty & NP_DIRTY_ROW);
    /* SELECT while the volume bar shows: Scrubber, bar gone */
    WHEEL(1);
    r = SELECT();
    CHECK_EQ(r.mode, NP_SCRUBBER);
    CHECK(!st.volume_shown);
    CHECK_EQ(r.volume_clicks, 0);
    /* the wheel is never volume outside Normal */
    CHECK_EQ(WHEEL(2).volume_clicks, 0);
}

static void test_rating(void)
{
    struct np_result r = { 0 };

    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 2, &music);
    now = 100;
    SELECT();
    r = SELECT();
    CHECK_EQ(r.mode, NP_RATING);
    CHECK_EQ(st.rating, 2);
    r = WHEEL(1);                   /* right raises */
    CHECK_EQ(r.rating_value, 3);
    CHECK(!r.rating_commit);        /* written when the mode is left */
    CHECK(r.dirty & NP_DIRTY_ROW);
    r = WHEEL(10);
    CHECK_EQ(st.rating, 5);         /* clamp */
    r = WHEEL(1);
    CHECK_EQ(r.dirty, 0);           /* no change, no redraw */
    WHEEL(-1);                      /* left lowers */
    CHECK_EQ(st.rating, 4);
    r = WHEEL(-9);
    CHECK_EQ(st.rating, 0);
    WHEEL(4);
    r = SELECT();                   /* leave: one commit */
    CHECK_EQ(r.mode, NP_NORMAL);    /* rated: SELECT returns to Normal */
    CHECK(r.rating_commit);
    CHECK_EQ(r.rating_value, 4);
    CHECK(r.dirty & NP_DIRTY_INFO);

    /* unchanged visit: no write */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 3, &music);
    SELECT(); SELECT();
    WHEEL(1); WHEEL(-1);
    r = SELECT();
    CHECK(!r.rating_commit);

    /* timeout commits */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    now = 100;
    SELECT(); SELECT();
    WHEEL(2);
    now += TO;
    r = TICK();
    CHECK_EQ(r.mode, NP_NORMAL);
    CHECK(r.rating_commit);
    CHECK_EQ(r.rating_value, 2);

    /* track change in Rating: old track's rating is committed, the new
     * track's rating is loaded, and the mode drops to Normal (owner) */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 1, &music);
    SELECT(); SELECT();
    WHEEL(2);
    r = np_set_track(&st, false, false, 0, 5, &music, now);
    CHECK(r.rating_commit);
    CHECK_EQ(r.rating_value, 3);
    CHECK_EQ(st.mode, NP_NORMAL);
    CHECK(r.mode_changed);
    CHECK_EQ(st.rating, 5);
    r = SELECT();
    CHECK(!r.rating_commit);

    /* np_leave() commits */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    SELECT(); SELECT();
    WHEEL(1);
    r = np_leave(&st);
    CHECK(r.rating_commit);
    CHECK_EQ(r.rating_value, 1);
    CHECK_EQ(r.mode, NP_NORMAL);

    /* out-of-range input rating is clamped */
    track(false, false, 9, &music);
    CHECK_EQ(st.rating, 5);
    track(false, false, -2, &music);
    CHECK_EQ(st.rating, 0);
}

static void test_shuffle(void)
{
    struct np_result r = { 0 };

    np_init(&st, HZ, NP_SHUFFLE_SONGS);
    track(false, false, 0, &music);
    SELECT(); SELECT();
    r = SELECT();
    CHECK_EQ(r.mode, NP_SHUFFLE);
    CHECK_EQ(st.shuffle, NP_SHUFFLE_SONGS);
    WHEEL(1);
    CHECK_EQ(st.shuffle, NP_SHUFFLE_ALBUMS);
    r = WHEEL(1);
    CHECK_EQ(st.shuffle, NP_SHUFFLE_ALBUMS);     /* clamp, no wrap */
    CHECK_EQ(r.dirty, 0);
    WHEEL(-5);
    CHECK_EQ(st.shuffle, NP_SHUFFLE_OFF);
    r = SELECT();
    CHECK(r.shuffle_commit);
    CHECK_EQ(r.shuffle_value, NP_SHUFFLE_OFF);
    CHECK_EQ(r.mode, NP_NORMAL);

    /* switching to an audiobook while on the shelf: commit, back to
     * Normal (books have no Shuffle step) */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    SELECT(); SELECT(); SELECT();
    WHEEL(1);
    r = np_set_track(&st, true, false, 0, 0, &book, now);
    CHECK(r.shuffle_commit);
    CHECK_EQ(r.shuffle_value, NP_SHUFFLE_SONGS);
    CHECK_EQ(r.mode, NP_NORMAL);
    CHECK(r.mode_changed);

    /* a track without lyrics while in Lyrics: Normal */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, true, 0, &music);
    SELECT(); SELECT(); SELECT(); SELECT();
    CHECK_EQ(st.mode, NP_LYRICS);
    r = np_set_track(&st, false, false, 0, 0, &music, now);
    CHECK_EQ(r.mode, NP_NORMAL);
    CHECK(r.dirty & NP_DIRTY_FULL);
}

static void test_lyrics(void)
{
    struct np_result r = { 0 };

    np_init(&st, HZ, NP_SHUFFLE_OFF);
    np_set_track(&st, false, true, 3, 0, &music, now);
    SELECT(); SELECT(); SELECT(); SELECT();
    CHECK_EQ(st.mode, NP_LYRICS);
    r = WHEEL(2);
    CHECK_EQ(st.lyrics_top, 2);
    CHECK(r.dirty & NP_DIRTY_FULL);
    WHEEL(5);
    CHECK_EQ(st.lyrics_top, 3);
    r = WHEEL(1);
    CHECK_EQ(r.dirty, 0);
    WHEEL(-9);
    CHECK_EQ(st.lyrics_top, 0);
    /* re-entering starts at the top */
    WHEEL(2);
    SELECT();
    SELECT(); SELECT(); SELECT(); SELECT();
    CHECK_EQ(st.mode, NP_LYRICS);
    CHECK_EQ(st.lyrics_top, 0);
}

static void test_scrubber(void)
{
    struct np_result r = { 0 };

    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    now = 100;
    elapsed = 30000;
    SELECT();
    CHECK_EQ(np_shown_pos(&st, elapsed), 30000);
    r = WHEEL(2);
    CHECK(!r.seek);                 /* not per click */
    CHECK_EQ(np_shown_pos(&st, elapsed), 32000);
    now += 5;
    elapsed = 30050;                /* playback moves on underneath */
    WHEEL(1);
    CHECK_EQ(np_shown_pos(&st, elapsed), 33000);
    now += SETTLE - 1;
    CHECK(!TICK().seek);
    now += 1;
    r = TICK();
    CHECK(r.seek);
    CHECK_EQ(r.seek_ms, 33000);
    /* marker held until playback has caught up, then follows play */
    CHECK_EQ(np_shown_pos(&st, 30100), 33000);
    now += SETTLE;
    r = TICK();
    CHECK(!r.seek);
    CHECK(r.dirty & NP_DIRTY_ROW);
    elapsed = 33500;
    CHECK_EQ(np_shown_pos(&st, elapsed), 33500);
    /* next wheel starts from the playback position */
    WHEEL(-1);
    CHECK_EQ(np_shown_pos(&st, elapsed), 32500);
    /* leaving with a seek pending emits it once */
    r = SELECT();
    CHECK(r.seek);
    CHECK_EQ(r.seek_ms, 32500);
    CHECK_EQ(r.mode, NP_NORMAL);    /* scrubbed: back to Normal */
    CHECK_EQ(np_shown_pos(&st, 1), 1);

    /* timeout with a seek pending: emitted with the drop to Normal */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    now = 100;
    SELECT();
    WHEEL(1);
    now += TO;
    r = TICK();
    CHECK(r.seek);
    CHECK_EQ(r.mode, NP_NORMAL);

    /* track change with a seek pending: dropped (old track's position) */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    SELECT();
    WHEEL(3);
    r = np_set_track(&st, false, false, 0, 0, &music, now);
    CHECK(!r.seek);
    CHECK_EQ(st.mode, NP_NORMAL);
    CHECK_EQ(np_shown_pos(&st, 7), 7);

    /* audiobook: fast wheel snaps to chapter starts */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(true, false, 0, &book);
    elapsed = 100000;
    SELECT();
    WHEELV(1, 60);
    CHECK_EQ(np_shown_pos(&st, elapsed), 600000);
    WHEELV(1, 60);
    CHECK_EQ(np_shown_pos(&st, elapsed), 1200000);
    WHEELV(-1, 60);
    CHECK_EQ(np_shown_pos(&st, elapsed), 600000);
    WHEELV(1, 0);
    CHECK_EQ(np_shown_pos(&st, elapsed), 601000);

    /* entering past the end clamps the marker */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    elapsed = 999999;
    SELECT();
    CHECK_EQ(st.scrub_ms, 240000);
    {
        struct np_scrub_ctx shorter = music;
        shorter.length_ms = 1000;
        WHEEL(-1);
        np_set_scrub(&st, &shorter);
        CHECK_EQ(st.scrub_ms, 1000);
    }
    elapsed = 30000;
}

/* SELECT after a change on a mode page returns to Normal; an untouched
 * page advances (owner, 2026-10-09, #58) */
static void test_select_after_change(void)
{
    struct np_result r = { 0 };

    /* Scrubber: scrubbed -> Normal, with the seek emitted */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    now = 100;
    elapsed = 30000;
    SELECT();
    WHEEL(3);
    r = SELECT();
    CHECK_EQ(r.mode, NP_NORMAL);
    CHECK(r.mode_changed);
    CHECK(r.seek);
    /* ...also after the seek already settled within the visit */
    SELECT();
    WHEEL(2);
    now += SETTLE;
    r = TICK();
    CHECK(r.seek);
    now += 2 * SETTLE;
    TICK();
    r = SELECT();
    CHECK_EQ(r.mode, NP_NORMAL);
    CHECK(!r.seek);
    /* untouched Scrubber (a wheel at the start that cannot move) */
    elapsed = 0;
    SELECT();
    WHEEL(-1);
    CHECK_EQ(SELECT().mode, NP_RATING);
    elapsed = 30000;

    /* Rating: rated -> Normal (one commit); untouched -> Shuffle */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 2, &music);
    SELECT(); SELECT();
    r = SELECT();
    CHECK_EQ(r.mode, NP_SHUFFLE);   /* Rating untouched: advance */
    np_leave(&st);
    SELECT(); SELECT();
    WHEEL(1);
    r = SELECT();
    CHECK_EQ(r.mode, NP_NORMAL);
    CHECK(r.rating_commit);
    CHECK_EQ(r.rating_value, 3);
    /* moved and moved back still counts as a change (the user touched
     * the page), but writes nothing */
    SELECT(); SELECT();
    WHEEL(1); WHEEL(-1);
    r = SELECT();
    CHECK_EQ(r.mode, NP_NORMAL);
    CHECK(!r.rating_commit);
    /* a wheel against the clamp is no change */
    track(false, false, 5, &music);
    SELECT(); SELECT();
    WHEEL(2);
    CHECK_EQ(SELECT().mode, NP_SHUFFLE);

    /* Shuffle: toggled -> Normal; untouched -> Lyrics (when present) */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, true, 0, &music);
    SELECT(); SELECT(); SELECT();
    CHECK_EQ(st.mode, NP_SHUFFLE);
    CHECK_EQ(SELECT().mode, NP_LYRICS);
    SELECT();
    SELECT(); SELECT(); SELECT();
    WHEEL(1);
    r = SELECT();
    CHECK_EQ(r.mode, NP_NORMAL);
    CHECK(r.shuffle_commit);

    /* the flag is per visit: the next page starts untouched */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    SELECT();
    CHECK_EQ(SELECT().mode, NP_RATING);
    CHECK_EQ(SELECT().mode, NP_SHUFFLE);
    /* a timeout after a change clears it too */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    now = 100;
    SELECT(); SELECT();
    WHEEL(1);
    now += TO;
    CHECK_EQ(TICK().mode, NP_NORMAL);
    CHECK_EQ(SELECT().mode, NP_SCRUBBER);
    CHECK_EQ(SELECT().mode, NP_RATING);
    /* Normal's volume wheel never counts */
    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    WHEEL(2);
    CHECK_EQ(SELECT().mode, NP_SCRUBBER);
}

/* #126: the Visualizer view: after Lyrics, music only, no timeout, the
 * wheel is the volume, kept across a music track change, never a book */
static void test_viz(void)
{
    struct np_result r;
    int i;

    np_init(&st, HZ, NP_SHUFFLE_OFF);
    track(false, false, 0, &music);
    np_set_viz(&st, true);
    SELECT(); SELECT(); SELECT();               /* S, R, Sh */
    r = SELECT();
    CHECK(r.mode == NP_VIZ && r.mode_changed && (r.dirty & NP_DIRTY_FULL));
    for (i = 0; i < 100; i++)                   /* 10 s, no input */
    {
        now += HZ / 10;
        r = TICK();
        CHECK(st.mode == NP_VIZ);
    }
    r = WHEEL(2);
    CHECK(r.volume_clicks == 2 && st.volume_shown && st.mode == NP_VIZ);
    now += 3 * HZ;
    r = TICK();
    CHECK(!st.volume_shown && st.mode == NP_VIZ);
    r = np_set_track(&st, false, false, 0, 0, &music, now);
    CHECK(st.mode == NP_VIZ && !r.mode_changed && (r.dirty & NP_DIRTY_FULL));
    r = np_set_track(&st, true, false, 0, 0, &book, now);   /* a book */
    CHECK(st.mode == NP_NORMAL && r.mode_changed);
    /* a book never offers it */
    SELECT(); SELECT();
    r = SELECT();
    CHECK(st.mode == NP_NORMAL);
    /* music again: VIZ -> SELECT -> Normal */
    track(false, true, 0, &music);
    SELECT(); SELECT(); SELECT(); SELECT();     /* S R Sh Lyrics */
    CHECK(st.mode == NP_LYRICS);
    r = SELECT();
    CHECK(st.mode == NP_VIZ && (r.dirty & NP_DIRTY_FULL));
    r = SELECT();
    CHECK(st.mode == NP_NORMAL && (r.dirty & NP_DIRTY_FULL));
    /* not offered: skipped */
    np_set_viz(&st, false);
    SELECT(); SELECT(); SELECT(); SELECT();
    r = SELECT();
    CHECK(st.mode == NP_NORMAL);
}

int main(void)
{
    test_cycles();
    test_timeout();
    test_volume();
    test_rating();
    test_shuffle();
    test_lyrics();
    test_scrubber();
    test_select_after_change();
    test_viz();
    CHECK_DONE("test_np_state");
}
