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

/* The first-start battery question (one-build brief WP3; owner's pick of
 * the 2026-11-11 mockups: the centred drum, "V6"). Header "Battery" on
 * the dark body, the drum (drum.c) with Stock, 2,000 mAh and 3,000 mAh,
 * the current value in the blue box and its neighbours peeking above and
 * below; it stops at the ends. A grey footnote under it says what the
 * answer does. Pushed by battcell_ask() (battcell.h) under the legal
 * notice, as a system screen.
 *
 * Keys: the wheel turns the drum; Select (on release, not in the first
 * half second, so a double press on the notice before it answers
 * nothing) stores the answer and leaves. Every other button, MENU
 * included, does nothing: like the legal notice, the only way out is an
 * answer. A power-off, an alarm or a USB session before Select stores
 * nothing, so it is asked again at the next start. */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/screen_stack.h"
#include "ipodui/paint.h"
#include "ipodui/drum.h"
#include "ipodui/header.h"
#include "ipodui/usbscreen.h"
#include "ipodui/battcurve.h"
#include "ipodui/battcell.h"
#include "ipodui/screens/screens.h"

#define BQ_DRUM_Y       78              /* the strip's top, LCD rows */
#define BQ_NOTE_Y       212             /* the footnote's first baseline */
#define BQ_NOTE_PITCH   15
#define BQ_NOTE_FG      LCD_RGBPACK(0xc0, 0xc4, 0xcc)
#define BQ_GUARD_TICKS  (HZ / 2)        /* Select ignored after showing */

/* the drum's rows, in order; row i answers cell i + 1 (battcurve.h) */
static const short bq_rows[3] = {
    LANG_IPODUI_BATTQ_STOCK, LANG_IPODUI_BATTQ_V2000, LANG_IPODUI_BATTQ_V3000
};

static struct ui_drum bq_drum;          /* 40 B; one question at a time */
static long bq_shown;                   /* first frame's tick, 0: not yet */

static const char *bq_label(int idx, void *data)
{
    (void)data;
    return str(bq_rows[idx]);
}

/* greedy centred word wrap in the current font, at most 'max' lines */
static void bq_note_centre(int y_base, int pitch, int max_w, int max,
                           const char *s)
{
    char line[96];
    int w, h, ln = 0;

    while (*s && ln < max)
    {
        const char *p = s, *brk = NULL;
        int n;

        for (;;)
        {
            while (*p && *p != ' ')
                p++;
            n = MIN((int)(p - s), (int)sizeof(line) - 1);
            memcpy(line, s, n);
            line[n] = 0;
            font_getstringsize((unsigned char *)line, &w, &h,
                               lcd_current_viewport->font);
            if (w > max_w && brk)
                break;
            brk = p;
            if (!*p)
                break;
            p++;
        }
        n = MIN((int)(brk - s), (int)sizeof(line) - 1);
        memcpy(line, s, n);
        line[n] = 0;
        ui_paint_text_centre(LCD_WIDTH / 2, y_base + ln * pitch, max_w, line);
        ln++;
        s = brk;
        while (*s == ' ')
            s++;
    }
}

static void bq_note(void)
{
    struct viewport vp;

    memset(&vp, 0, sizeof(vp));
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.drawmode = DRMODE_FG;
    vp.font = ui_theme->font_small;
    lcd_set_viewport(&vp);
    lcd_set_foreground(BQ_NOTE_FG);
    bq_note_centre(BQ_NOTE_Y, BQ_NOTE_PITCH, LCD_WIDTH - 40, 2,
                   str(LANG_IPODUI_BATTQ_NOTE));
    lcd_set_viewport(NULL);
}

static int bq_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
    s->title = str(LANG_IPODUI_BATTQ_TITLE);
    s->header_w = UI_HEADER_DARK;
    bq_shown = 0;
    ui_drum_init(&bq_drum, BQ_DRUM_Y, 3, 0, bq_label, NULL);
    return 0;
}

static void bq_draw(struct ui_screen *s, unsigned dirty)
{
    (void)s;
    if (!bq_shown)
        bq_shown = current_tick | 1;    /* drawn on top: the guard starts */
    if (dirty & UI_DIRTY_BODY)
    {
        ui_dark_body(UI_DARK_HDR_H, LCD_HEIGHT - UI_DARK_HDR_H);
        bq_note();
    }
    if (dirty & (UI_DIRTY_BODY | UI_DIRTY_BAND))
        ui_drum_draw(&bq_drum);
}

static enum ui_result bq_input(struct ui_screen *s, const struct ui_input *in)
{
    (void)s;
    switch (in->key)
    {
        case UI_KEY_WHEEL:
            (void)ui_drum_input(&bq_drum, in);  /* tick() draws the slide */
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            if (!bq_shown || TIME_BEFORE(current_tick,
                                         bq_shown + BQ_GUARD_TICKS))
            {
                UPDATE_TRACE("battq select ignored (guard)%s", "");
                return UI_HANDLED;
            }
            battcell_answer(bq_drum.sel + BATTCELL_STOCK);
            return UI_POP;
        default:
            return UI_HANDLED;          /* MENU, PLAY, the skips: nothing */
    }
}

static int bq_tick(struct ui_screen *s)
{
    (void)s;
    return ui_drum_tick(&bq_drum);
}

const struct ui_screen_ops ui_battq_screen = {
    .name    = "battq",
    .enter   = bq_enter,
    .leave   = NULL,
    .draw    = bq_draw,
    .input   = bq_input,
    .timeout = NULL,
    .tick    = bq_tick,
};
