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

/* The Class A boost bursts (every build) and the runtime accountant (sim
 * and developer builds): docs/phase12-brief.md 2.1, 2.3 item 3. */
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "ipodui/uilog.h"
#include "ipodui/anim.h"            /* UPDATE_TRACE */
#include "ipodui/modload.h"
#include "ipodui/modclass.h"
#ifdef HAVE_DEVSTATS
#include "devstats.h"
#endif

/* -- boost bursts ------------------------------------------------------------ */
#define BURST_MAX_MS    100             /* one burst */
#define BURST_WIN       (10 * HZ)       /* the budget's window */
#define BURST_BUDGET    (BURST_WIN / 10)    /* 10 % of it */

static long burst_end = -1;             /* cap tick of the burst held, -1 */
static long burst_t0;
static long win_t0;                     /* the window's start */
static long win_used;                   /* boosted ticks in the window */

bool ui_boost_burst(int ms)
{
    long now = current_tick;

    if (TIME_AFTER(now, win_t0 + BURST_WIN))
    {
        win_t0 = now;                   /* a new window */
        win_used = 0;
    }
    if (burst_end >= 0 || win_used >= BURST_BUDGET)
    {
        UPDATE_TRACE("boost burst refused used=%ld", win_used);
        return false;
    }
    ms = MAX(1, MIN(ms, BURST_MAX_MS));
    cpu_boost(true);
    burst_t0 = now;
    burst_end = now + (ms * HZ + 999) / 1000;
    return true;
}

void ui_boost_burst_end(void)
{
    if (burst_end < 0)
        return;                         /* the cap ended it already */
    win_used += MAX(current_tick - burst_t0, 1);
    burst_end = -1;
    cpu_boost(false);
}

void ui_boost_burst_poll(void)
{
    if (burst_end >= 0 && !TIME_BEFORE(current_tick, burst_end))
    {
        ui_log("modclass", "burst capped ticks=%ld",
               current_tick - burst_t0);
        ui_boost_burst_end();
    }
}

#ifdef IPODUI_MODCLASS
/* -- the accountant ------------------------------------------------------------
 * While a module's screen is on top it counts, per visit:
 *  - storage commands of the UI thread (devstats; the sim's file layer
 *    and the target's ATA driver both report there), plus modclass_io();
 *  - pushes (devstats' UI updates, plus modclass_push());
 *  - core_alloc calls (modclass_alloc());
 *  - boosted time (the boost counter, sampled once per main-loop pass).
 * It logs at most once a period and when the visit ends. A visit opens
 * an entry window (modclass_entry(): before the root's enter() reads; a
 * folder or a document opened inside the module opens another). Only the
 * UI thread's I/O counts: the sim's script thread (a host copy, a
 * screenshot) and the background threads are not the module's. A Class A
 * breach logs "modclass viol <name> <rule>" once per visit:
 *  reads  more than 2 storage commands in an entry window's first second,
 *         or, after the windows, more than two per write (a save's tmp
 *         file: its open and write; safefile_replace()'s own probes and
 *         renames are skipped, modclass_save());
 *  alloc  any core_alloc;
 *  boost  over 10 % boosted over a visit of at least 10 s;
 *  fps    over FPS_MAX pushes a second over a period of at least 1 s;
 *  page   a page load (modclass_page()) of more than 2 commands;
 *  pages  more page loads in a second than the module's declared rate
 *         (mod_desc.pages, the manifest's pages; brief 2.1's paged-list
 *         exception to "no storage while lit"). A module that declares
 *         none books its page loads as reads. */
#define PERIOD          (2 * HZ)
/* The frame clock (anim.h UI_ANIM_FRAME_TICKS, 3 ticks at HZ 100: 33.3
 * frames a second) is the cap; a period whose ends straddle a frame
 * reads one more, so the rule allows the clock's ceiling (34), not the
 * brief's round "30 fps" */
#define FPS_MAX         ((HZ + UI_ANIM_FRAME_TICKS - 1) / UI_ANIM_FRAME_TICKS)
#define PAGE_CMDS       2
/* boosted now: the firmware's counter (every holder) on the target; the
 * simulator has no CPU clock, so only our own bursts */
#if defined(HAVE_ADJUSTABLE_CPU_FREQ) && !defined(SIMULATOR)
#define BOOSTED()       (get_cpu_boost_counter() > 0)
#else
#define BOOSTED()       (burst_end >= 0)
#endif
#define ENTRY           HZ
#define ENTRY_READS     2

static struct {
    int8_t mod;                         /* -1: no module on top */
    uint8_t viol;                       /* rules already reported */
    long t0, p0, last;                  /* visit, period, last sample */
    long e0;                            /* the entry window's start */
    unsigned int tid;                   /* the UI thread (the accountant's) */
    uint32_t reads, entry_reads, writes, allocs, pushes, boost;
    uint32_t e_reads;                   /* this entry window's */
    uint8_t saving;                     /* inside a save (nested: the SDK's
                                           exit wraps safefile_replace()) */
    bool paging;                        /* inside a page load */
    uint8_t pages_max;                  /* the module's declared rate */
    uint32_t pages, pg_cmds, pg_n;      /* page loads; this one's commands;
                                           this second's */
    long pg_t0;                         /* this second's start */
    uint32_t p_pushes;                  /* this period */
    uint32_t ds_cmds, ds_lcd;           /* devstats baselines */
    uint32_t skip_lcd;                  /* updates that are no frame of
                                           the module (modclass_push) */
} acc = { .mod = -1 };

enum { V_READS = 1, V_ALLOC = 2, V_BOOST = 4, V_FPS = 8, V_PAGE = 16,
       V_PAGES = 32 };

static void ds_sample(uint32_t *cmds, uint32_t *lcd)
{
#ifdef HAVE_DEVSTATS
    /* the thread's slot, as devstats_io() files it (THREAD_ID_SLOT) */
    unsigned slot = thread_self() & 0xff;

    *cmds = slot < DEVSTATS_SLOTS ? devstats.t[slot].cmds : 0;
    *lcd = devstats.f3.lcd_updates;
#else
    *cmds = *lcd = 0;
#endif
}

static void viol(int rule, const char *name)
{
    if (acc.viol & rule)
        return;
    acc.viol |= rule;
    UPDATE_TRACE("modclass viol %s %s", mod_name(acc.mod), name);
    ui_log("modclass", "viol %s %s", mod_name(acc.mod), name);
}

/* an entry window over its budget (Class A, the only class built) */
static void entry_reads_add(uint32_t d)
{
    const struct mod_rt *r;

    acc.entry_reads += d;
    acc.e_reads += d;
    if (acc.e_reads <= ENTRY_READS)
        return;
    r = mod_rt_get(acc.mod);
    if (!r || r->mclass == MOD_CLASS_A)
        viol(V_READS, "reads");
}

static void account(void)
{
    long now = current_tick;
    uint32_t cmds, lcd, d;

    ds_sample(&cmds, &lcd);
    d = cmds - acc.ds_cmds;
    acc.ds_cmds = cmds;
    if (TIME_BEFORE(now, acc.e0 + ENTRY))
        entry_reads_add(d);
    acc.reads += d;
    d = lcd - acc.ds_lcd;
    acc.ds_lcd = lcd;
    d = d > acc.skip_lcd ? d - acc.skip_lcd : 0;
    acc.skip_lcd = 0;
    acc.pushes += d;
    acc.p_pushes += d;
    if (BOOSTED())
        acc.boost += now - acc.last;
    acc.last = now;
}

static void report(bool end)
{
    const struct mod_rt *r = mod_rt_get(acc.mod);
    long now = current_tick, vis = MAX(now - acc.t0, 1),
         per = MAX(now - acc.p0, 1);
    unsigned pct = acc.boost * 100 / vis;
    unsigned fps = acc.p_pushes * HZ / per;
    int cls = r ? r->mclass : MOD_CLASS_A;

    (void)end;                          /* the trace compiles away on target */
    if (cls == MOD_CLASS_A)
    {
        /* steady state: storage commands net of the saves the file layer
         * reported (W1-T's hook tells writes apart); an entry window's
         * own excess is caught as it is booked (account()) */
        if (acc.reads > acc.entry_reads + 2 * acc.writes)
            viol(V_READS, "reads");
        if (acc.allocs)
            viol(V_ALLOC, "alloc");
        if (vis >= 10 * HZ && pct > 10)
            viol(V_BOOST, "boost");
        if (per >= HZ && fps > FPS_MAX)
            viol(V_FPS, "fps");
    }
    UPDATE_TRACE("modclass %s class=%c reads=%lu writes=%lu allocs=%lu "
                 "boost_pct=%u fps=%u pages=%lu%s", mod_name(acc.mod),
                 'A' + cls, (unsigned long)acc.reads,
                 (unsigned long)acc.writes, (unsigned long)acc.allocs, pct,
                 fps, (unsigned long)acc.pages, end ? " end" : "");
    acc.p0 = now;
    acc.p_pushes = 0;
}

void modclass_top(int mod_id)
{
    if (mod_id == acc.mod)
        return;
    if (acc.mod >= 0)
    {
        account();
        report(true);
    }
    acc.mod = mod_id;
    if (mod_id < 0)
        return;
    acc.t0 = acc.p0 = acc.last = acc.e0 = current_tick;
    acc.tid = thread_self();
    acc.viol = 0;
    acc.reads = acc.entry_reads = acc.e_reads = acc.writes = acc.allocs = 0;
    acc.saving = 0;
    acc.paging = false;
    acc.pushes = acc.p_pushes = acc.boost = 0;
    acc.pages = acc.pg_cmds = acc.pg_n = 0;
    acc.pg_t0 = current_tick;
    {
        const struct mod_desc *d = mod_export_now(mod_id, MOD_SLOT_DESC);

        acc.pages_max = d && d->magic == MOD_HDR_MAGIC ? d->pages : 0;
    }
    ds_sample(&acc.ds_cmds, &acc.ds_lcd);
    acc.skip_lcd = 0;
}

void modclass_entry(int mod_id)
{
    if (mod_id != acc.mod)
    {
        modclass_top(mod_id);           /* a new visit: its own window */
        return;
    }
    if (mod_id < 0)
        return;
    account();                          /* what came before is booked */
    acc.e0 = current_tick;
    acc.e_reads = 0;
}

void modclass_save(bool begin)
{
    uint32_t lcd;

    if (acc.mod < 0 || thread_self() != acc.tid)
        return;
    if (begin)
    {
        if (!acc.saving++)
            account();                  /* what came before is booked */
        return;
    }
    if (!acc.saving || --acc.saving)
        return;                         /* unpaired, or an inner save */
    /* the save's commands: skipped, not booked as reads */
    ds_sample(&acc.ds_cmds, &lcd);
}

void modclass_page(bool begin)
{
    uint32_t cmds, lcd, d;
    long now = current_tick;

    if (acc.mod < 0 || thread_self() != acc.tid)
        return;
    if (begin)
    {
        account();                      /* what came before is booked */
        acc.paging = true;
        acc.pg_cmds = 0;
        return;
    }
    if (!acc.paging)
        return;
    acc.paging = false;
#ifdef HAVE_DEVSTATS
    ds_sample(&cmds, &lcd);
    d = cmds - acc.ds_cmds;
    acc.ds_cmds = cmds;
#else
    (void)cmds; (void)lcd;
    d = acc.pg_cmds;
#endif
    if (!acc.pages_max)
    {
        /* no declared paging: the commands are the screen's reads */
        if (TIME_BEFORE(now, acc.e0 + ENTRY))
            entry_reads_add(d);
        acc.reads += d;
        return;
    }
    acc.pages++;
    if (d > PAGE_CMDS)
        viol(V_PAGE, "page");
    if (!TIME_BEFORE(now, acc.pg_t0 + HZ))
    {
        acc.pg_t0 = now;                /* a new second */
        acc.pg_n = 0;
    }
    if (++acc.pg_n > acc.pages_max)
        viol(V_PAGES, "pages");
}

void modclass_io(bool write, uint32_t bytes)
{
    (void)bytes;
    if (acc.mod < 0 || thread_self() != acc.tid || acc.saving)
        return;
    if (acc.paging)
    {
#ifndef HAVE_DEVSTATS
        acc.pg_cmds++;                  /* the page load's own */
#endif
        return;
    }
    if (write)
        acc.writes++;
#ifdef HAVE_DEVSTATS
    account();                          /* booked in the second it happened */
#else
    else
    {
        if (TIME_BEFORE(current_tick, acc.e0 + ENTRY))
            entry_reads_add(1);
        acc.reads++;
    }
#endif
}

void modclass_alloc(void)
{
    if (acc.mod >= 0)
        acc.allocs++;
}

void modclass_push(bool frame)
{
    if (acc.mod < 0)
        return;
    /* the target's LCD driver counts every update itself (devstats):
     * what is no frame comes off its count */
#if !defined(HAVE_DEVSTATS) || defined(SIMULATOR)
    if (frame)
    {
        acc.pushes++;
        acc.p_pushes++;
    }
#else
    if (!frame)
        acc.skip_lcd++;
#endif
}

void modclass_pass(void)
{
    if (acc.mod < 0)
        return;
    account();
    if (!TIME_BEFORE(current_tick, acc.p0 + PERIOD))
        report(false);
}
#endif /* IPODUI_MODCLASS */
