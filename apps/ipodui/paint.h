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
 * iPod UI shell: shared painting primitives
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
#ifndef IPODUI_PAINT_H
#define IPODUI_PAINT_H

#include <stdbool.h>

/* A glossy, rounded-3D surface (docs/design/7g-spec.md, "Visual
 * language"): a base gradient top -> bot, the first 'split' 1/256ths of
 * the extent blended toward white (weight hi_top/256 at the leading edge
 * falling to hi_mid/256 at the split, then a one-row soft step), an
 * optional light first line and an optional darker last line. Colours
 * are computed per row (per column with UI_GLOSS_HORIZ); descriptors
 * live in theme.c. */
#define UI_GLOSS_LINE   0x01    /* first row/column is 'line' */
#define UI_GLOSS_EDGE   0x02    /* last row/column is 'edge' */
#define UI_GLOSS_HORIZ  0x04    /* gradient runs left -> right (vertical
                                 * bars such as the scrollbar thumb) */
struct ui_gloss {
    unsigned short top, bot;    /* base gradient ends (native colour) */
    unsigned short line, edge;  /* light first / dark last line */
    unsigned char hi_top;       /* white weight at the leading edge, /256 */
    unsigned char hi_mid;       /* white weight at the end of the gloss */
    unsigned char split;        /* gloss extent, /256 of the size */
    unsigned char flags;        /* UI_GLOSS_* */
};

/* Colour of row (column) i of a gloss surface n px in extent. */
unsigned ui_gloss_color(const struct ui_gloss *g, int i, int n);

/* Fills x, y, w, h (current viewport) with the gloss surface. Leaves the
 * viewport's foreground colour changed. */
void ui_paint_gloss(int x, int y, int w, int h, const struct ui_gloss *g);

/* Selection bar for one row at viewport coords: the sky gel of the
 * material kit (material.h UI_MAT_GEL_SKY: light top line, deepening
 * blue, a cyan lift in the lower third, darker bottom edge), one table
 * row per pixel row. Leaves the viewport's foreground colour changed. */
void ui_paint_selection(int x, int y, int w, int h);

/* Chevron '>' glyph, right-aligned so its right edge is at x_right,
 * vertically centred in [y, y+h). 'on_selection' picks white vs list_fg.
 * Anti-aliased by blending with the colour known to be underneath: the
 * selection bar row (when on_selection, [y, y+h) must be the same band
 * passed to ui_paint_selection) or list_bg. Uses the current viewport;
 * leaves its foreground colour changed. */
void ui_paint_chevron(int x_right, int y, int h, bool on_selection);

/* Width of the chevron glyph plus the gap between text and glyph (for
 * text truncation). The right margin, ui_theme->chevron_pad, is NOT
 * included: text must end before w - chevron_pad - ui_paint_chevron_w(). */
int  ui_paint_chevron_w(void);

/* Additions (documented, outside the fixed contract) */

/* Colour of row r (0-based) of a selection bar h px tall; the same
 * colours ui_paint_selection() draws (for anti-aliasing over it). */
unsigned ui_paint_selection_row(int r, int h);

/* The header background (gradient + bottom line) for columns x..x+w-1,
 * in the current viewport's coordinates, viewport y 0 = header top. */
void ui_paint_header_bg(int x, int w);

/* Blend two native colours; a = 0..15 is the weight of 'fg'. */
unsigned ui_paint_blend(unsigned bg, unsigned fg, int a);

/* -- #38 icon panels and inline values ------------------------------- */

/* Inline values (Settings rows): a list label "Name" UI_VALUE_SEP "Value"
 * shows Value right-aligned (ui_theme->value_fg, sel_fg on the
 * selection) where the chevron would be, and Name clips before it. */
#define UI_VALUE_SEP        '\x1f'
#define UI_VALUE_GAP        8       /* px between name and value (~) */

/* Colour of panel row y (0..239): ui_theme's panel gradient. */
unsigned ui_paint_panel_row(int y);
/* Fills rows y..y+h-1 of the current viewport (its full width) with the
 * panel gradient: one lcd_hline per row. */
void ui_paint_panel_bg(int y, int h);
/* Draws 's' centred on x = cx with the baseline of its last line at
 * y_last, word-wrapped to max_w and at most max_lines lines (the last
 * one ends in "..." if cut), 'pitch' px apart (0: the font height), in
 * the current viewport's font and fg (DRMODE_FG). Returns the number of
 * lines drawn. */
int  ui_paint_text_wrap(int cx, int y_last, int max_w, int max_lines,
                        int pitch, const char *s);
/* Draws 's' centred on x = cx, baseline y, clipped to max_w with "...". */
void ui_paint_text_centre(int cx, int y_base, int max_w, const char *s);

#endif /* IPODUI_PAINT_H */
