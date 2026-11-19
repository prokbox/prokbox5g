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
 * iPod UI shell: the custom EQ editor (#82)
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

/* The custom EQ editor (#82): the 5 bands of one custom slot edited on
 * their own curve. Opened from Settings > EQ (choosing a Custom row) and
 * from Extras > Custom EQ (a list of the 3 slots).
 *
 * The curve is the editor (owner, 2026-11-12): 5 band columns with light
 * separators, a frequency bar moving across the selected band's 4 stops,
 * then a dB step; the 4 frequency choices are labelled under the plot with
 * leader lines (2026-11-13). Steps: the wheel picks a band; Select: the
 * wheel moves the band's frequency; Select: the wheel sets its level (Hold
 * Select: narrow / wide, bands 2-4); Select: done. Menu steps back out and
 * leaves from the band step. Play compares with EQ off, level-matched
 * (the same level plan, flat). Hold Select on the band step resets the
 * slot to flat.
 *
 * What you hear is what you see: every change goes to the codec through
 * ui_eq_live() (the same level plan as the presets), nothing is stored
 * until the screen is left: then ui_eq_custom_set() saves the slot (once,
 * and only when it changed) and ui_eq_live(NULL) puts the stored EQ back.
 *
 * Real corners: the codec runs its filters with the 48 kHz coefficients
 * at 44.1, 88.2 and 96 kHz, so a band's corner is its nominal frequency
 * times fs / 48000 (0.92 at 44.1, 2 at 96); the lower rates use their own
 * coefficients (corner = nominal) and 22.05 / 11.025 kHz use the 24 / 12
 * kHz ones. Labels, readout, ticks, band columns and the curve all show
 * the real corner at the current output rate (unknown: 44.1 kHz) and
 * follow it when the rate changes.
 *
 * A peak's handle sits ON the curve at its centre. A shelf's corner is
 * its half-gain point, so a +4 dB bass shelf reads only +2 dB there: its
 * handle stays on the corner's bar but at the plateau's height, the
 * shelf's full level, so it says what the readout says (owner,
 * 2026-11-13).
 *
 * The readout is three captioned cells, Band / Frequency / Level, the one
 * the wheel is editing in blue (owner's pick, 2026-11-13;
 * docs/design/eq-editor.md).
 *
 * Log (ipodui.log): "eqedit open N +0@105 ..." (the stored slot), one
 * line per settled band change (hy= its handle's row), "eqedit saved N
 * ...", "eqedit
 * cmp=off|eq". Drawing: only the changed bands of rows, at most ~30
 * frames a second, the curve computed once per change. */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "settings.h"
#include "dac_ext.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/panelart.h"
#include "ipodui/screen_stack.h"
#include "ipodui/uilog.h"
#include "ipodui/eqlab_core.h"
#include "ipodui/eq_custom.h"
#include "ipodui/screens/screens.h"
#include "ipodui/screens/eqedit.h"

#define HDR_H           20              /* the shell header, its bottom line */
#define N_REGIONS       5
#define R_READ          0x01
#define R_PLOT          0x02
#define R_FREQ          0x04
#define R_NAMES         0x08
#define R_HINT          0x10
#define R_ALL           0x1f

/* Row bands (y, h) per region in bit order READ, PLOT, FREQ, NAMES, HINT,
 * which is also row order: together they tile the rows under the header
 * with none left between, so the entry draw paints every row the screen
 * owns and each band's push covers its own rows, its separator included. */
#define RG_READ_Y 20
#define RG_READ_H 37
#define RG_PLOT_Y 57
#define RG_PLOT_H 117
#define RG_FREQ_Y 174
#define RG_FREQ_H 20
#define RG_NAMES_Y 194
#define RG_NAMES_H 20
#define RG_HINT_Y 214
#define RG_HINT_H (LCD_HEIGHT - RG_HINT_Y)
#define RG_SEP_Y  (RG_READ_Y + RG_READ_H - 1)

static const struct { unsigned char y, h; } rg[N_REGIONS] = {
    { RG_READ_Y, RG_READ_H }, { RG_PLOT_Y, RG_PLOT_H },
    { RG_FREQ_Y, RG_FREQ_H }, { RG_NAMES_Y, RG_NAMES_H },
    { RG_HINT_Y, RG_HINT_H },
};

#define PLOT_Y          RG_PLOT_Y
#define PLOT_H          RG_PLOT_H
#define Y_TOP           (PLOT_Y + 5)                /* +12 dB */
#define Y_BOT           (PLOT_Y + PLOT_H - 5)       /* -12 dB */
#define Y_ZERO          ((Y_TOP + Y_BOT) / 2)
#define PX_12DB         (Y_ZERO - Y_TOP)
#define CHOICE_TICK_H          8               /* the choice ticks, up from Y_BOT+4 */
#define X0              8
#define XW              304             /* columns X0 .. X0+XW-1 */
/* the plot's frequency axis, 20 Hz .. 24 kHz: a corner doubled at 96 kHz
 * (the treble shelf's 11.7 kHz) must still be on it. log2(24000) in Q8 */
#define LF_MIN          EQLAB_LF_MIN
#define LF_MAX          3725

#define COL_BG          LCD_RGBPACK(0xff, 0xff, 0xff)
#define COL_SEL         LCD_RGBPACK(0xdc, 0xe7, 0xfa)
#define COL_GRID        LCD_RGBPACK(0xe2, 0xe2, 0xe2)
#define COL_ZERO        LCD_RGBPACK(0xa8, 0xa8, 0xa8)
#define COL_EDGE        LCD_RGBPACK(0xc8, 0xc8, 0xc8)
#define COL_TICK        LCD_RGBPACK(0x9a, 0x9a, 0x9a)
#define COL_BLUE        LCD_RGBPACK(0x1e, 0x6c, 0xdc)
#define COL_DOT         LCD_RGBPACK(0x8c, 0x96, 0xa8)
#define COL_CURVE       LCD_RGBPACK(0x00, 0x00, 0x00)
#define COL_CURVE_OFF   LCD_RGBPACK(0xb4, 0xb4, 0xb4)
#define COL_GREY        LCD_RGBPACK(0x70, 0x70, 0x70)
#define COL_HINT        LCD_RGBPACK(0x8c, 0x8c, 0x8c)

#define FREQ_GAP        10              /* between the choices' labels */
#define FRAME_TICKS     (HZ / 30 + 1)   /* about 30 fps at most */
#define ARROW           6               /* arrow depth, px */

enum ed_step { STEP_BAND, STEP_FREQ, STEP_LEVEL };

/* The screen's state (the arena, freed with the screen): ~0.9 KB */
struct ed_screen
{
    struct dac_eq_band band[EQLAB_BANDS];   /* being edited */
    struct dac_eq_band stored[EQLAB_BANDS]; /* as opened (to see a change) */
    struct dac_eq_band logged[EQLAB_BANDS]; /* as last logged */
    int slot;
    int sel;                    /* the band */
    enum ed_step step;
    unsigned dirty;             /* R_* to draw and push */
    bool curve_stale;
    bool cmp;                   /* Compare: EQ off */
    unsigned long rate;         /* the output rate (0: unknown) */
    unsigned long filt;         /* the filter coefficients' rate */
    int shift;                  /* log2(rate / filt) in Q8 */
    long last_frame, last_poll;
    uint8_t y[XW];              /* the curve, one row per column */
};

static bool bands_equal(const struct dac_eq_band *a,
                        const struct dac_eq_band *b)
{
    int i;

    for (i = 0; i < EQLAB_BANDS; i++)
        if (a[i].gain != b[i].gain || a[i].freq != b[i].freq ||
            (eqlab_has_width(i) && a[i].wide != b[i].wide))
            return false;
    return true;
}

/* -- the codec's rate ----------------------------------------------------- */

static unsigned long codec_rate(void)
{
    struct dac_stats st;

    dac_get_stats(&st);
#ifdef SIMULATOR
    {
        const char *e = getenv("IPODUI_EQ_RATE");   /* test hook: a rate */

        if (e && atol(e) > 0)
            return (unsigned long)atol(e);
    }
#endif
    return st.rate_hz;
}

/* The rate whose coefficients the codec's filters use (wm8758.c
 * audiohw_set_frequency(): ADDCTRL SR) */
static unsigned long filter_rate(unsigned long r)
{
    if (r >= 44100) return 48000;
    if (r >= 32000) return 32000;
    if (r >= 22050) return 24000;
    if (r >= 16000) return 16000;
    if (r >= 11025) return 12000;
    return 8000;
}

static void rate_set(struct ed_screen *ls, unsigned long r)
{
    unsigned long use = r ? r : 44100;

    ls->rate = r;
    ls->filt = filter_rate(use);
    ls->shift = eqlab_log2_q8(use) - eqlab_log2_q8(ls->filt);
}

/* a corner at the current rate, Hz */
static unsigned real_hz(const struct ed_screen *ls, unsigned nominal)
{
    unsigned long use = ls->rate ? ls->rate : 44100;

    return (unsigned)(((unsigned long)nominal * use + ls->filt / 2) / ls->filt);
}

/* -- geometry ------------------------------------------------------------- */

static int x_of_lf(int lf)
{
    return X0 + (lf - LF_MIN) * (XW - 1) / (LF_MAX - LF_MIN);
}

static int x_of_hz(unsigned hz)
{
    return x_of_lf(eqlab_log2_q8(hz));
}

static int y_of_db(int db)
{
    return Y_ZERO - db * PX_12DB / EQLAB_GAIN_MAX;
}

/* band i's column, x0 .. x1 (inclusive), at the real corners */
static void band_cols(const struct ed_screen *ls, int i, int *x0, int *x1)
{
    *x0 = i ? x_of_hz(real_hz(ls, eqlab_edge_hz[i - 1])) + 1 : X0;
    *x1 = i < EQLAB_BANDS - 1
          ? x_of_hz(real_hz(ls, eqlab_edge_hz[i])) - 1 : X0 + XW - 1;
}

static int tick_x(const struct ed_screen *ls, int i, int f)
{
    return x_of_hz(real_hz(ls, eqlab_hz[i][f]));
}

static int marker_x(const struct ed_screen *ls, int i)
{
    return tick_x(ls, i, ls->band[i].freq & 3);
}

/* the curve's row at column x */
static int curve_y(const struct ed_screen *ls, int x)
{
    return ls->y[MIN(MAX(x - X0, 0), XW - 1)];
}

/* Band i's handle row. A peak's: on the curve at its centre. A shelf's
 * (Bass, Treble): its full level, the plateau, kept in the plot; at its
 * corner the curve is only half way there. */
static int handle_y(const struct ed_screen *ls, int i)
{
    if (i == 0 || i == EQLAB_BANDS - 1)
        return MIN(MAX(y_of_db(ls->band[i].gain), PLOT_Y),
                   PLOT_Y + PLOT_H - 2);
    return curve_y(ls, marker_x(ls, i));
}

/* A plot label's box (x, y, w, h) is clear of the handles: the selected
 * band's bar, arrows and handle (its column, 12 px either side) and every
 * other band's handle (with a pixel to spare) */
static bool label_clear(const struct ed_screen *ls, int x, int y, int w, int h)
{
    int i;

    for (i = 0; i < EQLAB_BANDS; i++)
    {
        int hx = marker_x(ls, i), hy = handle_y(ls, i);

        if (i == ls->sel)
        {
            if (hx >= x - 12 && hx <= x + w + 12)
                return false;
        }
        else if (hx + 3 >= x && hx - 3 < x + w && hy + 3 >= y && hy - 3 < y + h)
            return false;
    }
    return true;
}

/* The curve at the real corners; recomputed only after a change.
 * APPROXIMATE (eqlab_core.h eqlab_db_q8()). */
static void curve_compute(struct ed_screen *ls)
{
    int c;

    for (c = 0; c < XW; c++)
    {
        int lf = LF_MIN + c * (LF_MAX - LF_MIN) / (XW - 1);
        int y = Y_ZERO - eqlab_db_q8(ls->band, lf, ls->shift) * PX_12DB
                         / (EQLAB_GAIN_MAX * 256);
        ls->y[c] = (uint8_t)MIN(MAX(y, PLOT_Y), PLOT_Y + PLOT_H - 2);
    }
    ls->curve_stale = false;
}

/* -- text ------------------------------------------------------------------ */

static void text_vp(struct viewport *vp, int font)
{
    memset(vp, 0, sizeof(*vp));
    vp->width = LCD_WIDTH;
    vp->height = LCD_HEIGHT;
    vp->flags = VP_DEFAULT_FLAGS;
    vp->drawmode = DRMODE_FG;
    vp->font = font;
    lcd_set_viewport(vp);
}

static int text_w(const char *s, int font)
{
    int w, h;

    font_getstringsize((const unsigned char *)s, &w, &h, font);
    return w;
}

static const char *band_name(int i)
{
    return str(LANG_IPODUI_EQED_BAND_1 + i);
}

static void fill_band(int y, int h, unsigned col)
{
    lcd_set_viewport(NULL);
    lcd_set_foreground(col);
    lcd_fillrect(0, y, LCD_WIDTH, h);
}

/* -- drawing ---------------------------------------------------------------- */

static void draw_readout(struct ed_screen *ls)
{
    const struct ui_theme *t = ui_theme;
    const struct dac_eq_band *b = &ls->band[ls->sel];
    struct viewport vp;
    char hz[12], g[8];

    fill_band(RG_READ_Y, RG_READ_H, COL_BG);
    /* the separator between the readout and the plot: the header's
     * bottom line, solid */
    lcd_set_foreground(t->header_rows[UI_HEADER_ROWS - 1]);
    lcd_hline(0, LCD_WIDTH - 1, RG_SEP_Y);
    eqlab_fmt_hz(hz, sizeof(hz), real_hz(ls, eqlab_hz[ls->sel][b->freq & 3]),
                 str(LANG_IPODUI_DECIMAL_SEP));
    snprintf(g, sizeof(g), b->gain ? "%+d" : "%d", (int)b->gain);
    if (ls->cmp)
    {
        text_vp(&vp, t->font_title);
        lcd_set_foreground(COL_GREY);
        ui_paint_text_centre(LCD_WIDTH / 2, RG_READ_Y + 23,
                             LCD_WIDTH - 8, str(LANG_IPODUI_EQED_COMPARE));
        lcd_set_viewport(NULL);
        return;
    }
    {
        /* three captioned cells; the one being edited in blue */
        const char *cap[3] = { str(LANG_IPODUI_EQED_CAP_BAND),
                               str(LANG_IPODUI_EQED_CAP_FREQ),
                               str(LANG_IPODUI_EQED_CAP_LEVEL) };
        char val[3][24];
        int i;

        snprintf(val[0], sizeof(val[0]), "%s", band_name(ls->sel));
        snprintf(val[1], sizeof(val[1]), "%s", hz);
        if (eqlab_has_width(ls->sel))
            snprintf(val[2], sizeof(val[2]), "%s dB %s", g,
                     str(b->wide ? LANG_IPODUI_EQED_WIDE
                                 : LANG_IPODUI_EQED_NARROW));
        else
            snprintf(val[2], sizeof(val[2]), "%s dB", g);
        for (i = 0; i < 3; i++)
        {
            int cx = LCD_WIDTH * (2 * i + 1) / 6;
            bool on = (int)ls->step == i;

            text_vp(&vp, t->font_small);
            lcd_set_foreground(on ? COL_BLUE : COL_HINT);
            ui_paint_text_centre(cx, RG_READ_Y + 14, 104, cap[i]);
            vp.font = t->font_title;
            lcd_set_viewport(&vp);
            lcd_set_foreground(on ? COL_BLUE : t->list_fg);
            ui_paint_text_centre(cx, RG_READ_Y + 30, 104, val[i]);
        }
        lcd_set_viewport(NULL);
    }
}

/* a filled triangle pointing dir (0 left, 1 right, 2 up, 3 down), its
 * tip at (x, y), ARROW px deep */
static void tri(int x, int y, int dir)
{
    int k;

    for (k = 0; k < ARROW; k++)
    {
        switch (dir)
        {
            case 0: lcd_vline(x + k, y - k, y + k); break;
            case 1: lcd_vline(x - k, y - k, y + k); break;
            case 2: lcd_hline(x - k, x + k, y + k); break;
            default: lcd_hline(x - k, x + k, y - k); break;
        }
    }
}

static void dot(int x, int y, int r)
{
    lcd_fillrect(x - r + 1, y - r, 2 * r - 1, 2 * r + 1);
    lcd_fillrect(x - r, y - r + 1, 2 * r + 1, 2 * r - 1);
}

/* a 7 x 7 ring (the frequency step's choices) */
static void ring(int x, int y)
{
    lcd_hline(x - 2, x + 2, y - 3);
    lcd_hline(x - 2, x + 2, y + 3);
    lcd_vline(x - 3, y - 2, y + 2);
    lcd_vline(x + 3, y - 2, y + 2);
}

static const unsigned short decade_hz[3] = { 100, 1000, 10000 };
static const char *const decade_name[3] = { "100", "1k", "10k" };

static void draw_plot(struct ed_screen *ls)
{
    const struct ui_theme *t = ui_theme;
    struct viewport vp;
    int i, c, x0, x1, mx, my, fh;

    if (ls->curve_stale)
        curve_compute(ls);
    fill_band(PLOT_Y, PLOT_H, COL_BG);
    /* the selected band's column */
    band_cols(ls, ls->sel, &x0, &x1);
    lcd_set_foreground(COL_SEL);
    lcd_fillrect(x0, PLOT_Y + 1, x1 - x0 + 1, PLOT_H - 2);
    /* grid: +-6 dB light, 0 dB darker */
    lcd_set_foreground(COL_GRID);
    lcd_hline(X0, X0 + XW - 1, y_of_db(6));
    lcd_hline(X0, X0 + XW - 1, y_of_db(-6));
    lcd_set_foreground(COL_ZERO);
    lcd_hline(X0, X0 + XW - 1, Y_ZERO);
    /* the decades: light lines, under the band edges */
    lcd_set_foreground(COL_GRID);
    for (i = 0; i < 3; i++)
        lcd_vline(x_of_hz(decade_hz[i]), PLOT_Y + 1, Y_BOT - 1);
    /* band edges */
    lcd_set_foreground(COL_EDGE);
    for (i = 0; i < EQLAB_BANDS - 1; i++)
        lcd_vline(x_of_hz(real_hz(ls, eqlab_edge_hz[i])), PLOT_Y + 1,
                  Y_BOT + 4);
    /* the scale: dB at the left, the decades by their lines; a label gives
     * way to a handle (a shelf's sits at its full level, so it can reach
     * them) and to the selected band's bar and arrows */
    text_vp(&vp, t->font_small);
    fh = font_get(t->font_small)->height;
    lcd_set_foreground(COL_HINT);
    for (i = 0; i < 5; i++)
    {
        const char *s = i < 3 ? decade_name[i] : i == 3 ? "+12" : "-12";
        int x = i < 3 ? x_of_hz(decade_hz[i]) + 2 : X0 + 1;
        int y = i < 3 ? Y_BOT - fh - 1
                      : i == 3 ? Y_TOP - fh / 2 + 1 : Y_BOT - fh / 2 - 1;

        if (label_clear(ls, x, y, text_w(s, t->font_small), fh))
            lcd_putsxy(x, y, s);
    }
    lcd_set_viewport(NULL);
    mx = marker_x(ls, ls->sel);
    /* every band's 4 choices: ticks; the selected band's in blue, rings
     * on the frequency step */
    for (i = 0; i < EQLAB_BANDS; i++)
    {
        int f;

        for (f = 0; f < EQLAB_FREQS; f++)
        {
            int x = tick_x(ls, i, f);

            if (i == ls->sel && ls->step == STEP_FREQ)
            {
                lcd_set_foreground(COL_BLUE);
                ring(x, Y_BOT + 1);
            }
            else
            {
                lcd_set_foreground(i == ls->sel ? COL_BLUE : COL_TICK);
                lcd_vline(x, Y_BOT + 4 - CHOICE_TICK_H + 1, Y_BOT + 4);
            }
        }
    }
    /* the selected band's bar */
    lcd_set_foreground(COL_BLUE);
    lcd_vline(mx, PLOT_Y + 1, Y_BOT + 4);
    /* the curve, 2 px; grey while Compare shows EQ off */
    lcd_set_foreground(ls->cmp ? COL_CURVE_OFF : COL_CURVE);
    for (c = 0; c < XW; c++)
    {
        int a = ls->y[c > 0 ? c - 1 : 0], b = ls->y[c];

        lcd_vline(X0 + c, MIN(a, b), MAX(a, b) + 1);
    }
    /* the handles on each band's bar (handle_y()); the selected band's in
     * blue */
    for (i = 0; i < EQLAB_BANDS; i++)
    {
        if (i == ls->sel)
            continue;
        lcd_set_foreground(COL_DOT);
        dot(marker_x(ls, i), handle_y(ls, i), 2);
    }
    my = handle_y(ls, ls->sel);
    lcd_set_foreground(COL_BLUE);
    dot(mx, my, 4);
    if (ls->step == STEP_FREQ)
    {
        /* none past the end of the choices: the wheel stops there */
        int f = ls->band[ls->sel].freq & 3;

        if (f > 0)
            tri(mx - 14, Y_BOT - 14, 0);
        if (f < EQLAB_FREQS - 1)
            tri(mx + 14, Y_BOT - 14, 1);
    }
    else if (ls->step == STEP_LEVEL)
    {
        /* the arrows stay inside the plot band (the neighbours are not
         * redrawn with it); none past the end of the range */
        int g = ls->band[ls->sel].gain;

        if (g < EQLAB_GAIN_MAX)
            tri(mx, MAX(my - 6 - ARROW, PLOT_Y + 1), 2);
        if (g > -EQLAB_GAIN_MAX)
            tri(mx, MIN(my + 6 + ARROW, PLOT_Y + PLOT_H - 1), 3);
    }
}

/* The selected band's 4 choices, labelled under the plot on every step.
 * Their ticks are ~12 px apart, closer than a label is wide, so the labels
 * are spread out in tick order, centred under the ticks (kept on the
 * screen), and a leader joins each tick to its label: the outer two turn
 * on the row above the inner two, so no leaders cross. The choice in use:
 * bold, blue, its leader blue. The labels are the REAL corners. */
static void draw_freqs(struct ed_screen *ls)
{
    const struct ui_theme *t = ui_theme;
    struct viewport vp;
    char lbl[EQLAB_FREQS][8];
    int w[EQLAB_FREQS], tx[EQLAB_FREQS], lx[EQLAB_FREQS];
    int f, cur = ls->band[ls->sel].freq & 3, total = 0, x, mid = 0;

    fill_band(RG_FREQ_Y, RG_FREQ_H, COL_BG);
    for (f = 0; f < EQLAB_FREQS; f++)
    {
        eqlab_fmt_hz_short(lbl[f], sizeof(lbl[f]),
                           real_hz(ls, eqlab_hz[ls->sel][f]),
                           str(LANG_IPODUI_DECIMAL_SEP));
        w[f] = text_w(lbl[f], f == cur ? t->font_title : t->font_small);
        tx[f] = tick_x(ls, ls->sel, f);
        total += w[f];
        mid += tx[f];
    }
    total += (EQLAB_FREQS - 1) * FREQ_GAP;
    x = mid / EQLAB_FREQS - total / 2;
    x = MIN(MAX(x, 4), LCD_WIDTH - 4 - total);
    for (f = 0; f < EQLAB_FREQS; f++)
    {
        lx[f] = x + w[f] / 2;
        x += w[f] + FREQ_GAP;
    }
    for (f = 0; f < EQLAB_FREQS; f++)
    {
        int yk = RG_FREQ_Y + (f == 0 || f == EQLAB_FREQS - 1 ? 1 : 3);

        lcd_set_foreground(f == cur ? COL_BLUE : COL_EDGE);
        lcd_vline(tx[f], RG_FREQ_Y, yk);
        lcd_hline(MIN(tx[f], lx[f]), MAX(tx[f], lx[f]), yk);
        lcd_vline(lx[f], yk, RG_FREQ_Y + 6);
    }
    text_vp(&vp, t->font_small);
    for (f = 0; f < EQLAB_FREQS; f++)
    {
        vp.font = f == cur ? t->font_title : t->font_small;
        lcd_set_viewport(&vp);
        lcd_set_foreground(f == cur ? COL_BLUE : COL_GREY);
        ui_paint_text_centre(lx[f], RG_FREQ_Y + 17, w[f] + 2, lbl[f]);
    }
    lcd_set_viewport(NULL);
}

/* The band names' left edges along the axis: each centred on its column,
 * then pushed apart where neighbours would come within NAME_GAP px (a
 * longer translation, or the columns of the 88.2/96 kHz axis), and kept
 * on the screen. Names whose total is wider than the screen share it
 * equally and end in "..." (traced). Pure layout: x[] and the width each
 * name may take, w[] (in: measured, out: allowed). */
#define NAME_GAP    6
#define NAME_EDGE   2
static void names_layout(const int *cx, int *w, int *x, int n)
{
    int i, total = 0, room = LCD_WIDTH - 2 * NAME_EDGE - (n - 1) * NAME_GAP;

    for (i = 0; i < n; i++)
        total += w[i];
    if (total > room)
        for (i = 0; i < n; i++)
            w[i] = MIN(w[i], room / n);
    for (i = 0; i < n; i++)
    {
        x[i] = cx[i] - w[i] / 2;
        if (i && x[i] < x[i - 1] + w[i - 1] + NAME_GAP)
            x[i] = x[i - 1] + w[i - 1] + NAME_GAP;
    }
    if (x[n - 1] + w[n - 1] > LCD_WIDTH - NAME_EDGE)
        x[n - 1] = LCD_WIDTH - NAME_EDGE - w[n - 1];
    for (i = n - 2; i >= 0; i--)
        if (x[i] + w[i] + NAME_GAP > x[i + 1])
            x[i] = x[i + 1] - NAME_GAP - w[i];
    if (x[0] < NAME_EDGE)
        x[0] = NAME_EDGE;
}

static void draw_names(struct ed_screen *ls)
{
    const struct ui_theme *t = ui_theme;
    struct viewport vp;
    int i, x0, x1, cx[EQLAB_BANDS], w[EQLAB_BANDS], x[EQLAB_BANDS];

    fill_band(RG_NAMES_Y, RG_NAMES_H, COL_BG);
    text_vp(&vp, t->font_small);
    /* the names are wider than the middle columns: centred on them and
     * allowed past their edges, never into each other (P20: German and
     * Dutch names met at 88.2/96 kHz) */
    for (i = 0; i < EQLAB_BANDS; i++)
    {
        band_cols(ls, i, &x0, &x1);
        cx[i] = (x0 + x1) / 2;
        w[i] = text_w(band_name(i), t->font_small);
    }
    names_layout(cx, w, x, EQLAB_BANDS);
    for (i = 0; i < EQLAB_BANDS; i++)
    {
        lcd_set_foreground(i == ls->sel ? COL_BLUE : COL_GREY);
        /* centred in its slot: cut with "..." (and traced) if narrowed */
        ui_paint_text_centre(x[i] + w[i] / 2, RG_NAMES_Y + 14, w[i],
                             band_name(i));
        if (i)
            UPDATE_TRACE("eqedit name %d x=%d w=%d gap=%d", i, x[i], w[i],
                         x[i] - x[i - 1] - w[i - 1]);
    }
    lcd_set_viewport(NULL);
}

static void draw_hint(struct ed_screen *ls)
{
    const struct ui_theme *t = ui_theme;
    struct viewport vp;
    int s;

    fill_band(RG_HINT_Y, RG_HINT_H, COL_BG);
    switch (ls->step)
    {
        case STEP_FREQ:
            s = LANG_IPODUI_EQED_HINT_FREQ;
            break;
        case STEP_LEVEL:
            s = eqlab_has_width(ls->sel) ? LANG_IPODUI_EQED_HINT_LEVEL_W
                                         : LANG_IPODUI_EQED_HINT_LEVEL;
            break;
        default:
            s = LANG_IPODUI_EQED_HINT_BAND;
            break;
    }
    text_vp(&vp, t->font_small);
    lcd_set_foreground(COL_HINT);
    ui_paint_text_centre(LCD_WIDTH / 2, RG_HINT_Y + (RG_HINT_H > 22 ? 15 : 14),
                         LCD_WIDTH - 4, str(s));
    lcd_set_viewport(NULL);
}

static void draw_regions(struct ed_screen *ls, unsigned r)
{
    if (r & R_READ)
        draw_readout(ls);
    if (r & R_PLOT)
        draw_plot(ls);
    if (r & R_FREQ)
        draw_freqs(ls);
    if (r & R_NAMES)
        draw_names(ls);
    if (r & R_HINT)
        draw_hint(ls);
}

/* -- the codec and the log ---------------------------------------------------- */

static bool codec_m128(void)
{
    struct dac_stats st;

    dac_get_stats(&st);
    return st.m128;
}

/* the curve on the codec: the edited one, or flat while Compare shows EQ
 * off (the same level plan, so the A/B stays level-matched) */
static void ed_apply(struct ed_screen *ls)
{
    if (ls->cmp)
    {
        struct dac_eq_band flat[EQLAB_BANDS];

        eqlab_flat(flat);
        ui_eq_live(flat);
    }
    else
        ui_eq_live(ls->band);
}

/* One line per band changed since the last log */
static void ed_log_changes(struct ed_screen *ls)
{
    int i;

    for (i = 0; i < EQLAB_BANDS; i++)
    {
        const struct dac_eq_band *b = &ls->band[i];

        if (!memcmp(b, &ls->logged[i], sizeof(*b)))
            continue;
        ls->logged[i] = *b;
        if (ls->curve_stale)
            curve_compute(ls);          /* a peak's handle rides it */
        ui_log("eqedit", "s%d b%d f=%u g=%+d bw=%c hy=%d m128=%d pc=%d fs=%lu",
               ls->slot + 1, i + 1, eqlab_hz[i][b->freq & 3], (int)b->gain,
               eqlab_has_width(i) ? (b->wide ? 'w' : 'n') : '-',
               handle_y(ls, i), (int)codec_m128(), eqlab_precut(ls->band),
               ls->rate);
    }
}

static void ed_log_slot(const char *what, int slot,
                        const struct dac_eq_band *b)
{
    char text[EQLAB_TEXT_LEN];

    eqlab_format(b, text, sizeof(text));
    ui_log("eqedit", "%s %d %s", what, slot + 1, text + 4);   /* no "eq1 " */
}

/* -- the screen --------------------------------------------------------------- */

static void status_poll(struct ed_screen *ls)
{
    unsigned long r = codec_rate();

    if (r != ls->rate)
    {
        rate_set(ls, r);
        ls->curve_stale = true;     /* the corners move with the rate */
        ls->dirty |= R_ALL;
    }
    ls->last_poll = current_tick;
}

/* a band value changed: the codec now, the drawing at the next frame */
static void ed_changed(struct ed_screen *ls)
{
    ls->cmp = false;
    ed_apply(ls);
    ls->curve_stale = true;
    ls->dirty |= R_READ | R_PLOT;
    status_poll(ls);
}

static void ed_reset(struct ed_screen *ls)
{
    eqlab_flat(ls->band);
    ls->cmp = false;
    ed_apply(ls);
    ls->curve_stale = true;         /* before the log: hy= rides it */
    ed_log_changes(ls);
    ls->dirty |= R_ALL;
    status_poll(ls);
}

static void ed_compare(struct ed_screen *ls)
{
    ed_log_changes(ls);             /* what is being compared */
    ls->cmp = !ls->cmp;
    ed_apply(ls);
    ui_log("eqedit", "cmp=%s m128=%d pc=%d", ls->cmp ? "off" : "eq",
           (int)codec_m128(), ls->cmp ? 0 : eqlab_precut(ls->band));
    ls->dirty |= R_READ | R_PLOT;
    status_poll(ls);
}

static int ed_enter(struct ui_screen *s, const void *arg)
{
    struct ed_screen *ls;
    int slot = (int)(intptr_t)arg;

    if (slot < 0 || slot >= EQ_CUSTOM_SLOTS)
        return -1;
    ls = ui_arena_alloc(sizeof(*ls));
    if (!ls)
        return -1;
    memset(ls, 0, sizeof(*ls));
    s->priv = ls;
    s->title = ui_eq_custom_name(slot);
    s->header_w = 0;                /* the shell's header: the slot's name */
    ls->slot = slot;
    ui_eq_custom_get(slot, ls->band);
    memcpy(ls->stored, ls->band, sizeof(ls->stored));
    memcpy(ls->logged, ls->band, sizeof(ls->logged));
    ls->sel = 3;                    /* Presence, the middle of the voice */
    ls->step = STEP_BAND;
    rate_set(ls, codec_rate());
    ls->curve_stale = true;
    ls->last_poll = current_tick;
    ed_log_slot("open", slot, ls->band);
    ed_apply(ls);                   /* what is drawn is what plays */
    return 0;
}

static void ed_leave(struct ui_screen *s)
{
    struct ed_screen *ls = s->priv;

    ed_log_changes(ls);
    if (!bands_equal(ls->band, ls->stored))
    {
        ui_eq_custom_set(ls->slot, ls->band);
        ed_log_slot("saved", ls->slot, ls->band);
    }
    ui_eq_live(NULL);               /* the stored EQ again, Compare or not */
}

static void ed_draw(struct ui_screen *s, unsigned dirty)
{
    struct ed_screen *ls = s->priv;

    if (dirty & (UI_DIRTY_BODY | UI_DIRTY_HEADER | UI_DIRTY_BAND))
    {
        draw_regions(ls, R_ALL);
        ls->dirty = 0;
    }
}

static enum ui_result ed_input(struct ui_screen *s, const struct ui_input *in)
{
    struct ed_screen *ls = s->priv;
    struct dac_eq_band *b = &ls->band[ls->sel];
    int d;

    switch (in->key)
    {
        case UI_KEY_WHEEL:
            if (!in->wheel_steps)
                return UI_HANDLED;
            d = in->wheel_steps > 0 ? 1 : -1;   /* one step per detent */
            if (ls->step == STEP_BAND)
            {
                int sel = MIN(MAX(ls->sel + d, 0), EQLAB_BANDS - 1);

                if (sel != ls->sel)
                {
                    ls->sel = sel;
                    ls->dirty |= R_READ | R_PLOT | R_FREQ | R_NAMES;
                }
            }
            else if (ls->step == STEP_FREQ)
            {
                int f = MIN(MAX((int)b->freq + d, 0), EQLAB_FREQS - 1);

                if (f != b->freq)
                {
                    b->freq = (uint8_t)f;
                    ed_changed(ls);
                    ls->dirty |= R_FREQ;        /* the choice's label */
                }
            }
            else
            {
                int g = MIN(MAX((int)b->gain + d, -EQLAB_GAIN_MAX),
                            EQLAB_GAIN_MAX);

                if (g != b->gain)
                {
                    b->gain = (int8_t)g;
                    ed_changed(ls);
                }
            }
            return UI_HANDLED;
        case UI_KEY_SELECT:
            if (!in->release && in->long_press)
            {
                /* hold: reset on the band step, the width on the level
                 * step (bands 2-4) */
                if (ls->step == STEP_BAND)
                    ed_reset(ls);
                else if (ls->step == STEP_LEVEL && eqlab_has_width(ls->sel))
                {
                    b->wide = !b->wide;
                    ed_changed(ls);
                }
                return UI_HANDLED;
            }
            if (!in->release || in->long_press)
                return UI_HANDLED;
            if (ls->step == STEP_BAND)
                ls->step = STEP_FREQ;
            else if (ls->step == STEP_FREQ)
                ls->step = STEP_LEVEL;
            else
            {
                ls->step = STEP_BAND;
                ed_log_changes(ls);
            }
            ls->dirty |= R_READ | R_PLOT | R_HINT;
            return UI_HANDLED;
        case UI_KEY_MENU:
            if (!in->release || in->long_press)
                return UI_HANDLED;
            if (ls->step == STEP_BAND)
                return UI_POP;
            ls->step = ls->step == STEP_LEVEL ? STEP_FREQ : STEP_BAND;
            if (ls->step == STEP_BAND)
                ed_log_changes(ls);
            ls->dirty |= R_READ | R_PLOT | R_HINT;
            return UI_HANDLED;
        case UI_KEY_PLAY:
            if (in->release && !in->long_press)
                ed_compare(ls);
            return UI_HANDLED;
        default:
            return UI_HANDLED;          /* the skips: nothing */
    }
}

/* Draws and pushes the changed regions, at most ~30 times a second (a
 * fast spin drops frames, never queues them); polls the codec's rate once
 * a second */
static int ed_tick(struct ui_screen *s)
{
    struct ed_screen *ls = s->priv;
    long now = current_tick;
    unsigned r;
    int i, y0 = -1, y1 = 0;

    if (TIME_AFTER(now, ls->last_poll + HZ - 1))
        status_poll(ls);
    if (!ls->dirty)
        return HZ;
    if (TIME_BEFORE(now, ls->last_frame + FRAME_TICKS))
        return ls->last_frame + FRAME_TICKS - now;
    r = ls->dirty;
    ls->dirty = 0;
    ls->last_frame = now;
    draw_regions(ls, r);
    /* one full-width push per run of neighbouring regions (bit order is
     * row order and the bands tile the rows, so a run is contiguous) */
    for (i = 0; i <= N_REGIONS; i++)
    {
        bool d = i < N_REGIONS && (r & (1u << i));
        bool contig = d && y0 >= 0 && rg[i].y == y1;

        if (y0 >= 0 && !contig)
        {
            ui_lcd_push("eqedit", 0, y0, LCD_WIDTH, y1 - y0);
            y0 = -1;
        }
        if (d && y0 < 0)
            y0 = rg[i].y;
        if (d)
            y1 = rg[i].y + rg[i].h;
    }
    return HZ;
}

const struct ui_screen_ops ui_eqedit_screen = {
    .name    = "eqedit",
    .enter   = ed_enter,
    .leave   = ed_leave,
    .draw    = ed_draw,
    .input   = ed_input,
    .timeout = NULL,
    .tick    = ed_tick,
};

int ui_eqedit_open(int slot)
{
    return ui_push(&ui_eqedit_screen, (const void *)(intptr_t)slot);
}

/* -- Extras > Custom EQ: the 3 slots ---------------------------------------- */

static int slots_count(void)
{
    return EQ_CUSTOM_SLOTS;
}

static int slots_get(void)
{
    return 0;
}

static int slots_set(int idx)
{
    ui_eqedit_open(idx);
    return 0;                           /* the list stays under the editor */
}

static const char *slots_label(int idx, char *buf, size_t len)
{
    (void)buf; (void)len;
    return ui_eq_custom_name(idx);
}

static const struct ui_vlist slots_list = {
    LANG_IPODUI_X_EQ, PANELART_EQ, slots_count, slots_get, slots_set,
    slots_label, slots_label };

UI_VLIST_SCREEN(ui_eqslots_screen, "eqslots", &slots_list);
