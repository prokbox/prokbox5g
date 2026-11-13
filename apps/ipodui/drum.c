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

/* The drum picker (drum.h). Colours and corner radii are roller.c's, so
 * the strip and the box are the same objects as on the Date & Time
 * screen. */
#include "config.h"
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/usbscreen.h"
#include "ipodui/drum.h"

#define DRUM_BOX_H      33
#define DRUM_BOX_W      (UI_DRUM_W - 20)
#define DRUM_PAD        4       /* repainted body above and below the strip */

#define STRIP_TOP       LCD_RGBPACK(0x4e, 0x52, 0x5e)
#define STRIP_BASE      LCD_RGBPACK(0x12, 0x13, 0x18)
#define STRIP_FOOT      LCD_RGBPACK(0x24, 0x26, 0x2e)
#define STRIP_EDGE      LCD_RGBPACK(0x4a, 0x4d, 0x56)
#define DRUM_FG         LCD_RGBPACK(0xf2, 0xf4, 0xff)

static const struct ui_gloss box_gloss = {
    .top    = LCD_RGBPACK(0x58, 0xa4, 0xf6),
    .bot    = LCD_RGBPACK(0x0a, 0x58, 0xd2),
    .line   = LCD_RGBPACK(0x1c, 0x4c, 0xa8),
    .edge   = LCD_RGBPACK(0x08, 0x38, 0x90),
    .hi_top = 90,
    .hi_mid = 20,
    .split  = 128,
    .flags  = UI_GLOSS_LINE | UI_GLOSS_EDGE,
};

static int corner_inset(int r, int h)
{
    static const unsigned char in[4] = { 4, 2, 1, 1 };
    int d = r < h - 1 - r ? r : h - 1 - r;

    return d < 4 ? in[d] : 0;
}

/* the strip's colour at row r (of UI_DRUM_H): smoked glass, lit at the top
 * and bottom edges, near black in the middle (roller.c's strip_row() for
 * a taller strip) */
static unsigned strip_row(int r)
{
    int d = r < UI_DRUM_H - 1 - r ? r : UI_DRUM_H - 1 - r;
    int a = d == 0 ? 5 : d == 1 ? 3 : d == 2 ? 1 : 0;
    int m = UI_DRUM_H / 2;
    unsigned c = r < m ? ui_paint_blend(STRIP_TOP, STRIP_BASE, r * 15 / m)
                       : ui_paint_blend(STRIP_BASE, STRIP_FOOT,
                                        (r - m) * 15 / (UI_DRUM_H - m));
    return a ? ui_paint_blend(c, STRIP_EDGE, a) : c;
}

void ui_drum_band(const struct ui_drum *d, int *y, int *h)
{
    *y = d->y - DRUM_PAD;
    *h = UI_DRUM_H + 2 * DRUM_PAD;
}

void ui_drum_init(struct ui_drum *d, int y, int n, int sel,
                  const char *(*label)(int, void *), void *data)
{
    d->n = n;
    d->sel = MIN(MAX(sel, 0), n - 1);
    d->y = y;
    d->label = label;
    d->data = data;
    ui_scroll_init(&d->sc, (n - 1) * UI_DRUM_PITCH);
    ui_scroll_set_target(&d->sc, d->sel * UI_DRUM_PITCH);
    ui_scroll_snap(&d->sc);
    d->last_frame = current_tick;
}

bool ui_drum_input(struct ui_drum *d, const struct ui_input *in)
{
    int sel = d->sel;

    if (in->key != UI_KEY_WHEEL || !in->wheel_steps)
        return false;
    sel += in->wheel_steps > 0 ? 1 : -1;        /* one value per detent */
    sel = MIN(MAX(sel, 0), d->n - 1);            /* the ends stop it */
    if (sel == d->sel)
        return false;
    d->sel = sel;
    ui_scroll_set_target(&d->sc, sel * UI_DRUM_PITCH);
    d->last_frame = current_tick;
    return true;
}

void ui_drum_draw(const struct ui_drum *d)
{
    const struct ui_theme *t = ui_theme;
    int sx = (LCD_WIDTH - UI_DRUM_W) / 2, row, i, by, bx, y0, bh;
    struct viewport vp;

    ui_drum_band(d, &y0, &bh);
    ui_dark_body(y0, bh);                       /* the body behind it */
    lcd_set_viewport(NULL);
    for (row = 0; row < UI_DRUM_H; row++)
    {
        int in = corner_inset(row, UI_DRUM_H);

        lcd_set_foreground(strip_row(row));
        lcd_hline(sx + in, sx + UI_DRUM_W - 1 - in, d->y + row);
    }
    /* the blue box, fixed in the middle */
    bx = sx + (UI_DRUM_W - DRUM_BOX_W) / 2;
    by = d->y + (UI_DRUM_H - DRUM_BOX_H) / 2;
    for (row = 0; row < DRUM_BOX_H; row++)
    {
        int in = corner_inset(row, DRUM_BOX_H);

        lcd_set_foreground(ui_gloss_color(&box_gloss, row, DRUM_BOX_H));
        lcd_hline(bx + in, bx + DRUM_BOX_W - 1 - in, by + row);
    }
    /* the values, clipped to the strip: the viewport is the strip */
    memset(&vp, 0, sizeof(vp));
    vp.x = sx;
    vp.y = d->y;
    vp.width = UI_DRUM_W;
    vp.height = UI_DRUM_H;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.drawmode = DRMODE_FG;
    vp.font = t->font_ui;
    lcd_set_viewport(&vp);
    for (i = 0; i < d->n; i++)
    {
        /* the value's centre row inside the strip */
        int off = i * UI_DRUM_PITCH - d->sc.pos;
        int cy = UI_DRUM_H / 2 + off, dist = off < 0 ? -off : off;
        int a;

        if (cy < -8 || cy > UI_DRUM_H + 8)
            continue;
        /* white at the middle, dim (a third) a pitch away */
        a = dist >= UI_DRUM_PITCH ? 5 : 15 - dist * 10 / UI_DRUM_PITCH;
        lcd_set_foreground(ui_paint_blend(
            strip_row(MIN(MAX(cy, 0), UI_DRUM_H - 1)), DRUM_FG, a));
        ui_paint_text_centre(UI_DRUM_W / 2, cy + 6, UI_DRUM_W - 16,
                             d->label(i, d->data));
    }
    lcd_set_viewport(NULL);
}

int ui_drum_tick(struct ui_drum *d)
{
    long now = current_tick;
    int y, h, dt;

    if (d->sc.pos == d->sc.target)
        return TIMEOUT_BLOCK;
    if (!ui_anim_frame_due())
        return UI_TICK_ANIM;
    ui_anim_frame();                    /* boost on the first frame */
    dt = now - d->last_frame;
    d->last_frame = now;
    ui_scroll_step(&d->sc, dt > 0 ? dt : 1);
    ui_drum_draw(d);
    ui_drum_band(d, &y, &h);
    ui_lcd_push("drum", (LCD_WIDTH - UI_DRUM_W) / 2 - DRUM_PAD, y,
                UI_DRUM_W + 2 * DRUM_PAD, h);
    return d->sc.pos != d->sc.target ? UI_TICK_ANIM : TIMEOUT_BLOCK;
}
