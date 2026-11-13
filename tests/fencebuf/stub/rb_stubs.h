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

/* Host stand-ins for the Rockbox headers apps/buffering.c includes, for
   tests/fencebuf (#290): the real buffering.c with the COP co-location's
   remote reader (HAVE_COP_CODEC), as the iPod UI firmware builds it
   (HAVE_IPOD_UI, STORAGE_WANTS_ALIGN). Adapted from tests/video/bufshim.
   The cross-core points are hooks the test defines (fb_*): the CPU's
   cache write-back (commit_dcache), the list mutex, yield, queue replies
   and events. At each one the test lets the COP's reader run (it reads
   SDRAM, never the CPU's cache) and checks what it may see. */
#ifndef RB_STUBS_H
#define RB_STUBS_H
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define HAVE_BCMFW_VIDEO
#define HAVE_IPOD_UI
#define HAVE_COP_CODEC
#define STORAGE_WANTS_ALIGN
#define INIT_ATTR
#define NORETURN_ATTR __attribute__((noreturn))
#define SHAREDBSS_ATTR
#define IF_COP(...)
#define IF_PRIO(...)
#define DEFAULT_STACK_SIZE 4096
#define PRIORITY_BUFFERING 0
#define CREATE_THREAD_FROZEN 0
#define HZ 100
#define SYS_TIMEOUT (-1)
#define SKIPBYTES(p, count) ((typeof (p))((char *)(p) + (count)))
#define alignof __alignof__
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ALIGN_UP(n, a) (((n) + (a) - 1) / (a) * (a))
#define ALIGN_DOWN(n, a) ((n) / (a) * (a))
#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif
#define STORAGE_ALIGN_BUFFER(buf, len) do { (void)(buf); (void)(len); } while (0)
#define STORAGE_OVERLAP(x) 0
#define STORAGE_PAD(x) (x)
#define logf(...) do { } while (0)
#define DEBUGF(...) do { } while (0)
#define panicf(...) do { fprintf(stderr, __VA_ARGS__); abort(); } while (0)

struct mutex { int locked; };
struct queue_event { long id; intptr_t data; };
struct event_queue { int dummy; };
struct queue_sender_list { int dummy; };
static inline void mutex_init(struct mutex *m) { m->locked = 0; }
void fb_mutex_lock(struct mutex *m);
void fb_mutex_unlock(struct mutex *m);
#define mutex_lock(m)   fb_mutex_lock(m)
#define mutex_unlock(m) fb_mutex_unlock(m)
/* the CPU's whole-cache write-back (system.h); the test's two-cache model */
void commit_dcache(void);
void fb_yield(void);
void fb_point(const char *where);

intptr_t test_queue_send(long id, intptr_t data);
#define queue_send(q, id, ...) test_queue_send((id), (intptr_t)(__VA_ARGS__))
#define queue_post(q, id, ...) ((void)test_queue_send((id), (intptr_t)(__VA_ARGS__)))
#define queue_reply(q, r) fb_queue_reply((intptr_t)(r))
void fb_queue_reply(intptr_t r);
#define queue_init(q, r) ((void)0)
#define queue_enable_queue_send(q, s, id) ((void)0)
#define queue_empty(q) true
#define queue_wait(q, ev) ((ev)->id = SYS_TIMEOUT)
#define queue_wait_w_tmo(q, ev, t) ((ev)->id = SYS_TIMEOUT)
#define create_thread(...) 0
#define thread_thaw(id) ((void)0)
#define sleep(t) ((void)0)
#define yield() fb_yield()
#define trigger_cpu_boost() ((void)0)
#define cancel_cpu_boost() ((void)0)
#define storage_sleep() ((void)0)
#define send_event(id, data) fb_send_event((const void *)(data))
void fb_send_event(const void *data);
static inline off_t ffilesize(int fd)
{
    off_t cur = lseek(fd, 0, SEEK_CUR), end = lseek(fd, 0, SEEK_END);
    lseek(fd, cur, SEEK_SET);
    return end;
}

enum { BUFFER_EVENT_BUFFER_LOW = 1, BUFFER_EVENT_BUFFER_RESET,
       BUFFER_EVENT_FILL_DONE, BUFFER_EVENT_FINISHED,
       BUFFER_EVENT_REBUFFER };

struct bitmap { unsigned char *data; };

/* metadata: only TYPE_ID3 handles use these, and the test opens none */
struct mp3entry { int dummy; };
#define METADATA_CLOSE_FD_ON_EXIT 0
static inline bool get_metadata_ex(struct mp3entry *id3, int fd,
                                   const char *path, int flags)
{ (void)id3; (void)fd; (void)path; (void)flags; return false; }
static inline void copy_mp3entry(struct mp3entry *d, const struct mp3entry *s)
{ *d = *s; }
static inline void adjust_mp3entry(struct mp3entry *e, void *n, const void *o)
{ (void)e; (void)n; (void)o; }
#endif
