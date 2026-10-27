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

#ifndef BCMFW_VIZ_VPU_H
#define BCMFW_VIZ_VPU_H

/* The visualizer's FFT and bars on the vector unit (viz_vpu.c; proposed,
 * sim only).  viz_vpu_init: the start-up check, 1 if the kernel agrees
 * with the C at both sizes (jobs.h J_VPU_FFT; fail: tests only, the check
 * expects the wrong powers).  viz_spectrum: jref's spectrum hook. */
#include <stdint.h>
#include "jviz.h"

int viz_vpu_init(int fail);
int viz_spectrum(uint32_t *p, const int16_t *pcm, uint32_t n);

/* The bars on the vector unit (proposed, sim only; viz_vpu.c, gen_viz.py
 * jv_bars16).  viz_bars: jref's bars hook (jviz.h jviz_bars_fn): rects of
 * 16 to VIZ_BARS_ROWS_MAX rows whose surface rows are 32-byte aligned with
 * a pitch of whole 16-pixel blocks, at most VIZ_BARS_CB_MAX blocks wide;
 * any other frame is the C's; JVIZ_BARS_FLIP by addressing alone (the
 * kernel unchanged).  viz_bars_init: its start-up check, 1 if five
 * frames (two flipped) equal jviz_bars_draw's (jobs.h J_VPU_BARS; fail:
 * tests only, never 1).  VIZ_BARS_CB_MAX is also the kernel's lane-array stride
 * (gen_viz.py reads it here). */
#define VIZ_BARS_CB_MAX     21      /* 16-pixel blocks: 320 pixels at any phase */
#define VIZ_BARS_ROWS_MAX   256     /* sixteen 16-row groups */

int viz_bars(uint16_t *px, uint32_t pitch, uint32_t x, uint32_t w, uint32_t h,
             const struct jviz_bars *b);
int viz_bars_init(int fail);

/* POLYLINE, SEGMENTS and PALROW's start-up check (viz_vpu.c): bit k set
 * if op k's scene (jviz.h jviz_draw_check: 0 POLYLINE, 1 SEGMENTS, 2
 * PALROW) gives its sum (fail: tests only, never a bit) */
uint32_t viz_draw_init(int fail);

#endif /* BCMFW_VIZ_VPU_H */
