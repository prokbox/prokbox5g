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
 * ProkBox (Fit-2, docs/fit2-brief.md section 5): the codec's decode on the
 * second core, the first client of firmware/export/cop_job.h. On by default
 * (owner, 2026-10-18); the developer switch turns it off until the next
 * boot. A change takes effect at the next codec load.
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
#ifndef _CODEC_COP_H_
#define _CODEC_COP_H_

#include "config.h"

#ifdef HAVE_COP_CODEC
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct codec_api;

/* The developer switch (not saved; on again at every boot) */
void codec_cop_set_enabled(bool on);
bool codec_cop_enabled(void);
/* A second core can take the codec (false in the simulator) */
bool codec_cop_available(void);
/* This run decodes on the second core */
bool codec_cop_active(void);
/* Counters since boot: decode runs on the COP, calls back to the CPU and
 * the time the COP waited for them */
void codec_cop_stats(uint32_t *runs, uint32_t *calls, uint32_t *wait_ms);

/* P19 19-1c: which core decodes (enum cpl_mode, codec_place_core.h): by
 * the screen state (the default), always the COP (Fit-2), or always the
 * CPU. Not saved; the developer row keeps a marker file for M-2. Takes
 * effect at the next run (a track boundary, under the 30 s hold) */
void codec_cop_set_place(int mode);
int  codec_cop_place(void);
/* codec_thread.c: an observation of the screen state, from wakes the
 * codec thread has anyway (its full-buffer sleep, a run's start) */
void codec_cop_observe(void);
/* Runs since boot: on the COP, and on the CPU */
void codec_cop_place_stats(uint32_t *cop, uint32_t *cpu);

/* p083-copclock: the COP's clock demand (codec_place_core.h). True while
 * the music on the COP asks for the boost; the smoothed and the last
 * period's demand in tenths of a MHz (either pointer may be NULL). Any
 * thread; the values are the COP's, read uncached */
bool codec_cop_demand(unsigned int *ema_x10, unsigned int *last_x10);
/* The function the CPU's tick calls when the demand's want changes (the
 * UI's wake; must be callable from the tick: a queue post) */
void codec_cop_set_demand_wake(void (*wake)(void));
#ifdef SIMULATOR
/* sim command "cop_demand": one period at x10 tenths of a MHz */
void codec_cop_sim_demand(unsigned int x10);
#endif

/* codec_thread.c, at Q_CODEC_LOAD: the api to hand the codec (the real
 * one, or the second core's view of it, whenever the COP could take a
 * run of this load) */
struct codec_api *codec_cop_select(struct codec_api *real, bool encoder);
/* codec_thread.c: runs the loaded codec on the core this run's choice
 * gives (track_start false: a seek's re-run, which keeps its core) */
int  codec_cop_run(bool track_start);
/* codec_thread.c, after codec_close() or a failed load */
void codec_cop_unload(void);
/* codecs.c: bytes kept back at the top of the codec slot (the worker's
 * stack), whenever this load is bound to the COP's view */
size_t codec_cop_reserve(void);
/* codecs.c */
void *codec_slot_top(size_t size);
/* codec_thread.c: no message waits for the codec */
bool codec_queue_idle(void);
/* 0.8.3: us codec_pcm_window() slept on a full buffer, since boot */
uint32_t codec_pcm_window_full_us(void);
/* codec_thread.c (#282 step 4): the CPU's half of the COP's insert:
 * publish `used` samples the COP wrote, then take a window of at least
 * `want` samples (0: publish only). NULL: a message waits, stop.
 * *hold: the window may be held across frames (no crossfade).
 * 0.8.3 colocate: with may_lease, after a full-buffer sleep it may hand
 * the write side to the COP instead (pcmbuf.h's lease): NULL, *leased */
void *codec_pcm_window(int used, unsigned long elapsed, off_t offset,
                       int want, bool may_lease, int *count, bool *hold,
                       bool *leased);
/* #282 step 4: the DSP runs on the COP with the decode (developer A/B
 * switch, not saved; on at every boot) */
void codec_cop_set_dsp(bool on);
bool codec_cop_dsp(void);
/* Counters since boot of the DSP on the COP: frames it ran, windows
 * (calls to the CPU to publish), microseconds inside dsp_process() */
void codec_cop_dsp_stats(uint32_t *frames, uint32_t *windows,
                         uint32_t *dsp_us);
/* 0.8.3 colocate: the COP reads the compressed ring itself (no call per
 * request_buffer/advance_buffer/set_elapsed). Developer A/B switch, not
 * saved, on at every boot; off says "COP FAILED (colocate off)" */
void codec_cop_set_colocate(bool on);
bool codec_cop_colocate(void);

#endif /* HAVE_COP_CODEC */
#endif /* _CODEC_COP_H_ */
