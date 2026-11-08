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

/* The awake rows' gate and records (firmware/export/pwr_awake.h) */
#include "config.h"
#include "system.h"
#include "pwr_awake.h"
#include "pwr_awake_tokens.h"

/* 130 B; shared (uncached on the PP): the audio thread and the codec
 * standby may run on the COP, the drain on the CPU */
static uint32_t dev_mask SHAREDBSS_ATTR;
static uint32_t failed SHAREDBSS_ATTR;
static uint8_t force_on SHAREDBSS_ATTR, force_off SHAREDBSS_ATTR;
static struct pwraw_rec rec[PWRAW_N] SHAREDBSS_ATTR;

static const char *const names[PWRAW_N] =
{
    "linger", "usboff", "lcdldo", "i2sgate", "xmb", "adc"
};

enum pwraw_why pwraw_why_now(int row)
{
    return pwraw_gate(row, PWRAW_TOKENS, dev_mask, failed);
}

bool pwraw_active(int row)
{
    uint32_t b = PWRAW_BIT(row);

    if (failed & b)
        return false;
    if (force_off & b)
        return false;
    if (force_on & b)
        return true;
    return pwraw_why_on(pwraw_why_now(row));
}

void pwraw_set_dev_mask(uint32_t mask)
{
#ifdef HAVE_IPOD_UI_DEVMODE
    dev_mask = mask;
#else
    (void)mask;                 /* a release build: tokens only */
#endif
}

uint32_t pwraw_dev_mask(void)
{
    return dev_mask;
}

void pwraw_force(int row, int on)
{
    uint8_t b = (uint8_t)PWRAW_BIT(row);

    force_on &= ~b;
    force_off &= ~b;
    if (on > 0)
        force_on |= b;
    else if (on == 0)
        force_off |= b;
}

void pwraw_note(int row, bool act, uint32_t a, uint32_t b)
{
    struct pwraw_rec *r = &rec[row];

    r->a = a;
    r->b = b;
    if (act)
        r->acts++;              /* last: the drain reads it as the edge */
    else
        r->undos++;
}

void pwraw_fail(int row, uint32_t a, uint32_t b)
{
    struct pwraw_rec *r = &rec[row];

    failed |= PWRAW_BIT(row);
    r->a = a;
    r->b = b;
    r->fails++;
}

const struct pwraw_rec *pwraw_rec(int row)
{
    return &rec[row];
}

const char *pwraw_row_name(int row)
{
    return row >= 0 && row < PWRAW_N ? names[row] : "?";
}
