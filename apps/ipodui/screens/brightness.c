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

/* Settings > Brightness (docs/phase11-brief.md section 3.6; spec
 * settings.md "Brightness"; owner photo brightness.jpg): a dark
 * full-screen screen, header "Brightness" centred; a dark rounded slider
 * box with a crescent moon at the left, a thin capsule track and a small
 * sun at the right; the percentage centred below. The wheel sets the
 * backlight live (backlight_set_brightness()); MENU or SELECT saves and
 * returns. Each step pushes two bands (the track and the number). The
 * level lives in s->priv: nothing allocates. Geometry from the photo
 * (~, +-2 px); the moon and sun are our own drawings (art/sys). */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "lang.h"
#include "settings.h"
#include "ipodui/fe_be.h"   /* P19 S1: settings saves cross */
#include "backlight.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/header.h"
#include "ipodui/usbscreen.h"
#include "ipodui/sysscreen.h"
#include "ipodui/screens/screens.h"

#ifdef HAVE_BACKLIGHT_BRIGHTNESS
#define B_MIN   MIN_BRIGHTNESS_SETTING
#define B_MAX   MAX_BRIGHTNESS_SETTING
#else
#define B_MIN   0
#define B_MAX   0
#endif

#define BOX_X       23
#define BOX_W       275         /* x 23..297 */
#define BOX_Y       114
#define BOX_H       42          /* y 114..155 */
#define TRACK_X     72
#define TRACK_W     178         /* x 72..249 */
#define TRACK_H     5
#define TRACK_Y     (BOX_Y + (BOX_H - TRACK_H) / 2)
#define MOON_X      44
#define SUN_X       263
#define PCT_BASE    213         /* "0%" centred, centre ~y 208 */
#define PCT_Y       196         /* its band */
#define PCT_H       22

#define C_BOX_TOP   LCD_RGBPACK(0x1c, 0x1f, 0x24)
#define C_BOX_BOT   LCD_RGBPACK(0x05, 0x06, 0x08)
#define C_BOX_EDGE  LCD_RGBPACK(0x3c, 0x41, 0x48)
#define C_TRACK     LCD_RGBPACK(0x4a, 0x4e, 0x56)
#define C_FILL_TOP  LCD_RGBPACK(0xf4, 0xf6, 0xfa)
#define C_FILL_BOT  LCD_RGBPACK(0xa8, 0xae, 0xb8)
#define C_ICON      LCD_RGBPACK(0xf0, 0xf2, 0xf6)

static int level(const struct ui_screen *s)
{
    return (int)(intptr_t)s->priv;
}

static int pct_of(int b)
{
    return B_MAX > B_MIN ? (b - B_MIN) * 100 / (B_MAX - B_MIN) : 100;
}

/* a row of a capsule h px tall: the inset at each end for row j */
static int cap_inset(int j, int h)
{
    int r = h / 2, d = j < r ? r - j : (j >= h - r ? j - (h - 1 - r) : 0);
    return d >= r ? r : (d > 0 ? (d * d + r - 1) / (2 * r) : 0);
}

static void draw_slider(int b)
{
    int j, fill = B_MAX > B_MIN ? (b - B_MIN) * TRACK_W / (B_MAX - B_MIN) : 0;

    lcd_set_viewport(NULL);
    /* the box: a dark gradient, a lighter top edge, corners cut by 2/1 px */
    for (j = 0; j < BOX_H; j++)
    {
        int in = j == 0 || j == BOX_H - 1 ? 2 : (j == 1 || j == BOX_H - 2);
        lcd_set_foreground(j == 0 ? C_BOX_EDGE
                           : ui_paint_blend(C_BOX_TOP, C_BOX_BOT,
                                            j * 15 / BOX_H));
        lcd_hline(BOX_X + in, BOX_X + BOX_W - 1 - in, BOX_Y + j);
    }
    /* the track: dark capsule, the level filled light from the left */
    for (j = 0; j < TRACK_H; j++)
    {
        int in = cap_inset(j, TRACK_H);
        lcd_set_foreground(C_TRACK);
        lcd_hline(TRACK_X + in, TRACK_X + TRACK_W - 1 - in, TRACK_Y + j);
        if (fill > 2 * in)
        {
            lcd_set_foreground(ui_paint_blend(C_FILL_TOP, C_FILL_BOT,
                                              j * 15 / TRACK_H));
            lcd_hline(TRACK_X + in, TRACK_X + fill - 1 - in, TRACK_Y + j);
        }
    }
    ui_sys_glyph(UI_SYS_MOON, MOON_X, BOX_Y + (BOX_H - ui_sys_glyph_h(UI_SYS_MOON)) / 2,
                 C_ICON);
    ui_sys_glyph(UI_SYS_SUN, SUN_X, BOX_Y + (BOX_H - ui_sys_glyph_h(UI_SYS_SUN)) / 2,
                 C_ICON);
}

static void draw_pct(int b)
{
    struct viewport vp;
    char buf[16];

    memset(&vp, 0, sizeof(vp));
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.drawmode = DRMODE_FG;
    vp.font = ui_theme->font_ui;
    lcd_set_viewport(&vp);
    lcd_set_foreground(LCD_WHITE);
    snprintf(buf, sizeof(buf), str(LANG_IPODUI_PERCENT), pct_of(b));
    ui_paint_text_centre(LCD_WIDTH / 2, PCT_BASE, LCD_WIDTH, buf);
    lcd_set_viewport(NULL);
}

static int bright_enter(struct ui_screen *s, const void *arg)
{
    int b = B_MIN;

    (void)arg;
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    b = MIN(MAX(global_settings.brightness, B_MIN), B_MAX);
#endif
    s->priv = (void *)(intptr_t)b;
    s->title = str(LANG_IPODUI_BRIGHTNESS);
    s->header_w = UI_HEADER_DARK;
    return 0;
}

static void bright_draw(struct ui_screen *s, unsigned dirty)
{
    if (dirty & UI_DIRTY_BODY)
        ui_dark_body(UI_DARK_HDR_H, LCD_HEIGHT - UI_DARK_HDR_H);
    else if (dirty & UI_DIRTY_BAND)
    {
        ui_dark_body(BOX_Y, BOX_H);
        ui_dark_body(PCT_Y, PCT_H);
    }
    else
        return;
    draw_slider(level(s));
    draw_pct(level(s));
}

static void bright_leave(struct ui_screen *s)
{
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
    if (global_settings.brightness != level(s))
    {
        global_settings.brightness = level(s);
        fe_settings_save();
    }
#else
    (void)s;
#endif
}

static enum ui_result bright_input(struct ui_screen *s,
                                   const struct ui_input *in)
{
    int b;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            b = MIN(MAX(level(s) + in->wheel_steps, B_MIN), B_MAX);
            if (b == level(s))
                return UI_HANDLED;
            s->priv = (void *)(intptr_t)b;
#ifdef HAVE_BACKLIGHT_BRIGHTNESS
            backlight_set_brightness(b);    /* live */
#endif
            UPDATE_TRACE("brightness %d pct=%d", b, pct_of(b));
            ui_invalidate_band(BOX_Y, BOX_H);
            ui_invalidate_band(PCT_Y, PCT_H);
            return UI_HANDLED;
        case UI_KEY_MENU:
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            return UI_POP;
        default:
            return UI_IGNORED;
    }
}

const struct ui_screen_ops ui_brightness_screen = {
    .name    = "brightness",
    .enter   = bright_enter,
    .leave   = bright_leave,       /* saves, also on hold MENU */
    .draw    = bright_draw,
    .input   = bright_input,
    .timeout = NULL,
};
