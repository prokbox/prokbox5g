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

#ifndef IPODUI_DRUM_H
#define IPODUI_DRUM_H

#include <stdbool.h>
#include "ipodui/input.h"
#include "ipodui/scroll.h"

/* The drum: a vertical picker for one choice out of a few text values,
 * the Date & Time roller's look (smoked strip, blue gloss box) but turned
 * on its side and centred on the screen. The selected value sits in the
 * box; the values before and after it peek, dimmed, above and below,
 * clipped by the strip's edges. The wheel turns it one value per detent;
 * it stops at the ends (no wrap). Turning slides the values through the
 * box: ui_drum_tick() draws the frames (partial pushes of the drum's own
 * band, ~30 fps, ease-out from scroll.c, boost through ui_anim_frame()).
 *
 * First user: the first-start battery question (screens/battq.c);
 * written so the time picker can move to it. Draws into the framebuffer
 * at LCD coordinates; the caller paints the dark body behind it (the
 * drum repaints its own band itself). */

#define UI_DRUM_W       150     /* the strip */
#define UI_DRUM_H       72
#define UI_DRUM_PITCH   34      /* px between values */

struct ui_drum {
    int n, sel;                 /* values, the chosen one */
    int y;                      /* top of the strip, LCD rows */
    const char *(*label)(int idx, void *data);
    void *data;
    struct ui_scroll sc;        /* px: sel * pitch at rest */
    long last_frame;
};

void ui_drum_init(struct ui_drum *d, int y, int n, int sel,
                  const char *(*label)(int, void *), void *data);
/* The wheel / NEXT / PREV: moves the choice by 'steps' (clamped), starts
 * the slide. Returns true if it moved (the caller returns UI_HANDLED
 * either way and lets ui_drum_tick() draw). */
bool ui_drum_input(struct ui_drum *d, const struct ui_input *in);
/* Draws the drum at its current position (static draw, no push). */
void ui_drum_draw(const struct ui_drum *d);
/* One slide frame when due: draws and pushes. Returns UI_TICK_ANIM while
 * moving, else TIMEOUT_BLOCK. */
int ui_drum_tick(struct ui_drum *d);
/* the band the drum paints, LCD coordinates */
void ui_drum_band(const struct ui_drum *d, int *y, int *h);

#endif /* IPODUI_DRUM_H */
