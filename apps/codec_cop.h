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

#endif /* HAVE_COP_CODEC */
#endif /* _CODEC_COP_H_ */
