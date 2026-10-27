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
 * iPod UI shell
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
#ifndef _IPODUI_THEME_H_
#define _IPODUI_THEME_H_

struct ui_gloss;

/* All metrics, colours and font ids of the shell, in one place.
 * Values come from docs/reference/7g/MEASUREMENTS.md. */

#define UI_HEADER_ROWS  20      /* header_rows[]: 19 gradient rows + line */

struct ui_theme {
    /* header bar */
    int header_h;               /* 20, incl. the 1 px bottom line */
    int header_title_x;         /* 6: title cell x (ink at 7, as main.png) */
    int header_baseline;        /* 14: title baseline (first row under ink) */
    int header_title_pad;       /* min gap title <-> glyph cluster */
    int header_side_w;          /* glyph cluster region at the right */
    int header_margin_r;        /* battery nub to the header's right edge */
    int header_glyph_gap;       /* 7: lock / play / battery spacing */
    /* per-row colour of the header, y 0..19 (the last entry is the 1 px
     * bottom line); sampled from main.png, non-linear */
    const unsigned short *header_rows;
    unsigned header_fg;         /* title */
    unsigned header_glyph_fg;   /* lock, bolt */
    /* lists (docs/design/7g-spec.md "List geometry"; owner photos,
     * rectified to 320x240): rows fill the 220 px body exactly; the
     * selection bar fills its whole row */
    int list_rows;              /* 9: single-line rows per view (4 x 25 px,
                                 * then 5 x 24 px) */
    int list_rows_two;          /* 5: two-line rows (songs, episodes), 44 px */
    int list_rows_art;          /* 4: two-line rows with art (albums,
                                 * shows), 55 px */
    int row_baseline;           /* 18: single-line text baseline (first row
                                 * under ink) from a 25 px row's top; 17
                                 * in a 24 px row */
    int text_pad_x;             /* 5: text cell x (ink at 6) */
    /* two-line rows: the title (font_ui, cap 11) and the second line
     * (font_small, cap 9) form a block centred in the row, 1 px high:
     * title baseline at (h - two_block_h) / 2 - 1 + two_title_base,
     * second line two_sub_dy below it (44 px: 18 / 35; 55 px: 24 / 41) */
    int two_block_h;            /* 29: title cap top to second baseline */
    int two_title_base;         /* 12 */
    int two_sub_dy;             /* 17 */
    int art_box;                /* 55: art box, the full row height */
    int art_text_x;             /* 65: text cell x right of the art box */
    int dot_gutter;             /* 14: room left of the text for the
                                 * unplayed dot (podcast rows) */
    unsigned list_fg_sub;       /* second line, grey */
    unsigned sel_fg_sub;        /* second line on the selection */
    unsigned list_sep;          /* two-line row separator */
    int chevron_pad;            /* 6: chevron right ink column to the
                                 * list right edge (exclusive) */
    int split_x;                /* 160: main menu list width; panel is x>=split_x */
    unsigned list_bg, list_fg;
    unsigned list_fg_dim;       /* secondary text, values (#3C3C3C) */
    unsigned list_fg_off;       /* a row that can't be chosen now (its
                                 * label and value): light grey */
    unsigned sel_fg;            /* text and chevron on the selection */
    unsigned sel_bg;            /* solid stand-in for the bar (its midpoint);
                                 * draw the bar with ui_paint_selection() */
    /* glossy surfaces (paint.h), shared so one descriptor retunes every
     * place that uses it */
    const struct ui_gloss *sbar_gloss;  /* scrollbar thumb: grey, HORIZ */
    unsigned panel_bg;
    /* #38 icon panels (Settings tiles, capacity, podcasts): a vertical
     * slate -> navy -> near-black gradient (owner photos settings-*.jpg,
     * ~), y 0, panel_mid_y and 239; white title/caption text; the dark
     * glossy tile */
    unsigned panel_top, panel_mid, panel_bot;
    int panel_mid_y;
    unsigned panel_fg;
    unsigned tile_top, tile_bot, tile_rim;
    /* blue inline values on Settings rows (settings-01/02 photos) */
    unsigned value_fg;
    /* fonts: shell fonts if they loaded, FONT_UI otherwise */
    int font_ui;                /* list rows: Bold 15 (cap 11) */
    int font_title;             /* header, Now Playing secondary: Bold 12
                                 * (cap 9) */
    int font_small;             /* small text: Regular 12 */
};

extern const struct ui_theme *ui_theme;
void ui_theme_init(void);                    /* loads fonts, sets ui_theme */

#endif /* _IPODUI_THEME_H_ */
