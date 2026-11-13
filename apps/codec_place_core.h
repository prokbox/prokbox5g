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
 * Placement by screen (a developer A/B arm: the owner pinned decoding to
 * the CPU on 2026-10-24, then put it on the COP for every build on
 * 2026-11-07, CPL_MODE_COP). Dark (the backlight off and the panel asleep)
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
#include <stdint.h>

/* The screen state, as the shell drives it */
enum cpl_screen {
    CPL_LIT = 0,    /* the backlight is on */
    CPL_DIM,        /* the backlight is off, the panel still awake */
    CPL_DARK,       /* the backlight is off and the panel asleep */
};

enum cpl_core { CPL_COP = 0, CPL_CPU };

/* The developer switch (codec_cop.c; M-2's arms). Owner, 2026-11-07
 * (Apple's core layout, superseding 2026-10-24's pin to the CPU):
 * CPL_MODE_COP is the default in every build; the other two exist only
 * for the A/B (CPL_MODE_CPU is the plain path) */
enum cpl_mode {
    CPL_MODE_SCREEN = 0,    /* A/B: by the screen state */
    CPL_MODE_COP,           /* the default: Fit-2, always on the COP */
    CPL_MODE_CPU,           /* A/B: always on the CPU, the plain path */
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

/* ---- The COP's clock demand (p083-copclock; docs/hardware.md "The clock:
 * the larger of both cores' needs", owner 2026-11-11) ----------------------
 * The decode and its DSP run on the COP while the CPU idles, so the CPU's
 * own boost requests no longer see the music's load. The COP measures
 * its busy time per block (decode + DSP, wall time less its waits: also
 * whatever else ran on the COP meanwhile, the PCM FIQ among it), times
 * the clock it ran at: cycles. Per period of at least CPL_DM_PERIOD_US of
 * OUTPUT audio (after resampling and time-stretch), the demand is
 * cycles / audio us = the MHz this music needs, whatever the clock was
 * (Apple's governor's busy share times clock). A smoothed demand at or
 * over CPL_DM_HOLD_X10 (or one period busy CPL_DM_SAT_PCT % or more of
 * its own clock: starving, at 24 MHz as at 30) asks for the boost; it is
 * let go under CPL_DM_FREE_X10 after
 * at least CPL_DM_MIN_HOLD periods. Units: tenths of a MHz.
 *
 * Why these numbers: at 30 MHz the whole COP is the music's (the UI is on
 * the CPU), so 30.0 is 100 % busy and starves. 21.0 is 70 %: the pcmbuf
 * (2 s watermark) absorbs a block's peaks (Opus: worst 48.9 ms against a
 * 20 ms block, device log 19306ce49a), not a mean that high. The light
 * formats stay under it: 16/44.1 FLAC and MP3 (estimate 15-19 MHz with
 * the DSP, clockpol.c). 15.0 to let go: a demand measured at 80 MHz is
 * never lower than at 30 for the same music (SDRAM stalls cost more
 * cycles at the higher clock), so a held track cannot fall under it by
 * the clock change alone: no flapping. The measurement is the device's
 * own time, so the emulator's ~1.4x speed moves only what it reads
 * (about 1/1.4 of silicon), not the decision on silicon. */
#define CPL_DM_PERIOD_US    1000000u
#define CPL_DM_HOLD_X10     210u
#define CPL_DM_FREE_X10     150u
#define CPL_DM_SAT_PCT      90u  /* busy share of the block's own clock */
#define CPL_DM_MIN_HOLD     10u

struct cpl_demand {
    unsigned int ema_x10;   /* the smoothed demand */
    unsigned int last_x10;  /* the last period's */
    unsigned int held;      /* periods since the boost was asked for */
    bool want;              /* the boost is asked for */
    bool seeded;            /* ema_x10 holds a period */
};

/* No demand, no boost asked for */
void cpl_demand_init(struct cpl_demand *d);
/* A new run (a track): its first period seeds the average and decides
 * afresh against the hold line; the last run's answer is not kept (a
 * light album after a heavy track lets go at its first second) */
void cpl_demand_restart(struct cpl_demand *d);
/* One period: cycles (MHz x us busy) and busy_us (us busy) over out_us
 * of output audio. Returns true when want changed */
bool cpl_demand_period(struct cpl_demand *d, uint32_t cycles,
                       uint32_t busy_us, uint32_t out_us);

#endif /* CODEC_PLACE_CORE_H */
