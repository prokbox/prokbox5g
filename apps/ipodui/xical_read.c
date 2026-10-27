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

/* iPod UI shell: the Calendars index's reader side (P12 W3-C): civil time
 * and the meta and table checks (xical_core.h). Linked into both modules
 * that use it (MOD_XINDEX's builder, MOD_CALENDARS's screens: modules
 * cannot call each other), or once in a resident build. Host-tested in
 * tests/xical */
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "crc32.h"
#include "ipodui/xical_core.h"

static int64_t fdiv(int64_t a, int64_t b)       /* floor division, b > 0 */
{
    int64_t q = a / b;

    return (a % b < 0) ? q - 1 : q;
}

int64_t xcal_days(int y, int m, int d)
{
    int64_t era, yoe, doy;

    y -= m <= 2;
    era = fdiv(y, 400);
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 730425;
}

void xcal_civil(int64_t days, int *y, int *m, int *d)
{
    int64_t z = days + 730425, era, doe, yoe, doy, mp;

    era = fdiv(z, 146097);
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

int xcal_wday(int64_t days)
{
    return (int)(((days % 7) + 7 + 6) % 7);     /* 2000-01-01: Saturday */
}

int xcal_mdays(int y, int m)
{
    static const uint8_t n[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31,
                                   30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return n[(m - 1) % 12];
}

int xcal_meta_check(const struct xidx_hdr *h, const struct xcal_meta *m,
                    uint32_t file_bytes)
{
    uint32_t tab = xcal_tab_off(h);

    if (h->kind != XIDX_CAL || !m->ndays || m->ndays > XCAL_DAYS_MAX
        || m->ncal > XCAL_MAX_CALS || m->bm_bytes != (m->ndays + 7u) / 8u
        || m->nevents > h->count
        || tab + XCAL_TAB_BYTES(m->ndays, m->ncal) > h->rec_off
        || h->str_off > file_bytes)
        return XIDX_EBOUNDS;
    return XIDX_OK;
}

int xcal_tab_check(const void *tab, uint32_t len)
{
    const struct xcal_meta *m = tab;
    const uint8_t *p = tab;
    const uint32_t at = offsetof(struct xcal_meta, tab_crc), zero = 0;
    uint32_t tb, crc;

    if (len < sizeof(*m) || m->ndays > XCAL_DAYS_MAX
        || m->ncal > XCAL_MAX_CALS)
        return XIDX_EBOUNDS;
    tb = XCAL_TAB_BYTES(m->ndays, m->ncal);
    if (len < tb)
        return XIDX_ESHORT;
    crc = crc_32(p, at, 0xffffffffu);
    crc = crc_32(&zero, 4, crc);
    crc = crc_32(p + at + 4, tb - at - 4, crc);
    return crc == m->tab_crc ? XIDX_OK : XIDX_ECRC;
}
