/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Heikki Hannikainen, Uwe Freese
 * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
 * Revisions copyright (C) 2005 by Gerald Van Baren
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
#include "adc.h"
#include "powermgmt.h"
#include "pcf5060x.h"
#include "pcf50605.h"
#ifdef HAVE_PWR_AWAKE
#include "kernel.h"
#include "pwr_awake.h"
#endif
#include "audiohw.h"

unsigned short battery_level_disksafe =
#if   defined(IPOD_NANO)
    3330;
#elif defined(IPOD_VIDEO) && !defined(IPOD_STORAGE_HDD)
    /* Phase 9 flash variant: the owner's 2026-10-22 rundown played and
     * logged normally down to 3,362 mV (reads to 3,368, the log flushed
     * at the shutdown), and flash has no spin-up surge: 100 mV lower
     * than the disk's floor (P10 H3, V6; docs/storage-choices.md) */
    3400;
#elif defined(IPOD_VIDEO)
    3500;
#elif defined(IPOD_COLOR)
    3300;
#elif defined(IPOD_3G)
    3700;
#else
    /* FIXME: calibrate value for other iPods */
    3300;
#endif

unsigned short battery_level_shutoff =
#if   defined(IPOD_NANO)
    3230;
#elif defined(IPOD_VIDEO)
    3300;
#elif defined(IPOD_COLOR)
    3300;
#elif defined(IPOD_3G)
    3500;
#else
    /* FIXME: calibrate value for other iPods */
    3000;
#endif

/* voltages (millivolt) of 0%, 10%, ... 100% when charging disabled */
unsigned short percent_to_volt_discharge[11] =
{
#if   defined(IPOD_NANO)
    /* measured values */
    3230, 3620, 3700, 3730, 3750, 3780, 3830, 3890, 3950, 4030, 4160
#elif defined(IPOD_VIDEO) && IPOD_BATTERY_MAH > 600
    /* Phase 9 large cell (e.g. 3000 mAh): fitted to the owner's 2026-10-22
     * rundown (docs/hardware.md "Measured discharge curve", tools/batcurve.py).
     * 0 % = flash disk-safe + 20 mV ("about to switch off"), 100 % = the
     * plateau after the surface charge. batcal still replaces it with
     * the unit's own fit. */
#ifdef IPOD_STORAGE_HDD
    /* 0 % = the disk's floor (3,500) + 20: the same rundown fitted with
     * 0 % there (11de3b6fe1), so the steps stay equal in charge and the
     * table rises (the flash row's 10 % is 3,500, under this 0 %) */
    3520, 3570, 3605, 3635, 3665, 3725, 3810, 3900, 3975, 4060, 4170
#else
    3420, 3500, 3565, 3605, 3635, 3675, 3750, 3865, 3955, 4045, 4170
#endif
#elif defined(IPOD_VIDEO)
    /* iPod Video 30GB Li-Ion 400mAh */
    3600, 3720, 3750, 3780, 3810, 3840, 3880, 3950, 4020, 4100, 4180
#elif defined(IPOD_COLOR)
    /* iPod Photo 30GB, see FS#9072 */
    3450, 3660, 3700, 3730, 3750, 3770, 3820, 3870, 3920, 4040, 4170
#elif defined(IPOD_3G)
    /* iPod 3G 40GB, first approach based upon measurements */
    3720, 3740, 3760, 3780, 3830, 3870, 3910, 3970, 4020, 4060, 4090
#else
    /* FIXME: calibrate value for other iPods */
    /* Table is "provisional" from IPOD_COLOR */
    3450, 3660, 3700, 3730, 3750, 3770, 3820, 3870, 3920, 4040, 4170
#endif
};

#if CONFIG_CHARGING
/* voltages (millivolt) of 0%, 10%, ... 100% when charging enabled */
unsigned short percent_to_volt_charge[11] =
{
#if   defined(IPOD_NANO)
    /* measured values */
    3230, 3620, 3700, 3730, 3750, 3780, 3830, 3890, 3950, 4030, 4160
#elif defined(IPOD_VIDEO) && IPOD_BATTERY_MAH > 600
    /* Phase 9 large cell: uncalibrated, see percent_to_volt_discharge */
    3600, 3720, 3750, 3780, 3810, 3840, 3880, 3950, 4020, 4100, 4180
#elif defined(IPOD_VIDEO)
    /* iPOD Video 30GB Li-Ion 400mAh */
    3600, 3720, 3750, 3780, 3810, 3840, 3880, 3950, 4020, 4100, 4180
#elif defined(IPOD_COLOR)
    /* iPod Photo 30GB, see FS#9072 */
    3450, 3660, 3700, 3730, 3750, 3770, 3820, 3870, 3920, 4040, 4170
#elif defined(IPOD_3G)
    /* iPod 3G 40GB, first approach based upon measurements */
    3720, 3740, 3760, 3780, 3830, 3870, 3910, 3970, 4020, 4060, 4090
#else
    /* FIXME: calibrate value for other iPods */
    /* Table is "provisional" from IPOD_COLOR */
    3450, 3660, 3700, 3730, 3750, 3770, 3820, 3870, 3920, 4040, 4170
#endif
};
#endif /* CONFIG_CHARGING */

#define BATTERY_SCALE_FACTOR 6000
/* full-scale ADC readout (2^10) in millivolt */

/* Returns battery voltage from ADC [millivolts] */
int _battery_voltage(void)
{
    return (adc_read(ADC_UNREG_POWER) * BATTERY_SCALE_FACTOR) >> 10;
}

#ifdef HAVE_IPOD_UI
/* ProkBox boot guard (apps/ipodui/bootbatt.c): a fresh conversion, not
 * adc_read()'s value of up to 400 ms ago. adc_init() first sets the
 * channel; calling it again later is harmless. */
int battery_voltage_scan(void)
{
    static bool adc_ready;

    if (!adc_ready)
    {
        adc_init();
        adc_ready = true;
    }
    return (adc_scan(ADC_UNREG_POWER) * BATTERY_SCALE_FACTOR) >> 10;
}
#endif

#ifdef HAVE_ACCESSORY_SUPPLY
void accessory_supply_set(bool enable)
{
    /* Set accessory power supply to 3.3V, otherwise switch it off. */
    unsigned char value = enable ? 0xf8 : 0x18;
    
    /* Write to register. */
    pcf50605_write(PCF5060X_D2REGC1, value);
}
#endif

#ifdef HAVE_PWR_LCDLDO
/* ProkBox AW-4 (apple-power-catalog.md, T-LCDLDO = S-4): Apple 1.3 puts
 * the LCD LDO in mode 010 (the same voltage code) only in sleep
 * (apple-power.md section 2.2 step 12); ProkBox does it whenever the LCD
 * sleeps, and puts the value it found back 10 ms before the BCM wakes.
 * Applied only when the register is in mode 111 (the start-up form); a
 * read-back that doesn't match, either way, is the failure signal (the
 * found value written back, the row off). UI or backlight thread. */
static int ldo_found = -1;          /* the value to put back; -1: none */

void pwraw_lcd_ldo(bool dark)
{
    int v;

    if (dark)
    {
        if (ldo_found >= 0 || !pwraw_active(PWRAW_LCDLDO))
            return;
        v = pcf50605_read(PCF5060X_D3REGC1);
        if (v < 0 || (v >> 5) != 7)
            return;                 /* not mode 111: left alone */
        pcf50605_write(PCF5060X_D3REGC1, (unsigned char)((v & 0x1f) | 0x40));
        ldo_found = v;
        if (pcf50605_read(PCF5060X_D3REGC1) != ((v & 0x1f) | 0x40))
        {
            pcf50605_write(PCF5060X_D3REGC1, (unsigned char)v);
            ldo_found = -1;
            pwraw_fail(PWRAW_LCDLDO, (uint32_t)v, 0);
            return;
        }
        pwraw_note(PWRAW_LCDLDO, true, (uint32_t)v, (v & 0x1f) | 0x40);
        return;
    }
    if (ldo_found < 0)
        return;
    v = ldo_found;
    ldo_found = -1;
    pcf50605_write(PCF5060X_D3REGC1, (unsigned char)v);
    sleep(2);                       /* T-LCDLDO: >= 10 ms before the BCM */
    if (pcf50605_read(PCF5060X_D3REGC1) != v)
    {
        pcf50605_write(PCF5060X_D3REGC1, (unsigned char)v);
        pwraw_fail(PWRAW_LCDLDO, (uint32_t)v, 1);
        return;
    }
    pwraw_note(PWRAW_LCDLDO, false, (uint32_t)v, 0);
}
#endif

#ifdef HAVE_LINEOUT_POWEROFF
void lineout_set(bool enable)
{
    /* Call audio hardware driver implementation */
    audiohw_enable_lineout(enable);
}
#endif
