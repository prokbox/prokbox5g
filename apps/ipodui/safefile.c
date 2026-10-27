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
 * iPod UI shell
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
/* The safe replace (safefile.h, #91). No statics, no allocation; the
 * directory recovery uses one MAX_PATH pair on the caller's stack.
 * Crash model: tests/fatcrash (safetest.c), a cut after every sector
 * write of every site's sequence, and a failed victim removal.
 *
 * Host builds (SAFEFILE_HOST): POSIX calls; a test may route rename and
 * remove through its own fault hooks with -Drename=... -Dremove=... or a
 * header of macros named by SAFEFILE_HOST_INCLUDE. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef SAFEFILE_HOST
#ifdef SAFEFILE_HOST_INCLUDE
#include SAFEFILE_HOST_INCLUDE      /* a test's fault-injecting shims */
#endif
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <strings.h>
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
static bool exists(const char *p) { return access(p, F_OK) == 0; }
#define modclass_save(begin) do { } while (0)
#else
#include "config.h"
#include <stdio.h>
#include "file.h"
#include "dir.h"
#include "string-extra.h"
/* the class accountant (P12): a replace is one save. Real only where
 * modclass.h has it (the sim, developer builds); host harnesses that
 * build this file against the firmware's fs (tests/fatcrash) have no
 * apps/ on their include path */
#if defined(SIMULATOR) || defined(HAVE_IPOD_UI_DEVMODE)
#include "ipodui/modclass.h"
#else
#define modclass_save(begin) do { } while (0)
#endif
#define exists(p) file_exists(p)
#endif
#include "safefile.h"

/* H1: a cut inside a rename can leave both names on one cluster chain
 * (fat.c FAT_ORDERED_DIRENTRIES). Removing either name then frees the
 * other's data, and the next allocation cross-links it. Only the native
 * file layer can tell (start clusters); a host or simulator file system
 * never shares a chain between names. */
int safefile_drop_alias(const char *path, const char *keep)
{
#if defined(SAFEFILE_HOST) || defined(SIMULATOR)
    (void)path; (void)keep;
    return 0;
#else
    return rbfs_remove_alias(path, keep);
#endif
}

bool safefile_remove(const char *path, const char *keep)
{
    int a;

    if (!exists(path))
        return true;            /* the common case: one lookup */
    a = keep ? safefile_drop_alias(path, keep) : 0;

    if (a > 0)
        return true;            /* only the second name went */
    if (a < 0)
        return !exists(path);   /* undecided: never free the data */
    /* "it was not there" counts as done; the errno of a failed remove is
     * not trusted (a lookup decides) */
    return remove(path) == 0 || !exists(path);
}

bool safefile_remove_in(const char *path, const char *dir)
{
    char other[MAX_PATH];
    const char *base = strrchr(path, '/');
    size_t dl = strlen(dir);
    DIR *d;
    struct dirent *e;
    int a = 0;

    if (!exists(path))
        return true;
    base = base ? base + 1 : path;
    d = opendir(dir);
    if (!d)
        return false;
    /* stops at the first verdict: an alias dropped (that one write, then
     * no further reads of the directory) or an error */
    while (a == 0 && (e = readdir(d)))
    {
        size_t nl = strlen(e->d_name);

        if (!strcmp(e->d_name, base) || e->d_name[0] == '.'
            || dl + 1 + nl >= sizeof(other))
            continue;
        memcpy(other, dir, dl);
        other[dl] = '/';
        memcpy(other + dl + 1, e->d_name, nl + 1);
        a = safefile_drop_alias(path, other);
    }
    closedir(d);
    if (a != 0)
        return a > 0 || !exists(path);
    return remove(path) == 0 || !exists(path);
}

static bool gone(const char *p, const char *keep)
{
    return safefile_remove(p, keep);
}

static int replace(const char *tmp, const char *dest, const char *prev)
{
    bool had = exists(dest);

    if (!had && exists(prev))
    {
        /* an earlier replace cut between 3 and 4: the old file first */
        if (rename(prev, dest) < 0)
            return -1;
        had = true;
    }
    if (!gone(prev, dest))                              /* 2 */
        return -1;
    if (had && rename(dest, prev) < 0)                  /* 3 */
        return -1;
    if (rename(tmp, dest) < 0)                          /* 4 */
    {
        /* a failed rename may still have landed: undo only onto a free
         * name (a cut here is what recovery is for) */
        if (had && !exists(dest))
            rename(prev, dest);
        return -1;
    }
    remove(prev);           /* 5: a leftover goes at the next replace or */
    return 0;               /* at recovery */
}

int safefile_replace(const char *tmp, const char *dest, const char *prev)
{
    int rc;

    /* the class accountant: its probes and renames are the save's, not
     * a module's reads (the tmp file's writes before it count as such) */
    modclass_save(true);
    rc = replace(tmp, dest, prev);
    modclass_save(false);
    return rc;
}

static void recover(const char *dest, const char *tmp, const char *prev);

void safefile_recover(const char *dest, const char *tmp, const char *prev)
{
    /* the save protocol's own probes, as a replace's (modclass_save()) */
    modclass_save(true);
    recover(dest, tmp, prev);
    modclass_save(false);
}

static void recover(const char *dest, const char *tmp, const char *prev)
{
    if (prev && exists(prev))
    {
        if (exists(dest))
            gone(prev, dest);   /* a cut in 3 or 5; in 3: one chain */
        else
            rename(prev, dest);
    }
    if (tmp)
        gone(tmp, dest);        /* a cut in 4: tmp may name dest's chain */
}

static bool playlist_name(const char *name, size_t len)
{
    return (len > 4 && !strncasecmp(name + len - 4, ".m3u", 4))
        || (len > 5 && !strncasecmp(name + len - 5, ".m3u8", 5));
}

/* the destination a marker names, and the size its temporary had when
 * the marker was written: "<dest>\n<size>\n". A torn marker (a cut while
 * it was written: no final newline) names nothing, and the save it
 * announced had not started its remove yet */
static bool intent_read(const char *marker, char *dest, long *size)
{
    char b[MAX_PATH + 16];
    char *nl, *end;
    ssize_t n;
    int fd = open(marker, O_RDONLY);

    if (fd < 0)
        return false;
    n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n < 4 || b[0] != '/' || b[n - 1] != '\n' || memchr(b, '\0', n))
        return false;
    b[n - 1] = '\0';
    nl = strchr(b, '\n');
    if (!nl || nl - b >= MAX_PATH || nl[1] < '0' || nl[1] > '9')
        return false;
    *nl = '\0';
    *size = strtol(nl + 1, &end, 10);
    if (*end)
        return false;
    memcpy(dest, b, (size_t)(nl - b) + 1);
    return true;
}

static long size_of(const char *path)
{
    long n;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -1;
    n = (long)lseek(fd, 0, SEEK_END);
    close(fd);
    return n;
}

void safefile_recover_dir(const char *dir, const char *suffix,
                          const char *marker)
{
    char tmp[MAX_PATH], dest[MAX_PATH], named[MAX_PATH + 16];
    size_t sl = strlen(suffix), dl = strlen(dir);
    long size;
    bool again = true;

    /* L1: a temporary here is never rolled forward. A save cut inside its
     * remove-then-rename is finished from its marker (playlist_init()); a
     * temporary the marker still names (its recovery failed) is left for
     * the next boot, every other one goes. The destination, a playlist
     * the user may have made, is never touched: a temporary sharing its
     * chain loses only its own name */
    named[0] = '\0';
    if (marker && intent_read(marker, named, &size))
    {
        size_t n = strlen(named);       /* < MAX_PATH: room for a suffix */
        if (n + sl < sizeof(named))
            memcpy(named + n, suffix, sl + 1);
    }

    /* remove one entry per directory pass: never write into a directory
     * while it is being enumerated */
    for (int pass = 0; again && pass < 64; pass++)
    {
        DIR *d = opendir(dir);
        struct dirent *e;

        again = false;
        if (!d)
            return;
        while ((e = readdir(d)))
        {
            size_t nl = strlen(e->d_name);

            if (nl <= sl || strcmp(e->d_name + nl - sl, suffix)
                || !playlist_name(e->d_name, nl - sl)
                || dl + 1 + nl >= sizeof(tmp))
                continue;
            memcpy(tmp, dir, dl);
            tmp[dl] = '/';
            memcpy(tmp + dl + 1, e->d_name, nl + 1);
            if (!strcmp(tmp, named))
                continue;
            memcpy(dest, tmp, dl + 1 + nl - sl);
            dest[dl + 1 + nl - sl] = '\0';
            again = true;
            break;
        }
        closedir(d);
        if (!again)
            return;
        if (!gone(tmp, exists(dest) ? dest : NULL))
            return;             /* an I/O error: leave it, never loop */
    }
}

bool safefile_intent_set(const char *marker, const char *dest, long size)
{
    char b[MAX_PATH + 16];
    int fd, n = snprintf(b, sizeof(b), "%s\n%ld\n", dest, size);
    bool ok;

    if (dest[0] != '/' || size < 0 || n <= 0 || n >= (int)sizeof(b)
        || strlen(dest) >= MAX_PATH || !safefile_remove(marker, NULL))
        return false;
    fd = open(marker, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    ok = write(fd, b, n) == (ssize_t)n;
    ok = close(fd) == 0 && ok;
    if (!ok)
        remove(marker);
    return ok;
}

void safefile_intent_recover(const char *marker, const char *suffix)
{
    char dest[MAX_PATH], tmp[MAX_PATH];
    size_t dl, sl = strlen(suffix);
    long size;

    if (!exists(marker))
        return;
    if (intent_read(marker, dest, &size)
        && (dl = strlen(dest)) + sl < sizeof(tmp))
    {
        memcpy(tmp, dest, dl);
        memcpy(tmp + dl, suffix, sl + 1);
        if (exists(tmp))
        {
            if (exists(dest))
            {
                if (!gone(tmp, dest))
                    return;             /* an I/O error: keep the marker */
            }
            /* L1: only the temporary the marker describes, whole, takes
             * the free name; any other is not this save's and is left */
            else if (size_of(tmp) == size && rename(tmp, dest) < 0)
                return;
        }
    }
    remove(marker);
}
