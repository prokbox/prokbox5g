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

/* The first-use video check's per-unit token on disk (vcheck.h): a small
 * text file in the hidden folder, written whole to a temporary, synced,
 * and put in place by the shell's safe replace (apps/ipodui/safefile.h:
 * a cut at any point leaves the old token or the new one, never two and
 * never a torn one).  No NOR or bootloader write: a wiped hidden folder
 * takes the token with it and the next play simply checks again.
 *
 * Host builds (VCHECK_HOST, tests/video/test_vcheck.c): POSIX calls and
 * safefile.c's own host mode. */
#include <stdbool.h>
#include <string.h>
#ifdef VCHECK_HOST
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#define HAVE_IPOD_UI 1          /* the safe replace, built in host mode */
#else
#include "config.h"
#include <stdio.h>
#include "file.h"
#endif
#ifdef HAVE_IPOD_UI
#include "ipodui/safefile.h"
#endif
#include "vcheck.h"
#include "vcheck_store.h"

int vcheck_store_read(const struct vcheck_files *f, struct vcheck_token *t)
{
    char text[VCHECK_TOKEN_MAX + 1];
    int fd, n;

#ifdef HAVE_IPOD_UI
    safefile_recover(f->path, f->tmp, f->prev);   /* a cut replace */
#endif
    fd = open(f->path, O_RDONLY);
    if (fd < 0)
        return VCHECK_STORE_NONE;
    n = read(fd, text, sizeof(text));
    close(fd);
    if (n <= 0 || n > VCHECK_TOKEN_MAX ||
        vcheck_token_parse(text, (size_t)n, t) < 0)
        return VCHECK_STORE_CORRUPT;
    return 0;
}

int vcheck_store_write(const struct vcheck_files *f,
                       const struct vcheck_token *t)
{
    char text[VCHECK_TOKEN_MAX + 1];
    int fd, n = vcheck_token_format(t, text, sizeof(text));
    bool ok;

    if (n < 0)
        return -1;
    fd = open(f->tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return -1;
    ok = write(fd, text, n) == n;
    if (fsync(fd) < 0)
        ok = false;
    if (close(fd) < 0)
        ok = false;
    if (ok)
    {
#ifdef HAVE_IPOD_UI
        ok = safefile_replace(f->tmp, f->path, f->prev) == 0;
#else
        remove(f->path);
        ok = rename(f->tmp, f->path) == 0;
#endif
    }
    if (!ok)
        remove(f->tmp);
    return ok ? 0 : -1;
}

int vcheck_store_drop(const struct vcheck_files *f)
{
    int fd;

    /* a cut replace settled first (its leftovers would put a token back;
       safefile's removals know when two names share one chain), then
       the token's own name */
#ifdef HAVE_IPOD_UI
    safefile_recover(f->path, f->tmp, f->prev);
#else
    remove(f->prev);
    remove(f->tmp);
#endif
    fd = open(f->path, O_RDONLY);
    if (fd < 0)
        return 0;               /* nothing to drop */
    close(fd);
    return remove(f->path) == 0 ? 1 : -1;
}
