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
    /* 0.8.3 road-test fix: the FIFO's state as each playing FIQ starts
     * (the first two after a start are not counted: the FIFO starts
     * empty). The DMA ends a piece with the I2S FIFO full; one the FIQ
     * finds empty ran dry before the next piece was started: a click */
    uint32_t fifo_empty;    /* FIQs that found the FIFO empty */
    uint32_t fifo_free_max; /* most free entries seen (16: empty) */
    uint32_t gap_max_us;    /* the longest time between two FIQs */
    /* 0.8.3 fifo96: what separates a late FIQ from a long one.  late: the
     * FIQ's entry after the hardware finished the piece before it, in us
     * (estimated from the piece's start, its length and the FIFO; with
     * the queue, from the next piece's progress); restart: entry to the
     * next piece armed; handler: entry to exit, the mix included; lock:
     * the longest PCM lock (it masks the FIQ's source) */
    uint32_t late_max_us;
    uint32_t late_over;     /* FIQs later than the FIFO lasts */
    uint32_t late_pc;       /* where the core was when the latest came */
    uint32_t late_psr;      /* (the interrupted pc and its cpsr) */
    uint32_t over_pc;       /* the same for the last FIQ over the FIFO */
    uint32_t over_psr;
    uint32_t restart_max_us;
    uint32_t handler_max_us;
    uint32_t lock_hold_max_us;
    /* the two-channel queue (pcm-pp.c, docs/pp5022-reference/dma.md 2.1):
     * dry: completions that found nothing queued (the FIFO ran out);
     * slack: the least time left on the playing piece as a FIQ ended */
    uint32_t q_pieces;
    uint32_t q_dry;
    uint32_t q_odd;         /* the queued piece ended before the older */
    uint32_t q_fell;        /* the queue given up in use: one channel */
    uint32_t q_slack_min_us;
};
/* 0.8.3 fifo96: the silent check of the two-channel queue, run once a
 * boot at the PCM's first start (pcm-pp.c).  verdict: 0 not run yet,
 * 1 proved (the queue is used), 2 refused (one channel at a time),
 * 3 inconclusive (no progress: retried at the next start, up to 3 runs).
 * Per order (0: channel 0 then 1; 1: 1 then 0): samples taken, the
 * samples where the waiting channel moved while the first was still
 * busy (overlap: it shares the request), the samples that found the
 * FIFO empty between the two (gap), whether each raised its completion
 * bit, the waiting channel's first status word and the run's time */
struct pcm_pp_qdir
{
    uint16_t samples;
    uint16_t overlap;
    uint16_t gap;
    uint8_t intr_first;
    uint8_t intr_second;
    uint32_t second_st0;
    uint32_t us;
};
struct pcm_pp_qprobe
{
    uint8_t verdict;
    uint8_t runs;
    struct pcm_pp_qdir dir[2];
};
void pcm_pp_queue_probe_result(struct pcm_pp_qprobe *out);
bool pcm_pp_queue_on(void);
/* One channel from now on (verdict refused): the PCM must be stopped */
void pcm_pp_queue_refuse(void);
/* CPU (0) or COP (1).  0 on success, -1 if the PCM is playing or the core
 * is not valid.  The CPU only (it reprograms both cores' priority) */
int pcm_pp_set_core(unsigned int core);
unsigned int pcm_pp_core(void);
void pcm_pp_get_stats(struct pcm_pp_stats *out);
/* 0.8.3: fifo_free_max, gap_max_us and the fifo96 maxima (late, the
 * pcs, restart, handler, lock, queue slack) start over */
void pcm_pp_reset_max(void);
/* 0.8.3: the longest time the cache's write-back and drop
 * (commit_discard_idcache) held the FIQ off on a core, in us, and how
 * many ran over 80 us (the I2S FIFO lasts 83 us at 96 kHz 24-bit),
 * since the last call; then start over (system-pp502x.c) */
void cache_discard_take_stats(unsigned int core, uint32_t *max_us,
                              uint32_t *over, uint32_t *n);

#endif /* HAVE_CORE_LAYOUT_A */
#endif /* CORELAYOUT_H */
