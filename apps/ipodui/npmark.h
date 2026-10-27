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
 * iPod UI shell: the now-playing marker in lists (#69)
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
#ifndef _IPODUI_NPMARK_H_
#define _IPODUI_NPMARK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The 7G marks the row of the track playing now with the speaker-with-
 * waves glyph at the row's right edge (spec music-lists.md "Now-playing
 * marker", owner 2026-10-12): the loud end of Now Playing's volume bar
 * (NPG_SPEAKER_WAVES, 18 x 13), black on a plain row, white on the blue
 * gel. Our addition: the playing book's row in Audiobooks and the current
 * entry (chapter or file) on a book's page.
 *
 * Drawing: the coverage mask is converted once (first draw) into the LCD
 * driver's 4-bit alpha format and blitted with one lcd_alpha_bitmap_part()
 * call (the anti-aliased font path) in the row's ink: it blends over the
 * gel's own rows, whatever the row height, with no per-pixel lcd calls.
 *
 * Identity, no I/O: a song row compares the library id (lib_id(), what
 * browse.c restores the selection by) with the playing id3's tagcache
 * index; a book row its key (lib_art_key()) with the book map's; a book
 * page entry the entry Now Playing's counter shows (np_pb_book_entry()).
 *
 * Moving: list screens poll from their tick() at rest, on the main loop's
 * existing wake-ups (HZ/2 while lit and playing; no timer of our own),
 * with the backlight on. np_pb_track() (+1 per PLAYBACK_EVENT_TRACK_
 * CHANGE) and the entry are compared with what the list drew; only the
 * visible rows whose mark changed are invalidated (the old row and the
 * new one). */

#define UI_NPMARK_W     18      /* the glyph (npglyphs.h) */
#define UI_NPMARK_H     13
#define UI_NPMARK_GAP   4       /* label end to the glyph, as to a chevron */

/* What a list compares its rows with. tc: the playing track's
 * tagcache_idx (database index + 1), 0 none; book: the playing book's
 * key (book queue), 0 none; entry: its current entry, -1 unknown. */
struct ui_npmark_now {
    long     tc;
    uint64_t book;
    int      entry;
};

#define UI_NPMARK_TC    0x01    /* fill tc */
#define UI_NPMARK_BOOK  0x02    /* fill book and entry */

/* Fills *n with what plays now (stopped: all none). Returns false if
 * playback is rewriting the track's data (seqlock): keep the old value
 * and ask again at the next tick. No I/O, no yield. */
bool ui_npmark_get(struct ui_npmark_now *n, unsigned what);

struct ui_list;
/* At rest, from a list screen's tick(): reads what plays now and, if it
 * differs from *cur (what the rows were drawn with), stores it in *cur
 * and invalidates the band of every visible row whose l->has_mark()
 * answer changed. Nothing while gliding or with the backlight off (the
 * next lit tick catches up). */
void ui_npmark_sync(struct ui_list *l, struct ui_npmark_now *cur,
                    unsigned what);

/* Draws the glyph in the current viewport, its right edge (exclusive) at
 * x_right, centred in a row 'h' px tall; white on the selection, else
 * black. Leaves the draw mode solid. */
void ui_npmark_draw(int x_right, int h, bool sel);

/* Where the playing queue came from: a screen calls this right after it
 * starts playback, with a key for its list (0 = no list to carry on in).
 * The playlist's creation stamp ties it to that queue. */
#define UI_NPMARK_ORIGIN_OTG  1u
void ui_npmark_origin(uint32_t key);
uint32_t ui_npmark_key(const void *p, size_t n);

/* SELECT on the marked row: the item already plays, so carry on with it
 * (resume if paused) and show Now Playing instead of starting it over.
 * Songs only when 'key' is the list the queue was started from (another
 * list starts its own queue, as before); 'qpos' >= 0 must also be the
 * queue position playing (a list with repeats: On-The-Go). A book's
 * chapter page passes UI_NPMARK_BOOK and needs neither. Refreshes 'cur'
 * first; true if it handled the select. */
bool ui_npmark_continue(struct ui_list *l, struct ui_npmark_now *cur,
                        unsigned what, uint32_t key, int qpos);

#endif /* _IPODUI_NPMARK_H_ */
