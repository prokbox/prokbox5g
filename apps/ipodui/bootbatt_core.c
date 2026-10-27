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

/* Boot battery guard: the pure decision (bootbatt_core.h). */
#include "ipodui/bootbatt_core.h"

int bb_median(int *mv, int n)
{
    int i, j;

    if (n <= 0)
        return 0;
    if (n > BB_SAMPLES)
        n = BB_SAMPLES;
    for (i = 1; i < n; i++)             /* insertion sort, n <= 5 */
    {
        int v = mv[i];
        for (j = i; j > 0 && mv[j - 1] > v; j--)
            mv[j] = mv[j - 1];
        mv[j] = v;
    }
    return mv[n / 2];
}

enum bb_verdict bb_decide(int *mv, int n, bool power, int *median_out)
{
    int m = bb_median(mv, n);

    if (median_out)
        *median_out = m;
    if (power)
        return BB_BOOT_POWER;
    if (n <= 0 || m < BB_MV_MIN || m > BB_MV_MAX)
        return BB_BOOT_NOREAD;
    return m < BB_FLOOR_MV ? BB_REFUSE : BB_BOOT;
}

enum bb_verdict bb_latch(enum bb_verdict v, bool drained)
{
    if (drained && (v == BB_BOOT || v == BB_BOOT_NOREAD))
        return BB_REFUSE_DRAINED;
    return v;
}

int bb_recharge_mv(int stored_zero, int builtin_zero)
{
    return (stored_zero > builtin_zero ? stored_zero : builtin_zero)
           + BB_RECHARGE_MV;
}

enum bb_verdict bb_latch_rest(enum bb_verdict v, bool drained, int rest_mv,
                              int stored_zero, int builtin_zero)
{
    if (drained && v == BB_BOOT
        && rest_mv >= bb_recharge_mv(stored_zero, builtin_zero))
        return BB_BOOT_CHARGED;
    return bb_latch(v, drained);
}

bool bb_clears(enum bb_verdict v)
{
    return v == BB_BOOT_POWER || v == BB_BOOT_CHARGED;
}

bool bb_refused(enum bb_verdict v)
{
    return v == BB_REFUSE || v == BB_REFUSE_DRAINED;
}

bool bb_zero_due(int filtered_mv, int zero_mv, bool power, bool safe,
                 bool latched)
{
    return !power && !latched && safe && filtered_mv > 0
           && filtered_mv <= zero_mv;
}

bool bb_shutdown_due(bool safe, bool latched)
{
    return safe && !latched;
}

static uint32_t latch_sum(const struct bb_latch_rec *r)
{
    /* FNV-1a over the four words, byte order as stored */
    const uint8_t *p = (const uint8_t *)r;
    uint32_t h = 2166136261u;
    unsigned int i;

    for (i = 0; i < 4 * sizeof(uint32_t); i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

void bb_latch_fill(struct bb_latch_rec *r, bool drained, uint32_t seq,
                   int zero_mv)
{
    r->magic = BB_LATCH_MAGIC;
    r->drained = drained ? 1 : 0;
    r->seq = seq;
    r->zero_mv = zero_mv > 0 ? (uint32_t)zero_mv : 0;
    r->sum = latch_sum(r);
}

int bb_latch_parse(const struct bb_latch_rec *r)
{
    if (r->magic != BB_LATCH_MAGIC || r->drained > 1
        || r->sum != latch_sum(r))
        return -1;
    return (int)r->drained;
}

const char *bb_verdict_name(enum bb_verdict v)
{
    switch (v)
    {
    case BB_BOOT:           return "boot";
    case BB_BOOT_POWER:     return "power";
    case BB_BOOT_NOREAD:    return "noread";
    case BB_REFUSE:         return "refuse";
    case BB_REFUSE_DRAINED: return "drained";
    case BB_BOOT_CHARGED:   return "charged";
    }
    return "?";
}
