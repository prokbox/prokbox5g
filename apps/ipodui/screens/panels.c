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
 * iPod UI shell: split-screen icon panels (#38)
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

/* The icon panels of the split screens (spec settings.md "Split screen",
 * catalogue 7.1, owner photos settings-*.jpg and mov-main-panel-*.jpg):
 * the Podcasts count, the Extras clock, the capacity panel (Settings and
 * Settings > About) and the Settings tiles.
 *
 * Every panel is the slate-to-navy gradient (one lcd_hline per row) with
 * white text, and its art is composed in place over it (8b-3): metal
 * tiles from the material kit, glyphs as 4-bit metallic shading masks
 * (panelart.h) through per-row palettes, and a head-on reflection on the
 * surface they stand on (spec settings.md "Panel tiles"). Nothing here
 * allocates, boosts, pushes or touches storage; the art is compiled in
 * (apps/ipodui/art/panelart.py, original drawings). */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "mv.h"
#include "timefuncs.h"
#include "fixedpoint.h"
#include "settings.h"
#include "lang.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/library.h"
#ifdef HAVE_BCMFW_VIDEO
#include "ipodui/libfile.h"     /* struct libfile_vstats */
#endif
#include "ipodui/screens/panels.h"
/* the wheel art lives once, in ipodui/panelart.c (Phase 9) */
#define panelart_wheel_a   panelart_wheel_a_tu
#define panelart_wheel_rgb panelart_wheel_rgb_tu
#include "ipodui/panelart.h"
#include "ipodui/caldigits.h"
#include "ipodui/clockart.h"        /* the Clocks page's west half (W2-W:
                                     * resident for the reveal) */
#include "ipodui/extrasart.h"       /* the Extras rows' icons (resident: a
                                     * module's row shows it before the
                                     * module loads) */
#include "ipodui/extras.h"
#include "ipodui/stopwatch.h"      /* ui_preview_x_stopwatch */
#include "ipodui/material.h"
#undef panelart_wheel_a
#undef panelart_wheel_rgb
extern const unsigned char *const panelart_wheel_a;
extern const unsigned short *const panelart_wheel_rgb;

/* The panels' time (the Extras clock, the calendar page). In a scripted
 * simulator run it is pinned (sim_panel_clock(): the "sim_clock" time,
 * else 10:08; the date Tuesday 6 October 2026), so goldens never depend
 * on the host's clock. An invalid RTC time (never set, a flat clock
 * battery) shows a sane page instead of indexing the month strings with
 * garbage: midnight, Thursday 1 January 2026. */
#ifdef SIMULATOR
int sim_panel_clock(void);              /* sim_cmd_clock.c */
extern const char *sim_script_file;     /* sim_script.c */
#endif

static struct tm *panel_time(void)
{
    static struct tm fixed;
    struct tm *tm;
#ifdef SIMULATOR
    int mod = sim_panel_clock();

    if (mod >= 0)
    {
        memset(&fixed, 0, sizeof(fixed));
        fixed.tm_year = 2026 - 1900;
        fixed.tm_mon = 9;
        fixed.tm_mday = 6;
        fixed.tm_wday = 2;
        fixed.tm_yday = 278;
        fixed.tm_hour = mod / 60;
        fixed.tm_min = mod % 60;
        return &fixed;
    }
#endif
    tm = get_time();
    if (tm && valid_time(tm))
        return tm;
    memset(&fixed, 0, sizeof(fixed));
    fixed.tm_year = 2026 - 1900;
    fixed.tm_mday = 1;
    fixed.tm_wday = 4;
    return &fixed;
}

int ui_panel_tile_wake(int art)
{
    struct tm *tm;

    if (art != UI_TILE_CALENDAR)
        return TIMEOUT_BLOCK;
    /* the page turns at midnight: drawn again a second after it */
    tm = panel_time();
    return (24 * 3600 - (tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec)
            + 1) * HZ;
}

#if LCD_PIXELFORMAT != RGB565
#error "panels.c composes RGB565"
#endif


/* Layout (panel coordinates, ~ from the straight-on owner photos) */
#define CX              (UI_PANEL_W / 2)
#define TEXT_W          (UI_PANEL_W - 12)
#define TITLE_BASE      30      /* item name, centre y ~25 */
#define TILE            76      /* tile, y 80-155 */
#define TILE_X          ((UI_PANEL_W - TILE) / 2)
#define TILE_Y          80
#define REFL_GAP        1       /* the surface it stands on */
#define REFL_H          18      /* its head-on mirror image */
#define CAPTION_BASE    214     /* last caption line, y ~210 */
#define CAPTION_LINES   3
#define CAPTION_PITCH   14      /* 7G: 3-line Clicker caption 176-189 /
                                 * 192-201 / 205-217 (audit, ~) */

/* -- composition, in place ------------------------------------------------ */

/* 8b-3: art is composed straight into the panel's own pixels (the LCD
 * framebuffer at UI_PANEL_X, or the strip a highlight fade draws into),
 * over the background panel_begin() painted there: no scratch, no extra
 * blit, and any size (the wide EQ tile with its reflection is 128 x 95,
 * more than the 16 KB scratch holds). Read-back from the destination is
 * confined to the corners, the reflections and the capsule ends (brief
 * 4.5: a BCM backend would replace these functions whole). */
struct canvas {
    fb_data *px;                /* panel pixel (0, 0) */
    int stride;
};

/* after panel_begin() (lcd_set_viewport() filled in vp->buffer) */
static void cv_panel(struct canvas *c, const struct ui_panel *p)
{
    const struct viewport *vp = p->vp;

    c->stride = vp->buffer->stride;
    c->px = vp->buffer->fb_ptr + vp->y * c->stride + vp->x;
}

#define CV_AT(c, x, y)  ((c)->px + (y) * (c)->stride + (x))

/* a = 0..32, the weight of fg; RGB565 with the green field moved up so
 * all three channels blend in one multiply. */
static inline unsigned blend32(unsigned bg, unsigned fg, unsigned a)
{
    uint32_t b = (bg | bg << 16) & 0x07e0f81f;
    uint32_t f = (fg | fg << 16) & 0x07e0f81f;
    uint32_t r = ((b * (32 - a) + f * a) >> 5) & 0x07e0f81f;
    return (r | r >> 16) & 0xffff;
}

static inline unsigned cov32(unsigned cov)        /* 0..15 -> 0..32 */
{
    return cov >= 15 ? 32 : cov * 2;
}

#define C_WHITE     LCD_RGBPACK(0xff, 0xff, 0xff)
#define C_BLACK     LCD_RGBPACK(0, 0, 0)

/* Coverage (0..32) of pixel (ci, cj) of a corner, (0, 0) outermost, by a
 * quarter disc of radius r: squared distance in half pixels against the
 * radius, a soft band one pixel wide (the capsule's rule). */
static unsigned corner_cov(int ci, int cj, int r)
{
    int dx = 2 * (r - ci) - 1, dy = 2 * (r - cj) - 1;
    int d2 = dx * dx + dy * dy, r2 = 4 * r * r;

    if (d2 <= r2 - 4 * r)
        return 32;
    if (d2 >= r2 + 4 * r)
        return 0;
    return (unsigned)(r2 + 4 * r - d2) * 32 / (8 * r);
}

/* A shading mask (panelart.h, caldigits.h: run-length 4-bit values) of
 * w x h at dst, through a 16-entry palette rebuilt by pal() at the start
 * of every row: value 0 leaves the pixel, any other is a lookup and a
 * store (no blend per pixel; brief 3.1 "Tile colouring"). */
typedef void (*pal_fn)(fb_data *pal, int row, const void *ctx);

static void mask_paint(fb_data *dst, int stride, int w, int h,
                       const unsigned char *rle, pal_fn pal, const void *ctx)
{
    fb_data tab[16];
    int x = 0, r = 0;

    pal(tab, 0, ctx);
    while (r < h)
    {
        unsigned b = *rle++, v = b & 15;
        int run = (b >> 4) + 1;

        while (run-- > 0)
        {
            if (v)
                dst[x] = tab[v];
            if (++x == w)
            {
                x = 0;
                dst += stride;
                if (++r == h)
                    return;
                pal(tab, r, ctx);
            }
        }
    }
}

/* Blends pre-shaded art at dst: run-length alpha plus one RGB565 colour
 * per non-transparent pixel (~14 instructions per pixel). */
static void cv_rgba(fb_data *dst, int stride, int w, int h,
                    const unsigned char *rle, const unsigned short *rgb)
{
    int px = 0, py = 0;

    while (py < h)
    {
        unsigned b = *rle++, a = b & 15;
        int run = (b >> 4) + 1;

        while (run-- > 0)
        {
            if (a)
                dst[px] = blend32(dst[px], *rgb++, cov32(a));
            if (++px == w)
            {
                px = 0;
                py++;
                dst += stride;
            }
        }
    }
}

/* base toward hi in 15 steps (16 blends per row, never per pixel) */
static void ramp(fb_data *tab, unsigned base, unsigned hi)
{
    int v;

    tab[0] = base;
    for (v = 1; v < 16; v++)
        tab[v] = blend32(base, hi, cov32(v));
}

/* The glyph metal at row j of an object h tall: white at the top cooling
 * to pale steel at the middle, a step greyer under the sheen line, then
 * lifting a little toward the foot (owner photos settings-02, -10: the
 * glyphs' lower halves read greyer, the step level across the tile). */
static unsigned metal_hi(int j, int h)
{
    int m = h / 2;

    if (j < m)
        return blend32(C_WHITE, LCD_RGBPACK(0xdc, 0xe0, 0xe8), j * 32 / m);
    return blend32(LCD_RGBPACK(0xc2, 0xc7, 0xd2), LCD_RGBPACK(0xd2, 0xd6, 0xde),
                   (j - m) * 32 / MAX(h - m, 1));
}

/* the palette contexts: a glyph on a tile (base = the tile's metal rows)
 * or on the panel itself (base = the panel gradient) */
struct glyph_pal {
    const fb_data *base;        /* per mask row, or NULL: the panel's */
    int y0;                     /* mask row 0: panel y (base NULL) */
    int j0, h;                  /* ... and row in the lit object */
};

static void glyph_pal(fb_data *tab, int r, const void *ctx)
{
    const struct glyph_pal *g = ctx;
    unsigned base = g->base ? g->base[r] : ui_paint_panel_row(g->y0 + r);

    ramp(tab, base, metal_hi(g->j0 + r, g->h));
}

/* The head-on reflection (spec settings.md "Panel tiles"): rows
 * [sy, sy + n) of the object, mirrored into rows dy.. under it, rh rows,
 * fading by ui_mat_refl_a() (the Now Playing art's curve, no slant). */
static void reflect(struct canvas *c, int x, int w, int sy, int n, int dy,
                    int rh)
{
    int k, i;

    for (k = 0; k < rh && k < n; k++)
    {
        const fb_data *s = CV_AT(c, x, sy + n - 1 - k);
        fb_data *d = CV_AT(c, x, dy + k);
        unsigned a = (ui_mat_refl_a(k, n, rh) + 4) >> 3;

        if (!a)
            continue;
        for (i = 0; i < w; i++)
            d[i] = blend32(d[i], s[i], a);
    }
}

/* The metal button (w x TILE at x, y): UI_MAT_METAL's rows (a light rim,
 * a smooth fall from slate to near black), the side edges catching the
 * light at the top and fading out down the sides, a darker foot, rounded
 * corners blended with what is under them. 'rows' gets the metal colour
 * of every row (the glyph palette's base). */
static void tile_paint(struct canvas *c, int x, int y, int w, fb_data *rows)
{
    const fb_data *m = ui_mat_rows(UI_MAT_METAL, TILE);
    const int r = PANELART_TILE_R;
    int i, j;

    for (j = 0; j < TILE; j++)
    {
        fb_data *p = CV_AT(c, x, y + j);
        unsigned col = m ? m[j] : ui_mat_color(UI_MAT_METAL, j, TILE);
        unsigned side = blend32(col, LCD_RGBPACK(0x8a, 0x8e, 0x98),
                                14 * (TILE - j) / TILE);
        int cj = j < r ? j : (j >= TILE - r ? TILE - 1 - j : -1);

        rows[j] = col;
        if (j == TILE - 1)
            col = side = blend32(col, C_BLACK, 12);
        for (i = 0; i < w; i++)
        {
            int ci = i < r ? i : (i >= w - r ? w - 1 - i : -1);
            unsigned px = (i == 0 || i == w - 1) ? side : col;

            if (ci >= 0 && cj >= 0)
                p[i] = blend32(p[i], px, cov32(panelart_corner[cj * r + ci]));
            else
                p[i] = px;
        }
    }
}

/* -- common parts ---------------------------------------------------------- */

static void panel_begin(const struct ui_panel *p)
{
    lcd_set_viewport(p->vp);
    lcd_set_drawmode(DRMODE_SOLID);
    ui_paint_panel_bg(0, UI_PANEL_H);
    lcd_set_foreground(ui_theme->panel_fg);
    lcd_set_drawmode(DRMODE_FG);
}

static void panel_text(int font, int base, const char *s)
{
    lcd_setfont(font);
    ui_paint_text_centre(CX, base, TEXT_W, s);
}

static void panel_end(void)
{
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_set_viewport(NULL);
}

const char *ui_device_name(void)
{
    const char *s = (const char *)global_settings.ipodui_device_name;
    return *s ? s : (const char *)str(LANG_IPODUI_DEVICE_NAME);
}

/* -- Settings tiles --------------------------------------------------------- */

/* The Date & Time page (owner 2026-10-08 and 2026-10-19; photo
 * settings-13-date-time): a large calendar page standing on the surface,
 * no tile: a glossy red month band, a white page fading to light grey
 * with the day in big digits, a thin page edge at the foot, round
 * corners. Drawn live from today's date (the digits are glyphs,
 * caldigits.h; no picture of a day is kept). The photo's page is ~97 x
 * 89 px; ours keeps the owner's 80 x 84 bound (the brief's scratch
 * figure) at the photo's proportions. */
#define CAL_W       80
#define CAL_H       74
#define CAL_X       ((UI_PANEL_W - CAL_W) / 2)
#define CAL_Y       79
#define CAL_BAND    19          /* the month band, its edge line below */
#define CAL_R       6
#define CAL_FOOT    2           /* the page edge */
#define CAL_INK     LCD_RGBPACK(0x12, 0x12, 0x14)
#define CAL_REFL    12          /* mirrors the page below the digits */

static unsigned cal_row(int j)
{
    if (j < CAL_BAND)
        return ui_mat_color(UI_MAT_GEL_RED, j, CAL_BAND);
    if (j == CAL_BAND)
        return LCD_RGBPACK(0x70, 0x12, 0x14);
    if (j == CAL_H - 2)
        return LCD_RGBPACK(0xa6, 0xa8, 0xb0);
    if (j == CAL_H - 1)
        return LCD_RGBPACK(0x66, 0x68, 0x70);
    return blend32(C_WHITE, LCD_RGBPACK(0xd0, 0xd3, 0xda),
                   (j - CAL_BAND - 1) * 32 / (CAL_H - CAL_BAND - 1 - CAL_FOOT));
}

static void digit_pal(fb_data *tab, int r, const void *ctx)
{
    ramp(tab, cal_row(*(const int *)ctx + r), CAL_INK);
}

static void draw_calendar(struct canvas *c, const struct tm *tm)
{
    int d[2], i, j, x, dy;

    for (j = 0; j < CAL_H; j++)
    {
        fb_data *p = CV_AT(c, CAL_X, CAL_Y + j);
        unsigned col = cal_row(j);
        unsigned edge = blend32(col, C_BLACK, 7);
        int cj = j < CAL_R ? j : (j >= CAL_H - CAL_R ? CAL_H - 1 - j : -1);

        for (i = 0; i < CAL_W; i++)
        {
            int ci = i < CAL_R ? i : (i >= CAL_W - CAL_R ? CAL_W - 1 - i : -1);
            unsigned px = (i == 0 || i == CAL_W - 1) ? edge : col;

            if (ci >= 0 && cj >= 0)
                p[i] = blend32(p[i], px, corner_cov(ci, cj, CAL_R));
            else
                p[i] = px;
        }
    }
    /* the day, "06" (stock shows two digits), centred on the page body */
    d[0] = tm->tm_mday / 10 % 10;
    d[1] = tm->tm_mday % 10;
    x = CAL_X + (CAL_W - caldig_w[d[0]] - caldig_w[d[1]]) / 2;
    dy = CAL_BAND + 1 + (CAL_H - CAL_BAND - 1 - CAL_FOOT - CALDIG_H) / 2;
    for (i = 0; i < 2; i++)
    {
        mask_paint(CV_AT(c, x, CAL_Y + dy), c->stride, caldig_w[d[i]],
                   CALDIG_H, caldig_rle + caldig_off[d[i]], digit_pal, &dy);
        x += caldig_w[d[i]];
    }
    reflect(c, CAL_X, CAL_W, CAL_Y, CAL_H, CAL_Y + CAL_H + REFL_GAP, CAL_REFL);
}

void ui_panel_draw_tile(const struct ui_panel *p, const char *title,
                        int art, const char *caption)
{
    const struct ui_theme *t = ui_theme;
    struct canvas c;

    panel_begin(p);
    cv_panel(&c, p);
    panel_text(t->font_ui, TITLE_BASE, title);

    if (art == UI_TILE_WORDS)
    {
        panel_text(t->font_ui, 128, str(LANG_IPODUI_WELCOME));
    }
    else if (art == UI_TILE_LOGO)
    {
        const int w = PANELART_WHEEL_W;
        cv_rgba(CV_AT(&c, (UI_PANEL_W - w) / 2, TILE_Y + (TILE - w) / 2),
                c.stride, w, w, panelart_wheel_a, panelart_wheel_rgb);
    }
    else if (art == UI_TILE_CALENDAR)
    {
        struct tm *tm = panel_time();

        draw_calendar(&c, tm);
        /* the month on the band (Bold 15: cap 11 on a 19-row band) */
        lcd_set_foreground(C_WHITE);
        panel_text(t->font_ui, CAL_Y + 15,
                   str(LANG_MONTH_JANUARY + tm->tm_mon));
        lcd_set_foreground(t->panel_fg);
    }
    else
    {
        fb_data rows[TILE];
        int w = TILE;

        if (art == PANELART_EQ)
            w = PANELART_EQ_TILE_W;
        tile_paint(&c, (UI_PANEL_W - w) / 2, TILE_Y, w, rows);
        if (art >= 0 && art < PANELART_GLYPHS)
        {
            const int gw = panelart_glyph_wh[art][0];
            const int gh = panelart_glyph_wh[art][1];
            const int gy = (TILE - gh) / 2;
            struct glyph_pal g = { rows + gy, 0, gy, TILE };

            mask_paint(CV_AT(&c, (UI_PANEL_W - gw) / 2, TILE_Y + gy),
                       c.stride, gw, gh, panelart_glyphs[art], glyph_pal,
                       &g);
        }
        reflect(&c, (UI_PANEL_W - w) / 2, w, TILE_Y, TILE,
                TILE_Y + TILE + REFL_GAP, REFL_H);
    }

    if (caption && *caption)
    {
        lcd_setfont(t->font_title);
        ui_paint_text_wrap(CX, CAPTION_BASE, TEXT_W, CAPTION_LINES,
                           CAPTION_PITCH, caption);
    }
    panel_end();
}

/* Instruction counts (ARM, estimated from the loops above; the
 * background is 240 lcd_hline() of 160 px either way, ~25k):
 *  - tile: 76 x 76 metal ~23k (corners ~3k of it), glyph 16 x 76 palette
 *    blends ~15k + ~3k px x ~6 ~18k, reflection 76 x 18 blends ~20k,
 *    text ~10k: ~90k (~1.1 ms at 80 MHz), against ~140k before 8b-3 (no
 *    canvas fill, no blit, no blend per glyph pixel); the wide EQ tile
 *    ~125k;
 *  - calendar: 80 x 74 page ~25k, digits 2 x 28 palettes ~9k + ~1.2k px,
 *    reflection 80 x 18 ~21k, text: ~70k;
 *  - capacity: wheel ~40k, capsule ~20k, its reflection ~10k, text:
 *    ~85k;
 *  - podcasts: 80 x 80 glyph ~30k, reflection ~15k: ~70k;
 *  - extras (#52): background 240 lcd_hline ~25k, the map mask (160 x
 *    180, ~3.7k runs) ~45k, the clock body (96 x 96 'grey' runs) ~40k,
 *    12 numerals ~15k, the hands over a 69 x 69 box with the box reject
 *    (~1,000 px reach hand_cov()'s divisions) ~0.3M cycles: ~0.45M
 *    cycles (~6 ms at 80 MHz), once a
 *    minute while the panel shows. */

/* -- capacity (Settings, About) ------------------------------------------- */

#define FREE_TTL    (30 * HZ)   /* the free count is in RAM (FSInfo): cheap,
                                   but no need to read it per draw */
static unsigned long cap_kib, free_kib;
static long free_tick;
static bool free_valid;

static void free_refresh(void)
{
    sector_t size = 0, freek = 0;

    if (free_valid && TIME_BEFORE(current_tick, free_tick + FREE_TTL))
        return;
#ifdef SIMULATOR
    /* a scripted run shows a fixed volume (the sim's disk is the host's,
     * whose free space changes under it): 930 GB, 28.1 GB free */
    if (sim_script_file)
    {
        size = 930ul * 1024 * 1024;
        freek = 29465000;           /* 281.0 tenths of a GiB */
    }
    else
#endif
    volume_size(IF_MV(0,) &size, &freek);   /* KiB, FAT bpb in RAM */
    cap_kib = size;
    free_kib = freek;
    free_tick = current_tick;
    free_valid = true;
}

/* "128" or "19.9" (GB = 2^30 B, as the iPod shows its capacity) */
const char *ui_fmt_gb(unsigned long kib, char *buf, size_t len)
{
    unsigned long tenths = (unsigned long)((uint64_t)kib * 10 / (1024 * 1024));

    if (tenths >= 1000)
        snprintf(buf, len, "%lu", tenths / 10);
    else
        snprintf(buf, len, "%lu.%lu", tenths / 10, tenths % 10);
    return buf;
}

/* The capacity capsule (owner 2026-10-08: "3D and metallic", a glossy,
 * rounded capsule with metallic shading and a fill; settings-01-about,
 * about-1): every segment is a raised gloss over its colour (a bright
 * top line, a lit upper half, a sheen line at the middle, the lower half
 * deepening, a faint lift and a darker foot), the free part either a
 * dark recessed channel (the Settings panel) or the same raised gloss in
 * light silver (About). Rounded, anti-aliased ends. */
#define BAR_W   126
#define BAR_H   13
#define BAR_Y   170
#define BAR_REFL 6

/* row y of h of a raised (or, 'inset', recessed) glossy body of colour c */
static unsigned gloss_row(unsigned c, int y, int h, bool inset)
{
    int t = y * 32 / h;                         /* 0..31 */

    if (inset)
    {
        /* a channel: shadowed under its top lip, lit along its floor */
        if (y == 0)
            return blend32(c, C_BLACK, 20);
        if (y == 1)
            return blend32(c, C_BLACK, 10);
        if (y == h - 1)
            return blend32(c, C_WHITE, 7);
        return blend32(c, C_WHITE, t / 10);
    }
    if (y == 0)
        return blend32(c, C_WHITE, 20);
    if (y == h - 1)
        return blend32(c, C_BLACK, 10);
    if (t < 16)
        return blend32(c, C_WHITE, 17 - t * 9 / 16);    /* lit: 17 .. 9 */
    if (t < 27)
        return blend32(c, C_BLACK, (t - 16) * 7 / 11);  /* the deep part */
    return blend32(c, C_WHITE, (t - 27) * 4 / 5);       /* the lift */
}

void ui_capsule_paint(fb_data *px, int stride, int w, int h, const int *end,
                      const unsigned short *col, int nseg,
                      unsigned free_col, bool free_inset)
{
    unsigned row[UI_CAPSULE_SEGS + 1];
    int x, y, k;
    const int r2 = h * h;

    if (nseg > UI_CAPSULE_SEGS)
        nseg = UI_CAPSULE_SEGS;
    for (y = 0; y < h; y++, px += stride)
    {
        int dy2 = (2 * y + 1 - h) * (2 * y + 1 - h);

        for (k = 0; k < nseg; k++)
            row[k] = gloss_row(col[k], y, h, false);
        row[nseg] = gloss_row(free_col, y, h, free_inset);
        k = 0;
        for (x = 0; x < w; x++)
        {
            /* capsule coverage: distance to the centre line, in half
             * pixels squared, against the radius (h / 2) */
            int ex = x < h / 2 ? h / 2 - x
                     : (x >= w - h / 2 ? x - (w - 1 - h / 2) : 0);
            int d2 = 4 * ex * ex + dy2, a;

            while (k < nseg && x >= end[k])
                k++;
            if (d2 >= r2)
                continue;
            a = d2 > r2 - 4 * h ? (r2 - d2) * 32 / (4 * h) : 32;
            px[x] = a >= 32 ? row[k] : blend32(px[x], row[k], a);
        }
    }
}

void ui_gel_square(fb_data *px, int stride, int s, unsigned col)
{
    int x, y;

    for (y = 0; y < s; y++, px += stride)
    {
        unsigned c = gloss_row(col, y, s, false);

        for (x = 0; x < s; x++)
            /* the corner pixels half covered: a softly rounded square */
            px[x] = ((x == 0 || x == s - 1) && (y == 0 || y == s - 1))
                    ? blend32(px[x], c, 14) : c;
    }
}

/* The Settings panel's fill (silver) and ui_capsule_draw()'s segments
 * 1-3 (About paints its own colours through ui_capsule_paint()). */
static const unsigned short capsule_col[UI_CAPSULE_SEGS] = {
    LCD_RGBPACK(0xb0, 0xbe, 0xdc), LCD_RGBPACK(0xd8, 0x8c, 0x3c),
    LCD_RGBPACK(0xd4, 0xc0, 0x48), LCD_RGBPACK(0xac, 0xac, 0xb2) };
#define CAPSULE_TRACK   LCD_RGBPACK(0x26, 0x29, 0x31)

void ui_capsule_draw(int x, int y, int w, int h, const uint16_t *seg_frac,
                     int nseg)
{
    int end[UI_CAPSULE_SEGS];
    int k, acc = 0;

    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > LCD_WIDTH
        || y + h > LCD_HEIGHT)
        return;
    if (nseg > UI_CAPSULE_SEGS)
        nseg = UI_CAPSULE_SEGS;
    for (k = 0; k < nseg; k++)
    {
        acc += seg_frac[k];
        end[k] = (int)((long)MIN(acc, UI_CAPSULE_ONE) * w / UI_CAPSULE_ONE);
    }
    lcd_set_viewport(NULL);         /* FBADDR uses the current viewport */
    /* not ownable: a panel provider, drawn by ui_panel_compose() after the settle */
    ui_capsule_paint(FBADDR(x, y), LCD_WIDTH, w, h, end, capsule_col, nseg,
                     CAPSULE_TRACK, true);
}

static int capacity_draw(const struct ui_panel *p, const void *arg,
                         long phase)
{
    const struct ui_theme *t = ui_theme;
    const int w = PANELART_WHEEL_W, bx = (UI_PANEL_W - BAR_W) / 2;
    struct canvas c;
    char gb[16], line[48];
    int used_w;

    (void)arg; (void)phase;
    free_refresh();
    panel_begin(p);
    cv_panel(&c, p);
    panel_text(t->font_ui, TITLE_BASE, ui_device_name());
    cv_rgba(CV_AT(&c, (UI_PANEL_W - w) / 2, 80), c.stride, w, w,
            panelart_wheel_a, panelart_wheel_rgb);
    used_w = cap_kib ? (int)((uint64_t)(cap_kib - free_kib) * BAR_W / cap_kib)
                     : 0;
    if (cap_kib && used_w < BAR_H / 2)
        used_w = BAR_H / 2;             /* a sliver always shows */
    ui_capsule_paint(CV_AT(&c, bx, BAR_Y), c.stride, BAR_W, BAR_H, &used_w,
                     capsule_col, 1, CAPSULE_TRACK, true);
    reflect(&c, bx, BAR_W, BAR_Y, BAR_H, BAR_Y + BAR_H + REFL_GAP, BAR_REFL);
    ui_fmt_gb(free_kib, gb, sizeof(gb));
    snprintf(line, sizeof(line), str(LANG_IPODUI_GB_FREE), gb);
    lcd_set_foreground(t->panel_fg);
    panel_text(t->font_ui, 211, line);
    panel_end();
    return TIMEOUT_BLOCK;
}

const struct ui_preview_ops ui_preview_capacity = {
    "capacity", capacity_draw, NAV_REST_ICON, UI_PANEL_XFADE };

/* -- podcasts ---------------------------------------------------------------- */

/* A panel glyph standing on the surface (8b-3: the main-menu glyphs get
 * the tiles' metal and the head-on reflection): the shading mask at
 * panel x, y through the panel gradient's palettes, then its mirror. */
static void panel_glyph(struct canvas *c, int x, int y, int w, int h,
                        const unsigned char *rle, int refl)
{
    struct glyph_pal g = { NULL, y, 0, h };

    mask_paint(CV_AT(c, x, y), c->stride, w, h, rle, glyph_pal, &g);
    reflect(c, x, w, y, h, y + h + REFL_GAP, refl);
}

#define POD_Y       48
#define POD_REFL    12

static int podcasts_draw(const struct ui_panel *p, const void *arg,
                         long phase)
{
    const struct ui_theme *t = ui_theme;
    const int w = PANELART_PODCAST_W;
    struct canvas c;
    char line[32];
    int n = lib_podcast_count();   /* precomputed: no lib_open() here */

    (void)arg; (void)phase;
    panel_begin(p);
    cv_panel(&c, p);
    panel_glyph(&c, (UI_PANEL_W - w) / 2, POD_Y, w, w, panelart_podcast,
                POD_REFL);
    if (n == 1)
        snprintf(line, sizeof(line), "%s", str(LANG_IPODUI_ONE_PODCAST));
    else if (n > 1)
        snprintf(line, sizeof(line), str(LANG_IPODUI_N_PODCASTS), n);
    else if (n == 0)
        snprintf(line, sizeof(line), "%s", str(LANG_IPODUI_NO_PODCASTS));
    else
        line[0] = '\0';                /* not known yet: no count */
    lcd_set_foreground(t->panel_fg);
    panel_text(t->font_ui, 158, line);
    panel_end();
    return TIMEOUT_BLOCK;
}

const struct ui_preview_ops ui_preview_podcasts = {
    "podcasts", podcasts_draw, NAV_REST_ICON, UI_PANEL_XFADE };

#ifdef HAVE_BCMFW_VIDEO
/* -- videos (Phase 14a) ---------------------------------------------------- */

/* The Videos panel with no poster to drift through (preview.c's
 * ui_preview_videos draws this until one can be read): the film strip
 * and "N Videos" / "1 Video" / "No Videos" (owner photo
 * mov-main-panel-videos.jpg; laid out like the Podcasts panel). The count
 * is About's Videos row, movies + TV episodes (owner T5), from the
 * precompute's totals in RAM: no I/O. Before any precompute no count. */
/* ~ the photo, scaled to our Podcasts panel: the strip from a little
 * below the podcast glyph's top, its text a few px above the Podcasts
 * line; its reflection stops above the text */
#define FILM_Y          54
#define FILM_REFL       10
#define VIDEOS_BASE     155

void ui_panel_draw_videos(const struct ui_panel *p)
{
    const struct ui_theme *t = ui_theme;
    const int w = PANELART_FILM_W, h = PANELART_FILM_H;
    struct libfile_vstats vs;
    struct lib_stats st;
    struct canvas c;
    char line[32];
    int n = -1;

    if (lib_vstats(&vs))
        n = vs.videos;
    else if (lib_stats(&st))
        n = 0;                      /* a precompute without video totals */
    panel_begin(p);
    cv_panel(&c, p);
    panel_glyph(&c, (UI_PANEL_W - w) / 2, FILM_Y, w, h, panelart_film,
                FILM_REFL);
    if (n == 1)
        snprintf(line, sizeof(line), "%s", str(LANG_IPODUI_ONE_VIDEO));
    else if (n > 1)
        snprintf(line, sizeof(line), str(LANG_IPODUI_N_VIDEOS), n);
    else if (n == 0)
        snprintf(line, sizeof(line), "%s", str(LANG_IPODUI_NO_VIDEOS));
    else
        line[0] = '\0';
    lcd_set_foreground(t->panel_fg);
    panel_text(t->font_ui, VIDEOS_BASE, line);
    panel_end();
}
#endif

/* -- Extras: world map, clock and date ------------------------------------ */

/* Owner photo main-menu-custom-hold (photo 1189) and extras-clocks-sel: the
 * eastern half of a world map in light grey on dark grey, a white 3-D
 * analog clock (~91 px, centre ~(80, 117)) with the numerals 1-12, and
 * the date under it ("Oct 6 2026"). The map is the right half of the
 * Clocks page's (P12, VIDEO-2 A3). All drawn at draw time from the time
 * of day: the art (panelart.h) is the map mask and the clock body; the
 * numerals come from the small font and the hands are computed. */

/* the panel's own background: a neutral dark grey, lighter at the top
 * (8-bit steps, so the long ramp shows no bands) */
static unsigned extras_bg(int y)
{
    static const unsigned char stop[3][3] = {
        { 0x60, 0x64, 0x6b }, { 0x3a, 0x3d, 0x43 }, { 0x18, 0x19, 0x1c } };
    const int m = UI_PANEL_H / 2;
    const unsigned char *a = stop[y >= m], *b = stop[(y >= m) + 1];
    int t = y < m ? y * 256 / m : (y - m) * 256 / (UI_PANEL_H - m);

    return LCD_RGBPACK(a[0] + ((b[0] - a[0]) * t >> 8),
                       a[1] + ((b[1] - a[1]) * t >> 8),
                       a[2] + ((b[2] - a[2]) * t >> 8));
}

/* the map's palette at mask row r: the land a light grey (cooler and
 * dimmer toward the south) over the background row */
static void map_pal(fb_data *tab, int r, const void *ctx)
{
    const int y = PANELART_MAP_Y0 + r;
    unsigned bg = extras_bg(y);
    unsigned land = blend32(LCD_RGBPACK(0xb8, 0xbd, 0xc4),
                            LCD_RGBPACK(0x86, 0x8b, 0x94),
                            r * 32 / PANELART_MAP_H);
    int v;

    (void)ctx;
    tab[0] = bg;
    for (v = 1; v < 16; v++)
        tab[v] = blend32(bg, land, v * 19 / 15);
}

/* Greyscale art at dst (panelart.h 'grey'): run-length alpha, and for
 * every pixel it does not leave clear an index into pal, from a second
 * run-length stream. */
static void cv_grey(fb_data *dst, int stride, int w, int h,
                    const unsigned char *arle, const unsigned char *grle,
                    const unsigned short *pal)
{
    int x = 0, y = 0, grun = 0;
    unsigned g = 0;

    while (y < h)
    {
        unsigned b = *arle++, a = b & 15;
        int run = (b >> 4) + 1;

        while (run-- > 0)
        {
            if (a)
            {
                if (grun == 0)
                {
                    unsigned c = *grle++;
                    g = c & 15;
                    grun = (c >> 4) + 1;
                }
                grun--;
                dst[x] = a == 15 ? pal[g] : blend32(dst[x], pal[g], cov32(a));
            }
            if (++x == w)
            {
                x = 0;
                y++;
                dst += stride;
            }
        }
    }
}

/* Coverage (0..32) of a pixel centre (x, y), in 1/16 px units from the
 * clock centre, by a hand from the centre to (hx, hy) of half-width hw. */
static unsigned hand_cov(int x, int y, int hx, int hy, int hw)
{
    const int m = hw + 16, bx = -hx / 6, by = -hy / 6;
    long t, l2, dx, dy, d2;

    /* cheap box reject first: the hand (from a sixth behind the centre
     * to its tip) plus its soft edge covers well under a third of the
     * face, and the divisions below are the cost */
    if (x < MIN(bx, hx) - m || x > MAX(bx, hx) + m
        || y < MIN(by, hy) - m || y > MAX(by, hy) + m)
        return 0;
    t = (long)x * hx + (long)y * hy;
    l2 = (long)hx * hx + (long)hy * hy;
    if (t < -l2 / 6 || t > l2)
        return 0;
    dx = x - hx * t / l2;
    dy = y - hy * t / l2;
    d2 = dx * dx + dy * dy;
    if (d2 >= (long)(hw + 16) * (hw + 16))
        return 0;
    if (d2 <= (long)hw * hw)
        return 32;
    /* a soft 1 px edge, linear in d^2 (close enough at this size) */
    return (unsigned)(((long)(hw + 16) * (hw + 16) - d2) * 32 /
                      ((long)(hw + 16) * (hw + 16) - (long)hw * hw));
}

#define CLOCK_CY        117     /* the face's centre, panel y */
#define CLOCK_NUM_R     32      /* numeral centres from the clock centre */
#define CLOCK_HOUR_L    21      /* hand lengths, px */
#define CLOCK_MIN_L     32
#define CLOCK_HAND      LCD_RGBPACK(0x1c, 0x1c, 0x20)
#define CLOCK_NUM       LCD_RGBPACK(0x58, 0x5a, 0x60)
#define DATE_BASE       219

void ui_clock_hands(fb_data *ctr, int stride, int hour, int min,
                    int hour_l, int min_l, unsigned hand, unsigned pin,
                    int xmax)
{
    /* angles from 12 o'clock; tips in 16ths of a px from the centre */
    int ha = ((hour % 12) * 60 + min) / 2, ma = min * 6;
    int hx = fp14_sin(ha) * (hour_l * 16) >> 14,
        hy = -(fp14_cos(ha) * (hour_l * 16) >> 14);
    int mx = fp14_sin(ma) * (min_l * 16) >> 14,
        my = -(fp14_cos(ma) * (min_l * 16) >> 14);
    const int r = min_l + 2;
    int x, y;

    for (y = -r; y <= r; y++)
    {
        fb_data *row = ctr + y * stride;
        int py = y * 16;

        for (x = -r; x <= MIN(r, xmax); x++)
        {
            int px = x * 16, d2 = x * x + y * y;
            unsigned a = MAX(hand_cov(px, py, hx, hy, 20),
                             hand_cov(px, py, mx, my, 12));

            /* the hub: a light pin in a dark ring */
            if (d2 <= 2)
                row[x] = pin;
            else if (d2 <= 8)
                row[x] = hand;
            else if (a)
                row[x] = blend32(row[x], hand, a);
        }
    }
}

static int extras_draw(const struct ui_panel *p, const void *arg, long phase)
{
    const struct ui_theme *t = ui_theme;
    const int w = PANELART_CLOCK_W, cx = UI_PANEL_W / 2;
    struct tm *tm = panel_time();
    struct canvas c;
    char line[32];
    int y, k;

    (void)arg; (void)phase;
    /* panel_begin() without the slate gradient: this panel has its own */
    lcd_set_viewport(p->vp);
    lcd_set_drawmode(DRMODE_SOLID);
    for (y = 0; y < UI_PANEL_H; y++)
    {
        lcd_set_foreground(extras_bg(y));
        lcd_hline(0, UI_PANEL_W - 1, y);
    }
    lcd_set_drawmode(DRMODE_FG);
    cv_panel(&c, p);
    mask_paint(CV_AT(&c, 0, PANELART_MAP_Y0), c.stride, UI_PANEL_W,
               PANELART_MAP_H, panelart_map, map_pal, NULL);
    cv_grey(CV_AT(&c, cx - w / 2, CLOCK_CY - w / 2), c.stride, w, w,
            panelart_clock_a, panelart_clock_g, panelart_clock_pal);
    /* the numerals, centred on their circle (cap 9: baseline + 4) */
    lcd_setfont(t->font_small);
    lcd_set_foreground(CLOCK_NUM);
    for (k = 1; k <= 12; k++)
    {
        char n[3];
        int a = k * 30;

        snprintf(n, sizeof(n), "%d", k);
        ui_paint_text_centre(cx + (fp14_sin(a) * CLOCK_NUM_R >> 14),
                             CLOCK_CY - (fp14_cos(a) * CLOCK_NUM_R >> 14) + 5,
                             20, n);
    }
    ui_clock_hands(CV_AT(&c, cx, CLOCK_CY), c.stride, tm->tm_hour,
                   tm->tm_min, CLOCK_HOUR_L, CLOCK_MIN_L, CLOCK_HAND,
                   LCD_RGBPACK(0xe8, 0xe8, 0xec), UI_PANEL_W);
    snprintf(line, sizeof(line), str(LANG_IPODUI_PANEL_DATE),
             str(LANG_MONTH_JANUARY + tm->tm_mon), tm->tm_mday,
             tm->tm_year + 1900);
    lcd_set_foreground(t->panel_fg);
    panel_text(t->font_ui, DATE_BASE, line);
    panel_end();
    /* animated: again at the next minute */
    return (60 - tm->tm_sec) * HZ;
}

const struct ui_preview_ops ui_preview_extras = {
    "extras", extras_draw, NAV_REST_ICON, UI_PANEL_XFADE };

/* -- P12 Clocks (W1-C): the shared backdrop, hands and time ---------------- */

struct tm *ui_panel_time(void)
{
    return panel_time();
}

/* columns [x0, x1) of the 160 px column (0 <= x0, x1 <= UI_PANEL_W), px
 * the column's left edge at row y0. The mask is walked a run at a time
 * (the rows before y0, and the columns out of range, are only counted),
 * and a row's palette made only for the rows drawn. */
static void backdrop_cols(fb_data *px, int stride, int y0, int h,
                          const unsigned char *rle, int x0, int x1)
{
    fb_data tab[16];
    int x = 0, r = 0, y, end = y0 + h;

    if (x1 <= x0 || h <= 0)
        return;
    /* the ramp first, then the land over it, row by row */
    for (y = y0; y < end; y++)
    {
        fb_data *row = px + (y - y0) * stride, c = extras_bg(y);
        int i;

        for (i = x0; i < x1; i++)
            row[i] = c;
    }
    if (end <= PANELART_MAP_Y0 || y0 >= PANELART_MAP_Y0 + PANELART_MAP_H)
        return;
    if (PANELART_MAP_Y0 >= y0)
        map_pal(tab, 0, NULL);
    while (r < PANELART_MAP_H)
    {
        unsigned b = *rle++, v = b & 15;
        int run = (b >> 4) + 1;

        while (run > 0)
        {
            int n = MIN(run, UI_PANEL_W - x), sy = PANELART_MAP_Y0 + r;

            if (v && sy >= y0)
            {
                fb_data *row = px + (sy - y0) * stride, c = tab[v];
                int i = MAX(x, x0), e = MIN(x + n, x1);

                for (; i < e; i++)
                    row[i] = c;
            }
            x += n;
            run -= n;
            if (x == UI_PANEL_W)
            {
                x = 0;
                if (++r == PANELART_MAP_H || PANELART_MAP_Y0 + r >= end)
                    return;
                if (PANELART_MAP_Y0 + r >= y0)
                    map_pal(tab, r, NULL);
            }
        }
    }
}
void ui_extras_backdrop(fb_data *px, int stride, int y0, int h,
                        const unsigned char *mask)
{
    backdrop_cols(px, stride, y0, h, mask ? mask : panelart_map, 0,
                  UI_PANEL_W);
}

void ui_extras_page_backdrop_cols(int y0, int h, int x0, int x1)
{
    /* not ownable: a panel provider, drawn by ui_panel_compose() after the settle */
    int stride = FBADDR(0, 1) - FBADDR(0, 0);

    lcd_set_viewport(NULL);
    x0 = MAX(x0, 0);
    x1 = MIN(x1, LCD_WIDTH);
    /* not ownable: a panel provider, drawn by ui_panel_compose() after the settle */
    backdrop_cols(FBADDR(0, y0), stride, y0, h, clockart_map_west,
                  x0, MIN(x1, UI_PANEL_W));
    /* not ownable: a panel provider, drawn by ui_panel_compose() after the settle */
    backdrop_cols(FBADDR(UI_PANEL_W, y0), stride, y0, h, panelart_map,
                  MAX(x0 - UI_PANEL_W, 0), x1 - UI_PANEL_W);
}

void ui_extras_page_backdrop(int y0, int h, int w)
{
    ui_extras_page_backdrop_cols(y0, h, 0, w);
}

/* -- P12 Extras rows (W1-F) -------------------------------------------------- */

/* Extras > Calendars (owner photos 1191, photo 1196): the time at the
 * top, today's calendar page, the weekday under it: the Date & Time page
 * drawn with a clock title and a weekday caption. Again at the next
 * minute (the time), which also covers midnight (the page). */
static int x_calendars_draw(const struct ui_panel *p, const void *arg,
                            long phase)
{
    struct tm *tm = panel_time();
    char now[16];
    int h = tm->tm_hour, m = tm->tm_min;

    (void)arg; (void)phase;
    if (global_settings.timeformat)             /* 12 hour clock */
        snprintf(now, sizeof(now), "%d:%02d %s", h % 12 ? h % 12 : 12, m,
                 str(h < 12 ? LANG_IPODUI_AM : LANG_IPODUI_PM));
    else
        snprintf(now, sizeof(now), "%02d:%02d", h, m);
    ui_panel_draw_tile(p, now, UI_TILE_CALENDAR,
                       str(LANG_IPODUI_SUNDAY + tm->tm_wday % 7));
    return (60 - MIN(tm->tm_sec, 59)) * HZ;
}

const struct ui_preview_ops ui_preview_x_calendars = {
    "x_calendars", x_calendars_draw, NAV_REST_ICON, UI_PANEL_XFADE };

/* An Extras row whose art has not landed yet: the panel's own surface,
 * nothing on it (no placeholder glyph that the real art would replace) */
static int x_plain_draw(const struct ui_panel *p, const void *arg, long phase)
{
    (void)arg; (void)phase;
    panel_begin(p);
    panel_end();
    return TIMEOUT_BLOCK;
}

const struct ui_preview_ops ui_preview_x_plain = {
    "x_plain", x_plain_draw, NAV_REST_ICON, UI_PANEL_XFADE };

/* -- the Extras rows' icons (owner, 2026-10-24) ----------------------------- */

/* Owner photos 1197, photo 1198: every Extras row has its own icon in
 * the panel, on the panel's slate, no title, no caption; centred where
 * the Extras clock sits (80, 117). Original art, extrasart.py. Owner,
 * second pass: one scale for all (about 84 px tall, every foot on one
 * baseline) and the head-on reflection under each, as the panels' tiles
 * and the art have. */

/* Palette art at dst (extrasart.h 'pal'): run-length alpha, and for every
 * pixel it does not leave clear an index into pal from a second
 * run-length stream of 5-bit indices (runs of 1..8). A store where the
 * art is opaque, a blend only along its edges. */
static void cv_pal(fb_data *dst, int stride, int w, int h,
                   const unsigned char *arle, const unsigned char *irle,
                   const unsigned short *pal)
{
    int x = 0, y = 0, irun = 0;
    unsigned ix = 0;

    while (y < h)
    {
        unsigned b = *arle++, a = b & 15;
        int run = (b >> 4) + 1;

        while (run-- > 0)
        {
            if (a)
            {
                if (irun == 0)
                {
                    unsigned c = *irle++;
                    ix = c & 31;
                    irun = (c >> 5) + 1;
                }
                irun--;
                dst[x] = a == 15 ? pal[ix]
                                 : blend32(dst[x], pal[ix], cov32(a));
            }
            if (++x == w)
            {
                x = 0;
                y++;
                dst += stride;
            }
        }
    }
}

/* The reflection under palette art (owner, 2026-10-24): the art's own
 * pixels as now composed (the alarm's live numerals too), mirrored about
 * its foot one row down (REFL_GAP), weighted by the art's alpha so only
 * the object shows, fading from XREFL_A0 to nothing over a third of its
 * height. Walks the alpha stream again, nothing stored. src: the art's
 * top left on the canvas. */
#define XREFL_A0        96      /* /256: 0.375 at the foot */

/* row y's fade, /256 (rh rows mirrored, y from the art's top) */
static unsigned refl_fade(int y, int h, int rh)
{
    int k = h - 1 - y;                  /* rows up from the foot */

    return k < rh ? XREFL_A0 * (unsigned)((rh - k) * (rh - k)) /
                    (unsigned)(rh * rh) : 0;
}

static void cv_pal_refl(fb_data *src, int stride, int w, int h,
                        const unsigned char *arle)
{
    const int rh = h / 3, r0 = h - rh;     /* mirror the rows [r0, h) */
    fb_data *dst = src + (2 * h - 1 + REFL_GAP) * stride;
    int x = 0, y = 0;
    unsigned fa = 0;

    while (y < h)
    {
        unsigned b = *arle++, a = b & 15;
        int run = (b >> 4) + 1;

        if (y < r0)                     /* the rows above: count them */
        {
            int n = MIN(run, (r0 - y) * w - x);

            x += n;
            y += x / w;
            x %= w;
            run -= n;
            if (y >= r0)
                fa = refl_fade(y, h, rh);
        }
        while (run-- > 0)
        {
            if (a)
            {
                unsigned m = (fa * cov32(a) + 128) >> 8;

                if (m)
                {
                    fb_data *d = dst - y * stride + x;

                    *d = blend32(*d, src[y * stride + x], m);
                }
            }
            if (++x == w)
            {
                x = 0;
                if (++y == h)
                    break;
                fa = refl_fade(y, h, rh);
            }
        }
    }
}

#define XICON_CY        CLOCK_CY        /* the icon square's centre, panel y */
#define XALARM_NUM      LCD_RGBPACK(0x2a, 0x2c, 0x32)   /* on the white face */

/* the art's top left on the canvas */
static fb_data *x_icon_at(struct canvas *c, int art)
{
    return CV_AT(c, (UI_PANEL_W - EXTRASART_W) / 2 + extrasart[art].x,
                 XICON_CY - EXTRASART_W / 2 + extrasart[art].y);
}

/* the panel's slate, then the icon; c gets the panel's canvas */
static void x_icon_c(const struct ui_panel *p, int art, struct canvas *c)
{
    panel_begin(p);
    cv_panel(c, p);
    cv_pal(x_icon_at(c, art), c->stride, extrasart[art].w, extrasart[art].h,
           extrasart[art].a, extrasart[art].i, extrasart[art].pal);
}

static void x_icon_refl(struct canvas *c, int art)
{
    cv_pal_refl(x_icon_at(c, art), c->stride, extrasart[art].w,
                extrasart[art].h, extrasart[art].a);
}

/* the icon and its reflection */
static void x_icon(const struct ui_panel *p, int art)
{
    struct canvas c;

    x_icon_c(p, art, &c);
    x_icon_refl(&c, art);
}

/* Alarms (photo 1197, photo 1198; also its own split list and the Main Menu's
 * Alarms item): the alarm clock; 12, 3, 6 and 9 in the small font, then
 * the reflection, so it mirrors them too */
static int x_alarms_draw(const struct ui_panel *p, const void *arg,
                         long phase)
{
    const int cx = (UI_PANEL_W - EXTRASART_W) / 2 + EXTRASART_ALARM_CX,
              cy = XICON_CY - EXTRASART_W / 2 + EXTRASART_ALARM_CY;
    struct canvas c;
    int k;

    (void)arg; (void)phase;
    x_icon_c(p, EXTRASART_ALARM, &c);
    lcd_setfont(ui_theme->font_small);
    lcd_set_foreground(XALARM_NUM);
    for (k = 0; k < 4; k++)
    {
        static const char *const num[4] = { "12", "3", "6", "9" };
        static const signed char dx[4] = { 0, 1, 0, -1 };
        static const signed char dy[4] = { -1, 0, 1, 0 };

        /* cap 9: the baseline 4 below the centre */
        ui_paint_text_centre(cx + dx[k] * EXTRASART_ALARM_NUM_R,
                             cy + dy[k] * EXTRASART_ALARM_NUM_R + 4, 20, num[k]);
    }
    x_icon_refl(&c, EXTRASART_ALARM);
    panel_end();
    return TIMEOUT_BLOCK;
}

#define X_ICON_PANEL(name, art)                                             \
    static int x_##name##_draw(const struct ui_panel *p, const void *arg,   \
                               long phase)                                  \
    {                                                                       \
        (void)arg; (void)phase;                                             \
        x_icon(p, art);                                                     \
        panel_end();                                                        \
        return TIMEOUT_BLOCK;                                               \
    }                                                                       \
    const struct ui_preview_ops ui_preview_x_##name = {                     \
        "x_" #name, x_##name##_draw, NAV_REST_ICON, UI_PANEL_XFADE };

const struct ui_preview_ops ui_preview_x_alarms = {
    "x_alarms", x_alarms_draw, NAV_REST_ICON, UI_PANEL_XFADE };
X_ICON_PANEL(contacts, EXTRASART_CONTACTS)
X_ICON_PANEL(games, EXTRASART_GAMES)
X_ICON_PANEL(notes, EXTRASART_NOTES)
X_ICON_PANEL(screenlock, EXTRASART_LOCK)
/* the Stopwatch row and the Stopwatch list's panel (stopwatch.h): the
 * dial at the set's scale (it was a 136 px dial, owner: "waaay bigger") */
X_ICON_PANEL(stopwatch, EXTRASART_STOPWATCH)

/* -- the Games list's covers (owner, 2026-10-24) ----------------------------- */

/* Box art for each of our games (extrasart.py: 8-bit era cartridge box
 * art, owner's second pass), standing on the panel's surface with its
 * reflection like the Extras icons. Resident, so a row shows its cover
 * before its module loads. arg: a const unsigned char UI_COVER_*
 * (extras.h); NULL or UI_COVER_NONE: the Games icon. */
static int x_gamecover_draw(const struct ui_panel *p, const void *arg,
                            long phase)
{
    int cv = arg ? *(const unsigned char *)arg : UI_COVER_NONE;

    (void)phase;
    x_icon(p, (cv <= UI_COVER_NONE || cv >= UI_COVER__N)
              ? EXTRASART_GAMES : EXTRASART_CV_CHUTE + cv - 1);
    panel_end();
    return TIMEOUT_BLOCK;
}

const struct ui_preview_ops ui_preview_x_gamecover = {
    "x_gamecover", x_gamecover_draw, NAV_REST_ICON, UI_PANEL_XFADE };
