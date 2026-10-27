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
 * iPod UI shell: video classification (Phase 14a)
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
#ifndef _IPODUI_VCLASS_H_
#define _IPODUI_VCLASS_H_

/* Pure C: no Rockbox headers, no statics, no allocation (host-tested by
 * tests/video/test_vclass.c). Contract: docs/phase14a-brief.md 3.2, 4.3.
 * Implemented by V2 in vclass_core.c; called only by ui_classify()
 * (apps/tagcache.c, the scanner, tagcache thread) under HAVE_BCMFW_VIDEO.
 * Never called on the audio thread (tagcache_ui_resumes() tests
 * id3->is_video directly) and never at browse time.
 *
 * Rules, first match wins (3.2):
 *   1. not is_video: returns 0, 'out' zeroed (the audio rules run as now).
 *   2. Podcast:  VIDEO | PODCAST   pcst, genre Podcast(s), or a path
 *                component "Podcasts". show = album.
 *   3. TV:       VIDEO | TV        stik 10, a tvsh tag, the nearest
 *                category folder "TV Shows", or a filename SxxEyy / NxNN.
 *   4. Movie:    VIDEO             everything else (stik 6 and 9 too).
 * A stik beats the folder. Patterns are case-insensitive:
 *   S(\d{1,2})[ ._-]?E(\d{1,3}), (\d{1,2})x(\d{2,3}),
 *   folder ^(Season|Series|S)\s*(\d{1,2})$.
 *
 * The flag bits are tagcache.h's TAGCACHE_UI_VIDEO / _TV / _PODCAST;
 * vclass_core.c repeats their values as VCLASS_* (it can't include
 * tagcache.h) and tagcache.c checks they agree. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VCLASS_PODCAST  0x0040      /* == TAGCACHE_UI_PODCAST */
#define VCLASS_VIDEO    0x0100      /* == TAGCACHE_UI_VIDEO */
#define VCLASS_TV       0x0200      /* == TAGCACHE_UI_TV */

struct vclass_in {
    const char *path;               /* full path of the file */
    const char *album, *title, *genre, *tvsh;   /* NULL = untagged */
    bool is_video, pcst;            /* mp3entry.is_video / .is_podcast */
    int stik, tvsn, tves;           /* mp3entry fields, 0 = none */
};

struct vclass_out {
    int32_t flag;                   /* VCLASS_* bits, 0 = not a video */
    const char *show;               /* TV / podcast: show ("" = unknown:
                                     * the screen shows
                                     * LANG_IPODUI_UNKNOWN_SHOW; nothing
                                     * like "Unknown Show" is stored) */
    const char *title;              /* title to store: tag or cleaned name */
    int season;                     /* TV: >= 1 (default 1) */
    int episode;                    /* TV: 0 = unnumbered (sorted after
                                     * numbered ones, by title) */
};

/* Classifies one scanned file. 'buf' is the caller's ui_title[MAX_PATH]
 * (add_tagcache()'s stack buffer, no static): cleaned strings are written
 * into it, split in two halves (show, title); out->show and out->title
 * point into 'buf' or into 'in'. Stored by the scanner as album <- show,
 * discnumber <- season, tracknumber <- episode, title <- title.
 * Returns out->flag. */
int32_t vclass_classify(const struct vclass_in *in, struct vclass_out *out,
                        char *buf, size_t len);

#endif /* _IPODUI_VCLASS_H_ */
