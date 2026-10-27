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

#include "np_pbcore.h"

enum np_rate_target np_rating_target(long tc_idx, int gen_snap, int gen_now,
                                     long *db_idx)
{
    *db_idx = -1;
    if (tc_idx <= 0)
        return NP_RATE_NO_ENTRY;
    if (gen_snap != gen_now)
        return NP_RATE_STALE;
    *db_idx = tc_idx - 1;
    return NP_RATE_WRITE;
}

void np_rating_ovr_set(struct np_rating_ovr *o, long tc_idx, int gen,
                       int stars)
{
    o->tc_idx = tc_idx > 0 ? tc_idx : 0;
    o->gen = gen;
    o->stars = np_rating_from_tag(np_rating_to_tag(stars));
}

int np_rating_shown(const struct np_rating_ovr *o, long tc_idx, int gen,
                    int db_stars)
{
    if (o->tc_idx > 0 && o->tc_idx == tc_idx && o->gen == gen)
        return o->stars;
    return db_stars;
}

struct np_counter np_counter_pick(bool is_audiobook, bool is_podcast,
                                  int queue_index, int queue_count,
                                  int chapter, int chapter_count)
{
    struct np_counter c = { NP_COUNT_NONE, 0, 0 };

    if (is_audiobook && !is_podcast && chapter_count > 0)
    {
        if (chapter < 0)
            chapter = 0;
        if (chapter >= chapter_count)
            chapter = chapter_count - 1;
        c.src = NP_COUNT_CHAPTER;
        c.n = chapter + 1;
        c.m = chapter_count;
    }
    else if (queue_count > 0 && queue_index >= 0)
    {
        if (queue_index >= queue_count)
            queue_index = queue_count - 1;
        c.src = NP_COUNT_QUEUE;
        c.n = queue_index + 1;
        c.m = queue_count;
    }
    return c;
}
