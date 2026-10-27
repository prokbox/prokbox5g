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
 * iPod UI shell: quick-scroll letter overlay
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
#ifndef _IPODUI_QSCROLL_H_
#define _IPODUI_QSCROLL_H_

#include <stdbool.h>
#include "ipodui/list.h"

/* The 7G quick-scroll letter: while the wheel spins fast through a long
 * sorted list, a large rounded translucent glossy grey square floats in
 * the middle of the list with the selected row's sort letter in big white
 * type. It hides QS_HOLD (~0.5 s) after the last fast wheel event.
 *
 * It is drawn by the list's overlay hook (list.h), so it rides on the
 * glide frames and static band pushes that happen anyway: no frames of
 * its own. Only showing, a letter change and hiding while the list is at
 * rest invalidate its band. The state is small and lives in the screen's
 * core_alloc block. */
struct ui_qscroll {
    long hide;                  /* tick to hide at */
    char letter;                /* 'A'..'Z' or '#'; 0 = none */
    bool shown;
};

/* Loads the letter font (7,990 B, a movable buflib allocation) once, at
 * shell start: never while browsing, when a new allocation could make
 * the audio buffer shrink (a gap). Without it the letter falls back to
 * the list font. */
void ui_qscroll_load(void);

void ui_qscroll_init(struct ui_qscroll *q);
/* After a wheel move of 'l': shows the letter on a fast spin through a
 * long list, or updates it while shown. 'letter' is the selected row's
 * sort letter (0 for a row without one, e.g. "All Songs"). */
void ui_qscroll_wheel(struct ui_qscroll *q, const struct ui_list *l,
                      unsigned velocity, char letter);
/* The overlay hook body: draws the square and letter clipped to [y, y+h)
 * (LCD coordinates) if shown. */
void ui_qscroll_draw(const struct ui_qscroll *q, const struct ui_list *l,
                     int y, int h);
/* Hides it when due. Returns ticks until it wants to run again, or
 * TIMEOUT_BLOCK. */
int  ui_qscroll_tick(struct ui_qscroll *q, const struct ui_list *l);
/* Forget it without a redraw (a full redraw of the list follows). */
void ui_qscroll_reset(struct ui_qscroll *q);

#endif /* _IPODUI_QSCROLL_H_ */
