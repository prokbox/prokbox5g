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
 * iPod UI shell
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
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "backlight.h"
#include "button.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/anim.h"
#include "ipodui/ptrace.h"
#include "ipodui/list.h"
#include "ipodui/input.h"
#include "ipodui/npmark.h"
#ifdef SIMULATOR
#include "ipodui/screen_stack.h"    /* glide_frame()'s split check */
#endif

#define LABEL_MAX   96

/* Marquee (the selected row only, no boost: ~20 fps, one row band) */
#define MQ_DELAY        HZ      /* still before the first scroll */
#define MQ_PAUSE        HZ      /* still after each lap */
#define MQ_STEP_TICKS   5
#define MQ_STEP_PX      2       /* 40 px/s */
#define MQ_GAP          40      /* end of the text to its repeat */
#define MQ_SEG          LCD_WIDTH   /* F2 image segment width */

/* Scrollbar, shown while the list moves and SBAR_HOLD after it: a grey
 * rounded thumb, no track, SBAR_W wide (owner, device #58: about one
 * lowercase letter of the list font), close to the edge it sits
 * against: flush with the screen's right edge (x 313-319), or SBAR_GAP
 * from the list half's right edge in a split (x 152-158, clear of the
 * dark panel). #74 (owner, 2026-10-15): a see-through overlay on top of
 * everything that never moves the text: the rows keep their full width
 * whether it shows or not, and it is blended (SBAR_ALPHA) over rows
 * drawn fresh in the same frame, never over a frame that already has it
 * (draw_scrollbar()'s clip rectangle is always freshly drawn pixels). */
#define SBAR_W          7
#define SBAR_GAP        1       /* split only */
#define SBAR_INSET      2       /* ~ gap to the view's top and bottom */
#define SBAR_MIN_THUMB  12      /* ~ */
#define SBAR_HOLD       HZ      /* hides ~1 s after the motion stops */
#define SBAR_ALPHA32    19      /* the thumb's weight, /32 (~60 %) */

#ifdef SIMULATOR
/* list_force_bar on|off (uisimulator/common/sim_cmd_fit3a.c): the bar of
 * every long list stays on (1), or hides at once (0, then -1: normal) */
static volatile signed char sbar_force = -1;
#define UI_LIST_SIM_WAKE    MAKE_SYS_EVENT(SYS_EVENT_CLS_MISC, 0x21)
#else
#define sbar_force          (-1)
#endif

/* Row slot tags of the shared strip: slot i holds content row slot_row[i],
 * or -1; slot_stub[i]: drawn with stand-in art (no fetch while gliding).
 * They belong to the list that claimed the strip last (its strip_id); a
 * claim by anyone else ('kept' false) clears them all. 48 bytes of
 * statics. */
#define SLOTS_MAX       16
static short slot_row[SLOTS_MAX];
static bool slot_stub[SLOTS_MAX];

/* F2: what the strip's marquee image (UI_STRIP_OWNER_MARQUEE) shows: the
 * list (strip_id), its selected row and the label width. 12 bytes. */
static struct { int id, sel, w; } mq_tag;

static void slots_clear(void)
{
    int i;
    for (i = 0; i < SLOTS_MAX; i++)
    {
        slot_row[i] = -1;
        slot_stub[i] = false;
    }
    mq_tag.id = 0;                  /* row contents may have changed */
}

/* -- geometry -------------------------------------------------------------- */

/* Whole rows per view; content row i starts at row_y(i) (px from the top
 * of the content), so every 'rows' consecutive rows fill the view. */
static int visible_rows(const struct ui_list *l)
{
    return l->style.rows > 0 ? l->style.rows : 1;
}

/* The 7G's whole-pixel rows (fidelity audit 1 fix #4): the view height
 * splits into 'rows' rows, the first (height % rows) of them 1 px taller
 * (220 / 9: four 25 px rows, then five of 24; tops 0, 25, 50, 75, 100,
 * 124, 148, 172, 196). The pattern repeats every view's worth of rows,
 * so a page always sums to the view height. */
static int row_y(const struct ui_list *l, int i)
{
    int rows = visible_rows(l);
    int q = l->vp.height / rows, rem = l->vp.height % rows;
    int k = i % rows;

    return (i / rows) * l->vp.height + k * q + MIN(k, rem);
}

static int row_hh(const struct ui_list *l, int i)
{
    return row_y(l, i + 1) - row_y(l, i);
}

/* The row containing content y (>= 0). */
static int row_at(const struct ui_list *l, int y)
{
    int rows = visible_rows(l);
    int q = l->vp.height / rows, rem = l->vp.height % rows;
    int k = y % l->vp.height, big = rem * (q + 1);

    k = k < big ? k / (q + 1) : rem + (k - big) / q;
    return (y / l->vp.height) * rows + k;
}

/* The tallest row: a strip slot's height. */
static int slot_h(const struct ui_list *l)
{
    int rows = visible_rows(l);
    return (l->vp.height + rows - 1) / rows;
}

static int view_h(const struct ui_list *l)
{
    return l->vp.height;
}

static int strip_slots(const struct ui_list *l)
{
    int n = UI_STRIP_SIZE / (slot_h(l) * l->vp.width * (int)sizeof(fb_data));
    return MIN(n, SLOTS_MAX);
}

/* Gliding needs the strip and room for every row the window can touch. */
static bool can_glide(const struct ui_list *l)
{
    return ui_strip_available() && strip_slots(l) >= visible_rows(l) + 1;
}

static bool long_list(const struct ui_list *l)
{
    return l->count > visible_rows(l);
}

/* A split list's scrollbar sits SBAR_GAP from the list half's edge */
static bool sbar_split(const struct ui_list *l)
{
    return l->vp.width < LCD_WIDTH && long_list(l);
}

/* Right end of the rows' bar, and the columns between the chevron's (and
 * the values') right edge and it: never dependent on the scrollbar */
static int row_w(const struct ui_list *l)
{
    return l->vp.width;
}

static int row_pad(const struct ui_list *l)
{
    (void)l;
    return ui_theme->chevron_pad;
}

/* Keeps the selection in view. 'dir' is the move that got it there (+1
 * down, -1 up, 0 a resettle or a new count): a move keeps
 * UI_LIST_EDGE_ROWS rows visible beyond the selection in its direction
 * (list.h), a resettle only keeps it visible, so a restored position
 * (browse.c, a pop) stays where it was. */
/* The top that keeps sel in view after a move in dir from top */
static int top_for(const struct ui_list *l, int top, int sel, int dir)
{
    int rows = visible_rows(l), m = UI_LIST_EDGE_ROWS;

    if (m > (rows - 1) / 2)
        m = (rows - 1) / 2;
    if (dir > 0 && sel > top + rows - 1 - m)
        top = sel - (rows - 1 - m);
    else if (dir < 0 && sel < top + m)
        top = sel - m;
    if (sel < top)
        top = sel;
    else if (sel >= top + rows)
        top = sel - rows + 1;
    if (top > l->count - rows)
        top = l->count - rows;
    if (top < 0)
        top = 0;
    return top;
}

static void clamp_top_dir(struct ui_list *l, int dir)
{
    l->top = top_for(l, l->top, l->selected, dir);
}

static void clamp_top(struct ui_list *l)
{
    clamp_top_dir(l, 0);
}

static void update_max(struct ui_list *l)
{
    ui_scroll_set_max(&l->scroll, long_list(l)
                      ? row_y(l, l->count) - view_h(l) : 0);
}

/* Text box of a row, in row coordinates: from the text cell to the
 * chevron (whether or not this row shows one, as in Phase 2). */
static int text_w(const struct ui_list *l)
{
    return row_w(l) - row_pad(l) - ui_paint_chevron_w()
           - l->style.text_x - l->extra_w;
}

static const char *label_of(struct ui_list *l, int idx, char *buf)
{
    const char *s = l->get_label(idx, l->data, buf, LABEL_MAX);
    const char *v;

    if (!s)
        return "";
    /* #38 inline value (paint.h UI_VALUE_SEP): the name part only */
    v = strchr(s, UI_VALUE_SEP);
    if (v)
    {
        size_t n = MIN((size_t)(v - s), (size_t)LABEL_MAX - 1);
        memmove(buf, s, n);
        buf[n] = '\0';
        s = buf;
    }
    return s;
}

/* #38: draws row idx's inline value, if its label has one, right-aligned
 * where the chevron would be, in the row viewport (row coordinates, text
 * top ty); returns the width it takes (value + gap) or 0. */
static int draw_value(struct ui_list *l, int idx, int w, int ty, bool sel)
{
    const struct ui_theme *t = ui_theme;
    char buf[LABEL_MAX];
    const char *s = l->get_label(idx, l->data, buf, LABEL_MAX);
    const char *v = s ? strchr(s, UI_VALUE_SEP) : NULL;
    int vw = 0;

    if (!v || !*++v)
        return 0;
    font_getstringsize(v, &vw, NULL, l->vp.font);
    lcd_set_foreground(sel ? t->sel_fg
                       : l->is_off && l->is_off(idx, l->data) ? t->list_fg_off
                       : t->value_fg);
    lcd_set_drawmode(DRMODE_FG);
    lcd_putsxy(w - row_pad(l) - vw, ty, v);
    lcd_set_drawmode(DRMODE_SOLID);
    return vw + UI_VALUE_GAP - row_pad(l) - ui_paint_chevron_w();
}

/* What row idx's inline value takes from text_w() (as draw_value()
 * returns it), without drawing; 0 without a value. */
static int value_w(struct ui_list *l, int idx)
{
    char buf[LABEL_MAX];
    const char *s = l->get_label(idx, l->data, buf, LABEL_MAX);
    const char *v = s ? strchr(s, UI_VALUE_SEP) : NULL;
    int vw = 0;

    if (!v || !*++v)
        return 0;
    font_getstringsize(v, &vw, NULL, l->vp.font);
    return MAX(0, vw + UI_VALUE_GAP - row_pad(l) - ui_paint_chevron_w());
}

/* #69: whether row idx has a chevron's place (drawn when selected) */
static bool row_chevron(struct ui_list *l, int idx)
{
    return l->has_chevron && l->has_chevron(idx, l->data);
}

/* #69: what row idx's now-playing mark takes from text_w() (the glyph
 * and its gap, less the chevron's place it sits in on a row without a
 * chevron); 0 without a mark. */
static int mark_w(struct ui_list *l, int idx)
{
    if (!l->has_mark || !l->has_mark(idx, l->data))
        return 0;
    return UI_NPMARK_W + UI_NPMARK_GAP
           - (row_chevron(l, idx) ? 0 : ui_paint_chevron_w());
}

/* The selected row's text box, less its inline value and its mark: the
 * marquee scrolls inside it, never over either. */
static int sel_text_w(struct ui_list *l)
{
    return text_w(l) - value_w(l, l->selected) - mark_w(l, l->selected);
}

/* Measures the selected label and restarts the marquee delay. */
static void marquee_reset(struct ui_list *l)
{
    char buf[LABEL_MAX];
    int w = 0;

    l->mq_w = 0;
    l->mq_x = 0;
    if (l->count <= 0)
        return;
    font_getstringsize(label_of(l, l->selected, buf), &w, NULL, l->vp.font);
    l->mq_tw = sel_text_w(l);
    if (w > l->mq_tw)
    {
        l->mq_w = w;
        l->mq_next = current_tick + MQ_DELAY;
    }
}

/* Resettles the view after a geometry change: snapped, nothing cached. */
static void resettle(struct ui_list *l)
{
    clamp_top(l);
    update_max(l);
    l->scroll.target = row_y(l, l->top);
    ui_scroll_snap(&l->scroll);
    l->fill_due = false;
    slots_clear();
    marquee_reset(l);
}

void ui_list_init(struct ui_list *l, const struct viewport *vp, int count,
                  const char *(*get_label)(int, void *, char *, size_t),
                  void *data)
{
    memset(l, 0, sizeof(*l));
    l->vp = *vp;
    l->vp.font = ui_theme->font_ui;
    l->vp.drawmode = DRMODE_SOLID;
    l->vp.buffer = NULL;
    l->count = count;
    l->get_label = get_label;
    l->data = data;
    l->style.rows = ui_theme->list_rows;
    l->style.text_x = ui_theme->text_pad_x;
    l->strip_id = ui_strip_new_owner();
    ui_scroll_init(&l->scroll, 0);
    update_max(l);
    marquee_reset(l);
}

void ui_list_set_style(struct ui_list *l, const struct ui_row_style *st)
{
    l->style = *st;
    resettle(l);
}

void ui_list_set_height(struct ui_list *l, int height, int rows)
{
    l->vp.height = height;
    l->style.rows = rows;
    resettle(l);
}

bool ui_list_row_band(const struct ui_list *l, int idx, int *y, int *h)
{
    int r = idx - l->top;

    if (idx < 0 || idx >= l->count || r < 0 || r >= visible_rows(l))
        return false;
    *y = l->vp.y + row_y(l, idx) - row_y(l, l->top);
    *h = row_hh(l, idx);
    return true;
}

void ui_list_rows_band(const struct ui_list *l, int *y, int *h)
{
    int base = row_y(l, l->top);
    int y0 = MAX(*y, l->vp.y) - l->vp.y;
    int y1 = MIN(*y + *h, l->vp.y + l->vp.height) - l->vp.y;
    int r0, r1;

    if (y1 <= y0)
    {
        *h = 0;
        return;
    }
    r0 = row_at(l, base + y0);
    r1 = row_at(l, base + y1 - 1);
    y0 = row_y(l, r0) - base;
    y1 = MIN(row_y(l, r1 + 1) - base, l->vp.height);
    *y = l->vp.y + y0;
    *h = y1 - y0;
}

/* -- drawing ------------------------------------------------------------ */

/* Draws 'text' at x, y of the current viewport, ending in "..." if it is
 * wider than w (whole characters, the ellipsis appended even after a
 * period, as on the 7G: "arr...."). */
static void put_fit(int x, int y, int w, const char *text, int font)
{
    char buf[LABEL_MAX + 4];
    int tw = 0, ew = 0;
    int n;

    font_getstringsize((const unsigned char *)text, &tw, NULL, font);
    if (tw <= w)
    {
        lcd_putsxy(x, y, (const unsigned char *)text);
        return;
    }
    font_getstringsize((const unsigned char *)"...", &ew, NULL, font);
    n = font_measurestring((const unsigned char *)text, LABEL_MAX - 1,
                           MAX(w - ew, 0), &tw, NULL, font);
    while (n > 0 && text[n - 1] == ' ')
        n--;
    memcpy(buf, text, n);
    strcpy(buf + n, "...");
    lcd_putsxy(x, y, (const unsigned char *)buf);
}

/* Single-line text baseline from the row top: ui_theme->row_baseline in
 * a 25 px row, 6.75 px above the row bottom on the 7G (audit fix #4), so
 * 1 px higher in a 24 px row; taller rows centre it as before. */
static int row_base(int h)
{
    int b = ui_theme->row_baseline;
    return h >= 25 ? b + (h - 25) / 2 : b + h - 25;
}

/* y of a text cell whose baseline (first row under the ink) is 'base' */
static int cell_y(int font, int base, int h)
{
    if (font == FONT_UI)
        return (h - font_get(font)->height) / 2;
    return base - font_get(font)->ascent;
}

/* Draws row 'idx' into the row-sized viewport 'rv' (the framebuffer at
 * the row's LCD position, or a strip slot): the selection bar (or list
 * background) over the whole row right of the art box, the art box, the
 * separator, the label(s) and the extras. The chevron is only drawn on
 * the selected row, as on the 7G. 'mx' is the marquee offset; 'fetch'
 * lets the art box read the art cache (at rest). Returns false if the
 * art box got a stand-in a fetch could replace. Leaves the default
 * viewport set. May yield when 'fetch' is set (see draw_thumb). */
static bool draw_row(struct ui_list *l, int idx, const struct viewport *rv,
                     int mx, bool fetch)
{
    const struct ui_theme *t = ui_theme;
    const struct ui_row_style st = l->style;
    struct viewport v = *rv, tv;
    int h = v.height;
    int w = row_w(l);               /* the selection bar's end */
    int x0 = st.thumb;
    bool sel = (idx == l->selected), two, final = true, chev;
    int mw;
    char buf[LABEL_MAX];
    const char *label, *sub = NULL;
    int base;

    v.font = l->vp.font;
    v.drawmode = DRMODE_SOLID;
    lcd_set_viewport(&v);

    if (idx < 0 || idx >= l->count)
    {
        lcd_set_foreground(t->list_bg);
        lcd_fillrect(0, 0, v.width, h);
        lcd_set_viewport(NULL);
        return true;
    }

    if (sel)
        ui_paint_selection(x0, 0, w - x0, h);
    else
    {
        lcd_set_foreground(t->list_bg);
        lcd_fillrect(x0, 0, w - x0, h);
        if (st.flags & UI_ROW_SEP)
        {
            lcd_set_foreground(t->list_sep);
            lcd_hline(x0, v.width - 1, h - 1);
        }
    }
    if (x0 > 0)
    {
        if (l->draw_thumb)
        {
            final = l->draw_thumb(idx, l->data, x0, fetch);
            lcd_set_viewport(&v);
        }
        else
        {
            lcd_set_foreground(t->list_bg);
            lcd_fillrect(0, 0, x0, h);
        }
    }
    chev = row_chevron(l, idx);
    if (sel && chev)
        ui_paint_chevron(w - row_pad(l) - 1, 0, h, true);
    mw = mark_w(l, idx);
    if (mw)
        ui_npmark_draw(w - row_pad(l) - (chev ? ui_paint_chevron_w() : 0),
                       h, sel);

    two = (st.flags & UI_ROW_TWO_LINE) && l->get_sublabel;
    if (two)
    {
        sub = l->get_sublabel(idx, l->data, buf, sizeof(buf));
        if (sub && !*sub)
            sub = NULL;
    }
    if (two)
        base = (h - t->two_block_h) / 2 - 1 + t->two_title_base;
    else
        base = row_base(h);

    if (sub)
    {
        /* the second line first: buf is reused for the label below */
        tv = v;
        tv.x += st.text_x;
        tv.width = text_w(l) - mw;
        tv.font = t->font_small;
        tv.fg_pattern = sel ? t->sel_fg_sub : t->list_fg_sub;
        tv.drawmode = DRMODE_FG;
        lcd_set_viewport(&tv);
        put_fit(0, cell_y(tv.font, base + t->two_sub_dy, h), tv.width, sub,
                tv.font);
    }

    label = label_of(l, idx, buf);
    if (*label)
    {
        /* the text gets its own viewport so long labels clip before the
         * chevron; the marquee scrolls inside it */
        int ty;

        tv = v;
        tv.x += st.text_x;
        tv.width = text_w(l) - mw;
        tv.fg_pattern = sel ? t->sel_fg
                        : l->is_off && l->is_off(idx, l->data)
                          ? t->list_fg_off : t->list_fg;
        tv.drawmode = DRMODE_FG;
        /* place by baseline, as the header does: the shell fonts carry
         * tall line metrics, so centring the cell sets the text low */
        ty = cell_y(tv.font, base, h);
        lcd_set_viewport(&v);
        tv.width -= MAX(0, draw_value(l, idx, w, ty, sel));
        lcd_set_viewport(&tv);
        if (sel && mx > 0 && l->mq_w)
        {
            lcd_putsxy(-mx, ty, label);
            lcd_putsxy(l->mq_w + MQ_GAP - mx, ty, label);
        }
        else
            put_fit(0, ty, tv.width, label, tv.font);
    }
    if (l->draw_extra)
    {
        lcd_set_viewport(&v);
        l->draw_extra(idx, l->data, w, h, sel);
    }
    lcd_set_viewport(NULL);
    return final;
}

/* Floats the screen's overlay over rows just drawn in [y, y+h). */
static void draw_overlay(struct ui_list *l, int y, int h)
{
    if (l->overlay)
    {
        lcd_set_viewport(NULL);
        l->overlay(l->data, y, h);
        lcd_set_viewport(NULL);
    }
}

/* The default viewport for content row 'idx' at rest (on screen). */
static void row_vp_lcd(const struct ui_list *l, int idx, struct viewport *rv)
{
    *rv = l->vp;
    rv->y += row_y(l, idx) - row_y(l, l->top);
    rv->height = row_hh(l, idx);
    rv->buffer = NULL;
}

static void sbar_rect(const struct ui_list *l, int *x, int *y, int *w, int *h)
{
    *w = SBAR_W;
    *x = l->vp.x + l->vp.width - SBAR_W - (sbar_split(l) ? SBAR_GAP : 0);
    *y = l->vp.y + SBAR_INSET;
    *h = view_h(l) - 2 * SBAR_INSET;
}

/* The thumb's rectangle (LCD coordinates) for the rendered scroll
 * position; false when there is none to show. */
static bool sbar_thumb(const struct ui_list *l, int *x, int *y, int *w,
                       int *h)
{
    int content = row_y(l, l->count), th;

    if (!l->sbar || content <= 0)
        return false;
    sbar_rect(l, x, y, w, h);
    th = *h * view_h(l) / content;
    if (th < SBAR_MIN_THUMB)
        th = SBAR_MIN_THUMB;
    if (th > *h)
        th = *h;
    if (l->scroll.max > 0)
        *y += (*h - th) * l->scroll.pos / l->scroll.max;
    *h = th;
    return true;
}

/* Blends the thumb (a glossy rod, ui_theme->sbar_gloss, no track; its
 * corners left out) over the framebuffer, SBAR_ALPHA32/32 of the way,
 * inside the clip rectangle cx, cy, cw, ch (LCD coordinates) only: the
 * caller drew those pixels fresh in this frame, so the thumb never
 * compounds. One pass over its own rectangle at most. */
static void draw_scrollbar(struct ui_list *l, int cx, int cy, int cw, int ch)
{
    const struct ui_gloss *g = ui_theme->sbar_gloss;
    unsigned col[SBAR_W];
    int x, y, w, h, x0, x1, y0, y1, i, r;

    if (!sbar_thumb(l, &x, &y, &w, &h))
        return;
    x0 = MAX(x, cx);
    x1 = MIN(x + w, cx + cw);
    y0 = MAX(y, cy);
    y1 = MIN(y + h, cy + ch);
    if (x0 >= x1 || y0 >= y1)
        return;
    for (i = 0; i < w; i++)
    {
        unsigned c = ui_gloss_color(g, i, w);
        /* packed 0x07e0f81f form, pre-weighted */
        col[i] = ((c | (c << 16)) & 0x07e0f81f) * SBAR_ALPHA32;
    }
    for (r = y0; r < y1; r++)
    {
        /* not ownable: the list column, left of UI_PANEL_X */
        fb_data *p = FBADDR(0, r);
        bool edge = (r == y || r == y + h - 1);

        for (i = x0; i < x1; i++)
        {
            unsigned d, o;

            if (edge && (i == x || i == x + w - 1))
                continue;           /* the rounded corners */
            d = p[i];
            d = (d | (d << 16)) & 0x07e0f81f;
            o = ((d * (32 - SBAR_ALPHA32) + col[i - x] + 0x02008010) >> 5)
                & 0x07e0f81f;
            p[i] = o | (o >> 16);
        }
    }
}

static void draw_band(struct ui_list *l, int y, int h)
{
    const struct ui_theme *t = ui_theme;
    struct viewport vp = l->vp;     /* lcd keeps the pointer; use a stack copy */
    struct viewport rv;
    int base = row_y(l, l->top);
    int y0 = MAX(y, vp.y) - vp.y;   /* band in list coordinates */
    int y1 = MIN(y + h, vp.y + vp.height) - vp.y;
    int r, e0 = -1, e1 = y1;        /* the rows drawn: [e0, e1) */

    if (y1 <= y0)
        return;

    for (r = row_at(l, base + y0); ; r++)
    {
        int ry = row_y(l, r) - base;

        if (ry >= y1)
            break;
        if (e0 < 0)
            e0 = MAX(ry, 0);
        if (r < l->count && ry + row_hh(l, r) <= vp.height)
        {
            row_vp_lcd(l, r, &rv);
            draw_row(l, r, &rv, r == l->selected ? l->mq_x : 0, true);
            e1 = MAX(y1, MIN(ry + row_hh(l, r), vp.height));
        }
        else
        {
            /* below the last item */
            lcd_set_viewport(&vp);
            lcd_set_foreground(t->list_bg);
            lcd_fillrect(0, ry, vp.width, y1 - ry);
            e1 = y1;
            break;
        }
    }
    if (e0 < 0)
        e0 = y0;
    lcd_set_viewport(NULL);
    /* whole rows were drawn: the overlay and the bar over all of them */
    draw_overlay(l, vp.y + e0, e1 - e0);
    draw_scrollbar(l, 0, vp.y + e0, LCD_WIDTH, e1 - e0);
}

void ui_list_draw(struct ui_list *l, int y, int h)
{
    if (y <= l->vp.y && y + h >= l->vp.y + l->vp.height)
    {
        /* a full draw (push, pop, WPS return): settle */
        l->scroll.target = row_y(l, l->top);
        ui_scroll_snap(&l->scroll);
        if (sbar_force <= 0 || !long_list(l))
            l->sbar = false;
        l->sbar_new = false;
        l->fill_due = false;
        marquee_reset(l);
    }
    else if (l->count > 0)
    {
        /* a row redrawn for a new value (a leaf) or a mark that came or
         * went (#69): the marquee's box follows, and a label that no
         * longer fits starts to scroll */
        int ry, rh;

        if (ui_list_row_band(l, l->selected, &ry, &rh)
            && ry < y + h && ry + rh > y && sel_text_w(l) != l->mq_tw)
            marquee_reset(l);
    }
    draw_band(l, y, h);
}

/* -- gliding ------------------------------------------------------------ */

static void slot_forget(int row)
{
    int i;
    for (i = 0; i < SLOTS_MAX; i++)
        if (slot_row[i] == row)
            slot_row[i] = -1;
}

/* One glide frame: renders the rows entering the window into their strip
 * slots (once; art only from RAM) and blits the window. A fast-spin jump
 * frame is the same at a row-grid position, traced "update anim:jump". */
static void glide_frame(struct ui_list *l)
{
    int sh = slot_h(l), vh = view_h(l), w = l->vp.width;
    int pos = l->scroll.pos;
    int nslots = strip_slots(l);
    struct frame_buffer_t fb;
    struct viewport rv;
    bool kept;
    fb_data *strip;
    int r;

    strip = ui_strip_claim(l->strip_id, &kept);
    if (!strip)
        return;
    if (!kept)
        slots_clear();

    for (r = row_at(l, pos); row_y(l, r) < pos + vh; r++)
    {
        int slot = r % nslots;
        int ry = row_y(l, r), rh = row_hh(l, r);
        fb_data *src = strip + slot * sh * w;
        int top = MAX(ry, pos), bot = MIN(ry + rh, pos + vh);

        if (slot_row[slot] != r)
        {
            memset(&fb, 0, sizeof(fb));
            fb.fb_ptr = src;
            fb.stride = STRIDE_MAIN(w, sh);
            fb.elems = LCD_NBELEMS(w, sh);
            memset(&rv, 0, sizeof(rv));
            rv.width = w;
            rv.height = rh;
            rv.buffer = &fb;
            slot_stub[slot] = !draw_row(l, r, &rv, 0, false);
            slot_row[slot] = r;
        }
        lcd_set_viewport(NULL);
        lcd_bitmap_part(src, 0, top - ry, STRIDE_MAIN(w, sh),
                        l->vp.x, l->vp.y + top - pos, w, bot - top);
    }
    ui_strip_put();
    draw_overlay(l, l->vp.y, vh);
    draw_scrollbar(l, 0, l->vp.y, LCD_WIDTH, vh);
#ifdef SIMULATOR
    {
        /* the band check of ui_main.c refresh() for glide frames, which
         * push here: a split screen's list stays left of its panel */
        const struct ui_screen *top = ui_top();

        if (ui_screen_is_split(top) && l->vp.x + w > top->header_w)
            UPDATE_TRACE("split overdraw %s glide", top->ops->name);
    }
#endif
    ui_lcd_push(l->jump ? "anim:jump" : "anim:scroll", 0, l->vp.y,
                LCD_WIDTH, vh);
}

/* After a glide: are rows on screen showing stand-in art? */
static bool stubs_visible(const struct ui_list *l)
{
    int nslots = strip_slots(l), r;

    if (nslots <= 0)
        return false;
    for (r = l->top; r < l->top + visible_rows(l) && r < l->count; r++)
        if (slot_row[r % nslots] == r && slot_stub[r % nslots])
            return true;
    return false;
}

/* At rest, after the glide ended: draws the rows that got stand-in art
 * again, now reading the art cache, one band each. */
static void fill_art(struct ui_list *l)
{
    int nslots = strip_slots(l), r;

    for (r = l->top; r < l->top + visible_rows(l) && r < l->count; r++)
    {
        int slot = r % nslots;
        struct viewport rv;

        if (slot_row[slot] != r || !slot_stub[slot])
            continue;
        slot_row[slot] = -1;        /* the next glide renders it again */
        slot_stub[slot] = false;
        /* F10: only the art box changes (nothing else reaches x < thumb:
         * the bar starts right of it, the overlay is centred) */
        row_vp_lcd(l, r, &rv);
        rv.width = l->style.thumb;
        rv.font = l->vp.font;
        rv.drawmode = DRMODE_SOLID;
        PT_VAR(t_row);
        lcd_set_viewport(&rv);
        l->draw_thumb(r, l->data, l->style.thumb, true);
        lcd_set_viewport(NULL);
        PT_LAP(t_row);
        PT_VAR(t_push);
        ui_lcd_push("art", rv.x, rv.y, rv.width, rv.height);
        PT_LAP(t_push);
        PT_TRACE("rowart fill row=%d fill_us=%lu push_us=%lu px=%d "
                 "since_glide_ms=%ld", r, t_row, t_push,
                 rv.width * rv.height,
                 (long)(current_tick - l->last_frame) * (1000 / HZ));
    }
}

static void start_glide(struct ui_list *l)
{
    if (l->scroll.pos == l->scroll.target)
        l->last_frame = current_tick;
}

/* Fast spin: whole-row jumps instead of a glide (UI_LIST_JUMP_V, list.h) */
static void jump_mode(struct ui_list *l)
{
    unsigned v = ui_input_wheel_speed();

    if (UI_LIST_JUMP_V <= 0)
        l->jump = false;
    else if (v >= UI_LIST_JUMP_V)
        l->jump = true;
    else if (v < UI_LIST_JUMP_V_EXIT)
        l->jump = false;
}

int ui_list_move(struct ui_list *l, int delta, int band_y[2], int band_h[2])
{
    int old_sel = l->selected, old_top = l->top;
    int sel = old_sel + delta;
    bool gliding = l->scroll.pos != l->scroll.target;
    int a, b;

    if (l->count <= 0)
        return 0;
    if (sel < 0)
        sel = 0;
    if (sel >= l->count)
        sel = l->count - 1;
    if (sel == old_sel)
        return 0;

    l->selected = sel;
    clamp_top_dir(l, sel > old_sel ? 1 : -1);
    if (long_list(l))
    {
        if (!l->sbar)
            l->sbar_new = true;     /* ui_list_tick() pushes its column */
        l->sbar = true;
        l->sbar_hide = current_tick + SBAR_HOLD;
    }
    marquee_reset(l);
    slot_forget(old_sel);
    slot_forget(sel);

    if (can_glide(l) && (gliding || l->top != old_top))
    {
        jump_mode(l);
        if (!l->jump)
            start_glide(l);
        ui_scroll_set_target(&l->scroll, row_y(l, l->top));
        l->sbar_new = false;        /* every frame pushes the whole view */
        l->fill_due = false;
        if (l->scroll.pos != l->scroll.target)
            return 0;               /* ui_list_tick() draws the frames */
        /* a glide reversed onto the exact spot it had reached: no frame
         * follows, so redraw the view at rest for the highlight */
        band_y[0] = l->vp.y;
        band_h[0] = view_h(l);
        return 1;
    }

    if (l->top != old_top)
    {
        /* no strip: snap, like Phase 2 */
        l->scroll.target = row_y(l, l->top);
        ui_scroll_snap(&l->scroll);
        l->sbar_new = false;
        band_y[0] = l->vp.y;
        band_h[0] = view_h(l);
        return 1;
    }

    /* only the old and the new row (bar + chevron move); the rows in
     * between are untouched even when the wheel skips several */
    a = MIN(old_sel, sel);
    b = MAX(old_sel, sel);
    if (b == a + 1)
    {
        ui_list_row_band(l, a, &band_y[0], &band_h[0]);
        band_h[0] = row_y(l, b + 1) - row_y(l, a);
        return 1;
    }
    ui_list_row_band(l, a, &band_y[0], &band_h[0]);
    ui_list_row_band(l, b, &band_y[1], &band_h[1]);
    return 2;
}

int ui_list_top_after(const struct ui_list *l, int delta)
{
    int sel = l->selected + delta;

    if (l->count <= 0)
        return 0;
    sel = sel < 0 ? 0 : sel >= l->count ? l->count - 1 : sel;
    if (sel == l->selected)
        return l->top;
    return top_for(l, l->top, sel, sel > l->selected ? 1 : -1);
}

int ui_list_shown_top(const struct ui_list *l)
{
    int r;

    if (l->count <= 0)
        return 0;
    r = row_at(l, MAX(l->scroll.pos, 0));
    return MIN(r, l->count - 1);
}

void ui_list_set_count(struct ui_list *l, int count)
{
    l->count = count;
    if (l->selected >= count)
        l->selected = count - 1;
    if (l->selected < 0)
        l->selected = 0;
    clamp_top(l);
    update_max(l);
    l->scroll.target = row_y(l, l->top);
    ui_scroll_snap(&l->scroll);
    l->fill_due = false;
    /* row contents may have shifted */
    slots_clear();
}

int ui_list_skip(const struct ui_list *l, int delta)
{
    int d = delta > 0 ? 1 : -1, i = l->selected, last = l->selected;
    int k = delta > 0 ? delta : -delta;

    if (!l->is_off || !delta)
        return delta;
    while (k > 0)
    {
        i += d;
        if (i < 0 || i >= l->count)
            break;                      /* clamped at the ends, no wrap */
        if (!l->is_off(i, l->data))
        {
            last = i;
            k--;
        }
    }
    return last - l->selected;
}

static int min_wait(int a, int b)
{
    if (a == TIMEOUT_BLOCK)
        return b;
    if (b == TIMEOUT_BLOCK)
        return a;
    return MIN(a, b);
}

/* Redraws the selected row at rest (marquee step or park). */
static void redraw_selected(struct ui_list *l)
{
    struct viewport rv;

    row_vp_lcd(l, l->selected, &rv);
    draw_row(l, l->selected, &rv, l->mq_x, true);
    draw_overlay(l, rv.y, rv.height);
    draw_scrollbar(l, 0, rv.y, LCD_WIDTH, rv.height);
    ui_lcd_push("marquee", 0, rv.y, LCD_WIDTH, rv.height);
}

/* F2: a marquee step. The selected label is rendered once (per
 * selection) over its bar rows into the strip as [label][gap][label],
 * then the cell's width again, cut into LCD-wide segments stacked one
 * row height apart (a viewport never exceeds the LCD); each step blits
 * the window at mq_x (from one or two segments) into the text cell's
 * rows and pushes the row band. The first step after a
 * lap (mq_x 0, the ellipsized label) and a missing strip take the full
 * row redraw. Claiming the strip only drops glide slots and a parked
 * drift cover, which re-render or re-read on their next use. */
static void mq_step(struct ui_list *l)
{
    const struct ui_theme *t = ui_theme;
    struct viewport rv, bv;
    struct frame_buffer_t fb;
    int tw = l->mq_tw, ty, fh, y1, bw, h, base, cy, nseg, k, x, n;
    bool kept, two;
    fb_data *buf;
    char lb[LABEL_MAX];
    const char *label;

    row_vp_lcd(l, l->selected, &rv);
    h = rv.height;
    bw = l->mq_w + MQ_GAP + tw;
    nseg = (bw + MQ_SEG - 1) / MQ_SEG;
    if (l->mq_x <= 0 || tw <= 0
        || (size_t)nseg * MQ_SEG * h * sizeof(fb_data) > UI_STRIP_SIZE)
    {
        redraw_selected(l);
        return;
    }
    buf = ui_strip_claim(UI_STRIP_OWNER_MARQUEE, &kept);
    if (!buf)
    {
        redraw_selected(l);
        return;
    }

    /* the title cell, as draw_row() places it; with a second line, never
     * below that line's cell */
    two = (l->style.flags & UI_ROW_TWO_LINE) && l->get_sublabel;
    if (two)
    {
        base = (h - t->two_block_h) / 2 - 1 + t->two_title_base;
        y1 = cell_y(t->font_small, base + t->two_sub_dy, h);
    }
    else
    {
        base = row_base(h);
        y1 = h;
    }
    cy = ty = cell_y(l->vp.font, base, h);
    fh = font_get(l->vp.font)->height;
    y1 = MIN(MIN(ty + fh, y1), h);
    ty = MAX(ty, 0);

    if (!kept || mq_tag.id != l->strip_id || mq_tag.sel != l->selected
        || mq_tag.w != l->mq_w)
    {
        /* render once: per segment the bar, then the label twice,
         * clipped to the segment (it may yield: the strip stays pinned) */
        label = label_of(l, l->selected, lb);
        for (k = 0; k < nseg; k++)
        {
            memset(&fb, 0, sizeof(fb));
            fb.fb_ptr = buf + k * MQ_SEG * h;
            fb.stride = STRIDE_MAIN(MQ_SEG, h);
            fb.elems = LCD_NBELEMS(MQ_SEG, h);
            memset(&bv, 0, sizeof(bv));
            bv.width = MQ_SEG;
            bv.height = h;
            bv.buffer = &fb;
            bv.font = l->vp.font;
            bv.drawmode = DRMODE_SOLID;
            lcd_set_viewport(&bv);
            ui_paint_selection(0, 0, MQ_SEG, h);
            bv.fg_pattern = t->sel_fg;
            bv.drawmode = DRMODE_FG;
            lcd_set_viewport(&bv);
            lcd_putsxy(-k * MQ_SEG, cy, label);
            lcd_putsxy(l->mq_w + MQ_GAP - k * MQ_SEG, cy, label);
        }
        lcd_set_viewport(NULL);
        mq_tag.id = l->strip_id;
        mq_tag.sel = l->selected;
        mq_tag.w = l->mq_w;
    }
    lcd_set_viewport(NULL);
    /* the window [mq_x, mq_x + tw) from one or two segments */
    for (x = 0; x < tw && y1 > ty; x += n)
    {
        int sx = l->mq_x + x;
        k = sx / MQ_SEG;
        sx -= k * MQ_SEG;
        n = MIN(tw - x, MQ_SEG - sx);
        lcd_bitmap_part(buf + k * MQ_SEG * h, sx, ty,
                        STRIDE_MAIN(MQ_SEG, h),
                        rv.x + l->style.text_x + x, rv.y + ty, n, y1 - ty);
    }
    ui_strip_put();
    /* the text cell is the only fresh part: the bar over it alone */
    if (y1 > ty)
        draw_scrollbar(l, rv.x + l->style.text_x, rv.y + ty, tw, y1 - ty);
    ui_lcd_push("marquee", 0, rv.y, LCD_WIDTH, rv.height);
}

int ui_list_tick(struct ui_list *l)
{
    long now = current_tick;
    int wait = TIMEOUT_BLOCK;

    if (l->scroll.pos != l->scroll.target)
    {
        int dt;

        if (l->jump)
        {
            /* one whole-row frame per UI_LIST_JUMP_TICKS at most: the
             * moves in between are dropped, never queued. No boost is
             * held between jumps: each one is a frame of its own. */
            long due = l->last_frame + UI_LIST_JUMP_TICKS;
            if (TIME_BEFORE(now, due))
                return due - now;
            ui_anim_frame();
            l->last_frame = now;
            ui_scroll_snap(&l->scroll);
        }
        else
        {
            if (!ui_anim_frame_due())
                return UI_TICK_ANIM;
            ui_anim_frame();
            dt = now - l->last_frame;
            l->last_frame = now;
            ui_scroll_step(&l->scroll, dt > 0 ? dt : 1);
            /* a fast spin must not leave the selection off screen */
            ui_scroll_limit_lag(&l->scroll, view_h(l) - slot_h(l));
        }
        if (l->sbar)
            l->sbar_hide = now + SBAR_HOLD;
        glide_frame(l);
        if (l->scroll.pos != l->scroll.target)
            return UI_TICK_ANIM;
        marquee_reset(l);           /* settled: restart the delay */
        /* art for the rows the glide drew with a stand-in: next tick,
         * once the animation (and its boost) has ended */
        if (stubs_visible(l))
        {
            l->fill_due = true;
            return 1;
        }
    }

    if (l->fill_due)
    {
        /* never inside an animation (its boost, "anim start/end") */
        if (ui_anim_active())
            return 1;
        /* jumping: no art reads until the wheel has rested */
        if (l->jump && TIME_BEFORE(now, l->last_frame + UI_LIST_JUMP_SETTLE))
            return l->last_frame + UI_LIST_JUMP_SETTLE - now;
        l->jump = false;
        l->fill_due = false;
        fill_art(l);
    }

#ifdef SIMULATOR
    if (sbar_force > 0 && !l->sbar && long_list(l))
    {
        l->sbar = l->sbar_new = true;
        l->sbar_hide = now + SBAR_HOLD;
    }
    else if (sbar_force == 0)
    {
        sbar_force = -1;            /* forced off: hides now */
        l->sbar_hide = now;
    }
#endif

    if (l->sbar_new)
    {
        /* appeared over a list at rest: the rows under the thumb drawn
         * fresh and the thumb blended over them, one full-width band */
        int x, y, w, h;
        l->sbar_new = false;
        if (sbar_thumb(l, &x, &y, &w, &h))
        {
            draw_band(l, y, h);
            ui_lcd_push("sbar", 0, y, LCD_WIDTH, h);
        }
    }

    if (l->sbar)
    {
        if (sbar_force > 0)
            ;                       /* held on (the sim's forced bar) */
        else if (TIME_BEFORE(now, l->sbar_hide))
            wait = l->sbar_hide - now;
        else
        {
            /* the rows under the thumb drawn again without it: one
             * full-width band over the thumb's height (the text never
             * moved, so nothing else changes) */
            int x, y, w, h;
            bool had = sbar_thumb(l, &x, &y, &w, &h);
            l->sbar = false;
            if (had)
            {
                draw_band(l, y, h);
                ui_lcd_push("sbar", 0, y, LCD_WIDTH, h);
            }
        }
    }

    if (l->mq_w && !is_backlight_on(true))
    {
        /* nobody sees it: park at the start, no wakeups */
        if (l->mq_x)
        {
            l->mq_x = 0;
            redraw_selected(l);
        }
        l->mq_dark = true;
        return wait;
    }
    if (l->mq_dark)
    {
        l->mq_dark = false;
        marquee_reset(l);           /* lit again: the full delay first */
    }

    if (l->mq_w)
    {
        if (!TIME_BEFORE(now, l->mq_next))
        {
            l->mq_x += MQ_STEP_PX;
            l->mq_next = now + MQ_STEP_TICKS;
            if (l->mq_x >= l->mq_w + MQ_GAP)
            {
                l->mq_x = 0;        /* a full lap: rest at the start */
                l->mq_next = now + MQ_PAUSE;
            }
            mq_step(l);
        }
        wait = min_wait(wait, l->mq_next - now);
    }
    return wait;
}

#ifdef SIMULATOR
/* uisimulator/common/sim_cmd_fit3a.c: "list_force_bar on|off". Any
 * thread; the UI thread acts at its next pass (woken here). */
void ui_list_force_bar(bool on)
{
    sbar_force = on ? 1 : 0;
    button_queue_post(UI_LIST_SIM_WAKE, 0);
}
#endif
