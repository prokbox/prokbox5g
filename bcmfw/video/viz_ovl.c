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

/* The visualizer's overlay (proposed, sim only; #126, jobs.h
 * J_OP_OVERLAY, docs/research/bcm2722/visualizer.md section 8): SPECTRUM
 * and BARS for the display-only image, which has no room for them
 * resident.  Linked at the image's overlay region (config.h
 * BCMFW_JOBS_OVL, build.py build_overlay) against the image's map: its
 * calls into the image (jref_f16w, jv_store16: build.py OVL_IMPORTS) are the
 * image's own addresses, and its header carries vc2cc's ABI word for
 * them, which the image checks before it runs anything here
 * (jobs_bcm.c ovl_ext).  The rest is the BCMFW_VIZ image's code
 * unchanged: jviz.c, jref_viz.c, the vector FFT and bars and their
 * start-up checks (viz_vpu.c). */
#include <stdint.h>
#include "config.h"
#include "hostproto.h"
#include "jref.h"
#include "jviz.h"
#include "viz_vpu.h"

extern char __vc2_abi[], __data_end[], __bss_start[], __bss_end[];
static uint32_t init(struct jref *j);
static uint32_t exec(struct jref *j, uint32_t op, const uint32_t *a, uint32_t n,
                     uint32_t *rep, uint32_t *nr);

/* the header, first at the base (.vectors): build.py checks its words */
const struct jref_ovl viz_ovl __attribute__((section(".vectors"))) =
{
    J_OVL_MAGIC, (uint32_t)__vc2_abi, (uint32_t)&viz_ovl, (uint32_t)__data_end,
    (uint32_t)__bss_end, init, exec
};

static int fft_on;              /* the FFT's start-up check agreed */
static int bars_on;             /* the bars' */

/* Once a load is checked: the .bss cleared (the PP writes only the load
 * image), the FFT's and the bars' start-up checks (J_VPU_FFT,
 * J_VPU_BARS), J_BARS_FLIP's bit (J_VPU_FLIP), the ops offered */
static uint32_t init(struct jref *j)
{
    uint32_t *p;

    for (p = (uint32_t *)__bss_start; p < (uint32_t *)__bss_end; p++)
        *p = 0;
    fft_on = viz_vpu_init(BCMFW_VIZ_FFT_FAIL);
    if (fft_on)
        MB(BCMFW_JOBS_BLOCK + J_VPU) |= J_VPU_FFT;
    bars_on = viz_bars_init(BCMFW_VIZ_FFT_FAIL);
    if (bars_on)
        MB(BCMFW_JOBS_BLOCK + J_VPU) |= J_VPU_BARS;
    MB(BCMFW_JOBS_BLOCK + J_VPU) |= J_VPU_FLIP;     /* BARS takes J_BARS_FLIP */
    /* POLYLINE, SEGMENTS, PALROW: each offered only if its scene sums
       right (viz_draw_init); else the PP draws that style */
    return 1u << (J_OP_SPECTRUM - 0x20) | 1u << (J_OP_BARS - 0x20) |
           viz_draw_init(BCMFW_VIZ_FFT_FAIL) << (J_OP_POLYLINE - 0x20);
}

static uint32_t exec(struct jref *j, uint32_t op, const uint32_t *a, uint32_t n,
                     uint32_t *rep, uint32_t *nr)
{
    if (op == J_OP_SPECTRUM)
        return n < J_SPEC_ARGS(1) ? HP_E_INVAL :
               jref_spectrum(j, a, n, rep, nr, fft_on ? viz_spectrum : 0);
    if (op == J_OP_BARS)
        return n < J_BARS_ARGS(1) ? HP_E_INVAL :
               jref_bars(j, a, n, bars_on ? viz_bars : 0);
    if (op == J_OP_POLYLINE)
        return n < J_POLY_ARGS ? HP_E_INVAL : jref_polyline(j, a, n);
    if (op == J_OP_SEGMENTS)
        return n < J_SEGS_ARGS(1) ? HP_E_INVAL : jref_segments(j, a, n);
    if (op == J_OP_PALROW)
        return n < J_PAL_ARGS(1) ? HP_E_INVAL : jref_palrow(j, a, n);
    return HP_E_UNKNOWN;
}
