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

/* P19 19-1c, codec placement by screen state: which core decodes each
 * run, and how long the codec sleeps on a full PCM buffer (option B + D,
 * docs/design/p19-placement.md section 4). Pure logic, no Rockbox calls
 * (host test: tests/codecplace/). Times are in ticks.
 *
 * Placement (a developer A/B mode since the owner pinned decoding to the
 * CPU, 2026-10-24). Dark (the backlight off and the panel asleep)
 * decodes on the CPU with the COP idle; lit decodes on the COP. The
 * codec stays loaded across tracks of one type, so the choice is made at
 * three points: the first run after a load (playback start, a codec
 * change), and each track boundary (a new run with no gap). A boundary
 * moves the decode only when the screen state has held CPL_HOLD_S there.
 * Never mid-track (a seek re-runs the codec with the core it had), and
 * never by reloading the codec. Between lit and dark (the backlight off,
 * the panel still awake) nothing moves.
 *
 * D. A full PCM buffer used to be polled every HZ/20. The playback FIQ
 * fills the DAC and may not post to a queue, but the buffer drains at the
 * output rate, so the time until it reaches a level is known: the codec
 * sleeps until the level is down to the low watermark (above the boost
 * watermark, so a refill is no rescue), and a message still ends the
 * sleep at once. */
#ifndef CODEC_PLACE_CORE_H
#define CODEC_PLACE_CORE_H

#include <stdbool.h>
#include <stddef.h>

/* The screen state, as the shell drives it */
enum cpl_screen {
    CPL_LIT = 0,    /* the backlight is on */
    CPL_DIM,        /* the backlight is off, the panel still awake */
    CPL_DARK,       /* the backlight is off and the panel asleep */
};

enum cpl_core { CPL_COP = 0, CPL_CPU };

/* The developer switch (codec_cop.c; M-2's arms). The owner pinned
 * decoding to the CPU (2026-10-24): CPL_MODE_CPU is the default and the
 * plain path; the other two exist only for the A/B */
enum cpl_mode {
    CPL_MODE_SCREEN = 0,    /* A/B: by the screen state */
    CPL_MODE_COP,           /* A/B: Fit-2, always on the COP */
    CPL_MODE_CPU,           /* the default: always on the CPU */
};

#define CPL_HOLD_S      30  /* the screen state's hold for a boundary */

struct cpl_state {
    int screen;             /* the last state observed */
    long since;             /* when it was first observed */
    bool seen;              /* screen and since are valid */
    int core;               /* the last choice (enum cpl_core) */
};

/* Clears the state: no observation, the COP as the last choice */
void cpl_init(struct cpl_state *s);
/* An observation of the screen state at 'now' */
void cpl_observe(struct cpl_state *s, int screen, long now);
/* The core for the first run after a load: by the state now, no hold */
int cpl_at_load(struct cpl_state *s, int mode, long now);
/* The core for a run at a track boundary: moves only when the state has
 * held hold_ticks (CPL_HOLD_S * HZ) */
int cpl_at_boundary(struct cpl_state *s, int mode, long now,
                    long hold_ticks);
/* May the codec put helper work (MP3's synthesis) on the COP for a run
 * on 'core'? Only when the decode itself is there: a run on the CPU
 * keeps to one core */
bool cpl_cop_assist(int core);

/* D's developer fallback: true = the old HZ/20 poll. Default false. */
extern bool cpl_full_legacy;
/* The codec's sleep on a full buffer. level: unplayed bytes; wm: the
 * boost watermark; byterate: output bytes a second. At least 1 tick and
 * at most hz; the old HZ/20 when the band above wm is too thin (or the
 * fallback is on). */
long cpl_full_wait(size_t level, size_t wm, size_t byterate, long hz);

#endif /* CODEC_PLACE_CORE_H */
