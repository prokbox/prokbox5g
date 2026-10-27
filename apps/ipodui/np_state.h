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
#ifndef _IPODUI_NP_STATE_H_
#define _IPODUI_NP_STATE_H_

/* Pure C: no kernel calls, no statics, no Rockbox headers (host-tested by
 * tests/nowplaying/). The screen feeds events with the current tick and
 * applies the result: it owns drawing, volume, seeking, the rating write
 * and the shuffle change. One struct np_state per Now Playing screen,
 * used from the UI thread only.
 *
 * Cycle (docs/design/7g-spec.md, Now Playing, owner 2026-10-06):
 *   Normal -> Scrubber -> Rating -> Shuffle (music only)
 *          -> Lyrics (only if the track has lyrics)
 *          -> Visualizer (music only, when the screen offers it; #126)
 *          -> Normal
 * Scrubber is skipped when the length is unknown (0). SELECT after a
 * change on a mode page (scrubbed, rated, shuffle moved: the shown value
 * moved at least once during the visit) returns to Normal instead of
 * advancing (owner, 2026-10-09, #58). Every non-Normal
 * mode but the Visualizer drops back to Normal after NP_MODE_TIMEOUT_MS
 * without input; wheel input (and SELECT) restarts the timer. The
 * Visualizer is a view to stay in (owner, 2026-10-24): no timeout, the
 * wheel is the volume (as in Normal), and a track change keeps it unless
 * the new track is an audiobook (never a visualizer for books).
 *
 * Writes are deferred to the moment a mode is left (SELECT, timeout,
 * track change, np_leave()): one seek per scrub gesture (after
 * NP_SEEK_SETTLE_MS of wheel rest, or on leaving), one rating write and
 * one shuffle change per visit, so a wheel spin never queues a database
 * write or a playlist rebuild per click. */

#include <stdbool.h>
#include "np_scrub.h"

#define NP_MODE_TIMEOUT_MS   4000   /* theme constant np_mode_timeout */
#define NP_VOLUME_SHOW_MS    2000   /* volume bar after the last wheel click
                                     * in Normal (not yet measured: owner) */
#define NP_SEEK_SETTLE_MS    250    /* scrubber: seek once the wheel rests */
#define NP_RATING_MAX        5

enum np_mode {
    NP_NORMAL = 0,
    NP_SCRUBBER,
    NP_RATING,
    NP_SHUFFLE,
    NP_LYRICS,
    NP_VIZ,                 /* #126: the visualizer view */
};

enum np_shuffle {           /* the shelf, left to right */
    NP_SHUFFLE_OFF = 0,
    NP_SHUFFLE_SONGS,
    NP_SHUFFLE_ALBUMS,
};

enum np_event_type {
    NP_EV_SELECT,           /* SELECT released */
    NP_EV_WHEEL,            /* clicks (+ = clockwise), velocity */
    NP_EV_TICK,             /* periodic: timeouts and the settled seek */
};

struct np_event {
    enum np_event_type type;
    int clicks;             /* NP_EV_WHEEL */
    unsigned velocity;      /* NP_EV_WHEEL: clicks/s (input.c) */
    long tick;              /* current_tick */
    unsigned long elapsed_ms; /* playback position now: the Scrubber's
                               * start point, and its base once a seek
                               * has settled (the marker follows play) */
};

/* What the screen has to redraw (bitmask) */
#define NP_DIRTY_ROW    0x01    /* the bottom row: bar / scrubber / rating
                                 * dots / shuffle shelf / volume bar */
#define NP_DIRTY_INFO   0x02    /* the info block (rating stars, shuffle
                                 * glyph under the header) */
#define NP_DIRTY_FULL   0x04    /* everything (Lyrics in or out) */

struct np_result {
    unsigned dirty;             /* NP_DIRTY_* */
    bool mode_changed;          /* animate: old row slides off left */
    enum np_mode mode;          /* mode after the event */
    int volume_clicks;          /* Normal: apply this many volume steps */
    bool seek;                  /* seek to seek_ms now */
    unsigned long seek_ms;
    bool rating_commit;         /* write rating_value to the track */
    int rating_value;
    bool shuffle_commit;        /* apply shuffle_value */
    enum np_shuffle shuffle_value;
};

struct np_state {
    enum np_mode mode;
    long hz;
    long last_input;            /* tick of the last SELECT/wheel */
    bool touched;               /* this mode page's value moved since it
                                 * was entered: SELECT goes to Normal */
    /* track */
    bool is_audiobook;
    bool has_lyrics;
    bool viz_ok;                /* the screen offers the Visualizer */
    struct np_scrub_ctx scrub;  /* length + chapters (caller owns them) */
    /* Normal: volume bar (the screen reads volume_shown) */
    bool volume_shown;
    long volume_last;
    /* Scrubber */
    unsigned long scrub_ms;     /* marker position while in Scrubber */
    bool seek_pending;          /* wheel moved, seek not yet emitted */
    bool seek_hold;             /* seek emitted, marker held ~settle */
    long seek_last;
    /* Rating */
    int rating;                 /* 0..NP_RATING_MAX, shown value */
    int rating_entry;           /* value when Rating was entered */
    /* Shuffle */
    enum np_shuffle shuffle;
    enum np_shuffle shuffle_entry;
    /* Lyrics */
    int lyrics_top;             /* first line shown */
    int lyrics_max;             /* max(0, lines - visible lines) */
};

/* hz: ticks per second (HZ). shuffle: the current setting. */
void np_init(struct np_state *st, long hz, enum np_shuffle shuffle);

/* A (new) track is current. Commits what the old track still had pending
 * (call it before switching the screen's own track data), then always
 * drops to Normal, as the 7G does (owner, 2026-10-07).
 * 'scrub' is copied (the chapter column it points to must stay valid
 * while it is current). lyrics_max = max(0, lyric lines - lines shown). */
struct np_result np_set_track(struct np_state *st, bool is_audiobook,
                              bool has_lyrics, int lyrics_max, int rating,
                              const struct np_scrub_ctx *scrub, long tick);

/* The screen offers the Visualizer view (or no longer does: the next
 * SELECT skips it; a shown one is left by the screen with np_leave()) */
void np_set_viz(struct np_state *st, bool ok);

/* Straight to the Visualizer (the charger's Visualizer face, #126):
 * commits what the current mode has pending; nothing if it isn't
 * offered (a book, or the screen says no) or already shown */
struct np_result np_show_viz(struct np_state *st, long tick);

/* Book length or chapters changed for the same track (e.g. the duration
 * firmed up after a VBR scan). Clamps the marker. */
void np_set_scrub(struct np_state *st, const struct np_scrub_ctx *scrub);

/* Lyrics were read (on entering Lyrics, after the body push; Phase 5
 * contract): lyrics_max = max(0, wrapped lines - lines shown); clamps the
 * scroll. lyrics_max < 0: the read failed (no lyrics after all): the
 * Lyrics step leaves the cycle for this track (the screen shows the
 * empty page until the next SELECT or the timeout). */
void np_set_lyrics(struct np_state *st, int lyrics_max);

struct np_result np_event(struct np_state *st, const struct np_event *ev);

/* Leaving Now Playing (MENU, another screen): commit pending writes and
 * return to Normal. */
struct np_result np_leave(struct np_state *st);

/* Display helpers */
/* Position the bar/labels show: the marker in Scrubber, else elapsed. */
unsigned long np_shown_pos(const struct np_state *st,
                           unsigned long elapsed_ms);

#endif /* _IPODUI_NP_STATE_H_ */
