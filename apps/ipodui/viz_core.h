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

#ifndef _IPODUI_VIZ_CORE_H_
#define _IPODUI_VIZ_CORE_H_

/* iPod UI shell: the Now Playing visualizer, the pure part (#126;
 * docs/design/p19-visualizer.md, p19-visualizer-styles.md section 2).
 * Band edges for SPECTRUM (or the CPU's jviz_bands), and the state the
 * styles doc puts on the PP: auto-gain, attack/release, the neighbour
 * ("monstercat") lift, peak hold and fall.  No Rockbox headers:
 * host-tested by tests/vizcore/.  Integers only (no FPU on the PP5022).
 *
 * Levels are jviz_l2 units (16 log2 of a power: 32 units = 6.02 dB), as
 * SPECTRUM replies them and jviz_bands() computes them.  Heights are
 * pixels from the rect's bottom, as BARS and jviz_bars_draw() take them. */

#include <stdint.h>

#define VIZ_BANDS           24      /* the v1 frame (visualizer.md 5) */
#define VIZ_BANDS_MAX       32      /* J_SPEC_BANDS_MAX */
#define VIZ_F_LO            50      /* Hz: the first band's lower edge */
#define VIZ_F_HI            16000   /* Hz: the last band's upper edge */
#define VIZ_RANGE_L2        256     /* the display range: 48 dB */
#define VIZ_GAIN_FLOOR_L2   200     /* auto-gain never reads a quieter
                                       top than this (silence stays flat) */
#define VIZ_TILT_L2         6       /* +3 units (about 0.6 dB) a band:
                                       about +1.6 dB an octave at 24 bands
                                       (the styles doc's +3 dB lit the FFT's
                                       floor on a pure tone) */
#define VIZ_PEAK_HOLD_MS    500

/* The styles, in the hold list's order (styles doc 4).  v1 draws five;
 * the rest are listed here and hidden (viz_style_shown()) until drawn. */
enum viz_style {
    VIZ_STYLE_BARS = 0,
    VIZ_STYLE_MIRROR,
    VIZ_STYLE_SCOPE,
    VIZ_STYLE_SPECTRO,              /* a scrolling heat map of the bands */
    VIZ_STYLE_RADIAL,               /* the bands as rays round a ring */
    VIZ_STYLE_COVER,
    VIZ_STYLE_AURA,
    VIZ_STYLES
};
/* bit s: style s is offered (built); Bars always.  v1 (owner,
 * 2026-10-25): Bars, Mirror, Scope, Spectrogram, Radial; Cover and Aura
 * need the cover's pixels and colours (later) */
#define VIZ_STYLES_SHOWN    ((1u << VIZ_STYLE_BARS) | (1u << VIZ_STYLE_MIRROR) \
                             | (1u << VIZ_STYLE_SCOPE)                       \
                             | (1u << VIZ_STYLE_SPECTRO)                     \
                             | (1u << VIZ_STYLE_RADIAL))
static inline int viz_style_shown(int s)
{
    return s >= 0 && s < VIZ_STYLES && ((VIZ_STYLES_SHOWN >> s) & 1);
}

/* nb + 1 band edges (bin indices, e[0] < e[1] < ... < e[nb] <= n / 2) for
 * n samples at rate Hz: log-spaced from VIZ_F_LO to min(VIZ_F_HI,
 * rate / 2), every band at least one bin wide (the low bands of a 512
 * window are one 86 Hz bin each).  nb is clamped so the bands fit the
 * bins.  Returns the band count used. */
unsigned viz_edges(uint16_t *e, unsigned nb, unsigned n, unsigned rate);

struct viz_dyn
{
    int32_t  top;                   /* auto-gain: the loudest band seen,
                                       l2 x 16, decaying */
    uint16_t level[VIZ_BANDS_MAX];  /* the shown height, px x 256 */
    uint16_t peak[VIZ_BANDS_MAX];   /* the cap, px x 256 */
    uint16_t pvel[VIZ_BANDS_MAX];   /* the cap's fall speed, px x 256 a frame */
    uint16_t hold[VIZ_BANDS_MAX];   /* ms the cap still holds */
};

void viz_dyn_init(struct viz_dyn *d);

/* One frame: nb band levels (l2; quiet true: silence, paused or no
 * fresh window, so every target is 0) into heights and peaks for a rect h
 * pixels tall (h <= 255), ms since the last frame (15 or 30 fps). */
void viz_dyn_step(struct viz_dyn *d, const uint16_t *l2, unsigned nb,
                  int quiet, unsigned h, unsigned ms, uint8_t *height,
                  uint8_t *peak);

/* The Mirror style's halves: the h rows of the rect split into the bars
 * (top part, *bars_h) and the reflection under them (*refl_h, about 30 %),
 * with a 1-row gap.  And the reflection's colour: c faded toward bg by
 * 5/8, per channel (RGB565). */
void viz_mirror_split(unsigned h, unsigned *bars_h, unsigned *refl_h);
uint16_t viz_fade565(uint16_t c, uint16_t bg);

#endif /* _IPODUI_VIZ_CORE_H_ */
