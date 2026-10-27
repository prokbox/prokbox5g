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
 * Phase 8b close: sim script command "expect_shadow"; see
 * sim_cmd_shadow.h.
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "lcd.h"
#include "sim_cmd_shadow.h"

/* The split shadow of apps/ipodui/material_data.h (ui_shadow_a, 10
 * columns). Copied here on purpose: the sim must judge the shell against
 * the spec's numbers, not against the table the shell uses. If the table
 * changes, this must be changed with it (and the brief's 3.1). */
#define SHADOW_W     10
#define PANEL_X      160
#define PANEL_H      240
#define REF_C0       12     /* reference columns: well past the seam */
#define REF_C1       27
#define MIN_REF_SUM  500    /* r+g+b (0..765) below this is too dark to
                             * judge a ratio */
#define MIN_FLAT_ROWS 8
static const unsigned char shadow_a[SHADOW_W] =
    { 92, 75, 59, 45, 33, 23, 15, 8, 4, 1 };

extern struct frame_buffer_t lcd_framebuffer_default;

static int px_sum(const fb_data *fb, int y, int c)
{
    unsigned p = fb[y * LCD_FBWIDTH + PANEL_X + c];
    unsigned r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    return (int)(r * 255 / 31 + g * 255 / 63 + b * 255 / 31);
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* Rows where the plain columns REF_C0..REF_C1 are flat (the same brightness, +-6) and
 * at least min_sum bright (r+g+b 0..765): there the picture is taken to be flat across the seam too, so the darkening of
 * column c against column REF_C0 is the shadow alone. Fills ratio[c]
 * (per mille, the median over those rows) and returns their number. */
static int seam_ratios(long ratio[SHADOW_W], int min_sum)
{
    const fb_data *fb = lcd_framebuffer_default.fb_ptr;
    int rows[PANEL_H][SHADOW_W + 1];
    int n = 0, y, c, i;
    int v[PANEL_H];

    for (y = 0; y < PANEL_H; y++)
    {
        int r0 = px_sum(fb, y, REF_C0), lo = r0, hi = r0;

        if (r0 < min_sum)
            continue;
        for (c = REF_C0 + 1; c <= REF_C1; c++)
        {
            i = px_sum(fb, y, c);
            lo = i < lo ? i : lo;
            hi = i > hi ? i : hi;
        }
        if (hi - lo > 6)
            continue;
        for (c = 0; c < SHADOW_W; c++)
            rows[n][c] = px_sum(fb, y, c) * 1000 / r0;
        n++;
    }
    for (c = 0; c < SHADOW_W; c++)
    {
        for (i = 0; i < n; i++)
            v[i] = rows[i][c];
        if (n)
            qsort(v, n, sizeof(int), cmp_int);
        ratio[c] = n ? v[n / 2] : 0;
    }
    return n;
}

bool sim_cmd_shadow(const char *cmd, char *argv[], int argc, int lineno)
{
    long col[SHADOW_W];
    int c, tol, bad = 0, n;
    long agg_got = 0, agg_exp = 0;

    if (strcmp(cmd, "expect_shadow") || argc < 2)
        return false;

    /* expect_shadow <label> [tol_per_mille [min_sum]]: over the rows where the
     * picture beside the seam is flat (columns 12..27), the median
     * ratio of panel column c to column 12 is within tol of
     * (256 - a[c]) / 256, aggregated over columns 0..4, and columns 0..2
     * each within 2 * tol. Default tol 80 (8 %): twice applied (0.58
     * aggregate) and not applied (1.00) both miss it. */
    tol = argc >= 3 ? atoi(argv[2]) : 80;
    n = seam_ratios(col, argc >= 4 ? atoi(argv[3]) : MIN_REF_SUM);
    if (n < MIN_FLAT_ROWS && argv[1][0] == '~')
    {
        fprintf(stderr, "sim_script: expect_shadow %s skipped (soft: %d flat "
                "rows, a mid-fade blend)\n", argv[1], n);
        return true;
    }
    if (n < MIN_FLAT_ROWS)
    {
        fprintf(stderr, "sim_script:%d: expect_shadow failed (%s: only %d "
                "flat bright rows beside the seam, need %d to judge)\n",
                lineno, argv[1], n, MIN_FLAT_ROWS);
        return true;
    }
    for (c = 0; c < 5; c++)
    {
        agg_got += col[c];
        agg_exp += (256 - shadow_a[c]) * 1000 / 256;
    }
    if (labs(agg_got - agg_exp) > 5L * tol)
        bad = 1;
    for (c = 0; c < 3; c++)
        if (labs(col[c] - (256 - shadow_a[c]) * 1000 / 256) > 2L * tol)
            bad = 1;
    fprintf(stderr, "sim_script: expect_shadow %s %d flat rows, cols0-4 %ld/%ld%s per mille "
            "(col0 %ld exp %d, col1 %ld exp %d, col2 %ld exp %d)\n",
            argv[1], n, agg_got / 5, agg_exp / 5, "", col[0],
            (256 - shadow_a[0]) * 1000 / 256, col[1],
            (256 - shadow_a[1]) * 1000 / 256, col[2],
            (256 - shadow_a[2]) * 1000 / 256);
    if (bad)
        fprintf(stderr, "sim_script:%d: expect_shadow failed (%s: the split "
                "shadow at panel columns 0..4 is %ld per mille of the plain "
                "columns, expected %ld: %s)\n", lineno, argv[1],
                agg_got / 5, agg_exp / 5,
                agg_got / 5 > agg_exp / 5 ? "not applied, or applied too "
                "lightly" : "applied twice, or too heavily");
    return true;
}
