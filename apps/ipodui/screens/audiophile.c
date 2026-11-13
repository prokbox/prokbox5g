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

/* Settings > Audiophile (#270): the High Sample Rate prompt (the warning,
 * the self-test, its result) and the fallback popup.
 *
 * The Audiophile menu itself is in screens/settings.c (its rows share the
 * Settings tiles and the split value list). This file holds:
 *
 *  - ui_ap_hirate_screen: the light full-screen prompt (setwidgets.h)
 *    shown the first time High Sample Rate is turned on (no AP_TESTED in
 *    ipodui_hirate_ok), or again when no rate passed before. Continue |
 *    Cancel, Cancel preselected (Reset's rule for a risky choice).
 *    Continue shows "Testing..." in full, then the next pass's tick runs
 *    audiophile_selftest() (~2 s, blocking; it stops and resumes music
 *    itself), then the result with one OK; the setting follows the
 *    result (Off when neither rate passed). Keys pressed during the test
 *    are dropped for half a second after it, so a queued press cannot
 *    dismiss the result unseen.
 *
 *  - the fallback popup: when audiophile_fallback_pending() (a runtime
 *    fault at a high rate moved playback to 44.1/48 kHz), the main loop
 *    pushes a small box over whatever screen is up (ui_ap_popup_poll(),
 *    lit, no slide). SELECT or MENU closes it with no slide; leaving it by
 *    any route counts as seen (audiophile_fallback_ack()). PLAY, PREV and
 *    NEXT keep their usual meaning; nothing here pauses playback.
 *
 * No allocation: the prompt's state is in s->priv; one static long holds
 * the test's end tick. */

#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "settings.h"
#include "audiophile.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/header.h"
#include "ipodui/anim.h"
#include "ipodui/alarm.h"
#include "ipodui/screens/setwidgets.h"
#include "ipodui/screens/screens.h"

/* -- the High Sample Rate prompt ------------------------------------------- */

enum { ST_WARN, ST_TESTING, ST_RESULT };
enum { SEG_CONTINUE, SEG_CANCEL, SEG_COUNT };

static const short warn_seg[SEG_COUNT] = {
    LANG_IPODUI_AP_CONTINUE, LANG_CANCEL_0 };
static const short ok_seg[1] = { LANG_IPODUI_OK };

/* s->priv: bits 0-7 the segment, 8-11 the state, 16-23 the test's mask */
#define P_SEG(v)    ((int)((v) & 0xff))
#define P_ST(v)     ((int)(((v) >> 8) & 0xf))
#define P_MASK(v)   ((int)(((v) >> 16) & 0xff))
#define P_MAKE(seg, st, mask) \
    ((void *)(intptr_t)((seg) | ((st) << 8) | ((mask) << 16)))

#define AP_QUIET_TICKS  (HZ / 2)    /* keys dropped after the test */

static long test_end;               /* current_tick when the test returned */

static intptr_t pv(const struct ui_screen *s)
{
    return (intptr_t)s->priv;
}

static int result_lang(int mask)
{
    int r = mask & (AP_RATE_88 | AP_RATE_96);

    return r == (AP_RATE_88 | AP_RATE_96) ? LANG_IPODUI_AP_RESULT_BOTH :
           r == AP_RATE_88 ? LANG_IPODUI_AP_RESULT_88 :
           r == AP_RATE_96 ? LANG_IPODUI_AP_RESULT_96 :
                             LANG_IPODUI_AP_RESULT_NONE;
}

static int hirate_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
    s->title = str(LANG_IPODUI_AP_HIRATE);
    s->priv = P_MAKE(SEG_CANCEL, ST_WARN, 0);   /* Cancel preselected */
    return 0;
}

static void hirate_draw(struct ui_screen *s, unsigned dirty)
{
    intptr_t v = pv(s);

    switch (P_ST(v))
    {
    case ST_WARN:
        if (dirty & UI_DIRTY_BODY)
        {
            ui_prompt_draw_n(LANG_IPODUI_AP_WARN_HEAD,
                             LANG_IPODUI_AP_WARN_BODY, 6);
            ui_seg_draw(warn_seg, SEG_COUNT, P_SEG(v), true);
        }
        else if (dirty & UI_DIRTY_BAND)
            ui_seg_draw(warn_seg, SEG_COUNT, P_SEG(v), false);
        break;
    case ST_TESTING:
        if (dirty & UI_DIRTY_BODY)
            ui_prompt_draw_n(LANG_IPODUI_AP_TESTING_HEAD,
                             LANG_IPODUI_AP_TESTING_BODY, 3);
        break;
    default:
        if (dirty & UI_DIRTY_BODY)
        {
            ui_prompt_draw_n(LANG_IPODUI_AP_RESULT_HEAD,
                             result_lang(P_MASK(v)), 4);
            ui_seg_draw(ok_seg, 1, 0, true);
        }
        break;
    }
}

static enum ui_result hirate_input(struct ui_screen *s,
                                   const struct ui_input *in)
{
    intptr_t v = pv(s);
    int seg = P_SEG(v), d = 0;

    if (P_ST(v) == ST_TESTING)
        return UI_HANDLED;              /* the test is about to run */
    if (P_ST(v) == ST_RESULT)
    {
        if (in->key != UI_KEY_SELECT && in->key != UI_KEY_MENU)
            return in->key == UI_KEY_WHEEL ? UI_HANDLED : UI_IGNORED;
        if (!in->release || in->long_press
            || TIME_BEFORE(current_tick, test_end + AP_QUIET_TICKS))
            return UI_HANDLED;
        return UI_POP;
    }
    switch (in->key)
    {
        case UI_KEY_WHEEL:
            d = in->wheel_steps;
            break;
        case UI_KEY_NEXT:
        case UI_KEY_PREV:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            d = in->key == UI_KEY_NEXT ? 1 : -1;
            break;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            if (seg != SEG_CONTINUE)
                return UI_POP;          /* Cancel: stays Off */
            s->priv = P_MAKE(0, ST_TESTING, 0);
            ui_invalidate(UI_DIRTY_BODY);   /* drawn, then the tick tests */
            return UI_HANDLED;
        default:
            return UI_IGNORED;          /* MENU: back, stays Off */
    }
    seg = MIN(MAX(seg + d, 0), SEG_COUNT - 1);
    if (seg != P_SEG(v))
    {
        s->priv = P_MAKE(seg, ST_WARN, 0);
        ui_invalidate_band(UI_SEG_Y, UI_SEG_H);
    }
    return UI_HANDLED;
}

/* "Testing..." is on the LCD (the main loop refreshes before the tick):
 * run the test, set the setting from it, show the result */
static int hirate_tick(struct ui_screen *s)
{
    int mask;

    if (P_ST(pv(s)) != ST_TESTING || (s->dirty & UI_DIRTY_BODY))
        return TIMEOUT_BLOCK;
    ui_anim_idle();                     /* no boost held through the wait */
    mask = audiophile_selftest();
    test_end = current_tick;
    audiophile_set_hirate((mask & (AP_RATE_88 | AP_RATE_96)) != 0);
    UPDATE_TRACE("audiophile test mask=0x%02x on=%d", mask,
                 (int)global_settings.ipodui_hirate);
    s->priv = P_MAKE(0, ST_RESULT, mask & 0xff);
    ui_invalidate(UI_DIRTY_BODY);
    return TIMEOUT_BLOCK;
}

const struct ui_screen_ops ui_ap_hirate_screen = {
    .name    = "ap-hirate",
    .enter   = hirate_enter,
    .leave   = NULL,
    .draw    = hirate_draw,
    .input   = hirate_input,
    .timeout = NULL,
    .tick    = hirate_tick,
};

/* -- the fallback popup ------------------------------------------------------ */

/* A light box in the middle of the screen, the prompt shelf's colours:
 * the heading, the body (up to 3 lines) and one OK, the selection blue */
#define PU_W            272
#define PU_H            132
#define PU_X            ((LCD_WIDTH - PU_W) / 2)
#define PU_Y            ((LCD_HEIGHT - PU_H) / 2 + 6)
#define PU_PAD          14
#define PU_HEAD_BASE    (PU_Y + 24)
#define PU_BODY_BASE    (PU_Y + 44)
#define PU_BODY_PITCH   15
#define PU_OK_W         70
#define PU_OK_H         24
#define PU_OK_Y         (PU_Y + PU_H - PU_OK_H - 10)
#define PU_BORDER       LCD_RGBPACK(0x5a, 0x5a, 0x62)
#define PU_HEAD_FG      LCD_RGBPACK(0x00, 0x00, 0x00)
#define PU_BODY_FG      LCD_RGBPACK(0x3c, 0x3c, 0x3c)

static const struct ui_gloss pu_gloss = {
    .top    = LCD_RGBPACK(0xf6, 0xf6, 0xf8),
    .bot    = LCD_RGBPACK(0xd6, 0xd6, 0xdc),
    .line   = LCD_RGBPACK(0xff, 0xff, 0xff),
    .edge   = LCD_RGBPACK(0xa8, 0xa8, 0xb0),
    .hi_top = 0,
    .hi_mid = 0,
    .split  = 0,
    .flags  = 0,
};

/* the rounded corners' inset per row from the top (radius 5); the pixels
 * outside them keep the screen beneath */
static const signed char pu_cut[] = { 4, 2, 1, 1, 0 };

static int pu_inset(int i)
{
    int r = (int)sizeof(pu_cut);

    if (i < r)
        return pu_cut[i];
    if (i >= PU_H - r)
        return pu_cut[PU_H - 1 - i];
    return 0;
}

static void popup_draw(struct ui_screen *s, unsigned dirty)
{
    const struct ui_theme *t = ui_theme;
    int i, w, h;
    const char *ok = str(LANG_IPODUI_OK);

    (void)s;
    if (!(dirty & UI_DIRTY_BODY))
        return;
    lcd_set_viewport(NULL);
    lcd_set_drawmode(DRMODE_SOLID);
    for (i = 0; i < PU_H; i++)
    {
        int c = pu_inset(i);

        /* the border: the first and last rows, then a pixel each side */
        if (i == 0 || i == PU_H - 1)
            lcd_set_foreground(PU_BORDER);
        else
            lcd_set_foreground(ui_gloss_color(&pu_gloss, i, PU_H));
        lcd_hline(PU_X + c, PU_X + PU_W - 1 - c, PU_Y + i);
        if (i > 0 && i < PU_H - 1)
        {
            /* a pixel each side, run out to the neighbour row's edge on
             * the corners so the outline has no gaps */
            int n = pu_inset(i < PU_H / 2 ? i - 1 : i + 1);
            int e = MAX(c, n - 1);

            lcd_set_foreground(PU_BORDER);
            lcd_hline(PU_X + c, PU_X + e, PU_Y + i);
            lcd_hline(PU_X + PU_W - 1 - e, PU_X + PU_W - 1 - c, PU_Y + i);
        }
    }
    lcd_set_drawmode(DRMODE_FG);
    lcd_setfont(t->font_ui);
    lcd_set_foreground(PU_HEAD_FG);
    lcd_putsxy(PU_X + PU_PAD, PU_HEAD_BASE - font_get(t->font_ui)->ascent,
               (const unsigned char *)str(LANG_IPODUI_AP_FALLBACK_HEAD));
    lcd_setfont(t->font_small);
    lcd_set_foreground(PU_BODY_FG);
    ui_wrap_text(PU_X + PU_PAD, PU_BODY_BASE, PU_BODY_PITCH,
                 PU_W - 2 * PU_PAD, 3, str(LANG_IPODUI_AP_FALLBACK_BODY));
    /* OK, the selection blue, centred at the foot */
    lcd_set_drawmode(DRMODE_SOLID);
    ui_paint_selection((LCD_WIDTH - PU_OK_W) / 2, PU_OK_Y, PU_OK_W, PU_OK_H);
    lcd_setfont(t->font_title);
    lcd_getstringsize((const unsigned char *)ok, &w, &h);
    lcd_set_drawmode(DRMODE_FG);
    lcd_set_foreground(LCD_WHITE);
    lcd_putsxy((LCD_WIDTH - w) / 2, PU_OK_Y + (PU_OK_H - h) / 2,
               (const unsigned char *)ok);
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_setfont(FONT_UI);
}

static int popup_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
    s->header_w = UI_HEADER_NONE;       /* the box only; ops.overlay */
    UPDATE_TRACE("audiophile popup shown%s", "");
    return 0;
}

/* any way out (SELECT, MENU, a held MENU to the root) counts as seen */
static void popup_leave(struct ui_screen *s)
{
    (void)s;
    audiophile_fallback_ack();
    UPDATE_TRACE("audiophile popup acked%s", "");
}

static enum ui_result popup_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    switch (in->key)
    {
        case UI_KEY_SELECT:
        case UI_KEY_MENU:
            if (in->release && !in->long_press)
                ui_pop_request_cut(s);  /* the screen beneath, no slide */
            return UI_HANDLED;
        case UI_KEY_WHEEL:
            return UI_HANDLED;
        default:
            return UI_IGNORED;          /* PLAY, PREV, NEXT as ever */
    }
}

static const struct ui_screen_ops popup_screen = {
    .name    = "ap-popup",
    .enter   = popup_enter,
    .leave   = popup_leave,
    .draw    = popup_draw,
    .input   = popup_input,
    .timeout = NULL,
    .overlay = true,
};

bool ui_ap_popup_poll(void)
{
    struct ui_screen *top;

    if (!audiophile_fallback_pending())
        return false;                   /* one uncached word: the usual */
    top = ui_top();
    if (!top || top->ops == &popup_screen || alarm_ringing())
        return false;
    if (ui_push_t(&popup_screen, NULL, UI_T_SYSTEM) < 0)
        return false;                   /* stack full: next pass */
    (void)ui_take_transition();         /* up at once, over the screen */
    return true;
}
