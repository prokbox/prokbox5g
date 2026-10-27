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
 * iPod UI shell: shared painting primitives
 *
 * Glossy surfaces (the selection bar, the scrollbar thumb), the header
 * background and the chevron. All artwork is data in rodata (small gloss
 * descriptors, the header row table and an 8 x 13 coverage map), drawn
 * with lcd_hline / lcd_vline / lcd_drawpixel; nothing is allocated and
 * nothing is decoded at draw time. Because only drawing primitives are
 * used, the same code paints into the LCD or into an offscreen strip set
 * up with viewport_set_buffer().
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
#include "config.h"
#include <string.h>
#include "system.h"
#include "lcd.h"
#include "font.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/material.h"

/* Original chevron artwork: coverage 0 (none) .. 15 (full ink), generated
 * by apps/ipodui/art/chevron.py (two 3 px strokes with round outer ends
 * and a mitred point, 16x16 supersampled). Proportions follow fidelity
 * audit 1 fix #8: ink 9 x 13 px (x 145-153 on a 160 px list), a 90 degree
 * point at mid height; rows 0 and 14 are empty so it centres in a 24 px
 * row at +5..+17. */
#define CHEV_W 9
#define CHEV_H 15
static const unsigned char chevron_cov[CHEV_H][CHEV_W] = {
    { 0,  0,  0,  0,  0,  0,  0,  0,  0},
    { 8, 15,  8,  0,  0,  0,  0,  0,  0},
    {15, 15, 15,  9,  0,  0,  0,  0,  0},
    { 8, 15, 15, 15,  9,  0,  0,  0,  0},
    { 0,  9, 15, 15, 15,  9,  0,  0,  0},
    { 0,  0,  9, 15, 15, 15,  9,  0,  0},
    { 0,  0,  0,  9, 15, 15, 15,  9,  0},
    { 0,  0,  0,  0, 13, 15, 15, 15,  5},
    { 0,  0,  0,  9, 15, 15, 15,  9,  0},
    { 0,  0,  9, 15, 15, 15,  9,  0,  0},
    { 0,  9, 15, 15, 15,  9,  0,  0,  0},
    { 8, 15, 15, 15,  9,  0,  0,  0,  0},
    {15, 15, 15,  9,  0,  0,  0,  0,  0},
    { 8, 15,  8,  0,  0,  0,  0,  0,  0},
    { 0,  0,  0,  0,  0,  0,  0,  0,  0},
};
#define CHEV_GAP 4      /* text to chevron */

unsigned ui_paint_blend(unsigned bg, unsigned fg, int a)
{
    int r = RGB_UNPACK_RED(bg), g = RGB_UNPACK_GREEN(bg),
        b = RGB_UNPACK_BLUE(bg);
    /* int casts: the unpack macros are unsigned for an unsigned colour,
     * and a darker 'fg' must give a negative difference */
    r += ((int)RGB_UNPACK_RED(fg) - r) * a / 15;
    g += ((int)RGB_UNPACK_GREEN(fg) - g) * a / 15;
    b += ((int)RGB_UNPACK_BLUE(fg) - b) * a / 15;
    return LCD_RGBPACK(r, g, b);
}

/* a = 0..256 is the weight of 'fg' */
static unsigned blend256(unsigned bg, unsigned fg, int a)
{
    int r = RGB_UNPACK_RED(bg), g = RGB_UNPACK_GREEN(bg),
        b = RGB_UNPACK_BLUE(bg);
    r += ((int)RGB_UNPACK_RED(fg) - r) * a >> 8;
    g += ((int)RGB_UNPACK_GREEN(fg) - g) * a >> 8;
    b += ((int)RGB_UNPACK_BLUE(fg) - b) * a >> 8;
    return LCD_RGBPACK(r, g, b);
}

unsigned ui_gloss_color(const struct ui_gloss *g, int i, int n)
{
    int s, w;
    unsigned c;

    if (n <= 1)
        return g->top;
    if (i < 0)
        i = 0;
    if (i >= n)
        i = n - 1;
    if ((g->flags & UI_GLOSS_LINE) && i == 0)
        return g->line;
    if ((g->flags & UI_GLOSS_EDGE) && i == n - 1 && n > 2)
        return g->edge;
    c = blend256(g->top, g->bot, i * 256 / (n - 1));
    s = (n * g->split + 128) >> 8;              /* gloss rows */
    if (i < s)                                  /* hi_top -> hi_mid */
        w = g->hi_top + ((int)g->hi_mid - g->hi_top) * i / MAX(s - 1, 1);
    else if (i == s)                            /* soft step */
        w = g->hi_mid / 3;
    else
        return c;
    return blend256(c, LCD_RGBPACK(0xff, 0xff, 0xff), w);
}

void ui_paint_gloss(int x, int y, int w, int h, const struct ui_gloss *g)
{
    int i;

    if (w <= 0 || h <= 0)
        return;
    if (g->flags & UI_GLOSS_HORIZ)
    {
        for (i = 0; i < w; i++)
        {
            lcd_set_foreground(ui_gloss_color(g, i, w));
            lcd_vline(x + i, y, y + h - 1);
        }
        return;
    }
    for (i = 0; i < h; i++)
    {
        lcd_set_foreground(ui_gloss_color(g, i, h));
        lcd_hline(x, x + w - 1, y + i);
    }
}

/* The selection bar is the sky gel from the material kit (owner default
 * O1): a generated row table for the list heights, else the same colours
 * evaluated from its keys. */
unsigned ui_paint_selection_row(int r, int h)
{
    const uint16_t *t = ui_mat_rows(UI_MAT_GEL_SKY, h);

    if (h <= 0)
        return ui_mat_color(UI_MAT_GEL_SKY, 0, 1);
    if (r < 0)
        r = 0;
    if (r >= h)
        r = h - 1;
    return t ? t[r] : ui_mat_color(UI_MAT_GEL_SKY, r, h);
}

void ui_paint_selection(int x, int y, int w, int h)
{
    const uint16_t *t = ui_mat_rows(UI_MAT_GEL_SKY, h);
    int i;

    if (w <= 0 || h <= 0)
        return;
    for (i = 0; i < h; i++)
    {
        lcd_set_foreground(t ? t[i] : ui_mat_color(UI_MAT_GEL_SKY, i, h));
        lcd_hline(x, x + w - 1, y + i);
    }
}

void ui_paint_chevron(int x_right, int y, int h, bool on_selection)
{
    const struct ui_theme *t = ui_theme;
    int x0 = x_right - CHEV_W + 1;
    int y0 = y + (h - CHEV_H) / 2;
    unsigned fg = on_selection ? t->sel_fg : t->list_fg;
    int r, c;

    for (r = 0; r < CHEV_H; r++)
    {
        unsigned bg = on_selection ? ui_paint_selection_row(y0 + r - y, h)
                                   : t->list_bg;
        for (c = 0; c < CHEV_W; c++)
        {
            int a = chevron_cov[r][c];
            if (!a)
                continue;
            lcd_set_foreground(a == 15 ? fg : ui_paint_blend(bg, fg, a));
            lcd_drawpixel(x0 + c, y0 + r);
        }
    }
}

int ui_paint_chevron_w(void)
{
    return CHEV_GAP + CHEV_W;   /* chevron_pad is applied by the caller */
}

void ui_paint_header_bg(int x, int w)
{
    const unsigned short *rows = ui_theme->header_rows;
    int r;

    if (w <= 0)
        return;
    for (r = 0; r < ui_theme->header_h && r < UI_HEADER_ROWS; r++)
    {
        lcd_set_foreground(rows[r]);
        lcd_hline(x, x + w - 1, r);
    }
}

/* -- #38 icon panels ------------------------------------------------------ */

unsigned ui_paint_panel_row(int y)
{
    const struct ui_theme *t = ui_theme;
    int m = t->panel_mid_y;

    if (y <= 0)
        return t->panel_top;
    if (y < m)
        return blend256(t->panel_top, t->panel_mid, (y << 8) / m);
    if (y >= LCD_HEIGHT - 1)
        return t->panel_bot;
    return blend256(t->panel_mid, t->panel_bot,
                    ((y - m) << 8) / (LCD_HEIGHT - 1 - m));
}

void ui_paint_panel_bg(int y, int h)
{
    int w = lcd_current_viewport->width;

    for (; h > 0; h--, y++)
    {
        lcd_set_foreground(ui_paint_panel_row(y));
        lcd_hline(0, w - 1, y);
    }
}

/* Width of the first n bytes of s in the current viewport's font. */
static int str_w(const char *s, int n)
{
    int w = 0;
    font_getstringnsize((const unsigned char *)s, n, &w, NULL,
                        lcd_current_viewport->font);
    return w;
}

void ui_paint_text_centre(int cx, int y_base, int max_w, const char *s)
{
    struct font *pf = font_get(lcd_current_viewport->font);
    char buf[64];
    int n = strlen(s), w = str_w(s, n);

    if (w > max_w)
    {
        /* cut at a character boundary that leaves room for "..." */
        int dots = str_w("...", 3);
        while (n > 0 && str_w(s, n) + dots > max_w)
            n--;
        while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80)
            n--;                        /* not inside a UTF-8 sequence */
        n = MIN(n, (int)sizeof(buf) - 4);
        memcpy(buf, s, n);
        strcpy(buf + n, "...");
        s = buf;
        w = str_w(s, n + 3);
    }
    lcd_putsxy(cx - w / 2, y_base - pf->ascent, s);
}

int ui_paint_text_wrap(int cx, int y_last, int max_w, int max_lines,
                       int pitch, const char *s)
{
    const char *line[4];
    int len[4], n = 0, k;
    int lh = pitch > 0 ? pitch
                       : (int)font_get(lcd_current_viewport->font)->height;

    if (max_lines > 4)
        max_lines = 4;
    /* greedy word wrap: each line takes words while they fit */
    while (*s && n < max_lines)
    {
        int end = 0, j = 0;
        while (*s == ' ')
            s++;
        while (s[j])
        {
            while (s[j] == ' ')
                j++;
            while (s[j] && s[j] != ' ')
                j++;
            if (end > 0 && str_w(s, j) > max_w)
                break;
            end = j;
        }
        if (end == 0)
            break;
        line[n] = s;
        len[n++] = end;
        s += end;
    }
    while (*s == ' ')
        s++;
    for (k = 0; k < n; k++)
    {
        char buf[64];
        int l = MIN(len[k], (int)sizeof(buf) - 4);
        memcpy(buf, line[k], l);
        buf[l] = '\0';
        if (k == n - 1 && *s)
            strcpy(buf + l, "...");     /* more text than lines */
        ui_paint_text_centre(cx, y_last - (n - 1 - k) * lh, max_w, buf);
    }
    return n;
}
