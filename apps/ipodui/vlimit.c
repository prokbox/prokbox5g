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
 * iPod UI shell: Settings > Volume Limit (Phase 11)
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

/* The 7G's flow (G12 p.30-31, brief 3.5), on a dark screen:
 *
 *   no code set   the limit screen: the volume bar with a small triangle at
 *                 the limit. The wheel moves the limit, PLAY plays or
 *                 pauses the queue as a preview, SELECT sets it and goes to
 *                 Enter Combination: MENU accepts with no code, four
 *                 digits plus SELECT set the code.
 *   code set      Enter Combination first. A wrong code resets the digits
 *                 to 0000 with a double clicker tick and stays locked; the
 *                 right one opens the limit screen, where PLAY accepts.
 *   remove        move the limit to the maximum (the code goes with it).
 *
 * One screen with three phases (state in the screen arena); the phases
 * switch with a full redraw, no slide. Storage: upstream volume_limit
 * (dB, enforced by sound_set_volume()) and ipodui_vlimit_code (-1 none).
 * A forgotten combination is recovered only through config.cfg. */
/* not ownable (file): a full-screen page or takeover; the drift panel was settled on the way in (ui_panel_hide(), or the takeover's own ui_fb_settle()), and a full lcd_update() releases it (bcmjob_owned_hook) */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "logf.h"
#include "audio.h"
#include "sound.h"
#include "settings.h"
#include "ipodui/fe_be.h"   /* P19 S1: settings saves cross */
#include "lang.h"
#include "misc.h"
#include "button.h"
#include "ipodui/ipodui.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/anim.h"
#include "ipodui/np_draw.h"
#include "ipodui/vlimit.h"
#include "ipodui/volcurve.h"
#include "ipodui/roller.h"
#include "ipodui/header.h"
#include "ipodui/usbscreen.h"
#include "ipodui/sc_hw.h"
#include "ipodui/lock.h"

enum vl_phase { VL_UNLOCK, VL_LIMIT, VL_NEWCODE };

struct vl_state {
    struct ui_roller roller;
    int  limit;                 /* the limit being set (dB) */
    uint8_t phase;
    bool locked;                /* a code was set when the screen opened */
};
/* P11 screen arena bound (screen_stack.h) */
_Static_assert(sizeof(struct vl_state) <= UI_ARENA_B_SETTING,
               "vl_state: arena bound");

/* geometry (320 x 240); the bar as on Now Playing (np VOL_X/VOL_W) */
#define VL_BAR_X        56
#define VL_BAR_W        213
#define VL_BAR_Y        120
#define VL_TRI_W        11
#define VL_TRI_H        7
#define VL_BAND_Y       (VL_BAR_Y - 4)
#define VL_BAND_H       (NP_BAR_H + 4 + VL_TRI_H + 8)
#define VL_ROLLER_Y     104     /* the roller strip (photo: centre y ~ 125) */
#define VL_TEXT_FG      LCD_RGBPACK(0xee, 0xf1, 0xf6)

static const struct ui_gloss fill_gloss = {
    .top    = LCD_RGBPACK(0x7c, 0xc0, 0xf8),
    .bot    = LCD_RGBPACK(0x1a, 0x66, 0xd8),
    .line   = LCD_RGBPACK(0x2e, 0x66, 0xb4),
    .edge   = LCD_RGBPACK(0x0c, 0x3c, 0x8c),
    .hi_top = 70,
    .hi_mid = 16,
    .split  = 128,
    .flags  = UI_GLOSS_LINE | UI_GLOSS_EDGE,
};
static const struct ui_gloss track_gloss = {
    .top    = LCD_RGBPACK(0x0c, 0x0e, 0x11),
    .bot    = LCD_RGBPACK(0x3a, 0x40, 0x49),
    .line   = LCD_RGBPACK(0x05, 0x06, 0x08),
    .edge   = LCD_RGBPACK(0x5a, 0x61, 0x6b),
    .hi_top = 0,
    .hi_mid = 0,
    .split  = 0,
    .flags  = UI_GLOSS_LINE | UI_GLOSS_EDGE,
};

/* -- the pure bits ------------------------------------------------------- */

bool ui_vlimit_active(void)
{
    return global_settings.volume_limit < sound_max(SOUND_VOLUME);
}

bool ui_vlimit_locked(void)
{
    return global_settings.ipodui_vlimit_code >= 0;
}

/* Clamps the output at once (upstream clamps only when a volume is next
 * set; Rev 1 M-5): the writer of volume_limit calls this. */
void ui_vlimit_apply(void)
{
    ui_sc_refresh();                    /* a Sound Check boost refits */
    sound_set_volume(global_status.volume);
    UPDATE_TRACE("vlimit set=%d code=%d applied=%d", global_settings.volume_limit,
                 ui_vlimit_locked() ? 1 : 0, global_status.volume);
}

static struct vl_state *vs_of(struct ui_screen *s)
{
    return s ? s->priv : NULL;
}

static int code_of(const struct vl_state *v)
{
    int i, c = 0;

    for (i = 0; i < 4; i++)
        c = c * 10 + v->roller.f[i].value;
    return c;
}

static void roller_reset(struct vl_state *v)
{
    int i;

    memset(&v->roller, 0, sizeof(v->roller));
    v->roller.n = 4;
    v->roller.dark = true;
    for (i = 0; i < 4; i++)
    {
        v->roller.f[i].min = 0;
        v->roller.f[i].max = 9;
        v->roller.f[i].digits = 1;
    }
}

/* Sets the limit (and the code, -1 none); removing the limit removes the
 * code. */
static void commit(const struct vl_state *v, int code)
{
    global_settings.volume_limit = v->limit;
    if (v->limit >= sound_max(SOUND_VOLUME))
        code = -1;
    global_settings.ipodui_vlimit_code = code;
    ui_vlimit_apply();
    fe_settings_save();
}

/* -- drawing ------------------------------------------------------------ */

/* The frosted body and the padlock are Screen Lock's art (lock.c; owner
 * photo 1201), shared. Body rows y0..y1-1 from x0: the limit bar
 * keeps the dark body (its white speakers and pointer, VL_TEXT_FG, were
 * designed on it), the combination phases the frosted one. */
static void body_rows(const struct vl_state *v, int x0, int y0, int y1)
{
    if (y0 < UI_DARK_HDR_H)
        y0 = UI_DARK_HDR_H;
    if (y1 > LCD_HEIGHT)
        y1 = LCD_HEIGHT;
    if (y1 <= y0)
        return;
    if (v->phase == VL_LIMIT)
    {
        ui_dark_body(y0, y1 - y0);
        return;
    }
    ui_lock_body(x0, y0, y1);
}

static struct np_canvas fb_canvas(void)
{
    struct np_canvas cv;

    lcd_set_viewport(NULL);
    cv.px = FBADDR(0, 0);
    cv.stride = (int)(FBADDR(0, 1) - FBADDR(0, 0));
    cv.w = LCD_WIDTH;
    cv.h = LCD_HEIGHT;
    return cv;
}

static void draw_bar(const struct vl_state *v)
{
    struct np_canvas cv = fb_canvas();
    int vmax = sound_max(SOUND_VOLUME);
    int vol = global_status.volume < v->limit ? global_status.volume : v->limit;
    int fill = ui_vol_frac(vol) * VL_BAR_W / 1024;
    int tx = VL_BAR_X + ui_vol_frac(v->limit) * VL_BAR_W / 1024;
    int r, w, h;

    (void)vmax;
    for (r = 0; r < NP_BAR_H; r++)
    {
        if (fill > 0)
        {
            lcd_set_foreground(ui_gloss_color(&fill_gloss, r, NP_BAR_H));
            lcd_hline(VL_BAR_X, VL_BAR_X + fill - 1, VL_BAR_Y + r);
        }
        if (fill < VL_BAR_W)
        {
            lcd_set_foreground(ui_gloss_color(&track_gloss, r, NP_BAR_H));
            lcd_hline(VL_BAR_X + fill, VL_BAR_X + VL_BAR_W - 1, VL_BAR_Y + r);
        }
    }
    /* the limit: a small white triangle under the bar, point up */
    lcd_set_foreground(VL_TEXT_FG);
    for (r = 0; r < VL_TRI_H; r++)
    {
        int half = r * (VL_TRI_W / 2) / (VL_TRI_H - 1);

        lcd_hline(tx - half, tx + half, VL_BAR_Y + NP_BAR_H + 3 + r);
    }
    np_glyph_size(NPG_SPEAKER, &w, &h);
    np_glyph_draw(&cv, NPG_SPEAKER, 41, VL_BAR_Y + (NP_BAR_H - h + 1) / 2,
                  NULL, VL_TEXT_FG, VL_TEXT_FG);
    np_glyph_size(NPG_SPEAKER_WAVES, &w, &h);
    np_glyph_draw(&cv, NPG_SPEAKER_WAVES, 273, VL_BAR_Y + (NP_BAR_H - h + 1) / 2,
                  NULL, VL_TEXT_FG, VL_TEXT_FG);
}

static void draw_contents(const struct vl_state *v, bool label, bool lock)
{
    if (v->phase == VL_LIMIT)
        draw_bar(v);
    else
    {
        if (lock)
            ui_lock_padlock(v->phase == VL_NEWCODE);
        ui_roller_draw(&v->roller, VL_ROLLER_Y,
                       !label ? -1 : v->phase == VL_NEWCODE ?
                       LANG_IPODUI_NEW_COMBINATION :
                       LANG_IPODUI_ENTER_COMBINATION);
    }
}

static void vl_draw(struct ui_screen *s, unsigned dirty)
{
    struct vl_state *v = vs_of(s);
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;

    if (!v)
        return;
    if (dirty & UI_DIRTY_BODY)
    {
        body_rows(v, 0, UI_DARK_HDR_H, LCD_HEIGHT);
        draw_contents(v, true, true);
    }
    else if (dirty & UI_DIRTY_BAND)
    {
        /* a band in the combination phases is the roller's (x >= 131):
         * the padlock left of it (x < UI_LOCK_ART_RIGHT) is neither erased
         * nor redrawn */
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
            body_rows(v, v->phase == VL_LIMIT ? 0 : UI_LOCK_ART_RIGHT, y[i],
                      y[i] + h[i]);
        draw_contents(v, false, false); /* the label would thicken */
    }
}

/* -- the screen ------------------------------------------------------------ */

static int vl_enter(struct ui_screen *s, const void *arg)
{
    struct vl_state *v;

    (void)arg;
    v = ui_arena_alloc(sizeof(*v));
    if (!v)
    {
        logf("ipodui: vlimit: arena full");
        return -1;
    }
    s->priv = v;
    s->title = str(LANG_IPODUI_VOLUME_LIMIT);
    s->header_w = UI_HEADER_DARK;   /* the main loop draws the dark header */
    v->limit = global_settings.volume_limit;
    v->locked = ui_vlimit_locked();
    v->phase = v->locked ? VL_UNLOCK : VL_LIMIT;
    roller_reset(v);
    return 0;
}

static void vl_leave(struct ui_screen *s)
{
    s->priv = NULL;
}

static void to_phase(struct vl_state *v, enum vl_phase p)
{
    v->phase = p;
    roller_reset(v);
    ui_invalidate(UI_DIRTY_BODY);
}

/* PLAY on the limit screen with no code: play or pause the queue to
 * hear the level (a preview) */
static void preview_toggle(void)
{
    int st = audio_status();

    if (st & AUDIO_STATUS_PAUSE)
        audio_resume();
    else if (st & AUDIO_STATUS_PLAY)
        audio_pause();
    else
        ui_playback_resume();
}

static enum ui_result limit_input(struct ui_screen *s, struct vl_state *v,
                                  const struct ui_input *in)
{
    int old = v->limit;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            /* one click = one step of the volume curve (P15), so the
             * triangle moves as evenly as the volume bar fills */
            v->limit = vol_step_to_db(vol_step_add(v->limit,
                                                   in->wheel_steps));
            if (v->limit != old)
                ui_invalidate_band(VL_BAND_Y, VL_BAND_H);
            return UI_HANDLED;
        case UI_KEY_PLAY:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            if (v->locked)
            {
                commit(v, global_settings.ipodui_vlimit_code);
                return UI_POP;          /* PLAY/PAUSE accepts the change */
            }
            preview_toggle();
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            if (!v->locked)
                to_phase(v, VL_NEWCODE);
            return UI_HANDLED;
        default:
            (void)s;
            return UI_IGNORED;          /* MENU leaves, nothing saved */
    }
}

static void wrong_code(struct vl_state *v)
{
    UPDATE_TRACE("vlimit unlock ok=%d", 0);
    roller_reset(v);
    keyclick_click(true, BUTTON_SELECT);        /* a double tick */
    sleep(HZ / 12);
    keyclick_click(true, BUTTON_SELECT);
    ui_invalidate_band(VL_ROLLER_Y - 2, 47);
}

static enum ui_result vl_input(struct ui_screen *s, const struct ui_input *in)
{
    struct vl_state *v = vs_of(s);
    int by, bh;

    if (!v)
        return UI_IGNORED;
    if (v->phase == VL_LIMIT)
        return limit_input(s, v, in);

    /* the roller phases: only its keys; others (a held MENU) stay global */
    if (in->key != UI_KEY_WHEEL && in->key != UI_KEY_SELECT &&
        in->key != UI_KEY_NEXT && in->key != UI_KEY_PREV &&
        in->key != UI_KEY_MENU)
        return UI_IGNORED;
    if (in->key == UI_KEY_MENU && (!in->release || in->long_press))
        return UI_IGNORED;
    switch (ui_roller_input(&v->roller, in, &by, &bh))
    {
        case UI_ROLLER_DONE:
            if (v->phase == VL_UNLOCK)
            {
                if (code_of(v) == global_settings.ipodui_vlimit_code)
                {
                    UPDATE_TRACE("vlimit unlock ok=%d", 1);
                    to_phase(v, VL_LIMIT);
                }
                else
                    wrong_code(v);
                return UI_HANDLED;
            }
            commit(v, code_of(v));          /* four digits + SELECT */
            return UI_POP;
        case UI_ROLLER_CANCEL:
            if (v->phase == VL_NEWCODE)
            {
                commit(v, -1);              /* MENU accepts with no code */
                return UI_POP;
            }
            return UI_POP;
        default:
            ui_invalidate_band(by, bh);
            return UI_HANDLED;
    }
}

const struct ui_screen_ops ui_vlimit_screen = {
    .name    = "vlimit",
    .enter   = vl_enter,
    .leave   = vl_leave,
    .draw    = vl_draw,
    .input   = vl_input,
    .timeout = NULL,
};
