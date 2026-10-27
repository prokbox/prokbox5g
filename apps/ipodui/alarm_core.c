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
 * iPod UI shell: alarms, the pure rules (P12, docs/phase12-brief.md 3.3)
 * No I/O, no clock, no PMU: host-tested in tests/alarm
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
#include <stddef.h>
#include "ipodui/alarm_core.h"
#include "crc32.h"

#define DAY_S 86400u

/* -- calendar (2000..2099, every 4th year a leap year) ------------------- */

static int days_in_month(int year, int mon)
{
    static const unsigned char dim[12] = { 31, 28, 31, 30, 31, 30,
                                           31, 31, 30, 31, 30, 31 };
    return dim[mon - 1] + (mon == 2 && (year & 3) == 0);
}

static bool date_ok(int year, int mon, int mday)
{
    return year >= 2000 && year <= 2099 && mon >= 1 && mon <= 12
           && mday >= 1 && mday <= days_in_month(year, mon);
}

/* days since 2000-01-01 */
static uint32_t days_from(int year, int mon, int mday)
{
    uint32_t y = (uint32_t)(year - 2000);
    uint32_t d = y * 365 + (y + 3) / 4;     /* leap days of 2000..year-1 */
    int m;

    for (m = 1; m < mon; m++)
        d += days_in_month(year, m);
    return d + (uint32_t)(mday - 1);
}

uint32_t alarm_rtc_make(int year, int mon, int mday, int h, int m, int s)
{
    return days_from(year, mon, mday) * DAY_S
           + (uint32_t)(h * 3600 + m * 60 + s);
}

void alarm_rtc_split(uint32_t t, int *year, int *mon, int *mday,
                     int *h, int *m, int *s)
{
    uint32_t d = t / DAY_S, r = t % DAY_S;
    int y = 2000, mo = 1;

    for (;;)
    {
        uint32_t yd = (y & 3) ? 365 : 366;
        if (d < yd)
            break;
        d -= yd;
        y++;
    }
    for (;;)
    {
        uint32_t md = (uint32_t)days_in_month(y, mo);
        if (d < md)
            break;
        d -= md;
        mo++;
    }
    *year = y;
    *mon = mo;
    *mday = (int)d + 1;
    *h = (int)(r / 3600);
    *m = (int)(r / 60 % 60);
    *s = (int)(r % 60);
}

int alarm_rtc_wday(uint32_t t)
{
    return (int)((t / DAY_S + 6) % 7);      /* 2000-01-01: a Saturday */
}

/* -- the next ring --------------------------------------------------------- */

static unsigned day_mask(const struct alarm *a)
{
    switch (a->repeat)
    {
    case AR_DAILY:    return 0x7f;
    case AR_WEEKDAYS: return ALARM_DAYS_WEEKDAYS;
    case AR_WEEKENDS: return ALARM_DAYS_WEEKENDS;
    case AR_DAYS:     return a->days & 0x7f;
    default:          return 0;
    }
}

/* the base time (no snooze) strictly after now, 0 = none */
static uint32_t base_next(const struct alarm *a, uint32_t now)
{
    uint32_t tod, day;
    unsigned mask;
    int k;

    if (a->hour > 23 || a->min > 59)
        return 0;
    tod = (uint32_t)a->hour * 3600 + (uint32_t)a->min * 60;
    if (a->repeat == AR_ONCE)
    {
        uint32_t t;
        if (!date_ok(a->year, a->mon, a->mday))
            return 0;
        t = days_from(a->year, a->mon, a->mday) * DAY_S + tod;
        return t > now ? t : 0;
    }
    mask = day_mask(a);
    if (!mask)
        return 0;
    day = now / DAY_S;
    for (k = 0; k <= 7; k++, day++)
    {
        uint32_t t = day * DAY_S + tod;
        if (t > now && (mask & (1u << ((day + 6) % 7))))
            return t;
    }
    return 0;
}

uint32_t alarm_next(const struct alarm *a, int n, uint32_t now, int *which)
{
    uint32_t best = 0;
    int i, w = -1;

    for (i = 0; i < n; i++)
    {
        uint32_t t;
        if (!a[i].on)
            continue;
        t = base_next(&a[i], now);
        if (a[i].snooze_until > now && (!t || a[i].snooze_until < t))
            t = a[i].snooze_until;
        if (t && (!best || t < best))
        {
            best = t;
            w = i;
        }
    }
    if (which)
        *which = w;
    return best;
}

void alarm_after_ring(struct alarm *a, uint32_t now, bool snoozed)
{
    a->missed = 0;                      /* Q17: it rang, so it is seen */
    if (snoozed && a->snooze && a->snoozes < ALARM_SNOOZE_MAX)
    {
        a->snooze_until = now + ALARM_SNOOZE_S;
        a->snoozes++;
        return;
    }
    a->snooze_until = 0;
    a->snoozes = 0;
    if (a->repeat == AR_ONCE)
        a->on = 0;
}

bool alarm_is_boot(uint32_t armed, uint32_t now)
{
    return armed && now >= armed && now - armed <= ALARM_BOOT_WIN_S;
}

void alarm_mark_missed(struct alarm *a, int n, uint32_t armed, uint32_t now)
{
    int i;

    if (!armed || now < armed)
        return;
    for (i = 0; i < n; i++)
    {
        /* its first occurrence (or snooze) at or after the armed time */
        uint32_t t = alarm_next(&a[i], 1, armed - 1, NULL);
        if (!t || t > now)
            continue;
        a[i].missed = 1;
        a[i].missed_at = t;
        a[i].snooze_until = 0;
        a[i].snoozes = 0;
        if (a[i].repeat == AR_ONCE)
            a[i].on = 0;
    }
}

/* -- PMU register image ---------------------------------------------------- */

static uint8_t bcd(int v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

void alarm_pmu_regs(uint32_t armed, bool daily, uint8_t regs[7])
{
    static const uint8_t ignore[7] = ALARM_REG_IGNORE;
    int y, mo, d, h, m, s;

    alarm_rtc_split(armed, &y, &mo, &d, &h, &m, &s);
    regs[0] = bcd(s);
    regs[1] = bcd(m);
    regs[2] = bcd(h);
    regs[3] = ignore[3];                /* the weekday: never compared (M9) */
    if (daily)
    {
        regs[4] = ignore[4];
        regs[5] = ignore[5];
        regs[6] = ignore[6];
    }
    else
    {
        regs[4] = bcd(d);
        regs[5] = bcd(mo);
        regs[6] = bcd(y - 2000);
    }
}

/* -- alarms.dat ------------------------------------------------------------ */

#define HDR_BYTES offsetof(struct alarms_file, a)

void alarms_file_seal(struct alarms_file *f, int count)
{
    f->magic = ALARMS_MAGIC;
    f->version = ALARMS_VERSION;
    f->count = (uint16_t)count;
    f->crc = crc_32(f->a, (uint32_t)count * sizeof(struct alarm), ~0u);
}

int alarms_file_check(const struct alarms_file *f, uint32_t bytes)
{
    if (bytes < HDR_BYTES)
        return ALARMS_ESHORT;
    if (f->magic != ALARMS_MAGIC || f->version != ALARMS_VERSION)
        return ALARMS_EMAGIC;
    if (f->count > ALARM_MAX)
        return ALARMS_ECOUNT;
    if (bytes < HDR_BYTES + f->count * sizeof(struct alarm))
        return ALARMS_ESHORT;
    if (f->crc != crc_32(f->a, (uint32_t)f->count * sizeof(struct alarm),
                         ~0u))
        return ALARMS_ECRC;
    return ALARMS_OK;
}
