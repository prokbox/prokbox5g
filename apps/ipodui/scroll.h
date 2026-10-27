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
 * iPod UI shell: scroll physics and wheel acceleration
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
#ifndef _IPODUI_SCROLL_H_
#define _IPODUI_SCROLL_H_

/* Pure C: no lcd/kernel includes, so the host harness (tests/ipodui/)
 * can compile scroll.c on its own. Units: pixels and ticks (HZ = 100). */

#include <stdbool.h>

struct ui_scroll {
    int pos;        /* current top offset in px (rendered) */
    int target;     /* where we're heading */
    int vel;        /* px per tick, internal: the last step's move, signed */
    int max;        /* max(0, content_h - view_h) */
};

void ui_scroll_init(struct ui_scroll *s, int max);
void ui_scroll_set_max(struct ui_scroll *s, int max);       /* clamps */
void ui_scroll_set_target(struct ui_scroll *s, int target); /* clamps */
/* Advance by dt ticks. Returns true while still moving. Converges to the
 * target in a bounded number of steps (ease-out: each tick covers a third
 * of the remaining distance, at least 1 px, so 25 px take 7 ticks and any
 * distance d takes at most ~2.5 * log2(d) + 3 ticks), never leaves
 * [0, max] and never passes the target (7G has no bounce). */
bool ui_scroll_step(struct ui_scroll *s, int dt);
/* Wheel acceleration: raw clicks this event + wheel velocity (clicks/s,
 * from button_get_data) -> selection steps. Monotonic non-decreasing in
 * velocity; returns >= 1 for clicks >= 1 (0 for clicks <= 0).
 * A "click" is one wheel event as posted by the clickwheel driver (4 raw
 * sensor positions = 15 degrees); input.c converts the driver's deg/s.
 * Curve: 1 step per click below 12 clicks/s (a slow turn), then
 * 1 + (v - 12) / 12, capped at 5: a fast spin (~36 clicks/s, 1.5 turns/s)
 * gives 3 steps per click, ~100 items/s. */
int  ui_wheel_steps(int clicks, unsigned velocity);

/* Additions to the fixed Phase 3 contract: */
/* Jumps pos so it is at most 'lag' px from the target (keeps the selected
 * row on screen during a fast spin). Moves only toward the target. */
void ui_scroll_limit_lag(struct ui_scroll *s, int lag);
/* pos = target, vel = 0. */
void ui_scroll_snap(struct ui_scroll *s);

#endif /* _IPODUI_SCROLL_H_ */
