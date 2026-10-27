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
 * The drift offload's cross-fade (docs/research/bcm2722/offload.md):
 * one step of the 160 x 240 panel through xfade_blk, bit-exact to the
 * shell's xfade_rect, and the RGB565 -> planes unpacking it needs.
 * The layout gen_xfade.py's kernels assume.
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
#ifndef BCMFW_XFADE_H
#define BCMFW_XFADE_H

#include <stdint.h>

#define XF_W          160               /* the panel */
#define XF_H          240
#define XF_OUT_PITCH  640               /* bytes: the LCD framebuffer's rows */
#define XF_PLANE      (2 * XF_W * XF_H) /* bytes: one 16-bit plane */
#define XF_PLANES     (3 * XF_PLANE)    /* bytes: R, G, B */

void xfade_unpack(uint16_t *planes, const uint16_t *px, int pitch);
void xfade_panel(uint16_t *out, uint16_t *state, const uint16_t *src, int weight);
void xfade_stage(uint16_t *stg, const uint16_t *px, int pitch);
void xfade_half(uint16_t *out, const uint16_t *stg);
void xfade_copy(uint16_t *out, const uint16_t *stg);

#endif
