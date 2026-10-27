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

/* Boot battery guard: sampling, the drained latch and the refusal
 * (bootbatt.h). */
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "power.h"
#include "powermgmt.h"
#include "storage.h"
#include "file.h"
#include "usb.h"
#include "lcd.h"
#include "rtc.h"
#include <string.h>
#ifdef SIMULATOR
#include "backlight-sim.h"
#else
#include "backlight-target.h"
#endif
#include "ipodui/anim.h"            /* UPDATE_TRACE */
#include "ipodui/uilog.h"
#include "ipodui/library.h"         /* np_pb_ab_wake() */
#include "ipodui/sysscreen.h"
#include "ipodui/bootbatt.h"
#include "ipodui/bootbatt_core.h"
#ifdef SIMULATOR
#include <stdlib.h>
#include "sim_capture.h"
int sim_write_count(void);          /* filesystem-sim.c */
static int sim_writes0;             /* at the sample */
#endif

#define HOLD_TICKS      (2 * HZ)    /* the screen, as the shell's */
#define POLL_TICKS      (HZ / 10)   /* the power inputs during it */
#define SPINDOWN_TICKS  (3 * HZ)    /* stage 2's refusal: the disk's wait */

/* Hidden like the #91 intent record: one sector, written in place */
#define LATCH_FILE      ROCKBOX_DIR "/.bootbatt"

static enum bb_verdict verdict = BB_BOOT_NOREAD;  /* until sampled */
static short sample_mv;
static bool storage_up;             /* stage 2: the refusal parks the disk */
static bool latch_on_disk;          /* the record last read or written: drained */
static uint32_t latch_seq;
static long zero_tick;              /* first service pass seen at 0 %, or 0 */
/* the record's health: 0 good, -1 missing, -2 unusable (truncated: removed
 * and made again), -3 I/O, -4 not a record (rewritten in place) */
static signed char rec_rc = -1;
static long repair_next;            /* no repair before this tick */
static unsigned char repair_shift;  /* backoff: 1 min << shift, to 64 min */

/* the 0 % write waits this long for the disk to be awake anyway (a
 * refill), then accepts one wake; a failed write retries after it */
#define ZERO_WAIT_TICKS (30 * HZ)
#define REPAIR_TICKS    (60 * HZ)
#define REPAIR_MAX_SHIFT 6

extern unsigned short percent_to_volt_discharge[11];  /* the table in use */

/* the main loop may block for long while lit and idle: a 10 s tick
 * check (compares only, no I/O) wakes it while the 0 % write is owed and
 * still possible (over disk-safe: not when P18 sleep or the shutoff
 * takes over below it) */
#define ZERO_CHECK_TICKS (10 * HZ)
static struct timeout zero_tmo;

static int zero_tmo_cb(struct timeout *t)
{
    (void)t;
    if (!latch_on_disk && battery_level_safe()
        && battery_voltage() <= percent_to_volt_discharge[0]
#if CONFIG_CHARGING
        && !charger_inserted()
#endif
       )
        np_pb_ab_wake();
    return ZERO_CHECK_TICKS;
}

static bool power_present(void)
{
#if CONFIG_CHARGING
    /* any input, or a charger (the sim reports a bare 0xf0) */
    return (power_input_status() & (POWER_INPUT | POWER_INPUT_CHARGER)) != 0;
#else
    return false;
#endif
}

/* a USB host has the disk: no write of ours */
static bool host_owns_disk(void)
{
#ifdef USB_FULL_INIT
    return usb_exclusive_storage();
#else
    return usb_inserted();
#endif
}

/* the record: target, one sector in place (file.c); the simulator, a
 * plain host file, used only by scripts that set IPODUI_SIM_DRAINED
 * (0 or 1, written at boot before the read) so a record one script
 * leaves never decides another's boot. Reads return the size, -1
 * missing, -2 unusable (too small), -3 I/O. */
#ifdef SIMULATOR
static bool sim_latch_on;

static int latch_read(struct bb_latch_rec *r)
{
    int fd, n;

    if (!sim_latch_on)
        return -1;
    fd = open(LATCH_FILE, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, r, sizeof(*r));
    close(fd);
    return n == (int)sizeof(*r) ? n : -2;
}

static int latch_write(const struct bb_latch_rec *r)
{
    int fd, n;

    if (!sim_latch_on)
        return -1;
    fd = open(LATCH_FILE, O_WRONLY);    /* never creates: as file.c */
    if (fd < 0)
        return -1;
    n = write(fd, r, sizeof(*r));
    close(fd);
    return n == (int)sizeof(*r) ? n : -3;
}

static int latch_create(void)
{
    struct bb_latch_rec z;
    int fd;

    if (!sim_latch_on)
        return -1;
    fd = open(LATCH_FILE, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd >= 0)
    {
        memset(&z, 0, sizeof(z));
        write(fd, &z, sizeof(z));
        close(fd);
    }
    return file_exists(LATCH_FILE) ? 0 : -1;
}

static void sim_latch_seed(void)
{
    const char *e = getenv("IPODUI_SIM_DRAINED");
    struct bb_latch_rec r;
    int w = sim_write_count(), fd;

    if (!e)
        return;
    sim_latch_on = true;
    /* "missing" / "empty": a record the host deleted or a torn create */
    remove(LATCH_FILE);
    if (!strcmp(e, "missing"))
        ;
    else if (!strcmp(e, "empty"))
    {
        fd = open(LATCH_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0)
            close(fd);
    }
    else
    {
        latch_create();
        bb_latch_fill(&r, atoi(e) > 0, 0, percent_to_volt_discharge[0]);
        latch_write(&r);
    }
    sim_writes0 += sim_write_count() - w;   /* the seed is not the guard's */
}
#else
static int latch_read(struct bb_latch_rec *r)
{
    return rbfs_record_read(LATCH_FILE, r, sizeof(*r));
}

static int latch_write(const struct bb_latch_rec *r)
{
    return rbfs_record_write(LATCH_FILE, r, sizeof(*r));
}

static int latch_create(void)
{
    return rbfs_record_create(LATCH_FILE);
}
#endif

static int latch_set(bool drained)
{
    struct bb_latch_rec r;
    int rc;

    bb_latch_fill(&r, drained, latch_seq + 1,
                  drained ? percent_to_volt_discharge[0] : 0);
    rc = latch_write(&r);
    if (rc > 0)
    {
        latch_seq++;
        latch_on_disk = drained;
        rec_rc = 0;
    }
    else
        rec_rc = rc < -3 ? -3 : rc;
    return rc;
}

/* a missing, truncated or foreign record made good, holding 'drained';
 * only while writing is allowed and the disk is ours (callers) */
static int latch_repair(bool drained)
{
    int rc;

    if (rec_rc == -2)
        remove(LATCH_FILE);         /* never resized in place: anew */
    rc = (rec_rc == -1 || rec_rc == -2) ? latch_create() : 0;
    if (rc >= 0)
        rc = latch_set(drained);
    if (rc > 0)
        repair_shift = 0;
    else
    {
        repair_next = current_tick + (REPAIR_TICKS << repair_shift);
        if (repair_shift < REPAIR_MAX_SHIFT)
            repair_shift++;
    }
    UPDATE_TRACE("bootbatt latch repair drained=%d rc=%d", (int)drained, rc);
    return rc;
}

static bool may_write(void)
{
    return (battery_level_safe() || power_present()) && !host_owns_disk();
}

void bootbatt_sample(void)
{
    int mv[BB_SAMPLES];
    int i, m;

    for (i = 0; i < BB_SAMPLES; i++)
    {
        if (i)
            sleep(1);               /* one tick apart: ADC noise, not one instant */
        mv[i] = battery_voltage_scan();
    }
    verdict = bb_decide(mv, BB_SAMPLES, power_present(), &m);
    sample_mv = m;
    /* the resting voltage, before any boot load, seeds the power thread's
     * filter (average_init); an invalid reading leaves its own read */
    if (verdict != BB_BOOT_NOREAD)
        powermgmt_seed_voltage(m);
#ifdef SIMULATOR
    sim_writes0 = sim_write_count();
#endif
}

bool bootbatt_boost_allowed(void)
{
    return verdict != BB_REFUSE;
}

/* the screen, the hold and the power-off; returns only when a cable
 * goes in during the hold (verdict then BB_BOOT_POWER) */
static void refuse(void)
{
    long end;

    /* the one call that picks the screen: a new design drops in there */
    ui_sys_dead_battery();
#ifdef SIMULATOR
    {
        /* the capture's own temp file is not the guard's write */
        int w = sim_write_count();
        sim_capture_screen("dead-battery");
        sim_writes0 += sim_write_count() - w;
    }
#endif
    /* hold it, watching the inputs: a cable plugged in now boots (to the
     * charging flow). Powering off instead would leave the unit dark on
     * the cable: the PMU wakes on the charger's edge, already gone. */
    end = current_tick + HOLD_TICKS;
    while (TIME_BEFORE(current_tick, end))
    {
        if (power_present())
        {
            UPDATE_TRACE("bootbatt plugged mv=%d", (int)sample_mv);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
            if (verdict == BB_REFUSE)
                cpu_boost(true);    /* init()'s, skipped at the sample */
#endif
            verdict = BB_BOOT_POWER;
            return;
        }
        sleep(POLL_TICKS);
    }

#ifdef SIMULATOR
    /* the scripts check that the refusal wrote nothing (no settings, no
     * log, no database, no latch): opens for writing, writes, renames,
     * removals */
    UPDATE_TRACE("bootbatt off writes=%d", sim_write_count() - sim_writes0);
#endif
    if (storage_up)
    {
        /* stage 2: the bootloader's spin, the mount's reads; park the
         * disk as shutdown_hw() would (bounded: power_off() follows) */
        long until = current_tick + SPINDOWN_TICKS;
        if (storage_disk_is_active())
            storage_spindown(1);
        while (storage_disk_is_active() && TIME_BEFORE(current_tick, until))
            sleep(HZ / 10);
    }
    /* stage 1: no storage yet, storage_init() never ran */
#ifdef SIMULATOR
    backlight_hw_off();
#else
    _backlight_off_normal();        /* no backlight thread yet */
#endif
#ifdef HAVE_LCD_SHUTDOWN
    lcd_shutdown();
#endif
    sleep(HZ / 4);
#ifdef HAVE_RTC_ALARM
    /* P12 (H6): before rtc_init() has run; a refused boot keeps an armed
     * alarm's wake (disk-free) */
    rtc_keep_alarm_wake();
#endif
    power_off();
    while (1)                           /* the PMU takes the rails */
        sleep(HZ);
}

void bootbatt_enforce(void)
{
    UPDATE_TRACE("bootbatt %s mv=%d thr=%d", bb_verdict_name(verdict),
                 (int)sample_mv, BB_FLOOR_MV);
    if (verdict == BB_REFUSE)
        refuse();
}

void bootbatt_latch_check(void)
{
    struct bb_latch_rec r;
    int drained = -1, stored_zero = 0, rc;

    timeout_register(&zero_tmo, zero_tmo_cb, ZERO_CHECK_TICKS, 0);

    storage_up = true;
#ifdef SIMULATOR
    /* from here: a stage-2 refusal writes nothing (the boot logo's
     * capture, a script's "boot_shot", writes before it) */
    sim_writes0 = sim_write_count();
    sim_latch_seed();
#endif
    rc = latch_read(&r);
    if (rc > 0)
    {
        drained = bb_latch_parse(&r);
        rec_rc = drained >= 0 ? 0 : -4;
        if (drained >= 0)
        {
            latch_seq = r.seq;
            stored_zero = (int)r.zero_mv;
        }
    }
    else
        rec_rc = rc < -3 ? -3 : rc;
    latch_on_disk = drained > 0;
    /* the built-in 0 % point: batcal's fit replaces the table only later,
     * at shell start */
    verdict = bb_latch_rest(verdict, drained > 0, sample_mv, stored_zero,
                            percent_to_volt_discharge[0]);
    UPDATE_TRACE("bootbatt latch drained=%d %s mv=%d need=%d", drained,
                 bb_verdict_name(verdict), (int)sample_mv,
                 drained > 0 ? bb_recharge_mv(stored_zero,
                                              percent_to_volt_discharge[0])
                             : 0);
    if (verdict == BB_REFUSE_DRAINED)
        refuse();                   /* back only with a cable in */

    if (drained > 0 && bb_clears(verdict))
    {
        rc = latch_set(false);
        UPDATE_TRACE("bootbatt latch cleared %s rc=%d",
                     bb_verdict_name(verdict), rc);
        ui_log("bootbatt", "drained latch cleared (%s) mv=%d rc=%d",
               bb_verdict_name(verdict), (int)sample_mv, rc);
    }
    else if (rec_rc && (bb_clears(verdict)
                        || (verdict == BB_BOOT
                            && sample_mv > battery_level_disksafe)))
    {
        /* first boot, or a record the host or a torn write spoiled:
         * (re)made clear, while writing is allowed (no USB host yet; the
         * rest over disk-safe: the power thread's filter may not have
         * its first value yet) */
        latch_repair(false);
    }
}

static void latch_drained(const char *why)
{
    int rc = rec_rc ? latch_repair(true) : latch_set(true);

    (void)rc; (void)why;            /* the trace: simulator only */
    UPDATE_TRACE("bootbatt latch set %s rc=%d", why, rc);
    if (rc > 0)
        ui_log("bootbatt", "drained latch set (%s) mv=%d", why,
               battery_voltage());
#ifdef SIMULATOR
    {
        struct bb_latch_rec r;
        UPDATE_TRACE("bootbatt latch readback=%d",
                     latch_read(&r) > 0 ? bb_latch_parse(&r) : -1);
    }
#endif
}

static void latch_clear(const char *why)
{
    int rc = rec_rc ? latch_repair(false) : latch_set(false);

    (void)rc; (void)why;
    UPDATE_TRACE("bootbatt latch cleared %s rc=%d", why, rc);
    if (rc > 0)
        ui_log("bootbatt", "drained latch cleared (%s)", why);
#ifdef SIMULATOR
    {
        struct bb_latch_rec r;
        UPDATE_TRACE("bootbatt latch readback=%d",
                     latch_read(&r) > 0 ? bb_latch_parse(&r) : -1);
    }
#endif
}

void bootbatt_service(void)
{
    bool power = power_present();
    int mv = battery_voltage();

    /* power seen (charger, or USB once the host lets go): clear */
    if (power && latch_on_disk && !host_owns_disk())
    {
        if (TIME_AFTER(current_tick, repair_next))
            latch_clear("power");
        return;
    }
    /* a missing or broken record, made good while the disk is awake
     * anyway (never a wake of its own), with backoff */
    if (rec_rc && storage_disk_is_active() && may_write()
        && TIME_AFTER(current_tick, repair_next))
        latch_repair(latch_on_disk);

    if (!bb_zero_due(mv, percent_to_volt_discharge[0], power,
                     battery_level_safe(), latch_on_disk))
    {
        if (power || latch_on_disk || mv > percent_to_volt_discharge[0])
            zero_tick = 0;
        return;
    }
    if (!zero_tick)
        zero_tick = current_tick;
    /* prefer the disk awake anyway; past the wait, one wake */
    if (!storage_disk_is_active()
        && TIME_BEFORE(current_tick, zero_tick + ZERO_WAIT_TICKS))
        return;
    latch_drained("zero");
    if (!latch_on_disk)
        zero_tick = current_tick;   /* failed: retry after the wait */
}

void bootbatt_latch_drained(void)
{
    /* the backup: normally the 0 % write already ran, and at the shutoff
     * writes are not allowed (under disk-safe on both variants) */
    if (!bb_shutdown_due(battery_level_safe(), latch_on_disk))
    {
        UPDATE_TRACE("bootbatt latch shutdown skipped latched=%d",
                     (int)latch_on_disk);
        return;
    }
    latch_drained("shutdown");
}

void bootbatt_power_seen(void)
{
    if (latch_on_disk && !host_owns_disk())
        latch_clear("charger");
}

void bootbatt_usb_exit(void)
{
    /* the disk is ours again and USB was power: clear a drained record,
     * and make good one the host deleted or truncated */
    repair_next = 0;
    repair_shift = 0;
    if (latch_on_disk)
        latch_clear("usb");
    else if (rec_rc)
        latch_repair(false);
}
