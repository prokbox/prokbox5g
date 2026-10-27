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
 * iPod UI shell: Extras > Screen Lock, the setup screens (P12 W1-L)
 * Module MOD_LOCKSET (Class A); the gate and locked screen are lock.c
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

/* The stock flow (Extras > Screen Lock), on the frosted combination body
 * Volume Limit shares (lock.h's art):
 *
 *   no combination  New Combination (open padlock), then Confirm
 *                   Combination; a different second entry says so under
 *                   the roller and starts again at New. MENU leaves with
 *                   nothing set.
 *   a combination   the two rows Lock and Reset beside the closed padlock.
 *                   Lock locks at once (lock.c pushes the locked screen
 *                   over this one; unlocking comes back here). Reset asks
 *                   for the current combination first, then forgets it
 *                   and starts New Combination.
 *
 * One screen with phases (state in the screen arena); the phases switch
 * with a full redraw, the wheel pushes the roller's or the rows' band
 * only. The combination is never logged. Trace:
 *   lockset phase=<new|confirm|menu|verify>
 *   lockset set step=<n>     lock_set_enter()'s result (enum lock_set_step)
 *   lockset verify ok=<0|1>
 */
#include "config.h"
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "lang.h"
#include "misc.h"
#include "button.h"
#include "settings.h"
#include "ipodui/fe_be.h"   /* P19 S1: settings saves cross */
#include "ipodui/ipodui.h"
#include "ipodui/anim.h"
#include "ipodui/paint.h"
#include "ipodui/theme.h"
#include "ipodui/header.h"
#include "ipodui/roller.h"
#include "ipodui/screen_stack.h"
#include "ipodui/usbscreen.h"
#include "ipodui/lock.h"
#include "ipodui/screens/screens.h"

enum ls_phase { LS_NEW, LS_CONFIRM, LS_MENU, LS_VERIFY };

struct ls_state {
    struct ui_roller roller;
    struct lock_setter setter;
    int16_t label;              /* LANG id under the roller, -1 none */
    uint8_t phase;
    uint8_t sel;                /* LS_MENU: 0 Lock, 1 Reset */
};
_Static_assert(sizeof(struct ls_state) <= UI_ARENA_B_SETTING,
               "ls_state: arena bound");

#define LS_ROLLER_Y     104     /* as the locked screen and Volume Limit */
#define LS_LABEL_Y      206     /* the label's band (baseline y 222) */
#define LS_LABEL_H      28
#define LS_ROW_X        (UI_LOCK_ART_RIGHT + 14)
#define LS_ROW_W        (LCD_WIDTH - 14 - LS_ROW_X)
#define LS_ROW_H        36
#define LS_ROWS_Y       (LS_ROLLER_Y - 6)   /* two rows, centred on the
                                               padlock's body */
#define LS_ROW_FG       LCD_RGBPACK(0x1e, 0x22, 0x2a)

/* for the trace only (no table: release builds log nothing) */
#define PHASE_NAME(p) ((p) == LS_NEW ? "new" : (p) == LS_CONFIRM ? \
                       "confirm" : (p) == LS_MENU ? "menu" : "verify")

static void roller_clear(struct ui_roller *r)
{
    int i;

    memset(r, 0, sizeof(*r));
    r->n = 4;
    r->dark = true;
    for (i = 0; i < 4; i++)
    {
        r->f[i].max = 9;
        r->f[i].digits = 1;
    }
}

static int roller_code(const struct ui_roller *r)
{
    int i, c = 0;

    for (i = 0; i < 4; i++)
        c = c * 10 + r->f[i].value;
    return c;
}

static void to_phase(struct ls_state *v, enum ls_phase p, int label)
{
    v->phase = p;
    v->sel = 0;
    v->label = label;
    roller_clear(&v->roller);
    if (p == LS_NEW)
        lock_set_begin(&v->setter);
    UPDATE_TRACE("lockset phase=%s", PHASE_NAME(p));
    ui_invalidate(UI_DIRTY_BODY);
}

static void set_label(struct ls_state *v, int lang)
{
    if (v->label == lang)
        return;
    v->label = lang;
    ui_invalidate_band(LS_LABEL_Y, LS_LABEL_H);
}

/* -- drawing ------------------------------------------------------------ */

static void draw_rows(const struct ls_state *v)
{
    struct viewport vp;
    int r;

    memset(&vp, 0, sizeof(vp));
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.drawmode = DRMODE_FG;
    vp.font = ui_theme->font_ui;
    for (r = 0; r < 2; r++)
    {
        int ry = LS_ROWS_Y + r * LS_ROW_H;

        lcd_set_viewport(NULL);
        if (r == v->sel)
            ui_paint_selection(LS_ROW_X, ry, LS_ROW_W, LS_ROW_H - 4);
        lcd_set_viewport(&vp);
        lcd_set_foreground(r == v->sel ? LCD_WHITE : LS_ROW_FG);
        ui_paint_text_centre(LS_ROW_X + LS_ROW_W / 2, ry + LS_ROW_H / 2 + 4,
                             LS_ROW_W - 8,
                             str(r ? LANG_IPODUI_X_RESET : LANG_IPODUI_X_LOCK));
    }
    lcd_set_viewport(NULL);
}

static void ls_draw(struct ui_screen *s, unsigned dirty)
{
    struct ls_state *v = s->priv;
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;
    bool label = false;

    if (!v)
        return;
    if (dirty & UI_DIRTY_BODY)
    {
        ui_lock_body(0, UI_DARK_HDR_H, LCD_HEIGHT);
        ui_lock_padlock(v->phase == LS_NEW || v->phase == LS_CONFIRM);
        label = true;
    }
    else if (dirty & UI_DIRTY_BAND)
    {
        /* right of the padlock, or the label's band below it (full
         * width; the label is drawn only on a clean body) */
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
        {
            bool lab = y[i] >= LS_LABEL_Y;

            ui_lock_body(lab ? 0 : UI_LOCK_ART_RIGHT, y[i], y[i] + h[i]);
            label |= lab;
        }
    }
    else
        return;
    if (v->phase == LS_MENU)
        draw_rows(v);
    else
        ui_roller_draw(&v->roller, LS_ROLLER_Y, label ? v->label : -1);
}

/* -- the screen ------------------------------------------------------------ */

static int ls_enter(struct ui_screen *s, const void *arg)
{
    struct ls_state *v = ui_arena_alloc(sizeof(*v));

    (void)arg;
    if (!v)
        return -1;
    s->priv = v;
    s->title = str(LANG_IPODUI_X_SCREEN_LOCK);
    s->header_w = UI_HEADER_DARK;   /* the main loop draws the dark header */
    if (global_settings.ipodui_lock_code == LOCK_CODE_NONE)
        to_phase(v, LS_NEW, LANG_IPODUI_X_NEW_COMBINATION);
    else
        to_phase(v, LS_MENU, -1);
    return 0;
}

static void ls_leave(struct ui_screen *s)
{
    s->priv = NULL;
}

static void wrong_tick(void)
{
    keyclick_click(true, BUTTON_SELECT);        /* a double tick */
    sleep(HZ / 12);
    keyclick_click(true, BUTTON_SELECT);
}

/* four digits and SELECT in a roller phase */
static void entered(struct ls_state *v, int code)
{
    struct lock_state st;
    int step;

    st.code = (int16_t)global_settings.ipodui_lock_code;
    st.locked = global_settings.ipodui_locked ? 1 : 0;
    if (v->phase == LS_VERIFY)
    {
        bool ok = code == st.code;

        UPDATE_TRACE("lockset verify ok=%d", (int)ok);
        if (ok)
        {
            lock_clear(&st);                    /* Reset: forget it */
            global_settings.ipodui_lock_code = st.code;
            global_settings.ipodui_locked = false;
            fe_settings_save();
            to_phase(v, LS_NEW, LANG_IPODUI_X_NEW_COMBINATION);
            return;
        }
        roller_clear(&v->roller);
        wrong_tick();
        ui_invalidate_band(LS_ROLLER_Y - 2, 47);
        set_label(v, LANG_IPODUI_X_WRONG_COMBINATION);
        return;
    }
    step = lock_set_enter(&v->setter, code, &st);
    UPDATE_TRACE("lockset set step=%d", step);
    switch (step)
    {
        case LOCK_SET_CONFIRM:
            v->phase = LS_CONFIRM;
            v->label = LANG_IPODUI_X_CONFIRM_COMBINATION;
            roller_clear(&v->roller);
            UPDATE_TRACE("lockset phase=%s", PHASE_NAME(LS_CONFIRM));
            ui_invalidate(UI_DIRTY_BODY);
            break;
        case LOCK_SET_DONE:
            global_settings.ipodui_lock_code = st.code;
            fe_settings_save();
            to_phase(v, LS_MENU, -1);
            break;
        case LOCK_SET_MISMATCH:
        default:
            wrong_tick();
            v->phase = LS_NEW;
            v->label = LANG_IPODUI_X_COMBINATION_MISMATCH;
            roller_clear(&v->roller);
            UPDATE_TRACE("lockset phase=%s", PHASE_NAME(LS_NEW));
            ui_invalidate(UI_DIRTY_BODY);
            break;
    }
}

static enum ui_result menu_input(struct ls_state *v,
                                 const struct ui_input *in)
{
    switch (in->key)
    {
        case UI_KEY_WHEEL:
        {
            int sel = in->wheel_steps > 0 ? 1 : 0;

            if (in->wheel_steps && sel != v->sel)
            {
                v->sel = sel;
                ui_invalidate_band(LS_ROWS_Y, 2 * LS_ROW_H);
            }
            return UI_HANDLED;
        }
        case UI_KEY_SELECT:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            if (v->sel == 0)
                ui_lock_engage();               /* the locked screen */
            else
                to_phase(v, LS_VERIFY, LANG_IPODUI_X_ENTER_COMBINATION);
            return UI_HANDLED;
        default:
            return UI_IGNORED;                  /* MENU leaves */
    }
}

static enum ui_result ls_input(struct ui_screen *s, const struct ui_input *in)
{
    struct ls_state *v = s->priv;
    int by, bh;

    if (!v)
        return UI_IGNORED;
    if (v->phase == LS_MENU)
        return menu_input(v, in);
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
            entered(v, roller_code(&v->roller));
            return UI_HANDLED;
        case UI_ROLLER_CANCEL:
            if (v->phase == LS_VERIFY)
            {
                to_phase(v, LS_MENU, -1);       /* back to Lock / Reset */
                return UI_HANDLED;
            }
            return UI_POP;                      /* nothing set */
        default:
            ui_invalidate_band(by, bh);
            if (in->key == UI_KEY_WHEEL)
                set_label(v, v->phase == LS_CONFIRM ?
                          LANG_IPODUI_X_CONFIRM_COMBINATION :
                          v->phase == LS_VERIFY ?
                          LANG_IPODUI_X_ENTER_COMBINATION :
                          LANG_IPODUI_X_NEW_COMBINATION);
            return UI_HANDLED;
    }
}

const struct ui_screen_ops ui_lockset_screen = {
    .name    = "lockset",
    .enter   = ls_enter,
    .leave   = ls_leave,
    .draw    = ls_draw,
    .input   = ls_input,
    .timeout = NULL,
};
