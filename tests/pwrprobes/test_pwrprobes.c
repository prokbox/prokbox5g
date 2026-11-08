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

/* Host test of apps/ipodui/devbench_pwr.h: the battery probes' order and
 * needs as a picked run (the runner's own bench_dep_skip), the row table
 * as devbench.c splices it in, the decodes, the verdicts and the restore
 * bookkeeping. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define HAVE_IPOD_UI_DEVMODE
#include "devbench.h"
#include "devbench_pwr.h"

static int fails;

#define CHECK(c) do { if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int ran(void) { return BENCH_PASS; }
#define devbench_pwrp_grade ran
#define devbench_pwrp_regs  ran
#define devbench_pwrp_rtc   ran
#define devbench_pwrp_stall ran
#define devbench_pwrp_ldo   ran
#define devbench_pwrp_den   ran
#define devbench_pwrp_srf   ran

/* devbench.c's table, an earlier row first, then the group as spliced */
static const struct bench_step T[] =
{
    { "s05", "Stages", NULL, NULL, ran, NULL, { NULL }, BENCH_F_AUTO, 120, 0,
      0 },
#define DEVBENCH_PWR_ROWS
#include "devbench_pwr.h"
};
#define NT ((int)(sizeof(T) / sizeof(T[0])))

static void t_table(void)
{
    static const char *const order[] =
        { "pgrd", "preg", "prtc", "pstl", "pldo", "pden", "psrf" };
    struct bench_run run;
    int p, k;

    CHECK(NT == 1 + BENCH_PWR_NPICKS);
    /* every row picked-only: never in the full session */
    for (k = 1; k < NT; k++)
    {
        CHECK(T[k].flags & BENCH_F_PICKED);
        CHECK(!(T[k].flags & (BENCH_F_REBOOTS | BENCH_F_LEAVES)));
        CHECK(T[k].ask == NULL);    /* the questions are the rows' own */
        CHECK(strlen(T[k].id) < BENCH_TOKEN_MAX);
    }
    CHECK(bench_select(BENCH_MODE_FULL, T, NT, &run) == 1);
    /* the picks: the table's ids, in the order basic to advanced */
    CHECK(bench_pwr_select(T, NT, &run) == BENCH_PWR_NPICKS);
    for (p = 0; p < run.n; p++)
    {
        CHECK(!strcmp(T[run.k[p]].id, order[p]));
        CHECK(run.role[p] == BENCH_R_ALSO);
    }
    /* the rows that may freeze carry the heads-up */
    for (k = 1; k < NT; k++)
        if (!strcmp(T[k].id, "pstl") || !strcmp(T[k].id, "pden") ||
            !strcmp(T[k].id, "psrf"))
            CHECK(T[k].say && strstr(T[k].say, "hold MENU+SELECT to "
                                     "restart"));
    /* a table without the group: nothing picked */
    CHECK(bench_pwr_select(T, 1, &run) == 0);
}

static void t_steer(void)
{
    struct bench_run run;
    struct bench_state st;
    int p, kpreg = bench_find(T, NT, "preg");

    bench_pwr_select(T, NT, &run);
    bench_state_init(&st);
    /* all run when nothing failed */
    for (p = 0; p < run.n; p++)
        CHECK(bench_dep_skip(&run, T, &st, p) < 0);
    /* a failed pstl, pldo or prtc strands nothing */
    st.res[bench_find(T, NT, "pstl")] = BENCH_FAIL;
    st.res[bench_find(T, NT, "pldo")] = BENCH_FAIL;
    st.res[bench_find(T, NT, "prtc")] = BENCH_HANG;
    for (p = 0; p < run.n; p++)
        CHECK(bench_dep_skip(&run, T, &st, p) < 0);
    /* preg hung (a register read froze): pden and psrf skip by themselves */
    st.res[kpreg] = BENCH_HANG;
    for (p = 0; p < run.n; p++)
    {
        const char *id = T[run.k[p]].id;
        int d = bench_dep_skip(&run, T, &st, p);

        if (!strcmp(id, "pden") || !strcmp(id, "psrf"))
            CHECK(d == kpreg);
        else
            CHECK(d < 0);
    }
    /* preg done (its normal result) gates nothing */
    st.res[kpreg] = BENCH_DONE;
    for (p = 0; p < run.n; p++)
        CHECK(bench_dep_skip(&run, T, &st, p) < 0);
}

static void t_decode(void)
{
    int32_t ppm;

    /* the speed grade: Apple's table */
    CHECK(pwrp_grade(0x00000000) == 0 && pwrp_grade_mhz(0) == 133);
    CHECK(pwrp_grade(0x00000020) == 1 && pwrp_grade_mhz(0x20) == 100);
    CHECK(pwrp_grade(0x00000040) == 2 && pwrp_grade_mhz(0x40) == 80);
    CHECK(pwrp_grade(0xFFFFFF7F) == 3 && pwrp_grade_mhz(0x60) == 66);
    /* 0x6000_5014: days, hours, minutes, seconds in 6-bit fields */
    CHECK(pwrp_5014_secs(0x01020304) == 86400u + 2 * 3600u + 3 * 60u + 4u);
    CHECK(pwrp_5014_secs(0x0000003B) + 1 == pwrp_5014_secs(0x00000100));
    /* its verdict */
    CHECK(pwrp_rtc_verdict(10, 10000000, 10, &ppm) == BENCH_PASS && ppm == 0);
    CHECK(pwrp_rtc_verdict(10, 10010000, 11, &ppm) == BENCH_PASS);
    CHECK(pwrp_rtc_verdict(0, 10000000, 10, &ppm) == BENCH_FAIL);
    CHECK(pwrp_rtc_verdict(10, 12000000, 12, &ppm) == BENCH_FAIL);
    CHECK(pwrp_rtc_verdict(10, 10000000, 13, &ppm) == BENCH_FAIL);
    /* the stalls: Apple's words, basic to advanced */
    CHECK(pwrp_stalls[0].word == 0x4800000F);
    CHECK(pwrp_stalls[3].word == 0x420000FA);
    CHECK(pwrp_stalls[PWRP_NSTALLS - 1].word == 0x4100000A);
    CHECK(pwrp_stall_ok(250, 252) && pwrp_stall_ok(10000, 10040));
    CHECK(!pwrp_stall_ok(250, 200) && !pwrp_stall_ok(10000, 12000));
    CHECK(!pwrp_stall_ok(1000, 0));
    /* the LDOs: our start-up values, Apple's sleep form */
    CHECK(pwrp_ldo_mode(0xF1) == PWRP_LDO_ON);
    CHECK(pwrp_ldo_sleep_val(0xF1) == 0x51);    /* 2.6 V, mode 010 */
    CHECK(pwrp_ldo_sleep_val(0xF0) == 0x50);    /* 2.5 V */
    CHECK(pwrp_ldo_sleep_val(0xF5) == 0x55);    /* Apple's code 21 */
    CHECK(pwrp_ldo_cv(0xF1) == 260 && pwrp_ldo_cv(0xF5) == 300);
    CHECK(pwrp_ldo_verdict(BENCH_PASS, BENCH_PASS) == BENCH_PASS);
    CHECK(pwrp_ldo_verdict(BENCH_PASS, BENCH_SKIP) == BENCH_DONE);
    CHECK(pwrp_ldo_verdict(BENCH_PASS, BENCH_FAIL) == BENCH_FAIL);
    CHECK(pwrp_ldo_verdict(BENCH_FAIL, BENCH_SKIP) == BENCH_FAIL);
    CHECK(pwrp_ldo_verdict(BENCH_SKIP, BENCH_SKIP) == BENCH_SKIP);
    /* sitting 4gh-b5: the screen's supply already in mode 010 (the
       lcdldo token): the sound part decides */
    CHECK(pwrp_ldo_mode(0x51) == PWRP_LDO_SLEEP);
    CHECK(pwrp_ldo_verdict(BENCH_DONE, BENCH_PASS) == BENCH_PASS);
    CHECK(pwrp_ldo_verdict(BENCH_DONE, BENCH_SKIP) == BENCH_DONE);
    CHECK(pwrp_ldo_verdict(BENCH_DONE, BENCH_FAIL) == BENCH_FAIL);
}

static void t_den(void)
{
    /* the Video's start-up word (system-pp502x.c): nothing named-unused */
    CHECK(pwrp_den_candidates(0xC2000124, false, false) == 0);
    /* with the wheel, I2S, I2C, PWM and both USB bits on: none */
    CHECK(pwrp_den_candidates(0xC243192E, false, false) == 0);
    /* FireWire and the LCD bit on: both, nothing else */
    CHECK(pwrp_den_candidates(0xC2000124 | (1u << 23) | (1u << 26), false,
                              false) == ((1u << 23) | (1u << 26)));
    /* USB1 alone, no cable: tested; with a cable: kept */
    CHECK(pwrp_den_candidates(0xC2000124 | (1u << 22), false, false)
          == (1u << 22));
    CHECK(pwrp_den_candidates(0xC2000124 | (1u << 22), true, false) == 0);
    /* the UARTs: kept with an accessory set up */
    CHECK(pwrp_den_candidates(0xC20001E4, false, false) == 0xC0);
    CHECK(pwrp_den_candidates(0xC20001E4, false, true) == 0);
    /* never: SYSTEM, EXTCLOCKS, USB0, I2S, I2C, OPTO, PWM, IDE0, unnamed */
    CHECK(pwrp_den_candidates(0xFFFFFFFF, true, true) ==
          ((1u << 23) | (1u << 26) | (1u << 14)));
    /* health: 2 s off, 200 ticks, the wheel, 2 s played */
    CHECK(pwrp_den_healthy(200, 2000000, true, true, 2000));
    CHECK(!pwrp_den_healthy(200, 2000000, false, true, 2000));
    CHECK(!pwrp_den_healthy(100, 2000000, true, true, 2000));
    CHECK(!pwrp_den_healthy(200, 2000000, true, true, 500));
    CHECK(pwrp_den_healthy(200, 2000000, true, false, 0));
}

static uint32_t fake[4];
static uint32_t fake_rd(uint32_t a) { return fake[(a >> 8) & 3]; }

static void t_restore(void)
{
    struct pwrp_saved s;
    struct pwrp_srf_io io;
    int i;

    pwrp_saved_init(&s);
    fake[0] = 0xC2000124;
    fake[1] = 0xF1;
    CHECK(pwrp_save(&s, 0x0, fake[0], 1u << 23));
    CHECK(pwrp_save(&s, PWRP_PMU(1), fake[1], 0xFF));
    CHECK(pwrp_unrestored(&s, fake_rd) < 0);
    /* a driver's own bit changed meanwhile: not the row's, still restored */
    fake[0] |= 1u << 16;
    CHECK(pwrp_unrestored(&s, fake_rd) < 0);
    /* the row's bit left on where it was off; the LDO left in mode 010 */
    fake[0] |= 1u << 23;
    CHECK(pwrp_unrestored(&s, fake_rd) == 0);
    fake[0] &= ~(1u << 23);
    fake[1] = pwrp_ldo_sleep_val(0xF1);
    CHECK(pwrp_unrestored(&s, fake_rd) == 1);
    fake[1] = 0xF1;
    CHECK(pwrp_unrestored(&s, fake_rd) < 0);
    CHECK((PWRP_PMU(0x26) & 1) && (PWRP_PMU(0x26) >> 8) == 0x26);
    /* full: refused, nothing overwritten */
    for (i = s.n; i < PWRP_SAVE_MAX; i++)
        CHECK(pwrp_save(&s, 0x0, 0, 0));
    CHECK(!pwrp_save(&s, 0x0, 0, 0) && s.n == PWRP_SAVE_MAX);

    /* psrf: the IRAM block's layout is the assembly's */
    CHECK(offsetof(struct pwrp_srf_io, bound_us) == 4);
    CHECK(offsetof(struct pwrp_srf_io, ack_us) == 8);
    CHECK(offsetof(struct pwrp_srf_io, held_us) == 12);
    CHECK(offsetof(struct pwrp_srf_io, rel_us) == 16);
    CHECK(offsetof(struct pwrp_srf_io, cfg_before) == 20);
    CHECK(offsetof(struct pwrp_srf_io, cfg_in) == 24);
    CHECK(offsetof(struct pwrp_srf_io, cfg_after) == 28);
    CHECK(sizeof(struct pwrp_srf_io) == 32);
    memset(&io, 0, sizeof(io));
    io.cfg_before = io.cfg_after = 0x00000400;
    io.cfg_in = io.cfg_before | PWRP_SRF_REQ | PWRP_SRF_ACK;
    io.ack_us = 3;
    io.rel_us = 5;
    io.held_us = PWRP_SRF_HOLD_US;
    CHECK(pwrp_srf_verdict(&io, 1, 1) == BENCH_PASS);
    CHECK(pwrp_srf_verdict(&io, 1, 2) == BENCH_FAIL);
    io.ack_us = PWRP_SRF_TIMEOUT;
    CHECK(pwrp_srf_verdict(&io, 1, 1) == BENCH_FAIL);
    io.ack_us = 3;
    io.cfg_after |= PWRP_SRF_REQ;           /* the request left set */
    CHECK(pwrp_srf_verdict(&io, 1, 1) == BENCH_FAIL);
}

int main(void)
{
    t_table();
    t_steer();
    t_decode();
    t_den();
    t_restore();
    if (fails)
        printf("%d failed\n", fails);
    return fails != 0;
}
