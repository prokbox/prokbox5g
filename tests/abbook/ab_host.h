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

/* Host shims for apps/ipodui/ab_store.c (AB_HOST_TEST), used by
 * test_abstore.c: POSIX files in a temp dir, a fake tick, a write()
 * that can be cut (power-cut simulation), and the P6 glue as test
 * hooks. GPL v2 or later, as the rest of Rockbox. */
#ifndef AB_HOST_H
#define AB_HOST_H

#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

struct ab_rec;
struct ab_book_block;

extern char ab_host_dat[512], ab_host_tmp[512], ab_host_old[512];
#define AB_STORE_DAT ab_host_dat
#define AB_STORE_TMP ab_host_tmp
#define AB_STORE_OLD ab_host_old

#define HZ 100
extern long current_tick;
#define UPDATE_TRACE(fmt, ...) ab_host_trace(fmt, __VA_ARGS__)
void ab_host_trace(const char *fmt, ...);

static inline unsigned int thread_self(void) { return 1; }
static inline int disable_irq_save(void) { return 0; }
static inline void restore_irq(int l) { (void)l; }
void register_storage_idle_func(void (*fn)(void));
#define AUDIO_STATUS_PAUSE 2
static inline int audio_status(void) { return 0; }  /* never paused */

/* power cut: after 'ab_host_cut' more bytes written, writes stop (the
 * write that crosses it is torn: only its first part lands) and every
 * later write fails; < 0 = no cut */
extern long ab_host_cut;
/* #91: a cut after 'ab_host_meta_cut' more renames/removes (the safe
 * replace's steps); 'ab_host_fail_remove' removes fail with EIO first */
extern long ab_host_meta_cut;
extern int ab_host_fail_remove;
ssize_t ab_host_write(int fd, const void *p, size_t n);
int ab_host_rename(const char *a, const char *b);
int ab_host_remove(const char *a);
#define write(f, p, n) ab_host_write(f, p, n)
#define rename(a, b) ab_host_rename(a, b)
#define remove(a) ab_host_remove(a)

/* glue (library.h in the target build) */
struct ab_book_block *ab_book_block(void);
_Bool ab_book_checkpoint(struct ab_rec *r, _Bool stopping);
_Bool ab_book_playing(void);
void np_pb_ab_wake(void);
int  lib_book_seed_begin(size_t bytes, void **buf);
int  lib_book_seed(struct ab_rec *out, int max);
void lib_book_seed_end(int h);

#endif
