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
 * iPod UI shell: header bar
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
#ifndef IPODUI_HEADER_H
#define IPODUI_HEADER_H

#include <stdbool.h>

/* struct ui_screen.header_w of a pushed dark full-screen screen (About,
 * Brightness, Charging, Volume Limit; P11): the main loop draws the dark
 * header (usbscreen.h ui_dark_header()) with the screen's title instead
 * of the light one, and slides the whole screen when the header kind
 * changes. ui_screen_is_split() stays false (header_w <= 0). */
#define UI_HEADER_DARK  (-1)
/* struct ui_screen.header_w of a headerless full-screen page (Extras >
 * Clocks, P12 W2-W): the main loop draws no header and pushes none, so a
 * header-only redraw (the battery, Hold, Time in Title) never lands on
 * the page; slides treat it as a header of height 0.
 * ui_screen_is_split() stays false. */
#define UI_HEADER_NONE  (-2)

struct ui_header_state {
    const char *title;          /* the text shown: the screen title, or
                                   the clock with Time in Title */
    const char *ghost;          /* mid cross-fade: the text going away,
                                   both drawn half-faded; else NULL */
    bool centre;                /* centred (dark screens), else left */
    int  width;                 /* header width from x 0; 0 = LCD_WIDTH */
    enum { UI_PLAY_NONE, UI_PLAY_PLAYING, UI_PLAY_PAUSED } play;
    int  battery_pct;           /* battery_level() */
    bool charging;              /* charging_state() */
    bool powered;               /* charger_inserted(): plug when not charging */
    bool hold;                  /* button_hold() */
    unsigned char busy;         /* #84: the 8-dot activity glyph, 0 = off,
                                   else 1..8 the lead dot (usbbusy.h) */
};

/* Draws only if 'st' differs from the last drawn state (or force).
 * Only the parts that changed (title, glyph cluster) are repainted.
 * Returns true if pixels changed; the caller then adds the header band
 * (0, 0, LCD_WIDTH, ui_theme->header_h) to its lcd_update_rect(). Never
 * calls lcd_update*() itself. */
bool ui_header_draw(const struct ui_header_state *st, bool force);
/* The x range (full header height) the last ui_header_draw() changed: the
 * whole width, or just the activity glyph when only it stepped (#84). */
void ui_header_push_rect(int *x, int *w);

/* Fill 'st' from system state. Leaves st->title and st->width alone. */
void ui_header_poll(struct ui_header_state *st);

/* Time in Title (docs/design/7g-spec.md, Global chrome): the header text
 * shows the screen's title while the user moves (after every screen
 * change and every input), then the clock once input has rested 2.5 s;
 * the clock stays until the next input (owner, 2026-10-10). The swap is a
 * one-frame (30 ms) cross-fade.
 * Sets st->title and st->ghost for 'title' on 'screen' (any pointer
 * identifying the screen; a new one, or a new title, restarts the cycle
 * with the title). While the backlight is off ('lit' false) the text
 * stays as it is. Returns ticks until the text next changes, or
 * TIMEOUT_BLOCK: the caller wakes then, only while lit. No boost, no
 * allocation; the clock is read from the RTC only while it shows. */
int ui_header_text(struct ui_header_state *st, const void *screen,
                   const char *title, bool lit);

/* Every input: the title comes back and the idle wait restarts. Returns
 * true when the clock was showing (the header needs a repaint). */
bool ui_header_note_input(void);

/* The Settings > Date & Time > Time in Title switch, backed by the
 * ipodui_time_in_title setting (P11, default Off, owner T1). The apply
 * hook: shell start calls it with the stored value before the first
 * header draw (ui_main.c), and Date & Time and Reset call it after they
 * change the setting. Until the first call the clock is off. */
void ui_header_set_time_in_title(bool on);
bool ui_header_time_in_title(void);

/* The clock text as the header shows it ("15:04", or "3:04 PM" with the
 * 12-hour setting), formatted from the RTC; '*ticks_to_minute' (if not
 * NULL) gets the ticks until it next changes. */
const char *ui_header_clock(int *ticks_to_minute);

/* The dark header of the 7G's full-screen system screens (USB; About,
 * Brightness, Screen Lock later): rows 0..h-1 filled from 'rows' (one
 * colour per row), 'title' centred in white Bold 12 (NULL: none), and
 * the light battery at the right with a bolt while charging, or a plug
 * when on power but not charging. Draws straight into the framebuffer
 * (no lcd_update); the next ui_header_draw() repaints in full. */
void ui_header_draw_dark(const unsigned short *rows, int h,
                         const char *title, int battery_pct, bool charging,
                         bool powered);

/* Battery colour thresholds, percent (owner 2026-10-22): green above
 * UI_BATT_AMBER_PCT, amber at or below it, red at or below
 * UI_BATT_RED_PCT. 20 is also artcache's LOW_BATTERY. */
#define UI_BATT_AMBER_PCT   35
#define UI_BATT_RED_PCT     20

/* 0 green, 1 amber, 2 red; green while charging */
static inline int ui_batt_band(int pct, bool charging)
{
    if (charging || pct > UI_BATT_AMBER_PCT)
        return 0;
    return pct > UI_BATT_RED_PCT ? 1 : 2;
}

#endif /* IPODUI_HEADER_H */
