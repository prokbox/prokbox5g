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
#define RAW_RD(off)     (*(volatile unsigned char *)(I2C_BASE + (off)))
#define RAW_WR(off, v)  (*(volatile unsigned char *)(I2C_BASE + (off)) = (v))
#define RAW_USEC()      ((uint32_t)USEC_TIMER)
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
#define PMU_RAW_WAIT_US 2000    /* one transaction of <= 4 bytes at the
                                 * bus's ~100 kHz is ~0.5 ms [estimate]; a
                                 * line held low ends here, never hangs */

static bool raw_idle(void)
{
    uint32_t t0 = RAW_USEC();

    while (RAW_RD(R_STATUS) & STATUS_BUSY)
        if (RAW_USEC() - t0 > PMU_RAW_WAIT_US)
            return false;
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

bool pmu_raw_rtc_seconds(uint32_t *s)
{
    static const uint8_t reg = 0x0A;
    uint8_t r[7];

    if (!raw_send(&reg, 1) || !raw_read(r, 4) || !raw_read(r + 4, 3))
        return false;
    return sleep_rtc_from_bcd(r, s);
}
