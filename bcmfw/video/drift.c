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
 * Stage 5b's drift image (BCMFW_DRIFT, docs/research/bcm2722/offload.md):
 * the main menu's panel drift done on the BCM.  The host uploads covers
 * into the two slots and asks, one op at a time, for a window shown
 * (a pan), the fade's planes prepared, one cross-fade step, or an 8b
 * half step (the panel averaged with the next window); each
 * shown frame goes to the panel through the LCD rect path.
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
#include <stdint.h>
#include "config.h"
#include "bcmfw.h"
#include "hw.h"
#include "xfade.h"

#if BCMFW_DRIFT

#define PARAM(k)  MB(BCMFW_DRIFT_PARAM + 4 * (k))
#define FB        ((uint16_t *)BCMFW_FB)
#define PANEL     (FB + BCMFW_DRIFT_X)
#define STATE     ((uint16_t *)BCMFW_DRIFT_STATE)
#define SRC       ((uint16_t *)BCMFW_DRIFT_SRC)
#define STG       ((uint16_t *)BCMFW_DRIFT_STG)

static uint32_t prep_win;           /* PREP's window: the fade's target */
static uint32_t staged_win = ~0u;   /* the window in STG, ~0 none */
static uint32_t count;

static const uint16_t *window(uint32_t win)
{
    uint32_t x = win & 0xFFF, y = (win >> 12) & 0xFFF;
    const uint16_t *slot = (win >> 24) ? (const uint16_t *)BCMFW_DRIFT_SLOT1
                                       : (const uint16_t *)BCMFW_DRIFT_SLOT0;

    return slot + y * BCMFW_DRIFT_SIDE + x;
}

static int window_ok(uint32_t win)
{
    return (win >> 24) <= 1 &&
           (win & 0xFFF) <= BCMFW_DRIFT_SIDE - XF_W &&
           ((win >> 12) & 0xFFF) <= BCMFW_DRIFT_SIDE - XF_H;
}

/* The window into the panel's place in the framebuffer: a pan, or the
 * fade's last step (weight 256 is xfade_rect's exact copy).  A window a
 * half step staged comes from the staging buffer (the vector copy). */
static void show(uint32_t win)
{
    const uint16_t *src = window(win);
    uint16_t *dst = PANEL;
    int x, y;

    if (win == staged_win)
    {
        xfade_copy(PANEL, STG);
        return;
    }
    for (y = 0; y < XF_H; y++, src += BCMFW_DRIFT_SIDE, dst += BCMFW_LCD_WIDTH)
        for (x = 0; x < XF_W; x++)
            dst[x] = src[x];
}

static void push(void)
{
    lcd_update_rect(BCMFW_DRIFT_X, XF_W | XF_H << 16);
}

/* CMD_DRIFT: the op in the parameter block.  A malformed op is refused
 * (P_LAST says so) and nothing is drawn: the host's log shows it.
 * Returns 1 if a frame went to the panel. */
int drift_command(void)
{
    uint32_t t0 = REG(TIMER_CLO);
    uint32_t op = PARAM(BCMFW_DRIFT_P_OP), win = PARAM(BCMFW_DRIFT_P_WIN);
    uint32_t w = PARAM(BCMFW_DRIFT_P_WEIGHT);

    if ((op & 0xFFFF0000) != BCMFW_DRIFT_MAGIC || !window_ok(win))
        op = 0;
    switch (op & 0xFFFF)
    {
    case BCMFW_DRIFT_SHOW:
        show(win);
        push();
        break;
    case BCMFW_DRIFT_PREP:
        staged_win = ~0u;       /* a new cover may be in either slot */
        xfade_unpack(STATE, PANEL, BCMFW_LCD_WIDTH);
        xfade_unpack(SRC, window(win), BCMFW_DRIFT_SIDE);
        prep_win = win;
        break;
    case BCMFW_DRIFT_FADE:
        if (w >= 256)
            show(prep_win);
        else
            xfade_panel(PANEL, STATE, SRC, w ? (int)w : 1);
        push();
        break;
    case BCMFW_DRIFT_HALF:
        if (win != staged_win)
        {
            xfade_stage(STG, window(win), BCMFW_DRIFT_SIDE);
            staged_win = win;
        }
        xfade_half(PANEL, STG);
        push();
        break;
    default:
        PARAM(BCMFW_DRIFT_P_LAST) = 0xBAD00000 | (PARAM(BCMFW_DRIFT_P_OP) & 0xFFFF);
        return 0;
    }
    PARAM(BCMFW_DRIFT_P_US) = REG(TIMER_CLO) - t0;
    PARAM(BCMFW_DRIFT_P_COUNT) = ++count;
    PARAM(BCMFW_DRIFT_P_LAST) = op;
    return (op & 0xFFFF) != BCMFW_DRIFT_PREP;
}
#endif
