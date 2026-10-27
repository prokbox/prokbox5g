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
 * iPod UI shell: the 24 MHz idle policy (Fit-1 C6, docs/fit1-brief.md
 * section C). Tells firmware/pll_idle.c whether the current track needs
 * the PLL, judges each minute played at 24 MHz, logs the clock counters.
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
#ifndef _IPODUI_CLOCKPOL_H_
#define _IPODUI_CLOCKPOL_H_

#include <stdbool.h>
#include "config.h"

#ifdef HAVE_PP_GOV
/* The band governor's developer switches (docs/design/pp-governor.md G1
 * and section 16), read at boot by clockpol_init() in developer builds
 * only and written by Developer > Advanced > Experiments > Speed
 * governor: pgov_on turns stage 1 on; pgov2_on adds the stage-2 policy
 * as a logged shadow; pgov2_apply applies it (refused while the UI runs
 * on the second core) */
#define CLOCKPOL_PGOV_ON    ROCKBOX_DIR "/pgov_on"
#define CLOCKPOL_PGOV2_ON   ROCKBOX_DIR "/pgov2_on"
#define CLOCKPOL_PGOV2_APPLY ROCKBOX_DIR "/pgov2_apply"
#endif

#ifdef HAVE_PLL_IDLE
/* ipodui_main(), once: turns the policy on */
void clockpol_init(void);
/* ui_main.c's PLAYBACK_EVENT_TRACK_CHANGE handler (audio thread) */
void clockpol_track_changed(void);
/* The shell's main loop (UI thread): re-reads the track (a speed or DSP
 * change; at most once a second), the minute's verdict, the "clk" line */
void clockpol_service(void);
/* The developer switch (not saved: on again at the next boot) */
void clockpol_set_enabled(bool on);
bool clockpol_enabled(void);
/* p19-battery: the lit clock's developer switch, "Lit screen: 30 / 24
 * MHz" (not saved; 30 at every boot).  On: a lit screen idles at 24 MHz
 * unless the CPU draws the drift (docs/design/lit-24mhz.md).  Applied at
 * once (the bench calls it from its own loop); marks power.csv */
void clockpol_lit24_set(bool on);
bool clockpol_lit24(void);
/* np-lag: the input boost (clockpol_core.h cp_input_boost()). The main
 * loop posts note_input() on every input event (fe_note_input(); it runs
 * on the CPU), saying whether the UI's frames compete with decode for the
 * clock (ui_main.c ui_competes_with_decode(): this file is Thumb, which
 * can't read the core id); input_wait() on every
 * pass releases a hold that ran out and returns the ticks until it does
 * (TIMEOUT_BLOCK: none held); input_drop() releases at once (before a
 * handler that may block: USB, sleep, power-off) */
void clockpol_note_input(bool decode_here);
int  clockpol_input_wait(void);
void clockpol_input_drop(void);
/* boosts taken and ticks held since boot (developer probe) */
void clockpol_input_stats(unsigned long *n, unsigned long *ticks);
#else
static inline void clockpol_init(void) {}
static inline void clockpol_track_changed(void) {}
static inline void clockpol_service(void) {}
static inline void clockpol_note_input(bool decode_here)
{ (void)decode_here; }
static inline int  clockpol_input_wait(void) { return -1; /* TIMEOUT_BLOCK */ }
static inline void clockpol_input_drop(void) {}
static inline void clockpol_input_stats(unsigned long *n,
                                        unsigned long *ticks)
{ *n = 0; *ticks = 0; }
#endif

#endif /* _IPODUI_CLOCKPOL_H_ */
