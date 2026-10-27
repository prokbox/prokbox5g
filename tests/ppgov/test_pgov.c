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
 * Host test of the PP5022 band governor's policy (firmware/pgov_core.c,
 * docs/design/pp-governor.md section 10.1): the selector, the voltage
 * layer and its ordering, the fallback, the record and the tables.
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
#include <stdio.h>
#include <string.h>
#include "pgov.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

/* deterministic parameters: the need is the sample (weight 1), H 80 */
static void params(struct pgov_params *p, int inband)
{
    pgov_params_default(p);
    p->ewma_shift = 0;
    p->headroom_pct = 80;
    p->down_pct = 80;
    p->sat_samples = 2;
    p->dwell_ms[PGOV_B1] = 500;
    p->dwell_ms[PGOV_B2] = 200;
    p->dwell_ms[PGOV_B3] = 100;
    p->inband = inband;
}

static struct pgov_in in_of(struct pgov_core *c, unsigned int busy)
{
    struct pgov_in in;
    memset(&in, 0, sizeof(in));
    in.busy_pm[0] = busy;
    in.cur_mhz = c->mhz;
    in.dt_ms = 10;
    return in;
}

/* busy permille that reads as `khz` of need at the current point */
static unsigned int busy_for(struct pgov_core *c, unsigned int khz)
{
    return khz / c->mhz;
}

static void step_n(struct pgov_core *c, struct pgov_in *in, int n)
{
    while (n--)
    {
        in->cur_mhz = c->mhz;
        pgov_core_step(c, in);
    }
}

static void test_selector(void)
{
    struct pgov_params p;
    struct pgov_core c;
    struct pgov_in in;

    params(&p, PGOV_INBAND_TOP);
    pgov_core_init(&c, &p, 0, 24);

    /* the headroom edge: 19.2 MHz fits 24, 19.3 does not */
    in = in_of(&c, 800);                /* 800 x 24 = 19.2 MHz */
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 24);
    in = in_of(&c, 805);                /* 19.32 MHz */
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 80);                 /* T, no band 1: band 2's top */
    CHECK(c.why == PGOV_WHY_LOAD && c.from == 24);

    /* the dwell: 200 ms below, not 190 */
    in = in_of(&c, 50);                 /* 4 MHz at 80 */
    step_n(&c, &in, 19);
    CHECK(c.mhz == 80);
    step_n(&c, &in, 1);
    CHECK(c.mhz == 24 && c.why == PGOV_WHY_DWELL);

    /* a gap of dwell - 10 ms, interrupted, never steps down */
    pgov_core_init(&c, &p, 0, 80);
    for (int i = 0; i < 10; i++)
    {
        in = in_of(&c, 50);
        step_n(&c, &in, 19);
        in = in_of(&c, 900);
        step_n(&c, &in, 1);
    }
    CHECK(c.mhz == 80);

    /* saturation: 2 samples >= 95 % at 30 go up before the EWMA would */
    p.ewma_shift = 4;
    pgov_core_init(&c, &p, 0, 30);
    in = in_of(&c, 960);
    in.floor_mhz = 30;
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 30);
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 80 && c.why == PGOV_WHY_SAT);
    p.ewma_shift = 0;

    /* floors: at least the floor; a boost at least 80, from a release the
     * dwell */
    pgov_core_init(&c, &p, 0, 24);
    in = in_of(&c, 100);
    in.floor_mhz = 30;
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 30 && c.why == PGOV_WHY_FLOOR);
    CHECK(pgov_core_boost(&c) == 80 && c.why == PGOV_WHY_BOOST);
    in = in_of(&c, 100);
    in.boost = 1;
    step_n(&c, &in, 50);
    CHECK(c.mhz == 80);
    in.boost = 0;
    in.floor_mhz = 30;
    step_n(&c, &in, 19);
    CHECK(c.mhz == 80);
    step_n(&c, &in, 1);
    CHECK(c.mhz == 30);                 /* in band 2: down to the floor */
    in.floor_mhz = 0;
    step_n(&c, &in, 20);
    CHECK(c.mhz == 24);

    /* a floor raised outside the tick (ATA) takes effect at once */
    pgov_core_init(&c, &p, 0, 24);
    CHECK(pgov_core_floor(&c, 30) == 30);
    CHECK(pgov_core_floor(&c, 0) == 30);    /* never lowers */
}

static void test_band1_band3(void)
{
    struct pgov_params p;
    struct pgov_core c;
    struct pgov_in in;

    /* band 1 live: 30 MHz is band 1, one band at a time down */
    params(&p, PGOV_INBAND_TOP);
    pgov_core_init(&c, &p, PGOV_LIVE_B1, 24);
    in = in_of(&c, 1000);               /* 24 MHz need: fits 30 x 0.8 */
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 30 && pgov_band_of(c.live, c.mhz) == PGOV_B1);
    pgov_core_boost(&c);
    CHECK(c.mhz == 80);
    in = in_of(&c, 10);
    step_n(&c, &in, 20);
    CHECK(c.mhz == 30);                 /* 2 -> 1 after 200 ms */
    step_n(&c, &in, 49);
    CHECK(c.mhz == 30);
    step_n(&c, &in, 1);
    CHECK(c.mhz == 24);                 /* 1 -> 0 after 500 ms */

    /* band 3: never from a boost; from saturation at 80; a cap leaves
     * it at once; down 100 ms */
    pgov_core_init(&c, &p, PGOV_LIVE_B1 | PGOV_LIVE_B3, 24);
    CHECK(pgov_core_boost(&c) == 80);
    in = in_of(&c, 700);                /* 56 MHz: band 2 */
    in.boost = 1;
    step_n(&c, &in, 5);
    CHECK(c.mhz == 80);
    in = in_of(&c, 1000);
    in.boost = 1;
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 80);                 /* the EWMA alone never enters 3 */
    in.cur_mhz = c.mhz;
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 100 && c.why == PGOV_WHY_SAT);
    in = in_of(&c, 1000);
    in.cap_mhz = 80;
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 80 && c.why == PGOV_WHY_CAP);
    in.cap_mhz = 0;
    in.cur_mhz = c.mhz;
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 80);                 /* a fresh saturated run first */
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 100);
    in = in_of(&c, 300);                /* 30 MHz at 100 */
    step_n(&c, &in, 9);
    CHECK(c.mhz == 100);
    step_n(&c, &in, 1);
    CHECK(c.mhz == 80);
    /* a floor of 100 without band 3 is 80 */
    pgov_core_init(&c, &p, 0, 24);
    in = in_of(&c, 0);
    in.floor_mhz = 100;
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 80);
}

static void test_proportional(void)
{
    struct pgov_params p;
    struct pgov_core c;
    struct pgov_in in;

    params(&p, PGOV_INBAND_PROP);
    pgov_core_init(&c, &p, 0, 24);
    in = in_of(&c, 1000);               /* 24 MHz: fits 30 x 0.8 */
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 30);
    in = in_of(&c, 1000);               /* 30 MHz: 48 */
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 48);
    in = in_of(&c, 1000);               /* 48: 60 x 0.8 = 48 fits */
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 60);
    in = in_of(&c, 1000);
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 80);
    /* with band 1 live, 30 is band 1's and band 2 starts at 48 */
    pgov_core_init(&c, &p, PGOV_LIVE_B1, 30);
    in = in_of(&c, 900);                /* 27 MHz */
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 48 && pgov_band_of(c.live, 48) == PGOV_B2);

    /* the down threshold: up at H, down only below dn of the lower point */
    p.down_pct = 110;
    pgov_core_init(&c, &p, 0, 48);
    in = in_of(&c, busy_for(&c, 34000));    /* > 30 x 1.1: stays */
    step_n(&c, &in, 30);
    CHECK(c.mhz == 48);
    in = in_of(&c, busy_for(&c, 32000));    /* > 30 x 0.8, <= 30 x 1.1 */
    step_n(&c, &in, 19);
    CHECK(c.mhz == 48);
    step_n(&c, &in, 1);
    CHECK(c.mhz == 30 && c.why == PGOV_WHY_DWELL);
}

static void test_voltage(void)
{
    struct pgov_params p;
    struct pgov_core c;
    struct pgov_in in;

    params(&p, PGOV_INBAND_TOP);
    pgov_core_init(&c, &p, PGOV_LIVE_B1, 24);
    in = in_of(&c, 100);
    step_n(&c, &in, 400);
    CHECK(c.vcode == PGOV_V_NORM);      /* not armed */
    pgov_core_arm(&c, true);
    step_n(&c, &in, 199);
    CHECK(c.vcode == PGOV_V_NORM);
    step_n(&c, &in, 1);
    CHECK(c.vcode == PGOV_V_LOW && c.vstate == PGOV_VS_LOW);  /* 2 s */
    CHECK(pgov_core_valid(&c, c.mhz, c.vcode));

    /* a boost while low: band 1's top now, the raise pending, then 80 */
    CHECK(pgov_core_boost(&c) == 30);
    CHECK(c.vcode == PGOV_V_NORM && c.vstate == PGOV_VS_RAISING);
    in = in_of(&c, 100);
    in.boost = 1;
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 80 && c.vstate == PGOV_VS_NORM);   /* settled: 10 ms */

    /* a floor while raising stays at band 1's top */
    pgov_core_init(&c, &p, PGOV_LIVE_B1, 24);
    pgov_core_arm(&c, true);
    in = in_of(&c, 100);
    step_n(&c, &in, 200);
    CHECK(c.vcode == PGOV_V_LOW);
    pgov_core_boost(&c);
    CHECK(pgov_core_floor(&c, 80) == 30);

    /* a storage write raises the voltage; the clock may stay */
    pgov_core_init(&c, &p, PGOV_LIVE_B1, 24);
    pgov_core_arm(&c, true);
    in = in_of(&c, 100);
    step_n(&c, &in, 200);
    in.vwrite = 1;
    pgov_core_step(&c, &in);
    CHECK(c.vcode == PGOV_V_NORM && c.mhz == 24);
    step_n(&c, &in, 300);
    CHECK(c.vcode == PGOV_V_NORM);      /* not low while writing */

    /* the fallback: normal at once, off for the boot, band 1 gone */
    pgov_core_init(&c, &p, PGOV_LIVE_B1, 24);
    pgov_core_arm(&c, true);
    in = in_of(&c, 100);
    step_n(&c, &in, 200);
    CHECK(pgov_core_fallback(&c));
    CHECK(c.vcode == PGOV_V_NORM && !c.dvs_ok && !(c.live & PGOV_LIVE_B1));
    step_n(&c, &in, 1000);
    CHECK(c.vcode == PGOV_V_NORM);
    CHECK(!pgov_core_fallback(&c));
    /* no band 1 on the unit: never armed */
    pgov_core_init(&c, &p, 0, 24);
    pgov_core_arm(&c, true);
    CHECK(!c.dvs_ok);
}

/* invariants 1-3 over random inputs, after every decision */
static void test_random(void)
{
    struct pgov_params p;
    struct pgov_core c;
    uint32_t r = 12345;
    int bad = 0;

    for (int arm = 0; arm < 4; arm++)
    {
        unsigned int live = (arm & 1 ? PGOV_LIVE_B1 : 0)
                          | (arm & 2 ? PGOV_LIVE_B3 : 0);
        pgov_params_default(&p);
        p.vdwell_ms = 50;
        pgov_core_init(&c, &p, live, 24);
        pgov_core_arm(&c, true);
        for (long i = 0; i < 250000; i++)
        {
            struct pgov_in in;
            unsigned int m0 = c.mhz, v0 = c.vcode, s0 = c.vstate;
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            memset(&in, 0, sizeof(in));
            in.busy_pm[0] = r % 1001;
            in.busy_pm[1] = (r >> 10) % 1001;
            if ((r >> 20) % 4 == 0)
                in.busy_pm[0] = in.busy_pm[1] = 0;
            in.cur_mhz = c.mhz;
            in.floor_mhz = ((r >> 22) % 8 == 0) ? 30
                         : ((r >> 22) % 64 == 1) ? 100 : 0;
            in.cap_mhz = ((r >> 25) % 16 == 0) ? 80 : 0;
            in.boost = (r >> 27) % 8 == 0;
            in.vwrite = (r >> 30) == 0;
            in.dt_ms = 10;
            if ((r >> 12) % 64 == 0)
                pgov_core_boost(&c);
            else if ((r >> 12) % 64 == 1)
                pgov_core_floor(&c, 30);
            else
                pgov_core_step(&c, &in);
            if (!pgov_core_valid(&c, c.mhz, c.vcode) && bad++ < 3)
                fprintf(stderr, "valid: %u MHz 0x%x\n", c.mhz, c.vcode);
            /* down only with the clock already at band 1 or lower */
            if (v0 == PGOV_V_NORM && c.vcode == PGOV_V_LOW
                && (m0 > 30 || c.mhz > 30) && bad++ < 3)
                fprintf(stderr, "low: %u -> %u\n", m0, c.mhz);
            /* above band 1 only from a normal voltage set at least one
             * sample (the settle) before */
            if (c.mhz > 30 && m0 <= 30
                && (s0 == PGOV_VS_LOW || v0 != PGOV_V_NORM) && bad++ < 3)
                fprintf(stderr, "up: %u -> %u vs %u i=%ld\n", m0, c.mhz, s0,
                        i);
            if (c.mhz == 100 && !(live & PGOV_LIVE_B3))
                bad++;
        }
    }
    CHECK(bad == 0);
}

static void test_record(void)
{
    struct pgov_record r;

    memset(&r, 0, sizeof(r));
    CHECK(pgov_rec_boot(&r, true));
    pgov_rec_armed(&r);
    CHECK(r.dvs == PGOV_REC_TRIAL);
    /* an unclean end: one strike skips the next boot only */
    CHECK(!pgov_rec_boot(&r, false));
    CHECK(r.dvs == PGOV_REC_TRIAL);
    CHECK(pgov_rec_boot(&r, true));
    /* a clean armed boot, then a second unclean within five: off */
    pgov_rec_armed(&r);
    pgov_rec_clean_end(&r, 10);
    CHECK(pgov_rec_boot(&r, true));
    pgov_rec_armed(&r);
    CHECK(!pgov_rec_boot(&r, false));
    CHECK(r.dvs == PGOV_REC_OFF && r.code == PGOV_FB_UNCLEAN);
    CHECK(!pgov_rec_boot(&r, true));
    /* the developer reset */
    pgov_rec_reset(&r);
    CHECK(pgov_rec_boot(&r, true));
    /* strikes six armed boots apart do not add up */
    for (int i = 0; i < 3; i++)
    {
        pgov_rec_armed(&r);
        CHECK(!pgov_rec_boot(&r, false));
        for (int j = 0; j < 5; j++)
        {
            CHECK(pgov_rec_boot(&r, true));
            pgov_rec_armed(&r);
            pgov_rec_clean_end(&r, 0);
        }
        CHECK(pgov_rec_boot(&r, true));
    }
    CHECK(r.dvs == PGOV_REC_TRIAL);
    /* a runtime trigger is attributable: off at once */
    pgov_rec_armed(&r);
    pgov_rec_fail(&r, PGOV_FB_CRC);
    CHECK(r.dvs == PGOV_REC_OFF && r.code == PGOV_FB_CRC);
    CHECK(!pgov_rec_boot(&r, false));
    /* ten clean hours: ok */
    pgov_rec_reset(&r);
    for (int i = 0; i < 10; i++)
    {
        CHECK(pgov_rec_boot(&r, true));
        pgov_rec_armed(&r);
        pgov_rec_clean_end(&r, 3600);
    }
    CHECK(r.dvs == PGOV_REC_OK);
}

static void test_tables(void)
{
    static const unsigned int pts[] = { 30, 48, 60, 80, 100 };

    /* Rockbox's words kept; the others by Apple's recipe */
    CHECK(pgov_pll_word(30) == 0x8a220501u);
    CHECK(pgov_pll_word(80) == 0x8a121403u);
    CHECK(pgov_pll_word(24) == 0 && pgov_pll_word(66) == 0);
    /* the literals in set_cpu_frequency() (system-pp502x.c) */
    CHECK(pgov_pll_word(48) == 0x8a22600cu);
    CHECK(pgov_pll_word(60) == 0x8a123c0cu);
    for (unsigned int i = 0; i < sizeof(pts)/sizeof(pts[0]); i++)
    {
        uint32_t w = pgov_pll_word(pts[i]);
        unsigned int n = w & 0x1f, m = (w >> 8) & 0x7f;
        unsigned int post = 1u << ((w >> 20) & 3);
        CHECK((w & ~0x00307f1fu) == 0x8a020000u);  /* N 5 bits, M 7 bits */
        CHECK(n && 24 * m % n == 0);
        CHECK(24 * m / n >= 96);                     /* the VCO */
        CHECK(24 * m / n / post == pts[i]);
        CHECK(post == (pts[i] >= 60 ? 2u : 4u));     /* Apple's ranges */
    }
    /* DEV_TIMING1: Apple's 90 ns rule, floored at Rockbox's values */
    CHECK(pgov_dev_timing1(24) == 0x0303);
    CHECK(pgov_dev_timing1(30) == 0x0303);
    CHECK(pgov_dev_timing1(48) == 0x0404);
    CHECK(pgov_dev_timing1(60) == 0x0808);
    CHECK(pgov_dev_timing1(80) == 0x0808);
    CHECK(pgov_dev_timing1(100) == 0x0808);
    CHECK(pgov_dev_timing1(125) == 0x0b0b);
    /* the grade */
    CHECK(pgov_grade_mhz(0x00) == 133);
    CHECK(pgov_grade_mhz(0x20) == 100);
    CHECK(pgov_grade_mhz(0x40) == 80);
    CHECK(pgov_grade_mhz(0xffffff7fu) == 66);
    CHECK(pgov_vceil_mhz(PGOV_LIVE_B1, PGOV_V_LOW) == 30);
    CHECK(pgov_vceil_mhz(0, PGOV_V_NORM) == 80);
    CHECK(pgov_vceil_mhz(PGOV_LIVE_B3, PGOV_V_NORM) == 100);
}

static void test_busy(void)
{
    struct pgov_params p;
    struct pgov_core c;
    struct pgov_in in;

    CHECK(pgov_busy_pm(10000, 10000) == 0);     /* a parked core */
    CHECK(pgov_busy_pm(12000, 10000) == 0);
    CHECK(pgov_busy_pm(0, 10000) == 1000);
    CHECK(pgov_busy_pm(4500, 10000) == 550);
    CHECK(pgov_busy_pm(0, 0) == 0);
    CHECK(pgov_busy_pm(3000000, 6000000) == 500);
    /* the need is the busier core's */
    params(&p, PGOV_INBAND_TOP);
    pgov_core_init(&c, &p, 0, 24);
    in = in_of(&c, 100);
    in.busy_pm[1] = 900;
    pgov_core_step(&c, &in);
    CHECK(c.mhz == 80);
    CHECK(pgov_need_mhz10(&c) == 216);
}


/* ---- Stage 2 (section 16): the apply layer ----------------------------- */

static uint32_t rnd(uint32_t *r)
{
    *r ^= *r << 13; *r ^= *r >> 17; *r ^= *r << 5;
    return *r;
}

/* Run pgov_hw_next() to the end against a model of the hardware: every
 * CLOCK must be within what the rail carries at that instant, every VDOWN
 * must find the clock in band 0/1.  Returns the op sequence's length (or
 * -1 on a violation) and writes the ops into seq */
static int drive(struct pgov_hw *hw, unsigned int mhz, unsigned int v,
                 uint32_t *now, unsigned char *seq, int max)
{
    int n = 0;

    for (;;)
    {
        struct pgov_op op;

        pgov_hw_next(hw, mhz, v, *now, &op);
        if (op.op == PGOV_OP_NONE)
            return n;
        if (n >= max)
            return -1;
        seq[n++] = op.op;
        switch (op.op)
        {
        case PGOV_OP_VUP:
            if (op.arg != PGOV_V_NORM)
                return -1;
            *now += 400;                /* an I2C write and read-back */
            hw->vcode = PGOV_V_NORM;
            hw->v_at_us = *now;
            break;
        case PGOV_OP_WAIT:
            if (op.wait_us == 0 || op.wait_us > PGOV_SETTLE_US)
                return -1;
            *now += op.wait_us;
            break;
        case PGOV_OP_CLOCK:
            if (op.arg > pgov_hw_ceil(hw, *now))
                return -1;              /* invariant 1 + the settle */
            if (op.arg > PGOV_MHZ_LOW && (hw->vcode != PGOV_V_NORM
                || *now - hw->v_at_us < PGOV_SETTLE_US))
                return -1;
            if (op.arg != PGOV_MHZ_XTAL && pgov_pll_word(op.arg) == 0)
                return -1;
            hw->mhz = op.arg;
            *now += 250;                /* a relock */
            break;
        case PGOV_OP_VDOWN:
            if (hw->mhz > PGOV_MHZ_LOW || !(hw->live & PGOV_LIVE_B1)
                || op.arg != PGOV_V_LOW)
                return -1;              /* clock down before voltage */
            hw->vcode = PGOV_V_LOW;
            *now += 400;
            break;
        default:
            return -1;
        }
    }
}

static void test_hw_order(void)
{
    static const uint8_t pts[] = { 24, 30, 48, 60, 80, 100 };
    struct pgov_hw hw;
    unsigned char seq[16];
    uint32_t now = 100000;
    uint32_t r = 777;
    int n, bad = 0;

    /* low at 30 -> 80: the voltage, the settle, then the clock */
    memset(&hw, 0, sizeof(hw));
    hw.live = PGOV_LIVE_B1;
    hw.mhz = 30;
    hw.vcode = PGOV_V_LOW;
    n = drive(&hw, 80, PGOV_V_NORM, &now, seq, 16);
    CHECK(n == 3 && seq[0] == PGOV_OP_VUP && seq[1] == PGOV_OP_WAIT
          && seq[2] == PGOV_OP_CLOCK && hw.mhz == 80);

    /* 80 -> band 0 low: the clock first, then the voltage */
    n = drive(&hw, 24, PGOV_V_LOW, &now, seq, 16);
    CHECK(n == 2 && seq[0] == PGOV_OP_CLOCK && seq[1] == PGOV_OP_VDOWN
          && hw.mhz == 24 && hw.vcode == PGOV_V_LOW);

    /* low at 24 -> 80: raise, band 1's top while it settles, then 80 */
    n = drive(&hw, 80, PGOV_V_NORM, &now, seq, 16);
    CHECK(n == 4 && seq[0] == PGOV_OP_VUP && seq[1] == PGOV_OP_CLOCK
          && seq[2] == PGOV_OP_WAIT && seq[3] == PGOV_OP_CLOCK
          && hw.mhz == 80);

    /* a low code asked with a clock above band 1: the clock is cut to
     * band 1's top first (invariant 1 whatever the caller asks) */
    n = drive(&hw, 80, PGOV_V_LOW, &now, seq, 16);
    CHECK(n == 2 && seq[0] == PGOV_OP_CLOCK && hw.mhz == 30
          && hw.vcode == PGOV_V_LOW);

    /* no band 1 on the unit: never lowered */
    hw.live = 0;
    hw.vcode = PGOV_V_NORM;
    hw.v_at_us = 0;
    n = drive(&hw, 24, PGOV_V_LOW, &now, seq, 16);
    CHECK(n == 1 && seq[0] == PGOV_OP_CLOCK && hw.vcode == PGOV_V_NORM);

    /* 100 MHz only with band 3 live */
    n = drive(&hw, 100, PGOV_V_NORM, &now, seq, 16);
    CHECK(n == 1 && hw.mhz == 80);
    hw.live = PGOV_LIVE_B3;
    n = drive(&hw, 100, PGOV_V_NORM, &now, seq, 16);
    CHECK(n == 1 && hw.mhz == 100);

    /* the ceiling */
    hw.vcode = PGOV_V_LOW;
    CHECK(pgov_hw_ceil(&hw, now) == 30);
    hw.vcode = PGOV_V_NORM;
    hw.v_at_us = now;
    CHECK(pgov_hw_ceil(&hw, now + PGOV_SETTLE_US - 1) == 30);
    CHECK(pgov_hw_ceil(&hw, now + PGOV_SETTLE_US) == 100);
    hw.v_at_us = 0xfffff000u;           /* the 32-bit counter wraps */
    CHECK(pgov_hw_ceil(&hw, 0x00000100u) == 30);
    CHECK(pgov_hw_ceil(&hw, 0xfffff000u + PGOV_SETTLE_US) == 100);

    /* random targets: no violation, and the target is reached */
    for (long i = 0; i < 200000; i++)
    {
        unsigned int m = pts[rnd(&r) % 6];
        unsigned int v = (rnd(&r) & 1) ? PGOV_V_LOW : PGOV_V_NORM;
        unsigned int want_m, want_v;

        if (i % 5000 == 0)
        {
            memset(&hw, 0, sizeof(hw));
            hw.live = rnd(&r) & (PGOV_LIVE_B1 | PGOV_LIVE_B3);
            hw.mhz = 24;
            hw.vcode = PGOV_V_NORM;
        }
        now += rnd(&r) % 30000;
        n = drive(&hw, m, v, &now, seq, 16);
        want_v = (v == PGOV_V_LOW && (hw.live & PGOV_LIVE_B1))
                 ? PGOV_V_LOW : PGOV_V_NORM;
        want_m = m > pgov_vceil_mhz(hw.live, v) ? pgov_vceil_mhz(hw.live, v)
                                                : m;
        if ((n < 0 || hw.mhz != want_m || hw.vcode != want_v) && bad++ < 3)
            fprintf(stderr, "hw: n=%d %u/%x -> %u/%x want %u/%x\n", n,
                    hw.mhz, hw.vcode, m, v, want_m, want_v);
    }
    CHECK(bad == 0);
}

/* The stage-2 policy driving the apply layer, sample by sample, with
 * boosts and the watchdog's revert: the ordering holds on every step, and
 * after the revert the rail stays normal under the stage-1 decision */
static void test_apply_loop(void)
{
    struct pgov_params p;
    struct pgov_core c1, c2;
    struct pgov_hw hw;
    unsigned char seq[16];
    uint32_t now = 0, r = 4242;
    unsigned long low_ms = 0, b3_ms = 0;
    int bad = 0, reverted = 0;

    pgov_params_default(&p);
    p.vdwell_ms = 100;
    pgov_core_init(&c1, &p, 0, 24);
    pgov_core_init(&c2, &p, PGOV_LIVE_B1 | PGOV_LIVE_B3, 24);
    pgov_core_arm(&c2, true);
    memset(&hw, 0, sizeof(hw));
    hw.live = PGOV_LIVE_B1 | PGOV_LIVE_B3;
    hw.mhz = 24;
    hw.vcode = PGOV_V_NORM;
    for (long i = 0; i < 300000; i++)
    {
        struct pgov_in in;
        struct pgov_core *a = reverted ? &c1 : &c2;
        unsigned int b = rnd(&r) % 1001;

        memset(&in, 0, sizeof(in));
        /* stretches of light load (band 0, low) and of saturation */
        in.busy_pm[0] = (i / 300) % 3 == 0 ? (b > 50 ? 1000 : b) : b / 8;
        in.cur_mhz = hw.mhz;
        in.dt_ms = 10;
        in.cap_mhz = (rnd(&r) % 64 == 0) ? 80 : 0;
        in.boost = (i / 50) % 7 == 0;
        pgov_core_step(&c1, &in);
        pgov_core_step(&c2, &in);
        if (rnd(&r) % 97 == 0)
            pgov_core_boost(a);
        now += 10000;
        if (drive(&hw, a->mhz, a->vcode, &now, seq, 16) < 0 && bad++ < 3)
            fprintf(stderr, "apply: i=%ld %u/%x\n", i, a->mhz, a->vcode);
        if (hw.vcode == PGOV_V_LOW)
            low_ms += 10;
        if (hw.mhz == 100)
            b3_ms += 10;
        if (!reverted && i == 200000)
        {
            /* the watchdog fired: the rail raised by the raw path, the
             * stage-2 policy falls back, stage 1 is applied from here */
            hw.vcode = PGOV_V_NORM;
            hw.v_at_us = now;
            pgov_core_fallback(&c2);
            reverted = 1;
        }
        if (reverted && hw.vcode != PGOV_V_NORM && bad++ < 3)
            fprintf(stderr, "low after revert i=%ld\n", i);
        if (reverted && hw.mhz > 80 && bad++ < 3)
            fprintf(stderr, "band 3 after revert i=%ld\n", i);
    }
    CHECK(bad == 0);
    if (!(low_ms > 0 && b3_ms > 0))
        fprintf(stderr, "low_ms=%lu b3_ms=%lu\n", low_ms, b3_ms);
    CHECK(low_ms > 0 && b3_ms > 0);     /* both stage-2 states were used */
    CHECK(c2.vcode == PGOV_V_NORM && !(c2.live & PGOV_LIVE_B1));
}

static void test_dog(void)
{
    struct pgov_dog d;

    memset(&d, 0, sizeof(d));
    CHECK(pgov_dog_step(&d, false, 0) == PGOV_DOG_OK);
    CHECK(pgov_dog_step(&d, true, 1000) == PGOV_DOG_OK);
    CHECK(pgov_dog_step(&d, true, 1249) == PGOV_DOG_OK);
    CHECK(pgov_dog_step(&d, true, 1250) == PGOV_DOG_FIRE);
    CHECK(pgov_dog_step(&d, true, 1260) == PGOV_DOG_OK);    /* once */
    CHECK(pgov_dog_step(&d, true, 9999) == PGOV_DOG_OK);
    /* confirmed in time: never fires; a new owed raise starts anew */
    memset(&d, 0, sizeof(d));
    CHECK(pgov_dog_step(&d, true, 0) == PGOV_DOG_OK);
    CHECK(pgov_dog_step(&d, true, 200) == PGOV_DOG_OK);
    CHECK(pgov_dog_step(&d, false, 210) == PGOV_DOG_OK);
    CHECK(pgov_dog_step(&d, true, 400) == PGOV_DOG_OK);
    CHECK(pgov_dog_step(&d, true, 640) == PGOV_DOG_OK);
    CHECK(!d.fired);
    CHECK(pgov_dog_step(&d, true, 650) == PGOV_DOG_FIRE);
    /* the tick's ms counter wraps */
    memset(&d, 0, sizeof(d));
    pgov_dog_step(&d, true, 0xffffff80u);
    CHECK(pgov_dog_step(&d, true, 0x10u) == PGOV_DOG_OK);
    CHECK(pgov_dog_step(&d, true, 0x7au) == PGOV_DOG_FIRE);
}

/* Review B1: a boost on the COP reads the rail's state and never writes
 * the policy.  Model: the CPU applies stage 2 sample by sample (as in
 * test_apply_loop); at random instants the COP boosts.  Reading the live
 * (uncached) state, its point is always within what the rail carries;
 * reading a snapshot a few samples old (a stale cache line) it is not:
 * the negative control shows the model would catch the bug */
static void test_cop_reader(void)
{
    struct pgov_params p;
    struct pgov_core c2;
    struct pgov_hw hw, stale;
    unsigned char seq[16];
    uint32_t now = 0, r = 99;
    int bad_live = 0, bad_stale = 0;

    pgov_params_default(&p);
    p.vdwell_ms = 100;
    pgov_core_init(&c2, &p, PGOV_LIVE_B1, 24);
    pgov_core_arm(&c2, true);
    memset(&hw, 0, sizeof(hw));
    hw.live = PGOV_LIVE_B1;
    hw.mhz = 24;
    hw.vcode = PGOV_V_NORM;
    stale = hw;
    for (long i = 0; i < 200000; i++)
    {
        struct pgov_in in;
        unsigned int b = rnd(&r) % 1001;

        memset(&in, 0, sizeof(in));
        in.busy_pm[0] = (i / 300) % 3 == 0 ? 1000 : b / 8;
        in.cur_mhz = hw.mhz;
        in.dt_ms = 10;
        pgov_core_step(&c2, &in);
        now += 10000;
        drive(&hw, c2.mhz, c2.vcode, &now, seq, 16);
        if (i % 7 == 0)
            stale = hw;             /* the COP's cached line, refreshed
                                       now and then */
        if (rnd(&r) % 5 == 0)
        {
            unsigned int ceil = pgov_hw_ceil(&hw, now);
            if (pgov_cop_boost_mhz(&hw, now) > ceil)
                bad_live++;
            if (pgov_cop_boost_mhz(&stale, now) > ceil)
                bad_stale++;
        }
    }
    CHECK(bad_live == 0);
    CHECK(bad_stale > 0);           /* the control: staleness is caught */
}

/* Review B2a: VDOWN's re-check under the I2C lock.  Each event that may
 * land between the power thread's plan and its PMU write must make the
 * re-check refuse; with none, it proceeds */
static void test_vdown_interleave(void)
{
    struct pgov_hw hw;

    memset(&hw, 0, sizeof(hw));
    hw.live = PGOV_LIVE_B1;
    hw.mhz = 24;
    hw.vcode = PGOV_V_LOW;          /* recorded by cpu_pgov_lower() */
    CHECK(pgov_vdown_recheck(PGOV2_APPLY, &hw, PGOV_V_LOW, 0, 24));
    /* the watchdog fired and the policy reverted: mode is shadow and the
     * decision is normal */
    CHECK(!pgov_vdown_recheck(PGOV2_SHADOW, &hw, PGOV_V_NORM, 0, 24));
    CHECK(!pgov_vdown_recheck(PGOV2_SHADOW, &hw, PGOV_V_LOW, 0, 24));
    CHECK(!pgov_vdown_recheck(PGOV2_APPLY, &hw, PGOV_V_NORM, 0, 24));
    /* a raise confirmed meanwhile (the rail recorded normal) */
    hw.vcode = PGOV_V_NORM;
    CHECK(!pgov_vdown_recheck(PGOV2_APPLY, &hw, PGOV_V_LOW, 0, 24));
    hw.vcode = PGOV_V_LOW;
    /* a storage command began */
    CHECK(!pgov_vdown_recheck(PGOV2_APPLY, &hw, PGOV_V_LOW, 1, 24));
    /* the clock went above band 1 (a boost) */
    CHECK(!pgov_vdown_recheck(PGOV2_APPLY, &hw, PGOV_V_LOW, 0, 80));
    CHECK(pgov_vdown_recheck(PGOV2_APPLY, &hw, PGOV_V_LOW, 0, 30));
    /* band 1 gone (a fallback cleared it) */
    hw.live = 0;
    CHECK(!pgov_vdown_recheck(PGOV2_APPLY, &hw, PGOV_V_LOW, 0, 24));
}

/* Review S1: every reboot leaves DCDC1 at 0xEC */
static void test_reboot_raise(void)
{
    CHECK(pgov_reboot_action(false, PGOV_V_LOW, true) == PGOV_RB_NONE);
    CHECK(pgov_reboot_action(true, PGOV_V_NORM, true) == PGOV_RB_NONE);
    CHECK(pgov_reboot_action(true, PGOV_V_LOW, true) == PGOV_RB_LOCKED);
    CHECK(pgov_reboot_action(true, PGOV_V_LOW, false) == PGOV_RB_RAW);
    /* any code other than normal is raised: a lowering recorded but not
     * yet written still counts as low */
    CHECK(pgov_reboot_action(true, 0x00, false) == PGOV_RB_RAW);
}

static void test_stage2_misc(void)
{
    struct pgov_params p;
    struct pgov_core c;
    struct pgov_record a, b;
    uint8_t buf[PGOV_REC_FILE_SIZE];

    /* a cap from outside the tick leaves band 3 at once */
    pgov_params_default(&p);
    pgov_core_init(&c, &p, PGOV_LIVE_B3, 100);
    CHECK(pgov_core_cap(&c, 80) == 80 && c.why == PGOV_WHY_CAP);
    CHECK(pgov_core_cap(&c, 0) == 80);      /* no cap */
    CHECK(pgov_core_cap(&c, 100) == 80);    /* never raises */

    /* disarmed while low: the raise is decided at once */
    pgov_core_init(&c, &p, PGOV_LIVE_B1, 24);
    c.dvs_ok = 1;
    c.vcode = PGOV_V_LOW;
    c.vstate = PGOV_VS_LOW;
    pgov_core_arm(&c, false);
    CHECK(c.vcode == PGOV_V_NORM && c.vstate == PGOV_VS_RAISING);

    /* band 3's check */
    CHECK(pgov_b3_check(400, 7, 7));
    CHECK(pgov_b3_check(380, 7, 7) && pgov_b3_check(420, 7, 7));
    CHECK(!pgov_b3_check(379, 7, 7) && !pgov_b3_check(421, 7, 7));
    CHECK(!pgov_b3_check(501, 7, 7));       /* still 80 MHz */
    CHECK(!pgov_b3_check(400, 7, 8));

    /* the record's file */
    memset(&a, 0, sizeof(a));
    a.dvs = PGOV_REC_TRIAL; a.b3 = PGOV_REC_OK; a.code = PGOV_FB_DOG;
    a.hist = 0x15; a.armed = 1; a.skip = 0; a.ok_boots = 7;
    pgov_rec_pack(&a, buf);
    CHECK(pgov_rec_unpack(buf, sizeof(buf), &b));
    CHECK(!memcmp(&a, &b, sizeof(a)));
    buf[6] ^= 1;
    CHECK(!pgov_rec_unpack(buf, sizeof(buf), &b));
    CHECK(b.dvs == PGOV_REC_UNTRIED && !b.armed);
    pgov_rec_pack(&a, buf);
    CHECK(!pgov_rec_unpack(buf, sizeof(buf) - 1, &b));
    CHECK(!pgov_rec_unpack(buf, 0, &b));

    /* the record across boots: armed then a crash is a strike (skip one
     * boot); a second in five turns DVS off; a planned end clears it */
    pgov_rec_reset(&a);
    CHECK(pgov_rec_boot(&a, false));
    pgov_rec_armed(&a);
    CHECK(a.armed && a.dvs == PGOV_REC_TRIAL);
    pgov_rec_pack(&a, buf);                 /* written before any low */
    pgov_rec_unpack(buf, sizeof(buf), &b);
    CHECK(!pgov_rec_boot(&b, false));       /* crashed: skip this boot */
    CHECK(pgov_rec_boot(&b, false));        /* the next may arm */
    pgov_rec_armed(&b);
    pgov_rec_clean_end(&b, 10);
    CHECK(!b.armed && pgov_rec_boot(&b, false));
    pgov_rec_armed(&b);
    CHECK(!pgov_rec_boot(&b, false) && b.dvs == PGOV_REC_OFF);
}

/* Review R3: the record at boot from pgov.dat and pgov.tmp */
static void test_rec_pick(void)
{
    struct pgov_record a, b, r;
    uint8_t ma[PGOV_REC_FILE_SIZE], tb[PGOV_REC_FILE_SIZE];

    pgov_rec_reset(&a);
    a.dvs = PGOV_REC_TRIAL;
    b = a;
    b.armed = 1;                    /* the newer write: armed */
    pgov_rec_pack(&a, ma);
    pgov_rec_pack(&b, tb);
    /* a cut between remove and rename: only the temporary */
    CHECK(pgov_rec_pick(ma, 0, tb, sizeof(tb), &r) == 2 && r.armed);
    /* a cut between sync and remove: both; the temporary is newer */
    CHECK(pgov_rec_pick(ma, sizeof(ma), tb, sizeof(tb), &r) == 2 && r.armed);
    /* a write cut before its sync: a broken temporary, the record holds */
    tb[3] ^= 0x40;
    CHECK(pgov_rec_pick(ma, sizeof(ma), tb, sizeof(tb), &r) == 1 && !r.armed
          && r.dvs == PGOV_REC_TRIAL);
    CHECK(pgov_rec_pick(ma, sizeof(ma), tb, 0, &r) == 1);
    /* neither: a fresh record */
    CHECK(pgov_rec_pick(ma, 0, tb, 0, &r) == 0 && r.dvs == PGOV_REC_UNTRIED);
}

/* Review R4: the apply pass sleeps out a settle instead of spinning */
static void test_settle_left(void)
{
    struct pgov_hw hw;

    memset(&hw, 0, sizeof(hw));
    hw.vcode = PGOV_V_NORM;
    hw.v_at_us = 1000;
    CHECK(pgov_hw_settle_left(&hw, 1000) == PGOV_SETTLE_US);
    CHECK(pgov_hw_settle_left(&hw, 1000 + 4000) == PGOV_SETTLE_US - 4000);
    CHECK(pgov_hw_settle_left(&hw, 1000 + PGOV_SETTLE_US) == 0);
    hw.v_at_us = 0xffffff00u;       /* wraps */
    CHECK(pgov_hw_settle_left(&hw, 0x100u) == PGOV_SETTLE_US - 0x200u);
    hw.vcode = PGOV_V_LOW;          /* not raised: nothing to wait for */
    CHECK(pgov_hw_settle_left(&hw, 0x100u) == 0);
    /* the wait and the ceiling agree: settled exactly when 100 is allowed */
    hw.vcode = PGOV_V_NORM;
    hw.live = PGOV_LIVE_B3;
    hw.v_at_us = 0;
    for (uint32_t t = 0; t < 2 * PGOV_SETTLE_US; t += 97)
        CHECK((pgov_hw_settle_left(&hw, t) == 0) == (pgov_hw_ceil(&hw, t) == 100));
}

int main(void)
{
    test_selector();
    test_band1_band3();
    test_proportional();
    test_voltage();
    test_random();
    test_record();
    test_tables();
    test_busy();
    test_hw_order();
    test_apply_loop();
    test_dog();
    test_cop_reader();
    test_vdown_interleave();
    test_reboot_raise();
    test_rec_pick();
    test_settle_left();
    test_stage2_misc();
    if (fails)
    {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    return 0;
}
