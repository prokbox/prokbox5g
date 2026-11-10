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

#include <errno.h>
#include <string.h>
#include "config.h"
#include "chaincheck.h"

#include <stdio.h>

char *chain_report_line(char *buf, int len, const char *path,
                        const struct chain_report *r)
{
    static const char why[] = "OFSPRU";
    const char *b = strrchr(path, '/');
    snprintf(buf, len, "%c s%lu c%lx %lu/%lu @%lx=%lx %s",
             (!r->size && r->fst) ? 'E' : why[r->stop < 6 ? r->stop : 5],
             (unsigned long)r->size, (unsigned long)r->fst,
             (unsigned long)r->walked, (unsigned long)r->want,
             (unsigned long)r->last, (unsigned long)r->next,
             b ? b + 1 : path);
    return buf;
}

#ifdef SIMULATOR
#include "file.h"
/* the host's files: no chain to walk; a file whose last byte reads is
 * whole (what a cut chain fails on the device), no runs to compare */
void chain_cache_drop(void) {}

int chain_check(const char *path, uint32_t *runs, int maxruns, int *nruns)
{
    return chain_check_why(path, runs, maxruns, nruns, NULL);
}

int chain_check_why(const char *path, uint32_t *runs, int maxruns,
                    int *nruns, struct chain_report *rep)
{
    char c;
    int rc = CHAIN_OK;
    (void)runs; (void)maxruns;
    *nruns = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return (errno == ENOENT || errno == ENOTDIR || errno == EISDIR)
             ? CHAIN_ABSENT : CHAIN_UNREADABLE;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0 || (size > 0 && (lseek(fd, size - 1, SEEK_SET) != size - 1
                                  || read(fd, &c, 1) != 1)))
        rc = CHAIN_DAMAGED;
    if (rep)
    {
        memset(rep, 0, sizeof(*rep));
        rep->size = size < 0 ? 0 : (uint32_t)size;
        rep->stop = rc == CHAIN_OK ? 0 : 2;     /* FAT_CHAIN_SHORT */
    }
    close(fd);
    return rc;
}
#else /* !SIMULATOR */
#include "file_internal.h"
#include "fat.h"

void chain_cache_drop(void)
{
    file_internal_lock_WRITER();
    fat_cache_drop(IF_MV(0));
    file_internal_unlock_WRITER();
}

int chain_check(const char *path, uint32_t *runs, int maxruns, int *nruns)
{
    return chain_check_why(path, runs, maxruns, nruns, NULL);
}

/* the writer lock, as afb_fat.c's lookup: no remove or write moves the
 * entry or the chain while it is walked */
int chain_check_why(const char *path, uint32_t *runs, int maxruns,
                    int *nruns, struct chain_report *rep)
{
    struct fat_chain_why w = { 0, 0, 0, 0, FAT_CHAIN_OK };
    struct path_component_info ci;
    struct filestr_base stream;
    int rc;

    *nruns = 0;
    file_internal_lock_WRITER();
    errno = 0;
    rc = open_stream_internal(path, FF_FILE | FF_PROBE | FF_INFO, &stream,
                              &ci);
    if (rc < 0)
        rc = (errno == ENOENT || errno == ENOTDIR || errno == EISDIR)
             ? CHAIN_ABSENT
             : CHAIN_UNREADABLE;  /* a folder is no file to free */
    else
    {
        long fst = ci.info.fatfile.firstcluster;

        w.last = fst;
        if (!ci.filesize)
        {
            /* an empty file names no cluster; one that does was never
             * closed cleanly, and its chain is unknown */
            rc = fst ? CHAIN_DAMAGED : CHAIN_OK;
            w.stop = fst ? FAT_CHAIN_PAST : FAT_CHAIN_OK;
        }
        else if (fst < 2)
        {
            rc = CHAIN_DAMAGED;     /* a size and no chain: cut at 0 */
            w.stop = FAT_CHAIN_RANGE;
        }
        else
        {
            rc = fat_link_chain_why(IF_MV(0,) fst, ci.filesize, runs,
                                    maxruns, nruns, &w);
            rc = rc < 0 ? CHAIN_UNREADABLE : rc ? CHAIN_OK : CHAIN_DAMAGED;
        }
        if (rep)
        {
            rep->size = ci.filesize;
            rep->fst = fst;
        }
    }
    if (rep)
    {
        if (rc == CHAIN_ABSENT || rc == CHAIN_UNREADABLE)
            rep->size = rep->fst = 0;
        rep->walked = w.walked;
        rep->want = w.want;
        rep->last = w.last;
        rep->next = w.next;
        rep->stop = rc == CHAIN_UNREADABLE ? FAT_CHAIN_UNREADABLE : w.stop;
    }
    file_internal_unlock_WRITER();
    return rc;
}
#endif /* !SIMULATOR */
