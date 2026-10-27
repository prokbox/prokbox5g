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
 * iPod UI shell: automatic battery calibration, the glue (tracker #70;
 * docs/batcal-brief.md sections 1, 2, 4, 5). The logic is in
 * batcal_core.c; this file feeds it and does the file I/O.
 *
 * Sampling: one timeout (the tick ISR) every second, always on: a few
 * reads of state the drivers already keep, then bc_tick(), which writes
 * RAM only (a 1,024 B ring and ~64 B of state). Below 2.5 V (no reading
 * yet at boot) it skips the second.
 *
 * Writing: only on the UI thread, only while the disk already spins and
 * never below disk-safe (bc_write_plan()). The sampler posts one shell
 * wake (np_pb_ab_wake()) when it sees the disk become active with
 * something to write, so the main loop's batcal_service() runs while a
 * refill or a browse has the disk up: zero extra wake-ups, at most one
 * write per 5 min unless a run opened, closed or ended. The idle event
 * (DISK_EVENT_SPINUP) writes when it runs on the UI thread, i.e. in
 * system_flush() at USB connect and at clean shutdown; on the storage
 * thread (as the disk goes to sleep) it does nothing. Each file is
 * written as a temp file, closed, the old one removed, the temp renamed:
 * a power cut leaves the old or the new file, and the loader falls back
 * to a complete temp file when the old one is already gone. While a
 * write or a fit runs the sampler skips its seconds (a few per run).
 *
 * Files (the hidden system folder): batcal.run, the open run (header,
 * samples, CRC-32); batcal.dat, the last 3 fits (text, CRC trailer).
 * Never named on screen.
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

#ifdef HAVE_BATTERY_LEARN
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "storage.h"
#include "ata_idle_notify.h"
#include "powermgmt.h"
#include "power.h"
#include "backlight.h"
#include "timefuncs.h"
#include "string-extra.h"
#if defined(HAVE_USB_CHARGING_ENABLE) && defined(HAVE_USBSTACK)
#include "usb.h"
#endif
#include "ipodui/uilog.h"
#include "ipodui/fe_be.h"             /* P19 S2 */
#include "ipodui/library.h"         /* np_pb_ab_wake() */
#include "ipodui/batcal.h"
#include "ipodui/batcal_core.h"

#define BATCAL_RUN      ROCKBOX_DIR "/batcal.run"
#define BATCAL_RUN_TMP  ROCKBOX_DIR "/batcal.run.tmp"
#define BATCAL_DAT      ROCKBOX_DIR "/batcal.dat"
#define BATCAL_DAT_TMP  ROCKBOX_DIR "/batcal.dat.tmp"
#define BATCAL_DAT_MAX  320         /* 3 fits: ~270 B */
#define WRITE_EVERY     (5 * 60 * HZ)

extern struct battery_tables_t device_battery_tables;  /* powermgmt.c */

typedef char bc_points_match[BC_POINTS == 11 ? 1 : -1]
    __attribute__((unused));

static uint16_t ring[BC_RING];      /* 1,024 B: the static limit */
static struct bc_state st;
static struct bc_fit fits[BC_FITS];
static unsigned short builtin[BC_POINTS];   /* the table at boot */
static bool builtin_default;        /* its isdefault, for the reset */
static struct timeout tmo;
static unsigned int ui_thread;
static long last_write;
static int nfits;
static volatile bool writing, fit_due, urgent;
static bool run_file, dat_dirty, prev_disk;

static bool charge_ok(void)
{
#if defined(HAVE_USB_CHARGING_ENABLE) && defined(HAVE_USBSTACK) \
    && !defined(SIMULATOR)
    return usb_charging_maxcurrent() >= 500;
#else
    return true;
#endif
}

/* The tick ISR, every second: RAM only */
static int tmo_cb(struct timeout *t)
{
    struct bc_in in;
    enum bc_event e;

    (void)t;
    in.mv = battery_voltage();
    if (writing || in.mv < 2500)
        return HZ;
    in.disk = storage_disk_is_active();
    in.busy = in.disk || is_backlight_on(true);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (cpu_frequency > CPUFREQ_NORMAL)
        in.busy = true;
#endif
    in.power = power_input_present();
    in.charge_ok = in.power && charge_ok();
#if CONFIG_CHARGING >= CHARGING_MONITOR
    in.charging = charging_state();
#else
    in.charging = false;
#endif
    e = bc_tick(&st, ring, &in);
    if (e == BC_EV_ENDED)
        fit_due = true;
    if (e >= BC_EV_OPEN)
        urgent = true;
    if (e == BC_EV_ENDED || (in.disk && !prev_disk && st.dirty))
        np_pb_ab_wake();            /* the main loop runs, disk up */
    prev_disk = in.disk;
    return HZ;
}

static void date_now(char *d)
{
#if CONFIG_RTC
    struct tm *tm = get_time();
    if (tm->tm_year >= 120 && valid_time(tm))
    {
        snprintf(d, 11, "%04d-%02d-%02d", tm->tm_year + 1900,
                 tm->tm_mon + 1, tm->tm_mday);
        return;
    }
#endif
    strcpy(d, "-");                 /* the clock isn't set */
}

/* Up to three parts into a temp file, closed, the old file removed, the
 * temp renamed */
static bool put_file(const char *path, const char *tmp, const void *a,
                     size_t al, const void *b, size_t bl, const void *c,
                     size_t cl)
{
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool ok;

    if (fd < 0)
        return false;
    ok = write(fd, a, al) == (ssize_t)al
         && (!bl || write(fd, b, bl) == (ssize_t)bl)
         && (!cl || write(fd, c, cl) == (ssize_t)cl);
    if (close(fd) < 0)
        ok = false;
    if (!ok)
    {
        remove(tmp);
        return false;
    }
    remove(path);
    return rename(tmp, path) >= 0;
}

static int open_either(const char *path, const char *tmp)
{
    int fd = open(path, O_RDONLY);
    return fd >= 0 ? fd : open(tmp, O_RDONLY);   /* removed, not renamed */
}

/* The power thread reads the table: 22 B swapped with IRQs masked */
static void set_table(const unsigned short *t, bool isdefault)
{
    int old = disable_irq_save();
    memcpy(device_battery_tables.discharge, t, sizeof(builtin));
    device_battery_tables.isdefault = isdefault;
    restore_irq(old);
}

static void apply(void)
{
    uint16_t t[BC_POINTS];

    if (!nfits)
        return;
    bc_blend(fits, nfits, t);
    if (!bc_sane(t))
        return;
    if (!device_battery_tables.isdefault)
        ui_log("batcal", "learned table replaces the levels file");
    set_table(t, false);
}

/* UI thread, the disk spinning. clean: written by system_flush(). */
static void write_now(bool clean)
{
    bool safe = battery_level_safe();
    enum bc_write w;

    writing = true;                 /* the sampler skips this second */
    asm volatile ("" ::: "memory"); /* no st access moves above it */
    w = bc_write_plan(&st, safe, run_file);
    if (w == BC_W_RUN)
    {
        struct bc_hdr h = st.h;
        uint32_t crc;
        if (clean)
            h.flags |= BC_F_CLEAN;
        crc = bc_run_crc(&h, ring);
        /* header, samples, CRC: at most 1,048 B, three writes into the
         * sector cache */
        if (put_file(BATCAL_RUN, BATCAL_RUN_TMP, &h, sizeof(h), ring,
                     h.n * 2u, &crc, sizeof(crc)))
        {
            st.dirty = 0;
            run_file = true;
        }
    }
    else if (w == BC_W_REMOVE)
    {
        remove(BATCAL_RUN);
        remove(BATCAL_RUN_TMP);
        run_file = false;
        st.dirty = 0;
    }
    if (dat_dirty && safe)
    {
        char buf[BATCAL_DAT_MAX];
        size_t len = bc_dat_format(buf, sizeof(buf), fits, nfits);
        if (len && put_file(BATCAL_DAT, BATCAL_DAT_TMP, buf, len, NULL, 0,
                            NULL, 0))
            dat_dirty = false;
    }
    last_write = current_tick;
    urgent = false;
    asm volatile ("" ::: "memory"); /* ... nor below the release */
    writing = false;
}

/* A run's end: fit it (or say why not) and close it. UI thread. */
static void finish(enum bc_boot d)
{
    struct bc_fit f;

    writing = true;
    asm volatile ("" ::: "memory");
    if (d == BC_BOOT_FIT)
    {
        bool ok = bc_fit_run(&st.h, ring, st.disksafe, f.mv);
        ui_log("batcal", "fit seq=%lu %s %u %u %u",
               (unsigned long)st.h.seq, ok ? "ok" : "rejected", f.mv[0],
               f.mv[5], f.mv[10]);
        if (ok)
        {
            f.seq = st.h.seq;
            f.hours = (uint32_t)st.h.n * st.h.period / 3600;
            date_now(f.date);
            nfits = bc_fits_add(fits, nfits, &f);
            dat_dirty = true;
            apply();
        }
    }
    else
        ui_log("batcal", "run dropped seq=%lu n=%u flags=%x",
               (unsigned long)st.h.seq, st.h.n, st.h.flags);
    st.h.flags = 0;
    st.h.n = 0;
    st.dirty = 1;
    urgent = true;
    asm volatile ("" ::: "memory");
    writing = false;
}

static bool load_run(void)
{
    uint32_t crc = 0;
    long size;
    bool ok = false;
    int fd = open_either(BATCAL_RUN, BATCAL_RUN_TMP);

    if (fd < 0)
        return false;
    run_file = true;                /* removed if it is no good */
    size = ffilesize(fd);
    if (read(fd, &st.h, sizeof(st.h)) == (ssize_t)sizeof(st.h)
        && st.h.n < BC_RING
        && read(fd, ring, st.h.n * 2u) == (ssize_t)(st.h.n * 2u)
        && read(fd, &crc, 4) == 4)
        ok = bc_run_valid(&st.h, ring, crc, size);
    close(fd);
    return ok;
}

static void load_fits(void)
{
    char buf[BATCAL_DAT_MAX];
    int n, fd = open_either(BATCAL_DAT, BATCAL_DAT_TMP);

    if (fd < 0)
        return;
    n = read(fd, buf, sizeof(buf));
    close(fd);
    nfits = n > 0 ? bc_dat_parse(buf, n, fits) : 0;
    if (!nfits)
        ui_log("batcal", "table file ignored");
}

static void idle_cb(unsigned short id, void *data, void *user)
{
    (void)id; (void)data; (void)user;
    if (!fe_be_is_service_thread())
        return;                     /* the storage thread, disk going down */
    /* system_flush(): USB connect or a shutdown, from any screen (some
     * reach default_event_handler() without batcal_poweroff()). Marked
     * clean either way: at USB the run closes as partial a second later. */
    if (st.h.flags & BC_F_RUN)
        st.dirty = 1;
    write_now(true);
}

void batcal_boot(void)
{
    uint16_t disksafe = *device_battery_tables.disksafe;
    int i;

    ui_thread = thread_self();
    memcpy(builtin, device_battery_tables.discharge, sizeof(builtin));
    builtin_default = device_battery_tables.isdefault;
    load_fits();
    bc_init(&st, disksafe);
    if (load_run())
    {
        enum bc_boot d = bc_boot_decide(&st.h, ring, disksafe);
        if (d == BC_BOOT_KEEP)
        {
            st.h.flags &= ~BC_F_CLEAN;  /* open again until a clean stop */
            st.h.boots++;
            st.run_secs = (uint32_t)st.h.n * st.h.period;
            st.dirty = 1;
            urgent = true;
        }
        else
            finish(d);
    }
    else
    {
        bool stale = run_file;
        bc_init(&st, disksafe);
        st.dirty = stale;           /* a bad file goes at the first write */
    }
    for (i = 0; i < nfits; i++)
        if (fits[i].seq >= st.next_seq)
            st.next_seq = fits[i].seq + 1;
    if (st.h.seq >= st.next_seq)
        st.next_seq = st.h.seq + 1;
    apply();
    if (st.dirty || dat_dirty)
        write_now(false);           /* boot: the disk is up */
    ui_log("batcal", "boot fits=%d run n=%u", nfits, st.h.n);
    add_event_ex(DISK_EVENT_SPINUP, false, idle_cb, NULL);
    timeout_register(&tmo, tmo_cb, HZ, 0);
}

void batcal_service(void)
{
    if (fit_due)
    {
        fit_due = false;
        finish(bc_boot_decide(&st.h, ring, st.disksafe));
    }
    if ((st.dirty || dat_dirty) && storage_disk_is_active()
        && bc_write_due(&st, urgent,
                        TIME_AFTER(current_tick, last_write + WRITE_EVERY)))
        write_now(false);
}

void batcal_poweroff(void)
{
    int old = disable_irq_save();

    if (st.h.flags & BC_F_RUN)
    {
        st.dirty = 1;               /* "clean" goes out with the flush */
        if (query_force_shutdown())
            st.h.flags |= BC_F_ENDING;  /* written only above disk-safe */
    }
    restore_irq(old);
}

/* the developer rows and the diagnostics report (diag.c, any build) */
void batcal_status(char *buf, size_t size)
{
    const char *d = nfits ? fits[nfits - 1].date : "-";

    if (!nfits)
        strlcpy(buf, "Built-in table", size);
    else
        snprintf(buf, size, "Calibrated: %d run%s%s%s", nfits,
                 nfits > 1 ? "s" : "", d[0] == '-' ? "" : ", last ",
                 d[0] == '-' ? "" : d);
}

void batcal_run_status(char *buf, size_t size)
{
    if ((st.h.flags & (BC_F_RUN | BC_F_ENDING)) == BC_F_RUN)
        snprintf(buf, size, "Run: %lu h logged",
                 (unsigned long)(st.h.n * (uint32_t)st.h.period / 3600));
    else
        strlcpy(buf, "Run: waiting for a full charge", size);
}

#ifdef HAVE_IPOD_UI_DEVMODE
void batcal_reset(void)
{
    uint32_t seq = st.next_seq;
    int old = disable_irq_save();

    bc_init(&st, st.disksafe);
    st.next_seq = seq;
    restore_irq(old);
    nfits = 0;
    dat_dirty = run_file = false;
    remove(BATCAL_RUN);
    remove(BATCAL_RUN_TMP);
    remove(BATCAL_DAT);
    remove(BATCAL_DAT_TMP);
    set_table(builtin, builtin_default);
    ui_log("batcal", "reset");
}
#endif /* HAVE_IPOD_UI_DEVMODE */

#endif /* HAVE_BATTERY_LEARN */
