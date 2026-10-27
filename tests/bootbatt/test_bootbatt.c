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

/* Host test of the boot battery guard's decision
 * (apps/ipodui/bootbatt_core.c): threshold, hysteresis against the
 * runtime floors, median of the samples, power present, bad readings. */
#include <stdio.h>
#include <string.h>
#include "ipodui/bootbatt_core.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } \
    } while (0)

static enum bb_verdict all(int mv, int power)
{
    int s[BB_SAMPLES], i;
    for (i = 0; i < BB_SAMPLES; i++)
        s[i] = mv;
    return bb_decide(s, BB_SAMPLES, power, NULL);
}

/* both stages: the floor, then the latch */
static enum bb_verdict rule(int mv, int power, int drained)
{
    return bb_latch(all(mv, power), drained);
}

/* A model of firmware/powermgmt.c's voltage filter (average_init and
 * average_step, BATT_AVE_SAMPLES 128, one step per HZ/2): the slow path
 * (disk idle) and the 2-sample fast path (disk active below 8 %). Returns
 * how many of 'steps' steps battery_level_safe() (voltage_now > disksafe)
 * held, the filter seeded with 'seed' and fed 'load' every step. */
#define MODEL_AVE 128
static int model_safe_steps(int seed, int load, int steps, int disksafe,
                            int fast)
{
    int now = seed, avg = seed * MODEL_AVE, i, safe = 0;
    for (i = 0; i < steps; i++)
    {
        if (fast)
        {
            now = (load + now + 1) / 2;
            avg += now - avg / MODEL_AVE;
        }
        else
        {
            avg += load - avg / MODEL_AVE;
            now = avg / MODEL_AVE;
        }
        if (now > disksafe)
            safe++;
    }
    return safe;
}

int main(void)
{
    /* iPod Video: shutoff 3300 on both; disk-safe 3500 (hdd), 3400
     * (flash, since the 2026-10-22 battery table). The floor is fixed:
     * it does not follow disk-safe */
    const int ds = 3500, ds_flash = 3400, shutoff = 3300;
    int m;

    /* the floor: owner decision 2026-10-22 */
    CHECK(BB_FLOOR_MV == 3400);
    CHECK(BB_FLOOR_MV > shutoff && BB_FLOOR_MV < ds);
    CHECK(BB_FLOOR_MV == ds_flash);     /* not disk-safe + 100 = 3500 */

    /* the rule table, no power: a drained cell is refused whatever it
     * rests at; an undrained one only under the floor */
    CHECK(rule(3570, 0, 1) == BB_REFUSE_DRAINED);   /* the owner's rest */
    CHECK(rule(3576, 0, 1) == BB_REFUSE_DRAINED);
    CHECK(rule(4200, 0, 1) == BB_REFUSE_DRAINED);   /* no voltage escape */
    CHECK(rule(3350, 0, 1) == BB_REFUSE);           /* the floor first */
    CHECK(rule(3450, 0, 0) == BB_BOOT);
    CHECK(rule(3570, 0, 0) == BB_BOOT);
    CHECK(rule(4200, 0, 0) == BB_BOOT);
    CHECK(rule(3350, 0, 0) == BB_REFUSE);
    CHECK(rule(3362, 0, 0) == BB_REFUSE);           /* the owner's cut */
    /* the edge: at the floor boots, one below refuses */
    CHECK(rule(BB_FLOOR_MV, 0, 0) == BB_BOOT);
    CHECK(rule(BB_FLOOR_MV - 1, 0, 0) == BB_REFUSE);
    CHECK(rule(shutoff, 0, 0) == BB_REFUSE);
    /* batcal's fitted 0 % (disk-safe + 20) now boots unless drained */
    CHECK(rule(ds + 20, 0, 0) == BB_BOOT);
    CHECK(rule(ds + 20, 0, 1) == BB_REFUSE_DRAINED);

    /* external power: boots as always, drained or not, and clears it */
    CHECK(rule(3000, 1, 0) == BB_BOOT_POWER);
    CHECK(rule(3000, 1, 1) == BB_BOOT_POWER);
    CHECK(rule(3570, 1, 1) == BB_BOOT_POWER);
    CHECK(bb_clears(rule(3570, 1, 1)));
    CHECK(!bb_clears(rule(4200, 0, 0)));
    CHECK(!bb_clears(rule(4200, 0, 1)));
    CHECK(!bb_clears(rule(0, 0, 1)));

    /* no reading (ADC/I2C fault): boots unless drained (a cable rescues
     * and clears a drained one) */
    CHECK(rule(0, 0, 0) == BB_BOOT_NOREAD);
    CHECK(rule(BB_MV_MIN - 1, 0, 0) == BB_BOOT_NOREAD);
    CHECK(rule(6000, 0, 0) == BB_BOOT_NOREAD);
    CHECK(rule(0, 0, 1) == BB_REFUSE_DRAINED);
    CHECK(bb_decide(NULL, 0, 0, NULL) == BB_BOOT_NOREAD);

    /* refused verdicts */
    CHECK(bb_refused(BB_REFUSE) && bb_refused(BB_REFUSE_DRAINED));
    CHECK(!bb_refused(BB_BOOT) && !bb_refused(BB_BOOT_POWER)
          && !bb_refused(BB_BOOT_NOREAD));
    /* stage 1 never says drained; the latch never un-refuses */
    CHECK(bb_latch(BB_REFUSE, 0) == BB_REFUSE);
    CHECK(bb_latch(BB_REFUSE, 1) == BB_REFUSE);

    /* when the latch is written: the display at 0 % (the table's 0 %
     * point: 3,420 flash, 3,520 hdd), on battery, writes allowed, once */
    {
        const int z_flash = ds_flash + 20, z_hdd = ds + 20;
        CHECK(bb_zero_due(3420, z_flash, 0, 1, 0));
        CHECK(bb_zero_due(3405, z_flash, 0, 1, 0));
        CHECK(!bb_zero_due(3421, z_flash, 0, 1, 0));    /* 1 %: no */
        CHECK(!bb_zero_due(3420, z_flash, 1, 1, 0));    /* a charger */
        CHECK(!bb_zero_due(3420, z_flash, 0, 1, 1));    /* once */
        CHECK(!bb_zero_due(3399, z_flash, 0, 0, 0));    /* under disk-safe */
        CHECK(!bb_zero_due(0, z_flash, 0, 1, 0));       /* no reading */
        CHECK(bb_zero_due(3515, z_hdd, 0, 1, 0));
        CHECK(!bb_zero_due(3530, z_hdd, 0, 1, 0));
        /* the 0 % point sits over disk-safe, so the window exists */
        CHECK(z_flash > ds_flash && z_hdd > ds);
        /* the shutdown backup: never under disk-safe, never twice */
        CHECK(bb_shutdown_due(1, 0));
        CHECK(!bb_shutdown_due(0, 0));
        CHECK(!bb_shutdown_due(1, 1));
        /* 0 % shown, then off or dead: refused on battery at any rest
         * until a charger; above 0 % only the floor applies */
        CHECK(rule(3570, 0, 1) == BB_REFUSE_DRAINED);
        CHECK(rule(3450, 0, 0) == BB_BOOT);
    }

    /* a charge the firmware never saw (Apple's firmware, a charger that
     * did not boot ours): a rest median at least max(stored, built-in
     * 0 %) + 300 clears; the empty cell's rests (3,569-3,593 [silicon])
     * stay refused */
    {
        const int zf = ds_flash + 20;               /* 3,420 */
        CHECK(BB_RECHARGE_MV == 300);
        CHECK(bb_recharge_mv(zf, zf) == 3720);
        CHECK(bb_recharge_mv(3500, zf) == 3800);    /* a fit's higher 0 % */
        CHECK(bb_recharge_mv(0, zf) == 3720);       /* none stored */
        CHECK(bb_latch_rest(all(3720, 0), 1, 3720, zf, zf) == BB_BOOT_CHARGED);
        CHECK(bb_latch_rest(all(4150, 0), 1, 4150, zf, zf) == BB_BOOT_CHARGED);
        CHECK(bb_latch_rest(all(3719, 0), 1, 3719, zf, zf) == BB_REFUSE_DRAINED);
        CHECK(bb_latch_rest(all(3593, 0), 1, 3593, zf, zf) == BB_REFUSE_DRAINED);
        CHECK(bb_latch_rest(all(3569, 0), 1, 3569, zf, zf) == BB_REFUSE_DRAINED);
        CHECK(3593 + 120 <= bb_recharge_mv(zf, zf));
        CHECK(bb_latch_rest(all(3750, 0), 1, 3750, 3500, zf) == BB_REFUSE_DRAINED);
        CHECK(bb_clears(BB_BOOT_CHARGED));
        /* not drained, power, no reading: as bb_latch() */
        CHECK(bb_latch_rest(all(3450, 0), 0, 3450, zf, zf) == BB_BOOT);
        CHECK(bb_latch_rest(all(3570, 1), 1, 3570, zf, zf) == BB_BOOT_POWER);
        CHECK(bb_latch_rest(all(0, 0), 1, 0, zf, zf) == BB_REFUSE_DRAINED);
        CHECK(bb_latch_rest(all(3350, 0), 1, 3350, zf, zf) == BB_REFUSE);
    }

    /* the latch record: round trip; torn, foreign or zeroed reads as
     * not a record (the caller then boots on the floor alone) */
    {
        struct bb_latch_rec r, z;
        bb_latch_fill(&r, true, 7, 3420);
        CHECK(bb_latch_parse(&r) == 1 && r.seq == 7 && r.zero_mv == 3420);
        r.zero_mv = 3000;                           /* torn */
        CHECK(bb_latch_parse(&r) == -1);
        bb_latch_fill(&r, false, 8, 0);
        CHECK(bb_latch_parse(&r) == 0);
        r.drained = 1;                              /* torn */
        CHECK(bb_latch_parse(&r) == -1);
        bb_latch_fill(&r, true, 9, 3420);
        r.magic ^= 1;
        CHECK(bb_latch_parse(&r) == -1);
        memset(&z, 0, sizeof(z));                   /* just made */
        CHECK(bb_latch_parse(&z) == -1);
        CHECK(sizeof(r) == 20);
    }

    /* the median: one sagging or one high sample does not decide */
    {
        int s[BB_SAMPLES] = { 3450, 3220, 3460, 3440, 3455 };
        CHECK(bb_decide(s, BB_SAMPLES, 0, &m) == BB_BOOT);
        CHECK(m == 3450);
    }
    {
        int s[BB_SAMPLES] = { 3350, 3900, 3340, 3360, 3345 };
        CHECK(bb_decide(s, BB_SAMPLES, 0, &m) == BB_REFUSE);
        CHECK(m == 3350);
    }
    {
        int s[3] = { 3700, 3500, 3610 };
        CHECK(bb_median(s, 3) == 3610);
        CHECK(s[0] == 3500 && s[1] == 3610 && s[2] == 3700);
    }

    /* M3: the filter seed. A 3,620 mV rest that sags 150 mV under the
     * boot load. Seeded from the guard's resting sample, the slow path
     * keeps battery_level_safe() true for the whole first minute (120
     * steps) and ends near 3,530; seeded from a reading under the load
     * (what the power thread's own first read would be) it is false from
     * the first step. */
    CHECK(rule(3620, 0, 0) == BB_BOOT);
    CHECK(model_safe_steps(3620, 3620 - 150, 120, ds, 0) == 120);
    CHECK(model_safe_steps(3620 - 150, 3620 - 150, 120, ds, 0) == 0);
    /* the fast path (the disk active and the level under 8 %: the
     * upstream brown-out watch, unchanged) follows the loaded voltage
     * within a few steps whatever the seed: writes stop while the load
     * holds the cell under disk-safe, by design */
    CHECK(model_safe_steps(3620, 3620 - 150, 120, ds, 1) < 6);
    /* the floor's consequence on hdd: a cell resting at 3,400-3,500
     * boots with writes already off (battery_level_safe() false from the
     * seed); on flash the same rest keeps them on */
    CHECK(model_safe_steps(3450, 3450, 120, ds, 0) == 0);
    CHECK(model_safe_steps(3450, 3450, 120, ds_flash, 0) == 120);
    /* the latch is set at the shutoff, under disk-safe on both: its one
     * sector does not test battery_level_safe() */
    CHECK(shutoff < ds_flash && shutoff < ds);

    CHECK(!strcmp(bb_verdict_name(BB_REFUSE), "refuse"));
    CHECK(!strcmp(bb_verdict_name(BB_REFUSE_DRAINED), "drained"));
    CHECK(!strcmp(bb_verdict_name(BB_BOOT_POWER), "power"));
    CHECK(!strcmp(bb_verdict_name(BB_BOOT_CHARGED), "charged"));

    if (fails)
        printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
