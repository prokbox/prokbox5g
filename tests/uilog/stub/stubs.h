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

/* host stubs for tests/uilog: the Rockbox calls apps/ipodui/uilog.c makes,
 * routed to test_uilog.c's fakes (file I/O on the host, with hooks) */
#ifndef STUB_UILOG_STUBS_H
#define STUB_UILOG_STUBS_H
#include <stdbool.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>

#define HZ 100
#define NUM_CORES 1
/* ROCKBOX_DIR comes from the compiler command line (a temp dir) */

extern long current_tick;

int  t_open(const char *path, int flags, int mode);
int  t_close(int fd);
long t_write(int fd, const void *buf, unsigned long n);
long t_filesize(int fd);
int  t_rename(const char *a, const char *b);
int  t_remove(const char *p);
#define open(p, f, m)   t_open(p, f, m)
#define close(fd)       t_close(fd)
#define write(fd, b, n) t_write(fd, b, n)
#define ffilesize(fd)   t_filesize(fd)
#define rename(a, b)    t_rename(a, b)
#define remove(p)       t_remove(p)

#define EVENT_CLASS_DISK 0x100
enum { DISK_EVENT_SPINUP = EVENT_CLASS_DISK | 1 };
bool add_event_ex(unsigned short id, bool oneshot,
                  void (*handler)(unsigned short id, void *event_data,
                                  void *user_data), void *user_data);

void t_debugf(const char *fmt, ...);
#define DEBUGF t_debugf

#define ATTRIBUTE_PRINTF(fmt, arg1) \
    __attribute__((format(printf, fmt, arg1)))
#endif
