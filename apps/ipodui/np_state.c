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
 * iPod UI shell: Now Playing SELECT-cycle state machine
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
/* Pure C on purpose: no Rockbox headers (host-tested by tests/nowplaying/). */
#include "np_state.h"

static long ms_ticks(const struct np_state *st, long ms)
{
    return ms * st->hz / 1000;
}

/* tick arithmetic that survives wrap-around */
static bool elapsed_ge(long now, long then, long ticks)
{
    return (long)((unsigned long)now - (unsigned long)then) >= ticks;
}

static bool mode_applies(const struct np_state *st, enum np_mode m)
{
    switch (m)
    {
    case NP_SCRUBBER:
        return st->scrub.length_ms > 0;
    case NP_SHUFFLE:
        return !st->is_audiobook;
    case NP_LYRICS:
        return st->has_lyrics;
    case NP_VIZ:
        return st->viz_ok && !st->is_audiobook;
    case NP_NORMAL:
    case NP_RATING:
    default:
        return true;
    }
}

static enum np_mode next_mode(const struct np_state *st, enum np_mode m)
{
    do
        m = (m == NP_VIZ) ? NP_NORMAL : (enum np_mode)(m + 1);
    while (!mode_applies(st, m));
    return m;
}

static void result_init(const struct np_state *st, struct np_result *r)
{
    r->dirty = 0;
    r->mode_changed = false;
    r->mode = st->mode;
    r->volume_clicks = 0;
    r->seek = false;
    r->seek_ms = 0;
    r->rating_commit = false;
    r->rating_value = st->rating;
    r->shuffle_commit = false;
    r->shuffle_value = st->shuffle;
}

/* Commit what the current mode has pending. 'drop_seek': the track
 * changed under the scrubber, so its position is meaningless now. */
static void commit_mode(struct np_state *st, struct np_result *r,
                        bool drop_seek)
{
    switch (st->mode)
    {
    case NP_SCRUBBER:
        if (st->seek_pending && !drop_seek)
        {
            r->seek = true;
            r->seek_ms = st->scrub_ms;
        }
        st->seek_pending = false;
        st->seek_hold = false;
        break;
    case NP_RATING:
        if (st->rating != st->rating_entry)
        {
            r->rating_commit = true;
            r->rating_value = st->rating;
            r->dirty |= NP_DIRTY_INFO;
        }
        st->rating_entry = st->rating;
        break;
    case NP_SHUFFLE:
        if (st->shuffle != st->shuffle_entry)
        {
            r->shuffle_commit = true;
            r->shuffle_value = st->shuffle;
            r->dirty |= NP_DIRTY_INFO;
        }
        st->shuffle_entry = st->shuffle;
        break;
    case NP_NORMAL:
    case NP_LYRICS:
    case NP_VIZ:
    default:
        break;
    }
}

static void enter_mode(struct np_state *st, struct np_result *r,
                       enum np_mode m, unsigned long elapsed_ms)
{
    if (m == st->mode)
        return;
    if (st->mode == NP_LYRICS || m == NP_LYRICS
        || st->mode == NP_VIZ || m == NP_VIZ)
        r->dirty |= NP_DIRTY_FULL;
    st->mode = m;
    st->touched = false;
    r->mode = m;
    r->mode_changed = true;
    r->dirty |= NP_DIRTY_ROW;
    st->volume_shown = false;
    switch (m)
    {
    case NP_SCRUBBER:
        st->scrub_ms = elapsed_ms > st->scrub.length_ms
                       ? st->scrub.length_ms : elapsed_ms;
        st->seek_pending = false;
        st->seek_hold = false;
        break;
    case NP_RATING:
        st->rating_entry = st->rating;
        break;
    case NP_SHUFFLE:
        st->shuffle_entry = st->shuffle;
        break;
    case NP_LYRICS:
        st->lyrics_top = 0;
        break;
    case NP_NORMAL:
    default:
        break;
    }
}

void np_init(struct np_state *st, long hz, enum np_shuffle shuffle)
{
    st->mode = NP_NORMAL;
    st->hz = hz > 0 ? hz : 100;
    st->last_input = 0;
    st->touched = false;
    st->is_audiobook = false;
    st->has_lyrics = false;
    st->viz_ok = false;
    st->scrub.length_ms = 0;
    st->scrub.ch.start = 0;
    st->scrub.ch.stride = sizeof(uint32_t);
    st->scrub.ch.count = 0;
    st->scrub.snap = false;
    st->volume_shown = false;
    st->volume_last = 0;
    st->scrub_ms = 0;
    st->seek_pending = false;
    st->seek_hold = false;
    st->seek_last = 0;
    st->rating = 0;
    st->rating_entry = 0;
    st->shuffle = shuffle;
    st->shuffle_entry = shuffle;
    st->lyrics_top = 0;
    st->lyrics_max = 0;
}

struct np_result np_set_track(struct np_state *st, bool is_audiobook,
                              bool has_lyrics, int lyrics_max, int rating,
                              const struct np_scrub_ctx *scrub, long tick)
{
    struct np_result r;
    enum np_mode m = st->mode;

    result_init(st, &r);
    commit_mode(st, &r, true);

    st->is_audiobook = is_audiobook;
    st->has_lyrics = has_lyrics;
    st->lyrics_max = lyrics_max > 0 ? lyrics_max : 0;
    st->lyrics_top = 0;
    st->rating = rating < 0 ? 0 : rating > NP_RATING_MAX ? NP_RATING_MAX
                                                         : rating;
    st->rating_entry = st->rating;
    st->scrub = *scrub;
    st->scrub_ms = 0;
    r.dirty |= NP_DIRTY_FULL | NP_DIRTY_ROW | NP_DIRTY_INFO;
    /* A track change always drops back to Normal (owner, 2026-10-07),
     * but for the Visualizer, a view to stay in, while music plays */
    (void)tick;
    if (m == NP_VIZ && mode_applies(st, NP_VIZ))
        r.dirty |= NP_DIRTY_FULL;
    else if (m != NP_NORMAL)
    {
        st->mode = NP_NORMAL;
        r.mode = NP_NORMAL;
        r.mode_changed = true;
    }
    st->touched = false;
    return r;
}

void np_set_viz(struct np_state *st, bool ok)
{
    st->viz_ok = ok;
}

struct np_result np_show_viz(struct np_state *st, long tick)
{
    struct np_result r;

    result_init(st, &r);
    if (st->mode != NP_VIZ && mode_applies(st, NP_VIZ))
    {
        commit_mode(st, &r, false);
        st->last_input = tick;
        enter_mode(st, &r, NP_VIZ, 0);
    }
    r.mode = st->mode;
    return r;
}

void np_set_scrub(struct np_state *st, const struct np_scrub_ctx *scrub)
{
    st->scrub = *scrub;
    if (st->scrub_ms > scrub->length_ms)
        st->scrub_ms = scrub->length_ms;
}

void np_set_lyrics(struct np_state *st, int lyrics_max)
{
    if (lyrics_max < 0)
    {
        st->has_lyrics = false;
        lyrics_max = 0;
    }
    st->lyrics_max = lyrics_max;
    if (st->lyrics_top > lyrics_max)
        st->lyrics_top = lyrics_max;
    if (st->lyrics_top < 0)
        st->lyrics_top = 0;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void on_wheel(struct np_state *st, struct np_result *r,
                     const struct np_event *ev)
{
    if (ev->clicks == 0)
        return;
    st->last_input = ev->tick;
    switch (st->mode)
    {
    case NP_NORMAL:
    case NP_VIZ:
        r->volume_clicks = ev->clicks;
        st->volume_shown = true;
        st->volume_last = ev->tick;
        r->dirty |= NP_DIRTY_ROW;
        break;
    case NP_SCRUBBER:
    {
        unsigned long base = (st->seek_pending || st->seek_hold)
                             ? st->scrub_ms : ev->elapsed_ms;
        unsigned long np = np_scrub_wheel(&st->scrub, base, ev->clicks,
                                          ev->velocity);
        st->seek_hold = false;
        if (np != base || st->seek_pending)
        {
            st->scrub_ms = np;
            st->seek_pending = true;
            st->touched = true;
            st->seek_last = ev->tick;
            r->dirty |= NP_DIRTY_ROW;
        }
        break;
    }
    case NP_RATING:
    {
        int v = clampi(st->rating + ev->clicks, 0, NP_RATING_MAX);
        if (v != st->rating)
        {
            st->rating = v;
            st->touched = true;
            r->rating_value = v;
            r->dirty |= NP_DIRTY_ROW | NP_DIRTY_INFO;
        }
        break;
    }
    case NP_SHUFFLE:
    {
        int v = clampi((int)st->shuffle + ev->clicks, NP_SHUFFLE_OFF,
                       NP_SHUFFLE_ALBUMS);
        if (v != (int)st->shuffle)
        {
            st->shuffle = (enum np_shuffle)v;
            st->touched = true;
            r->shuffle_value = st->shuffle;
            r->dirty |= NP_DIRTY_ROW;
        }
        break;
    }
    case NP_LYRICS:
    {
        int v = clampi(st->lyrics_top + ev->clicks, 0, st->lyrics_max);
        if (v != st->lyrics_top)
        {
            st->lyrics_top = v;
            r->dirty |= NP_DIRTY_FULL;
        }
        break;
    }
    }
}

static void on_tick(struct np_state *st, struct np_result *r,
                    const struct np_event *ev)
{
    long now = ev->tick;

    if (st->mode == NP_SCRUBBER)
    {
        if (st->seek_pending
            && elapsed_ge(now, st->seek_last, ms_ticks(st, NP_SEEK_SETTLE_MS)))
        {
            r->seek = true;
            r->seek_ms = st->scrub_ms;
            st->seek_pending = false;
            /* keep showing the marker until playback has caught up */
            st->seek_hold = true;
            st->seek_last = now;
        }
        else if (st->seek_hold
                 && elapsed_ge(now, st->seek_last,
                               ms_ticks(st, NP_SEEK_SETTLE_MS)))
        {
            st->seek_hold = false;
            r->dirty |= NP_DIRTY_ROW;
        }
    }
    if (st->mode != NP_NORMAL && st->mode != NP_VIZ
        && elapsed_ge(now, st->last_input, ms_ticks(st, NP_MODE_TIMEOUT_MS)))
    {
        commit_mode(st, r, false);
        enter_mode(st, r, NP_NORMAL, ev->elapsed_ms);
    }
    if ((st->mode == NP_NORMAL || st->mode == NP_VIZ) && st->volume_shown
        && elapsed_ge(now, st->volume_last, ms_ticks(st, NP_VOLUME_SHOW_MS)))
    {
        st->volume_shown = false;
        r->dirty |= NP_DIRTY_ROW;
    }
}

struct np_result np_event(struct np_state *st, const struct np_event *ev)
{
    struct np_result r;

    result_init(st, &r);
    switch (ev->type)
    {
    case NP_EV_SELECT:
        commit_mode(st, &r, false);
        st->last_input = ev->tick;
        /* a change on this page: SELECT means "done", back to Normal
         * (owner, 2026-10-09); an untouched page advances the cycle */
        enter_mode(st, &r, st->touched ? NP_NORMAL : next_mode(st, st->mode),
                   ev->elapsed_ms);
        break;
    case NP_EV_WHEEL:
        on_wheel(st, &r, ev);
        break;
    case NP_EV_TICK:
        on_tick(st, &r, ev);
        break;
    }
    r.mode = st->mode;
    return r;
}

struct np_result np_leave(struct np_state *st)
{
    struct np_result r;

    result_init(st, &r);
    commit_mode(st, &r, false);
    if (st->mode != NP_NORMAL)
    {
        st->mode = NP_NORMAL;
        r.mode = NP_NORMAL;
        r.mode_changed = true;
    }
    st->touched = false;
    st->volume_shown = false;
    return r;
}

unsigned long np_shown_pos(const struct np_state *st,
                           unsigned long elapsed_ms)
{
    if (st->mode == NP_SCRUBBER && (st->seek_pending || st->seek_hold))
        return st->scrub_ms;
    return elapsed_ms;
}
