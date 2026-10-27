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

/* Settings > About (docs/phase11-brief.md section 3.6; spec settings.md
 * "About"; owner photos about-1.jpg .. about-3.jpg): a dark full-screen
 * screen in three pages, and a fourth of ours.
 *  - Every page: the dark header with the device name centred; "X GB
 *    Used" at the left, our wheel logo in the centre, "Y GB Free" at the
 *    right; three page dots at the bottom.
 *  - Page 1: the capacity capsule (Audio, Video, Photos, Other, the rest
 *    free) and its legend.
 *  - Page 2: Songs, Videos, Podcasts | Photos, Games, Contacts with their
 *    counts (Videos, Photos, Games and Contacts are 0; no iTunes U:
 *    frozen default D2).
 *  - Page 3: Serial Number, Model, Version. Serial and model come from
 *    the hidden ipodui_serial / ipodui_model settings, a row without a
 *    value is hidden; Version is LANG_IPODUI_VERSION_STRING (D1). Then
 *    Build, the exact build id (rbversion, "<commit>-<yymmdd>"; the date
 *    is dropped if it doesn't fit), so the owner can tell builds apart
 *    (owner, 2026-10-12; not on the 7G). A scripted sim run pins it.
 *  - Page 4 (ours, not on the 7G; release-plan 6.2): Save Diagnostics, a
 *    row in page 3's style and a short wrapped line under it. SELECT
 *    here (and only here) writes the report (diag.c) and the line says
 *    where it went; again replaces it.
 * NEXT / PREV (on the release, SELECT not held) slide the lower part of
 * the screen to the next / previous page, 190 ms like a push (the main
 * loop's band slide, usbscreen.h ui_slide_band()); the wheel does
 * nothing, nor does SELECT but on page 4; MENU returns. The hidden developer chord (SELECT+PREV held,
 * devmode.c) still arms here on every page, and it never turns a page:
 * the chord's PREV release has SELECT down, or devmode swallows it.
 * This symbol and its .name "about" are the identity devmode arms on
 * (Phase 8a Revision 1 M6): they replace the stub's, unchanged.
 *
 * Data, no I/O: the counts from lib_stats() (the library precompute),
 * used and free from the FAT's FSInfo in RAM (volume_size(), as the
 * Settings capacity panel), read once at enter. The state is in the
 * screen arena. */

/* not ownable (file): a full-screen page or takeover; the drift panel was settled on the way in (ui_panel_hide(), or the takeover's own ui_fb_settle()), and a full lcd_update() releases it (bcmjob_owned_hook) */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "lang.h"
#include "mv.h"
#include "button.h"
#include "settings.h"
#include "version.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/header.h"
#include "ipodui/library.h"
#ifdef HAVE_BCMFW_VIDEO
#include "ipodui/libfile.h"             /* struct libfile_vstats */
#endif
#include "ipodui/usbscreen.h"
#include "ipodui/diag.h"
#include "ipodui/uilog.h"
/* the wheel art lives once, in ipodui/panelart.c (Phase 9) */
#define panelart_wheel_a   panelart_wheel_a_tu
#define panelart_wheel_rgb panelart_wheel_rgb_tu
#include "ipodui/panelart.h"
#undef panelart_wheel_a
#undef panelart_wheel_rgb
extern const unsigned char *const panelart_wheel_a;
extern const unsigned short *const panelart_wheel_rgb;
#include "ipodui/screens/panels.h"
#include "ipodui/screens/screens.h"

#ifdef SIMULATOR
extern const char *sim_script_file;     /* sim_script.c */
#endif

#define PAGES       4                   /* the 7G's three and ours */
#define PAGE_DIAG   3

/* geometry (photos, ~ +-2 px) */
#define LOGO_W      PANELART_WHEEL_W    /* 60 */
#define LOGO_X      ((LCD_WIDTH - LOGO_W) / 2)
#define LOGO_Y      46
/* "X GB Used" / "Y GB Free": the caps (title font, cap 9) centred on
 * the logo's ink (rows 47..104, centre 75.5): caps 71..79 */
#define GB_BASE     80
/* The recessed channel behind the top row (owner photos about-1..3: a
 * full-width band ~36 px tall centred on the text, a little darker than
 * the body, a darker line along its top and a faint light line along its
 * bottom; the leaf and the bottom of the logo stand out of it) */
#define BAND_Y      58
#define BAND_H      36
#define GB_LEFT     28
#define GB_RIGHT    292
#define PAGE_Y      122                 /* the part that slides */
#define PAGE_H      (LCD_HEIGHT - PAGE_Y)

#define BAR_X       17
#define BAR_W       286
#define BAR_Y       142
#define BAR_H       18
#define LEG_BASE    193
#define LEG_SQ      9
#define LEG_GAP     6                   /* square to label */

#define ROW_BASE    158                 /* pages 2 and 3, first row */
#define ROW_PITCH   18
#define P2_LABEL_L  20
#define P2_VALUE_L  153                 /* right edge */
#define P2_LABEL_R  175
#define P2_VALUE_R  302
#define P3_LABEL    28
#define P3_VALUE    190
/* page 4: its wrapped line, in the title font, ending on the last row */
#define P4_TEXT_W   (LCD_WIDTH - 2 * P3_LABEL)
#define P4_TEXT_BASE (ROW_BASE + 3 * ROW_PITCH)
#define P4_LINES    3
#define P4_PITCH    15

#define DOT_Y       223                 /* top of the 6 px dots */
#define DOT_D       6
#define DOT_PITCH   12

#define C_DOT_ON    LCD_RGBPACK(0xf4, 0xf6, 0xfa)
#define C_DOT_OFF   LCD_RGBPACK(0x10, 0x12, 0x18)

/* the capsule's colours, top and bottom of the row gradient: Audio,
 * Video, Photos, Other, then the free rest. Measured on about-1.jpg (the
 * legend squares and the bar, averaged) and corrected for the photo's
 * blue cast (its white text reads ~#8780a5 there: R x1.15, G x1.1,
 * B x0.9): Audio (90,109,200) -> light blue, Video (65,98,218) ->
 * deeper blue, Photos (165,105,81) -> orange, Other (80,37,52) -> red,
 * free (129,146,215) -> light blue-grey. This screen paints its own
 * capsule (draw_capsule()): the free part is light here, where
 * ui_capsule_draw()'s rest is the Settings panel's dark track. */
enum { SEG_AUDIO, SEG_VIDEO, SEG_PHOTOS, SEG_OTHER, SEG_FREE, SEGS };
static const unsigned short seg_top[SEGS] = {
    LCD_RGBPACK(0x7c, 0x9c, 0xe8), LCD_RGBPACK(0x50, 0x78, 0xe8),
    LCD_RGBPACK(0xf4, 0xa0, 0x50), LCD_RGBPACK(0xc0, 0x3c, 0x40),
    LCD_RGBPACK(0xc8, 0xd0, 0xea) };
static const unsigned short seg_bot[SEGS] = {
    LCD_RGBPACK(0x3c, 0x60, 0xc4), LCD_RGBPACK(0x20, 0x44, 0xc0),
    LCD_RGBPACK(0xc8, 0x6c, 0x1c), LCD_RGBPACK(0x80, 0x1c, 0x22),
    LCD_RGBPACK(0x84, 0x90, 0xc0) };
static const short seg_lang[SEG_FREE] = {
    LANG_IPODUI_AUDIO, LANG_IPODUI_VIDEO, LANG_IPODUI_PHOTOS,
    LANG_IPODUI_OTHER };
static const short leg_x[SEG_FREE] = { 18, 98, 176, 255 };
/* draw_capsule() hands every used segment to ui_capsule_paint() */
_Static_assert(SEG_FREE == UI_CAPSULE_SEGS, "about: capsule segments");

struct about {
    struct lib_stats st;
    unsigned long cap_kib, free_kib, audio_kib;
    bool stats_ok;
#ifdef HAVE_BCMFW_VIDEO
    unsigned long video_kib;    /* every video file's size (page 1) */
    long videos;                /* movies + TV episodes (page 2) */
#endif
    unsigned char page;
    signed char diag;           /* page 4: 0 not yet, 1 saved, -1 failed,
                                   2 saving (drawn before the write) */
};
_Static_assert(sizeof(struct about) <= UI_ARENA_B_SETTING,
               "about: arena bound");

/* -- drawing helpers ----------------------------------------------------- */

static inline fb_data mix(fb_data bg, unsigned fg, int a)   /* a 0..16 */
{
    int r = RGB_UNPACK_RED(bg), g = RGB_UNPACK_GREEN(bg),
        b = RGB_UNPACK_BLUE(bg);

    r += (((int)RGB_UNPACK_RED(fg) - r) * a) >> 4;
    g += (((int)RGB_UNPACK_GREEN(fg) - g) * a) >> 4;
    b += (((int)RGB_UNPACK_BLUE(fg) - b) * a) >> 4;
    return LCD_RGBPACK(r, g, b);
}

/* text with its baseline at y_base; align -1 left from x, +1 right to x */
static void text_at(int font, int x, int y_base, int align, const char *s)
{
    struct viewport vp;
    int w = 0, h;

    memset(&vp, 0, sizeof(vp));
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.drawmode = DRMODE_FG;
    vp.font = font;
    lcd_set_viewport(&vp);
    lcd_set_foreground(LCD_WHITE);
    if (align > 0)
        font_getstringsize((const unsigned char *)s, &w, &h, font);
    lcd_putsxy(x - w, y_base - font_get(font)->ascent,
               (const unsigned char *)s);
    lcd_set_viewport(NULL);
}

/* per row of the channel: >0 darken toward black, <0 lighten toward
 * white, in 16ths (a 1 px dark rim with a soft shadow under it, the
 * floor, a 1 px light lip) */
static const signed char band_shade[BAND_H] = {
    5, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, -1,
};

/* The body (#52, 8b-3 note: owner photos about-1..3 read darker than
 * the USB screen's body we used): the panels' slate-to-navy gradient,
 * as the Settings > About panel beside it, one colour per row. */
static void about_body(int y, int h)
{
    lcd_set_viewport(NULL);
    lcd_set_drawmode(DRMODE_SOLID);
    for (; h > 0; h--, y++)
    {
        lcd_set_foreground(ui_paint_panel_row(y));
        lcd_hline(0, LCD_WIDTH - 1, y);
    }
}

/* The channel over the body rows just painted: one colour per row (a
 * pair kept, so a dithered body would work too), each row its shaded
 * pair repeated; one table lookup per row, no per-pixel blend. */
static void draw_band(void)
{
    int y, x;

    lcd_set_viewport(NULL);
    for (y = 0; y < BAND_H; y++)
    {
        fb_data *row = FBADDR(0, BAND_Y + y);
        int k = band_shade[y];
        fb_data p0, p1;

        p0 = k > 0 ? mix(row[0], LCD_BLACK, k) : mix(row[0], LCD_WHITE, -k);
        p1 = k > 0 ? mix(row[1], LCD_BLACK, k) : mix(row[1], LCD_WHITE, -k);
        for (x = 0; x + 1 < LCD_WIDTH; x += 2)
        {
            row[x] = p0;
            row[x + 1] = p1;
        }
    }
}

/* the wheel glyph (panelart.h: 4-bit alpha runs + one colour per inked
 * pixel), blended over the body */
static void draw_logo(void)
{
    const unsigned char *rle = panelart_wheel_a;
    const unsigned short *rgb = panelart_wheel_rgb;
    int px = 0, py = 0;
    fb_data *row;

    lcd_set_viewport(NULL);
    row = FBADDR(LOGO_X, LOGO_Y);
    while (py < LOGO_W)
    {
        unsigned b = *rle++, a = b & 15;
        int run = (b >> 4) + 1;

        while (run-- > 0)
        {
            if (a)
                row[px] = a == 15 ? *rgb : mix(row[px], *rgb, a + (a >> 3));
            if (a)
                rgb++;
            if (++px == LOGO_W)
            {
                px = 0;
                py++;
                row += LCD_WIDTH;
            }
        }
    }
}

/* The capsule (photo about-1; owner 2026-10-08, "3D and metallic"): the
 * shared glossy capsule (panels.c ui_capsule_paint()), segments from the
 * left, ends[k] the end column of segment k, the free rest raised light
 * silver here (the Settings panel's is a dark channel). Segment colours
 * are the middles of seg_top/seg_bot; the gloss is the capsule's. */
static void draw_capsule(const int *ends)
{
    unsigned short col[SEG_FREE];
    int k;

    for (k = 0; k < SEG_FREE; k++)
        col[k] = ui_paint_blend(seg_top[k], seg_bot[k], 8);
    lcd_set_viewport(NULL);
    ui_capsule_paint(FBADDR(BAR_X, BAR_Y), LCD_WIDTH, BAR_W, BAR_H, ends,
                     col, SEG_FREE, ui_paint_blend(seg_top[SEG_FREE],
                                                   seg_bot[SEG_FREE], 6),
                     false);
}

static unsigned long frac_cols(unsigned long part, unsigned long whole)
{
    return whole ? (unsigned long)((uint64_t)part * BAR_W / whole) : 0;
}

static void page_capacity(const struct about *a)
{
    unsigned long used = a->cap_kib - MIN(a->free_kib, a->cap_kib);
    unsigned long audio = MIN(a->audio_kib, used);
    int ends[SEG_FREE], k;

    /* Audio, then no Video or Photos (not synced in v1), then Other */
    ends[SEG_AUDIO] = frac_cols(audio, a->cap_kib);
    if (audio && ends[SEG_AUDIO] < 1)
        ends[SEG_AUDIO] = 1;
    ends[SEG_VIDEO] = ends[SEG_PHOTOS] = ends[SEG_AUDIO];
#ifdef HAVE_BCMFW_VIDEO
    if (a->video_kib)
    {
        /* Video follows Audio; clamped to the used space */
        unsigned long video = MIN(a->video_kib, used - audio);

        ends[SEG_VIDEO] = ends[SEG_PHOTOS] =
            frac_cols(audio + video, a->cap_kib);
        if (video && ends[SEG_VIDEO] <= ends[SEG_AUDIO])
            ends[SEG_VIDEO] = ends[SEG_PHOTOS] = ends[SEG_AUDIO] + 1;
    }
#endif
    ends[SEG_OTHER] = frac_cols(used, a->cap_kib);
    if (used > audio && ends[SEG_OTHER] <= ends[SEG_PHOTOS])
        ends[SEG_OTHER] = ends[SEG_PHOTOS] + 1;      /* a tick always shows */
    draw_capsule(ends);

    for (k = 0; k < SEG_FREE; k++)
    {
        lcd_set_viewport(NULL);
        /* a gel square in the capsule's gloss (8b-3) */
        ui_gel_square(FBADDR(leg_x[k], LEG_BASE - LEG_SQ), LCD_WIDTH, LEG_SQ,
                      ui_paint_blend(seg_top[k], seg_bot[k], 8));
        text_at(ui_theme->font_title, leg_x[k] + LEG_SQ + LEG_GAP, LEG_BASE,
                -1, str(seg_lang[k]));
    }
}

static void count_row(int row, bool right, int lang, long n)
{
    char buf[16];
    int base = ROW_BASE + row * ROW_PITCH;

    snprintf(buf, sizeof(buf), "%ld", n);
    text_at(ui_theme->font_ui, right ? P2_LABEL_R : P2_LABEL_L, base, -1,
            str(lang));
    text_at(ui_theme->font_ui, right ? P2_VALUE_R : P2_VALUE_L, base, 1, buf);
}

static void page_counts(const struct about *a)
{
    count_row(0, false, LANG_IPODUI_SONGS, a->stats_ok ? a->st.songs : 0);
#ifdef HAVE_BCMFW_VIDEO
    count_row(1, false, LANG_IPODUI_VIDEOS, a->videos);
#else
    count_row(1, false, LANG_IPODUI_VIDEOS, 0);
#endif
    count_row(2, false, LANG_IPODUI_PODCASTS,
              a->stats_ok ? a->st.episodes : 0);
    count_row(0, true, LANG_IPODUI_PHOTOS, 0);
    count_row(1, true, LANG_IPODUI_GAMES, 0);
    count_row(2, true, LANG_IPODUI_CONTACTS, 0);
}

/* the build id, without the date when it would run off the screen */
static const char *build_id(char *buf, size_t size)
{
    const char *v = rbversion;
    char *dash;
    int w = 0, h;

#ifdef SIMULATOR
    if (sim_script_file)
        v = "0123456789-261012";        /* goldens don't follow commits */
#endif
    strlcpy(buf, v, size);
    font_getstringsize((const unsigned char *)buf, &w, &h, ui_theme->font_ui);
    dash = strchr(buf, '-');
    if (P3_VALUE + w > LCD_WIDTH - P3_LABEL / 2 && dash)
        *dash = '\0';
    return buf;
}

static void page_device(void)
{
    char build[32];
    const struct { short lang; const char *value; } rows[] = {
        { LANG_IPODUI_SERIAL_NUMBER, (const char *)global_settings.ipodui_serial },
        { LANG_IPODUI_MODEL, (const char *)global_settings.ipodui_model },
        { LANG_IPODUI_VERSION, str(LANG_IPODUI_VERSION_STRING) },
        { LANG_IPODUI_BUILD, build_id(build, sizeof(build)) },
    };
    int i, row = 0;

    for (i = 0; i < (int)ARRAYLEN(rows); i++)
    {
        int base = ROW_BASE + row * ROW_PITCH;

        if (!rows[i].value || !rows[i].value[0])
            continue;                   /* no value: no row (D1) */
        text_at(ui_theme->font_ui, P3_LABEL, base, -1, str(rows[i].lang));
        text_at(ui_theme->font_ui, P3_VALUE, base, -1, rows[i].value);
        row++;
    }
}

/* Page 4: Save Diagnostics. The row's value says what happened; the
 * line under it what to do (rule 8: every word through LANG_*) */
static void page_diag(const struct about *a)
{
    struct viewport vp;
    char msg[192];
    int value = a->diag > 1 ? LANG_IPODUI_DIAG_SAVING
              : a->diag > 0 ? LANG_IPODUI_DIAG_SAVED
              : a->diag < 0 ? LANG_IPODUI_DIAG_FAILED : -1;

    text_at(ui_theme->font_ui, P3_LABEL, ROW_BASE, -1,
            str(LANG_IPODUI_SAVE_DIAGNOSTICS));
    if (value >= 0)
        text_at(ui_theme->font_ui, LCD_WIDTH - P3_LABEL, ROW_BASE, 1,
                str(value));
    if (a->diag == 1)
        snprintf(msg, sizeof(msg), str(LANG_IPODUI_DIAG_DONE),
                 DIAG_REPORT_NAME);
    else
        strlcpy(msg, str(a->diag < 0 ? LANG_IPODUI_DIAG_RETRY
                                     : LANG_IPODUI_DIAG_HINT), sizeof(msg));
    memset(&vp, 0, sizeof(vp));
    vp.width = LCD_WIDTH;
    vp.height = LCD_HEIGHT;
    vp.flags = VP_DEFAULT_FLAGS;
    vp.drawmode = DRMODE_FG;
    vp.font = ui_theme->font_title;
    lcd_set_viewport(&vp);
    lcd_set_foreground(LCD_WHITE);
    ui_paint_text_wrap(LCD_WIDTH / 2, P4_TEXT_BASE, P4_TEXT_W, P4_LINES,
                       P4_PITCH, msg);
    lcd_set_viewport(NULL);
}

static void draw_dots(int page)
{
    static const signed char inset[DOT_D] = { 2, 1, 0, 0, 1, 2 };
    int i, j;

    lcd_set_viewport(NULL);
    for (i = 0; i < PAGES; i++)
    {
        int x = LCD_WIDTH / 2 - DOT_D / 2
                + (2 * i - (PAGES - 1)) * DOT_PITCH / 2;

        lcd_set_foreground(i == page ? C_DOT_ON : C_DOT_OFF);
        for (j = 0; j < DOT_D; j++)
            lcd_hline(x + inset[j], x + DOT_D - 1 - inset[j], DOT_Y + j);
    }
}

/* the sliding part: the page's content and the dots, over the body */
static void draw_page(const struct about *a)
{
    about_body(PAGE_Y, PAGE_H);
    if (a->page == 0)
        page_capacity(a);
    else if (a->page == 1)
        page_counts(a);
    else if (a->page == 2)
        page_device();
    else
        page_diag(a);
    draw_dots(a->page);
}

/* -- the screen ------------------------------------------------------------ */

static void read_capacity(struct about *a)
{
    sector_t size = 0, freek = 0;

#ifdef SIMULATOR
    /* a scripted run shows the Settings panel's pinned volume (the sim's
     * disk is the host's) */
    if (sim_script_file)
    {
        size = 930ul * 1024 * 1024;
        freek = 29465000;
    }
    else
#endif
    volume_size(IF_MV(0,) &size, &freek);   /* KiB, FSInfo in RAM */
    a->cap_kib = size;
    a->free_kib = freek;
}

static int about_enter(struct ui_screen *s, const void *arg)
{
    struct about *a = ui_arena_alloc(sizeof(*a));

    (void)arg;
    if (!a)
        return -1;
    s->priv = a;
    s->title = ui_device_name();
    s->header_w = UI_HEADER_DARK;
    read_capacity(a);
    a->stats_ok = lib_stats(&a->st);
    /* MiB -> KiB; Other is what the rest of the used space holds */
    a->audio_kib = a->stats_ok ? (unsigned long)a->st.audio_mib * 1024 : 0;
#ifdef HAVE_BCMFW_VIDEO
    {
        struct libfile_vstats vs;

        if (lib_vstats(&vs))
        {
            a->video_kib = (unsigned long)vs.video_mib * 1024;
            a->videos = vs.videos;
        }
    }
#endif
    a->page = 0;
    a->diag = 0;
    UPDATE_TRACE("about page=%d songs=%ld episodes=%ld", 1,
                 a->stats_ok ? (long)a->st.songs : -1L,
                 a->stats_ok ? (long)a->st.episodes : -1L);
#ifdef HAVE_BCMFW_VIDEO
    UPDATE_TRACE("about videos=%ld video_kib=%lu", a->videos, a->video_kib);
#endif
    return 0;
}

static void about_draw(struct ui_screen *s, unsigned dirty)
{
    struct about *a = s->priv;
    char gb[16], line[48];

    if (!(dirty & (UI_DIRTY_BODY | UI_DIRTY_BAND)))
        return;
    about_body(UI_DARK_HDR_H, PAGE_Y - UI_DARK_HDR_H);
    draw_band();
    draw_logo();
    ui_fmt_gb(a->cap_kib - MIN(a->free_kib, a->cap_kib), gb, sizeof(gb));
    snprintf(line, sizeof(line), str(LANG_IPODUI_GB_USED), gb);
    text_at(ui_theme->font_title, GB_LEFT, GB_BASE, -1, line);
    ui_fmt_gb(a->free_kib, gb, sizeof(gb));
    snprintf(line, sizeof(line), str(LANG_IPODUI_GB_FREE), gb);
    text_at(ui_theme->font_title, GB_RIGHT, GB_BASE, 1, line);
    draw_page(a);
}

static enum ui_result about_input(struct ui_screen *s,
                                  const struct ui_input *in)
{
    struct about *a = s->priv;
    int dir;

    switch (in->key)
    {
        case UI_KEY_NEXT:
        case UI_KEY_PREV:
            /* a page per release; never a skip or a seek here. With
             * SELECT down it is (part of) the developer chord: no page */
            if (!in->release || in->long_press
                || (button_status() & BUTTON_SELECT))
                return UI_HANDLED;
            dir = in->key == UI_KEY_NEXT ? 1 : -1;
            if (a->page + dir < 0 || a->page + dir >= PAGES)
                return UI_HANDLED;      /* no wrap */
            a->page += dir;
            UPDATE_TRACE("about page=%d", a->page + 1);
            ui_slide_band(dir, PAGE_Y, PAGE_H);
            return UI_HANDLED;
        case UI_KEY_SELECT:
            /* page 4 only, on the release (never part of the chord) */
            if (a->page != PAGE_DIAG || !in->release || in->long_press
                || (button_status() & (BUTTON_LEFT | BUTTON_RIGHT)))
                return UI_HANDLED;
            a->diag = 2;                /* "Saving" while the file goes */
            draw_page(a);
            ui_lcd_push("about", 0, PAGE_Y, LCD_WIDTH, PAGE_H);
            a->diag = diag_save_report() ? 1 : -1;
            UPDATE_TRACE("about diag=%d", a->diag);
            ui_invalidate(UI_DIRTY_BODY);
            return UI_HANDLED;
        case UI_KEY_WHEEL:
            return UI_HANDLED;          /* nothing (owner, 2026-10-06) */
        default:
            return UI_IGNORED;          /* MENU returns, PLAY plays */
    }
}

/* The real About replaces stub.c's under the same symbol and name: the
 * developer chord arms on this pointer (devmode.c, Phase 8a M6) */
const struct ui_screen_ops ui_about_screen = {
    .name    = "about",
    .enter   = about_enter,
    .leave   = NULL,                    /* arena: released by the pop */
    .draw    = about_draw,
    .input   = about_input,
    .timeout = NULL,
};
