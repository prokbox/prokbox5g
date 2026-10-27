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

#ifndef _IPODUI_VIZ_H_
#define _IPODUI_VIZ_H_

/* iPod UI shell: the Now Playing visualizer view (#126;
 * docs/design/p19-visualizer.md).  UI thread only.
 *
 * Who does what (docs/research/bcm2722/visualizer.md section 2):
 *  - PP: the PCM tap (viz_tap.c), the split-order window, the smoothing,
 *    gain and peaks (viz_core.c);
 *  - BCM2722, whenever the image offers the ops (J_OPS bits 19, 20): the
 *    FFT and bands (SPECTRUM) and the bars (BARS), presented as a rect the
 *    BCM owns (bcmjob_owned_set) while the view shows;
 *  - the CPU path, only when they are missing (the merged image, an older
 *    image, a refused overlay, a job failure): jviz_power_ref, jviz_bands
 *    and jviz_bars_draw on the PP, the same levels and pixels, unboosted at
 *    256 samples and 15 fps.  Logged loudly once per open ("viz CPU
 *    FALLBACK why=...").
 *
 * Memory: the tap's ring and the view's state from the screen arena
 * (Now Playing's part of it, taken at the first visualizer frame of a
 * push and released when Now Playing leaves: never a core_alloc beside
 * playback); the per-frame samples and powers in Now Playing's strip
 * (np_layout.h NP_STRIP_VIZ_OFF), which a slide may overwrite between
 * frames.
 *
 * The screen stays on while the view shows: the backlight timeouts are
 * held at "always on" and the idle timer is reset, restored when the view
 * stops (viz_stop()). */

#include <stdbool.h>

/* The bars' rect: the whole panel (owner, 2026-10-25: the visualizer is
 * full screen, no header, no title, no bar or counters).  The 24 bars,
 * 309 px, are centred in the full width; 240 rows (fifteen 16-row groups
 * for the BCM's vector bars).  x 0 and 640-byte rows: every row of the
 * rect starts on a 32-byte boundary of the front frame, Apple's
 * convention for the chip's surfaces (memory-access.md 11.5; owner rule
 * 2026-10-25: where the chip's behaviour is unproven, do what Apple's
 * firmware does) */
#define VIZ_X           0
#define VIZ_Y           0
#define VIZ_W           320
#define VIZ_H           240

/* The view starts (entering the Visualizer view): false if it can't (no
 * arena room, an audiobook): the caller leaves the view */
bool viz_start(void);
/* The view stops (another view, dark, a slide): the tap closes, the
 * BCM's rect is handed back (FENCE waited), the backlight timeouts are
 * restored.  The arena memory stays until viz_forget(). */
void viz_stop(void);
/* Now Playing leaves: viz_stop(), and the arena pointer is dropped (the
 * stack resets the arena after leave()) */
void viz_forget(void);
bool viz_on(void);
/* The view's background colour (RGB565, the dark theme), for Now
 * Playing's volume overlay over the bars */
unsigned viz_bg(void);
/* The view's background (the whole panel) after a full body paint */
void viz_paint_bg(void);
/* The rows the bars may draw, from the top: VIZ_H, or less while Now
 * Playing's volume overlay covers the panel's foot (its rows are the
 * CPU's: a smaller BCM rect is owned at once, its last PRESENT waited).
 * The bars keep their full-height geometry and colours; only the rows
 * under the clip go unpainted. */
void viz_clip(int h);
/* One frame if one is due: ticks until the next.  Its scratch is Now
 * Playing's strip (NP_STRIP_VIZ_OFF): '*lost' (the caller's, never
 * cleared here) is set when the claim found the strip taken by another
 * owner since Now Playing's last claim (a push's capture, the hold
 * list), so the caller rebuilds its parts there (the title's marquee,
 * the lyrics) before it uses them again */
int viz_frame(bool *lost);

/* The style (settings: ipodui_viz_style), one of viz_core.h's shown
 * styles; the hold list (long SELECT in the view) */
int viz_style(void);
#include "ipodui/screen_stack.h"
extern const struct ui_screen_ops ui_viz_style_screen;

/* The charger face (the hold list's While Charging: Clock or Visualizer;
 * ipodui_charge_face): true if the Visualizer face applies now (on a
 * charger, music playing, not a book, the setting on) */
bool viz_face_wanted(void);

#endif /* _IPODUI_VIZ_H_ */
