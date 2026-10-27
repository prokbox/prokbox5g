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
 * iPod UI shell: Screen Lock gate, locked screen and padlock art (P12 W1-L)
 * Resident: brief 3.4, Q10, Q11
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

/* The gate (lock.h). The state is the two settings, ipodui_lock_code and
 * ipodui_locked; lock_core.c holds the rules. Locked means the locked
 * screen is on the stack and input.c lets only its keys through: the
 * wheel and SELECT (and PREV/NEXT between digits), hold PLAY to sleep
 * (ui_main.c's global_hold(), before any screen), the Hold switch
 * (firmware), and alarms (the ring screen is a UI_T_SYSTEM push over it,
 * Q5/Q10). Hold SELECT + MENU is the hardware reset and needs nothing
 * from us. A data host unlocks (Q10); the save waits for the post-USB
 * path (no write at USB entry).
 *
 * Forgotten combination: connect the unit to any computer for data
 * (it unlocks at once); there is no other way in from the locked screen,
 * as on the stock player. Reset Settings, reachable only while unlocked,
 * also forgets the combination.
 *
 * The combination is never logged: trace lines carry ok=0/1 only.
 *
 * Trace (UPDATE_TRACE, sim.log):
 *   lock engage ok=<0|1>      Screen Lock's Lock row
 *   lock screen push          the gate put the locked screen up
 *   lock try ok=<0|1>         an entry on the locked screen
 *   lock usb unlock           a data host unlocked (lock_usb_host)
 *   lock save                 the two settings written
 *   lock key swallow k=<key>  a key the gate kept from the screens
 *   lock fix                  damaged settings repaired (lock_state_fix)
 */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "misc.h"
#include "button.h"
#include "settings.h"
#include "ipodui/fe_be.h"   /* P19 S1: settings saves cross */
#include "ata_idle_notify.h"
#include "ipodui/ipodui.h"
#include "ipodui/anim.h"
#include "ipodui/paint.h"
#include "ipodui/header.h"
#include "ipodui/input.h"
#include "ipodui/roller.h"
#include "ipodui/screen_stack.h"
#include "ipodui/usbscreen.h"
#include "ipodui/lock.h"
#ifdef SIMULATOR
#include <stdlib.h>
#include "ipodui/sleep.h"
#endif

/* -- the art (moved here from vlimit.c, P11; unchanged) ------------------ */

/* The body (owner #52: "frosted background"; photo
 * extras-screenlock-new-combination): a light, cool blue-grey, nearly
 * even from top to bottom, below the dark header the main loop draws
 * (header_w = UI_HEADER_DARK). Row colours are a function of y, so the
 * padlock's soft edges are blended over the known colour, never read
 * back, and a band redraw repeats them exactly. */
unsigned ui_lock_body_row(int y)
{
    int t = (y - UI_DARK_HDR_H) * 256 / (LCD_HEIGHT - UI_DARK_HDR_H);

    if (t < 0)
        t = 0;
    return LCD_RGBPACK(0x9a - (0x16 * t >> 8), 0xa4 - (0x16 * t >> 8),
                       0xb6 - (0x16 * t >> 8));
}

void ui_lock_body(int x0, int y0, int y1)
{
    if (y0 < UI_DARK_HDR_H)
        y0 = UI_DARK_HDR_H;
    if (y1 > LCD_HEIGHT)
        y1 = LCD_HEIGHT;
    lcd_set_viewport(NULL);
    lcd_set_drawmode(DRMODE_SOLID);
    for (; y0 < y1; y0++)
    {
        lcd_set_foreground(ui_lock_body_row(y0));
        lcd_hline(x0, LCD_WIDTH - 1, y0);
    }
}

/* An original metallic padlock (owner #52; photo
 * extras-screenlock-new-combination: left of the roller, shackle
 * x ~37-107 from y ~50, body x ~32-115, y ~105-175): a round silver
 * tube for the shackle, lit along its upper inside, and a bronze body
 * with a bright top lip, a sheen across its upper third and rounded
 * lower corners. Open (the shackle lifted, its right leg out of the
 * body) while a new combination is set, closed otherwise. Every pixel
 * is blended over ui_lock_body_row(), never over what the LCD holds. */
#define LOCK_CX         72
#define LOCK_RO         35      /* shackle radii */
#define LOCK_RI         25
#define LOCK_TOP        52      /* the closed shackle's top */
#define LOCK_LIFT       4       /* raised by this much when open */
#define LOCK_BODY_Y     105
#define LOCK_BODY_W     84
#define LOCK_BODY_H     70
#define LOCK_BODY_R     6
_Static_assert(LOCK_CX + LOCK_BODY_W / 2 + 4 == UI_LOCK_ART_RIGHT,
               "UI_LOCK_ART_RIGHT: right of the padlock");

/* the tube's colour at u = 0..16 across it (0 = its outer edge) */
static unsigned tube_col(int u)
{
    static const unsigned char lum[17] = {
        0x86, 0xa4, 0xc0, 0xd8, 0xea, 0xf6, 0xfc, 0xf4, 0xe4,
        0xd2, 0xc2, 0xb6, 0xb0, 0xae, 0xa8, 0x9a, 0x84 };
    int v = lum[u < 0 ? 0 : u > 16 ? 16 : u];

    return LCD_RGBPACK(v, v + (v < 0xfa ? 3 : 0), v + (v < 0xf6 ? 8 : 0));
}

static void lock_put(int x, int y, unsigned c, int a)   /* a = 0..15 */
{
    if (a <= 0 || y < UI_DARK_HDR_H || y >= LCD_HEIGHT)
        return;
    lcd_set_foreground(a >= 15 ? c : ui_paint_blend(ui_lock_body_row(y), c,
                                                    a));
    lcd_drawpixel(x, y);
}

void ui_lock_padlock(bool open)
{
    const int top = LOCK_TOP - (open ? LOCK_LIFT : 0);
    const int bx = LOCK_CX - LOCK_BODY_W / 2;
    const int ro2 = LOCK_RO * LOCK_RO, ri2 = LOCK_RI * LOCK_RI;
    int x, y;

    lcd_set_viewport(NULL);
    lcd_set_drawmode(DRMODE_SOLID);
    /* the shackle: a half ring (coverage and shading from the squared
     * distance, a soft edge ~1 px wide) on two legs */
    for (y = 0; y <= LOCK_RO; y++)
    {
        int dy = LOCK_RO - y;

        for (x = -LOCK_RO; x <= LOCK_RO; x++)
        {
            int d2 = x * x + dy * dy, a, u;

            if (d2 > ro2 + LOCK_RO || d2 < ri2 - LOCK_RI)
                continue;
            a = d2 > ro2 - LOCK_RO ? (ro2 + LOCK_RO - d2) * 15 / (2 * LOCK_RO)
              : d2 < ri2 + LOCK_RI ? (d2 - ri2 + LOCK_RI) * 15 / (2 * LOCK_RI)
              : 15;
            u = (ro2 - d2) * 16 / (ro2 - ri2);
            lock_put(LOCK_CX + x, top + y, tube_col(u), a);
        }
    }
    for (y = top + LOCK_RO + 1; y < LOCK_BODY_Y + 2; y++)
    {
        int w = LOCK_RO - LOCK_RI;

        for (x = 0; x < w; x++)
        {
            int u = x * 16 / (w - 1);

            /* the left leg always enters the body; the right one stops
             * short of it when open */
            lock_put(LOCK_CX - LOCK_RO + x, y, tube_col(u), 15);
            if (!open || y < LOCK_BODY_Y - 4)
                lock_put(LOCK_CX + LOCK_RI + x, y, tube_col(16 - u), 15);
        }
    }
    /* the body */
    for (y = 0; y < LOCK_BODY_H; y++)
    {
        int t = y * 256 / LOCK_BODY_H, k = y - (LOCK_BODY_H - LOCK_BODY_R);
        int in = 0, rr, g, b;
        unsigned c, edge;

        if (y == 0)
            t = -40;                            /* the lit lip */
        else if (y < LOCK_BODY_H / 3)
            t -= 30 * (LOCK_BODY_H / 3 - y) / (LOCK_BODY_H / 3); /* sheen */
        rr = 0xd8 - (0x52 * t >> 8);
        g = 0xbc - (0x50 * t >> 8);
        b = 0xa6 - (0x4a * t >> 8);
        c = LCD_RGBPACK(MIN(rr, 255), MIN(g, 255), MIN(b, 255));
        edge = ui_paint_blend(c, LCD_RGBPACK(0x4a, 0x36, 0x2c), 6);
        if (y == LOCK_BODY_H - 1)
            c = edge;
        if (k > 0)                              /* rounded lower corners */
        {
            int r = LOCK_BODY_R;

            while (in < r && (r - in) * (r - in) + k * k > r * r)
                in++;
        }
        lcd_set_foreground(c);
        lcd_hline(bx + in + 1, bx + LOCK_BODY_W - 2 - in, LOCK_BODY_Y + y);
        lock_put(bx + in, LOCK_BODY_Y + y, edge, in ? 10 : 15);
        lock_put(bx + LOCK_BODY_W - 1 - in, LOCK_BODY_Y + y, edge,
                 in ? 10 : 15);
    }
}

/* -- the state -------------------------------------------------------- */

#define LK_FIXED      0x01      /* the loaded settings were checked */
#define LK_SAVE_OWED  0x02      /* a data host unlocked: save after USB */
static uint8_t lk_flags;
static struct ui_screen *lk_screen;     /* the locked screen, while up */

static void state_get(struct lock_state *st)
{
    st->code = (int16_t)global_settings.ipodui_lock_code;
    st->locked = global_settings.ipodui_locked ? 1 : 0;
}

static void state_put(const struct lock_state *st)
{
    global_settings.ipodui_lock_code = st->code;
    global_settings.ipodui_locked = st->locked != 0;
}

/* The settings written at once (Q10: a power cut right after Lock keeps
 * the lock); never inside a USB session (lk_flags then owes it) */
static void lock_save(void)
{
    fe_settings_save();
    fe_idle_flush();                    /* P19 S2: on be_svc */
    UPDATE_TRACE("lock save%s", "");
}

static void lock_fix_once(void)
{
    struct lock_state st;

    if (lk_flags & LK_FIXED)
        return;
    lk_flags |= LK_FIXED;
    if (global_settings.ipodui_lock_code < -1
        || global_settings.ipodui_lock_code > LOCK_CODE_MAX)
        global_settings.ipodui_lock_code = LOCK_CODE_NONE;
    state_get(&st);
    if (lock_state_fix(&st))
    {
        state_put(&st);
        UPDATE_TRACE("lock fix%s", "");
        fe_settings_save();                /* with the next idle write */
    }
}

bool ui_lock_active(void)
{
    lock_fix_once();
    return global_settings.ipodui_locked;
}

#ifdef SIMULATOR
/* tests only (tests/ui, "# env:"): IPODUI_SIM_LOCK_UNSTACKED=<s> refuses
 * the locked screen until the RTC reads <s> s past the first push asked
 * for, so a script has a locked unit whose locked screen is not on the
 * stack (as after a push the screen arena refused) */
static bool sim_unstacked(void)
{
    static uint32_t until;
    static bool read;
    const char *e;

    if (!read)
    {
        read = true;
        e = getenv("IPODUI_SIM_LOCK_UNSTACKED");
        if (e)
            until = ui_sleep_rtc_now() + (uint32_t)atoi(e);
    }
    return until && ui_sleep_rtc_now() < until;
}
#endif

static int push_locked_screen(enum ui_transition t)
{
    int rc;

#ifdef SIMULATOR
    if (sim_unstacked())
    {
        UPDATE_TRACE("lock screen push refused (sim)%s", "");
        return -1;
    }
#endif
    rc = ui_push_t(&ui_lock_screen, NULL, t);

    UPDATE_TRACE("lock screen push rc=%d", rc);
    return rc;
}

void ui_lock_engage(void)
{
    struct lock_state st;
    bool ok;

    lock_fix_once();
    state_get(&st);
    ok = lock_engage(&st);
    UPDATE_TRACE("lock engage ok=%d", (int)ok);
    if (!ok)
        return;
    state_put(&st);
    lock_save();
    if (!lk_screen)
        push_locked_screen(UI_T_PUSH);
}

bool ui_lock_try(int code)
{
    struct lock_state st;
    bool ok;

    state_get(&st);
    ok = lock_try(&st, code);
    UPDATE_TRACE("lock try ok=%d", (int)ok);
    if (ok && global_settings.ipodui_locked)
    {
        state_put(&st);
        lock_save();
    }
    return ok;
}

void ui_lock_usb_host(void)
{
    struct lock_state st;

    if (!ui_lock_active())
        return;
    state_get(&st);
    lock_usb_host(&st);
    state_put(&st);
    lk_flags |= LK_SAVE_OWED;           /* written after the session */
    UPDATE_TRACE("lock usb unlock%s", "");
}

bool ui_lock_gate(void)
{
    struct ui_screen *top;

    if (!ui_lock_active())
    {
        if (lk_flags & LK_SAVE_OWED)
        {
            lk_flags &= ~LK_SAVE_OWED;
            lock_save();                /* the post-USB path */
        }
        if (lk_screen && ui_top() == lk_screen)
        {
            /* unlocked from outside (a data host): no slide over the
             * picture the USB screen left, one full redraw */
            ui_pop();
            (void)ui_take_transition();
            ui_invalidate(UI_DIRTY_ALL);
            return true;
        }
        return false;
    }
    if (lk_screen)
        return false;                   /* up, or under a system screen */
    top = ui_top();
    if (!top || top->transition == UI_T_SYSTEM)
        return false;                   /* a ring first; the lock after */
    if (push_locked_screen(UI_T_SYSTEM) < 0)
        return false;                   /* ui_lock_key() keeps it shut */
    (void)ui_take_transition();         /* up at once, no slide */
    ui_invalidate(UI_DIRTY_ALL);
    return true;
}

bool ui_lock_key(const struct ui_input *in)
{
    struct ui_screen *top;
    bool swallow;

    if (!ui_lock_active())
        return false;
    top = ui_top();
    if (lk_screen && top == lk_screen)
        swallow = in->key == UI_KEY_MENU;
    else if (top && top->transition == UI_T_SYSTEM)
        swallow = in->key == UI_KEY_MENU && in->repeat;
    else
        swallow = in->key != UI_KEY_PLAY;
    if (swallow && !in->repeat)
        UPDATE_TRACE("lock key swallow k=%d", (int)in->key);
    return swallow;
}

/* -- the locked screen ---------------------------------------------- */

#define LK_ROLLER_Y     104     /* as Volume Limit's (photo: centre ~125) */
#define LK_LABEL_Y      206     /* the label's band (baseline y 222) */
#define LK_LABEL_H      28

struct lk_state {
    struct ui_roller roller;
    int16_t label;              /* LANG id under the roller, -1 none */
};

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

static int lk_enter(struct ui_screen *s, const void *arg)
{
    struct lk_state *v = ui_arena_alloc(sizeof(*v));

    (void)arg;
    if (!v)
        return -1;
    s->priv = v;
    s->title = str(LANG_IPODUI_X_SCREEN_LOCK);
    s->header_w = UI_HEADER_DARK;
    roller_clear(&v->roller);
    v->label = LANG_IPODUI_X_ENTER_COMBINATION;
    lk_screen = s;
    return 0;
}

static void lk_leave(struct ui_screen *s)
{
    if (lk_screen == s)
        lk_screen = NULL;
    s->priv = NULL;
}

static void lk_draw(struct ui_screen *s, unsigned dirty)
{
    struct lk_state *v = s->priv;
    int y[UI_BANDS_MAX], h[UI_BANDS_MAX], i, n;
    bool label = false;

    if (!v)
        return;
    if (dirty & UI_DIRTY_BODY)
    {
        ui_lock_body(0, UI_DARK_HDR_H, LCD_HEIGHT);
        ui_lock_padlock(false);
        label = true;
    }
    else if (dirty & UI_DIRTY_BAND)
    {
        /* the roller's band right of the padlock; the label's band is
         * below it, full width (the label is redrawn only on a clean
         * body: its anti-aliased edge would thicken) */
        n = ui_dirty_bands(y, h);
        for (i = 0; i < n; i++)
        {
            bool lab = y[i] >= LK_LABEL_Y;

            ui_lock_body(lab ? 0 : UI_LOCK_ART_RIGHT, y[i], y[i] + h[i]);
            label |= lab;
        }
    }
    else
        return;
    ui_roller_draw(&v->roller, LK_ROLLER_Y, label ? v->label : -1);
}

static void set_label(struct lk_state *v, int lang)
{
    if (v->label == lang)
        return;
    v->label = lang;
    ui_invalidate_band(LK_LABEL_Y, LK_LABEL_H);
}

static enum ui_result lk_input(struct ui_screen *s, const struct ui_input *in)
{
    struct lk_state *v = s->priv;
    int by, bh;

    if (!v)
        return UI_HANDLED;
    /* only the roller's keys; PLAY's short press does nothing here, its
     * hold sleeps (global_hold, before us); MENU never comes (the gate) */
    if (in->key != UI_KEY_WHEEL && in->key != UI_KEY_SELECT &&
        in->key != UI_KEY_NEXT && in->key != UI_KEY_PREV)
        return UI_HANDLED;
    switch (ui_roller_input(&v->roller, in, &by, &bh))
    {
        case UI_ROLLER_DONE:
            if (ui_lock_try(roller_code(&v->roller)))
                return UI_POP;
            roller_clear(&v->roller);
            keyclick_click(true, BUTTON_SELECT);    /* a double tick */
            sleep(HZ / 12);
            keyclick_click(true, BUTTON_SELECT);
            ui_invalidate_band(by, bh);
            set_label(v, LANG_IPODUI_X_WRONG_COMBINATION);
            return UI_HANDLED;
        default:
            ui_invalidate_band(by, bh);
            if (in->key == UI_KEY_WHEEL)
                set_label(v, LANG_IPODUI_X_ENTER_COMBINATION);
            return UI_HANDLED;
    }
}

const struct ui_screen_ops ui_lock_screen = {
    .name    = "lock",
    .enter   = lk_enter,
    .leave   = lk_leave,
    .draw    = lk_draw,
    .input   = lk_input,
    .timeout = NULL,
};
