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
 * Portions derived from Rockbox (rockbox.org).
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

/* Phase 18-1 (docs/phase18-brief.md §3.1, Revision 1 H1): kernel-free
 * PMU reads for power_sleep(). The normal path cannot run inside a soft
 * sleep: i2c-pp.c times its waits by current_tick and yields, and the
 * ADC driver caches by current_tick; with the tick stopped one wait
 * would spin for ever. Here every wait is bounded by USEC_TIMER (the
 * free-running 1 MHz counter, independent of the core clock [rockbox-
 * derived]; whether it keeps counting while the core sleeps is D2's
 * question, and either way a wait runs with the core awake).
 *
 * The register sequence is pp_i2c_read_bytes()/pp_i2c_send_bytes()'s
 * [rockbox-derived]: the block moves at most 4 bytes per transaction
 * (quirk 22), so the 7-byte RTC read is one register-address write and
 * then a 4-byte and a 3-byte read, the split i2c_readbytes() makes.
 *
 * Built as the target file, or with PMU_RAW_HOST_TEST by tests/sleep
 * against a scripted bus (pmu_raw_host.h supplies the accessors). */

#ifdef PMU_RAW_HOST_TEST
#include "pmu_raw_host.h"
#else
#include "config.h"
#include "system.h"
#include "power_sleep.h"
#ifdef HAVE_BCMFW_DEVTEST
#define PMU_RAW_DIAG        /* b12: the RTC read's steps, recorded */
#endif
#define RAW_RD(off)     (*(volatile unsigned char *)(I2C_BASE + (off)))
#define RAW_WR(off, v)  (*(volatile unsigned char *)(I2C_BASE + (off)) = (v))
#define RAW_USEC()      ((uint32_t)USEC_TIMER)
#endif
#ifdef PMU_RAW_DIAG
#include <string.h>
#endif

/* i2c-pp.c's register map [rockbox-derived] */
#define R_CTRL      0x00
#define R_ADDR      0x04
#define R_DATA(i)   (0x0C + 4 * (i))
#define R_STATUS    0x1C
#define CTRL_SEND   0x80
#define CTRL_READ   0x20
#define STATUS_BUSY 0x40

#define PMU_ADDR        0x08    /* the PCF50605 */
/* b12 round 2 (#255): the busy wait's bound is Apple's, 100 ms (fw 1.3's
 * I2C wait at 0x100F913C polls STATUS bit 6 against USEC_TIMER with a
 * 100,000 us limit, called right after SEND as here) [apple-RE].  It was
 * 2 ms, from a ~100 kHz estimate the silicon never confirmed (the bus
 * speed is unknown, gpio-i2c.md section 6), and the first raw RTC read at
 * a sleep entry failed on silicon both times (b11).  A line held low
 * still ends here, never hangs; only a dead bus waits this long */
#define PMU_RAW_WAIT_US 100000
#define PMU_RAW_RTC_TRIES 2     /* b12: the RTC read, and one retry */
#define PMU_RAW_RETRY_US  1000  /* before the retry */

#ifdef PMU_RAW_DIAG
/* b12 (#255): the longest busy wait of the RTC read in progress, us */
static uint32_t diag_wait_us;
#endif

static bool raw_idle(void)
{
    uint32_t t0 = RAW_USEC();

    while (RAW_RD(R_STATUS) & STATUS_BUSY)
        if (RAW_USEC() - t0 > PMU_RAW_WAIT_US)
        {
#ifdef PMU_RAW_DIAG
            diag_wait_us = PMU_RAW_WAIT_US + 1;
#endif
            return false;
        }
#ifdef PMU_RAW_DIAG
    if (RAW_USEC() - t0 > diag_wait_us)
        diag_wait_us = RAW_USEC() - t0;
#endif
    return true;
}

static bool raw_send(const uint8_t *d, int len)
{
    int i;

    if (!raw_idle())
        return false;
    RAW_WR(R_ADDR, PMU_ADDR << 1);
    RAW_WR(R_CTRL, RAW_RD(R_CTRL) & ~CTRL_READ);
    for (i = 0; i < len; i++)
        RAW_WR(R_DATA(i), d[i]);
    RAW_WR(R_CTRL, (RAW_RD(R_CTRL) & ~0x6) | ((len - 1) << 1));
    RAW_WR(R_CTRL, RAW_RD(R_CTRL) | CTRL_SEND);
    return raw_idle();
}

static bool raw_read(uint8_t *d, int len)
{
    int i;

    if (!raw_idle())
        return false;
    RAW_WR(R_ADDR, (PMU_ADDR << 1) | 1);
    RAW_WR(R_CTRL, RAW_RD(R_CTRL) | CTRL_READ);
    RAW_WR(R_CTRL, (RAW_RD(R_CTRL) & ~0x6) | ((len - 1) << 1));
    RAW_WR(R_CTRL, RAW_RD(R_CTRL) | CTRL_SEND);
    if (!raw_idle())
        return false;
    for (i = 0; i < len; i++)
        d[i] = RAW_RD(R_DATA(i));
    return true;
}

bool pmu_raw_battery_mv(uint16_t *mv)
{
    /* ADCC2 = channel 2 << 1 | start, then ADCS1/ADCS2 at 0x30
     * (adc-ipod-pcf.c's sequence) [rockbox-derived] */
    static const uint8_t start[2] = { 0x2F, (2 << 1) | 1 };
    static const uint8_t res = 0x30;
    uint8_t d[2];

    if (!raw_send(start, 2) || !raw_send(&res, 1) || !raw_read(d, 2))
        return false;
    *mv = (uint16_t)(((((unsigned)d[0] << 2) | (d[1] & 3)) * 6000u) >> 10);
    return true;
}

#ifndef PMU_RAW_HOST_TEST
#include "pcf5060x.h"
#include "pcf50605.h"
/* pcf50605_standby_mode()'s write without the I2C driver: the panic exit
 * runs in an exception mode with interrupts off, where the driver's mutex
 * (and its yield) is a second panic (P12 device bench, 2026-10-24) */
bool pmu_raw_standby(void)
{
    const uint8_t d[2] = { PCF5060X_OOCC1,
                           GOSTDBY | CHGWAK | EXTONWAK | pcf50605_wakeup_flags };

    return raw_send(d, 2);
}

/* ProkBox band governor (docs/design/pp-governor.md section 16.5): the
 * raise of the core rail where the driver cannot be used (a panic or
 * exception reboot, nothing else running) or did not answer (under the
 * driver's lock); DCDC1 written and read back.  Only the two codes the
 * governor uses, mode bits as Rockbox's pcf50605_init() writes them */
#ifdef HAVE_PP_GOV_DVS
bool pmu_raw_core_voltage(uint8_t code)
{
    const uint8_t d[2] = { PCF5060X_DCDC1, code };
    const uint8_t reg = PCF5060X_DCDC1;
    uint8_t got;

    if (code != 0xEC && code != 0xEB)
        return false;
    if (!raw_send(d, 2) || !raw_send(&reg, 1) || !raw_read(&got, 1))
        return false;
    return got == code;
}
#endif /* HAVE_PP_GOV_DVS */
#endif

/* One RTC read: PMU_RAW_STEP_OK, or the step that failed (r holds what
 * was read up to it) */
static int rtc_once(uint8_t r[7], uint32_t *s)
{
    static const uint8_t reg = 0x0A;

    if (!raw_send(&reg, 1))
        return PMU_RAW_STEP_SEND;
    if (!raw_read(r, 4))
        return PMU_RAW_STEP_READ1;
    if (!raw_read(r + 4, 3))
        return PMU_RAW_STEP_READ2;
    return sleep_rtc_from_bcd(r, s) ? PMU_RAW_STEP_OK : PMU_RAW_STEP_BCD;
}

/* b12 (#255): a failed read is tried once more after about 1 ms (a
 * transaction caught at its end, a seconds roll-over between the two
 * block reads); the entry's refusal (SLEEP_REFUSE_RTC) stays for a bus
 * that still does not answer.  Bounded either way: at most two reads */
#ifdef PMU_RAW_DIAG
bool pmu_raw_rtc_read(uint32_t *s, struct pmu_raw_rtc_diag *d)
#else
bool pmu_raw_rtc_seconds(uint32_t *s)
#endif
{
    uint8_t r[7];
    int k, step = PMU_RAW_STEP_OK;

    for (k = 0; k < PMU_RAW_RTC_TRIES; k++)
    {
        if (k)
        {
            uint32_t t0 = RAW_USEC();

            while (RAW_USEC() - t0 < PMU_RAW_RETRY_US)
                ;
        }
#ifdef PMU_RAW_DIAG
        diag_wait_us = 0;
        memset(r, 0, sizeof(r));
#endif
        step = rtc_once(r, s);
#ifdef PMU_RAW_DIAG
        if (d)
        {
            int i;

            d->tries = k + 1;
            d->step[k] = step;
            d->status[k] = RAW_RD(R_STATUS);
            d->wait_us[k] = diag_wait_us;
            for (i = 0; i < 7; i++)
                d->r[k][i] = r[i];
        }
#endif
        if (step == PMU_RAW_STEP_OK)
            return true;
    }
    return false;
}

#ifdef PMU_RAW_DIAG
/* the last read's record, for the shell's refusal line */
struct pmu_raw_rtc_diag pmu_raw_rtc_last;

bool pmu_raw_rtc_seconds(uint32_t *s)
{
    return pmu_raw_rtc_read(s, &pmu_raw_rtc_last);
}
#endif
