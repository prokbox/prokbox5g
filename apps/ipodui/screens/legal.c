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

/* Settings > Legal (docs/phase11-brief.md section 3.6; docs/legal.md
 * section 2): a light, full-width text page under the normal header. It
 * opens as the first-boot notice does: the heading "This is not Apple
 * software" (LANG_IPODUI_NOTICE_HEAD, Bold 15) and the notice's first
 * paragraph (LANG_IPODUI_NOTICE_1 with the version string); then
 * LANG_IPODUI_LEGAL_1 .. _6 (the GPL notice of the base, the two type
 * licences, then the independence and trademark paragraphs of the long
 * disclaimer). Word-wrapped once at enter into a line table in the
 * screen arena; the wheel scrolls it a line per step. MENU returns.
 *
 * Notice mode (arg UI_LEGAL_NOTICE, notice.h): the first-boot notice. A
 * headerless light page, every line centred, the block centred on the
 * screen: the heading, then LANG_IPODUI_NOTICE_1 .. _6 (docs/legal.md
 * section 2, verbatim; _1 takes LANG_IPODUI_VERSION_STRING, "ProkBox5g
 * <ver>", and _5 LANG_IPODUI_NOTICE_URL), the last one, "Press Select to
 * continue.", in the dim grey. Only Select dismisses it, and not in its
 * first 3 s (a habitual click can't skip it unread; the last line counts
 * the seconds down, LANG_IPODUI_NOTICE_WAIT): ui_notice_ack(),
 * then the pop. Every other button does nothing (notice.c keeps the
 * global holds off it); the wheel scrolls only if the text overflows (a
 * longer translation).
 *
 * A scroll step redraws the text area (rows below the header) as one
 * band: ~14 short lines of Regular 12, about 1.5 M instructions (~20 ms
 * at 80 MHz) on a page that is read, not browsed (the written reason for
 * a full redraw, lean rule 4). */

/* #106: a module tenant defines its real screen names (the core sees the
 * stand-ins, modload.h); the target's module build passes this itself */
#ifndef IPODUI_IN_MODULE
#define IPODUI_IN_MODULE
#endif
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/header.h"
#include "ipodui/modclass.h"
#include "ipodui/notice.h"
#include "ipodui/screen_stack.h"
#include "ipodui/screens/screens.h"

#define PARAS_MAX   8               /* Legal: heading, lead, LEGAL_1.._6 */
#define NOTICE_PARAS 7              /* heading, NOTICE_1 .. _6 */
#define MARGIN      10
#define TEXT_W      (LCD_WIDTH - 2 * MARGIN)
#define LINE_PITCH  15
#define HEAD_PITCH  19              /* the Bold 15 heading line */
#define PARA_GAP    7
#define N_PITCH     14              /* notice: a little tighter, so the */
#define N_GAP       5               /* English text fits one screen */
#define TOP_PAD     8
#define MAX_LINES   64
#define LINE_MAX_B  120             /* bytes in one wrapped line */
#define LEAD_B      256             /* NOTICE_1 with the version */
#define URL_LINE_B  96              /* notice: "Licence ...: <url>" */
#define GUARD_TICKS (3 * HZ)        /* notice: Select ignored this long */

struct legal_line {
    uint16_t off;                   /* into the paragraph's string */
    uint8_t  len;
    uint8_t  para;                  /* bit 7: first line of a paragraph */
};

struct legal {
    int lines, top;                 /* wrapped lines; first one shown */
    int rows;                       /* lines that fit below the header */
    int y0;                         /* the first line's top */
    int paras;
    bool notice;
    long shown;                     /* notice: current_tick at enter */
    int wait_s;                     /* notice: countdown shown, 0 = done */
    int sel_y;                      /* notice: the Select line's top */
    const char *para[PARAS_MAX];
    char lead[LEAD_B];              /* NOTICE_1 with the version string */
    char url_line[URL_LINE_B];      /* notice: NOTICE_5 with the URL */
    struct legal_line line[MAX_LINES];
};
_Static_assert(sizeof(struct legal) <= UI_ARENA_B_LEGAL, "legal: arena bound");

static int para_font(const struct legal *g, int p)
{
    (void)g;
    return p == 0 ? ui_theme->font_ui : ui_theme->font_small;
}

static int line_pitch(const struct legal *g, int p)
{
    return p == 0 ? HEAD_PITCH : g->notice ? N_PITCH : LINE_PITCH;
}

static int para_gap(const struct legal *g)
{
    return g->notice ? N_GAP : PARA_GAP;
}

static int text_w(const char *s, int len, int font)
{
    char buf[LINE_MAX_B + 1];
    int w, h;

    len = MIN(len, LINE_MAX_B);
    memcpy(buf, s, len);
    buf[len] = '\0';
    font_getstringsize((const unsigned char *)buf, &w, &h, font);
    return w;
}

/* The paragraphs of either mode, in order (docs/legal.md section 2) */
static void set_paras(struct legal *g)
{
    int i, n = 0;

    snprintf(g->lead, sizeof(g->lead), str(LANG_IPODUI_NOTICE_1),
             str(LANG_IPODUI_VERSION_STRING));
    g->para[n++] = str(LANG_IPODUI_NOTICE_HEAD);
    g->para[n++] = g->lead;
    if (g->notice)
    {
        snprintf(g->url_line, sizeof(g->url_line), str(LANG_IPODUI_NOTICE_5),
                 str(LANG_IPODUI_NOTICE_URL));
        g->para[n++] = str(LANG_IPODUI_NOTICE_2);
        g->para[n++] = str(LANG_IPODUI_NOTICE_3);
        g->para[n++] = str(LANG_IPODUI_NOTICE_4);
        g->para[n++] = g->url_line;
        g->para[n++] = str(LANG_IPODUI_NOTICE_6);
    }
    else
        for (i = 0; i < 6; i++)
            g->para[n++] = str(LANG_IPODUI_LEGAL_1 + i);
    g->paras = n;
}

/* Greedy word wrap of every paragraph into the line table */
static void wrap_all(struct legal *g)
{
    int p;

    g->lines = 0;
    for (p = 0; p < g->paras && g->lines < MAX_LINES; p++)
    {
        const char *s = g->para[p];
        int font = para_font(g, p);
        int start = 0, n = strlen(s);
        bool first = true;

        while (start < n && g->lines < MAX_LINES)
        {
            int end = start, fit = -1;

            /* extend word by word while the line fits */
            while (end < n)
            {
                int next = end;
                while (next < n && s[next] == ' ')
                    next++;
                while (next < n && s[next] != ' ')
                    next++;
                if (next - start > LINE_MAX_B
                    || text_w(s + start, next - start, font) > TEXT_W)
                    break;
                fit = end = next;
            }
            if (fit < 0)                /* one word wider than the line */
                end = MIN(n, start + LINE_MAX_B);
            g->line[g->lines].off = start;
            g->line[g->lines].len = end - start;
            g->line[g->lines].para = p | (first ? 0x80 : 0);
            g->lines++;
            first = false;
            start = end;
            while (start < n && s[start] == ' ')
                start++;
        }
    }
}

/* The height of all the lines as drawn, gaps included */
static int block_h(const struct legal *g)
{
    int i, h = 0;

    for (i = 0; i < g->lines; i++)
    {
        if ((g->line[i].para & 0x80) && i)
            h += para_gap(g);
        h += line_pitch(g, g->line[i].para & 0x7f);
    }
    return h;
}

static int legal_enter(struct ui_screen *s, const void *arg)
{
    const struct ui_theme *t = ui_theme;
    struct legal *g = ui_arena_alloc(sizeof(*g));
    int area;

    if (!g)
        return -1;
    s->priv = g;
    s->title = str(LANG_IPODUI_LEGAL);
    g->notice = arg == UI_LEGAL_NOTICE;
    if (g->notice)
        s->header_w = UI_HEADER_NONE;
    g->shown = current_tick;
    g->wait_s = g->notice ? GUARD_TICKS / HZ : 0;
    set_paras(g);
    wrap_all(g);
    area = LCD_HEIGHT - (g->notice ? 0 : t->header_h);
    g->rows = (area - TOP_PAD) / LINE_PITCH;
    g->top = 0;
    g->y0 = TOP_PAD;
    if (g->notice && block_h(g) <= area - 2 * TOP_PAD)
        g->y0 = (area - block_h(g)) / 2;
    UPDATE_TRACE("legal lines=%d notice=%d h=%d", g->lines, (int)g->notice,
                 block_h(g));
    return 0;
}

static void legal_draw(struct ui_screen *s, unsigned dirty)
{
    const struct ui_theme *t = ui_theme;
    struct legal *g = s->priv;
    struct viewport vp;
    int i, y;

    if (!(dirty & (UI_DIRTY_BODY | UI_DIRTY_BAND)))
        return;
    memset(&vp, 0, sizeof(vp));
    vp.y = g->notice ? 0 : t->header_h;
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT - vp.y;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.font = t->font_small;
    vp.drawmode = DRMODE_SOLID;
    lcd_set_viewport(&vp);
    lcd_set_foreground(t->list_bg);
    lcd_fillrect(0, 0, vp.width, vp.height);
    lcd_set_drawmode(DRMODE_FG);
    y = g->y0;
    for (i = g->top; i < g->lines && y < vp.height; i++)
    {
        const struct legal_line *l = &g->line[i];
        int p = l->para & 0x7f, x = MARGIN, len = l->len;
        char buf[LINE_MAX_B + 1];

        if ((l->para & 0x80) && i != g->top)
            y += para_gap(g);
        memcpy(buf, g->para[p] + l->off, len);
        buf[len] = '\0';
        vp.font = para_font(g, p);
        if (g->notice && p == g->paras - 1)
        {
            /* the Select line counts down while Select is held off */
            g->sel_y = y;
            if (g->wait_s > 0)
                len = MIN(snprintf(buf, sizeof(buf),
                                   str(LANG_IPODUI_NOTICE_WAIT), g->wait_s),
                          (int)sizeof(buf) - 1);
        }
        lcd_set_foreground(g->notice && p == g->paras - 1 ? t->list_fg_dim
                                                         : t->list_fg);
        if (g->notice)
            x = (LCD_WIDTH - text_w(buf, len, vp.font)) / 2;
        lcd_putsxy(x, y, (const unsigned char *)buf);
        y += line_pitch(g, p);
    }
    lcd_set_viewport(NULL);
}

/* The notice's countdown: redraw only the Select line, once a second */
static int legal_tick(struct ui_screen *s)
{
    struct legal *g = s->priv;
    long left;
    int secs;

    if (!g->notice || g->wait_s == 0)
        return TIMEOUT_BLOCK;
    left = g->shown + GUARD_TICKS - current_tick;
    secs = left > 0 ? (int)((left + HZ - 1) / HZ) : 0;
    if (secs != g->wait_s)
    {
        g->wait_s = secs;
        ui_invalidate_band(g->sel_y, N_PITCH + 2);
    }
    return secs ? (int)(left - (secs - 1) * HZ) : TIMEOUT_BLOCK;
}

static enum ui_result legal_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    const struct ui_theme *t = ui_theme;
    struct legal *g = s->priv;
    int top, y = g->notice ? 0 : t->header_h;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            if (g->notice && g->y0 > TOP_PAD)
                return UI_HANDLED;      /* it all fits: nothing to scroll */
            top = MIN(MAX(g->top + in->wheel_steps, 0),
                      MAX(g->lines - g->rows + g->paras / 2, 0));
            top = MIN(top, MAX(g->lines - 1, 0));
            if (top != g->top)
            {
                g->top = top;
                ui_invalidate_band(y, LCD_HEIGHT - y);
            }
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (!g->notice || !in->release || in->long_press)
                return UI_HANDLED;
            if (TIME_BEFORE(current_tick, g->shown + GUARD_TICKS))
            {
                UPDATE_TRACE("notice select ignored (guard)%s", "");
                return UI_HANDLED;
            }
            ui_notice_ack();            /* the one write, deferred */
            /* the visit ends here: the Main Menu's first panel read, in
             * this same pass, is not this module's (modclass.h) */
            modclass_top(-1);
            return UI_POP;
        default:
            /* the notice: every other button does nothing; Legal: MENU
             * returns, PLAY and the skips as anywhere */
            return g->notice ? UI_HANDLED : UI_IGNORED;
    }
}

const struct ui_screen_ops ui_legal_screen = {
    .name    = "legal",
    .enter   = legal_enter,
    .leave   = NULL,                    /* arena: released by the pop */
    .draw    = legal_draw,
    .input   = legal_input,
    .timeout = NULL,
    .tick    = legal_tick,
};
