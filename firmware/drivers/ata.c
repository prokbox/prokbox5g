/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Alan Korr
 * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
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

//#define LOGF_ENABLE

#include <stdbool.h>
#include <inttypes.h>
#include "led.h"
#include "cpu.h"
#include "system.h"
#include "debug.h"
#include "panic.h"
#include "power.h"
#include "string.h"
#include "ata-driver.h"
#include "hang_watch.h"           /* ProkBox */
#include "ata-defines.h"
#include "fs_defines.h"
#include "storage.h"
#include "logf.h"
#include "devstats.h"   /* ProkBox Fit-1 M0: empty unless HAVE_DEVSTATS */
#ifdef STORAGE_QUIET_WAIT
#include "quiet_wait_core.h"   /* P19 19-1a */
#include "backlight.h"
#endif

#define SELECT_DEVICE1  0x10
#define SELECT_LBA      0x40

#define CONTROL_nIEN    0x02
#define CONTROL_SRST    0x04

#define CMD_READ_SECTORS           0x20
#define CMD_WRITE_SECTORS          0x30
#define CMD_WRITE_SECTORS_EXT      0x34
#define CMD_READ_MULTIPLE          0xC4
#define CMD_READ_MULTIPLE_EXT      0x29
#define CMD_WRITE_MULTIPLE         0xC5
#define CMD_WRITE_MULTIPLE_EXT     0x39
#define CMD_SET_MULTIPLE_MODE      0xC6
#ifdef HAVE_ATA_SMART
#define CMD_SMART                  0xB0
#endif
#define CMD_STANDBY_IMMEDIATE      0xE0
#define CMD_STANDBY                0xE2
#define CMD_IDENTIFY               0xEC
#define CMD_SLEEP                  0xE6
#define CMD_FLUSH_CACHE            0xE7
#define CMD_FLUSH_CACHE_EXT        0xEA
#define CMD_SET_FEATURES           0xEF
#define CMD_SECURITY_FREEZE_LOCK   0xF5
#ifdef HAVE_ATA_DMA
#define CMD_READ_DMA               0xC8
#define CMD_READ_DMA_EXT           0x25
#define CMD_WRITE_DMA              0xCA
#define CMD_WRITE_DMA_EXT          0x35
#endif

#define READWRITE_TIMEOUT 5*HZ

#ifdef HAVE_ATA_POWER_OFF
#define ATA_POWER_OFF_TIMEOUT 2*HZ
#endif

/* ProkBox P18 (#86, #114): the drive's power is cut when idle and at
 * sleep entry on both storage kinds (firmware/export/stor_pwr.h), so the
 * power-on path is built for flash too */
#if defined(HAVE_ATA_POWER_OFF) || defined(HAVE_STORAGE_POWEROFF)
#define ATA_POWER_CYCLE
#endif
#ifdef HAVE_STORAGE_POWEROFF
#include "stor_pwr.h"
#include "backlight.h"
static struct stor_pwr spw;         /* ~170 B: counters and event ring */
static const struct stor_pwr_tune spw_tune = {
    .hold_dark = STOR_PWR_HOLD_DARK_S * HZ,
    .hold_lit  = STOR_PWR_HOLD_LIT_S * HZ,
    .min_off   = STOR_PWR_MIN_OFF_S * HZ,
    .hold_max  = STOR_PWR_HOLD_MAX_S * HZ,
    .relax     = STOR_PWR_RELAX_S * HZ,
};
static bool spw_allow = true;       /* the developer switch */
static bool spw_solid;              /* set at init from IDENTIFY */
static bool spw_usb;                /* USB owns the drive */
static int spw_reason = STOR_PWR_R_ACCESS;  /* why the next power-on */
static bool spw_clean;              /* in standby with the cache safe */
#endif

#if defined(HAVE_USBSTACK)
#define ATA_ACTIVE_IN_USB 1
#else
#define ATA_ACTIVE_IN_USB 0
#endif

enum {
    ATA_BOOT = -1,
    ATA_OFF,
    ATA_SLEEPING,
    ATA_SPINUP,
    ATA_ON,
};

static int ata_state = ATA_BOOT;

static int ata_device; /* device 0 (master) or 1 (slave) */

#if (CONFIG_LED == LED_REAL)
static bool ata_led_enabled = true;
static bool ata_led_on = false;
#endif

#if defined(HAVE_IPOD_UI) && defined(HAVE_FLASH_STORAGE)
/* ProkBox (Fit-1 C1): flash has no spin-up, so the hard disk's 5 s of
 * hysteresis only keeps the adapter out of standby; 1 s covers a burst of
 * reads. Hard disk units keep 5 s and their spin-down setting. */
#define ATA_SLEEP_TIMEOUT_DEFAULT (1*HZ)
#else
#define ATA_SLEEP_TIMEOUT_DEFAULT (5*HZ)
#endif
static long sleep_timeout = ATA_SLEEP_TIMEOUT_DEFAULT;

static long last_disk_activity = -1;
#ifdef HAVE_ATA_POWER_OFF
static long power_off_tick = 0;
#endif

static uint8_t  multisectors; /* number of supported multisectors */

#ifdef HAVE_ATA_DMA
static int dma_mode = 0;
#endif

#ifdef HAVE_ATA_XFER_STATS
static struct ata_xfer_stats xfer_stats; /* ProkBox (#63) */
#endif

#ifdef HAVE_ATA_UDMA_FORCE
static int udma_force;          /* Fit-1 C7: 0, or the forced UDMA mode */
static bool udma_fell_back;
#endif

#ifdef ATA_POWER_CYCLE
static int ata_power_on(void);
#endif
static int perform_soft_reset(void);
static int set_multiple_mode(int sectors);
static int set_features(void);

#ifdef HAVE_STORAGE_POWEROFF
/* #114: the hardware behind stor_pwr_power_on() (stor_pwr_core.c) */
static int spw_hw_on(void *ctx)
{
    (void)ctx;
    return ata_power_on();
}

static void spw_hw_off(void *ctx)
{
    (void)ctx;
    ide_power_enable(false);
    sleep(HZ/10);           /* let the rail fall before a retry */
}

static long spw_hw_now(void *ctx)
{
    (void)ctx;
    return current_tick;
}

static const struct stor_pwr_ops spw_ops = {
    .power_on = spw_hw_on, .power_off = spw_hw_off, .now = spw_hw_now,
};
#endif

#ifdef HAVE_STORAGE_POWEROFF
static void (*ata_awake_hook)(void);     /* P19 19-1a (review M1) */

void storage_set_awake_hook(void (*fn)(void))
{
    ata_awake_hook = fn;
}

static inline void ata_awake_notify(void)
{
    if (ata_awake_hook)
        ata_awake_hook();
}
#endif

static inline void keep_ata_active(void)
{
    last_disk_activity = current_tick;
#ifdef STORAGE_QUIET_WAIT
    storage_quiet_kick();   /* P19 19-1a: a sleep is owed again */
#endif
}

static inline bool ata_sleep_timed_out(void)
{
    return sleep_timeout &&
           TIME_AFTER(current_tick, last_disk_activity + sleep_timeout);
}

static inline bool ata_power_off_timed_out(void)
{
#ifdef HAVE_ATA_POWER_OFF
    return power_off_tick && TIME_AFTER(current_tick, power_off_tick);
#else
    return false;
#endif
}

#ifdef HAVE_PP_GOV_DVS
/* ProkBox band governor, stage 2 (pp-governor.md 16.5, review S2): every
 * command this driver issues runs under ata_mutex, PIO and DMA, flash and
 * hard drive alike; while it is held the core rail is at its normal
 * voltage and 100 MHz is capped.  The functions are defined before the
 * macros, so they call the kernel's own */
#include "pgov.h"
static inline void ata_gov_lock(struct mutex *m)
{
    mutex_lock(m);
    pgov2_storage(true);
}
static inline void ata_gov_unlock(struct mutex *m)
{
    pgov2_storage(false);
    mutex_unlock(m);
}
#define mutex_lock(m)   ata_gov_lock(m)
#define mutex_unlock(m) ata_gov_unlock(m)
#endif
#include "ata-common.c"

#ifndef ATA_TARGET_POLLING
static ICODE_ATTR int wait_for_bsy(void)
{
    long timeout = current_tick + HZ*30;

    do
    {
        if (!(ATA_IN8(ATA_STATUS) & STATUS_BSY))
            return 1;
        keep_ata_active();
        yield();
    } while (TIME_BEFORE(current_tick, timeout));

    return 0; /* timeout */
}

static ICODE_ATTR int wait_for_rdy(void)
{
    long timeout;

    if (!wait_for_bsy())
        return 0;

    timeout = current_tick + HZ*10;

    do
    {
        if (ATA_IN8(ATA_ALT_STATUS) & STATUS_RDY)
            return 1;
        keep_ata_active();
        yield();
    } while (TIME_BEFORE(current_tick, timeout));

    return 0; /* timeout */
}
#else
#define wait_for_bsy    ata_wait_for_bsy
#define wait_for_rdy    ata_wait_for_rdy
#endif

static int ata_perform_wakeup(int state)
{
    logf("ata WAKE %ld", current_tick);
#ifdef HAVE_STORAGE_POWEROFF
    spw_clean = false;      /* writes may follow: flushed again first */
#endif
    if (state > ATA_OFF) {
        if (perform_soft_reset()) {
            return -1;
        }
    }
#ifdef HAVE_STORAGE_POWEROFF
    else {
        /* P18 (§4.2), #114 H1: one retry from a drive powered off again;
         * a power-up that fails both is cut and the caller leaves the
         * drive ATA_OFF, so the next access starts afresh. After
         * STOR_PWR_FAIL_MAX of them in a row the idle cut is given up
         * for the session (logged by the event ring, nothing shown) */
        bool give_up = false;
        int rc = stor_pwr_power_on(&spw, &spw_tune, &spw_ops, spw_reason,
                                   1000 / HZ, &give_up);
        if (give_up) {
            logf("ata power-up failed %d times: power kept",
                 STOR_PWR_FAIL_MAX);
            spw_allow = false;
        }
        if (rc)
            return -2;
    }
#elif defined(ATA_POWER_CYCLE)
    else {
        if (ata_power_on()) {
            return -2;
        }
    }
#endif

    return 0;
}

static int ata_perform_sleep(void)
{
    /* If device doesn't support PM features, don't try to sleep. */
    if (!ata_disk_can_sleep())
        return 0; // XXX or return a failure?

    logf("ata SLEEP %ld", current_tick);

    ATA_OUT8(ATA_SELECT, ata_device);

    if(!wait_for_rdy()) {
        DEBUGF("ata_perform_sleep() - not RDY\n");
        return -1;
    }

    /* STANDBY IMMEDIATE
        - writes all cached data
        - transitions to PM2:Standby
        - enters Standby_z power condition

      This places the device into a state where power-off is safe.  We
      will cut power at a later time.
    */
    ATA_OUT8(ATA_COMMAND, CMD_STANDBY_IMMEDIATE);

    if (!wait_for_rdy()) {
        DEBUGF("ata_perform_sleep() - CMD failed\n");
        return -2;
    }

    return 0;
}

static int ata_perform_flush_cache(void)
{
    uint8_t cmd;

    if (!canflush) {
        return 0;
#ifdef HAVE_LBA48
    } else if (ata_lba48 && identify_info[83] & (1 << 13)) {
        cmd = CMD_FLUSH_CACHE_EXT;  /* Flag, optional, ATA-6 and up, for use with LBA48 devices */
#endif
    } else if (identify_info[83] & (1 << 12)) {
        cmd = CMD_FLUSH_CACHE; /* Flag, mandatory, ATA-6 and up */
    } else if (identify_info[80] >= (1 << 5)) { /* Use >= instead of '&' because bits lower than the latest standard we support don't have to be set */
        cmd = CMD_FLUSH_CACHE; /* No flag, mandatory, ATA-5  (Optional for ATA-4) */
    } else {
        /* If neither (mandatory!) command is supported
           then don't issue it. */
       canflush = 0;
       return 0;
    }

    logf("ata FLUSH CACHE %ld", current_tick);

    ATA_OUT8(ATA_SELECT, ata_device);

    if(!wait_for_rdy()) {
        DEBUGF("ata_perform_flush_cache() - not RDY\n");
        return -1;
    }

    ATA_OUT8(ATA_COMMAND, cmd);

    if (!wait_for_rdy()) {
        DEBUGF("ata_perform_flush_cache() - CMD failed\n");
        return -2;
    }

    return 0;
}

#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
static struct ata_flush_stats ata_flush_stats;

void ata_flush_stats_get(struct ata_flush_stats *out)
{
    *out = ata_flush_stats;
}
#endif

#ifdef HAVE_STORAGE_POWEROFF
/* #114 M2: after a FLUSH CACHE that returned flush_rc, may the power be
 * cut? Upstream's guard (power management claimed, or FLUSH CACHE
 * offered), then the cache: flushed, or no write cache enabled (IDENTIFY
 * word 85 bit 5). This holds for any adapter, not only ours (#43). A
 * refusal is counted and logged (the event ring), never shown */
static bool spw_cache_ok(int flush_rc)
{
    bool ok = stor_pwr_cut_safe(canflush, flush_rc == 0,
                                identify_info[85] & (1 << 5),
                                ata_disk_can_sleep());
    if (ok)
        stor_pwr_note_clean(&spw);
    else {
        logf("ata cache not safe (flush %d): power kept", flush_rc);
        stor_pwr_note_dirty(&spw, current_tick);
    }
    return ok;
}
#endif

int ata_flush(void)
{
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    int rc = 0;
#endif
    if (ata_state >= ATA_SPINUP) {
        mutex_lock(&ata_mutex);
#ifdef HAVE_STORAGE_POWEROFF
        /* #114 L1: re-checked under the mutex; a power-down that ran in
         * between flushed the drive already */
        if (ata_state < ATA_SPINUP) {
            mutex_unlock(&ata_mutex);
            return 0;
        }
#endif
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
        /* #91 H2: what a write-order barrier costs on this unit's drive;
         * #91 M2: a failed flush, or a drive that offers none (every
         * barrier then does nothing), is counted and returned */
        unsigned long t0 = USEC_TIMER;
        rc = ata_perform_flush_cache();
        unsigned long dt = USEC_TIMER - t0;
        ata_flush_stats.n++;
        ata_flush_stats.total_us += dt;
        if (dt > ata_flush_stats.max_us)
            ata_flush_stats.max_us = dt;
        if (rc < 0)
            ata_flush_stats.fail++;
        else if (!canflush) {
            ata_flush_stats.nosup++;
            rc = 1;
        }
#else
        ata_perform_flush_cache();
#endif
        mutex_unlock(&ata_mutex);
    }
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    return rc;
#else
    return 0;
#endif
}

static ICODE_ATTR int wait_for_start_of_transfer(void)
{
    if (!wait_for_bsy())
        return 0;

    return (ATA_IN8(ATA_ALT_STATUS) & (STATUS_BSY|STATUS_DRQ)) == STATUS_DRQ;
}

static ICODE_ATTR int wait_for_end_of_transfer(void)
{
    if (!wait_for_bsy())
        return 0;
    return (ATA_IN8(ATA_ALT_STATUS) &
            (STATUS_BSY|STATUS_RDY|STATUS_DF|STATUS_DRQ|STATUS_ERR))
           == STATUS_RDY;
}

#if (CONFIG_LED == LED_REAL)
/* Conditionally block LED access for the ATA driver, so the LED can be
 * (mis)used for other purposes */
static void ata_led(bool on)
{
    ata_led_on = on;
    if (ata_led_enabled)
        led(ata_led_on);
}
#else
#define ata_led(on) led(on)
#endif

#ifndef ATA_OPTIMIZED_READING
static ICODE_ATTR void copy_read_sectors(unsigned char* buf, int wordcount)
{
    unsigned short tmp = 0;

    if ( (unsigned long)buf & 1)
    {   /* not 16-bit aligned, copy byte by byte */
        unsigned char* bufend = buf + wordcount*2;
        do
        {
            tmp = ATA_IN16(ATA_DATA);
#if defined(ROCKBOX_LITTLE_ENDIAN)
            *buf++ = tmp & 0xff; /* I assume big endian */
            *buf++ = tmp >> 8;   /*  and don't use the SWAB16 macro */
#else
            *buf++ = tmp >> 8;
            *buf++ = tmp & 0xff;
#endif
        } while (buf < bufend); /* tail loop is faster */
    }
    else
    {   /* 16-bit aligned, can do faster copy */
        unsigned short* wbuf = (unsigned short*)buf;
        unsigned short* wbufend = wbuf + wordcount;
        do
        {
            *wbuf = ATA_IN16(ATA_DATA);
        } while (++wbuf < wbufend); /* tail loop is faster */
    }
}
#endif /* !ATA_OPTIMIZED_READING */

#ifndef ATA_OPTIMIZED_WRITING
static ICODE_ATTR void copy_write_sectors(const unsigned char* buf,
                                              int wordcount)
{
    if ( (unsigned long)buf & 1)
    {   /* not 16-bit aligned, copy byte by byte */
        unsigned short tmp = 0;
        const unsigned char* bufend = buf + wordcount*2;
        do
        {
#if defined(ROCKBOX_LITTLE_ENDIAN)
            tmp = (unsigned short) *buf++;
            tmp |= (unsigned short) *buf++ << 8;
#else
            tmp = (unsigned short) *buf++ << 8;
            tmp |= (unsigned short) *buf++;
#endif
            ATA_OUT16(ATA_DATA, tmp);
        } while (buf < bufend); /* tail loop is faster */
    }
    else
    {   /* 16-bit aligned, can do faster copy */
        unsigned short* wbuf = (unsigned short*)buf;
        unsigned short* wbufend = wbuf + wordcount;
        do
        {
            ATA_OUT16(ATA_DATA, *wbuf);
        } while (++wbuf < wbufend); /* tail loop is faster */
    }
}
#endif /* !ATA_OPTIMIZED_WRITING */

static int ata_transfer_sectors(uint64_t start,
                                int incount,
                                void* inbuf,
                                int write)
{
    int ret = 0;
    long timeout;
    int count;
    void* buf;
    long spinup_start = spinup_start;
#ifdef HAVE_ATA_DMA
    bool usedma = false;
#endif
#ifdef HAVE_DEVSTATS
    bool woke = ata_state < ATA_ON;     /* M0: who wakes the disk */
#endif
#if defined(HAVE_DEVSTATS) || defined(HAVE_ATA_UDMA_FORCE)
    bool retried = false;
#endif

    if (start + incount > total_sectors) {
        ret = -1;
        goto error;
    }

#ifdef HAVE_IPOD_UI
    /* ProkBox (Fit-1 C12a): the FAT layer now asks for up to 256 KB at a
     * time; an LBA28 command moves at most 256 sectors (NSECTOR 0), so a
     * stock-sized hard disk without LBA48 gets it in pieces. */
#ifdef HAVE_LBA48
    if (!ata_lba48 && incount > 256)
#else
    if (incount > 256)
#endif
    {
        while (incount > 0) {
            int n = MIN(incount, 256);
            ret = ata_transfer_sectors(start, n, inbuf, write);
            if (ret)
                return ret;
            start += n;
            inbuf = (char *)inbuf + n * log_sector_size;
            incount -= n;
        }
        return 0;
    }
#endif

    keep_ata_active();

    ata_led(true);

    if (ata_state < ATA_ON) {
        spinup_start = current_tick;
        int state = ata_state;
        ata_state = ATA_SPINUP;
        if (ata_perform_wakeup(state)) {
#ifdef HAVE_STORAGE_POWEROFF
            /* #114 H1: a failed power-up was cut again; left ON, every
             * access would wait out wait_for_rdy() (~40 s) on it */
            if (state == ATA_OFF)
                ata_state = ATA_OFF;
#endif
            ret = -2;
            goto error;
        }
#ifdef HAVE_STORAGE_POWEROFF
        ata_awake_notify();     /* P19 19-1a: up from off or standby */
#endif
    }

    logf("ata XFER (%d) %d @ %llu", write, incount, start);

    timeout = current_tick + READWRITE_TIMEOUT;

    ATA_OUT8(ATA_SELECT, ata_device);
    if (!wait_for_rdy())
    {
        ret = -3;
        goto error;
    }

 retry:
#ifdef HAVE_DEVSTATS
    if (retried)
        DEVSTATS_DISK_RESET();          /* a failed command, soft reset */
#endif
#ifdef HAVE_ATA_UDMA_FORCE
    if (retried && udma_force) {
        /* C7: the first failure at a forced mode ends the experiment; the
         * retry runs at the negotiated mode (set_features() again) */
        udma_force = 0;
        udma_fell_back = true;
        perform_soft_reset();
    }
#endif
#if defined(HAVE_DEVSTATS) || defined(HAVE_ATA_UDMA_FORCE)
    retried = true;
#endif
    buf = inbuf;
    count = incount;
    while (TIME_BEFORE(current_tick, timeout)) {
        ret = 0;
        keep_ata_active();

#ifdef HAVE_ATA_DMA
        /* If DMA is supported and parameters are ok for DMA, use it */
        if (dma_mode && ata_dma_setup(inbuf, incount * log_sector_size, write))
            usedma = true;
#endif

#ifdef HAVE_LBA48
        if (ata_lba48)
        {
            ATA_OUT8(ATA_NSECTOR, count >> 8);
            ATA_OUT8(ATA_NSECTOR, count & 0xff);
            ATA_OUT8(ATA_SECTOR, (start >> 24) & 0xff); /* 31:24 */
            ATA_OUT8(ATA_SECTOR, start & 0xff); /* 7:0 */
            ATA_OUT8(ATA_LCYL, (start >> 32) & 0xff); /* 39:32 */
            ATA_OUT8(ATA_LCYL, (start >> 8) & 0xff); /* 15:8 */
            ATA_OUT8(ATA_HCYL, (start >> 40) & 0xff); /* 47:40 */
            ATA_OUT8(ATA_HCYL, (start >> 16) & 0xff); /* 23:16 */
            ATA_OUT8(ATA_SELECT, SELECT_LBA | ata_device);
#ifdef HAVE_ATA_DMA
            if (write)
                ATA_OUT8(ATA_COMMAND, usedma ? CMD_WRITE_DMA_EXT : CMD_WRITE_MULTIPLE_EXT);
            else
                ATA_OUT8(ATA_COMMAND, usedma ? CMD_READ_DMA_EXT : CMD_READ_MULTIPLE_EXT);
#else
            ATA_OUT8(ATA_COMMAND, write ? CMD_WRITE_MULTIPLE_EXT : CMD_READ_MULTIPLE_EXT);
#endif
        }
        else
#endif
        {
            ATA_OUT8(ATA_NSECTOR, count & 0xff); /* 0 means 256 sectors */
            ATA_OUT8(ATA_SECTOR, start & 0xff);
            ATA_OUT8(ATA_LCYL, (start >> 8) & 0xff);
            ATA_OUT8(ATA_HCYL, (start >> 16) & 0xff);
            ATA_OUT8(ATA_SELECT, ((start >> 24) & 0xf) | SELECT_LBA | ata_device);  /* LBA28, mask off upper 4 bits of 32-bit sector address */
#ifdef HAVE_ATA_DMA
            if (write)
                ATA_OUT8(ATA_COMMAND, usedma ? CMD_WRITE_DMA : CMD_WRITE_MULTIPLE);
            else
                ATA_OUT8(ATA_COMMAND, usedma ? CMD_READ_DMA : CMD_READ_MULTIPLE);
#else
            ATA_OUT8(ATA_COMMAND, write ? CMD_WRITE_MULTIPLE : CMD_READ_MULTIPLE);
#endif
        }

#ifdef HAVE_ATA_XFER_STATS
        {
            unsigned long *n = write ? &xfer_stats.pio_wr : &xfer_stats.pio_rd;
#ifdef HAVE_ATA_DMA
            if (usedma)
                n = write ? &xfer_stats.dma_wr : &xfer_stats.dma_rd;
#endif
            (*n)++;
        }
#endif
#ifdef HAVE_DEVSTATS
#ifdef HAVE_ATA_DMA
        DEVSTATS_IO(write, start, incount, woke, !usedma);
#else
        DEVSTATS_IO(write, start, incount, woke, true);
#endif
        woke = false;
#endif

        /* wait at least 400ns between writing command and reading status */
        __asm__ volatile ("nop");
        __asm__ volatile ("nop");
        __asm__ volatile ("nop");
        __asm__ volatile ("nop");
        __asm__ volatile ("nop");

#ifdef HAVE_ATA_DMA
        if (usedma) {
            if (!ata_dma_finish())
                ret = -7;

            if (ret != 0) {
                perform_soft_reset();
                goto retry;
            }

            if (ata_state == ATA_SPINUP) {
                ata_state = ATA_ON;
                spinup_time = current_tick - spinup_start;
            }
        }
        else
#endif /* HAVE_ATA_DMA */
        {
            while (count) {
                int sectors;
                int wordcount;
                int status;
                int error;

                if (!wait_for_start_of_transfer()) {
                    /* We have timed out waiting for RDY and/or DRQ, possibly
                       because the hard drive is shaking and has problems
                       reading the data. We have two options:
                       1) Wait some more
                       2) Perform a soft reset and try again.

                       We choose alternative 2.
                    */
                    perform_soft_reset();
                    ret = -5;
                    goto retry;
                }

                if (ata_state == ATA_SPINUP) {
                    ata_state = ATA_ON;
                    spinup_time = current_tick - spinup_start;
                }

                /* read the status register exactly once per loop */
                status = ATA_IN8(ATA_STATUS);
                error = ATA_IN8(ATA_ERROR);

                if (count >= multisectors)
                    sectors = multisectors;
                else
                    sectors = count;

                wordcount = sectors * log_sector_size / 2;

                if (write)
                    copy_write_sectors(buf, wordcount);
                else
                    copy_read_sectors(buf, wordcount);

                /*
                  "Device errors encountered during READ MULTIPLE commands
                  are posted at the beginning of the block or partial block
                  transfer, but the DRQ bit is still set to one and the data
                  transfer shall take place, including transfer of corrupted
                  data, if any."
                    -- ATA specification
                */
                if ( status & (STATUS_BSY | STATUS_ERR | STATUS_DF) ) {
                    perform_soft_reset();
                    ret = -6;
                    /* no point retrying IDNF, sector no. was invalid */
                    if (error & ERROR_IDNF)
                        break;
                    goto retry;
                }

                buf += sectors * log_sector_size; /* Advance one chunk of sectors */
                count -= sectors;

                keep_ata_active();
            }
        }

        if(!ret && !wait_for_end_of_transfer()) {
            int error;

            error = ATA_IN8(ATA_ERROR);
            perform_soft_reset();
            ret = -4;
            /* no point retrying IDNF, sector no. was invalid */
            if (error & ERROR_IDNF)
                break;
            goto retry;
        }
        break;
    }

  error:
    ata_led(false);

    if (ret < 0 && ata_state == ATA_SPINUP) {
        /* bailed out before updating */
        ata_state = ATA_ON;
    }

    return ret;
}

#ifndef MAX_PHYS_SECTOR_SIZE
int ata_read_sectors(IF_MD(int drive,)
                     sector_t start,
                     int incount,
                     void* inbuf)
{
#ifdef HAVE_MULTIDRIVE
    (void)drive; /* unused for now */
#endif

    mutex_lock(&ata_mutex);
    int rc = ata_transfer_sectors(start, incount, inbuf, false);
    mutex_unlock(&ata_mutex);
    return rc;
}

int ata_write_sectors(IF_MD(int drive,)
                      sector_t start,
                      int count,
                      const void* buf)
{
#ifdef HAVE_MULTIDRIVE
    (void)drive; /* unused for now */
#endif

    mutex_lock(&ata_mutex);
    int rc = ata_transfer_sectors(start, count, (void*)buf, true);
    mutex_unlock(&ata_mutex);
    return rc;
}
#endif /* ndef MAX_PHYS_SECTOR_SIZE */

static int STORAGE_INIT_ATTR check_registers(void)
{
    int i;
    wait_for_bsy();
    if (ATA_IN8(ATA_STATUS) & STATUS_BSY)
            return -1;

    for (i = 0; i<64; i++) {
        ATA_OUT8(ATA_NSECTOR, TEST_PATTERN1);
        ATA_OUT8(ATA_SECTOR,  TEST_PATTERN2);
        ATA_OUT8(ATA_LCYL,    TEST_PATTERN3);
        ATA_OUT8(ATA_HCYL,    TEST_PATTERN4);

        if (((ATA_IN8(ATA_NSECTOR) & 0xff) == TEST_PATTERN1) &&
            ((ATA_IN8(ATA_SECTOR) & 0xff) == TEST_PATTERN2) &&
            ((ATA_IN8(ATA_LCYL) & 0xff) == TEST_PATTERN3) &&
            ((ATA_IN8(ATA_HCYL) & 0xff) == TEST_PATTERN4))
            return 0;

        sleep(1);
    }
    return -2;
}

static int freeze_lock(void)
{
    /* does the disk support Security Mode feature set? */
    if (identify_info[82] & 2)
    {
        ATA_OUT8(ATA_SELECT, ata_device);

        if (!wait_for_rdy())
            return -1;

        ATA_OUT8(ATA_COMMAND, CMD_SECURITY_FREEZE_LOCK);

        if (!wait_for_rdy())
            return -2;
    }

    return 0;
}

void ata_spindown(int seconds)
{
    sleep_timeout = seconds * HZ;
#ifdef STORAGE_QUIET_WAIT
    storage_quiet_kick();   /* P19 19-1a: a new deadline */
#endif
}

bool ata_disk_is_active(void)
{
    return (ata_state >= ATA_SPINUP);
}

#ifdef HAVE_HANG_RECORD
/* ProkBox: the hang watcher's spin-up grace (hang_watch.c) */
bool ata_spinning_up(void)
{
    return ata_state == ATA_SPINUP;
}
#endif

void ata_sleepnow(void)
{
    if (ata_state >= ATA_SPINUP) {
        logf("ata SLEEPNOW %ld", current_tick);
        mutex_lock(&ata_mutex);
        if (ata_state == ATA_ON) {
#ifdef HAVE_STORAGE_POWEROFF
            /* #114 M2: whether the power may be cut later is decided
             * here, from this flush (spw_cache_ok()) */
            int f = ata_perform_flush_cache();
            if (!f && !ata_perform_sleep()) {
                spw_clean = spw_cache_ok(f);
#else
            if (!ata_perform_flush_cache() && !ata_perform_sleep()) {
#endif
                DEVSTATS_DISK_SLEEP(true);
                ata_state = ATA_SLEEPING;
#ifdef HAVE_ATA_POWER_OFF
                if (ata_disk_can_sleep() || canflush) {
                    power_off_tick = current_tick + ATA_POWER_OFF_TIMEOUT;
                }
#endif
            }
#ifdef HAVE_STORAGE_POWEROFF
            else {
                if (f)
                    spw_cache_ok(f);    /* counted and logged */
                DEVSTATS_DISK_SLEEP(false); /* stays ON: retried per tick */
            }
#elif defined(HAVE_DEVSTATS)
            else
                DEVSTATS_DISK_SLEEP(false); /* stays ON: retried per tick */
#endif
        }
        mutex_unlock(&ata_mutex);
    }
}

#ifdef HAVE_STORAGE_POWEROFF
/* ProkBox #114 / P18 (#86, brief §4.2): the one power-down path
 * (firmware/export/stor_pwr.h). Mutex held. */
static void spw_cut(int reason)
{
    logf("ata OFF %ld", current_tick);
    ide_power_enable(false);
    ata_state = ATA_OFF;
#ifdef HAVE_ATA_POWER_OFF
    power_off_tick = 0;
#endif
    stor_pwr_note_off(&spw, current_tick, reason, 1000 / HZ);
}

/* FLUSH CACHE, STANDBY IMMEDIATE, then the power off. The sleep entry
 * (STOR_PWR_KEEP_LOCK) keeps the mutex to the wake, so nothing starts a
 * transfer in the dark; every other caller drops it, so the next access
 * from any thread powers the drive up again (ata_transfer_sectors()) */
int ata_power_down(int reason, unsigned flags)
{
    int rc = 0;

    mutex_lock(&ata_mutex);
    if (spw_usb) {
        rc = -3;
        goto out;
    }
    if (ata_state == ATA_ON) {
        int f = ata_perform_flush_cache();
        int sl = ata_perform_sleep();

        spw_clean = spw_cache_ok(f);
        if (!spw_clean) {
            /* #114 M2: no cut while the cache may hold data. A failed
             * flush stays ON (the storage tick's sleepnow retries it);
             * a drive with a write cache and no FLUSH CACHE goes to
             * standby but keeps its power */
            if (!f && !sl)
                ata_state = ATA_SLEEPING;
            rc = (f && sl) ? -2 : -1;
            goto out;
        }
        ata_state = ATA_SLEEPING;
    }
    else if (ata_state == ATA_SLEEPING && !spw_clean) {
        rc = -1;                /* in standby, its cache not known safe */
        goto out;
    }
    if (ata_state > ATA_OFF) {
        if (spw_allow)
            spw_cut(reason);
        else if (rc == 0)
            rc = 1;             /* standby only: the developer switch */
    }
out:
    if (!(flags & STOR_PWR_KEEP_LOCK))
        mutex_unlock(&ata_mutex);
    return rc;
}

void ata_power_release(void)
{
    mutex_unlock(&ata_mutex);
}

int ata_power_up(int reason)
{
    int rc = 0;

    mutex_lock(&ata_mutex);
    if (ata_state < ATA_ON) {
        int state = ata_state;
        ata_state = ATA_SPINUP;
        spw_reason = reason;
        rc = ata_perform_wakeup(state);
        spw_reason = STOR_PWR_R_ACCESS;
        /* #114 H1: a failed power-up was cut again and stays OFF (the
         * next access tries afresh); a failed soft reset from standby
         * leaves it ON, as a failed transfer does */
        ata_state = (rc && state == ATA_OFF) ? ATA_OFF : ATA_ON;
        keep_ata_active();
        if (ata_state == ATA_ON)
            ata_awake_notify(); /* P19 19-1a */
    }
    mutex_unlock(&ata_mutex);
    return rc;
}

bool ata_powered(void)
{
    return ata_state != ATA_OFF;
}

bool ata_power_warm(void)
{
    return ata_state >= ATA_SPINUP
        || (spw_solid && ata_state == ATA_SLEEPING);
}

void ata_set_poweroff(bool allow)
{
    spw_allow = allow;
#ifdef STORAGE_QUIET_WAIT
    storage_quiet_kick();   /* P19 19-1a: a cut may be owed now */
#endif
}

bool ata_poweroff_allowed(void)
{
    return spw_allow;
}

const struct stor_pwr *ata_power_state(void)
{
    return &spw;
}

/* the storage thread's tick: time to cut the power of a drive in standby?
 * Flash: the policy's hold (stor_pwr_core.c); a disk: upstream's timer */
static bool spw_off_due(void)
{
    if (!spw_allow || spw_usb || !spw_clean)
        return false;       /* #114 M2: never with the cache unsafe */
    if (!spw_solid)
        return ata_power_off_timed_out();
    return stor_pwr_off_due(&spw, &spw_tune, current_tick,
                            last_disk_activity, is_backlight_on(true));
}
#endif /* HAVE_STORAGE_POWEROFF */

void ata_spin(void)
{
    keep_ata_active();
}

#ifdef STORAGE_QUIET_WAIT
/* P19 19-1a: when ata_event(Q_STORAGE_TICK) next has something to do:
 * standby at last_disk_activity + sleep_timeout while on, the power cut
 * (spw_off_due(), or upstream's power_off_tick) while in standby, and
 * nothing while off. The storage idle notices run at the standby, so
 * they need no deadline of their own. */
long ata_tick_wait(bool ticked)
{
    struct qw_stor_in in = {
        .now = current_tick,
        .last_activity = last_disk_activity,
        .sleep_timeout = sleep_timeout,
        .ticked = ticked,
    };
    long w;

    if (ata_state >= ATA_SPINUP)
        in.state = QW_ST_ON;
    else if (ata_state == ATA_SLEEPING)
        in.state = QW_ST_SLEEPING;
    else
        in.state = QW_ST_OFF;
#ifdef HAVE_STORAGE_POWEROFF
    if (spw_allow && !spw_usb && spw_clean) {
        if (spw_solid) {
            in.lit = is_backlight_on(true);
            in.off_armed = spw.on;
            in.off_deadline = last_disk_activity
                + stor_pwr_hold(&spw, &spw_tune, in.lit);
        }
#ifdef HAVE_ATA_POWER_OFF
        else {
            in.off_armed = power_off_tick != 0;
            in.off_deadline = power_off_tick;
        }
#endif
    }
#elif defined(HAVE_ATA_POWER_OFF)
    in.off_armed = power_off_tick != 0;
    in.off_deadline = power_off_tick;
#endif
    w = qw_stor_wait(&in, HZ);
    return w == QW_BLOCK ? TIMEOUT_BLOCK : w;
}
#endif /* STORAGE_QUIET_WAIT */

/* Hardware reset protocol as specified in chapter 9.1, ATA spec draft v5 */
#ifdef ATA_POWER_CYCLE
static int ata_hard_reset(void)
#else
static int STORAGE_INIT_ATTR ata_hard_reset(void)
#endif
{
    int ret;

    mutex_lock(&ata_mutex);

    ata_reset();

    /* state HRR2 */
    ATA_OUT8(ATA_SELECT, ata_device); /* select the right device */
    ret = wait_for_bsy();

    /* Massage the return code so it is 0 on success and -1 on failure */
    ret = ret?0:-1;

    mutex_unlock(&ata_mutex);

    return ret;
}

#ifdef HAVE_ATA_SMART
static int ata_smart(uint16_t *buf, uint8_t cmd)
{
    int i;

    ATA_OUT8(ATA_SELECT, ata_device);

    if(!wait_for_rdy()) {
        DEBUGF("identify() - not RDY\n");
        return -1;
    }

    ATA_OUT8(ATA_FEATURE, cmd);
    ATA_OUT8(ATA_HCYL, 0xc2);
    ATA_OUT8(ATA_LCYL, 0x4f);
    ATA_OUT8(ATA_SELECT, SELECT_LBA | ata_device);
    ATA_OUT8(ATA_COMMAND, CMD_SMART);

    if (!wait_for_start_of_transfer())
    {
        DEBUGF("identify() - CMD failed\n");
        return -2;
    }

    for (i = 0 ; i < 256 ; i++) {
        /* The SMART words are already swapped, so we need to treat
           this info differently that normal sector data */
        buf[i] = ATA_SWAP_IDENTIFY(ATA_IN16(ATA_DATA));
    }
    return 0;
}
int ata_read_smart(struct ata_smart_values* smart_data, uint8_t cmd)
{
    mutex_lock(&ata_mutex);
    int rc = ata_smart((uint16_t*)smart_data, cmd);
    mutex_unlock(&ata_mutex);
    return rc;
}
#endif /* HAVE_ATA_SMART */

// not putting this into STORAGE_INIT_ATTR, as ATA spec recommends to
// re-read identify_info after soft reset. So we'll do that.
static int identify(void)
{
    int i;

    ATA_OUT8(ATA_SELECT, ata_device);

    if(!wait_for_rdy()) {
        DEBUGF("identify() - not RDY\n");
        return -1;
    }
    ATA_OUT8(ATA_COMMAND, CMD_IDENTIFY);

    if (!wait_for_start_of_transfer())
    {
        DEBUGF("identify() - CMD failed\n");
        return -2;
    }

    for (i=0; i<ATA_IDENTIFY_WORDS; i++) {
        /* the IDENTIFY words are already swapped, so we need to treat
           this info differently that normal sector data */
        identify_info[i] = ATA_SWAP_IDENTIFY(ATA_IN16(ATA_DATA));
    }

    return 0;
}

static int perform_soft_reset(void)
{
/* If this code is allowed to run on a Nano, the next reads from the flash will
 * time out, so we disable it. It shouldn't be necessary anyway, since the
 * ATA -> Flash interface automatically sleeps almost immediately after the
 * last command.
 */
    int ret;
    int retry_count;

    logf("ata SOFT RESET %ld", current_tick);
#ifdef HAVE_ATA_XFER_STATS
    xfer_stats.resets++;
#endif

    ATA_OUT8(ATA_SELECT, SELECT_LBA | ata_device );
    ATA_OUT8(ATA_CONTROL, CONTROL_nIEN|CONTROL_SRST );
    sleep(1); /* >= 5us */

#ifdef HAVE_ATA_DMA
    /* DMA requires INTRQ be enabled */
    ATA_OUT8(ATA_CONTROL, 0);
#else
    ATA_OUT8(ATA_CONTROL, CONTROL_nIEN);
#endif
    sleep(1); /* >2ms */

    /* This little sucker can take up to 30 seconds */
    retry_count = 8;
    do
    {
        ret = wait_for_rdy();
    } while(!ret && retry_count--);

    if (!ret)
        return -1;

    if (identify())
        return -5;

    if ((ret = set_features()))
        return -60 + ret;

    if (set_multiple_mode(multisectors))
        return -3;

    if (identify())
        return -2;

    if (freeze_lock())
        return -4;

    return 0;
}

int ata_soft_reset(void)
{
    int ret = -6;

    mutex_lock(&ata_mutex);

    if (ata_state > ATA_OFF) {
        ret = perform_soft_reset();
    }

    mutex_unlock(&ata_mutex);
    return ret;
}

#ifdef ATA_POWER_CYCLE
static int ata_power_on(void)
{
    int rc;

    logf("ata ON %ld", current_tick);

    ide_power_enable(true);
    sleep(HZ/4); /* allow voltage to build up */

    /* Accessing the PP IDE controller too early after powering up the disk
     * makes the core hang for a short time, causing an audio dropout. This
     * also depends on the disk; iPod Mini G2 needs at least HZ/5 to get rid
     * of the dropout. Since this time isn't additive (the wait_for_bsy() in
     * ata_hard_reset() will shortened by the same amount), it's a good idea
     * to do this on all HDD based targets. */

#ifdef HAVE_STORAGE_POWEROFF
    /* P18: the controller lost DEV_IDE0 with the drive; its DMA and
     * interrupt set-up again before the reset (set_features() below
     * re-writes the PIO/UDMA timings). #114 M1: after the wait above, as
     * both touch the IDE controller (IDE0_CFG, the DMA control, the
     * timings, then the device control register) */
    ata_device_init();
    ATA_OUT8(ATA_CONTROL, 0);       /* INTRQ on: DMA completion needs it */

    /* #114 H1: a solid drive gets STOR_PWR_READY_S to clear BSY, not
     * upstream's 30 s for a disk spinning up, so a dead adapter fails the
     * power-up in seconds; a rotating one keeps the 30 s */
    {
        long timeout = current_tick +
                       (spw_solid ? STOR_PWR_READY_S * HZ : 30 * HZ);
        ata_reset();
        ATA_OUT8(ATA_SELECT, ata_device);
        while (ATA_IN8(ATA_STATUS) & STATUS_BSY) {
            if (!TIME_BEFORE(current_tick, timeout))
                return -1;
            keep_ata_active();
            yield();
        }
    }
#else
    if( ata_hard_reset() )
        return -1;
#endif

    if (identify())
        return -5;

    rc = set_features();
    if (rc)
        return -60 + rc;

    if (set_multiple_mode(multisectors))
        return -3;

    if (identify())
        return -2;

#ifdef HAVE_STORAGE_POWEROFF
    /* #114: a multi-card adapter (the 4-card JBOD) must report its whole
     * size again; one that answers before every card is up fails here and
     * gets the one retry in ata_perform_wakeup() [hypothesis: not seen] */
    {
        uint64_t n = ((uint64_t)identify_info[61] << 16) | identify_info[60];
#ifdef HAVE_LBA48
        if (ata_lba48)
            n = ((uint64_t)identify_info[103] << 48) |
                ((uint64_t)identify_info[102] << 32) |
                ((uint64_t)identify_info[101] << 16) | identify_info[100];
#endif
        if (n != total_sectors)
            return -7;
    }
#endif

    if (freeze_lock())
        return -4;

    return 0;
}
#endif /* ATA_POWER_CYCLE */

static int STORAGE_INIT_ATTR master_slave_detect(void)
{
    /* master? */
    ATA_OUT8(ATA_SELECT, 0);
    if (ATA_IN8(ATA_STATUS) & (STATUS_RDY|STATUS_BSY)) {
        ata_device = 0;
        DEBUGF("Found master harddisk\n");
    }
    else {
        /* slave? */
        ATA_OUT8(ATA_SELECT, SELECT_DEVICE1);
        if (ATA_IN8(ATA_STATUS) & (STATUS_RDY|STATUS_BSY)) {
            ata_device = SELECT_DEVICE1;
            DEBUGF("Found slave harddisk\n");
        }
        else
            return -1;
    }
    return 0;
}

static int set_multiple_mode(int sectors)
{
    ATA_OUT8(ATA_SELECT, ata_device);

    if(!wait_for_rdy()) {
        DEBUGF("set_multiple_mode() - not RDY\n");
        return -1;
    }

    ATA_OUT8(ATA_NSECTOR, sectors);
    ATA_OUT8(ATA_COMMAND, CMD_SET_MULTIPLE_MODE);

    if (!wait_for_rdy())
    {
        DEBUGF("set_multiple_mode() - CMD failed\n");
        return -2;
    }

    return 0;
}

#ifdef HAVE_ATA_DMA
static int ata_get_best_mode(unsigned short identword, int max, int modetype)
{
    unsigned short testbit = BIT_N(max);

    while (1) {
        if (identword & testbit)
            return max | modetype;
        testbit >>= 1;
        if (!testbit)
            return 0;
        max--;
    }
}
#endif

static int set_features(void)
{
    static struct {
        unsigned char id_word;
        unsigned char id_bit;
        unsigned char subcommand;
        unsigned char parameter;
    } features[] = {
        { 83, 14, 0x03, 0 },   /* force PIO mode by default */
#ifdef HAVE_ATA_DMA
        { 0, 0, 0x03, 0 },     /* DMA mode */
#endif
        /* NOTE: Above two MUST come first! */
        { 83, 3, 0x05, 0x80 }, /* adv. power management: lowest w/o standby */
        { 83, 9, 0x42, 0x80 }, /* acoustic management: lowest noise */
        { 82, 5, 0x02, 0 },    /* enable volatile write cache */
        { 82, 6, 0xaa, 0 },    /* enable read look-ahead */
    };
    int i;
    int pio_mode = 2; /* Lowest */

    /* Find out the highest supported PIO mode */
    if (identify_info[53] & (1<<1)) {  /* Is word 64 valid? */
      if (identify_info[64] & 2)
        pio_mode = 4;
      else if(identify_info[64] & 1)
        pio_mode = 3;
    }

    /* Update the table: set highest supported pio mode that we also support */
    features[0].parameter = 8 + pio_mode;

#ifdef HAVE_ATA_DMA
    if (identify_info[53] & (1<<2)) {
        int max_udma = ATA_MAX_UDMA;
#if ATA_MAX_UDMA > 2
        if (!identify_info[76] && !(identify_info[93] & (1<<13))) /* w93b13 is only valid for PATA, w76 is 0 PATA */
            max_udma = 2;
#endif
#ifdef HAVE_ATA_UDMA_FORCE
        /* C7: only a mode the adapter lists as supported */
        if (udma_force > max_udma && udma_force <= ATA_MAX_UDMA
            && (identify_info[88] & BIT_N(udma_force)))
            max_udma = udma_force;
#endif
        /* Ultra DMA mode info present, find a mode */
        dma_mode = ata_get_best_mode(identify_info[88], max_udma, 0x40);
    }

    if (!dma_mode) {
        /* No UDMA mode found, try to find a multi-word DMA mode */
        dma_mode = ata_get_best_mode(identify_info[63], ATA_MAX_MWDMA, 0x20);
        features[1].id_word = 63;
    } else {
        features[1].id_word = 88;
    }

    features[1].id_bit = dma_mode & 7;
    features[1].parameter = dma_mode;
#endif /* HAVE_ATA_DMA */

    ATA_OUT8(ATA_SELECT, ata_device);

    if (!wait_for_rdy()) {
        DEBUGF("set_features() - not RDY\n");
        return -1;
    }

    for (i=0; i < (int)(sizeof(features)/sizeof(features[0])); i++) {
        if (identify_info[features[i].id_word] & BIT_N(features[i].id_bit)) {
            ATA_OUT8(ATA_FEATURE, features[i].subcommand);
            ATA_OUT8(ATA_NSECTOR, features[i].parameter);
            ATA_OUT8(ATA_COMMAND, CMD_SET_FEATURES);

            if (!wait_for_rdy()) {
                DEBUGF("set_features() - CMD failed\n");
                return -10 - i;
            }

            if((ATA_IN8(ATA_ALT_STATUS) & STATUS_ERR) && (features[i].subcommand != 0x05)) {
                /* some CF cards don't like advanced powermanagement
                   even if they mark it as supported - go figure... */
                if(ATA_IN8(ATA_ERROR) & ERROR_ABRT) {
                    return -20 - i;
                }
            }
        }
    }

#ifdef ATA_SET_PIO_TIMING
    ata_set_pio_timings(pio_mode);
#endif

#ifdef HAVE_ATA_DMA
    ata_dma_set_mode(dma_mode);
#endif

    return 0;
}

unsigned short* ata_get_identify(void)
{
    return identify_info;
}

static int STORAGE_INIT_ATTR init_and_check(bool hard_reset)
{
    int rc;

    if (hard_reset)
    {
        /* This should reset both master and slave, we don't yet know what's in */
        ata_device = 0;
        if (ata_hard_reset())
            return -1;
    }

    rc = master_slave_detect();
    if (rc)
        return -10 + rc;

    /* symptom fix: else check_registers() below may fail */
    if (hard_reset && !wait_for_bsy())
        return -20;

    rc = check_registers();
    if (rc)
        return -30 + rc;

    return 0;
}

int STORAGE_INIT_ATTR ata_init(void)
{
    int rc = 0;
    bool coldstart;

    if (ata_state == ATA_BOOT) {
        mutex_init(&ata_mutex);
        hang_watch_mutex(HANG_MX_ATA, &ata_mutex);  /* ProkBox */
    }

    mutex_lock(&ata_mutex);

    /* must be called before ata_device_init() */
    coldstart = ata_is_coldstart();
    ata_led(false);
    ata_device_init();
    ata_enable(true);

    if (ata_state == ATA_BOOT) {
        ata_state = ATA_OFF;

        if (!ide_powered()) /* somebody has switched it off */
        {
            ide_power_enable(true);
            sleep(HZ/4); /* allow voltage to build up */
        }

#ifdef HAVE_ATA_DMA
        /* DMA requires INTRQ be enabled */
        ATA_OUT8(ATA_CONTROL, 0);
#endif

        /* first try, hard reset at cold start only */
        rc = init_and_check(coldstart);

        if (rc)
        {   /* failed? -> second try, always with hard reset */
            DEBUGF("ata: init failed, retrying...\n");
            rc  = init_and_check(true);
            if (rc) {
                goto error;
            }
        }

        rc = identify();
        if (rc) {
            rc = -40 + rc;
            goto error;
        }

        multisectors = identify_info[47] & 0xff;
        if (!multisectors && (identify_info[59] & 0x100) == 0x100)
            multisectors = identify_info[59] & 0xff;
        if (!multisectors)
            multisectors = 1; /* One transfer per REQ */

        DEBUGF("ata: max %d sectors per DRQ\n", multisectors);

        total_sectors = (identify_info[61] << 16) | identify_info[60];

#ifdef HAVE_LBA48
        if (identify_info[83] & 0x0400 && total_sectors == 0x0FFFFFFF) {
            total_sectors = ((uint64_t)identify_info[103] << 48) |
                    ((uint64_t)identify_info[102] << 32) |
                    ((uint64_t)identify_info[101] << 16) |
                    identify_info[100];
            ata_lba48 = true; /* use BigLBA */
        }
#endif /* HAVE_LBA48 */

        /* Logical sector size > 512B ? */
        if ((identify_info[106] & 0xd000) == 0x5000) /* B14, B12 */
            log_sector_size = (identify_info[117] | (identify_info[118] << 16)) * 2;
        else
            log_sector_size = 512;

        rc = freeze_lock();
        if (rc) {
            rc = -50 + rc;
            goto error;
        }

        rc = set_features(); // error codes are between -1 and -49
        if (rc) {
            rc = -60 + rc;
            goto error;
        }

#ifdef MAX_PHYS_SECTOR_SIZE
        rc = ata_get_phys_sector_mult();
        if (rc) {
            rc = -70 + rc;
            goto error;
        }
#endif
        ata_state = ATA_ON;
        keep_ata_active();
#ifdef HAVE_STORAGE_POWEROFF
        stor_pwr_init(&spw, current_tick, true);
        spw_solid = ata_disk_isssd();
#endif
    }

    rc = set_multiple_mode(multisectors);
    if (rc)
        rc = -100 + rc;

    rc = identify();
    if (rc) {
        rc = -40 + rc;
        goto error;
    }

error:
    mutex_unlock(&ata_mutex);
    return rc;
}

#if (CONFIG_LED == LED_REAL)
void ata_set_led_enabled(bool enabled)
{
    ata_led_enabled = enabled;
    if (ata_led_enabled)
        led(ata_led_on);
    else
        led(false);
}
#endif

long ata_last_disk_activity(void)
{
    return last_disk_activity;
}

#ifdef HAVE_ATA_XFER_STATS
void ata_get_xfer_stats(struct ata_xfer_stats *out)
{
    *out = xfer_stats;
}
#endif

#ifdef HAVE_ATA_DMA
/* Returns last DMA mode as set by set_features() */
int ata_get_dma_mode(void)
{
    return dma_mode;
}

/* Needed to allow updating while waiting for DMA to complete */
void ata_keep_active(void)
    __attribute__((alias("ata_spin")));
#endif

#ifdef HAVE_ATA_UDMA_FORCE
void ata_set_udma_force(int mode)
{
    udma_force = mode;
    if (ata_state >= ATA_SPINUP)
        ata_soft_reset();           /* set_features() with the new cap */
}

int ata_udma_force(void)
{
    return udma_force;
}

bool ata_udma_forced_now(void)
{
    return udma_force && (dma_mode & 7) > 2 && (dma_mode & 0xf8) == 0x40;
}

bool ata_udma_fell_back(void)
{
    return udma_fell_back;
}

void ata_udma_clear_fell_back(void)
{
    udma_fell_back = false;
}
#endif

#ifdef CONFIG_STORAGE_MULTI
int ata_num_drives(int first_drive)
{
    /* We don't care which logical drive number(s) we have been assigned */
    (void)first_drive;

    return 1;
}
#endif

int ata_event(long id, intptr_t data)
{
    int rc = 0;

    /* GCC does a lousy job culling unreachable cases in the default handler
       if statements are in a switch statement, so we'll do it this way. Only
       the first case is frequently hit anyway. */
    if (LIKELY(id == Q_STORAGE_TICK)) {
        /* won't see ATA_BOOT in here */
        if (ata_state != ATA_ON || !ata_sleep_timed_out()) {
#ifdef HAVE_STORAGE_POWEROFF
            /* #114: re-checked under the mutex, so a transfer that woke
             * the drive meanwhile is never followed by a cut */
            if (ata_state == ATA_SLEEPING && spw_off_due()) {
                mutex_lock(&ata_mutex);
                if (ata_state == ATA_SLEEPING && spw_off_due())
                    spw_cut(STOR_PWR_R_IDLE);
                mutex_unlock(&ata_mutex);
            }
#elif defined(HAVE_ATA_POWER_OFF)
            if (ata_state == ATA_SLEEPING && ata_power_off_timed_out()) {
                power_off_tick = 0;
                mutex_lock(&ata_mutex);
                logf("ata OFF %ld", current_tick);
                ide_power_enable(false);
                ata_state = ATA_OFF;
                mutex_unlock(&ata_mutex);
            }
#endif
            STG_EVENT_ASSERT_ACTIVE(STORAGE_ATA);
        }
    }
    else if (id == Q_STORAGE_SLEEPNOW) {
        ata_sleepnow();
    }
    else if (id == Q_STORAGE_SLEEP) {
        last_disk_activity = current_tick - sleep_timeout + HZ / 5;
    }
#ifndef USB_NONE
    else if (id == SYS_USB_CONNECTED) {
        logf("deq USB %ld", current_tick);
#ifdef HAVE_STORAGE_POWEROFF
        /* #114: powered and re-initialised before the acknowledgement
         * lets the host in; no idle power-off until the disconnect */
        mutex_lock(&ata_mutex);
        spw_usb = true;
        mutex_unlock(&ata_mutex);
        if (ata_state == ATA_OFF)
            ata_power_up(STOR_PWR_R_USB);
#endif
        if (ATA_ACTIVE_IN_USB) {
            /* There is no need to force ATA power on */
            STG_EVENT_ASSERT_ACTIVE(STORAGE_ATA);
        }
        else {
            mutex_lock(&ata_mutex);
            if (ata_state < ATA_ON) {
                ata_led(true);
                if (!(rc = ata_perform_wakeup(ata_state))) {
                    ata_state = ATA_ON;
                }
                ata_led(false);
            }
            mutex_unlock(&ata_mutex);
        }
    }
#endif /* ndef USB_NONE */
#if defined(HAVE_STORAGE_POWEROFF) && !defined(USB_NONE)
    else if (id == SYS_USB_DISCONNECTED) {
        spw_usb = false;
        rc = storage_event_default_handler(id, data, last_disk_activity,
                                           STORAGE_ATA);
    }
#endif
    else {
        rc = storage_event_default_handler(id, data, last_disk_activity,
                                           STORAGE_ATA);
    }

    return rc;
}
