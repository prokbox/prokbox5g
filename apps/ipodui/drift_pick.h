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
 * iPod UI shell: the drift panel's next-cover picker (pure)
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
#ifndef _IPODUI_DRIFT_PICK_H_
#define _IPODUI_DRIFT_PICK_H_

#include <stdint.h>

/* Owner rule (2026-10-09, spec main-menu.md "Drift cover order"): the
 * drift never shows the same cover twice in a row, including when the
 * cycle wraps and when the panel set changes order under it (a rebuild of
 * panels.pak, a new panel set after a library update). With one cover
 * only, it is held, never faded into itself.
 *
 * Covers are identified by their album key, not by their index: the
 * index of a cover moves whenever the art builder rewrites panels.pak.
 *
 * drift_pick_next(): the index to show after the cover 'cur_key' that
 * was at index 'cur' (-1: none on screen; 0 keys never match), among
 * 'count' covers whose keys are keys[0..count-1]: the first index after
 * 'cur' (wrapping; after cur_key's own index when the set moved and
 * keys[cur] is another cover) whose key differs from cur_key. -1 when
 * there is none
 * (no cover, or every cover is the one on screen): hold it.
 * O(count), count <= 32; no I/O (the keys come from panels.pak's header,
 * already in RAM). Host-tested in tests/drift. */
static inline int drift_pick_next(const uint64_t *keys, int count, int cur,
                                  uint64_t cur_key)
{
    int k, i;

    if (count <= 0)
        return -1;
    if (cur < 0 || cur >= count || (cur_key && keys[cur] != cur_key))
    {
        /* the set moved: go on from where that cover is now, if it is
         * still in the set, so the rotation carries on */
        cur = -1;
        for (i = 0; cur_key && i < count; i++)
            if (keys[i] == cur_key)
                cur = i;
    }
    for (k = 1; k <= count; k++)
    {
        i = (cur + k) % count;
        if (!cur_key || keys[i] != cur_key)
            return i;
    }
    return -1;
}

#endif /* _IPODUI_DRIFT_PICK_H_ */
