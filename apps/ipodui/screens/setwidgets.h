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
#ifndef IPODUI_SETWIDGETS_H
#define IPODUI_SETWIDGETS_H

#include <stdbool.h>
#include "lcd.h"

/* The light full-screen prompt and its segmented control (photo
 * audiobooks-speed.jpg), shared by Settings > Audiobooks, Reset Settings
 * (Reset | Cancel) and the On-The-Go Clear confirm (Clear | Cancel).
 * Moved verbatim from screens/settings.c by the P11 contract commit
 * (docs/phase11-brief.md section 4.8): the Audiobooks prompt is
 * pixel-identical. Geometry from the photo (~, +-2 px). */

#define UI_SEG_W        70      /* one segment */
#define UI_SEG_H        24
#define UI_SEG_SHELF_Y  180     /* the light shelf around the control */
#define UI_SEG_SHELF_H  52
#define UI_SEG_Y        (UI_SEG_SHELF_Y + (UI_SEG_SHELF_H - UI_SEG_H) / 2)
#define UI_SEG_MAX      3       /* segments */

/* The segmented control: n (<= UI_SEG_MAX) segments labelled lang[0..n-1]
 * (LANG_* ids), centred, segment 'sel' blue; with 'shelf' the shelf
 * around it too. A selection change redraws it with shelf false and
 * pushes ui_invalidate_band(UI_SEG_Y, UI_SEG_H). Draw only. */
void ui_seg_draw(const short *lang, int n, int sel, bool shelf);
/* The prompt's body: the light background below the header, the heading
 * (LANG id head_lang) and the body text (body_lang, wrapped, up to 3
 * lines). The caller then draws the control with ui_seg_draw(.., true).
 * Draw only. */
void ui_prompt_draw(int head_lang, int body_lang);
/* Greedy left-aligned word wrap in the current font, baselines from
 * y_base 'pitch' apart, at most max_lines; returns the lines drawn. */
int  ui_wrap_text(int x, int y_base, int pitch, int max_w, int max_lines,
                  const char *s);

#endif /* IPODUI_SETWIDGETS_H */
