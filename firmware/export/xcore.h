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

/* What both PP5022 cores may touch, and how (P19 S0, "19-9 step 0":
 * docs/design/p19-ui-on-cop.md sections 2.1, 3.2 and 6.3).
 *
 * The two cores share no cache coherency, with each other or with DMA, and
 * each core's maintenance acts on its own cache only (commit_dcache() on
 * the CPU never cleans a COP line: pp5022-reference quirks, bcm2722
 * quirks #52).  So:
 *
 *  1. A kernel object (mutex, semaphore, event queue, corelock, wakeup)
 *     or a flag that threads on both cores use lives in uncached memory:
 *     XCORE_BSS (zeroed) or XCORE_DATA (initialised), which are
 *     SHAREDBSS_ATTR / SHAREDDATA_ATTR (.ncbss/.ncdata, linked at the
 *     uncached alias) when HAVE_COP_COHERENCE is set, and nothing
 *     otherwise (the stock build's bytes stay upstream's).  IRAM is
 *     uncached too (lcd_state).  scripts/xcore_map_check.py fails the gate
 *     if a listed object is linked anywhere else.
 *  2. A word that both cores write takes a corelock (uncached is not
 *     atomic); better, one writer per word.
 *  3. A buffer handed across cores, or to a DMA engine (the pixel hand-off
 *     contract, design section 6.3):
 *       - the PRODUCER cleans its own cache after writing and before the
 *         hand-off (xcore_publish());
 *       - a CONSUMER on the other core reads it through the uncached
 *         alias (UNCACHED_ADDR), or drops its own cache first
 *         (xcore_acquire()) so no stale line of its own is read;
 *       - both ends are XCORE_LINE (16-byte, one cache line) aligned, so
 *         no line is shared with an unrelated neighbour whose eviction
 *         would write stale bytes over the hand-off (xcore_aligned()).
 *     A producer that will let DMA write the buffer cleans BEFORE the
 *     hand-off too: a dirty line evicted later would overwrite the DMA's
 *     data.
 *  4. buflib (core_alloc, core_free, core_pin, core_unpin, core_shrink),
 *     the tick-task list (tick_add_task/tick_remove_task) and the event
 *     table are CPU-only: buflib has no lock and compacts on the CPU, and
 *     the tick list masks only the caller's interrupts.  XCORE_ASSERT_CPU()
 *     marks those entry points: a panic in DEBUG builds, a counter
 *     (xcore_offcore, the COP is its only writer) in developer builds,
 *     nothing in release.
 *
 * On one core (the simulator, FORCE_SINGLE_CORE) every helper is free. */

#ifndef XCORE_H
#define XCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "system.h"

#ifdef HAVE_COP_COHERENCE
#define XCORE_BSS   SHAREDBSS_ATTR
#define XCORE_DATA  SHAREDDATA_ATTR
#else
#define XCORE_BSS
#define XCORE_DATA
#endif

#ifdef HAVE_CPU_CACHE_ALIGN
#define XCORE_LINE  CACHEALIGN_SIZE     /* 16 on the PP5022 */
#else
#define XCORE_LINE  16
#endif

/* Both ends of a hand-off buffer on a cache line */
static inline bool xcore_aligned(const void *p, size_t bytes)
{
    return !(((uintptr_t)p | bytes) & (XCORE_LINE - 1));
}

/* The producer, after writing a buffer another core or a DMA engine will
 * read (or a DMA engine will write): this core's whole data cache is
 * written back (the PP5022 has no ranged clean) */
static inline void xcore_publish(void)
{
#if defined(CPU_PP) && (CONFIG_PLATFORM & PLATFORM_NATIVE)
    commit_dcache();
#endif
}

/* A consumer on the other core, before reading a published buffer through
 * its cached address: this core's data cache is written back and dropped,
 * so no stale line of its own is read.  Not needed for uncached reads. */
static inline void xcore_acquire(void)
{
#if NUM_CORES > 1 && defined(CPU_PP) && (CONFIG_PLATFORM & PLATFORM_NATIVE)
    commit_discard_dcache();
#endif
}

#if NUM_CORES > 1 && defined(HAVE_COP_COHERENCE) && \
    (defined(DEBUG) || defined(HAVE_IPOD_UI_DEVMODE))
#define XCORE_CHECKS
/* calls of a CPU-only entry point from the COP (developer builds) */
extern volatile unsigned xcore_offcore;
#ifdef DEBUG
#include "panic.h"
#define XCORE_ASSERT_CPU(what) \
    do { if (CURRENT_CORE != CPU) panicf("%s off the CPU", (what)); } while (0)
#else
#define XCORE_ASSERT_CPU(what) \
    do { if (CURRENT_CORE != CPU) xcore_offcore++; } while (0)
#endif
#else
#define XCORE_ASSERT_CPU(what) do { } while (0)
#endif

/* ---- The pixel hand-off (BCM MF-4; design section 6.3) ------------------
 *
 * The framebuffer (lcd_static_framebuffer: cached .bss, 16-byte aligned,
 * 640-byte rows, on cache lines of its own) has one PRODUCER: the core of
 * the thread that draws the shell (the ui thread, or main inline).  It
 * claims the frame once, on its own thread, before it draws:
 * lcd_fb_claim().  A CONSUMER is any code on the other core that reads the
 * frame's pixels and pushes them to the BCM (by the CPU's port copy,
 * engine 2 or a PP DMA): lcd_update_rect() from a CPU thread (the wake
 * frame in lcd_awake, the devtest's hand-back) and vplayer's overlay.
 * For them:
 *   - lcd_fb_cross(): true on a core other than the producer's.  With the
 *     shell on the CPU (inline or the CPU thread) it is false everywhere
 *     that matters and nothing below runs: one uncached word read and the
 *     core id per push, in shell firmware builds only;
 *   - lcd_fb_fence(x, y, w, h): the consumer asks the producer to write
 *     back the rect's lines (rows x pitch, rounded out to cache lines:
 *     lcd_fb_lines()) through the shell's fe_fb_publish() (fe_fb.h:
 *     bounded, FE_FB_TIMEOUT_MS; CPU threads only, never with interrupts
 *     off) and returns its fe_fb_result.  Never under the port lock: the
 *     ui thread may be waiting for it.  A timeout (a modal loop on the ui
 *     thread that does not serve the fence, a hung COP) is counted
 *     (lcd_fb_fence_fails) and the push goes on: stale pixels at worst;
 *   - lcd_fb_read(x, y): the address of pixel (x, y) to read, through the
 *     uncached alias of the default frame (the CPU then never sees a stale
 *     line of its own).  The consumer's own cache is written back first, so
 *     anything it drew itself is in SDRAM too;
 *   - lcd_fb_publish(x, y, w, h): the producer, before it hands a rect to
 *     a CPU consumer by a message it then waits on (vplayer's overlay: the
 *     ui thread sleeps until the CPU is through, so it cannot answer a
 *     fence).  A no-op unless the producer is on the COP;
 *   - lcd_fb_take(): a consumer that will draw into the frame itself from
 *     now on: fence, drop its own lines, become the producer.  No caller on
 *     the iPod Video today: power_off()'s clear-and-push (power-ipod.c) is
 *     compiled out by HAVE_LCD_SHUTDOWN, and the shell build draws no
 *     panic screen (HAVE_PANIC_RECORD).
 *
 * Edge lines: the frame's rows are whole cache lines (640 % 16 == 0, base
 * aligned), so a full-width rect (the only shape engine 2 pushes) is line
 * aligned at both ends (e2-port.h's contract).  A narrower rect's first
 * and last lines carry neighbouring pixels: harmless, since the consumer
 * only reads them, through the uncached alias, and the producer's
 * write-back is the whole cache anyway (the PP5022 has no ranged clean).
 *
 * HAVE_FE_FB_HANDOFF (config/ipodvideo.h, every shell firmware build under
 * HAVE_COP_COHERENCE, release included since p19-relsplit): this is built;
 * the shell's fe_cop_refusal() keys on it.  The simulator and the stock
 * build compile none of it. */
#ifdef HAVE_FE_FB_HANDOFF
#if NUM_CORES < 2 || !(CONFIG_PLATFORM & PLATFORM_NATIVE) || defined(BOOTLOADER)
#error "HAVE_FE_FB_HANDOFF: the two-core target firmware only"
#endif
void lcd_fb_claim(void);
bool lcd_fb_cross(void);
void lcd_fb_lines(int x, int y, int w, int h,
                  uintptr_t *start, uint32_t *bytes);
int lcd_fb_fence(int x, int y, int w, int h);   /* enum fe_fb_result */
const void *lcd_fb_read(int x, int y);
void lcd_fb_publish(int x, int y, int w, int h);
void lcd_fb_take(void);
/* fences that timed out (a developer counter) */
extern volatile unsigned lcd_fb_fence_fails;
#else
#define lcd_fb_claim()              do { } while (0)
#define lcd_fb_cross()              false
#define lcd_fb_publish(x, y, w, h)  do { } while (0)
#define lcd_fb_take()               do { } while (0)
#endif

#endif /* XCORE_H */
