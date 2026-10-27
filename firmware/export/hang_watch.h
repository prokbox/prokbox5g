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

#ifndef _HANG_WATCH_H_
#define _HANG_WATCH_H_

/* ProkBox (HAVE_HANG_RECORD): the hang watcher.  A frozen unit leaves no
 * panic record (nothing faults), so a tick task watches a few threads'
 * progress counts and, when one at work makes no progress for its bound,
 * writes a PANIC_REC_HANG record (panic_record.h) once: the thread, its
 * pc/lr/sp, a stack scan, each watched thread's stack high-water, the
 * watched locks' owners and the log's tail.  No reset and no halt: the
 * owner's MENU + SELECT keeps the record for the next boot's log.  A
 * thread that gets going again was not hung for good: the shell logs the
 * record at once ("recovered") and the watcher arms again.
 *
 * Each watched thread's own code bumps its count (hang_beat()) once a
 * pass and is idle, never late, while it waits for work (hang_idle_*()
 * around its blocking queue wait; button_get_w_tmo() does it for any
 * watched thread).  Writes come from each slot's own thread only; the
 * slots are in uncached shared memory (XCORE_BSS), so the shell's thread
 * on the COP is watched too.  The watcher runs on the CPU tick. */

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

enum hang_slot
{
    HANG_UI = 0,        /* the thread running the shell's loop */
    HANG_BE,            /* be_svc (only while the UI split is on) */
    HANG_ART,           /* the art builder */
    HANG_STOR,          /* the storage thread (its idle callbacks write
                         * the settings, the logs, the resume records) */
    HANG_SLOTS
};

/* the locks whose owner a record names (hang_watch_mutex()) */
enum hang_mx
{
    HANG_MX_ATA = 0,
    HANG_MX_I2C,
    HANG_MX_N
};

/* what suspends the whole watcher (each flag has one writer) */
enum hang_susp
{
    HANG_SUSP_USB = 0,  /* the USB screen: the host owns the disk */
    HANG_SUSP_OFF,      /* a shutdown under way */
    HANG_SUSP_N
};

#ifdef HAVE_HANG_RECORD
struct thread_entry;
struct mutex;

struct hang_slot_rec
{
    volatile uint32_t beats;        /* the thread's own progress count */
    volatile uint8_t state;         /* HANG_ST_* (panic_record.h) */
    volatile uint8_t paused;        /* the thread's own: not watched now */
    struct thread_entry *volatile t;/* NULL: not watched */
};
extern struct hang_slot_rec hang_slots[HANG_SLOTS];

/* the thread itself, once, before its loop: watch it from now */
void hang_watch_register(enum hang_slot s);
/* the thread itself, each pass: progress */
static inline void hang_beat(enum hang_slot s)
{
    hang_slots[s].beats++;
}
/* the calling thread, if watched, waits for work: idle until the end.
 * Returns what the end restores (-1: not a watched thread) */
int hang_idle_begin(void);
void hang_idle_end(int prev);
/* a watched thread waits on another watched one (the shell on be_svc):
 * HANG_PEER_GRACE_MS more, so the stuck peer is the one named */
int hang_peer_begin(void);
/* the slot's own thread: not watched while on (a bench step's own long
 * work under its own watchdog) */
void hang_pause(enum hang_slot s, bool on);
/* nothing watched while any reason holds it on */
void hang_suspend(enum hang_susp why, bool on);
/* the running bench step's id for the record (devbench.c); NULL: none */
extern const char *volatile hang_bench_step;
/* a lock whose owner the record names (its driver's init) */
void hang_watch_mutex(enum hang_mx i, struct mutex *m);
/* the shell's service pass: the recorded thread is going again; true once
 * per record, with how long it was stuck (seconds) and its slot's name */
bool hang_take_recovered(long *secs, const char **name);
/* after the shell logged a recovered record: watch for the next one */
void hang_rearm(void);
#else
#define hang_watch_register(s)  do { } while (0)
#define hang_beat(s)            do { } while (0)
#define hang_idle_begin()       (-1)
#define hang_idle_end(p)        do { (void)(p); } while (0)
#define hang_peer_begin()       (-1)
#define hang_pause(s, on)       do { } while (0)
#define hang_suspend(w, on)     do { } while (0)
#define hang_watch_mutex(i, m)  do { } while (0)
#endif

#endif /* _HANG_WATCH_H_ */
