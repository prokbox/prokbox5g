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
 * iPod UI shell: the developer power log (tracker #10; docs/phase9-brief.md
 * section 3.6.5 and section 12 items 2, 6, 8, 10). Rundowns and A/B
 * battery tests read /.rockbox/power.csv; docs/hardware.md says how.
 *
 * Sampling: one timeout (timeout_register, the tick ISR) every HZ/4 while
 * the log is on, nothing while it is off. Each call counts the boosted,
 * backlit, LCD-awake and disk-active sub-samples; every interval (5, 30
 * or 60 s) it pushes one 20-byte row into a ring (powerlog_core.c: 192
 * rows, 6,348 B with the marks), a pinned core_alloc made when the log
 * goes on and freed when it goes off, so a unit with the log off (every
 * one but a developer's) keeps the RAM. The callback only writes RAM.
 *
 * Writing: a persistent DISK_EVENT_SPINUP handler, i.e. the storage
 * thread as the disk is about to idle (or the UI thread in misc.c's
 * system_flush(), at USB connect before the acknowledgement and at clean
 * shutdown), appends the pending rows to power.csv in one open/append/
 * close. The disk is spinning then anyway: no wake-up. It writes only
 * when a mark is pending, the ring is 3/4 full or 5 min have passed since
 * the last write. The one forced write is the UI thread's, in
 * powerlog_service(), when the ring reaches 7/8 because the disk hasn't
 * gone idle for that long (a 5 s interval with the screen off). Each line
 * is one write() into the file's sector cache, so the disk sees whole
 * sectors (about 11 for a full ring).
 *
 * Locks: none. The ISR and the two writers share the ring under
 * disable_irq_save() around a 20-byte copy; the writer formats from its
 * own copy and holds no pointer into the ring across write(), which
 * yields. A flag keeps the storage and UI threads' writes apart (both on
 * the CPU core, cooperative). Never a UI mutex.
 *
 * Files: power.csv appends; past POWERLOG_MAX it moves to power.csv.1
 * (one generation: remove the old .1, then rename, never a rename over
 * it) on the UI thread only (never on the storage thread's small stack:
 * see uilog.c), at a flush it makes itself (the forced write, the log
 * going off) or, when the storage thread's idle write found it due, at
 * the next main-loop pass that sees the disk spinning. Never in the
 * system_flush() at USB connect (that runs idle_cb: it only marks it). power.cfg, present while the log is on,
 * holds the interval and is read once at boot, so a 24 h rundown carries
 * on across a restart (a "boot" mark row, with the clock, shows it).
 *
 * Fit-1 M0 (docs/fit1-brief.md section A): each periodic row also carries
 * the interval's CPU idle and top two threads by CPU time, and the disk
 * commands, KB, wakes and busiest disk thread, from devstats (firmware/
 * export/devstats.h; the ISR reads its counters, RAM only). Thread names
 * are resolved at the write. powerlog_service() also turns devstats into
 * ipodui.log lines whether the log is on or not (developer builds):
 *   m0 tap2pcm ms=M why=play|skip|seek|resume
 *                       a request to the playback channel's first PCM
 *   m0 disk wake by=T rd|wr n=S play=P
 *                       a command that found the disk asleep (at most
 *                       M0_WAKE_LINES per pass; "m0 disk wakes more=N")
 *   m0 disk sleeps=N fails=F resets=R
 *                       on a change of F or R (a disk that can't enter
 *                       standby stays "active"; R: failed commands)
 * The main loop runs at least once a minute, lit or dark, so a line is at
 * most a minute late; ui_log() writes at the next storage idle.
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

#ifdef HAVE_IPOD_UI_DEVMODE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "storage.h"
#include "ata_idle_notify.h"
#include "powermgmt.h"
#include "power.h"
#include "backlight.h"
#include "lcd.h"
#include "audio.h"
#include "settings.h"
#include "timefuncs.h"
#include "debug.h"
#include "dac_ext.h"
#include "string-extra.h"
#include "core_alloc.h"
#include "ipodui/uilog.h"
#include "ipodui/powerlog.h"

#ifdef SIMULATOR
long powerlog_max = POWERLOG_MAX;
#define PL_MAX powerlog_max
#else
#define PL_MAX POWERLOG_MAX
#endif
#include "ipodui/powerlog_core.h"
#include "devstats.h"
#include "ipodui/udmatest.h"
#include "ipodui/devmode.h"
#include "ipodui/devstatus.h"
#ifdef HAVE_STORAGE_POWEROFF
#include "stor_pwr.h"           /* #114: the adapter's power */
#endif

#define SUB_TICKS       (HZ / 4)
#define SUBS_PER_SEC    4
#define FLUSH_EVERY     (5 * 60 * HZ)       /* the idle write's minimum age */
#define FLUSH_SOON      (PL_ROWS * 3 / 4)   /* ... unless this full */
#define FLUSH_FORCE     (PL_ROWS * 7 / 8)   /* the UI thread writes anyway */

typedef char pl_budget[sizeof(struct pl_ring) <= POWERLOG_RING_BYTES
                       ? 1 : -1] __attribute__((unused));

static struct pl_ring *ring;        /* in ring_h while on, else NULL */
static int ring_h;
static struct pl_acc acc;
static struct timeout tmo;
static int interval = POWERLOG_PERIOD;
static bool on, idle_registered, flushing, rotate_due, mark_pending;
static long last_flush;
static long force_after;            /* a failed forced write waits 60 s */
static unsigned short forced;

#ifdef HAVE_DEVSTATS
/* M0: the counters at the last periodic row (ISR only after start()) */
static uint32_t m0_cpu[DEVSTATS_SLOTS], m0_cmds[DEVSTATS_SLOTS];
static uint32_t m0_sectors, m0_wakes;
#if !defined(SIMULATOR)
static uint32_t m0_idle[NUM_CORES], m0_wall;

static uint8_t m0_pct(uint32_t part, uint32_t whole)
{
    uint32_t d = (whole + 50) / 100;

    if (!d)
        return PL_UNKNOWN;
    part /= d;
    return part > 100 ? 100 : (uint8_t)part;
}
#endif /* !SIMULATOR: the sim has no CPU time */

/* Takes the counters as the next row's base; fills w when non-NULL */
static void m0_row(struct pl_row *w)
{
    uint32_t cmds = 0, sectors = 0, wakes = 0, best = 0;
    unsigned int i, by = PL_UNKNOWN;
#if !defined(SIMULATOR)
    uint32_t now = USEC_TIMER, wall = now - m0_wall;
    uint32_t c1 = 0, c2 = 0;
    unsigned int s1 = PL_UNKNOWN, s2 = PL_UNKNOWN;
#endif

    for (i = 0; i < DEVSTATS_SLOTS; i++)
    {
        const struct devstats_thread *t = &devstats.t[i];
        uint32_t dc = t->cmds - m0_cmds[i];
#if !defined(SIMULATOR)
        uint32_t du = t->cpu_us - m0_cpu[i];
        if (du > c1)
        {
            c2 = c1; s2 = s1;
            c1 = du; s1 = i;
        }
        else if (du > c2)
        {
            c2 = du; s2 = i;
        }
#endif
        if (dc > best)
        {
            best = dc;
            by = i;
        }
        cmds += dc;
        sectors += t->sectors;
        wakes += t->wakes;
        m0_cmds[i] = t->cmds;
        m0_cpu[i] = t->cpu_us;
    }
    if (w)
    {
        uint32_t ds = sectors - m0_sectors, dw = wakes - m0_wakes;
        w->disk_cmds = cmds > 0xffff ? 0xffff : (uint16_t)cmds;
        w->disk_kb = ds / 2 > 0xffff ? 0xffff : (uint16_t)(ds / 2);
        w->disk_wakes = dw > 0xff ? 0xff : (uint8_t)dw;
        w->disk_by = (uint8_t)by;
#if !defined(SIMULATOR)
        w->idle_pct = m0_pct(devstats.idle_us[CPU] - m0_idle[CPU], wall);
#if NUM_CORES > 1
        w->cop_idle_pct = m0_pct(devstats.idle_us[COP] - m0_idle[COP], wall);
#endif
        w->cpu1_slot = (uint8_t)s1;
        w->cpu1_pct = s1 == PL_UNKNOWN ? PL_UNKNOWN : m0_pct(c1, wall);
        w->cpu2_slot = (uint8_t)s2;
        w->cpu2_pct = s2 == PL_UNKNOWN ? PL_UNKNOWN : m0_pct(c2, wall);
#endif
    }
    m0_sectors = sectors;
    m0_wakes = wakes;
#if !defined(SIMULATOR)
    for (i = 0; i < NUM_CORES; i++)
        m0_idle[i] = devstats.idle_us[i];
    m0_wall = now;
#endif
}
#endif /* HAVE_DEVSTATS */

/* The state now, with the accumulated shares (ISR or IRQs masked) */
static void sample(struct pl_row *w)
{
    int lvl = battery_level();
    int vol = global_status.volume;    /* dB */
    int st = audio_status();

    memset(w, 0, sizeof(*w));
    w->secs = (uint32_t)(current_tick / HZ);
    w->mv = (uint16_t)battery_voltage();
    w->pct = (lvl < 0 || lvl > 100) ? 255 : (uint8_t)lvl;
#if CONFIG_CHARGING
    if (charger_inserted())
        w->flags |= PL_F_CHARGER;
#if CONFIG_CHARGING >= CHARGING_MONITOR
    if (charging_state())
        w->flags |= PL_F_CHARGING;
#endif
#endif
    if (st & AUDIO_STATUS_PLAY)
        w->flags |= PL_F_PLAYING;
    if (st & AUDIO_STATUS_PAUSE)
        w->flags |= PL_F_PAUSED;
#ifdef HAVE_DAC_STANDBY
    {
        struct dac_stats ds;
        dac_get_stats(&ds);         /* a struct copy; counters may tear by
                                     * one in a sub-sample, harmless */
        if (ds.standby)
            w->flags |= PL_F_DAC_SB;
        if (dac_standby_allowed())
            w->flags |= PL_F_DAC_ALLOW;
        w->dac_sleeps = (uint16_t)ds.sleeps;
        w->dac_wakes = (uint16_t)ds.wakes;
    }
#endif
    w->vol = (int8_t)(vol < -128 ? -128 : vol > 127 ? 127 : vol);
    w->boost_pct = pl_acc_pct(acc.boost, acc.n);
    w->bl_pct = pl_acc_pct(acc.bl, acc.n);
    w->lcd_pct = pl_acc_pct(acc.lcd, acc.n);
    w->disk_pct = pl_acc_pct(acc.disk, acc.n);
    w->stor_on_pct = pl_acc_pct(acc.stor, acc.n);
#ifdef HAVE_STORAGE_POWEROFF
    {
        uint32_t ons = storage_power_state()->ons;
        w->stor_ons_total = ons > 0xffff ? 0xffff : (uint16_t)ons;
    }
#endif
    /* M0: none in a mark row (the periodic row covers the interval) */
    w->idle_pct = w->cop_idle_pct = PL_UNKNOWN;
    w->cpu1_slot = w->cpu1_pct = w->cpu2_slot = w->cpu2_pct = PL_UNKNOWN;
    w->disk_by = PL_UNKNOWN;
}

/* The tick ISR, every SUB_TICKS while on: RAM only */
static int tmo_cb(struct timeout *t)
{
    (void)t;
    acc.n++;
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (cpu_frequency > CPUFREQ_NORMAL)
        acc.boost++;
#endif
    if (is_backlight_on(true))
        acc.bl++;
#if defined(HAVE_LCD_ENABLE) || defined(HAVE_LCD_SLEEP)
    if (lcd_active())
        acc.lcd++;
#else
    acc.lcd++;
#endif
    if (storage_disk_is_active())
        acc.disk++;
#ifdef HAVE_STORAGE_POWEROFF
    if (storage_powered())
#endif
        acc.stor++;                 /* without the option: always powered */
    if (acc.n >= interval * SUBS_PER_SEC)
    {
        struct pl_row w;
        sample(&w);
#ifdef HAVE_DEVSTATS
        m0_row(&w);
#endif
#ifdef HAVE_STORAGE_POWEROFF
        {
            static uint32_t ons_base;   /* at the last periodic row */
            uint32_t ons = storage_power_state()->ons, d = ons - ons_base;
            w.stor_ons = d > 255 ? 255 : (uint8_t)d;
            ons_base = ons;
        }
#endif
        pl_ring_push(ring, &w);
        memset(&acc, 0, sizeof(acc));
    }
    return SUB_TICKS;
}

void powerlog_mark(const char *tag)
{
    struct pl_row w;
    int old;

    if (!on || !ring)
        return;
#if NUM_CORES > 1
    if (CURRENT_CORE != CPU)
        return;                     /* the ring is the CPU core's */
#endif
    old = disable_irq_save();
    sample(&w);                     /* the shares so far; the interval's
                                     * own row still comes on time */
    w.mark = pl_ring_mark(ring, tag);
    pl_ring_push(ring, &w);
    mark_pending = true;
    restore_irq(old);
}

static void rotate(void)             /* UI thread only */
{
    remove(POWERLOG_FILE ".1");
    if (rename(POWERLOG_FILE, POWERLOG_FILE ".1") < 0)
        ui_log("power", "log rotate failed");   /* due again next flush */
    rotate_due = false;
}

/* The thread name of a row's slot field, for the write */
static const char *slot_name(char *buf, uint8_t slot)
{
    if (slot == PL_UNKNOWN)
        return NULL;
#ifdef HAVE_DEVSTATS
    devstats_slot_name(slot, buf, PL_NAME_LEN);
    return buf;
#else
    (void)buf;
    return NULL;
#endif
}

/* Appends every pending row: one open, one write() per line into the
 * sector cache, one close. The line and names are static (the storage
 * thread's stack is small); the flushing flag keeps the two writers
 * apart. Stack: a 32 B row + the tag. ui: the UI thread's call, which
 * may rotate a file past POWERLOG_MAX first (the storage thread's only
 * marks it due). */
static char line[PL_LINE_MAX];
static char names_buf[3][PL_NAME_LEN];
static void flush(bool ui)
{
    char *buf = line;
    char tag[PL_TAG_LEN];
    struct pl_row w;
    unsigned int lines = 0;
    off_t size;
    int fd;

    if (flushing || !ring || pl_ring_pending(ring) == 0)
        return;
    flushing = true;
    mark_pending = false;           /* a mark made during this write is
                                     * pushed after the snapshot below */
    fd = open(POWERLOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
    {
        mark_pending = true;        /* kept: retried at the next idle */
        flushing = false;
        return;
    }
    size = ffilesize(fd);
    if (size > PL_MAX)
    {
        rotate_due = true;          /* the UI thread renames it, later */
        if (ui)
        {
            close(fd);
            rotate();
            fd = open(POWERLOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
            if (fd < 0)
            {
                mark_pending = true;
                flushing = false;
                return;
            }
            size = ffilesize(fd);   /* 0 unless the rename failed */
        }
    }
    if (size == 0)
        write(fd, pl_csv_header, strlen(pl_csv_header));
    for (;;)
    {
        uint32_t t;
        int old, n;

        old = disable_irq_save();
        t = ring->tail;
        if (t == ring->head)
        {
            restore_irq(old);
            break;
        }
        w = ring->rows[t % PL_ROWS];
        strlcpy(tag, pl_ring_tag(ring, w.mark), sizeof(tag));
        restore_irq(old);

        {
            const char *names[3];
            names[0] = slot_name(names_buf[0], w.cpu1_slot);
            names[1] = slot_name(names_buf[1], w.cpu2_slot);
            names[2] = slot_name(names_buf[2], w.disk_by);
            n = pl_format_row(buf, sizeof(line), &w, tag, names);
        }
        if (write(fd, buf, n) != n)
            break;                  /* kept: retried at the next flush */
        lines++;
        old = disable_irq_save();
        if (ring->tail == t)        /* else the ISR dropped it meanwhile */
            ring->tail = t + 1;
        restore_irq(old);
    }
    close(fd);
    last_flush = current_tick;
    flushing = false;
    DEBUGF("ipodui: powerlog flush rows=%u\n", lines);
    (void)lines;
}

static void idle_cb(unsigned short id, void *data, void *user)
{
    uint32_t pending;

    (void)id; (void)data; (void)user;
    if (!ring || (pending = pl_ring_pending(ring)) == 0)
        return;
    if (mark_pending || pending >= FLUSH_SOON
        || TIME_AFTER(current_tick, last_flush + FLUSH_EVERY))
        flush(false);               /* the disk spins: no wake-up */
}

#include "events.h"     /* P19: sys_events_peak() */
#ifdef HAVE_DEVSTATS
#define M0_WAKE_LINES   4       /* per pass; the rest are counted */
static uint16_t m0_pcm_seq, m0_fails, m0_resets;
static uint8_t m0_irq_off;
static uint32_t m0_wake_done;

/* P18 18-A (M1): per-thread wakes, a few lines per minute (ui_log lines
 * are 64 chars):
 *   m0 wakes win=S total=N rate=R        R in hundredths of a wake per s
 *   m0 wakes <thread> n=N                the busiest threads, most first
 *   m0 polls a=W/T s=W/T t=W/T p=W/T     P19 19-1a, at most every 10 min:
 *                                        audio, storage, tagcache, power
 *                                        waits/timeouts since the last
 * A wake is a thread made runnable (a wait ended: an event or a timeout),
 * counted by the kernel (DEVSTATS_THREAD_WAKE).  The UI thread's own
 * passes are in it. */
#define M0_WAKES_WIN    (60 * HZ)
#define M0_WAKES_TOP    6       /* npfix: was 12; the ring holds 12 lines */
#define M0_WAKES_REPEAT (10 * 60 * HZ)  /* an unchanged window, at most */
static uint32_t m0_thr_base[DEVSTATS_SLOTS];
static long m0_thr_at;
static uint32_t m0_sig;         /* npfix: the last logged window's shape */
static long m0_sig_at;

/* npfix: the longest single run of any CPU-core thread this window
 * ("m0 slice max thread=T us=U"): the cooperative kernel only switches
 * at a yield, so this is the most a woken UI thread can have waited for
 * a lower-priority thread. Logged when over 50 ms; cleared each window. */
static void m0_slices(void)
{
    uint32_t best = 0;
    unsigned int i, slot = 0;
    char name[PL_NAME_LEN];

    for (i = 0; i < DEVSTATS_SLOTS; i++)
    {
        uint32_t v = devstats.t[i].slice_max_us;
        devstats.t[i].slice_max_us = 0;
        if (v > best)
        {
            best = v;
            slot = i;
        }
    }
    if (best <= 50000)
        return;
    devstats_slot_name(slot, name, sizeof(name));
    ui_log("m0", "slice max thread=%s us=%lu", name, (unsigned long)best);
}

static void m0_thr_wakes(void)
{
    uint32_t d[DEVSTATS_SLOTS], total = 0, secs;
    unsigned int top[M0_WAKES_TOP], i, j, n = 0;
    char name[PL_NAME_LEN];

    if (m0_thr_at == 0)
    {
        m0_thr_at = current_tick;
        for (i = 0; i < DEVSTATS_SLOTS; i++)
            m0_thr_base[i] = devstats.t[i].thr_wakes;
        return;
    }
    if (current_tick - m0_thr_at < M0_WAKES_WIN)
        return;
    secs = (current_tick - m0_thr_at) / HZ;
    for (i = 0; i < DEVSTATS_SLOTS; i++)
    {
        uint32_t now = devstats.t[i].thr_wakes;
        d[i] = now - m0_thr_base[i];
        m0_thr_base[i] = now;
        total += d[i];
        if (!d[i])
            continue;
        for (j = n; j > 0 && d[top[j - 1]] < d[i]; j--)
            if (j < M0_WAKES_TOP)
                top[j] = top[j - 1];
        if (j < M0_WAKES_TOP)
        {
            top[j] = i;
            if (n < M0_WAKES_TOP)
                n++;
        }
    }
    m0_thr_at = current_tick;
    m0_slices();
    {
        /* npfix: the same shape as the last logged window (the top three
         * threads, the rate within a factor of two) is not logged again
         * for 10 min: it filled the 12-line ring during playback */
        uint32_t sig = 32 - (uint32_t)__builtin_clz((total / (secs ? secs : 1)) | 1);
        for (i = 0; i < n && i < 3; i++)
            sig = sig * 31 + top[i] + 1;
        if (sig == m0_sig && m0_sig_at
            && TIME_BEFORE(current_tick, m0_sig_at + M0_WAKES_REPEAT))
            return;
        m0_sig = sig;
        m0_sig_at = current_tick;
    }
    ui_log("m0", "wakes win=%lu total=%lu rate=%lu", (unsigned long)secs,
           (unsigned long)total,
           (unsigned long)(total * 100 / (secs ? secs : 1)));
    for (i = 0; i < n; i++)
    {
        devstats_slot_name(top[i], name, sizeof(name));
        ui_log("m0", "wakes %s n=%lu", name, (unsigned long)d[top[i]]);
    }
    {
        /* P19 19-1a (19-1f): the four pollers' waits since the last
         * polls line (a window not logged above rolls into the next),
         * and how many ended at their deadline (waits/timeouts): audio,
         * storage, tagcache, power */
        static uint32_t pw[4], pt[4];
        static long pat;
        uint32_t w[4], t[4];
        /* one line per 10 min at most: the ring is 12 lines */
        if (pat && TIME_BEFORE(current_tick, pat + M0_WAKES_REPEAT))
            return;
        pat = current_tick;
        for (i = 0; i < 4; i++)
        {
            w[i] = devstats.poll_waits[i] - pw[i];
            t[i] = devstats.poll_tmo[i] - pt[i];
            pw[i] = devstats.poll_waits[i];
            pt[i] = devstats.poll_tmo[i];
        }
        ui_log("m0", "polls a=%lu/%lu s=%lu/%lu t=%lu/%lu p=%lu/%lu ev=%u/%u",
               (unsigned long)w[0], (unsigned long)t[0],
               (unsigned long)w[1], (unsigned long)t[1],
               (unsigned long)w[2], (unsigned long)t[2],
               (unsigned long)w[3], (unsigned long)t[3],
               sys_events_peak(), sys_events_max());
    }
}

/* UI thread, every main-loop pass: devstats into ipodui.log lines */
static void m0_service(void)
{
    static const char *const why[] = { "?", "play", "skip", "seek",
                                       "resume" };
    struct devstats d;
    char name[PL_NAME_LEN];
    uint32_t i, n;
    int st;

    m0_thr_wakes();
    if (devstats.pcm_seq == m0_pcm_seq && devstats.wake_head == m0_wake_done
        && devstats.sleep_fails == m0_fails && devstats.resets == m0_resets
        && devstats.irq_off == m0_irq_off)
        return;                     /* the common case: nothing new */
    devstats_snapshot(&d);
    st = audio_status();
    if (d.pcm_seq != m0_pcm_seq)
    {
        m0_pcm_seq = d.pcm_seq;
        if (d.pcm_seq == d.req_seq) /* closed by a PCM start */
        {
            uint32_t ms = (d.pcm_us - d.req_us) / 1000;
            if (ms < 60000)
                ui_log("m0", "tap2pcm ms=%lu why=%s", (unsigned long)ms,
                       why[d.req_why < 5 ? d.req_why : 0]);
        }
    }
    n = d.wake_head - m0_wake_done;
    if (n > DEVSTATS_WAKES)         /* the ring lapped: the oldest lost */
        m0_wake_done = d.wake_head - DEVSTATS_WAKES;
    for (i = 0; m0_wake_done != d.wake_head && i < M0_WAKE_LINES; i++)
    {
        const struct devstats_wake *w =
            &d.wake[m0_wake_done++ % DEVSTATS_WAKES];
        devstats_slot_name(w->slot, name, sizeof(name));
        ui_log("m0", "disk wake by=%s %s n=%u play=%d t=%lu", name,
               w->write ? "wr" : "rd", (unsigned)w->count,
               (st & AUDIO_STATUS_PAUSE) ? 2 : (st & AUDIO_STATUS_PLAY) ? 1 : 0,
               (unsigned long)(w->tick / HZ));
    }
    if (m0_wake_done != d.wake_head)
    {
        ui_log("m0", "disk wakes more=%lu",
               (unsigned long)(d.wake_head - m0_wake_done));
        m0_wake_done = d.wake_head;
    }
    if (d.sleep_fails != m0_fails || d.resets != m0_resets
        || d.irq_off != m0_irq_off)
    {
        m0_fails = d.sleep_fails;
        m0_resets = d.resets;
        m0_irq_off = d.irq_off;
        ui_log("m0", "disk sleeps=%u fails=%u resets=%u irq=%lu%s",
               (unsigned)d.sleeps, (unsigned)d.sleep_fails,
               (unsigned)d.resets, (unsigned long)d.irq_waits,
               d.irq_off ? " polling" : "");
    }
}
#endif /* HAVE_DEVSTATS */

#ifdef HAVE_STORAGE_POWEROFF
/* #114, UI thread: the adapter's power changes into ipodui.log ("stor pwr
 * off ms=<on time> reason=..", "stor pwr on ms=<re-init> reason=.."), and
 * once an hour the cycle count and on time ("stor pwr hour"); the hour
 * line comes from the counters, so it is right even when the event ring
 * lapped while this thread slept */
static uint32_t stor_done, stor_h_ons, stor_h_wasted, stor_h_on;
static long stor_h_at;

static void stor_service(void)
{
    const struct stor_pwr *s = storage_power_state();
    const struct stor_pwr_event *e;
    uint32_t head = s->ev_head;

    if (head - stor_done > STOR_PWR_EVENTS)
    {
        ui_log("stor", "pwr events lost=%lu",
               (unsigned long)(head - stor_done - STOR_PWR_EVENTS));
        stor_done = head - STOR_PWR_EVENTS;
    }
    while (stor_done != head && (e = stor_pwr_event_at(s, stor_done)))
    {
        ui_log("stor", "pwr %s ms=%u reason=%s t=%lu", e->on ? "on" : "off",
               (unsigned)e->ms, stor_pwr_reason_name(e->reason),
               (unsigned long)(e->tick / HZ));
        stor_done++;
    }
    if (!stor_h_at)
        stor_h_at = current_tick;
    else if (TIME_AFTER(current_tick, stor_h_at + 3600 * HZ))
    {
        long span = current_tick - stor_h_at;
        uint32_t on = stor_pwr_on_ticks(s, current_tick);
        ui_log("stor", "pwr hour span_s=%ld cycles=%lu wasted=%lu "
               "on_pct=%lu allow=%d", span / HZ,
               (unsigned long)(s->ons - stor_h_ons),
               (unsigned long)(s->wasted - stor_h_wasted),
               (unsigned long)((uint64_t)(on - stor_h_on) * 100 / span),
               storage_poweroff_allowed() ? 1 : 0);
        stor_h_at = current_tick;
        stor_h_ons = s->ons;
        stor_h_wasted = s->wasted;
        stor_h_on = on;
    }
}
#endif

void powerlog_service(void)
{
#ifdef HAVE_STORAGE_POWEROFF
    stor_service();
    ipodui_devmode_ab_service();    /* #114 H3: the A/B's switches */
#endif
#ifdef HAVE_DEVSTATS
    m0_service();
    ipodui_devmode_f3_service();    /* Fit-3 step 0 */
#endif
    udma_service();                 /* C7: persist a UDMA2 fallback */
    if (flushing)
        return;
    if (rotate_due && storage_disk_is_active())
    {
        flushing = true;            /* keeps the storage thread's write out */
        rotate();
        flushing = false;
    }
    if (on && pl_ring_pending(ring) >= FLUSH_FORCE
        && TIME_AFTER(current_tick, force_after))
    {
        force_after = current_tick + 60 * HZ;   /* never a retry loop */
        forced++;
        ui_log("power", "log forced write n=%u", forced);
        flush(true);                /* wakes the disk: overflow otherwise */
    }
}

static void save_cfg(void)           /* UI thread, a developer action */
{
    char b[8];
    int fd = open(POWERLOG_CFG, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd >= 0)
    {
        int n = snprintf(b, sizeof(b), "%d\n", interval);
        write(fd, b, n);
        close(fd);
    }
}

/* "<word> YYYY-MM-DDTHH:MM", within PL_TAG_LEN */
static void mark_dated(const char *word)
{
    char tag[PL_TAG_LEN];
#if CONFIG_RTC
    struct tm *tm = get_time();
    snprintf(tag, sizeof(tag), "%s %04d-%02d-%02dT%02d:%02d", word,
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
             tm->tm_min);
#else
    strlcpy(tag, word, sizeof(tag));
#endif
    powerlog_mark(tag);
}

static void start(const char *why)
{
    if (on)
        return;
    /* Pinned: the tick ISR writes into it. A developer action (or the
     * boot resume, before playback): never a playback-time allocation in
     * normal use. */
    ring_h = core_alloc(sizeof(*ring));
    if (ring_h <= 0)
    {
        ring_h = 0;
        ui_log("power", "log on failed: no RAM (%u B)",
               (unsigned)sizeof(*ring));
        return;
    }
    core_pin(ring_h);
    ring = core_get_data(ring_h);
    pl_ring_reset(ring);
    memset(&acc, 0, sizeof(acc));
    if (!idle_registered)
        idle_registered = add_event_ex(DISK_EVENT_SPINUP, false, idle_cb,
                                       NULL);
    last_flush = current_tick;
#ifdef HAVE_DEVSTATS
    {
        int old = disable_irq_save();
        m0_row(NULL);               /* the first row's base */
        restore_irq(old);
    }
#endif
    on = true;
    timeout_register(&tmo, tmo_cb, SUB_TICKS, 0);
    mark_dated(why);
    ui_log("power", "log on every=%ds", interval);
}

void powerlog_boot(void)
{
    char b[8];
    int fd;
    int n, s;

    udma_boot();                    /* C7: the developer UDMA4 switch */
#ifdef HAVE_BCMFW_DEVTEST
    devswitch_boot();               /* a bench session's switches left
                                     * behind by a closed session: undone */
#endif
#ifdef HAVE_STORAGE_POWEROFF
    /* #114's A/B: the drive kept in standby, not powered off */
    if (file_exists(POWERLOG_STOR_STANDBY))
    {
        storage_set_poweroff(false);
        ui_log("stor", "pwr standby only (marker)");
    }
#endif
    fd = open(POWERLOG_CFG, O_RDONLY);

    if (fd < 0)
        return;                     /* off: the common case, nothing runs */
    n = read(fd, b, sizeof(b) - 1);
    close(fd);
    b[n > 0 ? n : 0] = '\0';
    s = atoi(b);
    if (s >= 1 && s <= 3600)
        interval = s;
    start("boot");
}

void powerlog_set(bool want)
{
    if (want == on)
        return;
    if (want)
    {
        start("on");
        save_cfg();
        return;
    }
    powerlog_mark("off");
    on = false;
    timeout_cancel(&tmo);
    flush(true);                    /* a developer action: the disk may
                                     * spin up for it */
    if (idle_registered)
    {
        remove_event_ex(DISK_EVENT_SPINUP, idle_cb, NULL);
        idle_registered = false;
    }
    remove(POWERLOG_CFG);
    while (flushing)
        yield();                    /* the storage thread's write, if it
                                     * yielded inside one: it reads ring */
    ring = NULL;
    ring_h = core_free(ring_h);
    ui_log("power", "log off forced=%u", forced);
}

bool powerlog_is_on(void)
{
    return on;
}

void powerlog_set_interval(int secs)
{
    int old;

    if (secs < 1 || secs > 3600)
        return;
    old = disable_irq_save();
    interval = secs;                /* the ISR reads it */
    restore_irq(old);
    if (on)
    {
        char tag[PL_TAG_LEN];
        snprintf(tag, sizeof(tag), "every=%ds", secs);
        save_cfg();
        powerlog_mark(tag);
    }
}

int powerlog_interval(void)
{
    return interval;
}

#endif /* HAVE_IPOD_UI_DEVMODE */
