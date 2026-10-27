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
 * iPod UI shell: the unlit clock face, pure core (Phase 8a, U2).
 * Private to unlit.c / unlit_core.c / tests/unlit: layout and painting
 * through a fill callback, so the host test renders the exact pixels the
 * shell draws. The contract API (state machine) is in unlit.h.
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
#ifndef _IPODUI_UNLIT_CORE_H_
#define _IPODUI_UNLIT_CORE_H_

#include <stdbool.h>
#include "ipodui/unlit.h"

/* grey levels handed to the fill callback (quantised to RGB565 there) */
#define UNLIT_PAPER     255

/* fills one rect (LCD coordinates, w, h > 0) in grey 'g' */
typedef void unlit_fill_fn(void *ctx, int x, int y, int w, int h, int g);

/* the time row: up to "12:58" (5 cells) */
struct unlit_clock {
    signed char n;              /* cells, 0 = none drawn */
    char ch[5];                 /* '0'..'9', ':' */
    short x[5];                 /* left edge of each cell */
};

void unlit_state_init(struct ui_unlit_state *s, long now);

/* Which button releases reach ui_unlit_step() (8a fix M2). A release
 * with no key bits is not a key (a bare BUTTON_REL in the queue): it
 * passes untouched, never swallowed, never a dismiss, never the end of
 * the swallowing. Another key's release while the dismissing key is
 * held is swallowed; any other release is stepped as UE_KEY_RELEASE. */
enum unlit_rel { UNLIT_REL_PASS, UNLIT_REL_SWALLOW, UNLIT_REL_STEP };
enum unlit_rel unlit_release_route(const struct ui_unlit_state *s,
                                   unsigned long bits,
                                   unsigned long dismiss_bits);

/* minute of day (0..1439) -> layout, 12 h: no leading zero, no AM/PM;
 * 24 h: zero-padded (docs/design/unlit.md) */
void unlit_clock_layout(int minute, bool h12, struct unlit_clock *c);
/* Paints the time row. With 'old' (what the framebuffer holds) and the
 * same cell positions, only the changed cells are cleared and repainted;
 * otherwise the whole band (y UNLIT_DIGIT_Y, 320 x 68) is. Returns the
 * cells painted. */
int unlit_paint_clock(const struct unlit_clock *c, const struct unlit_clock *old,
                      unlit_fill_fn *fill, void *ctx);
/* Clears the status band (y 161, 320 x 37) and paints
 * [play|pause] [padlock] [battery], centred as a group */
void unlit_paint_status(const struct ui_unlit_status *st,
                        unlit_fill_fn *fill, void *ctx);
/* battery fill width for a level in percent (generator: round half to
 * even of 42 * level, at least 1 px above 0 %) */
int unlit_fill_px(int percent);

/* single glyphs, for tests/unlit's glyph sheet */
enum unlit_glyph_id { UNLIT_G_PLAY = 10, UNLIT_G_PAUSE, UNLIT_G_LOCK,
                      UNLIT_G_COLON };
void unlit_paint_glyph(int id, int x, int y, unlit_fill_fn *fill, void *ctx);
void unlit_paint_battery(int x, int y, int fill_px, bool charging,
                         unlit_fill_fn *fill, void *ctx);
int  unlit_glyph_w(int id);     /* 0..9 digits, or an unlit_glyph_id */

#endif /* _IPODUI_UNLIT_CORE_H_ */
