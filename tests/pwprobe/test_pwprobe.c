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
 * Host test of the power probe rows' pure logic (apps/ipodui/devbcm_pw.h):
 * the deadline, the requests, the reply checks, PW3's lowest level, PW4's
 * speed reading and pass, the lock, and the ladder against Apple's rule
 * and table (brief-map-power.md sections 4 and 5).
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
#include "devbcm_pw.h"

#define BVP_E_TIMEOUT_FOR_TEST (-2)  /* bcmvp.h BVP_E_TIMEOUT */

static int fails;

#define CHECK(c) do { if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* A clean reply: held, n runs, all right, all back */
static void good(struct pwp_reply *r, uint32_t runs)
{
    memset(r, 0, sizeof(*r));
    r->status = HP_OK;
    r->n = HP_PW_REPLY_WORDS;
    r->w[HP_PW_R_EXIT] = HP_PW_X_OK;
    r->w[HP_PW_R_RUNS] = runs;
    r->w[HP_PW_R_SUM0] = 0x12345678;
    r->w[HP_PW_R_US0] = 10000;
    r->w[HP_PW_R_US] = 10000;
}

static void t_deadline(void)
{
    /* the hold, then the hang rule and the settles on top */
    CHECK(pwp_deadline_us(0) == TVP_HANG_US + 4 * HP_PW_SETTLE_US +
          HP_PW_PLL_US);
    CHECK(pwp_deadline_us(PWP_PW3_HOLD_MS) > PWP_PW3_HOLD_MS * 1000u +
          TVP_HANG_US);
    CHECK(PWP_PW3_HOLD_MS <= HP_PW_HOLD_MS && PWP_PW4_HOLD_MS <= HP_PW_HOLD_MS);
    /* longer than the probes' usual 2 s for every hold we send */
    CHECK(pwp_deadline_us(PWP_PW4_HOLD_MS) > 2000000u + PWP_PW4_HOLD_MS * 1000u);
    CHECK(pwp_prog_phase(HP_PW_PROG(HP_PW_PH_CHECK, 1234)) == HP_PW_PH_CHECK);
    CHECK(pwp_prog_ms(HP_PW_PROG(HP_PW_PH_CHECK, 1234)) == 1234);
    /* the TV's progress word is not ours */
    CHECK(pwp_prog_phase(HP_TV_PROG(HP_TV_P_HOLD, 5)) == 0);
    CHECK(HP_PW_PROGRESS == HP_TV_PROGRESS);
}

static void t_requests(void)
{
    uint32_t a[5];
    int k;

    CHECK(pwp_request(a, false, 0xC0DE, 5, PWP_PW3_HOLD_MS) == 5);
    CHECK(a[0] == HP_PW_MAGIC && a[1] == HP_PW_LEVEL && a[2] == 0xC0DE &&
          a[3] == 5 && a[4] == PWP_PW3_HOLD_MS);
    CHECK(pwp_request(a, true, 1, HP_PW_P_150, 0) == 5 && a[1] == HP_PW_POINT &&
          a[3] == HP_PW_P_150);
    CHECK(pwp_request(a, false, 1, 4, 0) == 0);
    CHECK(pwp_request(a, false, 1, 16, 0) == 0);
    CHECK(pwp_request(a, true, 1, HP_PW_POINTS, 0) == 0);
    CHECK(pwp_request(a, false, 1, 5, HP_PW_HOLD_MS + 1) == 0);
    CHECK(pwp_request(a, false, 1, 15, HP_PW_HOLD_MS) == 5);
    /* every step the rows send is in bounds */
    for (k = 0; k < PWP_PW3_STEPS; k++)
        CHECK(pwp_request(a, false, 1, pwp_pw3_level(k), PWP_PW3_HOLD_MS) == 5);
    for (k = 0; k < PWP_PW4_STEPS; k++)
        CHECK(pwp_request(a, true, 1, k, PWP_PW4_HOLD_MS) == 5);
    CHECK(pwp_pw3_level(0) == 11 && pwp_pw3_level(3) == 5 &&
          pwp_pw3_level(4) == 0 && pwp_pw3_level(-1) == 0);
}

/* The ladder against Apple's rule (@4cb8) and table (@141ba) */
static uint32_t apple_level(uint32_t hz)
{
    static const uint32_t up[] = { 46875000, 59665292, 74840572, 87268380,
                                   102016607, 116539140, 123607364,
                                   130841121, 140183656, 147075854,
                                   154130133, 160919540 };
    static const uint8_t lv[] = { 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 15 };
    unsigned i;

    for (i = 0; i < sizeof(up) / sizeof(up[0]); i++)
        if (up[i] >= hz)
            return lv[i];
    return 15;
}

static void t_ladder(void)
{
    int k;

    for (k = 0; k < HP_PW_POINTS; k++)
        CHECK(pwp_level(k) == apple_level(pwp_mhz(k) * 1000000u));
    CHECK(pwp_level(HP_PW_P_46) == 5 && pwp_level(HP_PW_P_150) == 15);
    CHECK(pwp_mhz(HP_PW_P_102) == 102 && pwp_mhz(HP_PW_POINTS) == 0);
    /* the ladder goes up: each point's level at least the one before */
    for (k = 1; k < HP_PW_POINTS; k++)
        CHECK(pwp_level(k) >= pwp_level(k - 1) && pwp_mhz(k) > pwp_mhz(k - 1));
    /* 27 MHz is Apple's level 5 too: PW3's floor */
    CHECK(apple_level(27000000) == HP_PW_LEVEL_MIN);
}

static void t_checks(void)
{
    struct pwp_reply r;

    good(&r, 7);
    CHECK(pwp_check(&r) == PWP_OK);
    r.status = BVP_E_TIMEOUT_FOR_TEST;
    CHECK(pwp_check(&r) == PWP_NOREPLY);
    good(&r, 1);
    r.status = HP_E_BUSY;
    CHECK(pwp_check(&r) == PWP_REFUSED);
    /* the state refusal carries two words, not a run */
    good(&r, 1);
    r.status = HP_E_STATE;
    r.n = 2;
    CHECK(pwp_check(&r) == PWP_REFUSED);
    good(&r, 1);
    r.n = 18;
    CHECK(pwp_check(&r) == PWP_SHORT);
    /* the restore first, whatever else went wrong */
    good(&r, 1);
    r.status = HP_E_STATE;
    r.w[HP_PW_R_EXIT] = HP_PW_X_CHECK;
    r.w[HP_PW_R_MISMATCH] = HP_PW_M_REG;
    CHECK(pwp_check(&r) == PWP_RESTORE);
    r.w[HP_PW_R_MISMATCH] = 0;
    r.w[HP_PW_R_FLAGS] = 1 << 8;
    CHECK(pwp_check(&r) == PWP_DENIED && pwp_denied(&r) == 1);
    r.w[HP_PW_R_FLAGS] = HP_PW_F_SDRAM;
    CHECK(pwp_check(&r) == PWP_WRONG);
    r.w[HP_PW_R_EXIT] = HP_PW_X_REFBAD;
    CHECK(pwp_check(&r) == PWP_REFBAD);
    r.w[HP_PW_R_EXIT] = HP_PW_X_READBACK;
    CHECK(pwp_check(&r) == PWP_READBACK);
    r.w[HP_PW_R_EXIT] = HP_PW_X_STALL;
    CHECK(pwp_check(&r) == PWP_STALL);
    /* OK on its face, but no run, or a wrong run, or no reference */
    good(&r, 0);
    CHECK(pwp_check(&r) == PWP_WRONG);
    good(&r, 3 | 1 << 16);
    CHECK(pwp_check(&r) == PWP_WRONG && pwp_runs(&r) == 3 && pwp_wrong(&r) == 1);
    good(&r, 3);
    r.w[HP_PW_R_SUM0] = 0;
    CHECK(pwp_check(&r) == PWP_WRONG);
    CHECK(strcmp(pwp_check_name(PWP_STALL), "counter stopped") == 0);
    CHECK(strcmp(pwp_check_name((enum pwp_check)99), "?") == 0);
}

static void t_pw3(void)
{
    struct pwp_reply r[PWP_PW3_STEPS];
    int k;

    for (k = 0; k < PWP_PW3_STEPS; k++)
        good(&r[k], 2000);
    CHECK(pwp_pw3_lowest(r, 4) == 5 && pwp_pw3_pass(r, 4));
    /* level 7 wrong: 9 is the lowest good, no pass */
    r[2].w[HP_PW_R_EXIT] = HP_PW_X_CHECK;
    r[2].status = HP_E_STATE;
    CHECK(pwp_pw3_lowest(r, 4) == 9 && !pwp_pw3_pass(r, 4));
    /* the run stopped after 7: only three replies */
    CHECK(pwp_pw3_lowest(r, 3) == 9);
    /* 11 already failed: none */
    r[0].status = BVP_E_TIMEOUT_FOR_TEST;
    CHECK(pwp_pw3_lowest(r, 4) == 0 && !pwp_pw3_pass(r, 4));
    /* 5 ok but an earlier one not: the steps stop at the first failure,
     * so a late ok never counts */
    for (k = 0; k < PWP_PW3_STEPS; k++)
        good(&r[k], 2000);
    r[1].w[HP_PW_R_MISMATCH] = HP_PW_M_REG;
    CHECK(pwp_pw3_lowest(r, 4) == 11 && !pwp_pw3_pass(r, 4));
    CHECK(!pwp_pw3_pass(r, 0));
}

static void t_pw4(void)
{
    struct pwp_reply r[PWP_PW4_STEPS];
    int k;

    for (k = 0; k < PWP_PW4_STEPS; k++)
    {
        good(&r[k], 900);
        /* the start's run at 27 MHz, the point's at f exactly */
        r[k].w[HP_PW_R_US0] = 27 * 1000;
        r[k].w[HP_PW_R_US] = 27 * 1000 * 27 / pwp_mhz(k);
    }
    CHECK(pwp_pw4_pass(r, PWP_PW4_STEPS));
    CHECK(!pwp_pw4_pass(r, PWP_PW4_STEPS - 1));
    for (k = 0; k < PWP_PW4_STEPS; k++)
        CHECK(pwp_speed_class(&r[k], k) == PWP_S_CLOCK);
    /* 74 MHz: x2.740 wanted */
    CHECK(pwp_want_milli(HP_PW_P_74) == 2740);
    r[1].w[HP_PW_R_US] = 27000 * 1000 / 2700;      /* x2.700: 1.5 % slow */
    CHECK(pwp_speed_class(&r[1], 1) == PWP_S_SLOWER);
    r[1].w[HP_PW_R_US] = 27000 * 1000 / 2730;      /* x2.730: within 1 % */
    CHECK(pwp_speed_class(&r[1], 1) == PWP_S_CLOCK);
    r[1].w[HP_PW_R_US] = 27000 * 1000 / 2800;      /* x2.800: fast */
    CHECK(pwp_speed_class(&r[1], 1) == PWP_S_FASTER);
    r[1].w[HP_PW_R_US] = 0;
    CHECK(pwp_speed_class(&r[1], 1) == PWP_S_NONE && pwp_speed_milli(&r[1]) == 0);
    CHECK(pwp_speed_class(&r[0], HP_PW_POINTS) == PWP_S_NONE);
    /* the speed is a reading: a slow point still passes */
    r[1].w[HP_PW_R_US] = 27000;
    CHECK(pwp_pw4_pass(r, PWP_PW4_STEPS));
    r[3].w[HP_PW_R_EXIT] = HP_PW_X_READBACK;
    r[3].status = HP_E_STATE;
    CHECK(!pwp_pw4_pass(r, PWP_PW4_STEPS));
}

static void t_lock(void)
{
    struct pwp_lock l;

    pwp_lock_init(&l);
    CHECK(!pwp_pw4_open(&l));
    pwp_note_pw3(&l, false);
    CHECK(!pwp_pw4_open(&l));
    pwp_note_pw3(&l, true);
    CHECK(pwp_pw4_open(&l));
    pwp_note_pw3(&l, false);        /* a later failure doesn't relock */
    CHECK(pwp_pw4_open(&l));
}

int main(void)
{
    t_deadline();
    t_requests();
    t_ladder();
    t_checks();
    t_pw3();
    t_pw4();
    t_lock();
    if (fails)
        printf("%d failed\n", fails);
    return fails != 0;
}
