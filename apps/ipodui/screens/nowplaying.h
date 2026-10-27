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
 * iPod UI shell: Now Playing screen (Phase 5 contract, NAV-H1/H2)
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
#ifndef _IPODUI_NOWPLAYING_H_
#define _IPODUI_NOWPLAYING_H_

#include "ipodui/screen_stack.h"

/* Owned by Phase 5 (docs/phase5-brief.md §4), not by the #38 agents; the
 * call sites in ui_main.c are #38 A's.
 *
 * NAV-H1: ui_show_wps() pushes this screen (arg NULL) once audio plays
 * or a resume started. enter() < 0 refuses the push; ui_show_wps() then
 * repaints the screen below in full. On nav the shared stub
 * (screens/nowplaying.c) runs the stock WPS inside enter() and returns
 * -1, so behaviour is as before. Full-screen: while it is on top the
 * panel and drift code never claim the strip (preview.h). */
extern const struct ui_screen_ops ui_nowplaying_screen;

/* NAV-H2: called by the main loop on every pass, on the UI thread, shown
 * or not (Now Playing's refill). Returns ticks until it wants the next
 * call, or TIMEOUT_BLOCK. Never blocks, never allocates. */
int ui_nowplaying_idle(void);

/* Reserves the "np" block (49,152 B, core_alloc, never freed). The shell
 * calls it once at start, after ui_strip_init(), before anything can
 * play. False (logged): no memory; Now Playing then refuses its push. */
bool ui_nowplaying_reserve(void);

/* Auto-return (owner, 2026-10-08): while anything plays (music, books,
 * podcasts alike) and the wheel and buttons have been idle on any other
 * screen for max(5 s, backlight timeout - 3 s) (8 s with the backlight
 * always on), ui_nowplaying_idle() pushes Now Playing on top, so MENU
 * returns to where the user was. Not over Now Playing itself or a value
 * editor (the USB screen runs outside the loop). The shell's input path
 * calls this on every input event (next to artcache_note_input()); until
 * it is called once, auto-return stays off. Trace: "np auto-return
 * after=<ms>". */
void ui_nowplaying_note_input(void);

/* #126: the charger's Visualizer face (the hold list's While Charging), from
 * unlit.c when the backlight times out: true if Now Playing (on top,
 * music) now shows its Visualizer and keeps the screen lit; false, the
 * clock face as usual */
#include <stdbool.h>
bool ui_nowplaying_viz_face(void);

#endif /* _IPODUI_NOWPLAYING_H_ */
