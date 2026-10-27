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
 * iPod UI shell: the light prompt and its segmented control
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

/* A light full-screen prompt, not a list (photo audiobooks-speed.jpg):
 * the heading and the body at the left, and near the bottom a light-gray
 * rounded shelf holding a dark segmented control (Slower | Normal |
 * Faster; Reset | Cancel), the chosen segment blue. Moved verbatim from
 * screens/settings.c (P11 contract commit, docs/phase11-brief.md section
 * 4.8); setwidgets.h has the API. */
#include "config.h"
#include <string.h>
#include "system.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/screens/setwidgets.h"

#define SP_TEXT_X       18      /* heading and body left edge */
#define SP_TEXT_W       (LCD_WIDTH - 2 * SP_TEXT_X)
#define SP_HEAD_BASE    62      /* heading baseline */
#define SP_BODY_BASE    84      /* body: first baseline */
#define SP_BODY_PITCH   15
#define SP_SHELF_X      16
#define SP_SHELF_Y      UI_SEG_SHELF_Y
#define SP_SHELF_W      (LCD_WIDTH - 2 * SP_SHELF_X)
#define SP_SHELF_H      UI_SEG_SHELF_H
#define SP_SEG_W        UI_SEG_W
#define SP_SEG_H        UI_SEG_H
#define SP_SEG_Y        UI_SEG_Y
#define SP_HEAD_FG      LCD_RGBPACK(0x00, 0x00, 0x00)
#define SP_BODY_FG      LCD_RGBPACK(0x3c, 0x3c, 0x3c)
#define SP_SEG_DIV      LCD_RGBPACK(0x0c, 0x0c, 0x0c)

static const struct ui_gloss sp_shelf_gloss = {
    .top    = LCD_RGBPACK(0xe6, 0xe6, 0xea),
    .bot    = LCD_RGBPACK(0xc6, 0xc6, 0xcc),
    .line   = LCD_RGBPACK(0xa8, 0xa8, 0xb0),
    .edge   = LCD_RGBPACK(0xf4, 0xf4, 0xf6),
    .hi_top = 0,
    .hi_mid = 0,
    .split  = 0,
    .flags  = UI_GLOSS_LINE | UI_GLOSS_EDGE,
};
static const struct ui_gloss sp_seg_gloss = {
    .top    = LCD_RGBPACK(0x4c, 0x4c, 0x4e),
    .bot    = LCD_RGBPACK(0x1c, 0x1c, 0x1e),
    .edge   = LCD_RGBPACK(0x0c, 0x0c, 0x0c),
    .hi_top = 60,
    .hi_mid = 20,
    .split  = 128,
    .flags  = UI_GLOSS_EDGE,
};

/* Rounds a w x h box's corners by repainting the pixels outside a
 * radius-r arc with the colour beneath (bg(row) for that row). */
static void sp_corners(int x, int y, int w, int h, const signed char *cut,
                       int r, unsigned (*bg)(int row))
{
    int i;

    for (i = 0; i < r; i++)
    {
        int c = cut[i];
        if (c <= 0)
            continue;
        lcd_set_foreground(bg(y + i));
        lcd_hline(x, x + c - 1, y + i);
        lcd_hline(x + w - c, x + w - 1, y + i);
        lcd_set_foreground(bg(y + h - 1 - i));
        lcd_hline(x, x + c - 1, y + h - 1 - i);
        lcd_hline(x + w - c, x + w - 1, y + h - 1 - i);
    }
}

static unsigned sp_bg_list(int row)
{
    (void)row;
    return ui_theme->list_bg;
}

static unsigned sp_bg_shelf(int row)
{
    return ui_gloss_color(&sp_shelf_gloss, row - SP_SHELF_Y, SP_SHELF_H);
}

int ui_wrap_text(int x, int y_base, int pitch, int max_w, int max_lines,
                 const char *s)
{
    int n = 0, asc = font_get(lcd_current_viewport->font)->ascent;
    char buf[96];

    while (*s && n < max_lines)
    {
        int end = 0, j = 0, w = 0;
        while (*s == ' ')
            s++;
        while (s[j] && j < (int)sizeof(buf) - 1)
        {
            while (s[j] == ' ')
                j++;
            while (s[j] && s[j] != ' ' && j < (int)sizeof(buf) - 1)
                j++;
            memcpy(buf, s, j);
            buf[j] = '\0';
            lcd_getstringsize((const unsigned char *)buf, &w, NULL);
            if (end > 0 && w > max_w)
                break;
            end = j;
        }
        if (end == 0)
            break;
        memcpy(buf, s, end);
        buf[end] = '\0';
        lcd_putsxy(x, y_base + n * pitch - asc, (const unsigned char *)buf);
        s += end;
        n++;
    }
    return n;
}

void ui_seg_draw(const short *lang, int n, int sel, bool shelf)
{
    static const signed char cut6[] = { 3, 2, 1, 1, 0, 0 };
    static const signed char cut3[] = { 2, 1, 0 };
    const struct ui_theme *t = ui_theme;
    const int seg_x = (LCD_WIDTH - n * SP_SEG_W) / 2;
    int i, w, h;

    lcd_set_viewport(NULL);
    if (shelf)
    {
        ui_paint_gloss(SP_SHELF_X, SP_SHELF_Y, SP_SHELF_W, SP_SHELF_H,
                       &sp_shelf_gloss);
        /* the rim's sides */
        lcd_set_foreground(sp_shelf_gloss.line);
        lcd_vline(SP_SHELF_X, SP_SHELF_Y, SP_SHELF_Y + SP_SHELF_H - 1);
        lcd_vline(SP_SHELF_X + SP_SHELF_W - 1, SP_SHELF_Y,
                  SP_SHELF_Y + SP_SHELF_H - 1);
        sp_corners(SP_SHELF_X, SP_SHELF_Y, SP_SHELF_W, SP_SHELF_H, cut6, 6,
                   sp_bg_list);
    }
    lcd_setfont(t->font_title);
    for (i = 0; i < n; i++)
    {
        int x = seg_x + i * SP_SEG_W;
        const char *lbl = str(lang[i]);

        if (i == sel)           /* the selection blue: the kit's sky gel */
            ui_paint_selection(x, SP_SEG_Y, SP_SEG_W, SP_SEG_H);
        else
            ui_paint_gloss(x, SP_SEG_Y, SP_SEG_W, SP_SEG_H, &sp_seg_gloss);
        if (i > 0)
        {
            lcd_set_foreground(SP_SEG_DIV);
            lcd_vline(x, SP_SEG_Y, SP_SEG_Y + SP_SEG_H - 1);
        }
        lcd_getstringsize((const unsigned char *)lbl, &w, &h);
        lcd_set_drawmode(DRMODE_FG);
        lcd_set_foreground(LCD_WHITE);
        lcd_putsxy(x + (SP_SEG_W - w) / 2, SP_SEG_Y + (SP_SEG_H - h) / 2,
                   (const unsigned char *)lbl);
        lcd_set_drawmode(DRMODE_SOLID);
    }
    sp_corners(seg_x, SP_SEG_Y, n * SP_SEG_W, SP_SEG_H,
               cut3, 3, sp_bg_shelf);
    lcd_setfont(FONT_UI);
}

void ui_prompt_draw(int head_lang, int body_lang)
{
    const struct ui_theme *t = ui_theme;
    int asc;

    lcd_set_viewport(NULL);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_foreground(t->list_bg);
    lcd_fillrect(0, t->header_h, LCD_WIDTH, LCD_HEIGHT - t->header_h);
    lcd_set_drawmode(DRMODE_FG);
    lcd_setfont(t->font_ui);
    asc = font_get(t->font_ui)->ascent;
    lcd_set_foreground(SP_HEAD_FG);
    lcd_putsxy(SP_TEXT_X, SP_HEAD_BASE - asc,
               (const unsigned char *)str(head_lang));
    lcd_setfont(t->font_small);
    lcd_set_foreground(SP_BODY_FG);
    ui_wrap_text(SP_TEXT_X, SP_BODY_BASE, SP_BODY_PITCH, SP_TEXT_W, 3,
                 str(body_lang));
    lcd_set_drawmode(DRMODE_SOLID);
}
