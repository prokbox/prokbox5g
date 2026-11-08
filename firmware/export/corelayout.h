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

/* Core layout (A), the first steps (docs/design/core-layout-decision.md
 * section 7, steps 0b, 1 and 2): Apple's layout puts the audio pipeline on
 * the COP and keeps the UI, the file system, storage, USB and the BCM
 * driver on the CPU.  Developer builds only (HAVE_CORE_LAYOUT_A,
 * ipodvideo-variant.h); each switch defaults to today's behaviour and is
 * thrown by a bench row (apps/ipodui/devbench_core.c), which puts it back.
 *
 *  0b  crt0-pp.S gives the COP a FIQ stack of its own; pcm-pp.c's FIQ
 *      vector target dispatches on the core that took the FIQ (Apple's
 *      form: its FIQ stub reads PROCESSOR_ID and branches, osos 1.3
 *      0x1000_015C).  Always on in a developer build: no switch.
 *  1   K-1: corelayout_set_park(true) stops the tick's pulse to the COP;
 *      the tick wakes it only for a due timeout (cop_park.h).
 *  2   pcm_pp_set_core(COP) moves the PCM DMA's interrupt (source 26) to
 *      the COP's FIQ; the PCM lock then masks the COP's source and waits
 *      out a FIQ in flight there.  Only while the PCM is stopped. */

#ifndef CORELAYOUT_H
#define CORELAYOUT_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#ifdef HAVE_CORE_LAYOUT_A

/* ---- K-1 (kernel-pp.c, thread-pp.c) ---- */
void corelayout_set_park(bool on);
bool corelayout_park(void);
/* The CPU's tick: pulses sent to the COP, and pulses held back (parked) */
void corelayout_park_stats(uint32_t *sent, uint32_t *held);
/* The COP's next due timeout check (thread-pp.c; the COP is its only
 * writer, in IRAM) */
long cop_next_tmo_check(void);

/* ---- Step 2 (pcm-pp.c) ---- */
struct pcm_pp_stats
{
    uint32_t fiq[2];        /* fiq_playback entries, by the core that ran it */
    uint32_t spurious[2];   /* FIQs with no target on that core (masked) */
    uint32_t held_off;      /* FIQs that found the source masked (a lock) */
    uint32_t lock_waits;    /* cross-core locks that waited out a FIQ */
    uint32_t lock_spin_max; /* the longest such wait, in polls */
    uint32_t lock_stuck;    /* waits that gave up (a FIQ never ended) */
    uint32_t lock_xcore;    /* locks taken from the core that does not
                               take the FIQ (mask, then wait out) */
};
/* CPU (0) or COP (1).  0 on success, -1 if the PCM is playing or the core
 * is not valid.  The CPU only (it reprograms both cores' priority) */
int pcm_pp_set_core(unsigned int core);
unsigned int pcm_pp_core(void);
void pcm_pp_get_stats(struct pcm_pp_stats *out);

#endif /* HAVE_CORE_LAYOUT_A */
#endif /* CORELAYOUT_H */
