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
 * iPod UI shell: Now Playing drawing core (Phase 5)
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
#ifndef _IPODUI_NP_DRAW_H_
#define _IPODUI_NP_DRAW_H_

/* Pure C: no Rockbox headers, no statics, no allocation, integer maths
 * only (host-tested by tests/npscreen/, instruction-counted bare metal).
 * Everything draws into a plain RGB565 canvas: on the device that is the
 * LCD framebuffer (stride LCD_WIDTH, origin at the LCD's 0,0), in the
 * host tests an array. The screen (screens/nowplaying.c) owns text,
 * fonts, viewports and LCD pushes; this file owns the pixel work that has
 * to be cheap: the slanted art and its reflection, the bars, the glyphs,
 * the digit labels.
 *
 * Geometry is the spec's (docs/design/spec/now-playing.md): art x 10-147,
 * left edge y 34-175, right edge y 43-167; bar row y 196-229. */

#include <stdbool.h>
#include <stdint.h>

typedef uint16_t np_px;             /* RGB565, the target's native order */

#define NP_RGB(r, g, b) \
    ((np_px)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define NP_WHITE NP_RGB(0xff, 0xff, 0xff)

struct np_canvas {
    np_px *px;                      /* pixel 0,0 */
    int stride;                     /* pixels per row */
    int w, h;                       /* clip: 0..w-1, 0..h-1 */
};

/* fg over bg, a = 0..16 (16 = fg) */
np_px np_blend16(np_px bg, np_px fg, unsigned a);
/* a = 0..256 */
np_px np_blend256(np_px bg, np_px fg, unsigned a);

/* ---- the slanted art ------------------------------------------------------
 * The 142 px art box (NP_ART_BOX) seen in perspective, turned towards the
 * text: column c (0..NP_SLANT_W-1) of the slant, at screen x NP_SLANT_X +
 * c, shows box column np_slant_u256(c) / 256 over a box height that falls
 * linearly from 135 px (left) to 119 px (right). The horizontal mapping is
 * projective (columns compress towards the far, right edge): with
 * hL = 135, hR = 119 and W = NP_SLANT_W - 1,
 *     u(c) = box * c * hR / (W * hL - (hL - hR) * c).
 * The fitted image (artcache_fit_rect(): aspect kept, centred in the box)
 * is mapped through the same quad, so non-square art keeps its shape and
 * sits on the slant with the reflection under its own bottom edge. */
#define NP_SLANT_X       9
#define NP_SLANT_W       137        /* x 9..145 */
#define NP_SLANT_TOP_L   33
#define NP_SLANT_BOT_L   167        /* inclusive: 135 rows */
#define NP_SLANT_TOP_R   44
#define NP_SLANT_BOT_R   162        /* inclusive: 119 rows */
#define NP_SLANT_BOX     142
/* Reflection (docs/design/np-reflection.md): each column mirrors about
 * its own contact point (the image's bottom edge on that column),
 * compressed by the column's own scale, with
 *     alpha = A0 * exp(-K * d / h)
 * (d rows below the contact line, h the image's height on the column:
 * the fade contour keeps the perspective) and a vertical blur growing
 * with d (1, 3 or 5 taps). It runs down behind the elapsed time and the
 * bar row (P11, owner: a glass floor) and the last NP_REFL_FEATHER rows
 * above NP_REFL_LIMIT are feathered by (rows_left / F)^2, so it fades to
 * nothing with no visible edge, above the bar's own reflection (y 220).
 * The blend is 5-bit, dithered in a checkerboard.
 * THE FADE CONSTANTS (one place; retune here, then re-run art/np/npph.py
 * and recapture the NP goldens). Measured 2026-10-09 on the rectified
 * owner references (np-reflection.md §3): np-clean-battlecry A0 0.35,
 * K ~4.9, gone by y ~212-218; np-clean-ufo (a web photo, contrast
 * boosted: A0 0.68) the same shape, gone by y ~220. */
#define NP_REFL_A0       90         /* /256: 0.35 at the contact line */
#define NP_REFL_K_Q8     1280       /* K * 256: K = 5.0 */
#define NP_REFL_LIMIT    220        /* exclusive: the bar's reflection
                                       rows start at 220 */
#define NP_REFL_FEATHER  12         /* rows: y 208-219 */
#define NP_REFL_LEN      (NP_REFL_LIMIT - NP_SLANT_BOT_R - 1)  /* 57 */
#define NP_REFL_BLUR_D   29         /* taps: 1 to d 14, 3 to d 28, then 5 */

/* QW2 (docs/phase11-brief.md §3.9): the composed image, everything
 * np_art_render() can touch: x NP_SLANT_X .. +NP_SLANT_W-1, y
 * NP_SLANT_TOP_L .. NP_REFL_LIMIT-1 (137 x 187), against NP_WHITE. The
 * art builder renders it once per bundle; Now Playing blits it.
 * NP_LAYOUT_HASH names the pixels it holds: any change to the geometry,
 * the reflection or the render itself (bump NP_RENDER_REV) makes every
 * composed bundle stale, and the builder composes it again. */
#define NP_RENDER_REV    2
#define NP_COMPOSED_X    NP_SLANT_X
#define NP_COMPOSED_Y    NP_SLANT_TOP_L
#define NP_COMPOSED_W    NP_SLANT_W
#define NP_COMPOSED_H    (NP_REFL_LIMIT - NP_SLANT_TOP_L)
#define NP_LH_(h, v)     (((h) * 31u + (unsigned)(v)) & 0xffffu)
#define NP_LAYOUT_HASH                                                     \
    ((uint16_t)NP_LH_(NP_LH_(NP_LH_(NP_LH_(NP_LH_(NP_LH_(NP_LH_(NP_LH_(    \
        NP_LH_(NP_LH_(NP_LH_(NP_LH_(0x5157u, NP_RENDER_REV), NP_SLANT_X),  \
        NP_SLANT_W), NP_SLANT_TOP_L), NP_SLANT_BOT_L), NP_SLANT_TOP_R),    \
        NP_SLANT_BOT_R), NP_SLANT_BOX), NP_REFL_LEN), NP_REFL_A0),         \
        NP_REFL_LIMIT), NP_REFL_FEATHER * 64 + NP_REFL_BLUR_D) ^ NP_REFL_K_Q8)

/* A canvas over a composed image (NP_COMPOSED_W x _H, stride _W) whose
 * coordinates are the screen's: np_art_render() writes into it exactly
 * what it writes on the LCD (the dither phase is screen-based). The
 * origin pointer lies before the buffer and is only ever offset back
 * into it (np_art_render() touches nothing outside the composed rect). */
void np_comp_canvas(struct np_canvas *cv, np_px *comp);

/* Box top and height of column c, both in 1/256 px */
int  np_slant_top256(int c);
int  np_slant_h256(int c);
/* Box column at slant column c, 1/256 px (0 .. NP_SLANT_BOX * 256) */
int  np_slant_u256(int c);

/* Fitted rectangle (as artcache_fit_rect(), repeated here so this file
 * stays free of the art cache headers; tests check they agree). */
struct np_fit { int w, h, x, y; };
void np_fit_rect(int src_w, int src_h, int box, struct np_fit *f);

/* Renders the slant of an art_w x art_h image (row-major, stride art_w;
 * the fitted bundle as read, or the placeholder) and its reflection onto
 * a canvas already cleared to bg. Top and bottom edges are antialiased
 * against bg. Touches only x NP_SLANT_X.. +NP_SLANT_W-1, y < NP_REFL_LIMIT;
 * the caller clears all of it (down to NP_REFL_LIMIT) to bg first.
 * art_w/art_h are the fitted size (<= NP_SLANT_BOX). */
void np_art_render(const struct np_canvas *cv, const np_px *art,
                   int art_w, int art_h, np_px bg);
/* np_art_render() for slant columns c0 .. c1 - 1 only (columns are
 * independent: the art builder renders in chunks with a yield between) */
void np_art_render_cols(const struct np_canvas *cv, const np_px *art,
                        int art_w, int art_h, np_px bg, int c0, int c1);

/* The no-art placeholder (spec: a dark slate-blue square with a large
 * light-grey note, slanted and reflected like real art): fills a box x box
 * image (stride box) with a vertical slate gradient and the note glyph
 * upscaled 5/2, centred. Pure pixel work, done once per placeholder. */
void np_placeholder_render(np_px *art, int box);

/* ---- glyphs (art/npglyphs.py -> npglyphs.h) ----------------------------- */
enum np_glyph_id {
    NPG_DIAMOND = 0,
    NPG_STAR_BIG,
    NPG_STAR_SMALL,
    NPG_DOT,
    NPG_REPEAT,
    NPG_REPEAT_ONE,
    NPG_SHUFFLE,
    NPG_SHUFFLE_BIG,
    NPG_SPEAKER,
    NPG_SPEAKER_WAVES,
    NPG_NOTE,
    NPG_BOOK,
    NPG_COUNT
};

struct np_glyph {
    uint8_t w, h;
    const unsigned char *cov;       /* 4-bit coverage */
    const unsigned char *inner;     /* glossy glyphs: coverage shrunk by the
                                       rim, else NULL */
};

void np_glyph_size(enum np_glyph_id id, int *w, int *h);

/* np_placeholder_render() with another 36 x 42 distance-field glyph:
 * NPG_NOTE (the same), or NPG_BOOK for an audiobook (P6 owner). */
void np_placeholder_render_glyph(np_px *art, int box, enum np_glyph_id id);

/* The pre-rendered placeholders (QW2; art/np/npph.py ->
 * npph_data.h): the composed image of np_placeholder_render_glyph(id) +
 * np_art_render() against NP_WHITE, built at build time, palette +
 * PackBits coded per row (the host test checks it pixel for pixel
 * against the runtime render). Decodes the whole composed image
 * (NP_COMPOSED_W x _H, stride _W) into comp: no division, one palette
 * load per pixel. id is NPG_NOTE or NPG_BOOK; false for any other. */
bool np_ph_decode(enum np_glyph_id id, np_px *comp);
/* Coverage 0..15 of glyph pixel x, y (0 outside): for pre-built tiles */
unsigned np_glyph_cov(enum np_glyph_id id, int x, int y);
/* Blends glyph 'id' at x, y over what the canvas holds. rows (h entries)
 * gives a colour per row (a gloss), else every pixel is 'fg'. Glossy
 * glyphs blend their rim towards 'edge'. Clipped to the canvas. */
void np_glyph_draw(const struct np_canvas *cv, enum np_glyph_id id,
                   int x, int y, const np_px *rows, np_px fg, np_px edge);

/* ---- bars (progress, scrubber track, volume) -----------------------------
 * Materials (spec now-playing.md "Bar row materials"): the fill is the
 * blue gel, the empty track a recessed groove with square ends, and both
 * have a flat (head-on) mirror reflection under them: the rows mirrored
 * about the bar's bottom edge at ui_mat_refl_a() (material.h, the kit).
 * Every row colour is pre-blended by the caller once per boot. */
#define NP_BAR_H        14          /* incl. the 1 px border rows */
#define NP_BAR_REFL     5           /* reflection rows under it */
#define NP_TICK_TOP     0           /* chapter ticks: the full bar height, top to bottom like the 7G (owner, 2026-10-11) */

struct np_bar_style {
    np_px track[NP_BAR_H];          /* the empty part, per row */
    np_px fill[NP_BAR_H];           /* the blue fill, per row */
    np_px fill_end[NP_BAR_H];       /* the fill's first column (darker) */
    np_px side[NP_BAR_H];           /* the track's square end columns */
    np_px track_refl[NP_BAR_REFL];  /* reflection rows, pre-blended */
    np_px fill_refl[NP_BAR_REFL];
    np_px tick;                     /* chapter ticks over the track */
    np_px tick_fill;                /* chapter ticks over the fill */
};

/* Rounded rectangle, rows[h] per-row colour, corners of radius r (<= 6)
 * antialiased over what the canvas holds (the shelf channel, its button) */
void np_rrect(const struct np_canvas *cv, int x, int y, int w, int h, int r,
              const np_px *rows);

/* Blends c at a (0..256) over a rectangle (clipped): soft shadows */
void np_blend_rect(const struct np_canvas *cv, int x, int y, int w, int h,
                   np_px c, unsigned a);

/* Paints columns [from, to) of a bar x0..x0+w-1 at row y (top border):
 * the fill up to fill_px (0..w), the track after it, chapter ticks where
 * 'ticks' (bit x, may be NULL) is set, the end columns, and the
 * reflection rows below. Used for full paints and 1 Hz deltas alike. */
void np_bar_cols(const struct np_canvas *cv, int x0, int y, int w,
                 int from, int to, int fill_px, const uint8_t *ticks,
                 const struct np_bar_style *st);

/* Fills a rectangle (clipped) */
void np_fill(const struct np_canvas *cv, int x, int y, int w, int h, np_px c);

/* ---- time labels from a pre-rendered digit atlas ----------------------- */
/* "m:ss", "h:mm:ss" ("-" first if neg); returns the length (<= 10). */
int  np_fmt_time(char *buf, unsigned long ms, bool neg);

#define NP_ATLAS_CHARS  "0123456789:-"
#define NP_ATLAS_N      12
struct np_atlas {
    const np_px *px;                /* atlas image, row-major */
    int stride, h;                  /* pixels per row, rows */
    uint8_t x[NP_ATLAS_N];          /* each character's column */
    uint8_t w[NP_ATLAS_N];          /* and advance width */
};
/* Width of s in the atlas (chars outside NP_ATLAS_CHARS count 0) */
int  np_atlas_width(const struct np_atlas *a, const char *s);
/* Copies s at x, y (the atlas's top row); returns the x after it */
int  np_atlas_draw(const struct np_canvas *cv, const struct np_atlas *a,
                   int x, int y, const char *s);

/* As np_atlas_draw(), but over a background already in place (the art's
 * reflection, P11): the atlas is dark ink on white, so each pixel
 * multiplies what is under it (out = under * atlas / white per channel),
 * which is the exact blend for black ink and within a step for the
 * near-black label ink. */
int  np_atlas_draw_mul(const struct np_canvas *cv, const struct np_atlas *a,
                       int x, int y, const char *s);

/* Redraws a label that showed 'old' at x, y as 's': when both are as long
 * and as wide (tabular digits), only the characters from the first one
 * that differs are copied (the 1 Hz case: usually one or two); else the
 * whole string. Returns the x after it, like np_atlas_draw(). */
int  np_atlas_update(const struct np_canvas *cv, const struct np_atlas *a,
                     int x, int y, const char *old, const char *s);

/* ---- the bar row layout ------------------------------------------------- */
/* Measured on the owner's 2.0.5 photos (photo 1203, 1222, rectified): the
 * remaining time's ink ends at x 308 (its advance, one column of
 * bearing, at NP_TIME_R_X) and the bar is centred, each end
 * NP_TIME_GAP from a slot as wide as the remaining-time label at the
 * track's length ("-4:06": bar x 55-265; "-11:47:55": x 84-236 against
 * 82-236 measured). The elapsed time is right-aligned NP_TIME_GAP_L
 * before the bar. The bar is shorter for long books, as on the 7G.
 * left_w is unused (kept for the callers' symmetry). */
#define NP_TIME_MARGIN  11          /* screen edge to the mirrored slot */
#define NP_TIME_R_X     310         /* remaining, right edge (exclusive) */
#define NP_TIME_GAP     10          /* bar to the remaining time */
#define NP_TIME_GAP_L   11          /* elapsed time to the bar */
void np_bar_span(int left_w, int right_w, int *x0, int *w);

#endif /* _IPODUI_NP_DRAW_H_ */
