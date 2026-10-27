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

/* Bare-metal ARM workloads for tests/unlit/insn.sh (Phase 8a U2): dynamic
 * instruction counts of the unlit face's per-event work in
 * apps/ipodui/unlit_core.c. FILL=1 stores every pixel of every rect into
 * a 320 x 240 RGB565 buffer with a plain 16-bit loop (an upper bound for
 * lcd_fillrect, whose rows are memset16); FILL=0 counts the core alone
 * (layout, glyph expansion, erosion, span emission). GPL v2+ */
#include <stdint.h>
#include <stddef.h>
#include "config.h"
#include "ipodui/unlit_core.h"

#ifndef WL
#define WL 0
#endif
#ifndef FILL
#define FILL 1
#endif
#define REPS 8

static uint16_t fb[LCD_WIDTH * LCD_HEIGHT];
volatile int sink;

static void fill(void *ctx, int x, int y, int w, int h, int g)
{
#if FILL
    uint16_t c = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
    int i, j;
    for (j = y; j < y + h; j++)
    {
        uint16_t *p = fb + j * LCD_WIDTH + x;
        for (i = 0; i < w; i++)
            p[i] = c;
    }
#else
    sink += x + y + w + h + g;
#endif
    (void)ctx;
}

int main(void)
{
    struct unlit_clock a, b;
    struct ui_unlit_status st = { 1, 1, 1, 19 };
    struct ui_unlit_state s;
    int k;

    unlit_clock_layout(4 * 60 + 1, true, &a);
    unlit_clock_layout(9 * 60 + 59, true, &b);
    unlit_state_init(&s, 0);
    for (k = 0; k < REPS; k++)
    {
#if WL == 1     /* minute step, one cell: 4:01 -> 4:02 */
        struct unlit_clock n;
        unlit_clock_layout(4 * 60 + 2, true, &n);
        sink += unlit_paint_clock(&n, &a, fill, NULL);
#elif WL == 2   /* minute step, the row re-centres: 9:59 -> 10:00 */
        struct unlit_clock n;
        unlit_clock_layout(10 * 60, true, &n);
        sink += unlit_paint_clock(&n, &b, fill, NULL);
#elif WL == 3   /* status row: play, padlock, charging battery */
        unlit_paint_status(&st, fill, NULL);
#elif WL == 4   /* first show: paper, "12:58", status row */
        struct unlit_clock n;
        unlit_clock_layout(12 * 60 + 58, true, &n);
        fill(NULL, 0, 0, LCD_WIDTH, LCD_HEIGHT, UNLIT_PAPER);
        sink += unlit_paint_clock(&n, NULL, fill, NULL);
        unlit_paint_status(&st, fill, NULL);
#elif WL == 5   /* one state-machine event (a key while swallowing) */
        sink += ui_unlit_step(&s, UE_KEY, k, true);
#else
        sink += k;
#endif
    }
    return 0;
}
