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

/* What jpeg_load.o, resize.o and bmp.o (the simulator's) need from the
 * rest of Rockbox, for the S1 reference: plain POSIX files, and aborts
 * for the album-art paths a file never takes. */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int sim_open(const char *p, int f, ...) { return open(p, f); }
ssize_t sim_read(int fd, void *b, size_t n) { return read(fd, b, n); }
off_t sim_lseek(int fd, off_t o, int w) { return lseek(fd, o, w); }
int sim_close(int fd) { return close(fd); }
off_t sim_ffilesize(int fd)
{
    struct stat st;

    return fstat(fd, &st) ? -1 : st.st_size;
}
void yield(void) { }
void *thread_self(void) { return NULL; }
void debugf(const char *fmt, ...) { (void)fmt; }
#define NEVER(name) void name(void); void name(void) { abort(); }
NEVER(base64_decode)
NEVER(get_ogg_format_and_move_to_comments)
NEVER(id3_unsynchronize)
NEVER(ogg_file_init)
NEVER(ogg_file_read)
