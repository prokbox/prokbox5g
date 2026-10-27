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

#ifndef _AUDIOBOOK_H_
#define _AUDIOBOOK_H_

#include <stdbool.h>
#include "chapters.h"   /* struct audiobook_chapters, metadata_read_chapters */

struct mp3entry;
struct screen;

/* --- audiobook_core.c: pure logic, no GUI/lang dependencies --- */

/* Restart threshold: skipping back within this many ms of a chapter start
   goes to the previous chapter instead of restarting the current one */
#define AB_SKIP_THRESH 3000ul

/* Index of the chapter containing curpos; also stored in curr_chapter_idx */
int ab_find_current_chapter(struct audiobook_chapters *ab, unsigned long curpos);

/* Chapter to jump to for a skip in 'direction' (+1/-1) from curr_pos, or -1
   if there is nowhere to go (skip forward from the last chapter) */
int ab_chapter_skip_target(struct audiobook_chapters *ab, int direction,
                           unsigned long curr_pos);

/* true (and curr_chapter_idx updated) if playback crossed a chapter boundary */
bool ab_chapter_changed(struct mp3entry *id3);

/* --- audiobook.c: UI / playback control --- */

/* Seek playback per ab_chapter_skip_target(); false if nothing was done */
bool ab_chapter_skip(struct audiobook_chapters *ab, int direction,
                     unsigned long curr_pos);

void ab_browse_chapters(struct audiobook_chapters *ab);

void ab_draw_chapter_markers(struct screen *screen,
                             struct audiobook_chapters *ab,
                             unsigned long tracklen,
                             int x, int y, int w, int h);

#endif /* _AUDIOBOOK_H_ */
