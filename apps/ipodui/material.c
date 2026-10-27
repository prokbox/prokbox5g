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
 * iPod UI shell: the material kit (material.h)
 *
 * The tables are generated rodata (material_data.h, from art/materials.py);
 * this file only looks them up, evaluates a material at a height that has
 * no table, and holds the reflection and shadow helpers. No allocation, no
 * state, no ICODE (IRAM is for codecs). Pure C: tests/material links it.
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
#include <stddef.h>
#include "material.h"

struct ui_mat_key { uint8_t t, r, g, b; };     /* t: 0..255 down the body */

#define MAT_LINE    0x01            /* first row is 'line' */
#define MAT_EDGE    0x02            /* last row is 'edge' */
struct ui_mat_def {
    const struct ui_mat_key *keys;
    uint8_t nkeys;
    uint16_t line, edge;
    uint8_t flags;
};

struct ui_mat_table {
    uint8_t m, h;
    const uint16_t *rows;
};

#include "material_data.h"

#define MAT_RGB(r, g, b) \
    ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

const uint16_t *ui_mat_rows(enum ui_mat m, int h)
{
    unsigned i;

    for (i = 0; i < sizeof(mat_tables) / sizeof(mat_tables[0]); i++)
        if (mat_tables[i].m == m && mat_tables[i].h == h)
            return mat_tables[i].rows;
    return NULL;
}

/* The colour at t (0..255) between the two keys around it */
static uint16_t key_lerp(const struct ui_mat_key *k, int n, int t)
{
    const struct ui_mat_key *a, *b;
    int i, span, f;

    for (i = 1; i < n - 1 && t > k[i].t; i++)
        ;
    a = &k[i - 1];
    b = &k[i];
    span = b->t - a->t;
    f = span ? (t - a->t) * 256 / span : 0;
    if (f < 0)
        f = 0;
    if (f > 256)
        f = 256;
    return MAT_RGB(a->r + (b->r - a->r) * f / 256,
                   a->g + (b->g - a->g) * f / 256,
                   a->b + (b->b - a->b) * f / 256);
}

uint16_t ui_mat_color(enum ui_mat m, int i, int h)
{
    const struct ui_mat_def *d = &mat_defs[(unsigned)m < UI_MAT_COUNT
                                           ? m : UI_MAT_GEL_BLUE];
    int lo = (d->flags & MAT_LINE) ? 1 : 0;
    int hi = (d->flags & MAT_EDGE) ? h - 2 : h - 1;
    int nb = hi - lo + 1;

    if (i < 0)
        i = 0;
    if (i >= h)
        i = h - 1;
    if ((d->flags & MAT_LINE) && i == 0)
        return d->line;
    if ((d->flags & MAT_EDGE) && i == h - 1 && h > 1)
        return d->edge;
    return key_lerp(d->keys, d->nkeys,
                    (i - lo) * 255 / (nb > 1 ? nb - 1 : 1));
}

/* round(256 * exp(-8.7 * i / 256)) (np-reflection.md §3), 255 for 256 */
static const uint8_t refl_exp[96] = {
    255, 247, 239, 231, 223, 216, 209, 202, 195, 189, 182, 176, 170, 165,
    159, 154, 149, 144, 139, 134, 130, 125, 121, 117, 113, 109, 106, 102,
     99,  96,  92,  89,  86,  83,  81,  78,  75,  73,  70,  68,  66,  64,
     61,  59,  57,  55,  54,  52,  50,  48,  47,  45,  44,  42,  41,  39,
     38,  37,  36,  34,  33,  32,  31,  30,  29,  28,  27,  26,  25,  25,
     24,  23,  22,  21,  21,  20,  19,  19,  18,  17,  17,  16,  16,  15,
     15,  14,  14,  13,  13,  12,  12,  12,  11,  11,  10,  10,
};

unsigned ui_mat_refl_a(int d, int h, int n)
{
    /* 72 * exp(-7 * d / h), exp by the 8.7 table: index = 256 * 7 d /
     * (8.7 h) */
    unsigned idx = (unsigned)(d * 256 * 70) / (unsigned)(h * 87);
    unsigned a = idx < sizeof(refl_exp) ? (72u * refl_exp[idx]) >> 8 : 0;

    if (d >= n - 2)
        a = a * (unsigned)(n - d) / 3;
    return a;
}

void ui_shadow_apply(uint16_t *px, int stride, int h)
{
    int y, c;

    for (y = 0; y < h; y++, px += stride)
        for (c = 0; c < UI_SHADOW_W; c++)
        {
            unsigned p = px[c], k = 256 - ui_shadow_a[c];

            px[c] = (uint16_t)(((((p >> 11) * k) >> 8) << 11)
                               | (((((p >> 5) & 63) * k) >> 8) << 5)
                               | (((p & 31) * k) >> 8));
        }
}
