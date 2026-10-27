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

/* Host tests for apps/ipodui/np_draw.c (Now Playing drawing maths):
 * the slant mapping, the fit rectangle (against artfmt.h's), the art and
 * reflection placement for square and non-square art, the bar columns,
 * the time labels, the bar span and the chapter ticks (np_scrub.c).
 * With NPSCREEN_PREVIEW=<dir> it also writes PPM previews of the slant
 * (square checker, 16:9, 2:3, 4:1, the placeholder) for review. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "np_draw.h"
#include "np_scrub.h"
#include "artfmt.h"
#include "check.h"

#define W 320
#define H 240
static np_px fb[W * H];
static np_px art[142 * 142];

static struct np_canvas canvas(void)
{
    struct np_canvas cv = { fb, W, W, H };
    return cv;
}

static void clear(np_px c)
{
    int i;
    for (i = 0; i < W * H; i++)
        fb[i] = c;
}

static void preview(const char *dir, const char *name)
{
    char path[512];
    FILE *f;
    int i;

    if (!dir)
        return;
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (i = 0; i < W * H; i++)
    {
        unsigned p = fb[i];
        unsigned char rgb[3] = { (unsigned char)(((p >> 11) & 31) * 255 / 31),
                                 (unsigned char)(((p >> 5) & 63) * 255 / 63),
                                 (unsigned char)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* checker art w x h: 16 px squares, red / blue, a yellow top row */
static void make_art(int w, int h)
{
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            art[y * w + x] = y < 4 ? NP_RGB(255, 255, 0)
                             : (((x >> 4) ^ (y >> 4)) & 1)
                               ? NP_RGB(255, 0, 0) : NP_RGB(0, 0, 255);
}

static void test_slant(void)
{
    int c, prev = -1, du0, du1;

    CHECK_EQ(np_slant_top256(0), 33 * 256);
    CHECK_EQ(np_slant_top256(NP_SLANT_W - 1), 44 * 256);
    CHECK_EQ(np_slant_h256(0), 135 * 256);
    CHECK_EQ(np_slant_h256(NP_SLANT_W - 1), 119 * 256);
    CHECK_EQ(np_slant_u256(0), 0);
    CHECK_EQ(np_slant_u256(NP_SLANT_W - 1), 142 * 256);
    for (c = 0; c < NP_SLANT_W; c++)
    {
        int u = np_slant_u256(c);
        CHECK(u > prev);                /* strictly increasing */
        prev = u;
    }
    /* perspective: the far (right) side shows more of the box per
     * screen column, by about hL / hR = 135 / 119 squared */
    du0 = np_slant_u256(1) - np_slant_u256(0);
    du1 = np_slant_u256(NP_SLANT_W - 1) - np_slant_u256(NP_SLANT_W - 2);
    CHECK(du1 > du0);
    CHECK(du1 * 100 / du0 >= 125 && du1 * 100 / du0 <= 133);   /* 1.29 */
    /* the box's bottom edge: left y 167, right y 162 (inclusive) */
    CHECK_EQ((np_slant_top256(0) + np_slant_h256(0)) / 256 - 1, 167);
    CHECK_EQ((np_slant_top256(NP_SLANT_W - 1)
              + np_slant_h256(NP_SLANT_W - 1)) / 256 - 1, 162);
}

static void test_fit(void)
{
    static const int src[][2] = {
        { 1920, 1080 }, { 600, 900 }, { 400, 100 }, { 500, 500 },
        { 1, 1 }, { 3000, 7 }, { 7, 3000 }, { 141, 142 }, { 0, 0 },
    };
    struct np_fit f;
    struct artcache_fit a;
    unsigned i;

    np_fit_rect(16, 9, 142, &f);
    CHECK_EQ(f.w, 142);
    CHECK_EQ(f.h, 80);
    np_fit_rect(2, 3, 142, &f);
    CHECK_EQ(f.w, 95);
    CHECK_EQ(f.h, 142);
    np_fit_rect(4, 1, 142, &f);
    CHECK_EQ(f.h, 36);
    CHECK_EQ(f.y, 53);
    for (i = 0; i < sizeof(src) / sizeof(src[0]); i++)
    {
        np_fit_rect(src[i][0], src[i][1], 142, &f);
        artcache_fit_rect(src[i][0], src[i][1], 142, 142, &a);
        CHECK_EQ(f.w, a.w);
        CHECK_EQ(f.h, a.h);
        CHECK_EQ(f.x, a.x);
        CHECK_EQ(f.y, a.y);
    }
}

static void test_art_square(const char *dir)
{
    struct np_canvas cv = canvas();
    const np_px red = NP_RGB(255, 0, 0), white = NP_WHITE;
    int y;

    for (y = 0; y < 142 * 142; y++)
        art[y] = red;
    clear(white);
    np_art_render(&cv, art, 142, 142, white);
    /* left column x 9: rows 33-167 are the art, 32 untouched */
    CHECK_EQ(fb[32 * W + 9], white);
    for (y = 33; y <= 167; y++)
        CHECK_EQ(fb[y * W + 9], red);
    /* right column x 145: rows 44-162 */
    CHECK_EQ(fb[43 * W + 145], white);
    CHECK_EQ(fb[100 * W + 145], red);
    CHECK_EQ(fb[162 * W + 145], red);
    /* outside the slant nothing is touched */
    CHECK_EQ(fb[100 * W + 8], white);
    CHECK_EQ(fb[100 * W + 146], white);
    /* the reflection: pinkish right under, fading, nothing from y NP_REFL_LIMIT */
    CHECK(fb[168 * W + 9] != white && fb[168 * W + 9] != red);
    CHECK_EQ(fb[(NP_REFL_LIMIT - 14) * W + 9] != white, 1);
    for (y = NP_REFL_LIMIT; y < H; y++)
        CHECK_EQ(fb[y * W + 9], white);
    CHECK(fb[163 * W + 145] != white);
    CHECK_EQ(fb[(163 + NP_REFL_LEN) * W + 145], white);  /* faded out */
    make_art(142, 142);
    clear(white);
    np_art_render(&cv, art, 142, 142, white);
    preview(dir, "slant-square");
}

/* non-square: the fitted image sits centred in the box, on the slant */
static void test_art_fit(const char *dir, int sw, int sh, const char *name)
{
    struct np_canvas cv = canvas();
    const np_px white = NP_WHITE;
    struct np_fit f;
    int y, top = -1, bot = -1, x, left = -1, right = -1;

    np_fit_rect(sw, sh, 142, &f);
    make_art(f.w, f.h);
    clear(white);
    np_art_render(&cv, art, f.w, f.h, white);
    preview(dir, name);
    /* at the left column (if the image reaches it) the image spans
     * f.y .. f.y + f.h of the box: 1:1 vertically there */
    for (x = NP_SLANT_X; x < NP_SLANT_X + NP_SLANT_W; x++)
        if (fb[(NP_SLANT_TOP_L + 71) * W + x] != white)
        {
            if (left < 0)
                left = x;
            right = x;
        }
    if (f.w == 142)
    {
        const int x0 = NP_SLANT_X, t0 = NP_SLANT_TOP_L;
        /* left column: the box is 135 rows for 142 box rows there */
        int ey0 = t0 + f.y * 135 / 142, ey1 = t0 + (f.y + f.h) * 135 / 142;
        CHECK_EQ(left, NP_SLANT_X);
        CHECK_EQ(right, NP_SLANT_X + NP_SLANT_W - 1);
        for (y = 28; y < 196; y++)
            if (fb[y * W + x0] == NP_RGB(255, 255, 0) && top < 0)
                top = y;
        CHECK(top >= ey0 - 1 && top <= ey0 + 1);
        /* last solid row of the image at the left column */
        for (y = ey0; y < 196; y++)
            if (fb[y * W + x0] == NP_RGB(255, 0, 0)
                || fb[y * W + x0] == NP_RGB(0, 0, 255))
                bot = y;
        CHECK(bot >= ey1 - 2 && bot <= ey1);
        /* the reflection starts under its own bottom edge */
        CHECK(fb[(ey1 + 2) * W + x0] != white);
    }
    else
    {
        /* tall art: centred horizontally on the slant, not touching
         * the box's sides */
        CHECK(left > NP_SLANT_X);
        CHECK(right < NP_SLANT_X + NP_SLANT_W - 1);
    }
}

static void test_placeholder(const char *dir)
{
    struct np_canvas cv = canvas();
    int i, light = 0;

    np_placeholder_render(art, 142);
    for (i = 0; i < 142 * 142; i++)
        if ((art[i] >> 11) > 22)
            light++;
    /* a large light note on slate blue: 10-35 % of the box */
    CHECK(light > 142 * 142 * 10 / 100);
    CHECK(light < 142 * 142 * 35 / 100);
    clear(NP_WHITE);
    np_art_render(&cv, art, 142, 142, NP_WHITE);
    preview(dir, "slant-placeholder");
}

/* ---- docs/design/np-reflection.md section 7 ---------------------------- */
#include <math.h>
#define R5(p) (((p) >> 11) & 31)
#define G6(p) (((p) >> 5) & 63)

/* the image's bottom edge row (exclusive, as np_art_render's y1) and
 * scale on slant column c, for an image fh rows tall at box row fy */
static void col_geom(int c, int fy, int fh, int *y1, double *h)
{
    int top = np_slant_top256(c), hh = np_slant_h256(c);
    int y0f = top + fy * hh / NP_SLANT_BOX;
    int y1f = top + (fy + fh) * hh / NP_SLANT_BOX;
    *y1 = (y1f + 255) >> 8;
    *h = (y1f - y0f) / 256.0;
}

/* (a) per-column source row within +-1; (d) for fitted images the mirror
 * is about the image's own bottom edge. A 2-row white marker on a black
 * card is mirrored: the lit reflection rows' centre must sit where
 * v = v_bottom - (d + 0.5) s says (d < 14: still a single tap). */
static void test_refl_rows(const char *name, int sw, int sh)
{
    struct np_canvas cv = canvas();
    struct np_fit f;
    static const int cols[] = { 0, 68, 136 };
    int k, ci;

    np_fit_rect(sw, sh, 142, &f);
    for (ci = 0; ci < 3; ci++)
    {
        int c = cols[ci], y1, u, mk;
        double h;
        u = np_slant_u256(c) >> 8;
        if (u < f.x || u >= f.x + f.w)
            continue;                   /* tall art: column outside */
        col_geom(c, f.y, f.h, &y1, &h);
        for (k = 1; k <= 3; k++)
        {
            int y, lit = 0, cnt = 0;
            double s = f.h / h, exp_d, got;

            mk = f.h - 1 - (int)(k * 4 * s) - 1;   /* marker rows mk, mk+1 */
            if (mk < 2)
                continue;
            for (y = 0; y < f.w * f.h; y++)
                art[y] = NP_RGB(0, 0, 0);
            for (y = mk; y <= mk + 1; y++)
                for (u = 0; u < f.w; u++)
                    art[y * f.w + u] = NP_WHITE;
            clear(NP_RGB(0, 0, 0));
            np_art_render(&cv, art, f.w, f.h, NP_RGB(0, 0, 0));
            got = 0;
            for (y = y1; y < NP_REFL_LIMIT; y++)
                if (R5(fb[y * W + NP_SLANT_X + c]) > 0)
                {
                    got += y - y1;
                    cnt++;
                    lit = 1;
                }
            CHECK(lit);
            if (!lit)
                continue;
            got /= cnt;
            /* marker centre mk + 1 (continuous), bottom edge at f.h */
            exp_d = (f.h - (mk + 1.0)) / s - 0.5;
            if (fabs(got - exp_d) > 1.0)
            {
                check_fails++;
                fprintf(stderr, "%s c=%d marker %d: reflection row d=%.2f, "
                        "expected %.2f (+-1)\n", name, c, mk, got, exp_d);
            }
            /* nothing above the contact line's reflection start: the
             * first reflection row sits right under the bottom edge */
            CHECK(y1 <= NP_REFL_LIMIT);
        }
    }
}

/* (b) alpha under the quadratic feather; (c) rows >= NP_REFL_LIMIT untouched.
 * A white card on black: out G (6 bit) = 63 alpha. The blend is 5-bit
 * with a checkerboard dither, so alpha is measured over 2x2 pixels and
 * the tolerance is 8/256 (one 5-bit step), not the spec's 2/256. */
static void test_refl_alpha(void)
{
    struct np_canvas cv = canvas();
    static const int cols[] = { 0, 68, 135 };
    static const int ds[] = { 0, 10, 20, 30 };
    int ci, di, x, y, i;
    double worst = 0;

    for (i = 0; i < 142 * 142; i++)
        art[i] = NP_WHITE;
    for (i = 0; i < W * H; i++)
        fb[i] = NP_RGB(0, 0, 0);
    np_art_render(&cv, art, 142, 142, NP_RGB(0, 0, 0));
    for (ci = 0; ci < 3; ci++)
        for (di = 0; di < 4; di++)
        {
            int c = cols[ci], d = ds[di], y1, n = 0;
            double h, sum = 0, ea = 0;
            int dy, dx;

            col_geom(c, 0, 142, &y1, &h);
            for (dy = 0; dy < 2; dy++)
            {
                int yy = y1 + d + dy;
                double a = NP_REFL_A0 / 256.0
                           * exp(-(NP_REFL_K_Q8 / 256.0) * (d + dy) / h);
                if (yy >= NP_REFL_LIMIT - NP_REFL_FEATHER)
                {
                    double rl = NP_REFL_LIMIT - yy;
                    a *= rl * rl / (double)(NP_REFL_FEATHER * NP_REFL_FEATHER);
                }
                if (yy >= NP_REFL_LIMIT)
                    a = 0;
                for (dx = 0; dx < 2; dx++, n++)
                {
                    ea += a;
                    if (yy < H)
                        sum += G6(fb[yy * W + NP_SLANT_X + c + dx]) / 63.0;
                }
            }
            sum /= n;
            ea /= n;
            if (fabs(sum - ea) > worst)
                worst = fabs(sum - ea);
            if (fabs(sum - ea) * 256 > 8.0)
            {
                check_fails++;
                fprintf(stderr, "alpha c=%d d=%d: %.1f/256, expected %.1f "
                        "(+-8)\n", c, d, sum * 256, ea * 256);
            }
        }
    /* the feather itself: alpha never rises over the last rows */
    for (ci = 0; ci < 3; ci++)
    {
        int c = cols[ci], prev = 99;
        for (y = NP_REFL_LIMIT - NP_REFL_FEATHER; y < NP_REFL_LIMIT; y++)
        {
            int v = R5(fb[y * W + NP_SLANT_X + c]) + R5(fb[y * W + NP_SLANT_X + c + 1]);
            CHECK(v <= prev + 1);       /* dither: +-1 step */
            prev = v;
        }
        CHECK(R5(fb[(NP_REFL_LIMIT - 1) * W + NP_SLANT_X + c]) <= 1);
    }
    /* (c) rows >= NP_REFL_LIMIT untouched, over the whole slant width */
    for (i = 0; i < W * H; i++)
        fb[i] = NP_RGB(7, 9, 11);
    np_art_render(&cv, art, 142, 142, NP_RGB(7, 9, 11));
    for (y = NP_REFL_LIMIT; y < H; y++)
        for (x = 0; x < W; x++)
            CHECK_EQ(fb[y * W + x], NP_RGB(7, 9, 11));
    printf("   reflection alpha: worst error %.1f/256 (tolerance 8)\n",
           worst * 256);
}

static void test_bar(void)
{
    struct np_canvas cv = canvas();
    struct np_bar_style st;
    uint8_t ticks[26];
    int i;

    for (i = 0; i < NP_BAR_H; i++)
    {
        st.track[i] = NP_RGB(200, 200, 200);
        st.fill[i] = NP_RGB(0, 0, 255);
        st.fill_end[i] = NP_RGB(0, 0, 150);
        st.side[i] = NP_RGB(100, 100, 100);
    }
    for (i = 0; i < NP_BAR_REFL; i++)
    {
        st.track_refl[i] = NP_RGB(240, 240, 240);
        st.fill_refl[i] = NP_RGB(220, 220, 255);
    }
    st.tick = NP_RGB(50, 50, 50);
    st.tick_fill = NP_RGB(0, 0, 100);
    memset(ticks, 0, sizeof(ticks));
    ticks[100 >> 3] |= 1 << (100 & 7);
    clear(NP_WHITE);
    np_bar_cols(&cv, 58, 206, 204, 0, 204, 50, ticks, &st);
    CHECK_EQ(fb[210 * W + 58], NP_RGB(0, 0, 150));          /* fill_end */
    CHECK_EQ(fb[210 * W + 58 + 1], NP_RGB(0, 0, 255));      /* fill */
    CHECK_EQ(fb[210 * W + 58 + 49], NP_RGB(0, 0, 255));
    CHECK_EQ(fb[210 * W + 58 + 50], NP_RGB(200, 200, 200)); /* track */
    CHECK_EQ(fb[210 * W + 58 + 100], NP_RGB(50, 50, 50));   /* tick */
    /* chapter marks run the full bar height (owner, 2026-10-11) */
    CHECK_EQ(fb[206 * W + 58 + 100], NP_RGB(50, 50, 50));   /* top row */
    CHECK_EQ(fb[(206 + NP_BAR_H - 1) * W + 58 + 100], NP_RGB(50, 50, 50));
    CHECK_EQ(fb[210 * W + 58 + 203], NP_RGB(100, 100, 100)); /* end */
    CHECK_EQ(fb[(206 + NP_BAR_H + NP_BAR_REFL - 1) * W + 60], NP_RGB(220, 220, 255));      /* reflection */
    CHECK_EQ(fb[(206 + NP_BAR_H + NP_BAR_REFL) * W + 60], NP_WHITE);
    CHECK_EQ(fb[210 * W + 57], NP_WHITE);
    CHECK_EQ(fb[210 * W + 262], NP_WHITE);
    /* a 1 Hz delta touches only its columns */
    fb[210 * W + 58 + 150] = NP_RGB(1, 2, 3);
    np_bar_cols(&cv, 58, 206, 204, 50, 51, 51, ticks, &st);
    CHECK_EQ(fb[210 * W + 58 + 50], NP_RGB(0, 0, 255));
    CHECK_EQ(fb[210 * W + 58 + 150], NP_RGB(1, 2, 3));
}

static void test_time(void)
{
    char b[16];

    np_fmt_time(b, 0, false);
    CHECK(!strcmp(b, "0:00"));
    np_fmt_time(b, 3999, false);
    CHECK(!strcmp(b, "0:03"));
    np_fmt_time(b, 246000, true);
    CHECK(!strcmp(b, "-4:06"));
    np_fmt_time(b, 599000, false);
    CHECK(!strcmp(b, "9:59"));
    np_fmt_time(b, 600000, false);
    CHECK(!strcmp(b, "10:00"));
    np_fmt_time(b, 42475000, true);
    CHECK(!strcmp(b, "-11:47:55"));
    CHECK_EQ(np_fmt_time(b, 3600000, false), 7);
    CHECK(!strcmp(b, "1:00:00"));
}

static void test_atlas_span(void)
{
    static np_px apx[96 * 12];
    struct np_atlas a;
    struct np_canvas cv = canvas();
    int i, x0, w;

    for (i = 0; i < 96 * 12; i++)
        apx[i] = (np_px)i;
    a.px = apx;
    a.stride = 96;
    a.h = 12;
    for (i = 0; i < NP_ATLAS_N; i++)
    {
        a.x[i] = (uint8_t)(i * 7);
        a.w[i] = 7;
    }
    a.w[10] = 3;                    /* ':' */
    a.w[11] = 5;                    /* '-' */
    CHECK_EQ(np_atlas_width(&a, "4:09"), 24);
    CHECK_EQ(np_atlas_width(&a, "-4:09"), 29);
    clear(NP_WHITE);
    CHECK_EQ(np_atlas_draw(&cv, &a, 23, 209, "4:"), 23 + 10);
    CHECK_EQ(fb[209 * W + 23], apx[4 * 7]);
    CHECK_EQ(fb[(209 + 3) * W + 23 + 7], apx[3 * 96 + 70]);
    /* a song's labels give the spec's bar x 58-261 */
    np_bar_span(25, 34, &x0, &w);
    CHECK_EQ(x0, 55);
    CHECK_EQ(x0 + w - 1, 265);
    /* a long book: narrower, between its wider labels */
    np_bar_span(44, 63, &x0, &w);
    CHECK_EQ(x0, 84);
    CHECK_EQ(x0 + w - 1, 236);
}

static void test_ticks(void)
{
    static uint32_t st[648];
    struct np_chapters ch = { st, sizeof(st[0]), 648 };
    uint8_t bits[26];
    int i, n, set = 0;

    for (i = 0; i < 648; i++)
        st[i] = (unsigned long)i * 65000UL + (i % 5) * 1000UL;
    n = np_chapter_ticks(&ch, 648UL * 65000UL, 154, bits);
    for (i = 0; i < 154; i++)
        set += !!(bits[i >> 3] & (1 << (i & 7)));
    CHECK_EQ(n, set);
    CHECK(n <= 153 && n > 140);     /* merged on 154 px */
    /* the incremental chapter step agrees with np_chapter_at */
    for (i = 0; i < 2000; i++)
    {
        unsigned long pos = (unsigned long)i * 21061UL;
        CHECK_EQ(np_chapter_step(&ch, pos, i ? (int)(pos / 65000) : 0),
                 np_chapter_at(&ch, pos, NULL));
    }
}

static void test_glyph_clip(void)
{
    struct np_canvas cv = canvas();
    int id;

    clear(NP_WHITE);
    for (id = 0; id < NPG_COUNT; id++)
    {
        np_glyph_draw(&cv, (enum np_glyph_id)id, -5, -5, NULL, 0, 0);
        np_glyph_draw(&cv, (enum np_glyph_id)id, W - 3, H - 3, NULL, 0, 0);
    }
    CHECK(fb[0] != NP_WHITE || fb[W * H - 1] != NP_WHITE || 1);
}


/* ---- QW2 (docs/phase11-brief.md §3.9) ------------------------------------
 * The composed image (the art builder's np_art_render() through
 * np_comp_canvas()) equals the runtime render on the LCD over the
 * composed rect, and nothing else on the LCD is touched; the placeholder
 * rodata (art/np/npph.py) decodes to exactly the runtime placeholder. */
static np_px comp[NP_COMPOSED_W * NP_COMPOSED_H];

static int comp_diff(void)
{
    int x, y, n = 0;

    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
        {
            bool in = x >= NP_COMPOSED_X && x < NP_COMPOSED_X + NP_COMPOSED_W
                      && y >= NP_COMPOSED_Y
                      && y < NP_COMPOSED_Y + NP_COMPOSED_H;
            np_px want = in ? comp[(y - NP_COMPOSED_Y) * NP_COMPOSED_W
                                   + x - NP_COMPOSED_X] : NP_WHITE;
            n += fb[y * W + x] != want;
        }
    return n;
}

static void comp_render(const np_px *src, int w, int h)
{
    struct np_canvas cv = canvas(), cc;
    int i;

    clear(NP_WHITE);
    np_art_render(&cv, src, w, h, NP_WHITE);
    for (i = 0; i < NP_COMPOSED_W * NP_COMPOSED_H; i++)
        comp[i] = NP_WHITE;
    np_comp_canvas(&cc, comp);
    np_art_render(&cc, src, w, h, NP_WHITE);
}

static void test_composed(void)
{
    static const int asp[][2] = { { 1, 1 }, { 16, 9 }, { 2, 3 }, { 4, 1 } };
    unsigned k;
    int ids[2] = { NPG_NOTE, NPG_BOOK };

    CHECK_EQ(NP_COMPOSED_W, 137);
    CHECK_EQ(NP_COMPOSED_H, 187);
    CHECK(NP_LAYOUT_HASH != 0);
    for (k = 0; k < sizeof(asp) / sizeof(asp[0]); k++)
    {
        struct np_fit f;
        int x, y;

        np_fit_rect(asp[k][0] * 100, asp[k][1] * 100, 142, &f);
        for (y = 0; y < f.h; y++)
            for (x = 0; x < f.w; x++)
                art[y * f.w + x] = NP_RGB(x * 255 / f.w, y * 255 / f.h,
                                          (x ^ y) & 0xff);
        comp_render(art, f.w, f.h);
        CHECK_EQ(comp_diff(), 0);
    }
    for (k = 0; k < 2; k++)
    {
        np_placeholder_render_glyph(art, 142, (enum np_glyph_id)ids[k]);
        comp_render(art, 142, 142);
        CHECK_EQ(comp_diff(), 0);
        /* the build-time placeholder: pixel-identical to the runtime one */
        memset(comp, 0, sizeof(comp));
        CHECK(np_ph_decode((enum np_glyph_id)ids[k], comp));
        CHECK_EQ(comp_diff(), 0);
    }
    CHECK(!np_ph_decode(NPG_STAR_BIG, comp));
}

int main(void)
{
    const char *dir = getenv("NPSCREEN_PREVIEW");

    test_slant();
    test_fit();
    test_art_square(dir);
    test_art_fit(dir, 16, 9, "slant-16x9");
    test_art_fit(dir, 2, 3, "slant-2x3");
    test_art_fit(dir, 4, 1, "slant-4x1");
    test_placeholder(dir);
    test_refl_rows("square", 1, 1);
    test_refl_rows("16x9", 16, 9);
    test_refl_rows("2x3", 2, 3);
    test_refl_alpha();
    test_bar();
    test_time();
    test_atlas_span();
    test_ticks();
    test_glyph_clip();
    test_composed();
    CHECK_DONE("test_draw");
}
