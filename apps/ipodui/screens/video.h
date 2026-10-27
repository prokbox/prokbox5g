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
 * iPod UI shell: Videos (Phase 14a)
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
#ifndef IPODUI_SCREENS_VIDEO_H
#define IPODUI_SCREENS_VIDEO_H

/* Contract (docs/phase14a-brief.md 3.6-3.8, 4.5), video builds only
 * (HAVE_BCMFW_VIDEO). Everything here runs on the UI thread. The player
 * calls (apps/video/vplayer.h) and their power rules are
 * docs/research/bcm2722/shell-contract.md 1-7; only video.c calls them. */

#ifdef HAVE_BCMFW_VIDEO

#include "ipodui/screen_stack.h"

/* V6, screens/video.c (3.7): plays one video file: vplayer_start(path)
 * FIRST (its core_alloc sizes the audio buffer after it), then the audio
 * as a one-item playlist at the resume position per Settings > Start
 * Playing, then pushes the video screen. 'tc_idx' is the file's tagcache
 * master index (resume position, played mark; -1 if unknown: then it
 * starts from the beginning). A video that can't play fails entirely
 * (owner, 2026-10-13): nothing plays and the video screen shows the
 * light failure prompt, OK back to the list (logged with
 * ui_log("video", "failed why=...")). Called by the Movies / TV episode
 * lists, a Podcasts video episode row (browse.c) and lib_play() for a
 * video row. */
void ui_video_play(const char *path, int tc_idx);

/* V6, screens/setvideos.c (3.8): pushes Videos > Settings (Start
 * Playing, Fit to Screen). Called by the Videos menu (V5). */
void ui_setvideos_push(void);

/* V5, screens/videos.c (3.6): the Videos split screen (Movies, TV Shows,
 * Settings), pushed by the main menu's Videos row (menu.c). arg: the
 * header title (const char *), NULL for "Videos". */
extern const struct ui_screen_ops ui_videos_screen;

#endif /* HAVE_BCMFW_VIDEO */

#endif /* IPODUI_SCREENS_VIDEO_H */
