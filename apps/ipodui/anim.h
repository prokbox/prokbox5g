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
 * iPod UI shell: frame clock, CPU boost, LCD push trace, offscreen strip
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
#ifndef _IPODUI_ANIM_H_
#define _IPODUI_ANIM_H_

#include <stdbool.h>
#include <stddef.h>
#include "kernel.h"
#include "lcd.h"
#include "logf.h"
#include "debug.h"

/* Trace of LCD pushes and animation state: logf on target, stderr in the
 * simulator (sim.log), checked by scripts/validate.sh. Lines:
 *   update full <screen>          whole LCD (boot, WPS return, no strip)
 *   update header y=0 h=20        header band
 *   update band y=.. h=..         static redraw after a selection move
 *                                 (old + new row, <= 2 rows)
 *   update anim:scroll y=.. h=..  one glide frame (the list view)
 *   update anim:slide y=.. h=..   one slide frame (all below the header)
 *   update marquee y=.. h=..      one marquee step (selected row band)
 *   update sbar x=.. y=.. w=.. h=..  scrollbar shown/hidden at rest
 *   anim start boost=N            first frame: cpu_boost(true); N is the
 *                                 firmware's boost count on target, the
 *                                 shell's own hold (1/0) in the simulator
 *   anim frame <ms>               ms since the previous frame (0 for the
 *                                 first frame of a run); the simulator
 *                                 appends " cpu=<us> pcpu=<us>": the UI
 *                                 thread's and the whole process's CPU
 *                                 time since the previous frame
 *   anim end boost=N              no animation left: cpu_boost(false) */
#define UPDATE_TRACE(fmt, ...) do { \
        logf("ipodui: " fmt, __VA_ARGS__); \
        DEBUGF("ipodui: " fmt "\n", __VA_ARGS__); \
    } while (0)

#define UI_ANIM_FRAME_TICKS (HZ/30)     /* ~30 fps cap (3 ticks = 33 fps) */

/* A screen's tick() returns this while it is animating: call again at
 * the next frame. Otherwise tick() returns ticks until its next timer
 * (marquee, scrollbar hide) or TIMEOUT_BLOCK. */
#define UI_TICK_ANIM (-2)

/* Frame pacing. Animations are time based (position from elapsed ticks),
 * so a slow frame makes the next one jump instead of lagging behind. */
bool ui_anim_frame_due(void);
int  ui_anim_wait(void);            /* ticks until the next frame is due */
/* Call right before drawing a frame: boosts on the first frame of a run
 * and logs the frame interval. */
void ui_anim_frame(void);
/* Starts the boost hold (and the "anim start" trace) before a frame's
 * composition, e.g. a slide's capture and first draw (F4: compose
 * boosted); the next ui_anim_frame() is due at once. No-op if held. */
void ui_anim_begin(void);
/* Called by the main loop when nothing animated in an iteration: drops
 * the boost. Balanced by construction (one boost held at most). */
void ui_anim_idle(void);
bool ui_anim_active(void);

/* Pushes an LCD rect (LCD coordinates) with the trace line
 * "update <kind> ...". Resets the current viewport first, since
 * lcd_update_rect() reads the current viewport's buffer. */
void ui_lcd_push(const char *kind, int x, int y, int w, int h);

/* The offscreen strip: one core_alloc block for the whole shell, shared
 * by the list's row cache (scroll frames) and the slide transition's
 * capture. A glide touches at most (rows per view + 1) rows, each in a
 * slot as tall as the list's tallest row: 9-row lists 10 x 25 px, 5-row
 * lists 6 x 44 px, 4-row lists 5 x 55 px (the largest: 275 px x 320 px
 * RGB565 = 176,000 B), which also holds a 320 x 220 body (140,800 B).
 * #38 (Rev 5 R1) also parks the drift's "next" cover here while a panel
 * rests: one 300 x 300 ARTCACHE_PANEL image, 180,000 B, so the strip is
 * sized for that (+4,000 B; the row-slot counts of every list style stay
 * the same). It also holds a full-height split capture (320 x 240 x 2 =
 * 153,600 B) and a highlight cross-fade's new panel (160 x 240 x 2 =
 * 76,800 B). Owners: lists >= 1 (ui_strip_new_owner()), the shell's
 * own users < 0 below. Who may claim when: preview.h. */
#define UI_STRIP_ROWS_SIZE  (5 * 55 * LCD_WIDTH * 2)
#define UI_STRIP_PANEL_SIZE (300 * 300 * 2)     /* artfmt.c ARTCACHE_PANEL */
#define UI_STRIP_SIZE   (UI_STRIP_PANEL_SIZE > UI_STRIP_ROWS_SIZE ? \
                         UI_STRIP_PANEL_SIZE : UI_STRIP_ROWS_SIZE)
#define UI_STRIP_OWNER_SLIDE (-1)   /* push/pop/split capture */
#define UI_STRIP_OWNER_PANEL (-2)   /* highlight cross-fade: the new panel */
#define UI_STRIP_OWNER_DRIFT (-3)   /* the drift's prefetched next cover */
#define UI_STRIP_OWNER_NP    (-4)   /* Now Playing (Phase 5), fixed offsets */
#define UI_STRIP_OWNER_MARQUEE (-5) /* a list's pre-rendered marquee label */
#define UI_STRIP_OWNER_USB   (-6)   /* the USB screen's disc and arrow frames (8a) */

bool ui_strip_init(void);           /* allocates once; false on failure */
bool ui_strip_available(void);
/* Pins the strip and returns it, or NULL if there is none. '*kept' says
 * whether 'owner' was also the last claimer (its contents are intact).
 * Every claim is paired with ui_strip_put() before yielding, except the
 * documented exceptions (preview.h): reads on the UI thread with the
 * strip pinned across one draw's I/O (the drift prefetch, the Now
 * Playing row's np_art_draw() and np_panel_draw()). The UI thread is the
 * strip's only user, so no other claimer can be waiting meanwhile. */
void *ui_strip_claim(int owner, bool *kept);
void ui_strip_put(void);
/* Whether 'owner' was the last claimer (its contents are intact), without
 * claiming: a plan can ask before it decides to take the strip. */
bool ui_strip_owned_by(int owner);
int  ui_strip_new_owner(void);      /* unique id for a list (>= 1) */

#endif /* _IPODUI_ANIM_H_ */
