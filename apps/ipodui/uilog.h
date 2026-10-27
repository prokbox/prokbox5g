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
 * iPod UI shell: the error/splash log (Phase 8a contract,
 * docs/phase8a-brief.md §3.3, §4.5; Revision 1 H3, M3)
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
#ifndef _IPODUI_UILOG_H_
#define _IPODUI_UILOG_H_

#include <stdarg.h>
#include <stdbool.h>
#include "gcc_extensions.h"
#include "config.h"

#define UI_LOG_RING  768                    /* static; 12 x 64 B lines */
#define UI_LOG_FILE  ROCKBOX_DIR "/ipodui.log"
#define UI_LOG_MAX   16384                  /* then the generations move on */
#define UI_LOG_KEEP  5                      /* .log.1 to .log.5: older boots */
#define UI_LOG_LINE  64
/* While the bench session runs (ui_log_bench): a step holds its lines in
 * RAM (ui_log_hold), and sitting 3's video step (2026-10-26) logged 150,
 * of which the 12-line ring dropped 138.  256 lines (16 KB, core_alloc'd
 * for the session only) and a 64 KB rotation size keep a step's evidence */
#define UI_LOG_BENCH_LINES  256
/* From the first ui_log_init() on (owner's orchestrator, 2026-10-26): the
 * 12-line static ring dropped lines all through sittings 3 and 4, the
 * tail before each hang included; 64 lines (4 KB, core_alloc'd and pinned
 * once, runtime heap, not _end) hold a busy moment.  The static ring
 * stays as the fallback when the allocation fails */
#define UI_LOG_BOOT_LINES   64
#define UI_LOG_BENCH_MAX    65536

struct ui_log_line {
    unsigned short seq;                     /* writer's sequence number */
    char text[UI_LOG_LINE - 2];
};

/* Ring + DEBUGF("ipodui: <tag> <text>\n"). Any CPU-core thread; never
 * yields; COP callers are dropped and counted. A repeat with the same fmt
 * pointer within 1 s is counted, not logged (repeat=<n>, M3). */
void ui_log(const char *tag, const char *fmt, ...) ATTRIBUTE_PRINTF(2, 3);
void ui_log_va(const char *tag, const char *fmt, va_list ap);

/* H3: register_storage_idle_func() is a oneshot, so ui_log_init() calls
 * add_event_ex(DISK_EVENT_SPINUP, false, <callback>, NULL) once
 * (idempotent). The callback returns at once when nothing is pending; on
 * the storage thread (2 KB stack) it copies one line at a time into a 64 B
 * local (seq-checked, < 128 B of stack) and appends it. A line logged
 * during a flush goes out on the next one. No usb_inserted() test:
 * storage.c skips idle notifies in USB mode, and USB_POWERED must flush.
 * Device timing: fires as the disk goes to sleep, at most once per
 * spin-down cycle (about every 30 s while the disk is in use). */
void ui_log_init(void);                     /* first call: generations on, boot line */
void ui_log_flush(void);                    /* UI thread: SYS_POWEROFF */
/* The bench session (devbench.c): while a step runs, every flush waits
 * (the lines stay in the RAM ring), so nothing of the log is written when
 * its watchdog may restart the iPod; it flushes between steps */
void ui_log_hold(bool on);
/* The bench session's start (on) and end (off), UI thread on the CPU (a
 * no-op on the COP): the ring grows to UI_LOG_BENCH_LINES from the core
 * allocator and the file rotates past UI_LOG_BENCH_MAX; off flushes, puts
 * the static ring back and frees it.  No memory: the small ring stays. */
void ui_log_bench(bool on);
/* UI thread, every main-loop pass: a rotation the storage thread's flush
 * found due (past UI_LOG_MAX) runs here while the disk spins anyway, so
 * remove()/rename() never run on the 2 KB storage stack (fix round). */
void ui_log_service(void);
/* P19 S7: the CPU moves the COP's lines into the ring now (be_svc, on
 * fe_be_log_kick()); a no-op off the CPU or with one core */
#if NUM_CORES > 1
void ui_log_drain(void);
#else
static inline void ui_log_drain(void) {}
#endif
#ifdef HAVE_PANIC_RECORD
/* UI thread, at boot after ui_log_init(): a panic or exception the last
 * boot recorded (panic_record.h) becomes "panic ..." lines, is marked
 * logged and is flushed at once, while the disk still spins from booting */
void ui_log_panic_record(void);
#endif

#endif /* _IPODUI_UILOG_H_ */
