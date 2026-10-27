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

/* iPod UI shell: the Calendars indexer (P12 W3-C), part of MOD_XINDEX:
 * walks /Calendars (one level of folders, Q12, created if missing), and
 * when its signature changed, parses every .ics / .vcs file into
 * calendars.idx (xical_core.h). Runs on the storage worker inside the
 * driver's maint_take() block (xindex.c), while playback is stopped.
 *
 * One pass:
 *  1. the last index, read whole into the block and checked: its sig;
 *  2. the walk: the root's calendar files and folders, then each folder's
 *     files, summed into the signature (name, size, mtime), plus the home
 *     city and the current month (the window rolls forward monthly, and
 *     a new home zone moves every time);
 *  3. equal and sound: fresh. Otherwise each file is read in 4 KB chunks
 *     into the parser (xcal_feed), the index laid out (xcal_emit) and
 *     written through safefile (tmp, fsync, rename).
 * The block (XIDX_BUILD_BLK): the index being built | instance uids |
 * To Do's | the parser | the read chunk | the file list. No allocation,
 * no boost of its own (the driver holds one), no screen. */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "usb.h"
#include "timefuncs.h"
#include "settings.h"
#include "ipodui/anim.h"
#include "ipodui/safefile.h"
#include "ipodui/tzpick.h"
#include "ipodui/xindex.h"
#include "ipodui/xical_core.h"

#define CHUNK       4096
#define FILES_MAX   (2 * XCAL_MAX_CALS)     /* listed; the rest only signed */
#define NAMES_POOL  4096
#define PATH_MAX_C  200                     /* "Folder/file.ics" kept */

struct cal_file {
    uint16_t name;                          /* the pool: "dir/file" or "file" */
    uint16_t base;                          /* its file name's offset */
};

struct cal_blk {
    uint8_t  out[(XCAL_FILE_BYTES + 3) & ~3u];
    uint16_t uid[XCAL_MAX_INSTANCES];
    struct xcal_rec todo[XCAL_TODO_MAX];
    struct xcal_build b;
    char     chunk[CHUNK];
    struct cal_file file[FILES_MAX];
    char     pool[NAMES_POOL];
    uint16_t dirs[FILES_MAX];               /* folders of the root */
    char     name[PATH_MAX_C];              /* scratch: off the worker's */
    char     path[MAX_PATH];                /* stack */
};
_Static_assert(sizeof(struct cal_blk) <= XIDX_BUILD_BLK,
               "the Calendars build fits the take");

struct walk {
    struct xidx_job *j;
    struct cal_blk *k;
    uint32_t sig, used;
    int nfile, ndir;
};

static bool stopped(const struct walk *w)
{
    return *w->j->stop || usb_inserted();
}

static bool is_cal(const char *name)
{
    const char *dot = strrchr(name, '.');

    return dot && (!strcasecmp(dot, ".ics") || !strcasecmp(dot, ".vcs"));
}

static int pool_add(struct walk *w, const char *s)
{
    uint32_t len = strlen(s) + 1, at = w->used;

    if (at + len > NAMES_POOL)
        return -1;
    memcpy(w->k->pool + at, s, len);
    w->used += len;
    return (int)at;
}

/* One folder's entries; folders only in the root */
static int walk_dir(struct walk *w, const char *path, const char *rel)
{
    struct dirent *e;
    DIR *d = opendir(path);

    if (!d)
        return XIDX_JOB_FAIL;
    while ((e = readdir(d)) != NULL)
    {
        struct dirinfo info;
        bool dir;
        char *name = w->k->name;
        int at;

        if (stopped(w))
        {
            closedir(d);
            return XIDX_JOB_STOPPED;
        }
        if (e->d_name[0] == '.')
            continue;
        info = dir_get_info(d, e);
        dir = (info.attribute & ATTR_DIRECTORY) != 0;
        if (dir ? rel != NULL : !is_cal(e->d_name))
            continue;
        w->sig = xidx_sig_add(w->sig, e->d_name,
                              dir ? 0 : (uint32_t)info.size,
                              dir ? 0 : (uint32_t)info.mtime);
        if (snprintf(name, PATH_MAX_C, "%s%s%s", rel ? rel : "",
                     rel ? "/" : "", e->d_name) >= PATH_MAX_C)
        {
            w->j->cut = 1;
            continue;
        }
        if (dir)
        {
            if (w->ndir < FILES_MAX && (at = pool_add(w, name)) >= 0)
                w->k->dirs[w->ndir++] = (uint16_t)at;
            else
                w->j->cut = 1;
            continue;
        }
        if (w->nfile < FILES_MAX && (at = pool_add(w, name)) >= 0)
        {
            w->k->file[w->nfile].name = (uint16_t)at;
            w->k->file[w->nfile].base = (uint16_t)(at + (rel ? strlen(rel) + 1
                                                             : 0));
            w->nfile++;
        }
        else
            w->j->cut = 1;
    }
    closedir(d);
    return XIDX_JOB_BUILT;
}

static bool last_sig(uint8_t *buf, uint32_t *sig)
{
    const struct xidx_hdr *h = (const struct xidx_hdr *)buf;
    int fd = open(XCAL_IDX, O_RDONLY);
    bool ok = false;
    off_t size;

    if (fd < 0)
        return false;
    size = ffilesize(fd);
    if (size >= (off_t)sizeof(*h) && size <= XIDX_FILE_MAX
        && read(fd, buf, size) == size
        && xidx_check_body(buf, (uint32_t)size) == XIDX_OK
        && h->kind == XIDX_CAL
        && xcal_tab_check(buf + sizeof(*h), h->rec_off - sizeof(*h))
           == XIDX_OK)
    {
        *sig = h->sig;
        ok = true;
    }
    close(fd);
    return ok;
}

static bool write_index(const uint8_t *out, int32_t bytes)
{
    int fd = open(XCAL_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool ok;

    if (fd < 0)
        return false;
    ok = write(fd, out, bytes) == bytes;
    ok = fsync(fd) == 0 && ok;
    ok = close(fd) == 0 && ok;
    if (ok && safefile_replace(XCAL_TMP, XCAL_IDX, XCAL_PREV) == 0)
        return true;
    remove(XCAL_TMP);
    return false;
}

/* one calendar file through the parser */
static int parse_file(struct walk *w, int i)
{
    struct cal_blk *k = w->k;
    char *path = k->path;
    int fd;
    ssize_t got;

    if (xcal_file_begin(&k->b, k->pool + k->file[i].base) < 0)
        return XIDX_JOB_BUILT;          /* the calendar cap: counted */
    snprintf(path, MAX_PATH, XCAL_ROOT_DIR "/%s",
             k->pool + k->file[i].name);
    fd = open(path, O_RDONLY);
    if (fd >= 0)
    {
        while ((got = read(fd, k->chunk, CHUNK)) > 0)
        {
            xcal_feed(&k->b, k->chunk, (uint32_t)got);
            if (stopped(w))
            {
                close(fd);
                return XIDX_JOB_STOPPED;
            }
            yield();                    /* a long file: let others run */
        }
        close(fd);
    }
    xcal_file_end(&k->b);
    return XIDX_JOB_BUILT;
}

int xcal_build_job(struct xidx_job *j)
{
    struct cal_blk *k = (struct cal_blk *)j->blk;
    struct walk w;
    struct tm *tm;
    char *path = k->path;
    uint32_t old = 0, now;
    bool have_old;
    int rc, i, n, city;
    int32_t bytes;

    if (j->blk_len < sizeof(*k))
        return XIDX_JOB_FAIL;
    j->n = j->bytes = 0;
    j->cut = j->made_dir = 0;
    safefile_recover(XCAL_IDX, XCAL_TMP, XCAL_PREV);
    have_old = last_sig(k->out, &old);

    memset(&w, 0, sizeof(w));
    w.j = j;
    w.k = k;
    w.sig = XIDX_SIG_INIT;
    if (!dir_exists(XCAL_ROOT_DIR))
    {
        /* Q12: the folder a user drops calendars into is always there */
        if (mkdir(XCAL_ROOT_DIR) == 0)
            j->made_dir = 1;
        rc = XIDX_JOB_BUILT;
    }
    else
        rc = walk_dir(&w, XCAL_ROOT_DIR, NULL);
    for (i = 0, n = w.ndir; rc == XIDX_JOB_BUILT && i < n; i++)
    {
        const char *rel = k->pool + k->dirs[i];

        if (snprintf(path, MAX_PATH, XCAL_ROOT_DIR "/%s", rel) >= MAX_PATH)
            continue;
        rc = walk_dir(&w, path, rel);
        if (rc == XIDX_JOB_FAIL)
            rc = XIDX_JOB_BUILT;        /* gone since */
    }
    if (rc != XIDX_JOB_BUILT)
        return rc;
    /* the home zone and the month are part of what the index says */
    tm = get_time();
    now = tz_rtc_make(tm);
    city = global_settings.ipodui_tz_city;
    w.sig = xidx_sig_add(w.sig, "#", (uint32_t)city,
                         (uint32_t)((tm->tm_year + 1900) * 12 + tm->tm_mon));
    j->n = w.nfile;
    if (have_old && old == w.sig)
        return XIDX_JOB_FRESH;

    /* a fixed calendar order: by path, any case (insertion sort, <= 32) */
    for (i = 1; i < w.nfile; i++)
    {
        struct cal_file t = k->file[i];
        int m = i;

        while (m > 0 && strcasecmp(k->pool + t.name,
                                   k->pool + k->file[m - 1].name) < 0)
        {
            k->file[m] = k->file[m - 1];
            m--;
        }
        k->file[m] = t;
    }
    memset(&k->b, 0, sizeof(k->b));
    k->b.now = now;
    k->b.home_std = (int16_t)tzp_city_offset(city);
    k->b.home_rule = (uint8_t)tzp_city_dst(city);
    k->b.out = k->out;
    k->b.uid = k->uid;
    k->b.todo = k->todo;
    xcal_begin(&k->b);
    for (i = 0; i < w.nfile; i++)
    {
        rc = parse_file(&w, i);
        if (rc != XIDX_JOB_BUILT)
            return rc;
    }
    bytes = xcal_emit(&k->b, w.sig);
    j->n = (int32_t)(k->b.n + k->b.ntodo);
    j->bytes = bytes;
    if (k->b.cut || k->b.cal_cut)
        j->cut = 1;
    UPDATE_TRACE("xcal files=%d cals=%u events=%lu inst=%lu todo=%lu "
                 "tzunk=%lu rrunsup=%lu poolcut=%lu itercut=%lu bad=%lu",
                 w.nfile, k->b.meta.ncal, (unsigned long)k->b.events,
                 (unsigned long)k->b.instances, (unsigned long)k->b.ntodo,
                 (unsigned long)k->b.tz_unknown, (unsigned long)k->b.rr_unsup,
                 (unsigned long)k->b.pool_cut, (unsigned long)k->b.iter_cut,
                 (unsigned long)k->b.bad);
    if (k->b.cut)
        UPDATE_TRACE("xcal cut n=%lu", (unsigned long)k->b.cut);
    if (stopped(&w))
        return XIDX_JOB_STOPPED;
    return write_index(k->out, bytes) ? XIDX_JOB_BUILT : XIDX_JOB_FAIL;
}
