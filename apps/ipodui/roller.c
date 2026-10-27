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
 * iPod UI shell: the roller widget (Phase 11)
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

/* N numeric fields in a dark rounded strip, the current one in a blue
 * rounded box (owner photo extras-screenlock-new-combination.jpg:
 * strip ~164 x 43 px at x 131, digits 24 px tall on a 39 px pitch, the
 * box 24 x 33 px; #52 re-measure). The label goes BELOW the strip, as in the photo (the
 * contract header says "above"; the photo wins). The digits are our own
 * drawing (art/check/check.py, checkglyph.h): coverage tiles blended
 * over the known colour under them, no font. */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "button.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/header.h"
#include "ipodui/roller.h"
#include "ipodui/checkglyph.h"

#define ROLLER_H        43      /* the strip */
#define ROLLER_BAND_PAD 2
#define ROLLER_CELL_BASE 21     /* a cell: ROLLER_CELL_BASE + digits * RD_W */
#define ROLLER_MARGIN   3       /* strip ink beyond the first/last cell */
#define ROLLER_RIGHT_X  131     /* the strip's left edge beside the lock */
#define ROLLER_RIGHT_W  162
#define ROLLER_BOX_H    33
#define ROLLER_LABEL_DY 118     /* label baseline below the strip's top */

#define STRIP_TOP       LCD_RGBPACK(0x4e, 0x52, 0x5e)
#define STRIP_BASE      LCD_RGBPACK(0x12, 0x13, 0x18)
#define STRIP_FOOT      LCD_RGBPACK(0x24, 0x26, 0x2e)
#define STRIP_EDGE      LCD_RGBPACK(0x4a, 0x4d, 0x56)
#define DIGIT_FG        LCD_RGBPACK(0xf2, 0xf4, 0xff)

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

/* the band of the last draw (the input step reports it) */
static int last_y = 100;

/* -- the strip -------------------------------------------------------- */

static int cell_w(const struct ui_roller_field *f)
{
    return ROLLER_CELL_BASE + f->digits * RD_W;
}

/* inset of a rounded rectangle's row r (of h) for radius 4 */
static int corner_inset(int r, int h)
{
    static const unsigned char in[4] = { 4, 2, 1, 1 };
    int d = r < h - 1 - r ? r : h - 1 - r;

    return d < 4 ? in[d] : 0;
}

/* the strip's colour at row r (of ROLLER_H): smoked glass (owner #52,
 * "lighter strip"; photo extras-screenlock-new-combination): a lit upper
 * band falling to near black at the middle, a faint lift toward the
 * foot, a soft lighter rim */
static unsigned strip_row(int r)
{
    int d = r < ROLLER_H - 1 - r ? r : ROLLER_H - 1 - r;
    int a = d == 0 ? 5 : d == 1 ? 3 : d == 2 ? 1 : 0;
    const int m = ROLLER_H * 9 / 20;
    unsigned c;

    if (r < m)
        c = ui_paint_blend(STRIP_TOP, STRIP_BASE, r * 15 / m);
    else
        c = ui_paint_blend(STRIP_BASE, STRIP_FOOT,
                           (r - m) * 15 / (ROLLER_H - m));
    return a ? ui_paint_blend(c, STRIP_EDGE, a) : c;
}

/* The digit's 4-bit coverage blended over per-row backgrounds: rowbg(r)
 * is the colour under pixel row r of the tile. */
static void draw_digit(int d, int x, int y, bool boxed, int box_y, int box_h)
{
    const unsigned char *cov = rdigit_cov[d];
    int r, c;

    for (r = 0; r < RD_H; r++, cov += RD_ROW)
    {
        unsigned bg = boxed ? ui_gloss_color(&box_gloss, y + r - box_y, box_h)
                            : strip_row(y + r - (last_y));
        for (c = 0; c < RD_W; c++)
        {
            int a = (c & 1) ? cov[c >> 1] & 15 : cov[c >> 1] >> 4;

            if (!a)
                continue;
            lcd_set_foreground(a >= 15 ? DIGIT_FG
                                       : ui_paint_blend(bg, DIGIT_FG, a));
            lcd_drawpixel(x + c, y + r);
        }
    }
}

void ui_roller_draw(const struct ui_roller *r, int y, int label_lang)
{
    const struct ui_theme *t = ui_theme;
    int total = 0, i, x0, sx, sw, row, cx;
    int t_font = t->font_ui;

    last_y = y;
    for (i = 0; i < r->n; i++)
        total += cell_w(&r->f[i]);
    sw = total + 2 * ROLLER_MARGIN;
    if (sw <= ROLLER_RIGHT_W)
        sx = ROLLER_RIGHT_X + ROLLER_RIGHT_W - sw;  /* beside the lock */
    else
        sx = (LCD_WIDTH - sw) / 2;
    x0 = sx + ROLLER_MARGIN;

    lcd_set_viewport(NULL);
    for (row = 0; row < ROLLER_H; row++)
    {
        int in = corner_inset(row, ROLLER_H);

        lcd_set_foreground(strip_row(row));
        lcd_hline(sx + in, sx + sw - 1 - in, y + row);
    }
    cx = x0;
    for (i = 0; i < r->n; i++)
    {
        const struct ui_roller_field *f = &r->f[i];
        int w = cell_w(f), n = f->digits, k, v = f->value;
        int dx = cx + (w - n * RD_W) / 2;
        int dy = y + (ROLLER_H - RD_H) / 2;
        bool cur = i == r->cur;
        int bw = n * RD_W + 6, bx = cx + (w - bw) / 2;
        int by = y + (ROLLER_H - ROLLER_BOX_H) / 2;
        char digs[8];

        if (n > 6)
            n = 6;
        if (cur)
        {
            /* the blue box, rounded like the strip but a radius smaller */
            for (row = 0; row < ROLLER_BOX_H; row++)
            {
                int in = corner_inset(row, ROLLER_BOX_H);

                lcd_set_foreground(ui_gloss_color(&box_gloss, row,
                                                  ROLLER_BOX_H));
                lcd_hline(bx + in, bx + bw - 1 - in, by + row);
            }
        }
        if (f->text)
        {
            const char *t = r->txt[(f->value - f->min) & 1];
            struct viewport vp;

            memset(&vp, 0, sizeof(vp));
            vp.width = LCD_WIDTH;
            vp.height = LCD_HEIGHT;
            vp.flags = VP_DEFAULT_FLAGS;
            vp.drawmode = DRMODE_FG;
            vp.font = t_font;
            lcd_set_viewport(&vp);
            lcd_set_foreground(DIGIT_FG);
            ui_paint_text_centre(cx + w / 2, y + ROLLER_H / 2 + 6, w, t);
            lcd_set_viewport(NULL);
            cx += w;
            continue;
        }
        for (k = n - 1; k >= 0; k--)
        {
            digs[k] = '0' + (v < 0 ? 0 : v) % 10;
            v /= 10;
        }
        for (k = 0; k < n; k++)
            draw_digit(digs[k] - '0', dx + k * RD_W, dy, cur, by,
                       ROLLER_BOX_H);
        cx += w;
    }
    if (label_lang >= 0)
    {
        struct viewport vp;

        memset(&vp, 0, sizeof(vp));
        vp.width = LCD_WIDTH;
        vp.height = LCD_HEIGHT;
        vp.flags = VP_DEFAULT_FLAGS;
        vp.drawmode = DRMODE_FG;
        vp.font = t->font_ui;
        lcd_set_viewport(&vp);
        lcd_set_foreground(LCD_WHITE);
        ui_paint_text_centre(LCD_WIDTH / 2, y + ROLLER_LABEL_DY, LCD_WIDTH - 20,
                             str(label_lang));
        lcd_set_viewport(NULL);
    }
}

/* -- input -------------------------------------------------------------- */

static void wrap_field(struct ui_roller_field *f, int steps)
{
    int span = f->max - f->min + 1, v;

    if (span <= 0)
        return;
    v = (f->value - f->min + steps) % span;
    if (v < 0)
        v += span;
    f->value = f->min + v;
}

enum ui_roller_res ui_roller_input(struct ui_roller *r,
                                   const struct ui_input *in,
                                   int *band_y, int *band_h)
{
    int steps = 0, cur = r->cur;

    *band_y = last_y - ROLLER_BAND_PAD;
    *band_h = ROLLER_H + 2 * ROLLER_BAND_PAD;
    switch (in->key)
    {
        case UI_KEY_WHEEL:
            steps = in->wheel_steps;
            if (!steps)
                return UI_ROLLER_BUSY;
            wrap_field(&r->f[cur], steps);
            return UI_ROLLER_BUSY;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_ROLLER_BUSY;
            if (cur + 1 >= r->n)
                return UI_ROLLER_DONE;
            r->cur = cur + 1;
            return UI_ROLLER_BUSY;
        case UI_KEY_NEXT:
        case UI_KEY_PREV:
            if (!in->release || in->long_press)
                return UI_ROLLER_BUSY;
            cur += in->key == UI_KEY_NEXT ? 1 : -1;
            if (cur >= 0 && cur < r->n)
                r->cur = cur;
            return UI_ROLLER_BUSY;
        case UI_KEY_MENU:
            if (!in->release || in->long_press)
                return UI_ROLLER_BUSY;
            return UI_ROLLER_CANCEL;
        default:
            return UI_ROLLER_BUSY;
    }
}
