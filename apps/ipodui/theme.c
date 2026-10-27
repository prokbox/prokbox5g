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
#include "config.h"
#include <stdio.h>
#include "lcd.h"
#include "font.h"
#include "rbpaths.h"
#include "logf.h"
#include "ipodui/theme.h"
#include "ipodui/screens/panels.h"
#include "ipodui/paint.h"
#include "ipodui/material.h"

/* Header, y 0..19: the material kit's header rows (art/materials.py
 * HEADER, the main.png profile measured there). */
typedef char header_rows_fit[(UI_MAT_HEADER_ROWS == UI_HEADER_ROWS) ? 1 : -1];

/* Glossy surfaces (paint.h, docs/design/7g-spec.md "Visual language").
 * Every selection blue (the list bar, the speed prompt's segment) is the
 * material kit's sky gel (material.h UI_MAT_GEL_SKY, art/materials.py). */

/* Scrollbar thumb (no reference, ~): a grey rounded rod lit from the
 * left, so the gloss runs across its width. */
static const struct ui_gloss sbar_gloss = {
    .top    = LCD_RGBPACK(0x8c, 0x8c, 0x8c),
    .bot    = LCD_RGBPACK(0x66, 0x66, 0x66),
    .edge   = LCD_RGBPACK(0x58, 0x58, 0x58),
    .hi_top = 120,
    .hi_mid = 64,
    .split  = 128,
    .flags  = UI_GLOSS_EDGE | UI_GLOSS_HORIZ,
};

/* Writable only for the three font ids filled in by ui_theme_init();
 * the colour tables above stay in flash/rodata. */
static struct ui_theme theme = {
    .header_h         = 20,
    .header_title_x   = 6,      /* cell x; "iPod" ink starts at x=7 */
    .header_baseline  = 14,     /* "iPod" cap rows 5-13 in main.png */
    .header_title_pad = 4,
    .header_side_w    = 66,     /* lock + play + battery + gaps + margin */
    .header_margin_r  = 5,      /* nub ends at x 154 (155 edge) */
    .header_glyph_gap = 7,      /* play -> battery (audit fix #7) */
    .header_rows      = ui_mat_header_rows,
    .header_fg        = LCD_RGBPACK(0x00, 0x00, 0x00),
    .header_glyph_fg  = LCD_RGBPACK(0x3a, 0x3a, 0x3a),
    /* Owner photos (artists-list-b, songs-list-scrollbar-b,
     * albums-list-twoline-b, music-menu-b), rectified to 320 x 240:
     * 9 single-line rows (cap rows 6/7-17 of each row, also on the
     * selection), 5 two-line song rows of 44 px, 4 album rows of 55 px
     * with a 55 px art box and text at x 65; header 20 + body 220. */
    .list_rows        = 9,
    .list_rows_two    = 5,
    .list_rows_art    = 4,
    .row_baseline     = 18,
    .text_pad_x       = 5,      /* cell x; ink at x 6 (audit fix #10) */
    .two_block_h      = 29,
    .two_title_base   = 12,
    .two_sub_dy       = 17,
    .art_box          = 55,
    .art_text_x       = 65,
    .dot_gutter       = 14,     /* ~ no reference */
    /* second line: ~#707070 regular (thin strokes read lighter in the
     * photos); on the selection about as bright as the title: white */
    .list_fg_sub      = LCD_RGBPACK(0x70, 0x70, 0x70),
    .sel_fg_sub       = LCD_RGBPACK(0xff, 0xff, 0xff),
    .list_sep         = LCD_RGBPACK(0xd6, 0xd6, 0xd6),
    .chevron_pad      = 6,      /* chevron ink x 145-153, values end at
                                 * 153.5 (audit fixes #8, #9) */
    .split_x          = 160,
    .list_bg          = LCD_RGBPACK(0xff, 0xff, 0xff),
    .list_fg          = LCD_RGBPACK(0x00, 0x00, 0x00),
    .list_fg_dim      = LCD_RGBPACK(0x3c, 0x3c, 0x3c),
    .list_fg_off      = LCD_RGBPACK(0xa8, 0xa8, 0xa8),  /* ~ no reference */
    .sel_fg           = LCD_RGBPACK(0xff, 0xff, 0xff),
    .sel_bg           = LCD_RGBPACK(34, 141, 219), /* sky gel 12/24 */
    .sbar_gloss       = &sbar_gloss,
    .panel_bg         = LCD_RGBPACK(46, 47, 52),
    .panel_top        = LCD_RGBPACK(0x4b, 0x56, 0x6c),
    .panel_mid        = LCD_RGBPACK(0x1e, 0x26, 0x38),
    .panel_bot        = LCD_RGBPACK(0x07, 0x09, 0x0f),
    .panel_mid_y      = 110,
    .panel_fg         = LCD_RGBPACK(0xff, 0xff, 0xff),
    .tile_top         = LCD_RGBPACK(0x3a, 0x3d, 0x45),
    .tile_bot         = LCD_RGBPACK(0x04, 0x05, 0x08),
    .tile_rim         = LCD_RGBPACK(0x6a, 0x6e, 0x78),
    .value_fg         = LCD_RGBPACK(0x1f, 0x6c, 0xd6),
    .font_ui          = FONT_UI,
    .font_title       = FONT_UI,
    .font_small       = FONT_UI,
};

const struct ui_theme *ui_theme = &theme;

/* Shell fonts, installed from apps/ipodui/fonts/ (TeX Gyre Heros, GUST
 * license, converted with tools/convttf -A). font_load_ex(path, 0, 0)
 * keeps the whole file (up to MAX_FONT_SIZE, 60000 B) in one movable
 * buflib allocation: no glyph cache, no .gc file and no disk access while
 * drawing. RAM = the file sizes, 91,570 B for the three. */
static int load_font(const char *name, int cur)
{
    char path[64];
    int id;

    if (cur != FONT_UI)
        return cur;     /* already loaded by an earlier ui_theme_init() */
    snprintf(path, sizeof(path), FONT_DIR "/%s.fnt", name);
    id = font_load_ex(path, 0, 0);
#ifdef HAVE_UI_THREAD_SPLIT
    /* P19 S3: pinned for the shell's life (loaded once, before playback):
     * the COP draws from it while a compaction on the CPU could move it */
    if (id >= 0)
        font_lock(id, true);
#endif
    if (id < 0)
    {
        logf("ipodui: font %s failed (%d), using FONT_UI", path, id);
        return FONT_UI;
    }
    return id;
}

void ui_theme_init(void)
{
    theme.font_ui    = load_font("15-ShellSans-Bold", theme.font_ui);
    theme.font_title = load_font("12-ShellSans-Bold", theme.font_title);
    theme.font_small = load_font("12-ShellSans", theme.font_small);
    ui_theme = &theme;
    ui_settings_init();             /* localised Settings labels (#38) */
}
