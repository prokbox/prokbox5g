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
 * iPod UI shell: Now Playing, pure playback-facade logic (Phase 5)
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
#ifndef _IPODUI_NP_PBCORE_H_
#define _IPODUI_NP_PBCORE_H_

/* Pure C: no Rockbox headers, no statics, no allocation (host-tested by
 * tests/nowplaying/test_pbcore.c). The decisions np_playback.c (P) and
 * the screen (N) both depend on, frozen by the Phase 5 contracts
 * (docs/phase5-brief.md, "Contracts (committed)"): the rating mapping and
 * its write guard (Revision 1 item 1), the "N of M" counter source
 * (Revision 2), and the seqlock rule for the chapter table (Revision 1
 * item 4). Changes go through the orchestrator. */

#include <stdbool.h>
#include <stdint.h>

/* ---- ratings (Revision 1 item 1, frozen default 9.7) ------------------------------
 * tag_rating holds 0..10 = stars x 2. mp3entry.tagcache_idx is 1-based
 * (0 = not in the database); tagcache_update_numeric() takes the 0-based
 * master index. A database index is only meaningful for the RAM cache
 * generation it was read under (tagcache_ui_generation()). */
#define NP_STARS_MAX        5
#define NP_TAG_RATING_MAX   10

static inline int np_rating_to_tag(int stars)
{
    if (stars < 0)
        stars = 0;
    if (stars > NP_STARS_MAX)
        stars = NP_STARS_MAX;
    return stars * 2;
}

static inline int np_rating_from_tag(long v)   /* 0..10 -> 0..5, rounds down */
{
    if (v <= 0)
        return 0;
    if (v >= NP_TAG_RATING_MAX)
        return NP_STARS_MAX;
    return (int)(v / 2);
}

enum np_rate_target {
    NP_RATE_WRITE = 0,      /* write tag_rating of master index *db_idx */
    NP_RATE_NO_ENTRY,       /* tc_idx 0: not in the database (log, drop) */
    NP_RATE_STALE,          /* the database was reloaded since the
                               snapshot: the index may name another file
                               (log, drop) */
};

/* Where a rating of the track snapshotted as (tc_idx, gen_snap) goes,
 * with the database now at generation gen_now. *db_idx gets tc_idx - 1
 * on NP_RATE_WRITE, else -1. */
enum np_rate_target np_rating_target(long tc_idx, int gen_snap, int gen_now,
                                     long *db_idx);

/* The screen's override: id3->rating is not updated by a database write,
 * so the stars just set are shown from here for as long as the same
 * entry (tc_idx, generation) is current. Plain data, N keeps one. */
struct np_rating_ovr {
    long tc_idx;            /* 0: none */
    int  gen;
    int  stars;
};

void np_rating_ovr_set(struct np_rating_ovr *o, long tc_idx, int gen,
                       int stars);
/* Stars to show for the current track: the override if it is for this
 * (tc_idx, gen), else db_stars (from the snapshot). */
int  np_rating_shown(const struct np_rating_ovr *o, long tc_idx, int gen,
                     int db_stars);

/* ---- the "N of M" row (Revision 2, owner 2026-10-07) ------------------
 *  - an audiobook file with chapters: chapter N of M (N from
 *    np_chapter_step()/np_chapter_at() on the elapsed position);
 *  - everything else: the queue position. That covers a multi-file book
 *    without chapters (a book plays alone, so the queue is the book), a
 *    book started from a user playlist without chapters, music, and every
 *    podcast (podcasts keep the queue counter even with chapters).
 *  - nothing to show (no queue): NP_COUNT_NONE, the row stays blank. */
enum np_counter_src {
    NP_COUNT_NONE = 0,
    NP_COUNT_QUEUE,
    NP_COUNT_CHAPTER,
};

struct np_counter {
    enum np_counter_src src;
    int n, m;               /* "n of m", n 1-based; 0 with NP_COUNT_NONE */
};

/* queue_index: 0-based position in the play queue, queue_count: entries;
 * chapter: 0-based chapter now (< 0: unknown, taken as the first),
 * chapter_count: chapters of the current file (0: none). */
struct np_counter np_counter_pick(bool is_audiobook, bool is_podcast,
                                  int queue_index, int queue_count,
                                  int chapter, int chapter_count);

/* ---- the chapter table seqlock (Revision 1 item 4) --------------------
 * np_pb_seq() is written by the audio thread only: + 2 on a track or
 * metadata change, and + 1 before / + 1 after playback rewrites or moves
 * its chapter table (odd = being rewritten). Chapter changes within a
 * track do not bump it. A reader copies with seq read before and after;
 * the copy is good only if np_seq_read_ok(). Threads are cooperative on
 * this target: the UI thread may use the table's pointers (np_track.
 * scrub.ch) between a check that np_pb_seq() == track.seq and its next
 * yield, never across one. */
static inline bool np_seq_stable(uint32_t seq)
{
    return (seq & 1u) == 0;
}

static inline bool np_seq_read_ok(uint32_t before, uint32_t after)
{
    return np_seq_stable(before) && before == after;
}

#endif /* _IPODUI_NP_PBCORE_H_ */
