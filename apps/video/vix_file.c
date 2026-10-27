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

/* File glue for the sample index: builds <dir>/<name>.vix for one video
   (player.md, "The sample index").  Meant for the library update (the
   tagcache thread's precompute step, which already has each file's size
   and mtime) and for the player's parse-at-open fallback.  The index is
   written to a .tmp file and renamed, so a cut-off update never leaves a
   half index behind. */

#include <stdio.h>
#include <string.h>
#include "file.h"
#include "dir.h"
#include "vix_file.h"

struct fdio
{
    int fd;
    uint64_t pos;               /* where the fd is, to skip needless seeks */
};

static long fd_read_at(void *ctx, uint64_t off, void *buf, unsigned long len)
{
    struct fdio *f = ctx;
    long n;

    if (off != f->pos && lseek(f->fd, (off_t)off, SEEK_SET) != (off_t)off)
        return -1;
    n = read(f->fd, buf, len);
    f->pos = n >= 0 ? off + (unsigned long)n : (uint64_t)-1;
    return n;
}

static int fd_write(void *ctx, const void *buf, unsigned long len)
{
    return write(*(int *)ctx, buf, len) == (long)len ? 0 : MP4_E_IO;
}

/* dir and every parent it lacks (mkdir fails harmlessly when one exists) */
static void make_dirs(const char *dir)
{
    char p[MAX_PATH];
    size_t i;

    if (snprintf(p, sizeof(p), "%s", dir) >= (int)sizeof(p))
        return;
    for (i = 1; p[i]; i++)
        if (p[i] == '/')
        {
            p[i] = '\0';
            mkdir(p);
            p[i] = '/';
        }
    mkdir(p);
}

int vix_path(const char *dir, const char *video, uint64_t size,
             uint32_t mtime, char *out, size_t cap)
{
    char name[VIX_NAME_LEN + 1];
    vix_name(video, size, mtime, name);
    return snprintf(out, cap, "%s/%s.vix", dir, name) < (int)cap
           ? 0 : MP4_E_SPACE;
}

long vix_build_file(const char *video, const char *dir, uint64_t size,
                    uint32_t mtime, void *scratch, size_t scratch_len)
{
    char path[MAX_PATH], tmp[MAX_PATH];
    struct mp4_movie m;
    struct mp4_io io;
    struct fdio in;
    long n;
    int out;

    if (vix_path(dir, video, size, mtime, path, sizeof(path)) < 0 ||
        snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return MP4_E_SPACE;

    in.fd = open(video, O_RDONLY);
    if (in.fd < 0)
        return MP4_E_IO;
    in.pos = 0;
    io.read_at = fd_read_at;
    io.ctx = &in;
    io.size = size;

    n = mp4_open(&m, &io);
    if (n >= 0)
    {
        make_dirs(dir);
        out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (out < 0)
            n = MP4_E_IO;
        else
        {
            n = vix_write(&m, scratch, scratch_len, fd_write, &out);
            close(out);
            if (n < 0 || rename(tmp, path) < 0)
            {
                remove(tmp);
                if (n >= 0)
                    n = MP4_E_IO;
            }
        }
    }
    close(in.fd);
    return n;
}
