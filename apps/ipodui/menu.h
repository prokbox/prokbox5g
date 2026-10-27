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
#ifndef _IPODUI_MENU_H_
#define _IPODUI_MENU_H_

#include <stdbool.h>
#include "ipodui/screen_stack.h"

struct ui_preview_ops;                    /* preview.h */

struct ui_menu_item {
    const char *label;
    const struct ui_screen_ops *target;   /* NULL: action only */
    const void *arg;                      /* NULL: the label is passed */
    bool (*visible)(void);                /* NULL = always */
    void (*action)(void);                 /* run on select if target NULL */
    /* #38: the split-screen panel for this item (preview.h). NULL: no
     * preview, the old panel stays. Only read on split menus. */
    const struct ui_preview_ops *preview;
    const void *preview_arg;              /* passed to preview->draw() */
};

struct ui_menu {
    const char *title;
    const struct ui_menu_item *items;
    int count;
    bool split;                           /* main menu: list left, panel right */
    bool lib_footer;                      /* a Music screen: the "Updating
                                             Library..." footer (below) */
    /* optional: called on enter and before every full draw (push, pop,
     * return from a full-screen screen), before visible() is asked, so
     * a menu can refresh what its labels and visibility read (e.g. the
     * Settings values). NULL = none. UI thread; must not draw. */
    void (*refresh)(void);
};

extern const struct ui_screen_ops ui_menu_screen;    /* arg: const struct ui_menu* */
extern const struct ui_screen_ops ui_stub_screen;    /* arg: const char *title */

extern const struct ui_menu ui_main_menu;

/* -- "Updating Library..." footer (8b Goal 6, owner 2026-10-11) ---------
 * The one sign that the library is being built: a slim band over the
 * bottom UI_LIBFOOT_H px of a Music screen's list (the Music menu and the
 * lists under it, Search included), shown only while lib_state() ==
 * LIB_UPDATING. It covers the last row area with no layout shift and is
 * the explanation for the lists that are empty meanwhile. The list
 * background darkened about 35 % with a 1 px top highlight (a per-row
 * table built once), centred small-font text and the 8-dot activity
 * glyph; it fades in and out over UI_LIBFOOT_STEPS frames (~200 ms).
 * One state for the whole shell: only the top screen ticks, so the band
 * keeps its level from one Music screen to the next. UI thread only.
 *
 * ui_libfoot_overlay(): from the list's overlay hook (list.h), after the
 *   rows of the LCD band [y, y + h) were drawn: draws the footer's part
 *   of that band (nothing at level 0). The screen's draw pushes it.
 * ui_libfoot_tick(): from the screen's tick(), after ui_list_tick(): the
 *   fade and the glyph steps, each drawn and pushed in the footer's own
 *   band only; the last fade-out step has the rows under it redrawn
 *   (ui_invalidate_band()). Returns ticks until its next step, or
 *   TIMEOUT_BLOCK (no timer of its own: the main loop's own wake-ups,
 *   at most 1 s apart while lit, notice the library state). */
#define UI_LIBFOOT_H        18
#define UI_LIBFOOT_STEPS    6
struct ui_list;
void ui_libfoot_overlay(const struct ui_list *l, int y, int h);
int  ui_libfoot_tick(const struct ui_list *l);

#endif /* _IPODUI_MENU_H_ */
