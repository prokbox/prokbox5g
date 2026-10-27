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

/* iPod UI shell: the sync-time indexer's driver (P12 W1-N, resident;
 * docs/phase12-brief.md 3.5, 8 rows 2, 4, 6, 10, 13). The sync moments
 * call xindex_sync(): the decision, the memory (one maint_take() block,
 * the M3 review fix), the module (MOD_XINDEX, pinned while it runs), the
 * job on the storage worker, and its end: the block freed, maint_done(),
 * the module unpinned, the result published by xindex_seq().
 *
 * States (8 row 10): fresh (the signature matches, or a trusted boot:
 * skipped), dirty (rebuilt), missing or corrupt (rebuilt; the screen shows
 * its empty state meanwhile), owed (playing, or the take refused: the last
 * good index stays, a marker file keeps the debt across a power-off, and
 * xindex_poll() runs it at the next stop or paused window). */
#include "config.h"
#include <stdbool.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "usb.h"
#include "audio.h"
#include "core_alloc.h"
#include "storage.h"
#ifdef HAVE_STORAGE_POWEROFF
#include "stor_pwr.h"
#endif
#include "backlight.h"
#include "ipodui/uilog.h"
#include "ipodui/anim.h"            /* UPDATE_TRACE (XLOG, release) */
#include "ipodui/maint.h"
#include "ipodui/modload.h"
#include "ipodui/extras.h"
#include "ipodui/bootstamp.h"
#include "ipodui/xindex.h"
#include "ipodui/xcontact_core.h"       /* XCONTACT_ROOT_DIR (W3-K) */
#include "ipodui/xical_core.h"          /* XCAL_ROOT_DIR (W3-C) */
#include "ipodui/tzpick.h"              /* tz_rtc_split */
#include "timefuncs.h"

/* devmode: the device log; else the sim trace only (no log lines in a
 * release build), as bootstamp.c */
#ifdef HAVE_IPOD_UI_DEVMODE
#define XLOG(...) ui_log("xidx", __VA_ARGS__)
#else
#define XLOG(fmt, ...) UPDATE_TRACE("xidx " fmt, __VA_ARGS__)
#endif

#define RETRY_TICKS (60 * HZ)           /* a failed job waits this long */
#define MONTH_TICKS (3600 * HZ)         /* L1: the month checked hourly */

static struct {
    struct xidx_job job;
    const struct xidx_ops *ops;
    volatile bool stop;
    volatile bool busy;
    bool owed;
    uint8_t seq;
    int handle;
    long t0, retry;
    long browse;                        /* a paged list's last input */
    const char *why;
    /* L1: the RTC month at the last check (year * 12 + month), so the
     * Calendars window rolls without a boot: hourly and at each wake */
    int month;
    long month_due;
    bool month_set;
} xs;

#ifndef IPODUI_MODLOAD
/* the simulator and a resident build link the module statically */
extern const void *const mod_exports_xindex[];
#endif

static const struct xidx_ops *module_ops(void)
{
#ifdef IPODUI_MODLOAD
    /* blocking: the sync moments are the snapshot batch's (modload.h) */
    return mod_export(MOD_XINDEX, MOD_SLOT_FIRST);
#else
    return mod_exports_xindex[MOD_SLOT_FIRST];
#endif
}

/* A boot the P16 stamp trusts: a clean shutdown and no USB session since
 * the last completed walk, so nothing changed /Notes; unless a build was
 * owed when that session ended */
static bool boot_trusted(void)
{
    struct boot_stamp s;

    return boot_stamp_saved(&s, NULL) && (s.flags & BOOT_STAMP_CLEAN)
           && !file_exists(XIDX_OWE);
}

static void owe(const char *why)
{
    (void)why;                          /* the trace only (release) */
    XLOG("owed why=%s", why);
    if (xs.owed)
        return;
    xs.owed = true;
    /* never a write while USB arrives (memory: no disk writes at USB
     * entry); the post-USB moment runs it again anyway */
    if (!usb_inserted())
    {
        int fd = open(XIDX_OWE, O_WRONLY | O_CREAT | O_TRUNC, 0666);

        if (fd >= 0)
            close(fd);
    }
}

static void paid(void)
{
    if (xs.owed || file_exists(XIDX_OWE))
        remove(XIDX_OWE);
    xs.owed = false;
}

/* One kind's result, logged; true if it wrote a new index */
static bool report(int kind, const char *dir, int rc)
{
    long ms = (current_tick - xs.t0) * 1000 / HZ;

    (void)ms; (void)dir; (void)kind;    /* the trace only (release) */
    if (xs.job.cut)
        XLOG("cut kind=%d n=%ld", kind, (long)xs.job.n);
    if (xs.job.made_dir)
        XLOG("made %s", dir);
    if (rc == XIDX_JOB_BUILT)
        XLOG("build kind=%d n=%ld bytes=%ld ms=%ld at=%s", kind,
             (long)xs.job.n, (long)xs.job.bytes, ms, xs.why);
    else if (rc == XIDX_JOB_FRESH)
        XLOG("fresh kind=%d n=%ld ms=%ld at=%s", kind, (long)xs.job.n, ms,
             xs.why);
    return rc == XIDX_JOB_BUILT;
}

/* The job's end: the storage worker (or inline). Single core, no yield
 * between the steps. rc: the worst kind's (stopped, failed, else ok) */
static void finish(int rc, bool built)
{
    core_unpin(xs.handle);
    core_free(xs.handle);
    xs.handle = 0;
    maint_done(MAINT_XIDX, rc >= 0);
#ifdef IPODUI_MODLOAD
    mod_unpin(MOD_XINDEX);
    /* H1: a builder has no reason to stay: the arena is the next
     * tenant's (evicted at the UI thread's next pass) */
    mod_discard(MOD_XINDEX);
#endif
    if (built)
        xs.seq++;                       /* an open screen reads it again */
    if (rc >= 0)
        paid();
    else
    {
        xs.retry = current_tick + RETRY_TICKS;
        owe(rc == XIDX_JOB_STOPPED ? "stopped" : "failed");
    }
    xs.busy = false;
}

/* One more kind in the same take, after the ones before it; rc becomes
 * the worse of the two (a stop wins, then a failure) */
static int run_kind(int (*fn)(struct xidx_job *), int kind, const char *dir,
                    int rc, bool *built)
{
    int rc2;

    if (!fn || rc == XIDX_JOB_STOPPED)
        return rc;
    rc2 = fn(&xs.job);
    if (report(kind, dir, rc2))
        *built = true;
    if (rc2 < 0 && (rc >= 0 || rc2 == XIDX_JOB_STOPPED))
        rc = rc2;
    return rc;
}

static void job_run(void)
{
    int rc;
    bool built;

    cpu_boost(true);                    /* one burst: the walks and sorts */
    rc = xs.ops->notes(&xs.job);
    built = report(XIDX_NOTES, XNOTE_ROOT_DIR, rc);
    /* W3-K, W3-C: Contacts, then Calendars, in the same take */
    rc = run_kind(xs.ops->contacts, XIDX_CONTACTS, XCONTACT_ROOT_DIR, rc,
                  &built);
    rc = run_kind(xs.ops->cal, XIDX_CAL, XCAL_ROOT_DIR, rc, &built);
    cpu_boost(false);
    finish(rc, built);
}

static void start(const char *why)
{
    int st = audio_status();
    const struct xidx_ops *ops;
    int h;

    if (xs.busy)
        return;
    /* only stopped, or paused with the window open (P16; maint_take()
     * says no otherwise): never while playing */
    if ((st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE))
    {
        owe("playing");
        return;
    }
    ops = module_ops();                 /* may wait: before the take */
    if (!ops)
    {
        owe("module");
        return;
    }
    if (!maint_take(MAINT_XIDX, XIDX_BUILD_BLK))
    {
        owe("take");
        return;
    }
    h = core_alloc(XIDX_BUILD_BLK);     /* right after the take, no yield */
    if (h <= 0)
    {
        maint_done(MAINT_XIDX, false);
        owe("alloc");
        return;
    }
    core_pin(h);
#ifdef IPODUI_MODLOAD
    mod_pin(MOD_XINDEX);
#endif
    memset(&xs.job, 0, sizeof(xs.job));
    xs.job.blk = core_get_data(h);
    xs.job.blk_len = XIDX_BUILD_BLK;
    xs.job.stop = &xs.stop;
    xs.ops = ops;
    xs.handle = h;
    xs.stop = false;
    xs.busy = true;
    xs.why = why;
    xs.t0 = current_tick;
#if defined(HAVE_STORAGE_JOBS) && !defined(SIMULATOR)
    /* the sim has no storage thread (firmware/storage.c is target-only) */
    if (storage_post_job(job_run))
        return;
#endif
    job_run();                          /* no worker: inline */
}

/* W3-C: the month Calendars' window was built in is still this month
 * (a trusted boot skips the walk, so the window would not roll until a
 * USB session): one open and one read of the header and its meta. No
 * index, or one that does not read: nothing to roll. */
static bool cal_month_current(void)
{
    struct {
        struct xidx_hdr h;
        struct xcal_meta m;
    } head;
    struct tm *now, built;
    int fd = open(XCAL_IDX, O_RDONLY);
    bool ok;

    if (fd < 0)
        return true;
    ok = read(fd, &head, sizeof(head)) == (ssize_t)sizeof(head);
    close(fd);
    if (!ok || head.h.magic != XIDX_MAGIC || head.h.kind != XIDX_CAL)
        return true;
    now = get_time();
    tz_rtc_split(head.m.built, &built);
    return built.tm_year == now->tm_year && built.tm_mon == now->tm_mon;
}

/* A trusted boot after a firmware update: an index written by another
 * format version (XIDX_VERSION) would pass as fresh and then not read;
 * one open and one header read each. No file, or not ours: nothing. */
static bool formats_current(void)
{
    static const char *const idx[] = { XNOTES_IDX, XCONTACTS_IDX, XCAL_IDX };
    struct xidx_hdr h;
    unsigned i;

    for (i = 0; i < sizeof(idx) / sizeof(idx[0]); i++)
    {
        int fd = open(idx[i], O_RDONLY);
        bool old;

        if (fd < 0)
            continue;
        old = read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h)
              && h.magic == XIDX_MAGIC && h.version != XIDX_VERSION;
        close(fd);
        if (old)
            return false;
    }
    return true;
}

static int month_now(void)
{
    struct tm *t = get_time();

    return t->tm_year * 12 + t->tm_mon;
}

void xindex_sync(enum xidx_when when)
{
    xs.month = month_now();             /* L1: this sync's month */
    xs.month_set = true;
    xs.month_due = current_tick + MONTH_TICKS;
    if (when == XIDX_AT_BOOT && boot_trusted() && !formats_current())
    {
        /* rebuilt once, in the new format */
        XLOG("owed why=%s", "format");
        xs.retry = current_tick;
        start("format");
        return;
    }
    if (when == XIDX_AT_BOOT && boot_trusted() && !cal_month_current())
    {
        /* a new month: the Calendars window rolls at this sync moment */
        XLOG("owed why=%s", "month");
        xs.retry = current_tick;
        start("month");
        return;
    }
    if (when == XIDX_AT_BOOT && boot_trusted())
    {
        XLOG("skip trusted kind=%d", XIDX_NOTES);
        return;
    }
    xs.retry = current_tick;
    start(when == XIDX_AT_BOOT ? "boot" : "usb");
}

void xindex_wake(void)
{
    xs.month_due = current_tick;        /* the next poll looks */
}

/* L1: a new month since the last sync or check: the Calendars window is
 * owed a roll (the signature carries the month, so the owed build
 * rebuilds that index only). RAM only: a boot checks the file again. One
 * RTC read an hour; no disk. */
static void month_check(void)
{
    int m;

    if (!xs.month_set || TIME_BEFORE(current_tick, xs.month_due))
        return;
    xs.month_due = current_tick + MONTH_TICKS;
    m = month_now();
    if (m == xs.month)
        return;
    xs.month = m;
    if (!xs.owed)
    {
        XLOG("owed why=%s", "month");
        xs.owed = true;
        xs.retry = current_tick;
    }
}

void xindex_poll(void)
{
    month_check();
    if (!xs.owed || xs.busy || TIME_BEFORE(current_tick, xs.retry))
        return;
    if (audio_status() != 0 && !maint_window_open())
        return;                         /* playing, or a pause too young */
    /* L2: not under a screen that reads an index, a game or Clocks, and
     * not unless the arena can place the indexer without evicting a held
     * tenant (else start() would fail and log each RETRY_TICKS): skipped
     * silently, tried again at a later pass */
    if (maint_declined(MAINT_XIDX) || usb_inserted()
        || extras_module_live(MOD_NOTES)
        || extras_module_live(MOD_CONTACTS)
        || extras_module_live(MOD_CALENDARS)
        || extras_module_live(MOD_CLOCKS) || mod_game_live()
        || !mod_room_for(MOD_XINDEX))
        return;
    xs.retry = current_tick + RETRY_TICKS;
    start("owed");
}

void xindex_cancel(void)
{
    if (xs.busy)
        xs.stop = true;
}

bool xindex_busy(void)
{
    return xs.busy;
}

unsigned xindex_seq(void)
{
    return xs.seq;
}

bool xindex_browse_keep(bool input)
{
    if (input)
        xs.browse = current_tick;
    else if (!xs.browse || TIME_AFTER(current_tick, xs.browse
                                                     + XIDX_BROWSE_HOLD))
        return false;
    if (is_backlight_on(true))
        storage_spin();
    return true;
}

bool xindex_disk_warm(void)
{
#ifdef HAVE_STORAGE_POWEROFF
    return storage_power_warm();
#else
    return storage_disk_is_active();
#endif
}
