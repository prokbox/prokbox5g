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

/* Wheel-region taps and holds (pure, host-tested in tests/chess): the
 * click wheel's ring as a four-way pad, from the polled absolute position
 * (sdk_wheel_pos(), 96 positions, 0 at the top, clockwise). A finger set
 * down in a quarter and lifted again within CP_TAP_MS is one step that
 * way; held still past CP_HOLD_MS it steps again every CP_REPEAT_MS.
 * A finger that slides more than CP_SLOP positions is turning the wheel,
 * and one that lands on a button as it is pressed is pressing it: neither
 * steps until it lifts. No SDK or Rockbox headers. */
#ifndef CHESS_PAD_H
#define CHESS_PAD_H

#include <stdint.h>
#include <stdbool.h>

enum { CP_UP, CP_RIGHT, CP_DOWN, CP_LEFT };    /* MENU, NEXT, PLAY, PREV */
#define CP_NONE         (-1)
#define CP_POSITIONS    96
#define CP_TAP_MS       400
#define CP_HOLD_MS      450
#define CP_REPEAT_MS    160
#define CP_SLOP         4

struct cp_pad {
    uint32_t t0;                /* when the finger landed */
    uint32_t next;              /* the next repeat */
    int8_t   pos0;              /* where it landed */
    int8_t   region;
    uint8_t  state;             /* CP_S_* */
    uint8_t  rsv;
};
enum { CP_S_OFF, CP_S_TOUCH, CP_S_HOLD, CP_S_VOID };

/* the quarter a position is in */
int  cp_region(int pos);
void cp_reset(struct cp_pad *p);
/* one poll: pos (0..95, or -1: no finger); other: a button went down or
 * the wheel turned since the last poll. Returns a direction (CP_UP ..
 * CP_LEFT) to step, or CP_NONE. */
int  cp_feed(struct cp_pad *p, int pos, bool other, uint32_t now_ms);
/* a finger is on the ring (the game keeps polling) */
static inline bool cp_active(const struct cp_pad *p)
{
    return p->state != CP_S_OFF;
}

#endif /* CHESS_PAD_H */
