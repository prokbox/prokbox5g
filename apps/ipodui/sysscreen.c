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

/* Phase 11 system screens (docs/phase11-brief.md sections 3.7, 4.6; U2b).
 *
 * Charging / Charged (frozen default E1, Rev 1 H-1): hold PLAY on power
 * (ui_main.c global_hold() -> charging_sleep_prepare(), which pauses and
 * saves first) pushes this ordinary stack screen: the dark chrome, the
 * header titled "Charging" or "Charged", and a big battery with a bolt
 * (charging) or a plug (charged, on power). No nested loop: the main
 * loop runs the backlight timeout, the unlit face and LCD sleep over it
 * as over any screen. It leaves on any key (swallowed with its release;
 * playback stays paused), when the charger goes, or after a USB session.
 * Wakes: timeout() 10 s while lit (one charging_state() poll, lean rule
 * 5), none while dark; unplug and USB arrive as events that wake the
 * loop, and tick() looks at them then.
 *
 * Connect to Power: clean_shutdown()'s low/empty branches (apps/misc.c,
 * guarded) call ui_sys_low_battery() on the UI thread. One full frame,
 * the dark header with the battery and no title, then 2 s.
 *
 * All glyphs are our own drawings (art/sys/sysscreen.py), 4-bit coverage
 * coloured at draw time; the battery body is two rounded rectangles from
 * corner masks. Nothing allocates; the Charging state (8 B) is in the
 * screen arena. */

/* not ownable (file): a full-screen page or takeover; the drift panel was settled on the way in (ui_panel_hide(), or the takeover's own ui_fb_settle()), and a full lcd_update() releases it (bcmjob_owned_hook) */
#include "config.h"
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "backlight.h"
#include "power.h"
#include "powermgmt.h"
#include "ipodui/anim.h"
#include "ipodui/sleep.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/header.h"
#include "ipodui/usbscreen.h"
#include "ipodui/sysscreen.h"
#include "ipodui/sysscreen_data.h"
#include "ipodui/unlit.h"
#include "ipodui/preview.h"     /* ui_fb_settle() (P19) */

/* -- glyphs ----------------------------------------------------------------- */

static const struct {
    const unsigned char *cov;
    unsigned char w, h;
} glyphs[UI_SYS_GLYPHS] = {
    [UI_SYS_BOLT] = { bolt_cov, BOLT_W, BOLT_H },
    [UI_SYS_PLUG] = { plug_cov, PLUG_W, PLUG_H },
    [UI_SYS_WARN] = { warn_cov, WARN_W, WARN_H },
    [UI_SYS_MOON] = { moon_cov, MOON_W, MOON_H },
    [UI_SYS_SUN]  = { sun_cov, SUN_W, SUN_H },
};

/* 'fg' over 'bg' by 4-bit coverage a (15 = fg): shifts only */
static inline fb_data mix(fb_data bg, unsigned fg, int a)
{
    int k = a + (a >> 3);               /* 0..16 */
    int r = RGB_UNPACK_RED(bg), g = RGB_UNPACK_GREEN(bg),
        b = RGB_UNPACK_BLUE(bg);

    r += (((int)RGB_UNPACK_RED(fg) - r) * k) >> 4;
    g += (((int)RGB_UNPACK_GREEN(fg) - g) * k) >> 4;
    b += (((int)RGB_UNPACK_BLUE(fg) - b) * k) >> 4;
    return LCD_RGBPACK(r, g, b);
}

static inline int cov_at(const unsigned char *cov, int w, int x, int y)
{
    int i = y * w + x;
    return (i & 1) ? cov[i >> 1] & 15 : cov[i >> 1] >> 4;
}

void ui_sys_glyph(enum ui_sys_glyph g, int x, int y, unsigned colour)
{
    const unsigned char *cov = glyphs[g].cov;
    int w = glyphs[g].w, h = glyphs[g].h, i, j;

    lcd_set_viewport(NULL);             /* FBADDR uses the current one */
    for (j = 0; j < h; j++)
    {
        fb_data *p = FBADDR(x, y + j);
        for (i = 0; i < w; i++)
        {
            int a = cov_at(cov, w, i, j);
            if (a == 15)
                p[i] = colour;
            else if (a)
                p[i] = mix(p[i], colour, a);
        }
    }
}

int ui_sys_glyph_w(enum ui_sys_glyph g)
{
    return glyphs[g].w;
}

int ui_sys_glyph_h(enum ui_sys_glyph g)
{
    return glyphs[g].h;
}

/* A rounded rectangle in 'colour', its corners from a top-left quadrant
 * mask of radius r (even): straight parts as full rows, corner pixels
 * blended (no per-pixel lcd calls) */
static void rrect(int x, int y, int w, int h, const unsigned char *mask,
                  int r, unsigned colour)
{
    int j;

    lcd_set_foreground(colour);
    for (j = 0; j < h; j++)
    {
        int cj = j < r ? j : (j >= h - r ? h - 1 - j : -1);
        int i;

        if (cj < 0)
        {
            lcd_hline(x, x + w - 1, y + j);
            continue;
        }
        lcd_hline(x + r, x + w - 1 - r, y + j);
        for (i = 0; i < r; i++)
        {
            int a = cov_at(mask, r, i, cj);
            fb_data *l = FBADDR(x + i, y + j), *rt = FBADDR(x + w - 1 - i, y + j);

            if (a == 15)
                *l = *rt = colour;
            else if (a)
            {
                *l = mix(*l, colour, a);
                *rt = mix(*rt, colour, a);
            }
        }
    }
}

/* -- the big battery -------------------------------------------------------- */

#define BAT_W       124         /* body, outline included */
#define BAT_H       62
#define BAT_X       ((LCD_WIDTH - BAT_W - NUB_W) / 2)
#define BAT_STROKE  4
#define NUB_W       8
#define NUB_H       24
#define FILL_PAD    3           /* gap between the outline and the fill */
#define FILL_X      (BAT_X + BAT_STROKE + FILL_PAD)
#define FILL_W      (BAT_W - 2 * (BAT_STROKE + FILL_PAD))
#define FILL_H      (BAT_H - 2 * (BAT_STROKE + FILL_PAD))

#define C_OUTLINE   LCD_RGBPACK(0xd4, 0xd8, 0xe0)
#define C_INSIDE    LCD_RGBPACK(0x10, 0x13, 0x18)
#define C_GLYPH     LCD_RGBPACK(0xff, 0xff, 0xff)
#define C_GREEN_TOP LCD_RGBPACK(0x9c, 0xe8, 0x6c)
#define C_GREEN_BOT LCD_RGBPACK(0x2c, 0x8c, 0x1c)
#define C_RED_TOP   LCD_RGBPACK(0xf0, 0x60, 0x50)
#define C_RED_BOT   LCD_RGBPACK(0xa0, 0x18, 0x10)
#define C_AMB_TOP   LCD_RGBPACK(0xf6, 0xd6, 0x68)
#define C_AMB_BOT   LCD_RGBPACK(0xa8, 0x70, 0x08)
#define C_WARN      LCD_RGBPACK(0xf4, 0xc0, 0x30)

/* The battery with its top at y, filled 'pct' percent (glossy rows from
 * top to bottom), and glyph g (or -1) centred in it */
static void big_battery(int y, int pct, int band, int g)
{
    int fill = pct <= 0 ? 0 : MAX(FILL_W * MIN(pct, 100) / 100, 3);
    unsigned top = band == 2 ? C_RED_TOP : band == 1 ? C_AMB_TOP : C_GREEN_TOP;
    unsigned bot = band == 2 ? C_RED_BOT : band == 1 ? C_AMB_BOT : C_GREEN_BOT;
    int j;

    lcd_set_viewport(NULL);
    rrect(BAT_X, y, BAT_W, BAT_H, corner_o_cov, CORNER_O_W, C_OUTLINE);
    rrect(BAT_X + BAT_STROKE, y + BAT_STROKE, BAT_W - 2 * BAT_STROKE,
          BAT_H - 2 * BAT_STROKE, corner_i_cov, CORNER_I_W, C_INSIDE);
    /* the nub: a short bar at the right, its outer corners squared */
    lcd_set_foreground(C_OUTLINE);
    lcd_fillrect(BAT_X + BAT_W + 1, y + (BAT_H - NUB_H) / 2, NUB_W - 1, NUB_H);
    if (band == 2 && fill < 4)                 /* an empty one still reads red */
        fill = 4;
    if (fill > 0)
    {
        for (j = 0; j < FILL_H; j++)
        {
            /* the upper third lighter (gloss), then the plain gradient */
            int a = j < FILL_H / 3 ? j * 6 / (FILL_H / 3) : 6 + (j - FILL_H / 3)
                    * 9 / (FILL_H - FILL_H / 3);
            lcd_set_foreground(ui_paint_blend(top, bot, MIN(a, 15)));
            lcd_hline(FILL_X, FILL_X + fill - 1, y + BAT_STROKE + FILL_PAD + j);
        }
    }
    if (g >= 0)
        ui_sys_glyph(g, BAT_X + (BAT_W - glyphs[g].w) / 2,
                     y + (BAT_H - glyphs[g].h) / 2,
                     g == UI_SYS_WARN ? C_WARN : C_GLYPH);
}

/* -- Charging / Charged ----------------------------------------------------- */

#define CHG_BAT_Y   96          /* the battery's top (centre ~y 127) */
#define CHG_LIT_WAKE (10 * HZ)  /* one charging_state() poll while lit */

struct chg {
    unsigned usb;               /* ui_usb_sessions() at enter */
    signed char fill_pct;       /* drawn: battery_level(), -1 none */
    bool charged;               /* drawn: "Charged" (plug) */
    bool key_down;              /* a key went down on this screen */
};
_Static_assert(sizeof(struct chg) <= UI_ARENA_B_CHARGING, "chg: arena bound");

static bool now_charged(void)
{
#if CONFIG_CHARGING >= CHARGING_MONITOR
    return !charging_state();
#else
    return false;
#endif
}

static int chg_enter(struct ui_screen *s, const void *arg)
{
    struct chg *c = ui_arena_alloc(sizeof(*c));

    (void)arg;
    if (!c)
        return -1;
    s->priv = c;
    s->header_w = UI_HEADER_DARK;
    c->usb = ui_usb_sessions();
    c->charged = now_charged();
    c->fill_pct = -1;
    s->title = str(c->charged ? LANG_IPODUI_CHARGED : LANG_IPODUI_CHARGING);
    UPDATE_TRACE("sys charging state=%s", c->charged ? "charged" : "charging");
    return 0;
}

static void chg_draw(struct ui_screen *s, unsigned dirty)
{
    struct chg *c = s->priv;

    if (dirty & UI_DIRTY_BODY)
        ui_dark_body(UI_DARK_HDR_H, LCD_HEIGHT - UI_DARK_HDR_H);
    else if (dirty & UI_DIRTY_BAND)
        ui_dark_body(CHG_BAT_Y, BAT_H);
    else
        return;
    c->fill_pct = c->charged ? 100 : battery_level();
    big_battery(CHG_BAT_Y, c->fill_pct, 0,
                c->charged ? UI_SYS_PLUG : UI_SYS_BOLT);
}

static void chg_pop(struct ui_screen *s, const char *why)
{
    UPDATE_TRACE("sys charging pop=%s", why);
    (void)why;                          /* trace only (no-op on target) */
    ui_pop_request(s);
}

static int chg_tick(struct ui_screen *s)
{
    struct chg *c = s->priv;
    bool charged;

    if (ui_usb_sessions() != c->usb)
    {
        chg_pop(s, "usb");              /* the USB screen took over */
        return TIMEOUT_BLOCK;
    }
#if CONFIG_CHARGING
    if (!charger_inserted())
    {
        chg_pop(s, "unplug");
        /* P18 Q9: the user put it to sleep on power; unplugged, it
         * sleeps (the screen below, paused, as the wake finds it) */
        ui_sleep_request(SLEEP_UNPLUGGED);
        return TIMEOUT_BLOCK;
    }
#endif
    charged = now_charged();
    if (charged != c->charged)
    {
        c->charged = charged;
        s->title = str(charged ? LANG_IPODUI_CHARGED : LANG_IPODUI_CHARGING);
        UPDATE_TRACE("sys charging state=%s", charged ? "charged" : "charging");
        ui_invalidate_band(CHG_BAT_Y, BAT_H);
    }
    else if (!charged && battery_level() != c->fill_pct)
        ui_invalidate_band(CHG_BAT_Y, BAT_H);
    return TIMEOUT_BLOCK;
}

static int chg_timeout(struct ui_screen *s)
{
    (void)s;
    return is_backlight_on(true) ? CHG_LIT_WAKE : TIMEOUT_BLOCK;
}

static enum ui_result chg_input(struct ui_screen *s, const struct ui_input *in)
{
    struct chg *c = s->priv;

    if (in->key == UI_KEY_NONE)
        return UI_HANDLED;              /* chords: nothing */
    if (in->key == UI_KEY_WHEEL)
    {
        chg_pop(s, "key");
        return UI_HANDLED;
    }
    /* leave on the release of a key pressed here, so the release never
     * reaches the screen below; PLAY does not resume (paused, as the
     * sleep left it) */
    if (!in->release)
        c->key_down = true;
    else if (c->key_down)
    {
        c->key_down = false;
        chg_pop(s, "key");
    }
    return UI_HANDLED;
}

const struct ui_screen_ops ui_sys_charging_screen = {
    .name    = "sys-charging",
    .enter   = chg_enter,
    .leave   = NULL,                    /* arena: released by the pop */
    .draw    = chg_draw,
    .input   = chg_input,
    .timeout = chg_timeout,
    .tick    = chg_tick,
};

/* -- Connect to Power ------------------------------------------------------- */

#define LOW_BAT_Y   74
#define LOW_TEXT_BASE 186
#define LOW_SHOW_TICKS (2 * HZ)
#define LOW_WAKE_TICKS (HZ / 4)         /* the LCD's wake after backlight_on */

static bool low_drawn;                  /* a frame is up, hold it later */

/* The image: the dark header with the battery and no title (the guide's
 * screen names none) and the big battery with the warning glyph. Glyphs
 * only (art/sys), no font: the boot guard draws it before any font is
 * loaded. */
static void low_image(bool empty)
{
    ui_dark_header(NULL, true);
    ui_dark_body(UI_DARK_HDR_H, LCD_HEIGHT - UI_DARK_HDR_H);
    big_battery(LOW_BAT_Y, empty ? 0 : 2, 2, UI_SYS_WARN);
}

/* One full frame: the image and the line. Waits up to LOW_WAKE_TICKS for
 * the LCD; false if it stayed asleep. */
static bool low_frame(bool empty)
{
    long end = current_tick + LOW_WAKE_TICKS;
    struct viewport vp;
    bool power = false;

    ui_fb_settle(0, 0, LCD_WIDTH, LCD_HEIGHT);  /* P19 screen truth: a takeover */

    backlight_on();                     /* also wakes a sleeping LCD */
#if defined(HAVE_LCD_ENABLE) || defined(HAVE_LCD_SLEEP)
    while (!lcd_active() && TIME_BEFORE(current_tick, end))
        sleep(1);
    if (!lcd_active())
        return false;
#else
    (void)end;
#endif
#if CONFIG_CHARGING
    power = charger_inserted();
#endif
    low_image(empty);
    memset(&vp, 0, sizeof(vp));
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.drawmode = DRMODE_FG;
    vp.font = ui_theme->font_ui;
    lcd_set_viewport(&vp);
    lcd_set_foreground(LCD_WHITE);
    ui_paint_text_centre(LCD_WIDTH / 2, LOW_TEXT_BASE, LCD_WIDTH - 20,
                         str(power ? LANG_IPODUI_CHARGING_WAIT
                                   : LANG_IPODUI_CONNECT_TO_POWER));
    lcd_set_viewport(NULL);
    lcd_update();
    return true;
}

void ui_sys_low_battery(bool empty)
{
    bool drawn = false;

    if (ui_shell_started())             /* the shell's fonts are there */
    {
        drawn = low_frame(empty);
        /* drawn over an unlit face (the battery monitor's shutdown
         * comes with no key): the face no longer owns the LCD */
        if (drawn)
            ui_unlit_foreign_draw();
    }
    UPDATE_TRACE("sys lowbatt empty=%d drawn=%d", (int)empty, (int)drawn);
    /* no wait here: the caller saves settings and resume first, then
     * calls ui_sys_low_battery_hold() (a weak cell must not brown out
     * before the save) */
    low_drawn = drawn;
}

/* The boot guard's screen (bootbatt.c), right after lcd_init(): no
 * font, no backlight driver, no disk yet, so the image alone (the
 * bootloader left the backlight on). The owner picks the final "dead
 * battery" design later; it goes here. Draws and returns; the guard
 * holds it. */
void ui_sys_dead_battery(void)
{
    low_image(false);
    lcd_update();
    UPDATE_TRACE("sys deadbatt drawn=%d", 1);
}

void ui_sys_low_battery_hold(void)
{
    if (low_drawn)
    {
        low_drawn = false;
        sleep(LOW_SHOW_TICKS);
    }
}
