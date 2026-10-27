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

/* Wheel-region taps and holds: see chess_pad.h. */
#include "chess_pad.h"

int cp_region(int pos)
{
    /* quarters centred on the top, right, bottom and left: 0 at the top
     * (the driver's value 0 sits under MENU [rockbox-derived]) */
    return ((pos + CP_POSITIONS / 8) / (CP_POSITIONS / 4)) & 3;
}

void cp_reset(struct cp_pad *p)
{
    p->state = CP_S_OFF;
    p->t0 = p->next = 0;
    p->pos0 = 0;
    p->region = 0;
    p->rsv = 0;
}

int cp_feed(struct cp_pad *p, int pos, bool other, uint32_t now_ms)
{
    int d;

    if (pos < 0)
    {
        bool tap = p->state == CP_S_TOUCH && !other
                   && now_ms - p->t0 < CP_TAP_MS;

        p->state = CP_S_OFF;
        return tap ? p->region : CP_NONE;
    }
    if (p->state == CP_S_OFF)
    {
        p->state = other ? CP_S_VOID : CP_S_TOUCH;
        p->pos0 = (int8_t)pos;
        p->region = (int8_t)cp_region(pos);
        p->t0 = now_ms;
        return CP_NONE;
    }
    if (p->state == CP_S_VOID)
        return CP_NONE;
    d = pos - p->pos0;
    if (d >= CP_POSITIONS / 2)
        d -= CP_POSITIONS;
    else if (d < -CP_POSITIONS / 2)
        d += CP_POSITIONS;
    if (other || d > CP_SLOP || d < -CP_SLOP)
    {
        p->state = CP_S_VOID;
        return CP_NONE;
    }
    if (p->state == CP_S_TOUCH)
    {
        if (now_ms - p->t0 < CP_HOLD_MS)
            return CP_NONE;
        p->state = CP_S_HOLD;
        p->next = now_ms + CP_REPEAT_MS;
        return p->region;
    }
    if ((int32_t)(now_ms - p->next) < 0)
        return CP_NONE;
    p->next = now_ms + CP_REPEAT_MS;
    return p->region;
}
