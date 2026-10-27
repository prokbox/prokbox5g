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

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "fat.h"
#include "audio.h"
#include "storage.h"
#include "tagcache.h"
#include "ipodui/uilog.h"
#include "ipodui/usbbusy.h"
#include "ipodui/xindex.h"
#include "ipodui/dbqueue.h"
#include "ipodui/artcache.h"
#include "ipodui/anim.h"
#include "backlight.h"

#define GRACE_TICKS   (2 * HZ)      /* the scan is posted just after us */
#define GAP_TICKS     (HZ)          /* a probe pass hands over to a scan */
#define MAX_TICKS     (10 * 60 * HZ) /* never a glyph that does not end */
#define STEP_TICKS    (HZ / 8)

static struct {
    bool on;
    bool seen;                      /* the scan was seen running */
    long t0, quiet, next;
    unsigned char phase;
} ub;

void ui_usbbusy_begin(void)
{
    ub.on = true;
    ub.seen = false;
    ub.t0 = ub.quiet = ub.next = current_tick;
    ub.phase = 1;
    tagcache_ui_sidecars_reset();   /* the count logged is this scan's */
}

/* Q6, checks only: nothing deleted, nothing written to the volume */
static void housekeeping(bool scan_ran)
{
    unsigned long fc = 0, nf = 0, cl = 0;
    int sc = tagcache_ui_sidecars();

    /* -1: the scan did not run to its end in the window (deferred while
     * audio holds the memory, or cut): no count to report */
    ui_log("usb", "housekeeping sidecars=%d", sc);
#ifndef SIMULATOR
    if (fat_get_fsinfo(IF_MV(0,) &fc, &nf, &cl))
    {
        /* Plausibility only: FSInfo is compared with the volume's cluster
         * count, not with a FAT free count. The scan has no such count
         * and a FAT walk (tens of seconds on 1 TB) is not for this
         * moment. Unknown (0xffffffff) or beyond the volume: a host would
         * walk the whole FAT before mounting. Logged; repair needs a go. */
        if (fc == 0xffffffffu || fc > cl)
            ui_log("usb", "fsinfo mismatch free=%lu cl=%lu", fc, cl);
        else
            ui_log("usb", "fsinfo ok free=%lu", fc);
    }
#else
    (void)fc; (void)nf; (void)cl;
    ui_log("usb", "fsinfo ok free=sim");
#endif
    /* snapshot batch hook (none on this branch yet) */
    xindex_sync(XIDX_AT_USB);       /* P12: the sync-time indexes */
    /* The scan, the art pass and the index sync log a burst (20+ lines)
     * with the disk awake; the 12-line ring only flushes at idle or with
     * audio stopped, so the busy end and these checks were overwritten
     * unwritten while music played (p16-usb-busy-play). The disk is awake
     * from the scan anyway (it ran, or the drive is spinning): write now,
     * but never wake a sleeping one. */
    if (scan_ran || storage_disk_is_active())
        ui_log_flush();
}

/* 3.20 (owner, device, 2026-10-25): any library update shows the glyph,
 * whatever started it (the boot's rebuild, the first boot of a new
 * library format, Rebuild Cache, Build Artwork, a queue's rebase). Seen
 * by this poll only: no event, no wake of its own. Its state follows
 * lit work only (owner, 2026-10-25): going dark drops it (the work a
 * sleep's window or a dark screen does is hidden), and the first lit
 * pass after starts the 1 s count afresh, so a wake into work about to
 * end shows nothing. */
#define LIB_SHOW_AFTER (HZ)         /* work under a second: no glyph */
#define LIB_MIN_ON     (HZ)         /* once up, at least a second */
#define LIB_GAP        (HZ)         /* phases a short gap apart: one run */

static struct {
    bool track, show, idle;
    long since, quiet, shown;
} lb;

/* The screen went dark (the poll's own dark pass, the backlight-off
 * event, a sleep's entry): the glyph's state is dropped, nothing drawn */
void ui_usbbusy_dark(void)
{
    long now = current_tick;

    if (lb.show)
    {
        ui_log("db", "busy glyph off dark ms=%ld",
               (now - lb.shown) * (1000 / HZ));
        /* check_trace.py busyglyph: no minimum for a dark end */
        UPDATE_TRACE("busyglyph off dark on=%ld t=%ld",
                     (now - lb.shown) * (1000 / HZ), now * (1000 / HZ));
    }
    lb.show = lb.track = false;
}

static bool lib_working(void)
{
    struct artcache_build_status a;

    if (tagcache_ui_updating() || dbq_working())
        return true;
    /* Build Artwork (the Music hold menu): the art builder's own
     * background passes after a reload are not the user's update */
    if (!artcache_maintenance_pending())
        return false;
    artcache_build_status(&a);
    return a.running && a.wait == ARTCACHE_BUILD_READY;
}

/* Updates lb; the ticks until it next decides (TIMEOUT_BLOCK: idle) */
static int lib_poll(long now, bool lit)
{
    bool w;

    if (!lit)
    {
        ui_usbbusy_dark();
        return TIMEOUT_BLOCK;
    }
    w = lib_working();

    if (w)
    {
        if (!lb.track)
        {
            lb.track = true;
            lb.since = now;
        }
        lb.quiet = now;
    }
    if (!lb.track)
        return TIMEOUT_BLOCK;
    if (!lb.show)
    {
        if (w && !TIME_BEFORE(now, lb.since + LIB_SHOW_AFTER))
        {
            lb.show = true;
            lb.idle = false;
            lb.shown = now;
            ui_log("db", "busy glyph on ms=%ld",
                   (now - lb.since) * (1000 / HZ));
            /* check_trace.py busyglyph: run >= 1000, on >= 1000 */
            UPDATE_TRACE("busyglyph on run=%ld t=%ld",
                         (now - lb.since) * (1000 / HZ), now * (1000 / HZ));
            return LIB_MIN_ON;
        }
        if (!w && TIME_AFTER(now, lb.quiet + LIB_GAP))
        {
            lb.track = false;       /* a short job: never shown */
            return TIMEOUT_BLOCK;
        }
        return w ? MAX(lb.since + LIB_SHOW_AFTER - now, 1)
                 : MAX(lb.quiet + LIB_GAP - now + 1, 1);
    }
    if (w == lb.idle)
    {
        /* check_trace.py busyglyph: up no longer than its tail after */
        lb.idle = !w;
        UPDATE_TRACE("busyglyph %s t=%ld", w ? "work" : "idle",
                     now * (1000 / HZ));
    }
    if (!w && TIME_AFTER(now, lb.quiet + LIB_GAP)
        && !TIME_BEFORE(now, lb.shown + LIB_MIN_ON))
    {
        lb.show = lb.track = false;
        ui_log("db", "busy glyph off ms=%ld",
               (now - lb.shown) * (1000 / HZ));
        UPDATE_TRACE("busyglyph off on=%ld t=%ld",
                     (now - lb.shown) * (1000 / HZ), now * (1000 / HZ));
        return TIMEOUT_BLOCK;
    }
    return MAX(MAX(lb.quiet + LIB_GAP + 1, lb.shown + LIB_MIN_ON) - now, 1);
}

/* #84: the post-USB moment; true while it lasts */
static bool usb_poll(long now)
{
    bool busy;

    if (!ub.on)
        return false;
    /* the scan itself, not a commit or reload waiting for playback to
     * stop; an owed scan counts only while it can run (a deferred one,
     * with audio holding the memory, retries each second: no glyph) */
    busy = tagcache_scan_active()
           || (tagcache_ui_scan_owed() && audio_status() == 0);
    if (busy)
    {
        ub.seen = true;
        ub.quiet = now;
    }
    if ((!busy && TIME_AFTER(now, ub.quiet + (ub.seen ? GAP_TICKS
                                                       : GRACE_TICKS)))
        || TIME_AFTER(now, ub.t0 + MAX_TICKS))
    {
        ub.on = false;
        ui_log("usb", "busy end ms=%ld seen=%d",
               (now - ub.t0) * (1000 / HZ), ub.seen);
        housekeeping(ub.seen);
        return false;
    }
    return true;
}

int ui_usbbusy_poll(struct ui_header_state *st, bool lit)
{
    long now = current_tick;
    bool was = ub.on || lb.show;
    bool usb = usb_poll(now);
    /* the backlight itself: a headerless caller passes lit false */
    bool on = is_backlight_on(true);
    int lw = lib_poll(now, on), step;
#ifdef SIMULATOR
    /* check_trace.py busyglyph: no step, no header push while dark */
    static signed char bl = -1;

    if (bl != on)
    {
        bl = on;
        UPDATE_TRACE("busyglyph bl=%d", (int)on);
    }
#endif

    if (!ub.on)
        xindex_poll();              /* P12: an index build owed */
    if (!usb && !lb.show)
    {
        /* dark: nothing drawn; the next lit pass clears a stale glyph */
        if (lit)
            st->busy = 0;
        return lit ? lw : TIMEOUT_BLOCK;
    }
    if (!was)
    {
        ub.phase = 1;               /* a new moment starts at the top */
        ub.next = now;
    }
    if (!lit)
        return HZ;                  /* nobody sees it: just look for the end */
    while (TIME_AFTER(now, ub.next))
    {
        ub.phase = ub.phase % 8 + 1;
        ub.next += STEP_TICKS;
        if (TIME_AFTER(now, ub.next + STEP_TICKS))
            ub.next = now;          /* frames are dropped, never queued */
    }
#ifdef SIMULATOR
    if (st->busy != ub.phase)
        UPDATE_TRACE("busyglyph step phase=%d t=%ld usb=%d", (int)ub.phase,
                     now * (1000 / HZ), (int)ub.on);
#endif
    st->busy = ub.phase;
    step = MAX(ub.next - now, 1);
    return lw == TIMEOUT_BLOCK ? step : MIN(step, lw);
}
