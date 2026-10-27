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
 * iPod UI shell: list-row art (thumbnail LRU, glyph tiles, unplayed dot)
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
#ifndef _IPODUI_ROWART_H_
#define _IPODUI_ROWART_H_

#include <stdbool.h>
#include <stdint.h>

/* The art boxes of two-line list rows (docs/phase4b-brief.md, agent C).
 *
 * Memory: one movable core_alloc block, reserved by ui_rowart_init() at
 * shell start (before any playback; nothing is allocated later): an LRU
 * of ROWART_THUMBS thumbnails at the art cache's thumbnail size, a small
 * table of keys (with the "no art" and "not built yet" answers too), and
 * two glyph tiles in the art box size: the music note, drawn once at
 * init, and one for the list's own glyph (podcast, book or film), drawn when a
 * list first asks for a glyph other than the one it holds (a list shows
 * one of them, so the 6 KB of a third tile is not needed). UI thread only; pinned only while a thumbnail is read.
 *
 * Reads: a thumbnail is read from the art cache (artcache_get(), one
 * "art get" trace line each) only when a row is drawn at rest and its key
 * isn't in the table: never while gliding. Without the block, every box
 * draws its glyph and nothing is read. */

#define ROWART_THUMBS   16

enum ui_rowart_glyph { UI_GLYPH_NOTE, UI_GLYPH_PODCAST, UI_GLYPH_BOOK,
#ifdef HAVE_BCMFW_VIDEO
                       UI_GLYPH_FILM,   /* a movie or show with no poster */
#endif
};

bool ui_rowart_init(void);              /* once, at shell start */

/* Draws the art box for 'key' (0: no art, the glyph) at x, y of the
 * current viewport, 'size' px square: the thumbnail (the art cache's
 * thumbnail size, which is the box), or the glyph tile while it has none. 'fetch': may read the art cache (at rest only). Returns false
 * when it drew the glyph for a key it has not looked up yet (a fetch
 * might find art), true otherwise. May yield when 'fetch' is set. */
bool ui_rowart_draw(uint64_t key, enum ui_rowart_glyph glyph, int x, int y,
                    int size, bool fetch);

/* At rest, about once a second: true if the art builder committed its
 * index since the last call (artcache_commit_seq(), F9) and some keys it
 * hadn't built were forgotten (redraw rows showing a glyph to pick their
 * art up). One volatile read when nothing changed. */
bool ui_rowart_poll(void);

/* Forgets every key (the library was rebuilt). */
void ui_rowart_flush(void);

/* The blue "unplayed" disc, 10 px, its top-left at x, y of the current
 * viewport; anti-aliased over the list background, or over the selection
 * bar of a row 'row_h' px tall when 'sel' (then white). */
void ui_rowart_dot(int x, int y, int row_h, bool sel);
#define UI_ROWART_DOT_D 10

#endif /* _IPODUI_ROWART_H_ */
