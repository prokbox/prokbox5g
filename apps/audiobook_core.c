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

/* Audiobook chapter logic with no GUI, language or playback dependencies,
   so it can be built and tested on the host. */

#include <stdbool.h>
#include "metadata.h"
#include "audiobook.h"

int ab_find_current_chapter(struct audiobook_chapters *ab, unsigned long curpos)
{
    int i = 0;
    while (i < ab->chapter_count - 1 && ab_ch_start(ab, i + 1) <= curpos)
        i++;

    ab->curr_chapter_idx = i;
    return i;
}

int ab_chapter_skip_target(struct audiobook_chapters *ab, int direction,
                           unsigned long curr_pos)
{
    if (ab->chapter_count <= 0)
        return -1;

    int chapter = ab_find_current_chapter(ab, curr_pos);

    if (direction == 0)
        return chapter;             /* restart the current chapter */

    if (direction > 0)
    {
        if (chapter == ab->chapter_count - 1)
            return -1;              /* already in the last chapter */
        return chapter + 1;
    }

    /* backwards: restart the current chapter unless we are near its start */
    if (chapter > 0
        && curr_pos - ab_ch_start(ab, chapter) < AB_SKIP_THRESH)
        chapter--;

    return chapter;
}

bool ab_chapter_changed(struct mp3entry *id3)
{
    struct audiobook_chapters *ab = id3->audiobook;
    if (!ab || ab->chapter_count == 0)
        return false;

    int idx = ab->curr_chapter_idx;
    unsigned long elapsed = id3->elapsed;

    if (idx < 0 || idx >= ab->chapter_count
        || elapsed < ab_ch_start(ab, idx)
        || (idx < ab->chapter_count - 1
            && elapsed >= ab_ch_start(ab, idx + 1)))
    {
        ab_find_current_chapter(ab, elapsed);
        return true;
    }
    return false;
}
