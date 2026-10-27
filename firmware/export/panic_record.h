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
#ifndef _PANIC_RECORD_H_
#define _PANIC_RECORD_H_

/* ProkBox (HAVE_PANIC_RECORD): what panicf() or an ARM exception last
 * reported, kept in RAM that crt0 does not clear, for the next boot's log.
 * On the iPod Video the record sits in the PANIC_REC_RESERVE bytes just
 * below the codec buffer, taken off the end of the audio buffer
 * (core_alloc.c), and is only ever touched through its uncached alias, so
 * no dirty cache line is lost at a reset or written over it later.
 * Pure logic here (firmware/common/panic_record.c, host-tested in
 * tests/panicrec); the area and the save wrapper are in panic.c. */

#include <stdint.h>
#include <stdbool.h>

#define PANIC_REC_MAGIC   0x32434e50u   /* "PNC2": valid, not yet logged */
#define PANIC_REC_USED    0x30434e50u   /* "PNC0": logged by a later boot */
#define PANIC_REC_MSG     120           /* message, NUL included */
#define PANIC_REC_THREAD  16
#define PANIC_REC_TRACE   8             /* code addresses found on the stack */
#define PANIC_REC_TAIL    1024          /* the log's last lines, '\n' ended */
#define PANIC_REC_INFO    128           /* a hang's stacks and lock owners */
#define PANIC_REC_RESERVE 1536          /* bytes taken off the audio buffer;
                                         * its last 8 bytes: the reboot
                                         * loop guard (panic.c) */

/* kind: 0 panicf(); 1 + the UIE number (undefined instruction, prefetch
 * abort, data abort, divide by zero, SWI) for an exception; PANIC_REC_HANG
 * for the hang watcher's record (hang_watch.h), which no reset follows */
#define PANIC_REC_HANG    0x48u         /* 'H': clear of 0 and 1 + UIE */
struct panic_record
{
    uint32_t magic;
    uint32_t kind;
    uint32_t count;                 /* panics since the last logged one */
    uint32_t pc;                    /* panicf: its caller; UIE: faulting pc */
    uint32_t lr;                    /* UIE: System-mode lr; panicf: 0 */
    uint32_t sp;                    /* System-mode sp */
    char thread[PANIC_REC_THREAD];
    char msg[PANIC_REC_MSG];
    uint32_t uptime;                /* current_tick at the panic */
    uint32_t trace[PANIC_REC_TRACE];/* return-address candidates, newest
                                       first (a stack scan, not an unwind) */
    char tail[PANIC_REC_TAIL];      /* ui_log's last lines (the hook) */
    char info[PANIC_REC_INFO];      /* hang: each watched thread's stack
                                       high-water and the watched locks'
                                       owners; else empty */
    uint32_t sum;                   /* over every word before it */
};

/* Seal r again after the fields past msg were filled (panic.c). */
void panic_record_seal(struct panic_record *r);

/* Fill and seal r. count goes on from a valid record not yet logged.
 * thread and msg may be NULL; both are truncated and NUL-terminated. */
void panic_record_write(struct panic_record *r, uint32_t kind, uint32_t pc,
                        uint32_t lr, uint32_t sp, const char *thread,
                        const char *msg);

/* A valid record not yet logged: mark it logged and return it (its fields
 * stay readable), else NULL. */
const struct panic_record *panic_record_take(struct panic_record *r);

/* a valid record not yet logged (the hang watcher never writes over one) */
bool panic_record_pending(const struct panic_record *r);

/* The hang watcher's test, per watched thread, each check (pure, host
 * tested).  beats: the thread's progress count; state: HANG_ST_*; now and
 * limit in ticks; grace: extra ticks granted now (a disk spin-up).  m
 * keeps what the last check saw.  true: no progress for the limit while
 * at work (or waiting on a watched peer, which gets HANG_PEER_GRACE more,
 * so the peer, if stuck too, is named first) */
enum { HANG_ST_WORK = 0, HANG_ST_IDLE, HANG_ST_PEER };
#define HANG_PEER_GRACE_MS 2000
struct hang_mon
{
    uint32_t seen;                  /* beats at the last change */
    long since;                     /* tick of the last progress */
};
bool hang_overdue(struct hang_mon *m, uint32_t beats, unsigned int state,
                  long now, long limit, long grace, long peer_grace);

/* The last record whole, logged or not (the diagnostics report): r, or
 * NULL when it was never sealed or has been torn since. Never changes it */
const struct panic_record *panic_record_peek(const struct panic_record *r);

#if !defined(PANIC_RECORD_HOST)
/* panic.c: the record in RAM that survives a reset ([hypothesis] until the
 * device test in docs/pp5022-reference/quirks.md is run), and the save
 * that panicf() and UIE() call before they halt. */
struct panic_record *panic_record_area(void);
void panic_record_save(uint32_t kind, uint32_t pc, uint32_t lr, uint32_t sp,
                       const char *msg);
/* The shell's log fills r->tail from its RAM ring at a panic (interrupts
 * off: no locks, no yields, no file I/O). NULL: none */
extern void (*panic_record_tail_hook)(char *buf, unsigned int size);
#if defined(CONFIG_PLATFORM) && (CONFIG_PLATFORM & PLATFORM_NATIVE) && defined(CPU_ARM)
/* up to n code addresses found in the 256 words from sp, newest first
 * (out[] is left as it is past the last one found) */
void panic_stack_scan(uint32_t sp, uint32_t *out, unsigned int n);
/* the name of the thread whose stack holds sp, or NULL */
const char *panic_stack_owner(uint32_t sp);
#endif
#ifdef HAVE_IPOD_UI
/* the reboot loop guard: true while this boot is one a panic started and
 * has not yet run PANIC_GUARD_TICKS; the shell calls the clear once past
 * them */
#define PANIC_GUARD_TICKS (30 * HZ)
bool panic_guard_armed(void);
void panic_guard_clear(void);
#if defined(CONFIG_PLATFORM) && (CONFIG_PLATFORM & PLATFORM_NATIVE)
/* panicf() and UIE() end here: silence, then a restart (or, a second
 * panic soon after a panic's restart, off). Never returns */
void panic_shell_exit(void);
#endif
#endif
#ifdef SIMULATOR
/* the sim has no RAM that outlives the process: the record is kept in
 * /.rockbox/.sim_panic_record.bin on the sim's disk (--root; per run, so
 * parallel scripts never share it), read at the first panic_record_area()
 * and written back here */
void panic_record_sync(void);
#else
#define panic_record_sync() do { } while (0)
#endif
#endif

#endif /* _PANIC_RECORD_H_ */
