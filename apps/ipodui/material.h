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
 * iPod UI shell: the material kit
 *
 * One definition of every shell material (spec index.md "Visual language":
 * blue gel, green/red gel, grey gloss, metal, groove), generated at build
 * time from art/materials.py into material_data.h, plus the head-on
 * reflection and the split-screen shadow (Phase 8b brief 3.1). Pure C (no
 * Rockbox headers), so the host tests link it as it is. Colours are
 * RGB565, the LCD's native order (fb_data on this target).
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
#ifndef IPODUI_MATERIAL_H
#define IPODUI_MATERIAL_H

#include <stdint.h>

enum ui_mat {                       /* one entry per material, spec index.md */
    UI_MAT_GEL_BLUE,                /* Now Playing fills, diamond, stars */
    UI_MAT_GEL_SKY,                 /* the same gel, lighter: selection bar,
                                     * header play/pause (owner default O1) */
    UI_MAT_GEL_GREEN, UI_MAT_GEL_RED,   /* battery fill, low battery */
    UI_MAT_GEL_AMBER,               /* battery at 30 % or less */
    UI_MAT_GLOSS_GREY,              /* scrollbar rod (horizontal) */
    UI_MAT_METAL,                   /* tiles, capsule, glyph shading */
    UI_MAT_GROOVE,                  /* NP track, capsule channel */
    UI_MAT_COUNT
};

/* Row colours of material m for an object h rows tall: a generated const
 * table for every (m, h) pair the shell draws (art/materials.py RUNTIME),
 * else NULL (the caller falls back to ui_mat_color()). Never computed per
 * pixel. */
const uint16_t *ui_mat_rows(enum ui_mat m, int h);

/* Row i of material m for an object h rows tall, evaluated from the keys:
 * the same value a table holds (the fallback for any other height; a few
 * multiplies per row, never per pixel). */
uint16_t ui_mat_color(enum ui_mat m, int i, int h);

/* Head-on reflection alpha (/256) of row d under an object h tall, n rows
 * (np_flat_refl_a() moved here, same values): 72 * exp(-7 * d / h)
 * (A0 ~ 0.28), the last two rows feathered (x 2/3, 1/3). For pre-blended
 * tables, never per pixel. */
unsigned ui_mat_refl_a(int d, int h, int n);

/* Split-screen shadow: alpha (/256) of panel column c (0 = the seam). */
#define UI_SHADOW_W 10                     /* owner 2026-10-20 (O9 was 6) */
extern const uint8_t ui_shadow_a[UI_SHADOW_W];
/* Applies the shadow to a composed 160 x h panel image in place (px is
 * the panel's column 0, the seam). */
void ui_shadow_apply(uint16_t *px, int stride, int h);

/* The header background, y 0..19 (theme.c's header_rows). */
#define UI_MAT_HEADER_ROWS 20
extern const unsigned short ui_mat_header_rows[UI_MAT_HEADER_ROWS];

#endif /* IPODUI_MATERIAL_H */
