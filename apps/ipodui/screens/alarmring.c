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
 * iPod UI shell: the alarm's ring screen (P12 Q5, docs/phase12-brief.md
 * 3.3). Resident: it never waits for a file. The dark system look, the
 * label as the title, the alarm's time, and Snooze / Dismiss.
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
#include "lang.h"
#include "settings.h"
#include "ipodui/alarm_ring.h"
#include "ipodui/anim.h"
#include "ipodui/header.h"
#include "ipodui/paint.h"
#include "ipodui/theme.h"
#include "ipodui/usbscreen.h"

/* Layout (capture C3 not taken: our own, in the Charging screen's frame) */
#define RING_TIME_BASE  104             /* the time's baseline */
#define RING_ROW_H      36
#define RING_ROWS_Y     (LCD_HEIGHT - 2 * RING_ROW_H - 8)
#define RING_LABEL_N    10              /* LANG_IPODUI_X_LABEL_NONE.. */

/* resident and tiny: the ring must not depend on the arena having room */
static struct {
    signed char sel;                    /* 0 Snooze, 1 Dismiss */
    bool key_down;                      /* a press seen here: act at its
                                           release, never a release of a
                                           press made before the ring */
} rs;

static int rows_first(void)
{
    return alarm_ring_can_snooze() ? 0 : 1;
}

static const char *ring_title(void)
{
    int i = alarm_ring_index();
    const struct alarm *a = alarm_get(i);

    if (!a || a->label == 0 || a->label >= RING_LABEL_N)
        return str(LANG_IPODUI_X_ALARM);
    return str(LANG_IPODUI_X_LABEL_NONE + a->label);
}

static int ring_enter(struct ui_screen *s, const void *arg)
{
    (void)arg;
    s->header_w = UI_HEADER_DARK;
    s->title = ring_title();
    rs.sel = (signed char)rows_first();
    rs.key_down = false;
    return 0;
}

static void ring_leave(struct ui_screen *s)
{
    (void)s;
    if (alarm_ringing())
        alarm_ring_end(ALARM_END_DISMISS);  /* hold MENU, a pop to root */
}

static void ring_draw(struct ui_screen *s, unsigned dirty)
{
    struct viewport vp;
    char buf[16];
    int y, mo, d, h, m, sec, r;

    (void)s;
    if (!(dirty & (UI_DIRTY_BODY | UI_DIRTY_BAND)))
        return;
    ui_dark_body(UI_DARK_HDR_H, LCD_HEIGHT - UI_DARK_HDR_H);
    alarm_rtc_split(alarm_ring_rtc(), &y, &mo, &d, &h, &m, &sec);
    if (global_settings.timeformat)         /* 12 hour clock */
        snprintf(buf, sizeof(buf), "%d:%02d %s", h % 12 ? h % 12 : 12, m,
                 str(h < 12 ? LANG_IPODUI_AM : LANG_IPODUI_PM));
    else
        snprintf(buf, sizeof(buf), "%02d:%02d", h, m);

    memset(&vp, 0, sizeof(vp));
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.drawmode = DRMODE_FG;
    vp.font = ui_theme->font_ui;
    lcd_set_viewport(&vp);
    lcd_set_foreground(LCD_WHITE);
    ui_paint_text_centre(LCD_WIDTH / 2, RING_TIME_BASE, LCD_WIDTH - 20, buf);
    for (r = rows_first(); r < 2; r++)
    {
        int ry = RING_ROWS_Y + r * RING_ROW_H;
        if (r == rs.sel)
            ui_paint_selection(8, ry, LCD_WIDTH - 16, RING_ROW_H - 4);
        lcd_set_foreground(r == rs.sel ? LCD_WHITE : LCD_RGBPACK(200, 200,
                                                                 200));
        ui_paint_text_centre(LCD_WIDTH / 2, ry + RING_ROW_H / 2 + 4,
                             LCD_WIDTH - 40,
                             str(r ? LANG_IPODUI_X_DISMISS
                                   : LANG_IPODUI_X_SNOOZE));
    }
    lcd_set_viewport(NULL);
}

static enum ui_result ring_act(bool snooze)
{
    rs.key_down = false;
    alarm_ring_end(snooze && alarm_ring_can_snooze() ? ALARM_END_SNOOZE
                                                     : ALARM_END_DISMISS);
    return UI_POP;
}

static enum ui_result ring_input(struct ui_screen *s,
                                 const struct ui_input *in)
{
    (void)s;
    if (!alarm_ringing())
        return UI_POP;
    if (in->key == UI_KEY_WHEEL)
    {
        int sel = in->wheel_steps > 0 ? 1 : rows_first();
        if (sel != rs.sel)
        {
            rs.sel = (signed char)sel;
            ui_invalidate(UI_DIRTY_BODY);
        }
        return UI_HANDLED;
    }
    if (in->key == UI_KEY_NONE || in->long_press)
        return UI_HANDLED;              /* chords; hold PLAY: global_hold() */
    if (!in->release)
    {
        rs.key_down = true;
        return UI_HANDLED;
    }
    if (!rs.key_down)
        return UI_HANDLED;              /* pressed before the ring */
    switch (in->key)
    {
    case UI_KEY_SELECT:
        return ring_act(rs.sel == 0);
    case UI_KEY_PLAY:                   /* Q5: snooze, or dismiss if none */
        return ring_act(true);
    case UI_KEY_MENU:
        return ring_act(false);
    default:
        rs.key_down = false;
        return UI_HANDLED;
    }
}

static int ring_tick(struct ui_screen *s)
{
    if (!alarm_ringing())
    {
        UPDATE_TRACE("alarm ring screen pop%s", "");
        ui_pop_request(s);              /* ended elsewhere: USB key, cap */
        return TIMEOUT_BLOCK;
    }
    if (rs.sel < rows_first())
    {
        rs.sel = (signed char)rows_first();
        ui_invalidate(UI_DIRTY_BODY);
    }
    return TIMEOUT_BLOCK;               /* the beep is the engine's
                                           deadline (alarm_timeout()) */
}

const struct ui_screen_ops ui_alarm_ring_screen = {
    .name    = "alarm-ring",
    .enter   = ring_enter,
    .leave   = ring_leave,
    .draw    = ring_draw,
    .input   = ring_input,
    .tick    = ring_tick,
};

void alarm_ring_close(void)
{
    if (ui_top()->ops == &ui_alarm_ring_screen)
        ui_pop();
}
