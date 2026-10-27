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
 * iPod UI shell: the no-change boot stamp and the boot window
 * (Phase 16-2, #89 and #87)
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
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#ifdef SIMULATOR
#include <stdlib.h>
#endif
#include "string-extra.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "dir.h"
#include "pathfuncs.h"
#include "button.h"
#include "usb.h"
#include "tagcache.h"
#ifndef SIMULATOR
#include "fat.h"
#endif
#include "ipodui/anim.h"
#include "ipodui/uilog.h"
#include "ipodui/bootstamp.h"
#include "ipodui/xindex.h"
#ifdef IPODUI_MODULES
#include "ipodui/modload.h"
#endif

#define STAMP_FILE ROCKBOX_DIR "/boot_stamp.dat"
#define STAMP_TMP  ROCKBOX_DIR "/boot_stamp.tmp"   /* L3: written first */

/* devmode: the device log; else the sim trace only (lean: no log lines in
 * a release build) */
#ifdef HAVE_IPOD_UI_DEVMODE
#define BS_LOG(...) ui_log("boot", __VA_ARGS__)
#else
#define BS_LOG(fmt, ...) UPDATE_TRACE("boot " fmt, __VA_ARGS__)
#endif

static struct boot_stamp saved;         /* 32 B: what boot found */
static bool saved_ok;
static enum boot_why saved_why = BOOT_WHY_NOSTAMP;
static signed char decided = -1;        /* -1, 0 skipped, 1 walked */
static enum boot_why decided_why = BOOT_WHY__N;
static int walks;                       /* scans this boot */
#ifdef SIMULATOR
static bool sim_unclean;
#endif

void boot_stamp_spoil(void)
{
    unsigned char buf[sizeof(struct boot_stamp) + 1];
    ssize_t n;
    int fd;

    if (file_exists(STAMP_TMP))
        remove(STAMP_TMP);              /* a write cut before its rename */
    fd = open(STAMP_FILE, O_RDONLY);
    if (fd < 0)
        return;                         /* none: walk */
    n = read(fd, buf, sizeof(buf));     /* one more: a longer file fails */
    close(fd);
    saved_ok = boot_stamp_check(buf, n > 0 ? (size_t)n : 0, &saved);
    saved_why = saved_ok ? BOOT_WHY_TRUST : BOOT_WHY_TORN;
    /* spoil before anything else this boot can fail: a crash from here on
     * finds no stamp at the next boot */
    if (remove(STAMP_FILE) < 0)
    {
        fd = open(STAMP_FILE, O_WRONLY | O_TRUNC);
        if (fd >= 0)
            close(fd);                  /* 0 bytes: never sound */
        else
            saved_ok = false;           /* it may outlive us: not now */
    }
}

bool boot_stamp_saved(struct boot_stamp *out, enum boot_why *why)
{
    if (out)
        *out = saved;
    if (why)
        *why = saved_why;
    return saved_ok;
}

/* One folder, depth first, in the order the library walk reads it
 * (tagcache.c check_dir()): path[len] is its end; the hidden folder and
 * "._" sidecars are left out, a folder's entries end with a marker. */
static enum boot_digest_state walk_dir(struct boot_digest *d, char *path,
                                       size_t len, size_t sz, long deadline,
                                       bool (*stop)(void))
{
    enum boot_digest_state st = BOOT_DIGEST_MATCH;
    struct dirent *e;
    DIR *dir = opendir(path);

    if (!dir)
        return BOOT_DIGEST_FAIL;
    while ((e = readdir(dir)) != NULL)
    {
        struct dirinfo info;
        bool isdir;
        int n;

        if (is_dotdir_name(e->d_name)
            || (e->d_name[0] == '.' && e->d_name[1] == '_'))
            continue;
        if (!TIME_BEFORE(current_tick, deadline))
        {
            st = BOOT_DIGEST_SLOW;
            break;
        }
        if (stop && stop())
        {
            st = BOOT_DIGEST_FAIL;
            break;
        }
        info = dir_get_info(dir, e);
        isdir = (info.attribute & ATTR_DIRECTORY) != 0;
        n = snprintf(path + len, sz - len, "%s%s",
                     len && path[len - 1] == '/' ? "" : "/", e->d_name);
        if (n < 0 || (size_t)n >= sz - len)
        {
            path[len] = '\0';
            st = BOOT_DIGEST_FAIL;
            break;
        }
        if (isdir && !strcasecmp(path, ROCKBOX_DIR))
        {
            path[len] = '\0';           /* ours: changes at every run */
            continue;
        }
        boot_digest_entry(d, e->d_name, isdir, (uint32_t)info.size,
                          (uint32_t)info.mtime, BOOT_DIRENT_CLUSTER(e));
        if (isdir)
            st = walk_dir(d, path, len + n, sz, deadline, stop);
        path[len] = '\0';
        if (st != BOOT_DIGEST_MATCH)
            break;
    }
    closedir(dir);
    if (st == BOOT_DIGEST_MATCH)
        boot_digest_leave(d);
    return st;
}

enum boot_digest_state boot_stamp_digest(struct boot_digest *d,
                                         const char *const roots[],
                                         char *path, size_t pathsz,
                                         long deadline, bool (*stop)(void))
{
    boot_digest_begin(d);
    for (int i = 0; roots[i]; i++)
    {
        enum boot_digest_state st;
        int j;

        for (j = 0; j < i && strcmp(roots[j], roots[i]); j++)
            ;
        if (j < i || !dir_exists(roots[i]))
            continue;                   /* as the walk: once, if there */
        if ((size_t)strlcpy(path, roots[i], pathsz) >= pathsz)
            return BOOT_DIGEST_FAIL;
        st = walk_dir(d, path, strlen(path), pathsz, deadline, stop);
        if (st != BOOT_DIGEST_MATCH)
            return st;
    }
    return BOOT_DIGEST_MATCH;       /* the caller compares */
}

bool boot_stamp_write(uint32_t digest, uint32_t generation,
                      unsigned boots_skipped, uint32_t last_walk_rtc,
                      bool settle)
{
    struct boot_stamp s;
    bool ok;
    int fd;

    memset(&s, 0, sizeof(s));
#ifndef SIMULATOR
    {
        unsigned long fc = 0;
        if (fat_get_fsinfo(IF_MV(0,) &fc, NULL, NULL))
            s.fsinfo_free = fc;
    }
#endif
    s.last_walk_rtc = last_walk_rtc;
    s.dir_digest = digest;
    s.db_generation = generation;
    s.boots_skipped = boots_skipped > 0xffff ? 0xffff : boots_skipped;
    s.flags = BOOT_STAMP_CLEAN;
    boot_stamp_seal(&s);
#ifdef SIMULATOR
    if (sim_unclean)
    {
        BS_LOG("stamp %s=0 why=%s", settle ? "settle" : "write",
               "sim_unclean");
        return false;
    }
#endif
    /* L3: the data goes under a temporary name first and is synced; only
     * then does the stamp's own name appear (the rename's barrier, #91),
     * so a cut never shows the name over a reused cluster that still
     * holds an older, sound stamp. The boot removed the name, so the
     * rename never lands on an existing one (#91); a second write in one
     * session (a power-off USB superseded) removes it first. */
    remove(STAMP_FILE);
    fd = open(STAMP_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    ok = write(fd, &s, sizeof(s)) == (ssize_t)sizeof(s);
    if (ok && fsync(fd) < 0)
        ok = false;
    if (close(fd) < 0)
        ok = false;
    if (ok && rename(STAMP_TMP, STAMP_FILE) < 0)
        ok = false;
    if (!ok)
        remove(STAMP_TMP);
    BS_LOG("stamp %s=%d skipped=%u gen=%ld walk_rtc=%lu",
           settle ? "settle" : "write", ok ? 1 : 0, boots_skipped,
           (long)generation, (unsigned long)last_walk_rtc);
    (void)settle;
    return ok;
}

void boot_stamp_drop(const char *why)
{
    /* outside the log: BS_LOG is UPDATE_TRACE, empty on the device, in a
     * release build, and the stamp must go there too (P12 audit) */
    int rc = remove(STAMP_FILE);

    (void)rc;
    (void)why;
    BS_LOG("stamp removed=%d (%s)", rc == 0 ? 1 : 0, why);
}

void boot_stamp_remove(void)
{
    boot_stamp_drop("usb exit");
}

void boot_stamp_decided(enum boot_why why)
{
    decided_why = why;
    decided = boot_why_trusted(why) ? 0 : 1;
}

void boot_stamp_walk_started(void)
{
    walks++;
}

/* #87 (docs/phase16-brief.md 4.2, Q4): the boot screen stays up while the
 * library's boot check runs, boosted, at most BOOT_HOLD_MS. A walk that
 * the progress says cannot end inside the hold hands over at once (it
 * goes on in the background, the lists stay browsable: #85). */
void maint_boot_window(void)
{
    const long t0 = current_tick;
    const long end = t0 + BOOT_HOLD_MS * HZ / 1000;
    const char *how = "done";
    int st, pm;

#ifdef SIMULATOR
    /* tests/ui: scripts that need the device's order (the boot check
     * outlasting the hold, music resumed before it ends): no hold, the
     * check at the thread's own timeout as before */
    if (getenv("SIM_BOOT_NOHOLD"))
    {
        BS_LOG("window ms=0 %s st=%d", "nohold", tagcache_boot_check(NULL));
        xindex_sync(XIDX_AT_BOOT);      /* P12: the sync-time indexes */
        return;
    }
#endif
    tagcache_boot_window();             /* now, not at the next timeout */
    cpu_boost(true);
    for (;;)
    {
        long el = current_tick - t0;

        st = tagcache_boot_check(&pm);
        if (st == 2)
            break;
        if (button_status() != BUTTON_NONE || usb_inserted())
        {
            how = "event";              /* a key held or USB: the user's */
            break;
        }
        if (!TIME_BEFORE(current_tick, end))
        {
            how = "cap";
            break;
        }
        /* a walk: hold on only if its projected end is inside the hold */
        if (st == 1 && el >= HZ / 2
            && (pm <= 0 ? el >= HZ : t0 + el * 1000 / pm > end))
        {
            how = "handover";
            break;
        }
        sleep(HZ / 20);
    }
    /* #100: the panel-set rotation at a restart runs here (16-4; not built
     * yet). The art top-up starts after, from the art builder, as before. */
#ifdef IPODUI_MODULES
    mod_pass();                         /* #106: the boot snapshot batch */
#endif
    xindex_sync(XIDX_AT_BOOT);          /* P12: the sync-time indexes */
    cpu_boost(false);
    (void)how;                          /* the log only (release) */
    BS_LOG("window ms=%ld %s st=%d", (current_tick - t0) * (1000 / HZ), how,
           st);
}

#ifdef SIMULATOR
void sim_boot_stamp_unclean(void)
{
    sim_unclean = true;                 /* a stamp on disk stays */
}

void sim_boot_stamp_nostamp(void)
{
    sim_unclean = true;
    if (remove(STAMP_FILE) == 0)
        BS_LOG("stamp removed=1 (%s)", "sim_nostamp");
}

int sim_boot_stamp_walk(int *n, const char **why)
{
    if (n)
        *n = walks;
    if (why)
        *why = decided < 0 ? "-" : boot_why_name(decided_why);
    return decided;
}
#endif
