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
 * iPod UI shell: Now Playing lyrics wrapping (Phase 5)
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
#ifndef _IPODUI_NP_LYRICS_H_
#define _IPODUI_NP_LYRICS_H_

/* Pure C: no Rockbox headers, no statics, no allocation (host-tested by
 * tests/npscreen/test_lyrics.c). The text is the embedded lyrics as read
 * (UTF-8, <= NP_LYRICS_MAX bytes in the strip); the wrap result is one
 * uint16_t start offset per screen line (NP_LYRICS_LINES_MAX in the
 * strip). Widths come from a per-character callback (the font's advance
 * widths), so the same code runs on the host.
 *
 * Rules (frozen default 9.9, ⚠️): lines break at LF (CR, CRLF accepted),
 * then at the last space that fits; a word wider than the line breaks
 * between characters. Leading "[mm:ss.xx]" time tags of synchronised
 * (LRC-style) lyrics are skipped. Blank lines are kept (one empty line),
 * as they separate verses. */

#include <stdint.h>

struct np_lyr_font {
    int (*advance)(void *ctx, unsigned cp);     /* px, >= 0 */
    void *ctx;
    int width;                                  /* line width, px */
};

/* Wraps text[0..len) into at most max_lines lines. starts[i] is the byte
 * offset of line i. Returns the number of lines (0 for empty text). Text
 * past max_lines is dropped (the caller logs). */
int np_lyrics_wrap(const char *text, int len, const struct np_lyr_font *f,
                   uint16_t *starts, int max_lines);

/* Line i's visible bytes: [*b, *e) without the line break, trailing
 * spaces and leading time tags. n = the count np_lyrics_wrap() returned. */
void np_lyrics_line(const char *text, int len, const uint16_t *starts,
                    int n, int i, int *b, int *e);

/* max(0, lines - visible): the scroll range for np_set_lyrics() */
static inline int np_lyrics_max_top(int lines, int visible)
{
    return lines > visible ? lines - visible : 0;
}

#endif /* _IPODUI_NP_LYRICS_H_ */
