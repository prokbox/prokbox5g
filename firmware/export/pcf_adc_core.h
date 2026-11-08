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

/* AW-9 (docs/pp5022-reference/apple-power-catalog.md section 2.8): the
 * PCF5060x ADC's done bit. Apple 1.3 writes ADCC1, waits 20-35 us and
 * polls ADCS2 bit 7 before it takes the result (osos 0x1028_341C
 * [apple-RE]); Rockbox read ADCS1/ADCS2 straight after the start. Each
 * poll here is one two-byte I2C read of ADCS1/ADCS2 (itself well over
 * 35 us on the PP's I2C), so the result is in hand as soon as the bit is.
 * Bounded: after PCF_ADC_POLLS reads the last one is used, as before, and
 * the miss is counted. Pure, no Rockbox calls (tests/pwrawake/). */
#ifndef PCF_ADC_CORE_H
#define PCF_ADC_CORE_H

#define PCF_ADCS2_RDY   0x80    /* ADCS2: conversion done [apple-RE; the
                                 * PCF50606 datasheet's ADCRDY hypothesis] */
#define PCF_ADC_POLLS   4

/* read2(ctx, d): reads ADCS1 and ADCS2 into d[0], d[1]; < 0 on an I2C
 * error. Returns the reads made when the bit was seen (1..max), or -1
 * when it never was (d holds the last read, the old behaviour) */
static inline int pcf_adc_wait(int (*read2)(void *ctx, unsigned char *d),
                               void *ctx, unsigned char *d, int max)
{
    int k;

    for (k = 1; k <= max; k++)
    {
        if (read2(ctx, d) >= 0 && (d[1] & PCF_ADCS2_RDY))
            return k;
    }
    return -1;
}

/* the 10-bit result from ADCS1 (high 8) and ADCS2 (low 2) */
static inline unsigned short pcf_adc_value(const unsigned char *d)
{
    return (unsigned short)((d[0] << 2) | (d[1] & 0x3));
}

#endif /* PCF_ADC_CORE_H */
