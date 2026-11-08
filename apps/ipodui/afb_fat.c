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
 * iPod UI shell: the Apple fallback's steps on the volume (#137,
 * afb_fat.h). Also built on the host by tests/applefb against the real
 * fat.c and file layer on a FAT32 image.
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
#include "config.h"
#include <string.h>
#include <stdio.h>
#include "string-extra.h"
#include <errno.h>
#include "file.h"
#include "dir.h"
#include "ipodui/afb_fat.h"

#ifndef SIMULATOR
#include "fat.h"
#include "fs_attr.h"
#include "file_internal.h"

/* path's entry under the writer lock: 1 found, 0 absent, < 0 error */
static int lookup(const char *path, unsigned int type,
                  struct path_component_info *ci)
{
    struct filestr_base stream;
    int rc;

    errno = 0;
    rc = open_stream_internal(path, type | FF_PROBE | FF_INFO, &stream, ci);
    if (rc >= 0)
        return 1;
    return (errno == ENOENT || errno == ENOTDIR) ? 0 : -1;
}

int afb_vol_probe(const char *path, struct afb_probe *p, bool chain)
{
    struct path_component_info ci;
    int rc;

    memset(p, 0, sizeof(*p));
    file_internal_lock_WRITER();
    rc = lookup(path, FF_FILE, &ci);
    if (rc > 0)
    {
        p->exists = true;
        p->fst = ci.info.fatfile.firstcluster;
        p->size = ci.filesize;
        if (chain)
            p->healthy = fat_link_chain(IF_MV(0,) p->fst, p->size, p->runs,
                                        AFB_MAXRUNS, &p->nruns);
    }
    file_internal_unlock_WRITER();
    return rc < 0 ? -1 : 0;
}

int afb_vol_link(const char *name, uint32_t fst, uint32_t size)
{
    struct path_component_info ci;
    struct fat_file file;
    int rc;

    file_internal_lock_WRITER();
    rc = lookup(AFB_LINK_DIR, FF_DIR, &ci);
    if (rc > 0)
    {
        /* the entry first (empty), then its chain and size: a cut between
         * leaves an empty read-only file, which the next park drops */
        rc = fat_create_file(&ci.info.fatfile, name,
                             ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_ARCHIVE,
                             &file, NULL);
        if (rc >= 0)
            rc = fat_link_set(&file, fst, size);
    }
    else
        rc = -1;
    file_internal_unlock_WRITER();
    return rc < 0 ? -1 : 0;
}

int afb_vol_set(const char *path, uint32_t fst, uint32_t size)
{
    struct path_component_info ci;
    int rc;

    file_internal_lock_WRITER();
    rc = lookup(path, FF_FILE, &ci);
    rc = rc > 0 ? fat_link_set(&ci.info.fatfile, fst, size) : -1;
    file_internal_unlock_WRITER();
    return rc < 0 ? -1 : 0;
}

int afb_vol_runs_free(const uint32_t *runs, int nruns)
{
    int rc;

    file_internal_lock_WRITER();
    rc = fat_link_runs_free(IF_MV(0,) runs, nruns);
    file_internal_unlock_WRITER();
    return rc;
}

int afb_vol_rechain(const uint32_t *runs, int nruns)
{
    int rc;

    file_internal_lock_WRITER();
    rc = fat_link_rechain(IF_MV(0,) runs, nruns);
    file_internal_unlock_WRITER();
    return rc;
}

int afb_vol_park(void)
{
    if (!dir_exists(AFB_LINK_DIR))
        return 1;
    return rmdir_shallow(AFB_LINK_DIR) < 0 ? -1 : 0;
}

void afb_vol_fp(uint32_t *fp_free, uint32_t *fp_next)
{
    unsigned long fc = 0, nf = 0, cl = 0;

    fat_get_fsinfo(IF_MV(0,) &fc, &nf, &cl);
    *fp_free = fc;
    *fp_next = nf;
}

#else /* SIMULATOR: the host's files stand in; a link is an empty file */

int afb_vol_probe(const char *path, struct afb_probe *p, bool chain)
{
    int fd = open(path, O_RDONLY);

    memset(p, 0, sizeof(*p));
    if (fd < 0)
        return 0;
    p->exists = true;
    p->size = ffilesize(fd);
    close(fd);
    /* a stand-in first cluster: stable for the same path and size */
    p->fst = (afb_hash(AFB_HASH0, path, strlen(path)) ^ p->size) | 2;
    if (chain)
    {
        p->healthy = p->size > 0;
        p->nruns = 1;
        p->runs[0] = p->fst;
        p->runs[1] = 1;
    }
    return 0;
}

int afb_vol_link(const char *name, uint32_t fst, uint32_t size)
{
    char path[64];
    int fd;

    (void)fst; (void)size;
    snprintf(path, sizeof(path), AFB_LINK_DIR "/%s", name);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0444);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

int afb_vol_set(const char *path, uint32_t fst, uint32_t size)
{
    (void)path; (void)fst; (void)size;
    return 0;
}

int afb_vol_runs_free(const uint32_t *runs, int nruns)
{
    (void)runs; (void)nruns;
    return 0;
}

int afb_vol_rechain(const uint32_t *runs, int nruns)
{
    (void)runs; (void)nruns;
    return -1;
}

int afb_vol_park(void)
{
    char path[sizeof(AFB_LINK_DIR) + MAX_PATH];
    DIR *d = opendir(AFB_LINK_DIR);
    struct dirent *e;

    if (!d)
        return 1;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), AFB_LINK_DIR "/%s", e->d_name);
        remove(path);
    }
    closedir(d);
    return rmdir(AFB_LINK_DIR) < 0 ? -1 : 0;
}

void afb_vol_fp(uint32_t *fp_free, uint32_t *fp_next)
{
    *fp_free = 1;
    *fp_next = 1;
}
#endif /* SIMULATOR */
