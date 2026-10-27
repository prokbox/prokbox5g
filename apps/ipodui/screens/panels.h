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
 * iPod UI shell: panel providers and the Settings tree (#38 contracts)
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
#ifndef _IPODUI_PANELS_H_
#define _IPODUI_PANELS_H_

#include <stddef.h>
#include <stdint.h>
#include "ipodui/preview.h"
#include "ipodui/menu.h"

/* Agent B's side of the #38 contract (owner of this header after the
 * contracts commit). Providers follow struct ui_preview_ops (preview.h);
 * all are icon panels: rest NAV_REST_ICON, UI_PANEL_XFADE, drawn from RAM
 * (compiled-in original artwork from the art/ generators), no storage access.
 *
 * Implemented in screens/panels.c (providers) and screens/settings.c
 * (the Settings tree). */

/* Main menu > Podcasts: podcast glyph + "N Podcasts" (LANG_*). arg NULL. */
extern const struct ui_preview_ops ui_preview_podcasts;
/* Main menu > Extras: world map + clock + date (stub art in v1). arg
 * NULL. Animated: draw() returns ticks to the next minute. */
extern const struct ui_preview_ops ui_preview_extras;
/* Main menu > Settings and Settings > About: the device name (a stored
 * string setting, default "ProkBox", never hard-coded), the wheel glyph
 * scaled down, the capacity capsule bar and "N GB Free" (a cached FAT
 * free-space figure; draw() never wakes storage). arg NULL. */
extern const struct ui_preview_ops ui_preview_capacity;
/* A Settings item's tile, reflection and caption (catalogue §7.1). arg:
 * the item's tile descriptor, a type private to screens/settings.c. */
extern const struct ui_preview_ops ui_preview_settings_tile;

/* The Settings menu (stock 7G tree; split, like the main menu). menu.c
 * points the main menu's Settings item at it. Its labels are filled in
 * from the language at shell start (ui_settings_init()) and its inline
 * values refreshed whenever menu.c rebuilds its rows (the first item's
 * visible() hook), so it is not const. */
extern struct ui_menu ui_settings_menu;
/* Shell start (from ui_theme_init()) and after a language change. */
void ui_settings_init(void);

/* Scratch: none since 8b-3. Every provider here composes in place, in
 * the panel's own pixels after its background (panels.c "composition,
 * in place"); the panel scratch (preview.h) is not used by panels.c. */

/* -- Settings tiles (screens/settings.c draws its items through this) -- */

/* The tile art: a glyph of panelart.h (enum panelart_glyph, 0..), or one
 * of these. */
#define UI_TILE_CALENDAR    100     /* a page with this month and day */
#define UI_TILE_LOGO        101     /* no tile: our wheel glyph (Legal) */
#define UI_TILE_WORDS       102     /* no tile: "Welcome" (Language) */

/* Draws a whole Settings panel into p: 'title' at the top, the tile (and
 * its reflection) in the middle, 'caption' (wrapped, up to 3 lines) at
 * the bottom. 'caption' may be NULL. Same rules as a provider's draw(). */
#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a: the Videos panel without posters (film strip and the count),
 * a provider's draw() into p */
void ui_panel_draw_videos(const struct ui_panel *p);
#endif
void ui_panel_draw_tile(const struct ui_panel *p, const char *title,
                        int art, const char *caption);
/* What a tile provider returns after ui_panel_draw_tile() (8b-3): ticks
 * until the art changes by itself (the calendar page at midnight), else
 * TIMEOUT_BLOCK. */
int ui_panel_tile_wake(int art);

/* The device name: the "device name" setting, or the localised default
 * (LANG_IPODUI_DEVICE_NAME, "ProkBox"). */
const char *ui_device_name(void);

/* -- P11 exports (docs/phase11-brief.md section 4.8; About, U2b) ---------- */

/* "128" or "19.9": KiB as GB (2^30 B, as the iPod shows its capacity),
 * into buf; returns buf. The Settings capacity panel uses it too. */
const char *ui_fmt_gb(unsigned long kib, char *buf, size_t len);
/* The capacity capsule painted straight into the framebuffer at x, y
 * (w x h, inside the LCD; any size, the panel's is 130 x 12), blended over
 * what is there (paint the background first): segment k (Audio, Video,
 * Photos, Other, in that order, from its fill table in panels.c) takes
 * seg_frac[k] / UI_CAPSULE_ONE of the width after the segments before it;
 * the rest is the dark track. Draw only (no lcd_update); sets the default
 * viewport. ~15 instructions per pixel. */
#define UI_CAPSULE_SEGS 4
#define UI_CAPSULE_ONE  1024
void ui_capsule_draw(int x, int y, int w, int h, const uint16_t *seg_frac,
                     int nseg);
/* 8b-3, the glossy metal capsule itself (About draws its own through it):
 * w x h into px (row stride 'stride' pixels), blended at its round ends
 * over what is there; segment k (col[k], a raised gloss) from end[k - 1]
 * (0 for k = 0) to end[k] - 1, the rest free_col, recessed (a channel)
 * when free_inset, else raised. ~8 instructions per pixel. */
void ui_capsule_paint(fb_data *px, int stride, int w, int h, const int *end,
                      const unsigned short *col, int nseg,
                      unsigned free_col, bool free_inset);
/* A legend square (About page 1) in the capsule's raised gloss: s x s
 * into px, its corner pixels half blended. */
void ui_gel_square(fb_data *px, int stride, int s, unsigned col);

/* -- P12 Clocks (W1-C): one copy of the Extras art, shared with the
 * Clocks page (docs/phase12-brief.md checklist 12) --------------------- */

struct tm;
/* The panels' time: the RTC, pinned in a scripted simulator run
 * (sim_clock, else 10:08 on Tuesday 6 October 2026), a sane date when the
 * RTC is invalid. */
struct tm *ui_panel_time(void);
/* Rows [y0, y0 + h) (screen rows; the map spans 20..199) of a 160 px wide
 * column of the Extras backdrop, written at px (row y0) with row stride
 * 'stride': the panel's dark ramp and the land of 'mask' over it. mask
 * NULL is the panel's own map, longitudes 0..180 E (the right half of the
 * Clocks page); else a mask of the same geometry and format (the page's
 * western half, clockart.h). Plain stores, ~3 instructions per pixel. */
void ui_extras_backdrop(fb_data *px, int stride, int y0, int h,
                        const unsigned char *mask);
/* The Clocks page's whole backdrop (both halves: the west from clockart.h,
 * resident so the reveal can draw it after the page has gone) in the
 * framebuffer: rows [y0, y0 + h), columns 0..w-1 (w <= LCD_WIDTH). */
void ui_extras_page_backdrop(int y0, int h, int w);
/* ... columns [x0, x1) only (clipped to the screen): the reveal's
 * frames redraw just what the split screen has uncovered. */
void ui_extras_page_backdrop_cols(int y0, int h, int x0, int x1);
/* An analog clock's hands and hub, anti-aliased, centred on ctr: hour and
 * minute hands hour_l and min_l px long in 'hand', the hub pin in 'pin'
 * (the Extras panel: 21, 32). Blends over what is there; columns right
 * of ctr + xmax are left alone (the reveal's clip at the screen edge). */
void ui_clock_hands(fb_data *ctr, int stride, int hour, int min,
                    int hour_l, int min_l, unsigned hand, unsigned pin,
                    int xmax);

#endif /* _IPODUI_PANELS_H_ */
