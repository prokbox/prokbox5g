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

/* Host test of the fitted cell's capacity, sag and curve
 * (apps/ipodui/battcurve.c; one-build brief WP3). Every expected value
 * is a literal, worked out by hand from the formula in battcurve.h
 * (python: floor(A + (v - A) * (top - sag - A) / (top - A) + 0.5)),
 * never by the code under test. */
#include <stdio.h>
#include <string.h>
#include "ipodui/battcurve.h"
#include "ipodui/batcal_core.h"   /* bc_sane(): batcal's own limits */

/* batcal_core.c's file CRC; not used by bc_sane() */
uint32_t crc_32(const void *src, uint32_t len, uint32_t crc)
{
    (void)src; (void)len;
    return crc;
}

static int fails, checks;

#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

/* the storage base rows, as powermgmt-ipod-pcf.c has them (the owner's
 * 2026-10-22 rundown, docs/hardware.md "Measured discharge curve") */
static const uint16_t rot[11] =
    { 3520, 3570, 3605, 3635, 3665, 3725, 3810, 3900, 3975, 4060, 4170 };
static const uint16_t solid[11] =
    { 3420, 3500, 3565, 3605, 3635, 3675, 3750, 3865, 3955, 4045, 4170 };

static int row_eq(const uint16_t *got, const uint16_t *want, const char *what)
{
    if (!memcmp(got, want, 11 * sizeof(*got)))
        return 1;
    fprintf(stderr, "%s: got", what);
    for (int i = 0; i < 11; i++)
        fprintf(stderr, " %u", got[i]);
    fprintf(stderr, "\n");
    return 0;
}

static void scale_case(const uint16_t *base, int sag, const uint16_t *want,
                       const char *what)
{
    uint16_t out[11], inplace[11];

    memset(out, 0xa5, sizeof(out));
    battcurve_scale(base, sag, out);
    CHECK(row_eq(out, want, what));
    CHECK(bc_sane(out));        /* a table batcal itself would accept */
    memcpy(inplace, base, sizeof(inplace));     /* in == out allowed */
    battcurve_scale(inplace, sag, inplace);
    CHECK(row_eq(inplace, want, what));
}

int main(void)
{
    /* capacity: the answer, the original cell by RAM; unanswered = the
     * original cell (deep-sleep timing on the safe side) */
    CHECK(battcurve_mah(BATTCELL_STOCK, 32) == 650);
    CHECK(battcurve_mah(BATTCELL_STOCK, 64) == 850);
    CHECK(battcurve_mah(BATTCELL_2000, 32) == 2000);
    CHECK(battcurve_mah(BATTCELL_2000, 64) == 2000);
    CHECK(battcurve_mah(BATTCELL_3000, 32) == 3000);
    CHECK(battcurve_mah(BATTCELL_3000, 64) == 3000);
    CHECK(battcurve_mah(BATTCELL_UNSET, 32) == 650);
    CHECK(battcurve_mah(BATTCELL_UNSET, 64) == 850);

    /* sag: stock 35/30, 2,000 10, 3,000 and unanswered 0 */
    CHECK(battcurve_sag(BATTCELL_STOCK, 32) == 35);
    CHECK(battcurve_sag(BATTCELL_STOCK, 64) == 30);
    CHECK(battcurve_sag(BATTCELL_2000, 32) == 10);
    CHECK(battcurve_sag(BATTCELL_2000, 64) == 10);
    CHECK(battcurve_sag(BATTCELL_3000, 64) == 0);
    CHECK(battcurve_sag(BATTCELL_UNSET, 32) == 0);
    CHECK(battcurve_sag(BATTCELL_UNSET, 64) == 0);

    /* sag 0 (3,000 mAh, unanswered): the base row itself */
    scale_case(rot, 0, rot, "rotating sag 0");
    scale_case(solid, 0, solid, "solid sag 0");

    /* rotating drive */
    {
        static const uint16_t s35[11] = { 3520, 3567, 3600, 3629, 3657,
            3714, 3794, 3880, 3951, 4031, 4135 };
        static const uint16_t s30[11] = { 3520, 3568, 3601, 3630, 3658,
            3716, 3797, 3882, 3954, 4035, 4140 };
        static const uint16_t s10[11] = { 3520, 3569, 3604, 3633, 3663,
            3722, 3806, 3894, 3968, 4052, 4160 };
        scale_case(rot, 35, s35, "rotating stock 32 MB");
        scale_case(rot, 30, s30, "rotating stock 64 MB");
        scale_case(rot, 10, s10, "rotating 2,000 mAh");
    }
    /* solid drive */
    {
        static const uint16_t s35[11] = { 3420, 3496, 3558, 3596, 3625,
            3663, 3735, 3844, 3930, 4016, 4135 };
        static const uint16_t s30[11] = { 3420, 3497, 3559, 3598, 3626,
            3665, 3737, 3847, 3934, 4020, 4140 };
        static const uint16_t s10[11] = { 3420, 3499, 3563, 3603, 3632,
            3672, 3746, 3859, 3948, 4037, 4160 };
        scale_case(solid, 35, s35, "solid stock 32 MB");
        scale_case(solid, 30, s30, "solid stock 64 MB");
        scale_case(solid, 10, s10, "solid 2,000 mAh");
    }

    /* the end to end: answer -> sag -> curve, for the owner's unit (64 MB,
     * solid) and a stock 30 GB (32 MB, rotating) */
    {
        uint16_t out[11];
        static const uint16_t thin[11] = { 3520, 3567, 3600, 3629, 3657,
            3714, 3794, 3880, 3951, 4031, 4135 };
        battcurve_scale(solid, battcurve_sag(BATTCELL_3000, 64), out);
        CHECK(row_eq(out, solid, "owner's unit"));
        battcurve_scale(rot, battcurve_sag(BATTCELL_STOCK, 32), out);
        CHECK(row_eq(out, thin, "stock 30 GB"));
    }

    /* a degenerate row (flat) or an absurd sag is copied, never wrapped */
    {
        static const uint16_t flat[11] = { 3700, 3700, 3700, 3700, 3700,
            3700, 3700, 3700, 3700, 3700, 3700 };
        uint16_t out[11];
        battcurve_scale(flat, 30, out);
        CHECK(row_eq(out, flat, "flat row"));
        battcurve_scale(rot, 5000, out);
        CHECK(row_eq(out, rot, "sag over the span"));
        battcurve_scale(rot, -20, out);
        CHECK(row_eq(out, rot, "negative sag"));
    }

    printf("battcurve: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
