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
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "core_alloc.h"
#include "rbpaths.h"
#include "logf.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/screen_stack.h"
#include "ipodui/qscroll.h"

/* No clean reference of the overlay exists (photo-search-quickscroll.jpg
 * is the Search letter strip, a different control), so every value is an
 * estimate in the shell's glossy language (~). */
#define QS_MIN_ROWS 50          /* ~ lists longer than this */
#ifndef SIMULATOR
#define QS_VEL      16          /* ~ clicks/s: a brisk spin (1 step/click
                                 *   is < 12, see scroll.h) */
#else
/* the script driver's "wheel +n" reaches only ~10 clicks/s in the
 * simulator (measured), so a scripted spin counts as fast from 8 */
#define QS_VEL      8
#endif
#define QS_HOLD     (HZ/2)      /* hides ~0.5 s after the fast spin */
#define QS_SIZE     84          /* ~ square, px */
#define QS_RADIUS   12          /* ~ corner radius */
#define QS_ALPHA    29          /* ~ opacity of the square, /32 (91%) */
#define QS_CAP      35          /* ~ letter cap height on screen, px */
/* A-Z only, cap 26 px, drawn scaled to QS_CAP: 7,990 B resident instead
 * of 17,066 B for a 48 px cut, to stay in the 100 KB font budget
 * (docs/hardware.md) */
#define QS_FONT     "35-ShellSans-Letters"

/* A glossy grey slab: dark grey gradient, the top ~45% lifted toward
 * white, a light first line and a darker last line (~). */
static const struct ui_gloss qs_gloss = {
    .top    = LCD_RGBPACK(0x5c, 0x5c, 0x5c),
    .bot    = LCD_RGBPACK(0x26, 0x26, 0x26),
    .line   = LCD_RGBPACK(0xa8, 0xa8, 0xa8),
    .edge   = LCD_RGBPACK(0x14, 0x14, 0x14),
    .hi_top = 92,
    .hi_mid = 40,
    .split  = 116,
    .flags  = UI_GLOSS_LINE | UI_GLOSS_EDGE,
};

static int font_id = -1;        /* letter font, -1 = not loaded */

/* F3: what every glide frame used to recompute, tabulated. A movable
 * block reserved with the font at shell start (never later): per slab
 * row the gloss colour pre-multiplied by its weight (with the rounding
 * term) and the corner coverage, built once; then the scaled letter's
 * weights (0..32), rebuilt only when the letter changes. Size: 84 x 12 B
 * + the widest scaled letter's box (~41 x 45 B), ~2.9 KB. Without it
 * the overlay draws as before. */
struct qs_row {
    uint32_t pre;               /* c * QS_ALPHA + rounding (blend565) */
    uint32_t epre;              /* c * edge weight + rounding */
    unsigned char whole;        /* first fully covered column */
    unsigned char ea;           /* edge weight, 0 = no partial pixel */
};
struct qs_tab {
    struct qs_row row[QS_SIZE];
    char letter;                /* letter in a[], 0 = none */
    unsigned char dw, dh;       /* its scaled box */
    unsigned char max_w, max_h; /* the box a[] was sized for */
    unsigned char a[];          /* dw x dh weights, row major */
};
static int tab_handle;          /* 0 = none */

#define QS_ROUND    0x02008010u /* 16 in each packed field: /32 rounding */

static inline uint32_t pack32(unsigned c)
{
    return (c | (c << 16)) & 0x07e0f81f;
}

static int inset16(int r);
static void scaled_box(struct font *pf, int ch, int *dw, int *dh);

static void tab_init(void)
{
    struct font *pf = font_id >= 0 ? font_get(font_id) : NULL;
    struct qs_tab *t;
    int mw = 0, mh = 0, r, handle;

    if (pf && pf->depth)
    {
        int ch;
        for (ch = 'A'; ch <= 'Z'; ch++)
        {
            int dw, dh;
            scaled_box(pf, ch, &dw, &dh);
            mw = MAX(mw, dw);
            mh = MAX(mh, dh);
        }
        if (mw > 255 || mh > 255)
            mw = mh = 0;
    }
    handle = core_alloc(sizeof(struct qs_tab) + mw * mh);
    if (handle <= 0)
    {
        logf("ipodui: qscroll tables: no memory");
        return;
    }
    tab_handle = handle;
    t = core_get_data(handle);
    for (r = 0; r < QS_SIZE; r++)
    {
        uint32_t c = pack32(ui_gloss_color(&qs_gloss, r, QS_SIZE));
        int in = inset16(r);
        int ea = QS_ALPHA * (16 - (in & 15)) / 16;

        t->row[r].pre = c * QS_ALPHA + QS_ROUND;
        t->row[r].whole = (in + 15) >> 4;
        t->row[r].ea = ((in & 15) && t->row[r].whole > 0) ? ea : 0;
        t->row[r].epre = c * ea + QS_ROUND;
    }
    t->letter = 0;
    t->max_w = mw;
    t->max_h = mh;
}

void ui_qscroll_load(void)
{
    char path[64];

    if (font_id >= 0)
        return;
    snprintf(path, sizeof(path), FONT_DIR "/" QS_FONT ".fnt");
    font_id = font_load_ex(path, 0, 0);
#ifdef HAVE_UI_THREAD_SPLIT
    if (font_id >= 0)
        font_lock(font_id, true);   /* P19 S3: as theme.c's fonts */
#endif
    if (font_id < 0)
        logf("ipodui: qscroll font failed (%d)", font_id);
    if (tab_handle <= 0)
        tab_init();
}

void ui_qscroll_init(struct ui_qscroll *q)
{
    q->shown = false;
    q->letter = 0;
    q->hide = 0;
}

void ui_qscroll_reset(struct ui_qscroll *q)
{
    q->shown = false;
}

static void rect_of(const struct ui_list *l, int *x, int *y)
{
    *x = l->vp.x + (l->vp.width - QS_SIZE) / 2;
    *y = l->vp.y + (l->vp.height - QS_SIZE) / 2;
}

static void invalidate(const struct ui_list *l)
{
    int x, y, h = QS_SIZE;

    if (ui_list_gliding(l))
        return;                 /* the next glide frame redraws it */
    /* whole rows, so it merges with row bands as a list band */
    rect_of(l, &x, &y);
    ui_list_rows_band(l, &y, &h);
    if (h > 0)
        ui_invalidate_band(y, h);
}

void ui_qscroll_wheel(struct ui_qscroll *q, const struct ui_list *l,
                      unsigned velocity, char letter)
{
    bool was = q->shown;
    char old = q->letter;

    if (l->count > QS_MIN_ROWS && velocity >= QS_VEL && letter)
    {
        q->shown = true;
        q->hide = current_tick + QS_HOLD;
    }
    if (!q->shown)
        return;
    if (letter)
        q->letter = letter;     /* "All ..." rows keep the last one */
    if (!was || q->letter != old)
        invalidate(l);
}

int ui_qscroll_tick(struct ui_qscroll *q, const struct ui_list *l)
{
    if (!q->shown)
        return TIMEOUT_BLOCK;
    if (TIME_BEFORE(current_tick, q->hide))
        return q->hide - current_tick;
    q->shown = false;
    invalidate(l);
    return TIMEOUT_BLOCK;
}

/* -- drawing --------------------------------------------------------------- */

/* RGB565 blend, a = 0..32 the weight of 'fg' */
static inline unsigned blend565(unsigned bg, unsigned fg, unsigned a)
{
    unsigned b = (bg | (bg << 16)) & 0x07e0f81f;
    unsigned f = (fg | (fg << 16)) & 0x07e0f81f;
    /* + 0x02008010: round each field (16 = half of /32), so greys stay
     * grey (truncation would favour the 6-bit green) */
    unsigned r = ((f * a + b * (32 - a) + 0x02008010) >> 5) & 0x07e0f81f;
    return (r | (r >> 16)) & 0xffff;
}

static int isqrt(int n)
{
    int x = 0, b = 1 << 14;

    while (b > n)
        b >>= 2;
    while (b)
    {
        if (n >= x + b)
        {
            n -= x + b;
            x = (x >> 1) + b;
        }
        else
            x >>= 1;
        b >>= 2;
    }
    return x;
}

/* Inset of row r's span from the square's side, in 1/16 px (rounded
 * corners: a circle of QS_RADIUS sampled at the row centre). */
static int inset16(int r)
{
    int dy;

    if (r >= QS_RADIUS && r < QS_SIZE - QS_RADIUS)
        return 0;
    dy = (r < QS_RADIUS ? QS_RADIUS - r : r - (QS_SIZE - QS_RADIUS) + 1)
         * 16 - 8;
    return QS_RADIUS * 16 - isqrt(QS_RADIUS * QS_RADIUS * 256 - dy * dy);
}

/* blend565(bg, c, a) given 'pre' = pack32(c) * a + QS_ROUND */
static inline unsigned blend_pre(unsigned bg, uint32_t pre, unsigned a)
{
    uint32_t r = ((pack32(bg) * (32 - a) + pre) >> 5) & 0x07e0f81f;
    return (r | (r >> 16)) & 0xffff;
}

/* F3: draw_slab() from the row table, pixel-identical. */
static void draw_slab_tab(const struct qs_tab *t, int x, int y,
                          int r0, int r1)
{
    int r;

    for (r = r0; r < r1; r++)
    {
        const struct qs_row *row = &t->row[r];
        uint32_t pre = row->pre;
        int whole = row->whole;
        /* not ownable: the slab is centred in the list's viewport (the left column on a split screen) */
        fb_data *p = FBADDR(x, y + r);
        fb_data *end = p + QS_SIZE - whole;
        fb_data *q;

        for (q = p + whole; q < end; q++)
            *q = FB_SCALARPACK_LCD(
                blend_pre(FB_UNPACK_SCALAR_LCD(*q), pre, QS_ALPHA));
        if (row->ea)
        {
            p[whole - 1] = FB_SCALARPACK_LCD(blend_pre(
                FB_UNPACK_SCALAR_LCD(p[whole - 1]), row->epre, row->ea));
            p[QS_SIZE - whole] = FB_SCALARPACK_LCD(blend_pre(
                FB_UNPACK_SCALAR_LCD(p[QS_SIZE - whole]), row->epre,
                row->ea));
        }
    }
}

/* Translucent square rows [r0, r1) at x, y: each pixel of the
 * framebuffer is blended toward the row's gloss colour; the corner
 * pixels by their coverage. (Without the table only.) */
static void draw_slab(int x, int y, int r0, int r1)
{
    int r;

    for (r = r0; r < r1; r++)
    {
        unsigned c = ui_gloss_color(&qs_gloss, r, QS_SIZE);
        int in = inset16(r);
        int whole = (in + 15) >> 4;        /* first fully covered column */
        int edge_a = QS_ALPHA * (16 - (in & 15)) / 16;
        /* not ownable: the slab is centred in the list's viewport (the left column on a split screen) */
        fb_data *p = FBADDR(x, y + r);
        int i;

        for (i = whole; i < QS_SIZE - whole; i++)
            p[i] = FB_SCALARPACK_LCD(
                blend565(FB_UNPACK_SCALAR_LCD(p[i]), c, QS_ALPHA));
        if ((in & 15) && whole > 0)
        {
            /* the partly covered pixel at each end */
            i = whole - 1;
            p[i] = FB_SCALARPACK_LCD(
                blend565(FB_UNPACK_SCALAR_LCD(p[i]), c, edge_a));
            i = QS_SIZE - whole;
            p[i] = FB_SCALARPACK_LCD(
                blend565(FB_UNPACK_SCALAR_LCD(p[i]), c, edge_a));
        }
    }
}

/* Coverage 0..15 of the glyph's pixel (x, y), 0 outside it. 4-bit font
 * cells store the background weight, low nibble first, rows packed. */
static inline int glyph_px(const unsigned char *bits, int w, int h,
                           int x, int y)
{
    int n;

    if (x < 0 || y < 0 || x >= w || y >= h)
        return 0;
    n = y * w + x;
    return 15 - ((n & 1) ? bits[n >> 1] >> 4 : bits[n >> 1] & 15);
}

static void scaled_box(struct font *pf, int ch, int *dw, int *dh)
{
    int asc = pf->ascent;

    *dw = (font_get_width(pf, ch) * QS_CAP + asc - 1) / asc;
    *dh = (pf->height * QS_CAP + asc - 1) / asc;
}

/* The weight 0..32 of the scaled letter's pixel (dx, dy): bilinear from
 * the 4-bit cell, round(a * 32 / (15 << 16)) as a multiply and shift (no
 * divide per pixel on the ARM7: exact for every a, checked
 * exhaustively). */
static int letter_weight(const unsigned char *bits, int w, int h, int step,
                         int dx, int dy)
{
    int sy = ((2 * dy + 1) * step >> 1) - 0x8000;
    int iy = sy >> 16, fy = (sy >> 8) & 0xff;
    int sx = ((2 * dx + 1) * step >> 1) - 0x8000;
    int ix = sx >> 16, fx = (sx >> 8) & 0xff;
    int a0 = glyph_px(bits, w, h, ix, iy) * (256 - fx)
           + glyph_px(bits, w, h, ix + 1, iy) * fx;
    int a1 = glyph_px(bits, w, h, ix, iy + 1) * (256 - fx)
           + glyph_px(bits, w, h, ix + 1, iy + 1) * fx;
    int a = a0 * (256 - fy) + a1 * fy;  /* 0 .. 15 << 16 */

    return a ? ((a >> 8) * 546 + 33280) >> 16 : 0;
}

/* F3: the letter from its cached weights (built at the letter's first
 * frame), pixel-identical to draw_letter(). False if the box doesn't
 * fit the table. */
static bool draw_letter_tab(struct font *pf, int ch, int x0, int y0,
                            int c0, int c1)
{
    struct qs_tab *t;
    int dw, dh, left, top, dx, dy;

    scaled_box(pf, ch, &dw, &dh);
    t = core_get_data(tab_handle);
    if (dw > t->max_w || dh > t->max_h)
        return false;
    if (t->letter != ch)
    {
        /* the bits first: a glyph-cache load may move the block */
        const unsigned char *bits = font_get_bits(pf, ch);
        int w = font_get_width(pf, ch), h = pf->height;
        int step = (pf->ascent << 16) / QS_CAP;

        t = core_get_data(tab_handle);
        for (dy = 0; dy < dh; dy++)
            for (dx = 0; dx < dw; dx++)
                t->a[dy * dw + dx] = letter_weight(bits, w, h, step,
                                                   dx, dy);
        t->letter = ch;
        t->dw = dw;
        t->dh = dh;
    }
    left = x0 + (QS_SIZE - dw) / 2;
    top = y0 + (QS_SIZE - QS_CAP) / 2;
    for (dy = MAX(0, c0 - top); dy < dh && top + dy < c1; dy++)
    {
        const unsigned char *a = &t->a[dy * dw];
        /* not ownable: the slab is centred in the list's viewport (the left column on a split screen) */
        fb_data *p = FBADDR(left, top + dy);

        for (dx = 0; dx < dw; dx++)
            if (a[dx])
                p[dx] = FB_SCALARPACK_LCD(blend565(
                    FB_UNPACK_SCALAR_LCD(p[dx]), 0xffff, a[dx]));
    }
    return true;
}

/* The letter 'ch' of the 4-bit letter font, scaled so its caps are
 * QS_CAP px tall (bilinear), blended in white over the square at x0, y0;
 * rows [c0, c1) only. (Without the table only.) */
static void draw_letter(struct font *pf, int ch, int x0, int y0,
                        int c0, int c1)
{
    const unsigned char *bits = font_get_bits(pf, ch);
    int w = font_get_width(pf, ch), h = pf->height;
    int step = (pf->ascent << 16) / QS_CAP;     /* source px per px, 16.16 */
    int dw, dh, left, top, dx, dy;

    scaled_box(pf, ch, &dw, &dh);
    left = x0 + (QS_SIZE - dw) / 2;
    top = y0 + (QS_SIZE - QS_CAP) / 2;
    for (dy = MAX(0, c0 - top); dy < dh && top + dy < c1; dy++)
    {
        /* not ownable: the slab is centred in the list's viewport (the left column on a split screen) */
        fb_data *p = FBADDR(left, top + dy);

        for (dx = 0; dx < dw; dx++)
        {
            int a = letter_weight(bits, w, h, step, dx, dy);
            if (a)
                p[dx] = FB_SCALARPACK_LCD(blend565(
                    FB_UNPACK_SCALAR_LCD(p[dx]), 0xffff, a));
        }
    }
}

/* '#' is not in the letter font: two bars across, two slanted bars down,
 * cut from solid rows (~ the font's stem weight). */
static void draw_hash(int cx, int top, int y0, int y1)
{
    const int h = 35, w = 30, stem = 6, slant = 6;
    int r;

    lcd_set_foreground(LCD_RGBPACK(0xff, 0xff, 0xff));
    for (r = 0; r < h; r++)
    {
        int y = top + r;
        int off = slant * (h - 1 - r) / (h - 1) - slant / 2;
        if (y < y0 || y >= y1)
            continue;
        if ((r >= 9 && r < 9 + stem) || (r >= 21 && r < 21 + stem))
            lcd_hline(cx - w / 2 + off, cx + w / 2 - 1 + off, y);
        else
        {
            lcd_hline(cx - 9 + off, cx - 9 + off + stem - 1, y);
            lcd_hline(cx + 4 + off, cx + 4 + off + stem - 1, y);
        }
    }
}

void ui_qscroll_draw(const struct ui_qscroll *q, const struct ui_list *l,
                     int y, int h)
{
    struct viewport vp;
    int x0, y0, c0, c1, font, fw = 0, fh;
    char s[2];

    if (!q->shown || !q->letter)
        return;
    rect_of(l, &x0, &y0);
    c0 = MAX(y, y0);
    c1 = MIN(y + h, y0 + QS_SIZE);
    if (c1 <= c0)
        return;

    lcd_set_viewport(NULL);
    if (tab_handle > 0)
        draw_slab_tab(core_get_data(tab_handle), x0, y0, c0 - y0, c1 - y0);
    else
        draw_slab(x0, y0, c0 - y0, c1 - y0);

    if (q->letter == '#')
    {
        draw_hash(x0 + QS_SIZE / 2, y0 + (QS_SIZE - QS_CAP) / 2, c0, c1);
        return;
    }
    if (font_id >= 0 && font_get(font_id)->depth)
    {
        if (tab_handle <= 0 || !draw_letter_tab(font_get(font_id),
                                                q->letter, x0, y0, c0, c1))
            draw_letter(font_get(font_id), q->letter, x0, y0, c0, c1);
        return;
    }

    /* no letter font: the list font, unscaled */
    font = ui_theme->font_ui;
    fh = font_get(font)->ascent;
    s[0] = q->letter;
    s[1] = '\0';
    font_getstringsize((const unsigned char *)s, &fw, NULL, font);

    /* the letter through a viewport over the clipped rows of the square */
    memset(&vp, 0, sizeof(vp));
    vp.x = x0;
    vp.y = c0;
    vp.width = QS_SIZE;
    vp.height = c1 - c0;
    vp.font = font;
    vp.drawmode = DRMODE_FG;
    vp.fg_pattern = LCD_RGBPACK(0xff, 0xff, 0xff);
    lcd_set_viewport(&vp);
    lcd_putsxy((QS_SIZE - fw) / 2, y0 + (QS_SIZE - fh) / 2 - c0, s);
    lcd_set_viewport(NULL);
}
