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
 *  Host tests for apps/ipodui/material.c, the shell's material kit (Phase 8b
 * brief 3.1). Pure C; see run.sh.
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
#include <stdio.h>
#include <string.h>
#include "material.h"
#include "np_gel_golden.h"

static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", \
    __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* luminance x 1000 of an RGB565 colour, from its 8-bit channels */
static int lum(uint16_t v)
{
    int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 299 * r + 587 * g + 114 * b;
}

static void same_rows(const char *what, const uint16_t *a, const uint16_t *b,
                      int n)
{
    int i;

    for (i = 0; i < n; i++)
        CHECK(a[i] == b[i], "%s row %d: 0x%04x != golden 0x%04x", what, i,
              a[i], b[i]);
}

/* Every table equals its key evaluation; a pair without a table is NULL
 * and the evaluation still answers. */
static void test_tables(void)
{
    int m, h, i, tables = 0;

    for (m = 0; m < UI_MAT_COUNT; m++)
        for (h = 1; h <= 120; h++)
        {
            const uint16_t *r = ui_mat_rows(m, h);
            if (!r)
                continue;
            tables++;
            for (i = 0; i < h; i++)
                CHECK(r[i] == ui_mat_color(m, i, h),
                      "material %d h %d row %d: table 0x%04x, keys 0x%04x",
                      m, h, i, r[i], ui_mat_color(m, i, h));
        }
    CHECK(tables == 9, "%d run-time tables, art/materials.py RUNTIME has 9",
          tables);
    /* the pairs the shell draws (art/materials.py RUNTIME) */
    CHECK(ui_mat_rows(UI_MAT_GEL_BLUE, 14) && ui_mat_rows(UI_MAT_GEL_BLUE, 15)
          && ui_mat_rows(UI_MAT_GEL_BLUE, 19) && ui_mat_rows(UI_MAT_GROOVE, 14)
          && ui_mat_rows(UI_MAT_GEL_SKY, 24) && ui_mat_rows(UI_MAT_GEL_SKY, 25)
          && ui_mat_rows(UI_MAT_GEL_SKY, 44) && ui_mat_rows(UI_MAT_GEL_SKY, 55)
          && ui_mat_rows(UI_MAT_METAL, 76),
          "a run-time pair has no table");
    CHECK(ui_mat_rows(UI_MAT_GEL_SKY, 23) == NULL, "no table for h 23");
    CHECK(ui_mat_rows(UI_MAT_COUNT, 14) == NULL, "no table past the enum");
    /* clamped rows, any height */
    CHECK(ui_mat_color(UI_MAT_GEL_SKY, -3, 30) ==
          ui_mat_color(UI_MAT_GEL_SKY, 0, 30), "row < 0 clamps");
    CHECK(ui_mat_color(UI_MAT_GEL_SKY, 40, 30) ==
          ui_mat_color(UI_MAT_GEL_SKY, 29, 30), "row >= h clamps");
}

/* Now Playing's gel, groove and reflection: the kit must reproduce the
 * golden captured on main before the move, bit for bit. */
static void test_np_golden(void)
{
    unsigned k;
    int d;

    same_rows("gel 14", ui_mat_rows(UI_MAT_GEL_BLUE, 14), np_gel_golden_14, 14);
    same_rows("gel 15", ui_mat_rows(UI_MAT_GEL_BLUE, 15), np_gel_golden_15, 15);
    same_rows("gel 19", ui_mat_rows(UI_MAT_GEL_BLUE, 19), np_gel_golden_19, 19);
    same_rows("groove 14", ui_mat_rows(UI_MAT_GROOVE, 14),
              np_groove_golden_14, 14);
    for (k = 0; k < sizeof(np_refl_golden) / sizeof(np_refl_golden[0]); k++)
    {
        const struct np_refl_golden *g = &np_refl_golden[k];
        for (d = 0; d < g->n; d++)
            CHECK(ui_mat_refl_a(d, g->h, g->n) == g->a[d],
                  "refl h %d n %d d %d: %u != golden %u", g->h, g->n, d,
                  ui_mat_refl_a(d, g->h, g->n), g->a[d]);
    }
}

/* Spec index.md, the gel: the body deepens to its richest colour around
 * the middle, then brightens toward the bottom; light top line, darker
 * bottom edge. One valley in the body's luminance at every height. */
static void test_gel_profile(void)
{
    static const int gels[] = { UI_MAT_GEL_BLUE, UI_MAT_GEL_SKY,
                                UI_MAT_GEL_GREEN, UI_MAT_GEL_RED,
                                UI_MAT_GEL_AMBER };
    unsigned k;
    int h, i;

    for (k = 0; k < sizeof(gels) / sizeof(gels[0]); k++)
        for (h = 8; h <= 60; h++)
        {
            int rising = 0, prev = lum(ui_mat_color(gels[k], 1, h));
            for (i = 2; i < h - 1; i++)
            {
                int l = lum(ui_mat_color(gels[k], i, h));
                if (l > prev)
                    rising = 1;
                CHECK(!(rising && l < prev - 2000),
                      "gel %d h %d: darkens again at row %d", gels[k], h, i);
                prev = l;
            }
            CHECK(lum(ui_mat_color(gels[k], 0, h)) >
                  lum(ui_mat_color(gels[k], 1, h)),
                  "gel %d h %d: top line not lighter", gels[k], h);
            CHECK(lum(ui_mat_color(gels[k], h - 1, h)) <
                  lum(ui_mat_color(gels[k], h - 2, h)),
                  "gel %d h %d: bottom edge not darker", gels[k], h);
        }
}

static void test_header_and_shadow(void)
{
    uint16_t col[3 * UI_SHADOW_W];
    int i;

    for (i = 1; i < UI_MAT_HEADER_ROWS; i++)
        CHECK(lum(ui_mat_header_rows[i]) <= lum(ui_mat_header_rows[i - 1]),
              "header row %d lighter than the one above", i);
    CHECK(ui_shadow_a[0] == 92, "shadow at the seam: %u, want 36 %% (92)",
          ui_shadow_a[0]);
    CHECK(UI_SHADOW_W == 10, "shadow width %d, want 10 (owner 2026-10-20)",
          UI_SHADOW_W);
    for (i = 1; i < UI_SHADOW_W; i++)
        CHECK(ui_shadow_a[i] < ui_shadow_a[i - 1], "shadow not falling at %d",
              i);
    /* applied once, in place, columns 0..W-1 only, 'stride' apart */
    for (i = 0; i < 3 * UI_SHADOW_W; i++)
        col[i] = 0xffff;
    ui_shadow_apply(col, 2 * UI_SHADOW_W, 2);
    for (i = 0; i < UI_SHADOW_W; i++)
    {
        unsigned k = 256 - ui_shadow_a[i];
        uint16_t want = (uint16_t)(((31 * k >> 8) << 11) | ((63 * k >> 8) << 5)
                                   | (31 * k >> 8));
        CHECK(col[i] == want, "shadow col %d: 0x%04x != 0x%04x", i, col[i],
              want);
        CHECK(col[2 * UI_SHADOW_W + i] == want, "shadow row 1 col %d", i);
        CHECK(col[UI_SHADOW_W + i] == 0xffff, "shadow touched col %d",
              UI_SHADOW_W + i);
    }
}

int main(void)
{
    test_tables();
    test_np_golden();
    test_gel_profile();
    test_header_and_shadow();
    if (fails)
        printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
