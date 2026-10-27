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
 * iPod UI shell: the blurred-cover fill behind a fitted thumbnail
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
#ifndef _IPODUI_ARTBLUR_H_
#define _IPODUI_ARTBLUR_H_

#include <stdint.h>
#include <stddef.h>

/* Box-blur radii of the three passes per axis: together ~ a Gaussian of
 * sigma 5.8 px (owner mock-up: radius ~6 at 55 px), and the darkening of
 * the result, /256 (~60%). */
#define ARTBLUR_R0      5
#define ARTBLUR_R1      6
#define ARTBLUR_DARK    154

/* Scratch artblur_fill() needs for a W x H slot (bytes). */
#define ARTBLUR_SCRATCH(W, H) \
    (3 * (size_t)(W) * (H) + 2 * ((size_t)((W) > (H) ? (W) : (H)) \
                                  + 2 * ARTBLUR_R1 + 1))

/* The bars of dst (W x H, RGB565) around the centred spot of src (sw x sh,
 * RGB565, the fitted art: sw == W or sh == H) = src stretched to W x H,
 * blurred (3 box passes a side, edges clamped), darkened and dithered;
 * the art's own spot is left for the caller to copy src into. Integer
 * maths, two divides per call, none per pixel. 'scratch' holds
 * ARTBLUR_SCRATCH(W, H) bytes and must not overlap src or dst. Build
 * time only (the art builder), never while drawing. 'pause' (may be NULL)
 * runs between the passes: the builder yields and may park there. */
void artblur_fill(uint16_t *dst, int W, int H,
                  const uint16_t *src, int sw, int sh, uint8_t *scratch,
                  void (*pause)(void));

#endif /* _IPODUI_ARTBLUR_H_ */
