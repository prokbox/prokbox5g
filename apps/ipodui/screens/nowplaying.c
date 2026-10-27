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
 * iPod UI shell: Now Playing (Phase 5, #4)
 *
 * The 7G's Now Playing (docs/design/spec/now-playing.md): the slanted art
 * with its reflection, title (marquee), artist, album or chapter, rating
 * and "n of N", and the bar row with its SELECT cycle (Normal / Scrubber /
 * Rating / Shuffle / Lyrics) and the volume overlay. Everything comes from
 * the playback facade (np_playback.h) and the art cache; the pixel work is
 * np_draw.c, the cycle np_state.c, the scrub maths np_scrub.c.
 *
 * Memory (docs/phase5-brief.md §6, D13/D14): one "np" block, reserved on
 * the shell's first main-loop pass (before anything can play) and never
 * freed, at the fixed offsets of np_layout.h; this screen's own state is
 * in the block's spare bytes after NP_BLK_END. The strip, claimed only
 * as UI_STRIP_OWNER_NP, holds the title's marquee image, the two row
 * captures of a mode slide and the lyrics. Nothing is allocated after
 * shell start; statics are a handle and two flags.
 *
 * Drawing: input() only changes state; tick() draws what changed and
 * pushes full-width bands (the bar row y 196-229 at 1 Hz, on scrub,
 * volume and mode changes; the title band y 58-75 per marquee step; the
 * body on a track change and Lyrics in or out). draw() is the full
 * repaint the main loop asks for (push, pop, boot).
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
/* not ownable (file): a full-screen page or takeover; the drift panel was settled on the way in (ui_panel_hide(), or the takeover's own ui_fb_settle()), and a full lcd_update() releases it (bcmjob_owned_hook) */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "storage.h"
#ifdef HAVE_STORAGE_POWEROFF
#include "stor_pwr.h"         /* npfix: the jump line's pwr= */
#endif
#include "kernel.h"
#include "lcd.h"
#include "font.h"
#include "button.h"
#include "audio.h"
#include "backlight.h"
#include "settings.h"
#include "ipodui/lock.h"
#include "ipodui/uilog.h"
#include "core_alloc.h"
#include "lang.h"
#include "logf.h"
#include "ipodui/anim.h"
#include "ipodui/theme.h"
#include "ipodui/paint.h"
#include "ipodui/artcache.h"
#include "ipodui/artfmt.h"
#include "ipodui/np_playback.h"
#include "ipodui/ab_book.h"
#include "ipodui/np_layout.h"
#include "ipodui/viz.h"
#include "ipodui/header.h"          /* UI_HEADER_NONE (#126 full screen) */
#include "ipodui/preview.h"         /* ui_fb_settle() */
#include "ipodui/np_draw.h"
#include "ipodui/material.h"
#include "ipodui/np_lyrics.h"
#include "ipodui/screens/nowplaying.h"
#include "ipodui/screens/holdmenu.h"
#include "ipodui/vlimit.h"
#include "ipodui/mq_jobs.h"         /* P19 19-5: the title as a BCM PAN */
#ifdef HAVE_MQ_JOBS
#include "ipodui/artkey.h"          /* the strip's key */
#include "bcmjob_pp.h"              /* the simulator's parity check */
#endif
#ifdef SIMULATOR
#include "ipodui/simclock.h"
#include "file.h"
#include "rbpaths.h"
#endif

/* np_draw.c packs colours itself: it must agree with the LCD */
NP_STATIC_ASSERT(NP_RGB(0x12, 0x34, 0x56) == LCD_RGBPACK(0x12, 0x34, 0x56));
NP_STATIC_ASSERT(sizeof(fb_data) == sizeof(np_px));

/* ---- layout (spec now-playing.md; ≈ values from the owner's photos) --- */
#define BODY_Y          20
#define BODY_H          (LCD_HEIGHT - BODY_Y)
#define TXT_X           160
#define TXT_W           155             /* to x 315 */
/* The text block at the owner's 2.0.5 positions (fidelity audit 1, row 1:
 * cap tops 71 / 91 / 109 / 146, rating stars y 128-136 measured on the
 * rectified photo 1208). */
#define TITLE_BAND_Y    69              /* title cap top 71 */
#define TITLE_BAND_H    NP_MARQUEE_H    /* 18 */
#define TITLE_CAP_TOP   71
#define CAP_UI          11              /* Bold 15 */
#define CAP_SMALL       9               /* Bold 12 */
#define ARTIST_CAP_TOP  91
#define ALBUM_CAP_TOP   109
#define STARS_Y         128             /* y 128-137 */
#define STAR_PITCH_S    12
#define STARS_BAND_Y    (STARS_Y - 1)   /* NPD_STARS: y 127-138 */
#define STARS_BAND_H    12
#define COUNT_CAP_TOP   146
#define LINE_BAND_H     18              /* a Bold 12 line incl. descenders */
#define ALBUM_BAND_Y    (ALBUM_CAP_TOP - 4)     /* y 105-122 */
#define COUNT_BAND_Y    (COUNT_CAP_TOP - 4)     /* y 142-159 */
#define INFO_Y          BODY_Y
#define INFO_H          (NP_ROW_Y - BODY_Y)
#define MQ_WIN          (NP_MARQUEE_W - 1240)   /* 152: x 160-311 */
#define MQ_GAP          40              /* list marquee rules (9.8) */
#define MQ_CAP          1200
#define MQ_DELAY        HZ
#define MQ_PAUSE        HZ
#define MQ_STEP_TICKS   5
#define MQ_STEP_PX      2               /* 40 px/s */
#define GLYPH_Y         24              /* repeat / shuffle, y 23-37 */
#define GLYPH_R         312
#define GLYPH_GAP       6
#define GLYPH_BAND_Y    (BODY_Y + 1)    /* NPD_GLYPHS: y 21-38 */
#define GLYPH_BAND_H    18
#define GLYPH_BAND_X    262
/* bar row (measured on the rectified photo 1203, 1208, 1222) */
#define BAR_Y           206             /* y 206-219, reflection 220-224;
                                           the art's reflection runs behind
                                           it to y 219 (P11) */
#define LABEL_Y         209             /* atlas top: cap top 210 */
#define ATLAS_H         12
#define ATLAS_BASE      10              /* baseline row in the atlas */
#define MINUS_GAP       1               /* extra px after the "-" (owner) */
#define VOL_X           56              /* volume track x 56-268 */
#define VOL_W           213
#define SPK_X           42              /* ink x 42-51: 4 px before the bar */
#define SPKW_X          273             /* ink from x 273: 4 px after it */
#define RATE_PITCH      21              /* 19 px stars, tips ~2 px apart */
#define RATE_Y          203             /* stars y 203-221 (photo 1206) */
#define STAR_REFL       4               /* flat reflection rows */
#define SHELF_X         70              /* the sunken channel x 70-269 */
#define SHELF_W         200
#define SHELF_Y         204             /* y 204-229 */
#define SHELF_H         26
#define SHELF_R         4
#define BTN_Y           (SHELF_Y + 1)   /* the raised button y 205-224 */
#define BTN_H           20
#define BTN_R           3
#define BTN_SHADOW      3               /* soft shadow rows under it */
#define SHUF_ICON_X     30              /* ink x 32-52 */
#define SHUF_ICON_Y     208             /* ink y 209-225 */
#define SHELF_CAP_TOP   212             /* option text, as photo 1208 */
/* lyrics page (owner, 2026-10-08, lyrics-screenshot.png) */
#define LYR_X           10              /* lines left-aligned, ~x 11 */
#define LYR_TITLE_CAP   31              /* centred title, Bold 12 */
#define LYR_CAP0        60              /* first lyric line, Regular 12 */
#define LYR_PITCH       18              /* ~17-18 px, 10 lines visible */
#define LYR_SB_X        316             /* the lists' scrollbar column */
#define LYR_SB_W        4
/* timing */
#define SLIDE_TICKS     ((HZ * 19 + 99) / 100)  /* 190 ms (9.4) */
#define STATE_POLL      (HZ / 10)               /* settle / timeouts */
#define WAKE_POLL       (HZ / 20)               /* lit yet? (after a wake) */

/* colours */
#define C_BG        LCD_RGBPACK(0xff, 0xff, 0xff)
#define C_TITLE     LCD_RGBPACK(0x00, 0x00, 0x00)
#define C_DIM       LCD_RGBPACK(0x3c, 0x3c, 0x3c)
#define C_GLYPH     LCD_RGBPACK(0x55, 0x55, 0x55)
#define C_SPEAKER   LCD_RGBPACK(0x1c, 0x1c, 0x1c)   /* near-black, solid */
#define C_SPEAKER_DARK LCD_RGBPACK(0xd8, 0xe0, 0xea) /* #126: over the dark Visualizer */
#define C_STAR_S    LCD_RGBPACK(0x22, 0x22, 0x22)
#define C_DOT       LCD_RGBPACK(0x1a, 0x1a, 0x1a)
#define C_SEL_TXT   LCD_RGBPACK(0x2b, 0x67, 0xd0)
#define C_LYRICS    LCD_RGBPACK(0x3c, 0x3c, 0x3c)

/* What tick() still has to draw */
#define NPD_ROW     0x01
#define NPD_INFO    0x02
#define NPD_BODY    0x04
#define NPD_ART     0x08
#define NPD_TITLE   0x10
#define NPD_ROWFULL 0x20            /* the row from scratch (geometry, art) */
#define NPD_STARS   0x40            /* the text block's rating band only */
#define NPD_GLYPHS  0x80            /* the repeat / shuffle glyph band only */
#define NPD_CHAPTER 0x100           /* the album/chapter line + counter */

enum slide_kind { SL_NONE = 0, SL_ROW, SL_LYR_IN, SL_BODY_OUT };

/* Pre-computed colours and tiles (the glyph-tile area of the block),
 * built once per boot: every bar-row material is a table copy at draw
 * time (lean rules). */
#define STAR_W      20                  /* npglyphs NPG_STAR_BIG box */
#define STAR_H      19
#define STAR_TILE_H (STAR_H + STAR_REFL)
struct np_styles {
    struct np_bar_style bar;
    np_px diamond[15], diamond_edge;
    np_px dia_on[15 * 15];      /* the diamond over the track ... */
    np_px dia_off[15 * 15];     /* ... and over the background */
    np_px star[STAR_H];         /* the azure gloss, per row */
    np_px star_col[STAR_W * STAR_TILE_H];   /* the star + reflection ... */
    uint8_t star_a[STAR_W * STAR_TILE_H];   /* ... and its alpha, 0..32 */
    np_px shelf[SHELF_H];       /* the sunken channel, per row */
    np_px btn[BTN_H];           /* the raised button, per row */
};
NP_STATIC_ASSERT(sizeof(struct np_styles) <= NP_BLK_GLYPHS_SIZE);

/* This screen's state, in the block after NP_BLK_END */
struct np_scr {
    bool ready;                 /* styles and atlas built */
    bool have_track;
    bool stopped;               /* playback ended: pop */
    bool lit;
    bool viz_full;              /* #126: the Visualizer's page shows: no
                                   header (UI_HEADER_NONE), the panel's */
    long wake_until;            /* painted before the backlight thread
                                   turned the light on: poll until then
                                   (0: not waiting) */
    unsigned dirty;             /* NPD_* */
    /* art: the block's art area holds a COMPOSED image (QW2) */
    artcache_key_t art_key;     /* what the block's art area holds */
    bool art_held;              /* the art area holds art_key's image */
    bool art_ph;                /* ... or the placeholder */
    bool art_ph_book;           /* ... of a book (the book glyph) */
    bool art_legacy;            /* ... or a fitted (version 1) bundle,
                                   art_w x art_h, until draw_art()
                                   composes it in place of the source */
    bool art_defer;             /* the placeholder is up; the read is
                                   the next pass (disk asleep), or the
                                   art thread's (npf: a track's cover) */
    long art_defer_t;           /* npf: when the request went out */
    int  art_w, art_h;
    artcache_key_t art_want;
    bool art_pending;           /* PENDING: re-read on a commit */
    int  art_commit;
    /* title marquee */
    int  title_w;
    int  mq_x;
    long mq_next;
    bool mq_ok;                 /* strip image matches the title */
    bool mq_bcm;                /* P19 19-5: the BCM steps it (a PAN) */
    /* bar row */
    enum np_mode row_mode;      /* what the row shows */
    bool row_volume;
    int  bar_x, bar_w;
    int  lw, rw;                /* label slot widths */
    int  fill_px;               /* drawn: fill (Normal) */
    int  dia_x;                 /* drawn: diamond left (Scrubber), -1 */
    int  vol_px;                /* drawn: volume fill */
    int  row_rating;            /* drawn: rating row stars */
    long shown_sec;             /* drawn: elapsed second, -1 = none */
    long sec_tick;              /* current_tick when shown_sec changed,
                                   0: unknown (a full redraw) */
    unsigned long sec_pos;      /* the position then */
    long shown_rem;
    char lbl_l[12], lbl_r[12];  /* drawn: the time labels */
    int  ticks_n;
    int  chapter;               /* array index, -1 */
    struct np_scrub_ctx bscrub; /* a book's scrub context (scrub_ctx()) */
    struct np_rating_ovr ovr;
    char rate_path[MAX_PATH];   /* the file a rating commit is for: the
                                   page's track, kept past a track change
                                   for the old track's pending commit */
    /* slides */
    enum slide_kind slide;
    enum slide_kind slide_req;
    long slide_start;
    int  slide_done;
    /* lyrics */
    bool lyr_ok;                /* text and line table valid in the strip */
    int  lyr_len, lyr_lines, lyr_lh, lyr_vis, lyr_drawn;
    struct np_atlas atlas;
    bool band_ok;               /* the reflection band below is valid */
};

/* 8-aligned (#121): np_scr holds 64-bit keys (and, on the 64-bit sim,
 * pointers and longs); the block itself starts on buflib's alignment */
#define NP_SCR_OFF  ((NP_BLK_END + 7u) & ~7u)
NP_STATIC_ASSERT(NP_SCR_OFF + sizeof(struct np_scr) <= NP_BLK_SIZE);
NP_STATIC_ASSERT(NP_STRIP_BODY_SIZE == LCD_WIDTH * BODY_H * sizeof(fb_data));
NP_STATIC_ASSERT(NP_REFL_BAND_H > 0 && NP_ROW_Y + NP_REFL_BAND_H
                 == NP_REFL_LIMIT);
NP_STATIC_ASSERT(NP_REFL_BAND_X == NP_SLANT_X
                 && NP_REFL_BAND_W == NP_SLANT_W);
/* QW2: the art area is the composed rect, and one read() of a bundle
 * (header + pixels) lands in NP_BLK_HDR + the art area */
NP_STATIC_ASSERT(NP_COMP_X == NP_COMPOSED_X && NP_COMP_Y == NP_COMPOSED_Y
                 && NP_COMP_W == NP_COMPOSED_W && NP_COMP_H == NP_COMPOSED_H);
NP_STATIC_ASSERT(NP_BLK_HDR_SIZE == ARTBUNDLE_HDR_BYTES
                 && NP_BLK_ART_OFF == NP_BLK_HDR_OFF + NP_BLK_HDR_SIZE
                 && NP_BLK_HDR_SIZE + NP_BLK_ART_SIZE == ARTCACHE_NP_ONE_READ);
NP_STATIC_ASSERT(NP_ART_BOX * NP_ART_BOX * 2 <= NP_BLK_ART_SIZE);

struct np_mem {
    char *hdr;                  /* NP_BLK_HDR: a bundle read starts here */
    np_px *art;                 /* the composed art, NP_COMP_W x _H */
    np_px *atlas;
    struct np_styles *sty;
    struct np_track *t;
    struct np_state *st;
    uint8_t *ticks;
    struct np_scr *sc;
    np_px *band;                /* NP_REFL_BAND_W x NP_REFL_BAND_H: an
                                   alias of the art's rows 164-170 */
};

static int np_handle;           /* the "np" block, 0 = none */
static bool np_reserved;        /* reservation tried */
static bool np_pop_posted;
#ifdef SIMULATOR
static bool sim_slow_pass;          /* ROCKBOX_DIR/.sim_np_slowpass */
#endif
static long np_last_input;      /* auto-return: the last input event */
static bool np_input_hooked;    /* the shell reports input (else off) */

/* ---- the block -------------------------------------------------------- */

static bool np_reserve(void)
{
    if (np_reserved)
        return np_handle > 0;
    np_reserved = true;
    np_handle = core_alloc(NP_BLK_SIZE);
    if (np_handle <= 0)
    {
        np_handle = 0;
        logf("np: no memory for the np block (%d B)", NP_BLK_SIZE);
        return false;
    }
    memset(core_get_data(np_handle), 0, NP_BLK_SIZE);
    return true;
}

/* Pins the block for one callback (it may yield inside artcache_get) */
static bool mem_get(struct np_mem *m)
{
    char *b;

    if (!np_handle)
        return false;
    core_pin(np_handle);
    b = core_get_data(np_handle);
    m->hdr = b + NP_BLK_HDR_OFF;
    m->art = (np_px *)(b + NP_BLK_ART_OFF);
    m->atlas = (np_px *)(b + NP_BLK_DIGITS_OFF);
    m->sty = (struct np_styles *)(b + NP_BLK_GLYPHS_OFF);
    m->t = (struct np_track *)(b + NP_BLK_TRACK_OFF);
    m->st = (struct np_state *)(b + NP_BLK_STATE_OFF);
    m->ticks = (uint8_t *)(b + NP_BLK_TICKS_OFF);
    m->sc = (struct np_scr *)(b + NP_SCR_OFF);
    m->band = m->art + NP_REFL_BAND_ROW * NP_COMP_W;
    m->sc->atlas.px = m->atlas;
    return true;
}

static void mem_put(void)
{
    core_unpin(np_handle);
}

/* ---- small helpers ---------------------------------------------------- */

static struct np_canvas lcd_canvas(void)
{
    struct np_canvas cv;

    lcd_set_viewport(NULL);
    cv.px = FBADDR(0, 0);
    cv.stride = (int)(FBADDR(0, 1) - FBADDR(0, 0));
    cv.w = LCD_WIDTH;
    cv.h = LCD_HEIGHT;
    return cv;
}

/* Clears a rectangle of the bar row to its background: white, and the
 * art's reflection where the saved band covers it (np-reflection.md §6),
 * so a row redraw or a slide never cuts the reflection. */
/* the art's reflection shows in the row band (not in the Visualizer,
 * which shows no art: #126) */
static bool band_shown(const struct np_mem *m)
{
    return m->sc->band_ok && m->st->mode != NP_VIZ;
}

static void bg_fill(struct np_mem *m, int x, int y, int w, int h)
{
    struct np_canvas cv = lcd_canvas();
    int x0 = MAX(x, NP_REFL_BAND_X), x1 = MIN(x + w, NP_REFL_BAND_X
                                                   + NP_REFL_BAND_W);
    int y0 = MAX(y, NP_ROW_Y), y1 = MIN(y + h, NP_ROW_Y + NP_REFL_BAND_H);
    int r;

    np_fill(&cv, x, y, w, h, C_BG);
    if (!band_shown(m) || x0 >= x1 || y0 >= y1)
        return;
    for (r = y0; r < y1; r++)
        memcpy(cv.px + r * cv.stride + x0,
               m->band + (r - NP_ROW_Y) * NP_REFL_BAND_W
               + (x0 - NP_REFL_BAND_X),
               (x1 - x0) * sizeof(np_px));
}

static void boost(bool on)
{
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    cpu_boost(on);
#else
    (void)on;
#endif
}

static int cell_y(int font, int cap_top, int cap)
{
    return cap_top + cap - font_get(font)->ascent;
}

static void text_vp(struct viewport *vp, int font, unsigned fg,
                    int x, int y, int w, int h)
{
    memset(vp, 0, sizeof(*vp));
    vp->x = x;
    vp->y = y;
    vp->width = w;
    vp->height = h;
    vp->font = font;
    vp->drawmode = DRMODE_FG;
    vp->fg_pattern = fg;
    vp->bg_pattern = C_BG;
}

/* Text into the current viewport at x, y, ending in "..." if wider than
 * w (artist and album lines). */
static void put_fit(int x, int y, int w, const char *text, int font)
{
    char buf[NP_TXT + 4];
    int tw = 0, ew = 0, n;

    font_getstringsize((const unsigned char *)text, &tw, NULL, font);
    if (tw <= w)
    {
        lcd_putsxy(x, y, (const unsigned char *)text);
        return;
    }
    font_getstringsize((const unsigned char *)"...", &ew, NULL, font);
    n = font_measurestring((const unsigned char *)text, NP_TXT - 1,
                           MAX(w - ew, 0), &tw, NULL, font);
    while (n > 0 && text[n - 1] == ' ')
        n--;
    memcpy(buf, text, n);
    strcpy(buf + n, "...");
    lcd_putsxy(x, y, (const unsigned char *)buf);
}

static int font_bold12(void)
{
    return ui_theme->font_title;
}

/* ---- styles and the digit atlas (once per boot) ----------------------- */

/* ---- materials (spec now-playing.md "Bar row materials", index.md
 * "blue gel"): light from the top. The blue gel and the groove come from
 * the shared material kit (material.h, art/materials.py: key colours
 * measured on the rectified owner photos 1209 / 1222, toned down a
 * little for the camera's saturation; tests/material pins them to the
 * values this screen had before the move). */
/* the gel's faint lighter top line and its 1 px darker bottom edge */
#define GEL_TOP     ui_mat_color(UI_MAT_GEL_BLUE, 0, NP_BAR_H)
#define GEL_EDGE    ui_mat_color(UI_MAT_GEL_BLUE, NP_BAR_H - 1, NP_BAR_H)

/* n rows of material m from the kit (n is one of its run-time heights) */
static void mat_rows(np_px *rows, enum ui_mat m, int n)
{
    const uint16_t *t = ui_mat_rows(m, n);
    int i;

    for (i = 0; i < n; i++)
        rows[i] = t ? t[i] : ui_mat_color(m, i, n);
}

/* The flat reflection rows under an object whose rows are src[h] */
static void flat_refl(np_px *out, int n, const np_px *src, int h)
{
    int d;

    for (d = 0; d < n; d++)
        out[d] = np_blend256(C_BG, src[MAX(h - 2 - d, 0)],
                             ui_mat_refl_a(d, h, n));
}


static void build_styles(struct np_styles *s)
{
    struct np_canvas tc;
    np_px grow[NP_BAR_H];
    int i, r;

    /* the bars: gel fill, groove track, their flat reflections */
    mat_rows(s->bar.fill, UI_MAT_GEL_BLUE, NP_BAR_H);
    mat_rows(grow, UI_MAT_GROOVE, NP_BAR_H);
    for (i = 0; i < NP_BAR_H; i++)
    {
        s->bar.track[i] = grow[i];
        /* the fill's left end slightly darker (rounded look); the
         * groove's square ends a touch darker than its body */
        s->bar.fill_end[i] = np_blend256(s->bar.fill[i], GEL_EDGE, 110);
        s->bar.side[i] = np_blend256(grow[i], LCD_RGBPACK(0x80, 0x80, 0x80),
                                     (i == 0 || i == NP_BAR_H - 1) ? 0 : 70);
    }
    flat_refl(s->bar.fill_refl, NP_BAR_REFL, s->bar.fill, NP_BAR_H);
    flat_refl(s->bar.track_refl, NP_BAR_REFL, s->bar.track, NP_BAR_H);
    /* under the grey groove the reflection is very faint */
    for (i = 0; i < NP_BAR_REFL; i++)
        s->bar.track_refl[i] = np_blend256(C_BG, s->bar.track_refl[i], 110);
    s->bar.tick = LCD_RGBPACK(0xa0, 0xa0, 0xa0);    /* subtle */
    s->bar.tick_fill = GEL_EDGE;

    /* the scrubber diamond in the gel, composed once over what it moves
     * on: row 0 is the background above the bar, rows 1-14 the groove */
    mat_rows(s->diamond, UI_MAT_GEL_BLUE, 15);
    s->diamond_edge = LCD_RGBPACK(0x14, 0x44, 0x96);
    tc.px = s->dia_on;
    tc.stride = tc.w = tc.h = 15;
    for (r = 0; r < 15; r++)
        np_fill(&tc, 0, r, 15, 1, r ? s->bar.track[r - 1] : C_BG);
    np_glyph_draw(&tc, NPG_DIAMOND, 0, 0, s->diamond, 0, s->diamond_edge);
    tc.px = s->dia_off;
    np_fill(&tc, 0, 0, 15, 15, C_BG);
    np_glyph_draw(&tc, NPG_DIAMOND, 0, 0, s->diamond, 0, s->diamond_edge);

    /* the rating star: a colour + alpha tile (composited over whatever is
     * behind it: the art's reflection reaches the stars' top rows, owner
     * on photo 1206), the blue gel down the star (deeper top, brightest
     * just above the bottom), a faint lighter line along the top edges of
     * the arms and a slightly darker bottom edge, then its faint flat
     * reflection: the rows mirrored about its bottom row */
    mat_rows(s->star, UI_MAT_GEL_BLUE, STAR_H);
    for (r = 0; r < STAR_H; r++)
        for (i = 0; i < STAR_W; i++)
        {
            unsigned c = np_glyph_cov(NPG_STAR_BIG, i, r);
            unsigned above = np_glyph_cov(NPG_STAR_BIG, i, r - 1);
            unsigned below = np_glyph_cov(NPG_STAR_BIG, i, r + 1);
            np_px col = s->star[r];

            if (c && above < 8)     /* a top edge: faint, thin */
                col = np_blend256(col, GEL_TOP, 90);
            else if (c && below < 8)    /* a bottom edge */
                col = np_blend256(col, GEL_EDGE, 110);
            s->star_col[r * STAR_W + i] = col;
            s->star_a[r * STAR_W + i] = (uint8_t)((c * 32 + 7) / 15);
        }
    for (r = 0; r < STAR_REFL; r++)
    {
        unsigned a = ui_mat_refl_a(r, STAR_H, STAR_REFL);
        for (i = 0; i < STAR_W; i++)
        {
            int src = (STAR_H - 1 - r) * STAR_W + i;
            s->star_col[(STAR_H + r) * STAR_W + i] = s->star_col[src];
            s->star_a[(STAR_H + r) * STAR_W + i] =
                (uint8_t)((s->star_a[src] * a + 128) >> 8);
        }
    }

    /* the shuffle shelf: a sunken channel (dark inner top edge, light
     * inner bottom edge, a floor a touch darker than the page) */
    for (i = 0; i < SHELF_H; i++)
    {
        int g = 0xd2 + (i * 0x10) / SHELF_H;    /* floor, lighter down */
        if (i == 0)
            g = 0x96;
        else if (i == 1)
            g = 0xb2;
        else if (i == 2)
            g = 0xc6;
        else if (i == SHELF_H - 1)
            g = 0xf2;
        s->shelf[i] = (np_px)LCD_RGBPACK(g, g, g + 2 > 255 ? 255 : g + 2);
    }
    /* ... and the raised satin button: a light top edge, a body from a
     * lighter top to a slightly darker bottom, a darker bottom edge */
    for (i = 0; i < BTN_H; i++)
    {
        int g = 0xfc - (i * 0x1e) / BTN_H;
        if (i == 0)
            g = 0xff;
        else if (i == BTN_H - 1)
            g = 0xa6;
        else if (i == BTN_H - 2)
            g = 0xc8;
        s->btn[i] = (np_px)LCD_RGBPACK(g, g, g);
    }
}

/* "0-9 : -" in Bold 12 on white, rendered once: the 1 Hz labels are then
 * row copies instead of font rendering. */
static void build_atlas(struct np_mem *m)
{
    static const char chars[] = NP_ATLAS_CHARS;
    struct np_atlas *a = &m->sc->atlas;
    struct font *pf = font_get(font_bold12());
    int font = font_bold12();
    int maxw = NP_BLK_DIGITS_SIZE / (int)sizeof(np_px) / ATLAS_H;
    struct frame_buffer_t fb;
    struct viewport vp;
    int i, x = 0;

    for (i = 0; i < NP_ATLAS_N; i++)
    {
        int w = font_get_width(pf, (unsigned char)chars[i]);
        if (chars[i] == '-')
            w += MINUS_GAP;         /* the 7G's wider "- 4:06" (owner) */
        if (x + w > maxw)
            w = 0;                  /* logged below: never on ShellSans */
        a->x[i] = (uint8_t)x;
        a->w[i] = (uint8_t)w;
        x += w;
    }
    if (x >= maxw)
        logf("np: digit atlas full (%d px)", x);
    a->px = m->atlas;
    a->stride = maxw;
    a->h = ATLAS_H;

    memset(&fb, 0, sizeof(fb));
    fb.fb_ptr = m->atlas;
    fb.stride = STRIDE_MAIN(maxw, ATLAS_H);
    fb.elems = LCD_NBELEMS(maxw, ATLAS_H);
    text_vp(&vp, font, C_TITLE, 0, 0, maxw, ATLAS_H);
    vp.buffer = &fb;
    lcd_set_viewport(&vp);
    /* cleared directly, not with an INVERSEVID fill: that one copies from
     * the LCD backdrop when one is set, out of this buffer's bounds */
    for (i = 0; i < maxw * ATLAS_H; i++)
        m->atlas[i] = C_BG;
    lcd_set_drawmode(DRMODE_FG);
    for (i = 0; i < NP_ATLAS_N; i++)
    {
        char s[2] = { chars[i], '\0' };
        if (a->w[i])
            lcd_putsxy(a->x[i], ATLAS_BASE - pf->ascent,
                       (const unsigned char *)s);
    }
    lcd_set_viewport(NULL);
}

/* ---- track facts ------------------------------------------------------ */

static int shown_stars(struct np_mem *m)
{
    if (m->st->mode == NP_RATING)
        return m->st->rating;
    return np_rating_shown(&m->sc->ovr, m->t->tc_idx, m->t->db_gen,
                           m->t->stars);
}

/* The chapter table is playback's: usable only while seq is unchanged,
 * with no yield before the use (np_pbcore.h). */
static bool chapters_ok(struct np_mem *m)
{
    return m->sc->have_track && m->t->scrub.ch.count > 0
           && np_pb_seq() == m->t->seq;
}

/* ---- the whole book (P6 §3.4, owner Rev 2) ------------------------------
 * A multi-file book queue whose map matched the playing file (t->book
 * active, filled by np_pb_snapshot()) spans the whole book, so it feels
 * like one m4b: the bar, both times, the ticks, the counter and the
 * scrub are book positions (file start + the file's elapsed). A
 * single-file book keeps the Phase 5 path: the file is the book, and its
 * chapters already give the bar, the ticks and the counter. */
NP_STATIC_ASSERT(AB_TICK_RES == NP_BAR_W);
NP_STATIC_ASSERT(NP_BOOK_TICK_BYTES == AB_TICK_BYTES);

static bool book_on(const struct np_mem *m)
{
    const struct np_track *t = m->t;
    return t->book.active && t->book.len_ms > 0 && t->queue_count > 1;
}

/* What the bar spans: the book, or the file */
static unsigned long bar_len(const struct np_mem *m)
{
    return book_on(m) ? m->t->book.len_ms : m->t->length_ms;
}

/* A shown (book) position in the playing file */
static unsigned long file_pos(const struct np_mem *m, unsigned long pos)
{
    unsigned long fs = m->t->book.file_start_ms;

    if (!book_on(m))
        return pos;
    return pos > fs ? pos - fs : 0;
}

/* The book block's map while it is the one the snapshot matched (same
 * length and file count), else NULL. Valid until the next yield. */
static const struct ab_book_map *book_map(const struct np_mem *m)
{
    const struct ab_book_block *b = ab_book_block();

    if (!b || !book_on(m) || b->map.file_count != m->t->queue_count
        || b->map.len_ms != m->t->book.len_ms)
        return NULL;
    return &b->map;
}

/* What np_state scrubs: the track's own context, or for a book its
 * length with the book's file starts (the map) and the playing file's
 * chapters (playback's table) as the snap set (§3.4: fast = snap to
 * entry starts, within the file from the slot, across files from the
 * map). Both columns may move, so this is set again before every
 * np_event() (np_scrub.h), with no yield in between. */
static const struct np_scrub_ctx *scrub_ctx(struct np_mem *m)
{
    struct np_scrub_ctx *c = &m->sc->bscrub;
    const struct ab_book_map *map;

    if (!book_on(m))
        return &m->t->scrub;
    memset(c, 0, sizeof(*c));
    c->length_ms = m->t->book.len_ms;
    c->snap = true;
    c->ch.stride = c->sub.stride = sizeof(uint32_t);
    map = book_map(m);
    if (map)
    {
        c->ch.start = map->file_start_ms;
        c->ch.count = map->file_count;
    }
    if (chapters_ok(m))
    {
        c->sub = m->t->scrub.ch;
        c->sub_base = m->t->book.file_start_ms;
    }
    return c;
}

/* The book entry now (0-based): the map's entry for (file, its chapter),
 * else the snapshot's. The counter and the album line both come from
 * it (owner 2026-10-08). */
static int book_entry(const struct np_mem *m)
{
    const struct ab_book_map *map = book_map(m);
    int e = -1;

    if (map)
        e = ab_book_entry_at(map, m->t->queue_index,
                             m->sc->chapter > 0 ? m->sc->chapter : 0);
    return e >= 0 ? e : m->t->book.entry;
}

/* Bar geometry, label slots and chapter ticks for the current track */
static void layout_track(struct np_mem *m)
{
    struct np_scr *sc = m->sc;
    unsigned long len = bar_len(m);
    char buf[12];

    np_fmt_time(buf, len, false);
    sc->lw = np_atlas_width(&sc->atlas, buf);
    np_fmt_time(buf, len, true);
    sc->rw = np_atlas_width(&sc->atlas, buf);
    np_bar_span(sc->lw, sc->rw, &sc->bar_x, &sc->bar_w);
    if (sc->bar_w > NP_BAR_W)
        sc->bar_w = NP_BAR_W;
    sc->ticks_n = 0;
    memset(m->ticks, 0, NP_BLK_TICKS_SIZE);
    if (book_on(m))         /* the map's, rescaled to the bar (D11) */
        sc->ticks_n = ab_ticks_scale(m->t->book.ticks, sc->bar_w, m->ticks);
    else if (m->t->is_audiobook && chapters_ok(m))
        sc->ticks_n = np_chapter_ticks(&m->t->scrub.ch, m->t->length_ms,
                                       sc->bar_w, m->ticks);
}

/* Chapter now, title refresh at a change. Returns true if it changed. */
static bool chapter_update(struct np_mem *m, unsigned long pos)
{
    struct np_scr *sc = m->sc;
    int ch;

    if (!m->t->is_audiobook || !chapters_ok(m))
        return false;
    ch = np_chapter_step(&m->t->scrub.ch, file_pos(m, pos), sc->chapter);
    if (ch == sc->chapter)
        return false;
    sc->chapter = ch;
    if (!np_pb_chapter_title(m->t->seq, ch, m->t->album, NP_TXT))
        logf("np: no chapter title %d", ch);
    return true;
}

/* ---- art ---------------------------------------------------------------- */

static unsigned long now_us(void)
{
#if defined(SIMULATOR)
    return sim_clock_now_us();
#elif defined(USEC_TIMER)
    return USEC_TIMER;
#else
    return current_tick * (1000000 / HZ);
#endif
}

/* The placeholder into the art area: decoded from rodata (art/np/npph.py,
 * pixel-identical to the runtime render; ~24k pixels, no division) */
static void art_placeholder(struct np_mem *m, bool book)
{
    struct np_scr *sc = m->sc;
    unsigned long t0;

    if (sc->art_ph && sc->art_ph_book == book && !sc->art_legacy)
        return;
    t0 = now_us();
    (void)t0;                       /* trace only */
    boost(true);
    np_ph_decode(book ? NPG_BOOK : NPG_NOTE, m->art);
    boost(false);
    sc->art_ph = true;
    sc->art_ph_book = book;
    sc->art_legacy = false;
    sc->band_ok = true;
    UPDATE_TRACE("np art src=%s reads=%d us=%lu",
                 book ? "ph-book" : "ph-note", 0, now_us() - t0);
}

/* Makes the art area hold the wanted art (QW2, docs/phase11-brief.md
 * §3.9): a composed bundle is ONE read() of header + pixels straight into
 * NP_BLK_HDR + the art area, and that is all (the slant and the
 * reflection were rendered by the art builder). A legacy (fitted) bundle
 * is read the same way and composed by draw_art(). No art: the
 * placeholder from rodata. 'may_defer': a new key while the disk sleeps
 * shows the placeholder first and reads on the next pass (sc->art_defer),
 * so a spin-up never holds the new track's screen back. Storage: at most
 * one bundle read, only while lit and visible (Revision 1 item 11).
 * npfix: a track's cover never reads on this thread at a track change:
 * the art thread's copy (artcache_np_take(): read ahead at a fill's end,
 * or on request) is one memcpy; until it lands the placeholder stays up
 * (art_defer), NP_NPF_WAIT at most (then the old read, logged, only
 * with storage up; review M4: else the placeholder stays, asked again).
 * A book's chapter art is not covered (review): its key resolves through
 * the art index (lookup() on the UI's read block) to its group's bundle,
 * and the art thread reads bundles by key with no index reader it may
 * share, so a chapter boundary still reads here, one pass after the
 * placeholder when the drive sleeps (a power-up on this thread then).
 * Returns true if the art area changed (draw_art() then). */
#define NP_NPF_WAIT (5 * HZ)
/* npfix: art reads in this np_tick pass (the slow-pass line's cause) */
static unsigned char np_pass_art_reads;

static bool art_load(struct np_mem *m, bool may_defer)
{
    struct np_scr *sc = m->sc;
    artcache_key_t key = sc->art_want;
    struct bitmap bm;
    int res = ARTCACHE_NOART;
    bool book = m->t->is_audiobook && !m->t->is_podcast;
    bool showing_ph, took = false;
    bool cover = key && key == m->t->art_key;
    unsigned long t0;

    /* (a placeholder for no key stays unless song/book changed) */
    if (sc->art_held && sc->art_key == key && !sc->art_pending
        && !sc->art_defer && !(sc->art_ph && sc->art_ph_book != book))
        return false;
    if (cover && (sc->art_key != key || sc->art_defer || !may_defer))
    {
        /* the art thread's copy (npfix) */
        memset(&bm, 0, sizeof(bm));
        res = artcache_np_take(key, &bm, m->hdr, ARTCACHE_NP_ONE_READ);
        if (res != ARTCACHE_NP_NOTYET)
            took = true;
        else if (may_defer && !(sc->art_defer && sc->art_key == key))
        {
            artcache_np_request(key);
            sc->art_defer_t = current_tick;
            sc->art_key = key;
            sc->art_held = true;
            sc->art_pending = false;
            sc->art_defer = true;
            if (sc->art_ph && sc->art_ph_book == book && !sc->art_legacy)
                return false;       /* already up */
            art_placeholder(m, book);
            return true;
        }
        else if (may_defer
                 && TIME_BEFORE(current_tick, sc->art_defer_t + NP_NPF_WAIT))
            return false;           /* still waiting for it */
        else if (may_defer && !artcache_disk_awake())
        {
            /* review M4: the fallback never powers storage up on this
             * thread: the placeholder stays and the art thread is asked
             * again (its read may power it up) */
            UPDATE_TRACE("np art npf timeout disk=%d", 0);
            ui_log("np", "art npf timeout disk=0");
            artcache_np_request(key);
            sc->art_defer_t = current_tick;
            return false;
        }
        else if (may_defer)
        {
            UPDATE_TRACE("np art npf timeout disk=%d", 1);
            ui_log("np", "art npf timeout disk=1");
        }
        if (!took)
            res = ARTCACHE_NOART;
    }
    if (!took && key && may_defer && sc->art_key != key
        && !artcache_disk_awake())
    {
        sc->art_key = key;
        sc->art_held = true;
        sc->art_pending = false;
        sc->art_defer = true;
        if (sc->art_ph && sc->art_ph_book == book && !sc->art_legacy)
            return false;           /* already up */
        art_placeholder(m, book);
        return true;
    }
    sc->art_defer = false;
    showing_ph = sc->art_ph && sc->art_ph_book == book && !sc->art_legacy;
    t0 = now_us();
    (void)t0;                       /* trace only */
    if (took)
        sc->art_commit = artcache_commit_seq();
    else if (key)
    {
        memset(&bm, 0, sizeof(bm));
        sc->art_commit = artcache_commit_seq();
        res = artcache_get(key, ARTCACHE_NOWPLAYING, &bm, m->hdr,
                           ARTCACHE_NP_ONE_READ);
        np_pass_art_reads++;
        if (now_us() - t0 > 300000) /* a spin-up or a busy disk */
            ui_log("np", "art read slow ms=%lu res=%d",
                   (now_us() - t0) / 1000, res);
    }
    sc->art_key = key;
    sc->art_held = true;
    sc->art_pending = (res == ARTCACHE_PENDING);
    if (res == ARTCACHE_OK && bm.data == (void *)m->art)
    {
        if (bm.width == NP_COMP_W && bm.height == NP_COMP_H)
        {
            sc->art_ph = false;
            sc->art_legacy = false;
            sc->band_ok = true;
            UPDATE_TRACE("np art src=%s reads=%d us=%lu", "bundle",
                         took ? 0 : 1, now_us() - t0);
            return true;
        }
        if (bm.width > 0 && bm.height > 0 && bm.width <= NP_ART_BOX
            && bm.height <= NP_ART_BOX)
        {
            /* a bundle from before QW2: rendered at runtime (draw_art())
             * until the builder composes it */
            sc->art_ph = false;
            sc->art_legacy = true;
            sc->art_w = bm.width;
            sc->art_h = bm.height;
            sc->band_ok = false;
            UPDATE_TRACE("np art src=%s reads=%d us=%lu", "legacy",
                         took ? 0 : 1, now_us() - t0);
            return true;
        }
        res = ARTCACHE_ERR;
    }
    if (res != ARTCACHE_NOART && res != ARTCACHE_PENDING)
        logf("np: art read %d", res);
    /* a refused read (a stale layout, a cut file) may have landed in the
     * art area: decode the placeholder again, but the screen only
     * changes if it showed something else */
    if (key)
        sc->art_ph = false;
    art_placeholder(m, book);
    return !showing_ph;
}

/* The art column from y BODY_Y down to the reflection's limit (it reaches
 * into the row band: the caller redraws the row over it): white around
 * the composed rect, the rect a copy of the art area, one memcpy per row.
 * A legacy bundle is rendered here once (np_art_render(), as before QW2)
 * and the result copied into the art area over its source, so from then
 * on it is drawn and its band restored like any other. */
static void draw_art(struct np_mem *m)
{
    struct np_canvas cv = lcd_canvas();
    struct np_scr *sc = m->sc;
    int r;

    if (sc->art_legacy)
    {
        unsigned long t0 = now_us();

        (void)t0;                   /* trace only */
        np_fill(&cv, 0, BODY_Y, TXT_X, NP_REFL_LIMIT - BODY_Y, C_BG);
        boost(true);
        np_art_render(&cv, m->art, sc->art_w, sc->art_h, C_BG);
        for (r = 0; r < NP_COMP_H; r++)
            memcpy(m->art + r * NP_COMP_W,
                   cv.px + (NP_COMP_Y + r) * cv.stride + NP_COMP_X,
                   NP_COMP_W * sizeof(np_px));
        boost(false);
        sc->art_legacy = false;
        sc->band_ok = true;
        UPDATE_TRACE("np art legacy render us=%lu", now_us() - t0);
        return;
    }
    np_fill(&cv, 0, BODY_Y, TXT_X, NP_COMP_Y - BODY_Y, C_BG);
    np_fill(&cv, 0, NP_COMP_Y, NP_COMP_X, NP_COMP_H, C_BG);
    np_fill(&cv, NP_COMP_X + NP_COMP_W, NP_COMP_Y,
            TXT_X - NP_COMP_X - NP_COMP_W, NP_COMP_H, C_BG);
    for (r = 0; r < NP_COMP_H; r++)
        memcpy(cv.px + (NP_COMP_Y + r) * cv.stride + NP_COMP_X,
               m->art + r * NP_COMP_W, NP_COMP_W * sizeof(np_px));
}

/* ---- the info block ----------------------------------------------------- */

static void draw_title_plain(struct np_mem *m)
{
    struct viewport vp;
    int font = ui_theme->font_ui;

    text_vp(&vp, font, C_TITLE, TXT_X, TITLE_BAND_Y,
            m->sc->title_w > MQ_WIN ? MQ_WIN : TXT_W, TITLE_BAND_H);
    lcd_set_viewport(&vp);
    lcd_set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    lcd_fillrect(0, 0, vp.width, vp.height);
    lcd_set_drawmode(DRMODE_FG);
    lcd_putsxy(-m->sc->mq_x, cell_y(font, TITLE_CAP_TOP, CAP_UI)
               - TITLE_BAND_Y, (const unsigned char *)m->t->title);
    if (m->sc->title_w > MQ_WIN)
        lcd_putsxy(m->sc->title_w + MQ_GAP - m->sc->mq_x,
                   cell_y(font, TITLE_CAP_TOP, CAP_UI) - TITLE_BAND_Y,
                   (const unsigned char *)m->t->title);
    lcd_set_viewport(NULL);
}

static void draw_stars_small(int stars)
{
    struct np_canvas cv = lcd_canvas();
    int i;

    np_fill(&cv, TXT_X, STARS_BAND_Y, TXT_W, STARS_BAND_H, C_BG);
    for (i = 0; i < stars; i++)
        np_glyph_draw(&cv, NPG_STAR_SMALL, TXT_X + i * STAR_PITCH_S, STARS_Y,
                      NULL, C_STAR_S, C_STAR_S);
}

static void draw_state_glyphs(struct np_mem *m)
{
    struct np_canvas cv = lcd_canvas();
    int x = GLYPH_R, w, h, rep = np_pb_repeat();

    /* a book queued alone stops at its end (Rev 2): no Repeat glyph */
    if (m->t->is_audiobook && m->t->queue_count <= 1)
        rep = REPEAT_OFF;
    np_fill(&cv, GLYPH_BAND_X, GLYPH_BAND_Y, LCD_WIDTH - GLYPH_BAND_X,
            GLYPH_BAND_H, C_BG);
    if (rep == REPEAT_ALL || rep == REPEAT_ONE || rep == REPEAT_SHUFFLE)
    {
        enum np_glyph_id g = rep == REPEAT_ONE ? NPG_REPEAT_ONE : NPG_REPEAT;
        np_glyph_size(g, &w, &h);
        x -= w;
        np_glyph_draw(&cv, g, x, GLYPH_Y, NULL, C_GLYPH, C_GLYPH);
        x -= GLYPH_GAP;
    }
    /* shuffle never applies to a book (NP-2): no indicator */
    if (!m->t->is_audiobook && np_pb_shuffle() != NP_SHUFFLE_OFF)
    {
        np_glyph_size(NPG_SHUFFLE, &w, &h);
        np_glyph_draw(&cv, NPG_SHUFFLE, x - w, GLYPH_Y, NULL, C_GLYPH,
                      C_GLYPH);
    }
}

/* The album (or chapter title) line and the "n of N" counter: they come
 * from the same chapter entry and are redrawn together (a chapter change
 * pushes just these two bands, NPD_CHAPTER). 'clear': clear the bands
 * first (else draw_info() cleared the whole block). */
static void draw_chapter_lines(struct np_mem *m, bool clear)
{
    struct np_track *t = m->t;
    struct np_canvas cv = lcd_canvas();
    struct viewport vp;
    struct np_counter cnt;
    int font = font_bold12();
    char buf[32];

    if (clear)
    {
        np_fill(&cv, TXT_X, ALBUM_BAND_Y, TXT_W, LINE_BAND_H, C_BG);
        np_fill(&cv, TXT_X, COUNT_BAND_Y, TXT_W, LINE_BAND_H, C_BG);
    }
    text_vp(&vp, font, C_DIM, TXT_X, BODY_Y, TXT_W, INFO_H);
    lcd_set_viewport(&vp);
    put_fit(0, cell_y(font, ALBUM_CAP_TOP, CAP_SMALL) - BODY_Y, TXT_W,
            t->album, font);
    if (book_on(m))
    {
        /* the whole book (Rev 2): entry N of the book's M */
        cnt.src = NP_COUNT_CHAPTER;
        cnt.n = book_entry(m) + 1;
        cnt.m = t->book.entries;
        if (cnt.m <= 0 || cnt.n < 1 || cnt.n > cnt.m)
            cnt.src = NP_COUNT_NONE;
    }
    else
        cnt = np_counter_pick(t->is_audiobook, t->is_podcast,
                              t->queue_index, t->queue_count,
                              m->sc->chapter, t->scrub.ch.count);
    if (cnt.src != NP_COUNT_NONE)
    {
        snprintf(buf, sizeof(buf), str(LANG_NP_N_OF_M), cnt.n, cnt.m);
        lcd_putsxy(0, cell_y(font, COUNT_CAP_TOP, CAP_SMALL) - BODY_Y,
                   (const unsigned char *)buf);
    }
    lcd_set_viewport(NULL);
}

static void draw_info(struct np_mem *m)
{
    struct np_track *t = m->t;
    struct np_canvas cv = lcd_canvas();
    struct viewport vp;
    int font = font_bold12();

    np_fill(&cv, TXT_X, BODY_Y, LCD_WIDTH - TXT_X, INFO_H, C_BG);
    draw_state_glyphs(m);
    draw_title_plain(m);

    text_vp(&vp, font, C_DIM, TXT_X, BODY_Y, TXT_W, INFO_H);
    lcd_set_viewport(&vp);
    put_fit(0, cell_y(font, ARTIST_CAP_TOP, CAP_SMALL) - BODY_Y, TXT_W,
            t->artist, font);
    lcd_set_viewport(NULL);
    draw_chapter_lines(m, false);
    draw_stars_small(shown_stars(m));
}

/* ---- the bar row -------------------------------------------------------- */

static unsigned long elapsed_now(struct np_mem *m)
{
    /* never clamped to the length: it may be 0 (unknown) or stale until
     * np_pb_refresh_length() catches up (the "full bar / -0:00" bug);
     * np_bar_x() and the remaining time clamp on their own. A book:
     * the book position (the file's start + its elapsed) */
    unsigned long e = np_pb_elapsed_ms();
    return book_on(m) ? m->t->book.file_start_ms + e : e;
}

/* The two time labels. 'full': the slots were cleared (a row redraw);
 * else only the characters that changed since the last call. */
/* fb-stall: the cadence of the elapsed label (owner: the times seemed to
 * race, slow and speed up). Each one-second step in steady Normal-mode
 * playback: dpos (playing time since the last step: the audio's pace),
 * lag (how far past the whole second it was drawn: the wake's) and dt
 * (wall time: the UI thread's). Steady: dpos ~ dt ~ 1000, lag small.
 * tests/ui/ipodui-fb-np-cadence.script checks dpos and lag; on the device
 * a step off by more than 300 ms goes to ipodui.log, so a run tells
 * audio (dpos) from UI wake (dt) irregularity. */
static void sec_cadence(struct np_mem *m, unsigned long pos, bool full)
{
    struct np_scr *sc = m->sc;
    long sec = (long)(pos / 1000);

    if (sec == sc->shown_sec && !full)
        return;
    /* npfix: a step of more (or less) than one second is the owner's
     * "skip"; the +1 test below never saw it, so it is logged here with
     * what else ran (the wall time since the last step, storage, boost) */
    if (!full && sc->sec_tick && sc->shown_sec >= 0
        && sec != sc->shown_sec + 1 && m->st->mode == NP_NORMAL
        && !m->st->seek_pending && !m->st->seek_hold
        && np_pb_track() == m->t->track_id /* not a track change */
        && sec != 0)            /* nor a skip or restart: the position
                                   resets before the track id moves */
    {
        long gap = (long)(current_tick - sc->sec_tick) * (1000 / HZ);
        int boost = 0, pwr = 1;
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
        boost = get_cpu_boost_counter();
#endif
#ifdef HAVE_STORAGE_POWEROFF
        pwr = storage_powered() ? 1 : 0;
#endif
        UPDATE_TRACE("np jump sec=%ld->%ld gap_ms=%ld", sc->shown_sec, sec,
                     gap);
        ui_log("np", "jump sec=%ld->%ld gap_ms=%ld disk=%d boost=%d pwr=%d",
               sc->shown_sec, sec, gap, storage_disk_is_active() ? 1 : 0,
               boost, pwr);
    }
    if (!full && sc->sec_tick && sec == sc->shown_sec + 1
        && m->st->mode == NP_NORMAL && !m->st->seek_pending
        && !m->st->seek_hold && pos >= sc->sec_pos)
    {
        long dpos = (long)(pos - sc->sec_pos);
        long dt = (long)(current_tick - sc->sec_tick) * (1000 / HZ);
        long lag = (long)(pos % 1000);

        UPDATE_TRACE("np sec %ld dpos=%ld lag=%ld dt=%ld", sec, dpos, lag,
                     dt);
        /* npfix: the wall time in the sim too (dt is replaced below for
         * the device threshold; the stall script checks rdt) */
        UPDATE_TRACE("np secdt %ld rdt=%ld", sec, dt);
#ifdef SIMULATOR
        dt = dpos;                  /* the sim's audio runs below real time */
#endif
        if (dpos < 700 || dpos > 1300 || dt < 700 || dt > 1300)
        {
            /* what else ran: a late step with the disk on points at a
             * buffer refill, with a boost held at other UI work */
            int boost = 0;
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
            boost = get_cpu_boost_counter();
#endif
            ui_log("np", "cadence sec=%ld dpos=%ld lag=%ld dt=%ld disk=%d "
                   "boost=%d", sec, dpos, lag, dt,
                   storage_disk_is_active() ? 1 : 0, boost);
        }
    }
    else if (!full && !sc->sec_tick && sc->shown_sec >= 0
             && sec == sc->shown_sec + 1 && m->st->mode == NP_NORMAL)
    {
        /* the step after a full row redraw (new art mid-track): untimed,
         * but the run of +1 steps goes on (the npstall rule) */
        UPDATE_TRACE("np secdt %ld rdt=-1", sec);
    }
    sc->sec_tick = full ? 0 : (current_tick ? current_tick : 1);
    sc->sec_pos = pos;
}

static void draw_labels(struct np_mem *m, unsigned long pos, bool full)
{
    struct np_scr *sc = m->sc;
    struct np_canvas cv = lcd_canvas();
    unsigned long len = bar_len(m);
    unsigned long rem = len > pos ? len - pos : 0;
    int x0 = NP_TIME_R_X - sc->rw - 2;
    int le = sc->bar_x - NP_TIME_GAP_L;     /* elapsed: right-aligned */
    char buf[12];
    int w;

    if (full)
        sc->lbl_l[0] = sc->lbl_r[0] = '\0';
    /* the atlas cells are opaque: clear only what the text leaves */
    np_fmt_time(buf, pos, false);
    if (strcmp(buf, sc->lbl_l))
    {
        w = np_atlas_width(&sc->atlas, buf);
        if (band_shown(m) && le - sc->lw - 2 < NP_REFL_BAND_X + NP_REFL_BAND_W
            && LABEL_Y < NP_ROW_Y + NP_REFL_BAND_H)
        {
            /* over the art's reflection (P11 glass floor): restore the
             * slot from the band, then the text on top of it */
            bg_fill(m, le - sc->lw - 2, LABEL_Y, sc->lw + 2, ATLAS_H);
            np_atlas_draw_mul(&cv, &sc->atlas, le - w, LABEL_Y, buf);
        }
        else
        {
            if (w != np_atlas_width(&sc->atlas, sc->lbl_l))
                np_fill(&cv, le - sc->lw - 2, LABEL_Y, sc->lw + 2 - w,
                        ATLAS_H, C_BG);
            np_atlas_update(&cv, &sc->atlas, le - w, LABEL_Y, sc->lbl_l,
                            buf);
        }
        strcpy(sc->lbl_l, buf);
    }
    np_fmt_time(buf, rem, true);
    if (!len)
    {
        /* length unknown: no remaining time (never "-0:00") */
        if (sc->lbl_r[0])
            np_fill(&cv, x0, LABEL_Y, NP_TIME_R_X - x0, ATLAS_H, C_BG);
        sc->lbl_r[0] = '\0';
    }
    else if (strcmp(buf, sc->lbl_r))
    {
        w = np_atlas_width(&sc->atlas, buf);
        if (w != np_atlas_width(&sc->atlas, sc->lbl_r))
            np_fill(&cv, x0, LABEL_Y, NP_TIME_R_X - w - x0, ATLAS_H, C_BG);
        np_atlas_update(&cv, &sc->atlas, NP_TIME_R_X - w, LABEL_Y,
                        sc->lbl_r, buf);
        strcpy(sc->lbl_r, buf);
    }
    sec_cadence(m, pos, full);
    sc->shown_sec = (long)(pos / 1000);
    sc->shown_rem = (long)(rem / 1000);
}

#define DIA_W 15                /* npglyphs NPG_DIAMOND */

/* The diamond from its two pre-composed tiles (over the track, over the
 * background beyond the bar's ends): a 15 x 15 copy, no blending. */
static void draw_diamond(struct np_mem *m, int x)
{
    struct np_scr *sc = m->sc;
    struct np_canvas cv = lcd_canvas();
    int r, c;

    for (r = 0; r < DIA_W; r++)
    {
        np_px *dst = cv.px + (BAR_Y - 1 + r) * cv.stride;
        for (c = 0; c < DIA_W; c++)
        {
            int xx = x + c;
            if (xx < 0 || xx >= LCD_WIDTH)
                continue;
            dst[xx] = (xx >= sc->bar_x && xx < sc->bar_x + sc->bar_w)
                      ? m->sty->dia_on[r * DIA_W + c]
                      : m->sty->dia_off[r * DIA_W + c];
        }
    }
    sc->dia_x = x;
}

static int diamond_x(struct np_mem *m, unsigned long pos)
{
    return m->sc->bar_x + np_bar_x(pos, bar_len(m), m->sc->bar_w)
           - DIA_W / 2;
}

/* Repaints the columns the diamond at x covered, except those the next
 * one (at nx; -1: none) will cover anyway */
static void undraw_diamond(struct np_mem *m, int x, int nx)
{
    struct np_scr *sc = m->sc;
    struct np_canvas cv = lcd_canvas();
    int c;

    for (c = x; c < x + DIA_W; c++)
    {
        if (nx >= 0 && c >= nx && c < nx + DIA_W)
            continue;
        np_fill(&cv, c, BAR_Y - 1, 1, 1 + NP_BAR_H + NP_BAR_REFL, C_BG);
        np_bar_cols(&cv, sc->bar_x, BAR_Y, sc->bar_w, c - sc->bar_x,
                    c - sc->bar_x + 1, 0, sc->ticks_n ? m->ticks : NULL,
                    &m->sty->bar);
    }
    sc->dia_x = -1;
}

/* The volume limit (P11 §3.5; ⚠️ look frozen by Q, no reference
 * capture): a small near-black triangle above the bar, pointing down at
 * the limit, when a limit below the maximum is set. Static while the bar
 * shows (the limit changes only in Settings), so only full paints draw
 * it; the bar's own deltas never reach rows above BAR_Y. */
#define VLIM_H 4                    /* rows BAR_Y-5 .. BAR_Y-2 */
static void draw_vlimit(const struct np_canvas *cv)
{
    int f = ui_vol_frac(global_settings.volume_limit), x, r;

    if (f >= 1024)
        return;
    x = VOL_X + f * (VOL_W - 1) / 1024;
    for (r = 0; r < VLIM_H; r++)
        np_fill(cv, x - (VLIM_H - 1 - r), BAR_Y - 1 - VLIM_H + r,
                2 * (VLIM_H - 1 - r) + 1, 1, C_SPEAKER);
}

static void draw_volume(struct np_mem *m, bool full)
{
    struct np_scr *sc = m->sc;
    struct np_canvas cv = lcd_canvas();
    int px = np_pb_volume_pct() * VOL_W / 100;
    int w, h;

    if (full)
    {
        /* the speakers have no reflection (spec), the bar has; over the
         * Visualizer (#126, dark) its background and light speakers */
        bool dark = m->st->mode == NP_VIZ;
        fb_data spk = dark ? C_SPEAKER_DARK : C_SPEAKER;

        if (dark)
            np_fill(&cv, 0, NP_ROW_Y, LCD_WIDTH, NP_ROW_H,
                    (fb_data)viz_bg());
        else
            bg_fill(m, 0, NP_ROW_Y, LCD_WIDTH, NP_ROW_H);
        np_glyph_size(NPG_SPEAKER, &w, &h);
        np_glyph_draw(&cv, NPG_SPEAKER, SPK_X, BAR_Y + (NP_BAR_H - h + 1) / 2,
                      NULL, spk, spk);
        np_glyph_size(NPG_SPEAKER_WAVES, &w, &h);
        np_glyph_draw(&cv, NPG_SPEAKER_WAVES, SPKW_X,
                      BAR_Y + (NP_BAR_H - h + 1) / 2, NULL, spk, spk);
        np_bar_cols(&cv, VOL_X, BAR_Y, VOL_W, 0, VOL_W, px, NULL,
                    &m->sty->bar);
        draw_vlimit(&cv);
    }
    else if (px != sc->vol_px)
        np_bar_cols(&cv, VOL_X, BAR_Y, VOL_W, MIN(px, sc->vol_px) - 1,
                    MAX(px, sc->vol_px) + 1, px, NULL, &m->sty->bar);
    sc->vol_px = px;
}

/* One cell of the rating row: a glossy star with its flat reflection
 * (a pre-composed tile copy, i < n) or a plain dot (no reflection) */
static void draw_rating_cell(struct np_mem *m, int i, int n, bool clear)
{
    struct np_canvas cv = lcd_canvas();
    int cx = LCD_WIDTH / 2 - 2 * RATE_PITCH + i * RATE_PITCH;
    int r;

    if (clear)
        bg_fill(m, cx - STAR_W / 2, RATE_Y, STAR_W, STAR_TILE_H);
    if (i < n)
    {
        /* over what is behind it (white, or the art's reflection) */
        const np_px *col = m->sty->star_col;
        const uint8_t *al = m->sty->star_a;
        int c;

        for (r = 0; r < STAR_TILE_H; r++)
        {
            np_px *dst = cv.px + (RATE_Y + r) * cv.stride + cx - STAR_W / 2;
            for (c = 0; c < STAR_W; c++, col++, al++)
                if (*al)
                    dst[c] = *al >= 32 ? *col
                                       : np_blend256(dst[c], *col, *al * 8u);
        }
    }
    else
        np_glyph_draw(&cv, NPG_DOT, cx - 2, RATE_Y + 8, NULL, C_DOT, C_DOT);
}

static void draw_rating_row(struct np_mem *m)
{
    int i;

    bg_fill(m, 0, NP_ROW_Y, LCD_WIDTH, NP_ROW_H);
    for (i = 0; i < NP_STARS_MAX; i++)
        draw_rating_cell(m, i, m->st->rating, false);
    m->sc->row_rating = m->st->rating;
}

/* A rating change: only the cells that flipped */
static void rating_delta(struct np_mem *m)
{
    struct np_scr *sc = m->sc;
    int n = m->st->rating, i;

    for (i = MIN(n, sc->row_rating); i < MAX(n, sc->row_rating); i++)
        draw_rating_cell(m, i, n, true);
    sc->row_rating = n;
}

/* The shelf (owner photo 1208): a sunken rounded channel; the three
 * options sit flat inside it in near-black; the selection is a raised
 * satin button with a soft drop shadow and blue text, no outline. The
 * channel never changes; the button moves. */
static void draw_shelf(struct np_mem *m)
{
    struct np_styles *s = m->sty;
    struct np_canvas cv = lcd_canvas();
    const char *opt[3];
    int tw[3], i, sum = 0, pad, x, w, h;
    int font = font_bold12();
    struct viewport vp;

    opt[0] = str(LANG_OFF);
    opt[1] = str(LANG_NP_SHUFFLE_SONGS);
    opt[2] = str(LANG_NP_SHUFFLE_ALBUMS);
    for (i = 0; i < 3; i++)
    {
        font_getstringsize((const unsigned char *)opt[i], &tw[i], NULL, font);
        sum += tw[i];
    }
    pad = MAX((SHELF_W - sum) / 6, 4);

    bg_fill(m, 0, NP_ROW_Y, LCD_WIDTH, NP_ROW_H);
    np_glyph_size(NPG_SHUFFLE_BIG, &w, &h);
    np_glyph_draw(&cv, NPG_SHUFFLE_BIG, SHUF_ICON_X, SHUF_ICON_Y, NULL,
                  C_GLYPH, C_GLYPH);
    np_rrect(&cv, SHELF_X, SHELF_Y, SHELF_W, SHELF_H, SHELF_R, s->shelf);

    x = SHELF_X;
    for (i = 0; i < 3; i++)
    {
        int cw = tw[i] + 2 * pad;
        bool sel = (int)m->st->shuffle == i;

        if (i == 2)
            cw = SHELF_X + SHELF_W - x;
        if (sel)
        {
            int bx = x + 1, bw = cw - 2, k;

            /* the soft shadow on the channel floor, then the button */
            for (k = 0; k < BTN_SHADOW; k++)
                np_blend_rect(&cv, bx + 1 + (k == 0), BTN_Y + BTN_H + k,
                              bw - 2 - 2 * (k == 0), 1,
                              LCD_RGBPACK(0, 0, 0), 64 - k * 15);
            np_blend_rect(&cv, bx + bw, BTN_Y + 2, 1, BTN_H - 1,
                          LCD_RGBPACK(0, 0, 0), 40);
            np_rrect(&cv, bx, BTN_Y, bw, BTN_H, BTN_R, s->btn);
        }
        text_vp(&vp, font, sel ? C_SEL_TXT : C_TITLE, x, BTN_Y, cw, BTN_H);
        lcd_set_viewport(&vp);
        lcd_putsxy((cw - tw[i]) / 2,
                   cell_y(font, SHELF_CAP_TOP, CAP_SMALL) - BTN_Y,
                   (const unsigned char *)opt[i]);
        lcd_set_viewport(NULL);
        x += cw;
    }
}

/* Normal / Scrubber: labels, the bar, the fill or the diamond */
static void draw_progress(struct np_mem *m)
{
    struct np_scr *sc = m->sc;
    struct np_canvas cv = lcd_canvas();
    unsigned long pos = np_shown_pos(m->st, elapsed_now(m));
    bool scrub = m->st->mode == NP_SCRUBBER;

    bg_fill(m, 0, NP_ROW_Y, LCD_WIDTH, NP_ROW_H);
    sc->fill_px = scrub ? 0 : np_bar_x(pos, bar_len(m), sc->bar_w);
    np_bar_cols(&cv, sc->bar_x, BAR_Y, sc->bar_w, 0, sc->bar_w, sc->fill_px,
                sc->ticks_n ? m->ticks : NULL, &m->sty->bar);
    sc->dia_x = -1;
    if (scrub && bar_len(m))        /* no marker while the length is
                                       unknown: the scrubber is idle */
        draw_diamond(m, diamond_x(m, pos));
    draw_labels(m, pos, true);
}

/* The whole row for the current mode */
static void draw_row(struct np_mem *m)
{
    struct np_scr *sc = m->sc;

    /* the Visualizer (#126, full screen): no bar or counters; only the
     * volume overlay, over the bars' foot (NP_ROW_Y to the panel's
     * bottom), while it shows */
    enum np_mode rm = m->st->mode == NP_VIZ ? NP_NORMAL : m->st->mode;

    if (m->st->mode == NP_VIZ)
    {
        sc->row_mode = NP_NORMAL;
        sc->row_volume = m->st->volume_shown;
        if (sc->row_volume)
        {
            struct np_canvas cv = lcd_canvas();

            viz_clip(NP_ROW_Y);     /* the foot is the CPU's first */
            np_fill(&cv, 0, NP_ROW_Y + NP_ROW_H, LCD_WIDTH,
                    LCD_HEIGHT - NP_ROW_Y - NP_ROW_H, (fb_data)viz_bg());
            draw_volume(m, true);
        }
        return;
    }
    sc->row_mode = rm;
    sc->row_volume = rm == NP_NORMAL && m->st->volume_shown;
    switch (rm)
    {
    case NP_NORMAL:
        if (sc->row_volume)
            draw_volume(m, true);
        else
            draw_progress(m);
        break;
    case NP_SCRUBBER:
        draw_progress(m);
        break;
    case NP_RATING:
        draw_rating_row(m);
        break;
    case NP_SHUFFLE:
        draw_shelf(m);
        break;
    case NP_LYRICS:
    case NP_VIZ:
    default:
        bg_fill(m, 0, NP_ROW_Y, LCD_WIDTH, NP_ROW_H);
        break;
    }
}

/* 1 Hz and scrub deltas: only what moved */
static bool row_time(struct np_mem *m, bool force)
{
    struct np_scr *sc = m->sc;
    struct np_canvas cv;
    unsigned long pos, len = bar_len(m);
    bool scrub = m->st->mode == NP_SCRUBBER;
    bool changed = false;
    int px;

    if (sc->row_volume || (sc->row_mode != NP_NORMAL && !scrub)
        || sc->row_mode != m->st->mode)
        return false;
    pos = np_shown_pos(m->st, elapsed_now(m));
    if (force || (long)(pos / 1000) != sc->shown_sec
        || (long)((len > pos ? len - pos : 0) / 1000) != sc->shown_rem)
    {
        draw_labels(m, pos, force);
        changed = true;
    }
    cv = lcd_canvas();
    if (scrub && !len)
    {
        if (sc->dia_x >= 0)
        {
            undraw_diamond(m, sc->dia_x, -1);
            changed = true;
        }
    }
    else if (scrub)
    {
        px = diamond_x(m, pos);
        if (px != sc->dia_x)
        {
            if (sc->dia_x >= 0)
                undraw_diamond(m, sc->dia_x, px);
            if (sc->fill_px)
            {
                np_bar_cols(&cv, sc->bar_x, BAR_Y, sc->bar_w, 0,
                            sc->fill_px, 0, sc->ticks_n ? m->ticks : NULL,
                            &m->sty->bar);
                sc->fill_px = 0;
            }
            draw_diamond(m, px);
            changed = true;
        }
    }
    else
    {
        if (sc->dia_x >= 0)
        {
            undraw_diamond(m, sc->dia_x, -1);
            changed = true;
        }
        px = np_bar_x(pos, len, sc->bar_w);
        if (px != sc->fill_px)
        {
            np_bar_cols(&cv, sc->bar_x, BAR_Y, sc->bar_w,
                        MIN(px, sc->fill_px), MAX(px, sc->fill_px), px,
                        sc->ticks_n ? m->ticks : NULL, &m->sty->bar);
            sc->fill_px = px;
            changed = true;
        }
    }
    return changed;
}

/* ---- lyrics ------------------------------------------------------------- */

/* Screen y of the first lyric line's cell (cap top LYR_CAP0) */
static int lyr_top(void)
{
    return cell_y(ui_theme->font_small, LYR_CAP0, CAP_SMALL);
}

static int lyr_advance(void *ctx, unsigned cp)
{
    return font_get_width((struct font *)ctx, cp);
}

/* Reads and wraps the lyrics into the strip. False: none. */
static bool lyrics_load(struct np_mem *m)
{
    struct np_scr *sc = m->sc;
    struct font *pf = font_get(ui_theme->font_small);
    struct np_lyr_font lf;
    bool kept, trunc = false;
    char *strip = ui_strip_claim(UI_STRIP_OWNER_NP, &kept);
    int len;

    if (!kept)
        sc->mq_ok = false;
    sc->lyr_ok = false;
    sc->lyr_lines = 0;
    sc->lyr_lh = LYR_PITCH;
    sc->lyr_vis = (LCD_HEIGHT - lyr_top()) / LYR_PITCH;
    if (!strip)
        return false;
    len = np_pb_read_lyrics(m->t->seq, strip + NP_STRIP_LYRICS_OFF,
                            NP_LYRICS_MAX, &trunc);
    if (trunc)
        logf("np: lyrics cut at %d B", NP_LYRICS_MAX);
    if (len >= 0)
    {
        lf.advance = lyr_advance;
        lf.ctx = pf;
        lf.width = LCD_WIDTH - 2 * LYR_X;
        boost(true);
        sc->lyr_lines = np_lyrics_wrap(strip + NP_STRIP_LYRICS_OFF, len, &lf,
                            (uint16_t *)(strip + NP_STRIP_LINES_OFF),
                            NP_LYRICS_LINES_MAX);
        boost(false);
        sc->lyr_len = len;
        sc->lyr_ok = true;
    }
    ui_strip_put();
    np_set_lyrics(m->st, len < 0 ? -1
                  : np_lyrics_max_top(sc->lyr_lines, sc->lyr_vis));
    return len >= 0;
}

/* A viewport over the lyrics body: on the LCD (fb NULL, origin 0, BODY_Y)
 * or over the pre-rendered page in the strip (320 x BODY_H) */
static void lyr_vp(struct viewport *vp, int font, unsigned fg,
                   struct frame_buffer_t *fb)
{
    text_vp(vp, font, fg, 0, fb ? 0 : BODY_Y, LCD_WIDTH, BODY_H);
    vp->buffer = fb;
}

/* Draws lyric lines [first, last) (rows of the visible page) in body
 * coordinates, each row cleared first; the scrollbar column is left. */
static void lyrics_lines(struct np_mem *m, struct frame_buffer_t *fb,
                         int first, int last)
{
    struct np_scr *sc = m->sc;
    struct viewport vp;
    bool kept;
    char *strip;
    int i, top = lyr_top() - BODY_Y;

    lyr_vp(&vp, ui_theme->font_small, C_LYRICS, fb);
    vp.width = LYR_SB_X - 1;
    lcd_set_viewport(&vp);
    lcd_set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    for (i = first; i < last; i++)
        lcd_fillrect(0, top + i * sc->lyr_lh, vp.width, sc->lyr_lh);
    lcd_set_drawmode(DRMODE_FG);
    strip = sc->lyr_ok ? ui_strip_claim(UI_STRIP_OWNER_NP, &kept) : NULL;
    if (strip && !kept)
    {
        sc->lyr_ok = false;
        sc->mq_ok = false;
    }
    else if (strip)
    {
        const char *text = strip + NP_STRIP_LYRICS_OFF;
        const uint16_t *starts = (const uint16_t *)(strip
                                                    + NP_STRIP_LINES_OFF);
        for (i = first; i < last; i++)
        {
            char buf[160];
            int b, e, li = m->st->lyrics_top + i;

            np_lyrics_line(text, sc->lyr_len, starts, sc->lyr_lines, li,
                           &b, &e);
            if (e - b > (int)sizeof(buf) - 1)
                e = b + sizeof(buf) - 1;
            memcpy(buf, text + b, e - b);
            buf[e - b] = '\0';
            if (buf[0])
                lcd_putsxy(LYR_X, top + i * sc->lyr_lh,
                           (const unsigned char *)buf);
        }
    }
    if (strip)
        ui_strip_put();
    lcd_set_viewport(NULL);
}

/* The scrollbar on the right edge, in the lists' style (list.c: a 1 px
 * edged track with rounded ends, the glossy grey thumb of
 * ui_theme->sbar_gloss): thumb = visible lines over all lines. */
#define LYR_SB_TRACK    LCD_RGBPACK(0xe4, 0xe4, 0xe4)   /* = list.c */
#define LYR_SB_EDGE     LCD_RGBPACK(0xb4, 0xb4, 0xb4)   /* = list.c */
static void lyrics_sbar(struct np_mem *m, struct frame_buffer_t *fb)
{
    struct np_scr *sc = m->sc;
    const struct ui_gloss *g = ui_theme->sbar_gloss;
    struct viewport vp;
    int x = LYR_SB_X, y = lyr_top() - BODY_Y, w = LYR_SB_W;
    int h = sc->lyr_vis * sc->lyr_lh, th, ty, i, max;

    lyr_vp(&vp, ui_theme->font_small, C_LYRICS, fb);
    lcd_set_viewport(&vp);
    lcd_set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    lcd_fillrect(x - 1, y, LCD_WIDTH - x + 1, h);
    lcd_set_drawmode(DRMODE_SOLID);
    if (sc->lyr_lines > sc->lyr_vis && h > 4)
    {
        max = sc->lyr_lines - sc->lyr_vis;
        th = MAX(h * sc->lyr_vis / sc->lyr_lines, 12);
        ty = (h - th) * MIN(m->st->lyrics_top, max) / max;
        lcd_set_foreground(LYR_SB_EDGE);
        lcd_vline(x, y + 1, y + h - 2);
        lcd_vline(x + w - 1, y + 1, y + h - 2);
        lcd_hline(x + 1, x + w - 2, y);
        lcd_hline(x + 1, x + w - 2, y + h - 1);
        lcd_set_foreground(LYR_SB_TRACK);
        lcd_fillrect(x + 1, y + 1, w - 2, h - 2);
        y += ty;
        ui_paint_gloss(x, y + 1, w, th - 2, g);
        for (i = 1; i < w - 1; i++)
        {
            lcd_set_foreground(ui_gloss_color(g, i, w));
            lcd_drawpixel(x + i, y);
            lcd_drawpixel(x + i, y + th - 1);
        }
    }
    lcd_set_viewport(NULL);
}

/* The whole page: the song title centred at the top (Bold 12), the
 * visible lines, the scrollbar (owner, 2026-10-08) */
static void lyrics_render(struct np_mem *m, struct frame_buffer_t *fb)
{
    struct viewport vp;
    int font = font_bold12(), tw = 0;

    lyr_vp(&vp, font, C_TITLE, fb);
    lcd_set_viewport(&vp);
    lcd_set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    lcd_fillrect(0, 0, LCD_WIDTH, BODY_H);
    lcd_set_drawmode(DRMODE_FG);
    font_getstringsize((const unsigned char *)m->t->title, &tw, NULL, font);
    if (tw > LCD_WIDTH - 2 * LYR_X)
        put_fit(LYR_X, cell_y(font, LYR_TITLE_CAP, CAP_SMALL) - BODY_Y,
                LCD_WIDTH - 2 * LYR_X, m->t->title, font);
    else
        lcd_putsxy((LCD_WIDTH - tw) / 2,
                   cell_y(font, LYR_TITLE_CAP, CAP_SMALL) - BODY_Y,
                   (const unsigned char *)m->t->title);
    lcd_set_viewport(NULL);
    lyrics_lines(m, fb, 0, m->sc->lyr_vis);
    lyrics_sbar(m, fb);
}

static void draw_lyrics_page(struct np_mem *m)
{
    lyrics_render(m, NULL);
    m->sc->lyr_drawn = m->st->lyrics_top;
    m->sc->row_mode = NP_LYRICS;
    m->sc->row_volume = false;
}

/* One-line scroll: move the lines (not the title), draw the line that
 * appears and the scrollbar */
static void lyrics_scroll(struct np_mem *m)
{
    struct np_scr *sc = m->sc;
    int d = m->st->lyrics_top - sc->lyr_drawn;
    int lh = sc->lyr_lh, n = sc->lyr_vis, y;
    int top = lyr_top(), bot = top + n * lh;

    if (d == 0)
        return;
    if (d != 1 && d != -1)
    {
        draw_lyrics_page(m);
        return;
    }
    lcd_set_viewport(NULL);
    if (d > 0)
        for (y = top; y < bot - lh; y++)
            memcpy(FBADDR(0, y), FBADDR(0, y + lh),
                   (LYR_SB_X - 1) * sizeof(fb_data));
    else
        for (y = bot - 1; y >= top + lh; y--)
            memcpy(FBADDR(0, y), FBADDR(0, y - lh),
                   (LYR_SB_X - 1) * sizeof(fb_data));
    sc->lyr_drawn = m->st->lyrics_top;
    if (d > 0)
        lyrics_lines(m, NULL, n - 1, n);
    else
        lyrics_lines(m, NULL, 0, 1);
    lyrics_sbar(m, NULL);
}
/* ---- the body ----------------------------------------------------------- */

static void draw_body(struct np_mem *m)
{
    struct np_canvas cv = lcd_canvas();

    if (m->st->mode == NP_LYRICS)
    {
        if (!m->sc->lyr_ok)
            lyrics_load(m);
        draw_lyrics_page(m);
        return;
    }
    np_fill(&cv, 0, BODY_Y, LCD_WIDTH, BODY_H, C_BG);
    if (m->st->mode == NP_VIZ)
    {
        /* #126: the whole panel is the bars' (its frames: np_tick);
         * the volume overlay over their foot while it shows */
        viz_paint_bg();
        draw_row(m);
        return;
    }
    draw_art(m);
    draw_info(m);
    draw_row(m);
}

/* ---- the marquee (F2: the title rendered once into the strip) ---------- */

static void marquee_reset(struct np_mem *m)
{
    struct np_scr *sc = m->sc;
    int w = 0;

#ifdef HAVE_MQ_JOBS
    if (sc->mq_bcm)
    {
        mqj_stop("title");          /* a new title: drawn from rest */
        sc->mq_bcm = false;
    }
#endif

    font_getstringsize((const unsigned char *)m->t->title, &w, NULL,
                       ui_theme->font_ui);
    sc->title_w = w;
    sc->mq_x = 0;
    sc->mq_ok = false;
    sc->mq_next = current_tick + MQ_DELAY;
}

static void marquee_render(struct np_mem *m, fb_data *strip)
{
    struct np_scr *sc = m->sc;
    struct frame_buffer_t fb;
    struct viewport vp;
    int font = ui_theme->font_ui;
    int w = MIN(sc->title_w, MQ_CAP);
    int y = cell_y(font, TITLE_CAP_TOP, CAP_UI) - TITLE_BAND_Y;

    memset(&fb, 0, sizeof(fb));
    fb.fb_ptr = strip + NP_STRIP_MARQUEE_OFF / sizeof(fb_data);
    fb.stride = STRIDE_MAIN(NP_MARQUEE_W, NP_MARQUEE_H);
    fb.elems = LCD_NBELEMS(NP_MARQUEE_W, NP_MARQUEE_H);
    text_vp(&vp, font, C_TITLE, 0, 0, NP_MARQUEE_W, NP_MARQUEE_H);
    vp.buffer = &fb;
    boost(true);
    lcd_set_viewport(&vp);
    lcd_set_drawmode(DRMODE_SOLID | DRMODE_INVERSEVID);
    lcd_fillrect(0, 0, NP_MARQUEE_W, NP_MARQUEE_H);
    lcd_set_drawmode(DRMODE_FG);
    /* past the cap the text is cut where the gap starts */
    vp.width = w;
    lcd_set_viewport(&vp);
    lcd_putsxy(0, y, (const unsigned char *)m->t->title);
    vp.x = w + MQ_GAP;
    vp.width = MQ_WIN;
    lcd_set_viewport(&vp);
    lcd_putsxy(-(vp.x - w - MQ_GAP), y, (const unsigned char *)m->t->title);
    lcd_set_viewport(NULL);
    boost(false);
    sc->mq_ok = true;
    UPDATE_TRACE("np mq render w=%d", w);
}

/* The window at sc->mq_x from the strip into the framebuffer (no push) */
static void marquee_show(struct np_mem *m)
{
    struct np_scr *sc = m->sc;
    fb_data *strip;
    bool kept;

    strip = ui_strip_claim(UI_STRIP_OWNER_NP, &kept);
    if (!strip)
    {
        draw_title_plain(m);
        return;
    }
    if (!kept)
    {
        sc->mq_ok = false;
        sc->lyr_ok = false;
    }
    if (!sc->mq_ok)
        marquee_render(m, strip);
    lcd_set_viewport(NULL);
    lcd_bitmap_part(strip + NP_STRIP_MARQUEE_OFF / sizeof(fb_data),
                    sc->mq_x, 0, STRIDE_MAIN(NP_MARQUEE_W, NP_MARQUEE_H),
                    TXT_X, TITLE_BAND_Y, MQ_WIN, NP_MARQUEE_H);
    ui_strip_put();
}

#ifdef HAVE_MQ_JOBS
/* ---- the marquee on the BCM (P19 19-5; jobs.md section 5) ---------------
 * At F2's first step the strip goes to a BCM slot and one stepped PAN
 * draws F2's frames from then on: no wakeups here.  Whatever redraws the
 * title's band, a full draw, a slide of the body, dark, a new title or
 * leaving ends it first (ANIM_STOP, FENCE, wait: mqj_stop()); F2 goes on
 * from the PAN's phase, the frame the glass shows. */

static const char *mq_why;          /* the last "cpu" reason traced */

/* F2 from a phase (-1: from rest, the full delay) */
static void mq_from_phase(struct np_scr *sc, long u)
{
    int wait_ms = 0;

    sc->mq_x = u < 0 ? 0 : mqj_phase_x(u, &wait_ms);
    sc->mq_next = current_tick + (u < 0 ? MQ_DELAY
                                        : (wait_ms * HZ + 999) / 1000);
}

/* The PAN ends: F2 owns the title again, at the phase (no drawing: the
 * caller draws or pushes what it was going to) */
static void mq_bcm_end(struct np_mem *m, const char *why)
{
    struct np_scr *sc = m->sc;
    long u;

    if (!sc->mq_bcm)
        return;
    u = mqj_stop(why);
    if (u < 0)
        u = mqj_phase();            /* ended already (an epoch) */
    sc->mq_bcm = false;
    mq_from_phase(sc, u);
}

/* ui_fb_settle() met the window (a slide, a takeover): the glass's frame
 * into the framebuffer, so a capture is what the user saw */
static void mq_settle(long phase)
{
    struct np_mem m;

    if (!mem_get(&m))
        return;
    if (m.sc->mq_bcm)
    {
        m.sc->mq_bcm = false;
        mq_from_phase(m.sc, phase);
        marquee_show(&m);
    }
    mem_put();
}

/* The strip's content key: the title, its width, the font (the colours
 * are fixed: C_TITLE on the body) */
static uint64_t mq_key(struct np_mem *m)
{
    return (uint64_t)artkey_fnv32(m->t->title) << 32
           | (uint32_t)(m->sc->title_w & 0xFFFF) << 16
           | (uint32_t)(ui_theme->font_ui & 0xFF) << 8 | 0x4D;
}

/* At F2's first step (mq_x 0, due): the strip up and the PAN sent with
 * its first frame step 1, as F2's.  false: F2 takes this step */
static bool mq_bcm_begin(struct np_mem *m, int lap)
{
    struct np_scr *sc = m->sc;
    const char *why;
    fb_data *strip;
    bool kept, ok;

    if (!mqj_available(&why))
    {
        if (why != mq_why)
            UPDATE_TRACE("np mq cpu why=%s", why);
        mq_why = why;
        return false;
    }
    mq_why = NULL;
    strip = ui_strip_claim(UI_STRIP_OWNER_NP, &kept);
    if (!strip)
        return false;
    if (!kept)
    {
        sc->mq_ok = false;
        sc->lyr_ok = false;
    }
    if (!sc->mq_ok)
        marquee_render(m, strip);
    /* pinned while claimed: the upload yields */
    ok = mqj_start((const uint16_t *)strip
                   + NP_STRIP_MARQUEE_OFF / sizeof(fb_data),
                   NP_MARQUEE_W, NP_MARQUEE_H, mq_key(m), lap, MQ_WIN,
                   TXT_X, TITLE_BAND_Y, MQJ_PAUSE_MS, mq_settle);
    ui_strip_put();
    sc->mq_bcm = ok;
    return ok;
}

#ifdef HAVE_BCM_JOBS_MODEL
/* The simulator's parity (bcm_drift parity): F2 draws and pushes each
 * step too, at the PAN's own phase, a tick past the step; the model's
 * panel must equal it in the window */
static int mq_parity(struct np_mem *m)
{
    struct np_scr *sc = m->sc;
    int wait_ms = 0;
    unsigned long d;
    long u = mqj_phase();

    sc->mq_x = mqj_phase_x(u, &wait_ms);
    if (u >= MQJ_PAUSE_MS && wait_ms > MQJ_STEP_MS - 1000 / HZ)
        return 1;                   /* a late pass on a step's edge: the
                                       model's frame for it may follow in
                                       the same tick; look a tick later */
    marquee_show(m);
    ui_lcd_push("np:title", 0, TITLE_BAND_Y, LCD_WIDTH, TITLE_BAND_H);
    d = bcmjob_model_diff(TXT_X, TITLE_BAND_Y, MQ_WIN, NP_MARQUEE_H);
    UPDATE_TRACE("np mq parity x=%d diff=%lu", sc->mq_x, d);
    return (wait_ms * HZ + 999) / 1000 + 1;
}
#endif
#endif /* HAVE_MQ_JOBS */

/* One step: blit the window. Returns ticks until the next step. */
static int marquee_step(struct np_mem *m, long now)
{
    struct np_scr *sc = m->sc;
    int lap = MIN(sc->title_w, MQ_CAP) + MQ_GAP;

    if (sc->title_w <= MQ_WIN || m->st->mode == NP_LYRICS)
    {
#ifdef HAVE_MQ_JOBS
        mq_bcm_end(m, "mode");
#endif
        return TIMEOUT_BLOCK;
    }
#ifdef HAVE_MQ_JOBS
    if (sc->mq_bcm)
    {
        if (mqj_running())
        {
#ifdef HAVE_BCM_JOBS_MODEL
            if (bcmjob_drift_mode() == BCMJOB_DRIFT_PARITY)
                return mq_parity(m);
#endif
            return TIMEOUT_BLOCK;   /* the BCM steps it: no wakeups */
        }
        /* gone with an epoch (the restart showed the framebuffer, the
         * title at rest): F2 from the phase, drawn now */
        mq_bcm_end(m, "epoch");
        marquee_show(m);
        ui_lcd_push("np:title", 0, TITLE_BAND_Y, LCD_WIDTH, TITLE_BAND_H);
        return MAX(sc->mq_next - now, 1);
    }
#endif
    if (TIME_BEFORE(now, sc->mq_next))
        return sc->mq_next - now;
#ifdef HAVE_MQ_JOBS
    if (sc->mq_x == 0 && mq_bcm_begin(m, lap))
    {
#ifdef HAVE_BCM_JOBS_MODEL
        if (bcmjob_drift_mode() == BCMJOB_DRIFT_PARITY)
            return mq_parity(m);
#endif
        return TIMEOUT_BLOCK;
    }
#endif
    sc->mq_x += MQ_STEP_PX;
    if (sc->mq_x >= lap)
    {
        sc->mq_x = 0;
        sc->mq_next = now + MQ_PAUSE;
    }
    else
        sc->mq_next = now + MQ_STEP_TICKS;

    marquee_show(m);
    ui_lcd_push("np:title", 0, TITLE_BAND_Y, LCD_WIDTH, TITLE_BAND_H);
    return sc->mq_next - now;
}

/* ---- slides -------------------------------------------------------------- */

static void rows_copy(fb_data *dst, int dst_stride, const fb_data *src,
                      int src_stride, int h, int w)
{
    int y;
    for (y = 0; y < h; y++)
        memcpy(dst + y * dst_stride, src + y * src_stride,
               w * sizeof(fb_data));
}

static int eased(long el, int total)
{
    long inv, rest;

    if (el >= SLIDE_TICKS)
        return total;
    /* cubic ease-out, 1/1024 units */
    inv = 1024 - el * 1024 / SLIDE_TICKS;
    rest = inv * inv / 1024 * inv / 1024;
    return total - (int)(total * rest / 1024);
}

/* Starts the slide asked for: captures what is on screen, draws the new
 * state, and leaves the old picture in the framebuffer for frame 0. */
static void slide_begin(struct np_mem *m, enum slide_kind k)
{
    struct np_scr *sc = m->sc;
    fb_data *strip, *fb0;
    bool kept;
    int stride;

#ifdef HAVE_MQ_JOBS
    if (k != SL_ROW && sc->mq_bcm)
    {
        mq_bcm_end(m, "slide");     /* the capture: the glass's frame */
        marquee_show(m);
    }
#endif

    strip = ui_strip_claim(UI_STRIP_OWNER_NP, &kept);
    if (!kept)
    {
        sc->mq_ok = false;
        if (k != SL_LYR_IN)         /* lyrics were just read: keep them */
            sc->lyr_ok = false;
    }
    lcd_set_viewport(NULL);
    fb0 = FBADDR(0, 0);
    stride = (int)(FBADDR(0, 1) - fb0);
    sc->slide = SL_NONE;
    if (!strip)
    {
        /* no strip: no slide, just the new state */
        if (k == SL_ROW)
            draw_row(m);
        else
            draw_body(m);
        ui_lcd_push(k == SL_ROW ? "np:row" : "np:body", 0,
                    k == SL_ROW ? NP_ROW_Y : BODY_Y, LCD_WIDTH,
                    k == SL_ROW ? NP_ROW_H : BODY_H);
        return;
    }
    if (k == SL_ROW)
    {
        fb_data *old = strip + NP_STRIP_ROW_OLD_OFF / sizeof(fb_data);
        fb_data *nw = strip + NP_STRIP_ROW_NEW_OFF / sizeof(fb_data);
        fb_data *row = fb0 + NP_ROW_Y * stride;

        rows_copy(old, LCD_WIDTH, row, stride, NP_ROW_H, LCD_WIDTH);
        ui_strip_put();             /* drawing may yield (fonts) */
        draw_row(m);
        strip = ui_strip_claim(UI_STRIP_OWNER_NP, &kept);
        if (!strip || !kept)
        {
            if (strip)
                ui_strip_put();
            ui_lcd_push("np:row", 0, NP_ROW_Y, LCD_WIDTH, NP_ROW_H);
            return;
        }
        old = strip + NP_STRIP_ROW_OLD_OFF / sizeof(fb_data);
        nw = strip + NP_STRIP_ROW_NEW_OFF / sizeof(fb_data);
        rows_copy(nw, LCD_WIDTH, row, stride, NP_ROW_H, LCD_WIDTH);
        rows_copy(row, stride, old, LCD_WIDTH, NP_ROW_H, LCD_WIDTH);
    }
    else if (k == SL_BODY_OUT)
    {
        /* the new body goes to the strip (it overwrites the marquee,
         * the row captures and the lyrics: all rebuilt later) */
        fb_data *cap = strip;
        fb_data *body = fb0 + BODY_Y * stride;
        int y, x;

        rows_copy(cap, LCD_WIDTH, body, stride, BODY_H, LCD_WIDTH);
        sc->mq_ok = false;
        sc->lyr_ok = false;
        ui_strip_put();
        draw_body(m);
        strip = ui_strip_claim(UI_STRIP_OWNER_NP, &kept);
        if (!strip || !kept)
        {
            if (strip)
                ui_strip_put();
            ui_lcd_push("np:body", 0, BODY_Y, LCD_WIDTH, BODY_H);
            return;
        }
        for (y = 0; y < BODY_H; y++)
        {
            fb_data *a = body + y * stride, *b = strip + y * LCD_WIDTH;
            for (x = 0; x < LCD_WIDTH; x++)
            {
                fb_data p = a[x];
                a[x] = b[x];
                b[x] = p;
            }
        }
    }
    if (k == SL_LYR_IN)
    {
        /* the page is rendered once into the strip (NP_STRIP_PAGE_OFF,
         * clear of the lyrics text), so every frame is the same two row
         * copies as the body push out: no text rendering per frame */
        struct frame_buffer_t fb;

        memset(&fb, 0, sizeof(fb));
        fb.fb_ptr = strip + NP_STRIP_PAGE_OFF / sizeof(fb_data);
        fb.stride = STRIDE_MAIN(LCD_WIDTH, BODY_H);
        fb.elems = LCD_NBELEMS(LCD_WIDTH, BODY_H);
        sc->mq_ok = false;          /* the page overwrites the marquee */
        sc->lyr_drawn = m->st->lyrics_top;
        boost(true);
        lyrics_render(m, &fb);
        boost(false);
        sc->row_mode = NP_LYRICS;
        sc->row_volume = false;
    }
    ui_strip_put();
    sc->slide = k;
    sc->slide_start = current_tick - 1;     /* the first frame moves */
    sc->slide_done = 0;
}

/* One frame (or the last one with 'finish'). Returns true while running */
/* P11 (np-reflection.md §6): the art's reflection runs behind the bar
 * row, but the row captures hold it at their own columns, so a plain
 * shift would slide it along with the row. Per band row, a captured
 * pixel that is just background (the band pixel, or white, where it was
 * taken) shows the band at its screen column instead: the reflection
 * stays put and only the row's own drawing moves. ~140 compares a row. */
static void slide_keep_band(struct np_mem *m, fb_data *row, int y, int want)
{
    const np_px *band = m->band + y * NP_REFL_BAND_W;
    int x;

    for (x = NP_REFL_BAND_X; x < NP_REFL_BAND_X + NP_REFL_BAND_W; x++)
    {
        int sx = x < LCD_WIDTH - want ? x + want : x - (LCD_WIDTH - want);
        np_px under = sx >= NP_REFL_BAND_X
                      && sx < NP_REFL_BAND_X + NP_REFL_BAND_W
                      ? band[sx - NP_REFL_BAND_X] : C_BG;

        if (row[x] == under)
            row[x] = band[x - NP_REFL_BAND_X];
    }
}

static bool slide_frame(struct np_mem *m, bool finish)
{
    struct np_scr *sc = m->sc;
    long el = current_tick - sc->slide_start;
    int want, dx, y, y0, h, stride;
    fb_data *strip = NULL, *fb0;
    bool kept = true;

    if (!sc->slide)
        return false;
    if (!finish && !ui_anim_frame_due())
        return true;
    ui_anim_frame();
    want = finish ? LCD_WIDTH : eased(el, LCD_WIDTH);
    dx = want - sc->slide_done;
    if (dx <= 0)
        return true;
    lcd_set_viewport(NULL);
    fb0 = FBADDR(0, 0);
    stride = (int)(FBADDR(0, 1) - fb0);
    y0 = sc->slide == SL_ROW ? NP_ROW_Y : BODY_Y;
    h = sc->slide == SL_ROW ? NP_ROW_H : BODY_H;

    strip = ui_strip_claim(UI_STRIP_OWNER_NP, &kept);
    if (!strip || !kept)
    {
        /* lost the capture: show the new state as it is */
        if (strip)
            ui_strip_put();
        sc->dirty |= sc->slide == SL_ROW ? NPD_ROWFULL : NPD_BODY;
        sc->slide = SL_NONE;
        return false;
    }
    if (sc->slide == SL_ROW)
    {
        /* old row out left, new row in from the right */
        const fb_data *old = strip + NP_STRIP_ROW_OLD_OFF / sizeof(fb_data);
        const fb_data *nw = strip + NP_STRIP_ROW_NEW_OFF / sizeof(fb_data);
        for (y = 0; y < h; y++)
        {
            fb_data *row = fb0 + (y0 + y) * stride;
            memcpy(row, old + y * LCD_WIDTH + want,
                   (LCD_WIDTH - want) * sizeof(fb_data));
            memcpy(row + LCD_WIDTH - want, nw + y * LCD_WIDTH,
                   want * sizeof(fb_data));
            if (band_shown(m) && y < NP_REFL_BAND_H)
                slide_keep_band(m, row, y, want);
        }
    }
    else
    {
        for (y = 0; y < h; y++)
        {
            fb_data *row = fb0 + (y0 + y) * stride;
            /* the new body's columns [done, want) at the right edge:
             * the body capture (out) or the pre-rendered page (in) */
            memmove(row, row + dx, (LCD_WIDTH - dx) * sizeof(fb_data));
            memcpy(row + LCD_WIDTH - dx,
                   strip + NP_STRIP_PAGE_OFF / sizeof(fb_data)
                   + y * LCD_WIDTH + sc->slide_done, dx * sizeof(fb_data));
        }
    }
    if (strip)
        ui_strip_put();
    sc->slide_done = want;
    ui_lcd_push("anim:np", 0, y0, LCD_WIDTH, h);
    if (want >= LCD_WIDTH)
    {
        sc->slide = SL_NONE;
        return false;
    }
    return true;
}

/* ---- applying the state machine's results ------------------------------ */

static void request_mode_visual(struct np_mem *m, enum np_mode from,
                                enum np_mode to)
{
    struct np_scr *sc = m->sc;

    /* #126: the Visualizer in or out: the whole screen in full, no
     * slide (its rect is the visualizer's, not a captured page).  Full
     * screen (owner, 2026-10-25): no header while it shows; out, the
     * header and the page come back in one full redraw */
    if (to == NP_VIZ || sc->viz_full || viz_on())
    {
        struct ui_screen *s = ui_top();
        bool full = to == NP_VIZ;

        if (!full)
        {
            viz_stop();             /* the BCM's rect handed back */
            ui_fb_settle(0, 0, LCD_WIDTH, LCD_HEIGHT);
            /* the title comes back at rest, the full delay first (as
             * after dark): not from the offset it had when the view
             * covered it */
            sc->mq_x = 0;
            sc->mq_next = current_tick + MQ_DELAY;
        }
        viz_clip(VIZ_H);
        sc->viz_full = full;
        if (s && s->ops == &ui_nowplaying_screen)
            s->header_w = full ? UI_HEADER_NONE : 0;
        UPDATE_TRACE("np viz full=%d", (int)full);
        ui_invalidate(UI_DIRTY_ALL); /* header (or none) + page, one push */
        return;
    }
    if (to == NP_LYRICS)
    {
        sc->slide_req = SL_LYR_IN;
        sc->lyr_ok = false;         /* read on entry (one wake) */
    }
    else if (from == NP_LYRICS)
        sc->slide_req = SL_BODY_OUT;
    else if ((from == NP_NORMAL || from == NP_SCRUBBER)
             && (to == NP_NORMAL || to == NP_SCRUBBER))
        sc->dirty |= NPD_ROW;       /* the same bar: in place */
    else
        sc->slide_req = SL_ROW;
}

static void apply(struct np_mem *m, const struct np_result *r,
                  long rate_tc, int rate_gen, int32_t rate_bind)
{
    struct np_scr *sc = m->sc;
    enum np_mode from = sc->row_mode;

    if (r->volume_clicks)
    {
        np_pb_volume_step(r->volume_clicks);
        sc->dirty |= NPD_ROW;
    }
    if (r->seek)
    {
        if (!book_on(m))
            np_pb_seek(r->seek_ms, m->t->seq);
        else if (!np_pb_seek_book(r->seek_ms, m->t->track_id)
                 && r->seek_ms >= m->t->book.file_start_ms
                 && r->seek_ms - m->t->book.file_start_ms
                    <= m->t->length_ms)
            np_pb_seek(file_pos(m, r->seek_ms), m->t->seq); /* in file */
    }
    if (r->rating_commit)
    {
        if (np_pb_set_rating(rate_tc, rate_bind, sc->rate_path,
                             r->rating_value))
            np_rating_ovr_set(&sc->ovr, rate_tc, rate_gen, r->rating_value);
        sc->dirty |= NPD_STARS;
    }
    if (r->shuffle_commit)
    {
        np_pb_set_shuffle(r->shuffle_value);
        sc->dirty |= NPD_GLYPHS;
    }
    if (r->mode_changed)
        request_mode_visual(m, from, r->mode);
    else if (r->dirty & NP_DIRTY_ROW)
        sc->dirty |= NPD_ROW;
    /* np_state flags "info" only for the rating and the shuffle setting:
     * their own bands (finding 6), never the whole text block */
    if (r->dirty & NP_DIRTY_INFO)
        sc->dirty |= NPD_STARS;
}

static void np_feed(struct np_mem *m, const struct np_event *ev)
{
    struct np_result r;
    enum np_mode was = m->st->mode;

    if (book_on(m))         /* re-point the book's snap columns */
        np_set_scrub(m->st, scrub_ctx(m));
    r = np_event(m->st, ev);
    if (r.mode_changed)
    {
        static const char * const names[] = {
            "normal", "scrubber", "rating", "shuffle", "lyrics",
            "visualizer" };
        /* "np mode <from> -> <to> ev=<np_event_type>" (#58: SELECT
         * after a change returns to Normal; tests/ui) */
        UPDATE_TRACE("np mode %s -> %s ev=%d", names[was], names[r.mode],
                     (int)ev->type);
        (void)names;            /* the trace is compiled out on target */
    }
    (void)was;
    apply(m, &r, m->t->tc_idx, m->t->db_gen, m->t->bind);
}

/* ---- track changes ---------------------------------------------------- */

/* A seq move on the same track (Rev 3: track_id unchanged): playback
 * rewrote or moved the chapter table, or the length firmed up. Only the
 * scrub context is re-pointed; the mode, the marker and the art stay, and
 * the bar row / info are redrawn only if what they show changed. */
static void track_same(struct np_mem *m)
{
    struct np_scr *sc = m->sc;
    int bx = sc->bar_x, bw = sc->bar_w, tn = sc->ticks_n;
    uint8_t ticks[NP_BLK_TICKS_SIZE];

    memcpy(ticks, m->ticks, sizeof(ticks));
    np_set_scrub(m->st, scrub_ctx(m));
    layout_track(m);
    sc->chapter = -1;
    chapter_update(m, elapsed_now(m));
    if (bx != sc->bar_x || bw != sc->bar_w || tn != sc->ticks_n
        || memcmp(ticks, m->ticks, sizeof(ticks)))
        sc->dirty |= NPD_ROWFULL;
    /* the chapter line and counter: two small bands, so no compare */
    sc->dirty |= NPD_CHAPTER;
    logf("np: seq move, same track %lu", (unsigned long)m->t->track_id);
}

/* #58: the same track's database entry (index, stars) arrived after the
 * snapshot (np_pb_entry_moved()): the stars and the rating shown follow
 * it, unless the user is changing the rating right now (the page keeps
 * their value; its commit goes to the new entry). */
static void track_entry(struct np_mem *m)
{
    struct np_state *st = m->st;
    struct np_track *t = m->t;

    UPDATE_TRACE("np entry tc=%ld stars=%d", t->tc_idx, t->stars);
    if (st->mode == NP_RATING && st->touched)
        return;
    st->rating = np_rating_shown(&m->sc->ovr, t->tc_idx, t->db_gen,
                                 t->stars);
    st->rating_entry = st->rating;
    m->sc->dirty |= NPD_STARS | (st->mode == NP_RATING ? NPD_ROW : 0);
}

/* Takes a snapshot; on a new track drops to Normal and asks for the body.
 * Returns false when playback has stopped. */
static bool track_sync(struct np_mem *m)
{
    struct np_scr *sc = m->sc;
    struct np_track *t = m->t;
    long old_tc = t->tc_idx;
    int old_gen = t->db_gen;
    int32_t old_bind = t->bind;
    uint32_t old_id = t->track_id;
    bool old_ab = t->is_audiobook, old_pod = t->is_podcast;
    int old_stars = t->stars;
    bool was_book = sc->have_track && book_on(m);
    uint32_t old_len = t->book.len_ms;
    int old_entries = t->book.entries;
    struct np_result r;
    enum np_snap snap;

    snap = np_pb_snapshot(t);
    if (snap == NP_SNAP_NONE)
    {
        sc->stopped = true;
        return false;
    }
    if (snap == NP_SNAP_BUSY)
        return true;                /* again at the next tick */
    /* identity, not validity: track_id 0 is "unknown" (a real change) */
    if (sc->have_track && old_id && t->track_id == old_id)
    {
        if (t->is_audiobook == old_ab && t->is_podcast == old_pod)
        {
            track_same(m);
            if (t->tc_idx != old_tc || t->stars != old_stars)
                track_entry(m);
            return true;
        }
        /* #58: the database entry arrived and reclassified the track
         * (a podcast or a genre-tagged book): the cycle, the counter and
         * the glyphs change, so it is set up as a new track; what the
         * page had pending goes to the entry now known */
        old_tc = t->tc_idx;
        old_gen = t->db_gen;
        old_bind = t->bind;
    }
    if (was_book && book_on(m) && t->book.len_ms == old_len
        && t->book.entries == old_entries
        && (m->st->mode == NP_NORMAL || m->st->mode == NP_SCRUBBER))
    {
        /* the next file of the same book (a fast scrub or a chapter skip
         * across files, or plain playback): the book is one piece (owner
         * Rev 2), so Normal or the Scrubber and its marker (a book
         * position) carry on. Nothing is pending in these two modes but
         * a seek, which is book-wide too. ⚠️ owner round. */
        np_set_scrub(m->st, scrub_ctx(m));
    }
    else
    {
        r = np_set_track(m->st, t->is_audiobook, t->has_lyrics, 0, 0,
                         scrub_ctx(m), current_tick);
        if (sc->have_track)
            apply(m, &r, old_tc, old_gen, old_bind); /* old track's commits */
    }
    sc->have_track = true;
    /* the new track's path, for its own rating commits (B-Low) */
    np_pb_copy_path(sc->rate_path, sizeof(sc->rate_path), t->path_crc);
    m->st->rating = np_rating_shown(&sc->ovr, t->tc_idx, t->db_gen,
                                    t->stars);
    m->st->rating_entry = m->st->rating;
    sc->slide_req = SL_NONE;
    sc->row_mode = NP_NORMAL;
    sc->chapter = -1;
    sc->lyr_ok = false;
    layout_track(m);
    chapter_update(m, elapsed_now(m));
    sc->art_want = np_pb_art_key(t, t->is_audiobook ? sc->chapter : -1);
    marquee_reset(m);
    sc->dirty = NPD_BODY | NPD_ART;
    return true;
}

/* ---- screen ops ----------------------------------------------------------- */

static int np_enter(struct ui_screen *s, const void *arg)
{
    struct np_mem m;

    (void)arg;
    if (!np_reserve() || !mem_get(&m))
        return -1;                  /* logged; the screen below repaints */
    if (!m.sc->ready)
    {
        build_styles(m.sty);
        build_atlas(&m);
        m.sc->ready = true;
    }
    s->title = str(LANG_NOW_PLAYING);
#ifdef SIMULATOR
    sim_slow_pass = file_exists(ROCKBOX_DIR "/.sim_np_slowpass");
#endif
    np_init(m.st, HZ, np_pb_shuffle());
    np_set_viz(m.st, true);         /* #126: music offers the Visualizer */
    m.sc->have_track = false;
    m.sc->stopped = false;
    m.sc->viz_full = false;         /* a push starts in Normal, headed */
    m.sc->slide = m.sc->slide_req = SL_NONE;
    m.sc->lit = true;
    m.sc->wake_until = 0;
    m.t->seq = 1;                  /* odd: no snapshot yet */
    np_pop_posted = false;
    if (!track_sync(&m))
    {
        mem_put();
        return -1;                  /* nothing playing */
    }
    if (m.sc->have_track)
        art_load(&m, false);        /* one bundle read, before the slide */
    m.sc->dirty = 0;                /* draw() paints everything */
    mem_put();
    return 0;
}

static void np_leave_op(struct ui_screen *s)
{
    struct np_mem m;
    struct np_result r;

    (void)s;
    artcache_note_np(false);
    if (!mem_get(&m))
        return;
#ifdef HAVE_MQ_JOBS
    if (m.sc->mq_bcm)
    {
        mq_bcm_end(&m, "leave");
        marquee_show(&m);           /* a slide's capture: the glass's */
    }
#endif
    if (m.sc->slide)
        m.sc->slide = SL_NONE;
    if (m.sc->have_track && !m.sc->stopped)
    {
        r = np_leave(m.st);
        apply(&m, &r, m.t->tc_idx, m.t->db_gen, m.t->bind);
    }
    m.sc->slide_req = SL_NONE;
    m.sc->row_mode = NP_NORMAL;
    m.sc->dirty = 0;
    viz_forget();                   /* the arena resets after leave() */
    mem_put();
}

static void np_draw(struct ui_screen *s, unsigned dirty)
{
    struct np_mem m;
    unsigned long t0;

    (void)s;
    if (!(dirty & UI_DIRTY_BODY) || !mem_get(&m))
        return;
#ifdef HAVE_MQ_JOBS
    mq_bcm_end(&m, "draw");         /* the full draw takes the title */
#endif
    t0 = now_us();
    (void)t0;                       /* trace only */
    if (!m.sc->lit && is_backlight_on(true))
    {
        /* the first lit paint after dark (the face dismissed): this full
         * draw is the catch-up np_tick would do as bands; the title
         * parks for the full delay */
        m.sc->lit = true;
        m.sc->mq_x = 0;
        m.sc->mq_next = current_tick + MQ_DELAY;
    }
    /* #58 (device): no tick runs under the unlit face, so after a dark
     * stretch the snapshot is the track from before it. Snapshot again
     * first (RAM only, no I/O): this full paint, which lcd_awake() may
     * push as it is, shows what plays. A stop pops at the next tick. */
    if (m.sc->have_track && !m.sc->stopped
        && (np_pb_seq() != m.t->seq || np_pb_entry_moved(m.t)))
    {
        uint32_t was = m.t->track_id;

        track_sync(&m);
        if (m.t->track_id != was)   /* device evidence (ipodui.log) */
            ui_log("np", "wake resnap id=%lu->%lu lit=%d",
                   (unsigned long)was, (unsigned long)m.t->track_id,
                   (int)is_backlight_on(true));
    }
    if (!is_backlight_on(true))
    {
        /* the wake press reached us before the backlight thread lit the
         * screen: the tick polls briefly instead of sleeping as if dark */
        m.sc->wake_until = current_tick + HZ;
        if (!m.sc->wake_until)
            m.sc->wake_until = 1;
    }
    if (m.sc->have_track)
    {
        /* a new image while the disk sleeps: the placeholder now, the
         * read at the next tick (never a spin-up before this frame) */
        if (!m.sc->art_held || m.sc->art_key != m.sc->art_want)
            art_load(&m, true);
        draw_body(&m);
        /* "np draw id=<drawn> playing=<now> lit=<0|1> art=<ph|img>
         * defer=<0|1> us=<paint>": a full paint; after "unlit exit" the
         * first one must draw the playing track (check_trace.py npwake) */
        UPDATE_TRACE("np draw id=%lu playing=%lu lit=%d art=%s defer=%d "
                     "us=%lu", (unsigned long)m.t->track_id,
                     (unsigned long)np_pb_track(),
                     (int)is_backlight_on(true),
                     m.sc->art_ph ? "ph" : "img", (int)m.sc->art_defer,
                     now_us() - t0);
    }
    else
    {
        struct np_canvas cv = lcd_canvas();
        np_fill(&cv, 0, BODY_Y, LCD_WIDTH, BODY_H, C_BG);
    }
    m.sc->dirty = 0;
    mem_put();
}

static enum ui_result np_input(struct ui_screen *s, const struct ui_input *in)
{
    struct np_mem m;
    struct np_event ev;
    enum ui_result res = UI_IGNORED;

    (void)s;
    if (!mem_get(&m))
        return UI_IGNORED;
    if (m.sc->stopped)
    {
        mem_put();
        return UI_POP;
    }
    if (m.sc->slide && (in->key == UI_KEY_SELECT || in->key == UI_KEY_WHEEL
                        || in->key == UI_KEY_MENU))
        slide_frame(&m, true);      /* input wins: land the slide first */

    memset(&ev, 0, sizeof(ev));
    ev.tick = current_tick;
    ev.elapsed_ms = elapsed_now(&m);
    switch (in->key)
    {
    case UI_KEY_WHEEL:
        if (!m.sc->have_track)
            break;
        ev.type = NP_EV_WHEEL;
        ev.clicks = in->wheel_steps;
        /* the Scrubber takes physical clicks (one per wheel event) and
         * applies the speed itself (np_scrub_wheel(): 1 s, doubling
         * steps, or one chapter per click when fast); the list's
         * accelerated step count made a fast click jump several
         * chapters (fix round M3) */
        if (m.st->mode == NP_SCRUBBER && ev.clicks)
            ev.clicks = ev.clicks > 0 ? 1 : -1;
        ev.velocity = in->wheel_velocity;
        np_feed(&m, &ev);
        res = UI_HANDLED;
        break;
    case UI_KEY_SELECT:
        res = UI_HANDLED;
        if (in->long_press && !in->release && m.sc->have_track
            && m.st->mode == NP_VIZ)
        {
            /* #126: hold SELECT in the Visualizer: its style list.  The
             * rect goes back to the CPU first (the push captures the
             * framebuffer) */
            viz_stop();
            ui_push(&ui_viz_style_screen, NULL);
            break;
        }
        if (in->long_press && !in->release && m.sc->have_track)
        {
            /* hold SELECT: the hold menu (P11 §3.3; it drops the
             * release of this hold) */
            long tc = m.t->tc_idx;
            struct mp3entry *id3;

            /* the first snapshot can come before the track's database
             * lookup (the TRACK_BUFFER event sets tagcache_idx a tick
             * later) and stays until the next one: read it now, from
             * the same track and database, or Browse Album/Artist go */
            if (tc <= 0 && m.t->db_gen == ui_hold_db_gen()
                && np_pb_seq() == m.t->seq
                && (id3 = audio_current_track()) != NULL)
                tc = id3->tagcache_idx;
            ui_hold_open_np(tc, m.t->db_gen);
            break;
        }
        if (!in->release || in->long_press || !m.sc->have_track)
            break;
        ev.type = NP_EV_SELECT;
        np_feed(&m, &ev);
        break;
    case UI_KEY_MENU:
        if (in->release && !in->long_press)
            res = UI_POP;           /* leave() commits */
        break;
    case UI_KEY_PREV:
    case UI_KEY_NEXT:
        /* audiobooks: by chapter (spec); the rest is the global skip */
        if (in->release && !in->long_press && m.t->is_audiobook
            && chapters_ok(&m))
        {
            const struct np_chapters *ch = &m.t->scrub.ch;
            unsigned long pos = file_pos(&m, ev.elapsed_ms), target;
            int i = np_chapter_step(ch, pos, m.sc->chapter);

            if (in->key == UI_KEY_NEXT)
            {
                if (i + 1 >= ch->count)
                    break;          /* last chapter: next file/stop */
                target = np_chapter_start(ch, i + 1);
            }
            else if (i == 0 && pos <= 3000 && book_on(&m)
                     && m.t->queue_index > 0)
                break;              /* a book file's first seconds: the
                                       previous file (the global skip) */
            else if (pos - np_chapter_start(ch, i) > 3000 || i == 0)
                target = np_chapter_start(ch, i);
            else
                target = np_chapter_start(ch, i - 1);
            np_pb_seek(target, m.t->seq);
            res = UI_HANDLED;
        }
        break;
    default:
        break;
    }
    mem_put();
    return res;
}

/* Pushes what tick() drew, as full-width bands: the smallest band that
 * covers each part (finding 6: a rating or shuffle change pushes its own
 * 13 or 19 rows, not the 178-row info block). */
static void flush(struct np_mem *m, unsigned drew)
{
#ifdef HAVE_MQ_JOBS
    if (m->sc->mq_bcm && (drew & (NPD_BODY | NPD_INFO | NPD_ART | NPD_TITLE)))
    {
        /* a band over the title: the PAN stops first (its FENCE: a push
         * and the ring aren't ordered), and the band carries the phase's
         * frame, F2's from now on */
        mq_bcm_end(m, "band");
        marquee_show(m);
    }
#else
    (void)m;
#endif
    if (drew & NPD_BODY)
    {
        if (m->sc->viz_full)        /* #126: the page is the panel */
            ui_lcd_push("np:body", 0, 0, LCD_WIDTH, LCD_HEIGHT);
        else
            ui_lcd_push("np:body", 0, BODY_Y, LCD_WIDTH, BODY_H);
        return;
    }
    if (drew & (NPD_INFO | NPD_ART))
        ui_lcd_push("np:info", 0, BODY_Y, LCD_WIDTH, INFO_H);
    else
    {
        if (drew & NPD_GLYPHS)
            ui_lcd_push("np:glyphs", 0, GLYPH_BAND_Y, LCD_WIDTH,
                        GLYPH_BAND_H);
        if (drew & NPD_TITLE)
            ui_lcd_push("np:title", 0, TITLE_BAND_Y, LCD_WIDTH,
                        TITLE_BAND_H);
        if (drew & NPD_STARS)
            ui_lcd_push("np:stars", 0, STARS_BAND_Y, LCD_WIDTH,
                        STARS_BAND_H);
        if (drew & NPD_CHAPTER)
        {
            ui_lcd_push("np:chapter", 0, ALBUM_BAND_Y, LCD_WIDTH,
                        LINE_BAND_H);
            ui_lcd_push("np:counter", 0, COUNT_BAND_Y, LCD_WIDTH,
                        LINE_BAND_H);
        }
    }
    if (drew & NPD_ROW)
        ui_lcd_push("np:row", 0, NP_ROW_Y, LCD_WIDTH,
                    m->sc->viz_full ? LCD_HEIGHT - NP_ROW_Y : NP_ROW_H);
}

static int min_wait(int a, int b)
{
    if (a == TIMEOUT_BLOCK)
        return b;
    if (b == TIMEOUT_BLOCK)
        return a;
    return MIN(a, b);
}

static int np_tick(struct ui_screen *s)
{
    struct np_mem m;
    struct np_scr *sc;
    struct np_event ev;
    unsigned drew = 0;
    long now = current_tick;
    bool lit = is_backlight_on(true);
    int wait = TIMEOUT_BLOCK;
    unsigned long pos;

    (void)s;
    if (!mem_get(&m))
        return TIMEOUT_BLOCK;
    sc = m.sc;
    artcache_note_np(true);         /* review M3: the art park's rule */

    if (sc->stopped)
    {
        /* frozen default 9.15: pop. The main loop pops a requester
         * right after its tick, outside the input path (8a fix M2). */
        if (!np_pop_posted)
        {
            np_pop_posted = true;
            UPDATE_TRACE("np pop %s", "stopped");
            ui_pop_request(s);
        }
        mem_put();
        return TIMEOUT_BLOCK;
    }

    if (sc->slide)
    {
        if (slide_frame(&m, false))
        {
            mem_put();
            return UI_TICK_ANIM;
        }
    }

    /* a new track (or the first snapshot), or the track's database
     * entry arrived after the snapshot (#58) */
    if (!sc->have_track || np_pb_seq() != m.t->seq
        || np_pb_entry_moved(m.t))
    {
        if (!track_sync(&m))
        {
            mem_put();
            return 1;               /* the stop path above, next pass */
        }
        if (!sc->have_track)
        {
            mem_put();
            return STATE_POLL;      /* BUSY: try again */
        }
    }

    /* the state machine's timers: settle seek, mode timeout, volume */
    memset(&ev, 0, sizeof(ev));
    ev.type = NP_EV_TICK;
    ev.tick = now;
    ev.elapsed_ms = elapsed_now(&m);
    np_feed(&m, &ev);

    if (!lit)
    {
        /* nobody sees it: no drawing, no art reads, no marquee; the
         * next lit tick repaints what changed */
#ifdef HAVE_MQ_JOBS
        mq_bcm_end(&m, "dark");     /* parked at the light (below) */
#endif
        sc->lit = false;
        viz_stop();                 /* lit only: the tap closes */
        if (sc->dirty & NPD_ART)
            sc->dirty |= NPD_BODY;
        if (m.st->mode != NP_NORMAL || m.st->volume_shown
            || m.st->seek_pending || m.st->seek_hold)
            wait = STATE_POLL;
        if (sc->wake_until && TIME_BEFORE(now, sc->wake_until))
            wait = min_wait(wait, WAKE_POLL);   /* the light is coming */
        else
            sc->wake_until = 0;
        mem_put();
        return wait;
    }
    if (sc->wake_until)
    {
        /* painted before the light came: how long the backlight thread
         * took (ipodui.log when slow) */
        long waited = (long)(now - (sc->wake_until - HZ)) * (1000 / HZ);

        if (waited > 200)
            ui_log("np", "wake lit after ms=%ld", waited);
        sc->wake_until = 0;
    }
    if (!sc->lit)
    {
        sc->lit = true;
        sc->dirty |= NPD_ROW;       /* the clock moved while dark */
        sc->mq_x = 0;               /* parked: the full delay first */
        sc->mq_next = now + MQ_DELAY;
        sc->dirty |= NPD_TITLE;
    }

    if (sc->slide_req)
    {
        enum slide_kind k = sc->slide_req;
        sc->slide_req = SL_NONE;
        if (k == SL_LYR_IN && !sc->lyr_ok)
            lyrics_load(&m);        /* one open + <= 8 KB */
        slide_begin(&m, k);
        sc->dirty &= ~(k == SL_ROW ? NPD_ROW : (NPD_ROW | NPD_BODY
                                                | NPD_INFO | NPD_TITLE));
        if (sc->slide)
        {
            slide_frame(&m, false);
            mem_put();
            return UI_TICK_ANIM;
        }
    }

    /* audiobooks: the chapter at the playing position */
    pos = elapsed_now(&m);
    /* the length can arrive or change after the snapshot (Rev 3, known
     * item 8): new bar geometry, labels and marker range */
    if (np_pb_refresh_length(m.t))
    {
        np_set_scrub(m.st, scrub_ctx(&m));
        layout_track(&m);
        sc->dirty |= NPD_ROWFULL;
    }
    if (chapter_update(&m, pos))
    {
        artcache_key_t k = np_pb_art_key(m.t, sc->chapter);
        sc->dirty |= NPD_CHAPTER;
        if (k != sc->art_want)
        {
            sc->art_want = k;
            sc->dirty |= NPD_ART;
        }
    }
    /* art that was PENDING: once the builder committed something new */
    if (sc->art_pending && artcache_commit_seq() != sc->art_commit)
        sc->dirty |= NPD_ART;

    /* Every dirty bit is served in this pass (end review finding 2):
     * a new image does not swallow the info, title or row requests. */
    /* the art read deferred by the last pass (disk asleep) */
    if (sc->art_defer)
        sc->dirty |= NPD_ART;
    if (sc->dirty & (NPD_ART | NPD_BODY))
    {
        bool changed = art_load(&m, true);

        if (sc->dirty & NPD_BODY)
        {
            draw_body(&m);
            drew |= NPD_BODY;
        }
        else if (changed && m.st->mode != NP_LYRICS && m.st->mode != NP_VIZ)
        {
            /* the reflection reaches into the row band: the row is
             * composed again over the new band */
            draw_art(&m);
            drew |= NPD_ART;
            sc->dirty |= NPD_ROWFULL;
        }
    }
    if (drew & NPD_BODY)
        ;                           /* everything is on screen */
    else if (m.st->mode == NP_LYRICS)
    {
        if (sc->lyr_drawn != m.st->lyrics_top)
        {
            boost(true);
            lyrics_scroll(&m);
            boost(false);
            drew |= NPD_BODY;
        }
    }
    else
    {
        /* the Visualizer shows no art, text, bar or counter: only the
         * volume overlay over the bars' foot while it shows (#126) */
        bool viz = m.st->mode == NP_VIZ;
        enum np_mode rm = viz ? NP_NORMAL : m.st->mode;

        if (viz)
        {
            if (m.st->volume_shown)
            {
                if (!sc->row_volume || (sc->dirty & NPD_ROWFULL))
                {
                    draw_row(&m);       /* the overlay in, clip first */
                    drew |= NPD_ROW;
                }
                else if (sc->dirty & (NPD_ROW | NPD_ROWFULL))
                {
                    draw_volume(&m, false);
                    drew |= NPD_ROW;
                }
            }
            else
            {
                /* the overlay timed out: the bars take the foot back
                 * at their next frame */
                sc->row_volume = false;
                viz_clip(VIZ_H);
            }
        }
        else if (sc->dirty & NPD_INFO)
        {
            draw_info(&m);
            drew |= NPD_INFO;
        }
        else
        {
            if (sc->dirty & NPD_TITLE)
            {
                draw_title_plain(&m);
                drew |= NPD_TITLE;
            }
            if (sc->dirty & NPD_STARS)
            {
                draw_stars_small(shown_stars(&m));
                drew |= NPD_STARS;
            }
            if (sc->dirty & NPD_GLYPHS)
            {
                draw_state_glyphs(&m);
                drew |= NPD_GLYPHS;
            }
            if (sc->dirty & NPD_CHAPTER)
            {
                draw_chapter_lines(&m, true);
                drew |= NPD_CHAPTER;
            }
        }
        if (viz)
            ;
        else if (sc->dirty & NPD_ROWFULL)
        {
            draw_row(&m);
            drew |= NPD_ROW;
        }
        else if (sc->dirty & NPD_ROW)
        {
            bool vol = rm == NP_NORMAL && m.st->volume_shown;
            if (vol && sc->row_volume && sc->row_mode == NP_NORMAL)
                draw_volume(&m, false);
            else if (m.st->mode == NP_RATING && sc->row_mode == NP_RATING)
                rating_delta(&m);
            else if (rm == sc->row_mode && !vol && !sc->row_volume
                     && (rm == NP_NORMAL || rm == NP_SCRUBBER))
                row_time(&m, false);
            else if (m.st->mode == NP_SCRUBBER
                     && sc->row_mode == NP_NORMAL && !sc->row_volume)
            {
                /* Normal -> Scrubber in place: the fill goes, the
                 * diamond comes */
                sc->row_mode = NP_SCRUBBER;
                row_time(&m, true);
            }
            else if (m.st->mode == NP_NORMAL && sc->row_mode == NP_SCRUBBER)
            {
                sc->row_mode = NP_NORMAL;
                row_time(&m, true);
            }
            else
                draw_row(&m);
            drew |= NPD_ROW;
        }
        else if (row_time(&m, false))
            drew |= NPD_ROW;
    }
    sc->dirty = 0;
    flush(&m, drew);
    {
        /* npfix: a lit pass over 150 ms, with what it did (art reads,
         * which bands, the storage state) */
        long ms = (long)(current_tick - now) * (1000 / HZ);
        if (ms > 150)
            ui_log("np", "slow pass ms=%ld art=%d drew=%x disk=%d", ms,
                   (int)np_pass_art_reads, drew,
                   storage_disk_is_active() ? 1 : 0);
        np_pass_art_reads = 0;
    }
#ifdef SIMULATOR
    /* tests only (ipodui-p8b-np-tick): a pass as long as the device's,
     * whose LCD push over the bridge takes milliseconds while the PCM
     * callback moves the elapsed time on; the sim's pass is otherwise
     * atomic under its kernel lock */
    if (sim_slow_pass)
        sleep(1);
#endif

    /* when to come back */
    if (m.st->mode != NP_LYRICS
        && (audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
           == AUDIO_STATUS_PLAY)
    {
        unsigned long e = elapsed_now(&m);  /* a book: its seconds */
        int ms = 1000 - (int)(e % 1000) + 10;
        /* 8b (device log "np cadence ... lag=945 dt=1920"): the PCM
         * position moves on in ~93 ms steps while the pass runs, and the
         * wake is aimed right at the next second, so it often crosses it
         * between row_time()'s read and this one. Aiming a second ahead
         * from here would then leave the old second on screen for a
         * whole second: a label row_time() keeps live comes back at the
         * next tick instead (it draws then, so this never repeats). */
        if (m.st->mode == NP_NORMAL
            && sc->row_mode == NP_NORMAL && !sc->row_volume && !m.st->volume_shown
            && (long)(np_shown_pos(m.st, e) / 1000) != sc->shown_sec)
            ms = 1000 / HZ;
        wait = min_wait(wait, MAX(ms * HZ / 1000, 1));
    }
    if (m.st->mode == NP_VIZ)
    {
        /* #126: the view's frames (lit only: the dark branch above
         * stopped it); a view that can't start leaves for Normal */
        if (!viz_on() && !viz_start())
        {
            struct np_result r;

            ui_log("np", "viz unavailable: back to normal");
            np_set_viz(m.st, false);
            r = np_leave(m.st);
            apply(&m, &r, m.t->tc_idx, m.t->db_gen, m.t->bind);
            wait = min_wait(wait, 1);
        }
        else
        {
            bool lost = false;

            wait = min_wait(wait, viz_frame(&lost));
            if (lost)
            {
                /* the view's claim took the strip back from another
                 * owner (the hold list's push and pop): the title and
                 * the lyrics there are rebuilt at their next use, never
                 * shown from what that owner left (sitting 4b: the
                 * title came back as the list's capture) */
                sc->mq_ok = false;
                sc->lyr_ok = false;
                UPDATE_TRACE("np strip lost by=viz%s", "");
            }
        }
    }
    else
        wait = min_wait(wait, marquee_step(&m, current_tick));
    if (m.st->mode != NP_NORMAL || m.st->volume_shown
        || m.st->seek_pending || m.st->seek_hold)
        wait = min_wait(wait, STATE_POLL);
    if (sc->art_pending)
        wait = min_wait(wait, HZ);  /* look at the commit seq */
    if (sc->art_defer)              /* the deferred read: next pass; the
                                       art thread's: a look every HZ/10 */
        wait = min_wait(wait, sc->art_key == m.t->art_key ? HZ / 10 : 1);
    mem_put();
    return wait;
}

static int np_timeout(struct ui_screen *s)
{
    (void)s;
    return TIMEOUT_BLOCK;           /* tick() says when */
}

const struct ui_screen_ops ui_nowplaying_screen = {
    .name    = "nowplaying",
    .enter   = np_enter,
    .leave   = np_leave_op,
    .draw    = np_draw,
    .input   = np_input,
    .timeout = np_timeout,
    .tick    = np_tick,
};

void ui_nowplaying_note_input(void)
{
    np_last_input = current_tick;
    np_input_hooked = true;
}

/* The idle delay before auto-return, in ticks */
static long auto_return_delay(void)
{
    long bl = backlight_get_current_timeout();  /* ticks; <= 0: always on
                                                   (or never lit) */
    if (bl <= 0)
        return 8 * HZ;
    return MAX(5 * HZ, bl - 3 * HZ);
}

/* Ticks until auto-return is due (TIMEOUT_BLOCK: not armed); pushes Now
 * Playing when it is */
static int auto_return(void)
{
    struct ui_screen *s = ui_top();
    long due;
    int st = audio_status();

    if (!np_input_hooked || !(st & AUDIO_STATUS_PLAY)
        || (st & AUDIO_STATUS_PAUSE) || !s || !s->ops
        || s->ops == &ui_nowplaying_screen
        || (s->ops->name && !strcmp(s->ops->name, "settings-choice"))
        || ui_lock_active())        /* P12: never over the locked screen */
        return TIMEOUT_BLOCK;
    due = np_last_input + auto_return_delay();
    if (TIME_BEFORE(current_tick, due))
        return (int)(due - current_tick);
    UPDATE_TRACE("np auto-return after=%ld",
                 (long)((current_tick - np_last_input) * 1000 / HZ));
    np_last_input = current_tick;   /* once per idle stretch */
    if (ui_push(&ui_nowplaying_screen, NULL) < 0)
        ui_invalidate(UI_DIRTY_ALL);
    return TIMEOUT_BLOCK;
}

/* #126: the charger's Visualizer face (unlit.c, at the backlight's
 * timeout): Now Playing on top with music switches to its Visualizer,
 * which keeps the screen lit.  False: the clock face as before (another
 * screen on top: auto-return brings Now Playing up before the timeout in
 * the usual case) */
bool ui_nowplaying_viz_face(void)
{
    struct ui_screen *top = ui_top();
    struct np_mem m;
    struct np_result r;
    bool ok = false;

    if (!viz_face_wanted() || !top || top->ops != &ui_nowplaying_screen
        || !mem_get(&m))
        return false;
    if (m.sc->have_track && !m.sc->stopped && !m.t->is_audiobook)
    {
        r = np_show_viz(m.st, current_tick);
        if (r.mode_changed)
            apply(&m, &r, m.t->tc_idx, m.t->db_gen, m.t->bind);
        ok = m.st->mode == NP_VIZ;
    }
    mem_put();
    if (ok)
    {
        backlight_on();
        UPDATE_TRACE("np viz face%s", "");
    }
    return ok;
}

bool ui_nowplaying_reserve(void)
{
    return np_reserve();
}

int ui_nowplaying_idle(void)
{
    int w, a;

    /* fallback until ui_main.c calls ui_nowplaying_reserve() after
     * ui_strip_init() (then a no-op): reserving lazily at the first push
     * instead trips "buflib: invalid handle pin: 0" after a tagcache
     * commit in the sim (ipodui-p5-art-fit), so never rely on that */
    if (!np_reserved)
        np_reserve();
    w = np_pb_idle();
    a = auto_return();
    return min_wait(w, a);
}
