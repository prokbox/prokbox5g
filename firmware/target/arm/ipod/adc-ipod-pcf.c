/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Linus Nielsen Feltzing
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
#include "config.h"
#include "cpu.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "string.h"
#include "adc.h"
#include "pcf50605.h"
#include "i2c-pp.h"
#ifdef HAVE_POWER_SLEEP
#include "power_sleep.h"
#endif

struct adc_struct {
    long timeout;
    void (*conversion)(unsigned short *data);
    short channelnum;
    unsigned short data;
};

static struct adc_struct adcdata[NUM_ADC_CHANNELS] IDATA_ATTR;

#ifdef HAVE_PWR_AWAKE
#include "pcf_adc_core.h"
#include "pwr_awake.h"
static int adc_read2(void *ctx, unsigned char *d)
{
    (void)ctx;
    return pcf50605_read_multiple(0x30, d, 2);  /* ADCS1, ADCS2 */
}
#endif

static unsigned short _adc_read(struct adc_struct *adc)
{
    if (TIME_AFTER(current_tick, adc->timeout)) {
        unsigned char data[2];
        unsigned short value;

        i2c_lock();

        /* 5x per 2 seconds */
        adc->timeout = current_tick + (HZ * 2 / 5);

        /* ADCC1, 10 bit, start */
        pcf50605_write(0x2f, (adc->channelnum << 1) | 0x1);
#ifdef HAVE_PWR_AWAKE
        /* ProkBox AW-9: the done bit first, as Apple does (bounded) */
        {
            int k = pcf_adc_wait(adc_read2, NULL, data, PCF_ADC_POLLS);
#ifdef HAVE_IPOD_UI_DEVMODE
            /* the developer log's count: acts = the bit never seen,
             * undos = it took more than one read */
            if (k != 1)
                pwraw_note(PWRAW_ADC, k < 0, adc->channelnum,
                           (uint32_t)(k < 0 ? 0 : k));
#else
            (void)k;
#endif
        }
        value = pcf_adc_value(data);
#else
        pcf50605_read_multiple(0x30, data, 2); /* ADCS1, ADCS2 */
        value   = data[0];
        value <<= 2;
        value  |= data[1] & 0x3;
#endif

        if (adc->conversion) {
            adc->conversion(&value);
        }
        adc->data = value;

        i2c_unlock();
        return value;
    } else
    {
        return adc->data;
    }
}

/* Force an ADC scan _now_ */
unsigned short adc_scan(int channel) {
    struct adc_struct *adc = &adcdata[channel];
    adc->timeout = 0;
    return _adc_read(adc);
}

#ifdef HAVE_POWER_SLEEP
/* ProkBox Phase 18-1: after a soft sleep the cache's timeouts are on a
 * tick that stood still for hours; the next read of every channel goes
 * to the PMU */
void adc_invalidate(void)
{
    int i;

    for (i = 0; i < NUM_ADC_CHANNELS; i++)
        adcdata[i].timeout = current_tick - 1;
}
#endif

/* Retrieve the ADC value, only does a scan periodically */
unsigned short adc_read(int channel) {
    return _adc_read(&adcdata[channel]);
}

void adc_init(void)
{
    struct adc_struct *adc_battery = &adcdata[ADC_BATTERY];
    adc_battery->channelnum = 0x2; /* ADCVIN1, resistive divider */
    adc_battery->timeout = 0;
    adcdata[ADC_ACCESSORY].channelnum = 4;
    adcdata[ADC_ACCESSORY].timeout = 0;
#if defined(IPOD_VIDEO) || defined(IPOD_NANO)
    adcdata[ADC_4066_ISTAT].channelnum = 7;
    adcdata[ADC_4066_ISTAT].timeout = 0;
#endif
    _adc_read(adc_battery);
}
