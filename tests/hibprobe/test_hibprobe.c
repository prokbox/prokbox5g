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
 * Host test of the hibernate probe rows' pure logic (apps/ipodui/
 * devbcm_hib.h): the holds against the image's bound, the request, the
 * reply checks, what the 0x1F0 word said, the cycle's verdict and PW5's
 * pass, the counter reading, the wake against a cold start, PW6's raw
 * read classes, the locks; and the PP's constants (bcm-devtest.h
 * BCMDT_HIB_*) against the image's (hostproto.h HP_HIB_*).
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
#include "devbcm_hib.h"
#include "bcm-devtest.h"

static int fails;

#define CHECK(c) do { if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* A clean reply: woken, all back, every pattern kept */
static void good(struct pwh_reply *r)
{
    memset(r, 0, sizeof(*r));
    r->status = HP_OK;
    r->n = HP_HIB_REPLY_WORDS;
    r->w[HP_HIB_R_EXIT] = HP_HIB_X_OK;
    r->w[HP_HIB_R_SRAM_FIRST] = 0xFFFFFFFF;
    r->w[HP_HIB_R_SDRAM_FIRST] = 0xFFFFFFFF;
    r->w[HP_HIB_R_FRAME_PRE] = r->w[HP_HIB_R_FRAME_POST] = 0x1234;
    r->w[HP_HIB_R_HELD_US] = 1000000;
}

static void t_constants(void)
{
    /* the two sides of the 0x1F0 handshake are one protocol */
    CHECK(BCMDT_HIB_WORD == HP_HIB_WORD);
    CHECK(BCMDT_HIB_PARKED == HP_HIB_PARKED);
    CHECK(BCMDT_HIB_WAKE == HP_HIB_WAKE);
    CHECK(BCMDT_HIB_AWAKE == HP_HIB_AWAKE);
    CHECK(BCMDT_HIB_GAVEUP == HP_HIB_GAVEUP);
    /* Apple's wake and park words differ only in the low half, so a
     * half-written word is never mistaken for another */
    CHECK((HP_HIB_PARKED ^ HP_HIB_WAKE) >> 16 == 0);
    CHECK((HP_HIB_WAKE ^ HP_HIB_AWAKE) >> 16 == 0);
    CHECK(BCMDT_GPO_RUN == 0x4000 && BCMDT_GPO_RETAIN == 0x8000);
    CHECK(BCMDT_HIB_PIN_US == 5000);
    /* the end names follow BCMDT_HIB_* */
    CHECK(!strcmp(pwh_end_name(BCMDT_HIB_OK), "awake"));
    CHECK(!strcmp(pwh_end_name(BCMDT_HIB_REFUSED), "refused (pins)"));
    CHECK(!strcmp(pwh_end_name(BCMDT_HIB_E_PARK), "never parked"));
    CHECK(!strcmp(pwh_end_name(BCMDT_HIB_E_WAKE), "no answer to the wake"));
    CHECK(!strcmp(pwh_end_name(-7), "?") && !strcmp(pwh_end_name(1), "?"));
}

static void t_holds(void)
{
    int k;

    CHECK(pwh_hold_ms(0) == 1000 && pwh_hold_ms(1) == 10000 &&
          pwh_hold_ms(2) == 60000);
    CHECK(pwh_hold_ms(-1) == 0 && pwh_hold_ms(PWH_HOLDS) == 0);
    /* the image's own bound covers every hold, the pins, the park's and
     * the wake's bounds, with a margin of seconds */
    for (k = 0; k < PWH_HOLDS; k++)
        CHECK((uint64_t)PWH_WAIT_MS * 1000 > (uint64_t)pwh_hold_ms(k) * 1000 +
              2 * BCMDT_HIB_PIN_US + 1000u * (BCMDT_HIB_PARK_MS +
              2 * BCMDT_HIB_WAKE_MS) + TVP_HANG_US);
    CHECK(PWH_WAIT_MS <= HP_HIB_WAIT_MS);
    /* the reply's bound is at least the hang rule */
    CHECK(PWH_REPLY_US >= TVP_HANG_US);
}

static void t_request(void)
{
    uint32_t a[4];

    CHECK(pwh_request(a, 0xC0DE, PWH_WAIT_MS) == 4);
    CHECK(a[0] == HP_HIB_MAGIC && a[1] == HP_HIB_PARK && a[2] == 0xC0DE &&
          a[3] == PWH_WAIT_MS);
    CHECK(pwh_request(a, 1, 0) == 0);
    CHECK(pwh_request(a, 1, HP_HIB_WAIT_MS + 1) == 0);
    CHECK(pwh_request(a, 1, 1) == 4);
    CHECK(pwh_prog_phase(HP_HIB_PROG(HP_HIB_PH_PARKED, 77)) == HP_HIB_PH_PARKED);
    CHECK(pwh_prog_ms(HP_HIB_PROG(HP_HIB_PH_PARKED, 77)) == 77);
    CHECK(pwh_prog_phase(HP_PW_PROG(3, 5)) == 0);    /* another probe's tag */
}

static void t_word(void)
{
    CHECK(pwh_word(HP_HIB_PARKED) == PWH_W_HELD);
    CHECK(pwh_word(0xFFFFFFFF) == PWH_W_RESTARTED);
    CHECK(pwh_word(0) == PWH_W_LOST);
    CHECK(pwh_word(HP_HIB_WAKE) == PWH_W_LOST);
    CHECK(!strcmp(pwh_word_name(PWH_W_HELD), "held (parked)"));
}

static void t_checks(void)
{
    struct pwh_reply r;

    good(&r);
    CHECK(pwh_check(&r) == PWH_OK);
    r.status = -2;
    CHECK(pwh_check(&r) == PWH_NOREPLY);
    good(&r);
    r.status = HP_E_BUSY;
    r.n = 0;
    CHECK(pwh_check(&r) == PWH_REFUSED);
    good(&r);
    r.status = HP_E_STATE;
    r.n = 2;                        /* the entry refusal: reg, core */
    CHECK(pwh_check(&r) == PWH_REFUSED);
    good(&r);
    r.n = 5;
    CHECK(pwh_check(&r) == PWH_SHORT);
    /* the restore before anything else */
    good(&r);
    r.w[HP_HIB_R_MISMATCH] = HP_HIB_M_PLL << 3;
    r.w[HP_HIB_R_SRAM_BAD] = 9;
    CHECK(pwh_check(&r) == PWH_RESTORE);
    good(&r);
    r.w[HP_HIB_R_FLAGS] = 1 << 8 | HP_HIB_F_SDRAM;
    CHECK(pwh_check(&r) == PWH_DENIED && pwh_denied(&r) == 1);
    good(&r);
    r.status = HP_E_STATE;
    r.w[HP_HIB_R_EXIT] = HP_HIB_X_GAVEUP;
    CHECK(pwh_check(&r) == PWH_GAVEUP);
    r.w[HP_HIB_R_EXIT] = HP_HIB_X_STALL;
    CHECK(pwh_check(&r) == PWH_STALL);
    r.w[HP_HIB_R_EXIT] = HP_HIB_X_SDIDLE;
    CHECK(pwh_check(&r) == PWH_SDIDLE);
    r.w[HP_HIB_R_EXIT] = HP_HIB_X_SDRAM;
    CHECK(pwh_check(&r) == PWH_SDRAM);
    /* an ok exit with a failure status is not ok */
    good(&r);
    r.status = HP_E_STATE;
    CHECK(pwh_check(&r) == PWH_REFUSED);
    good(&r);
    r.w[HP_HIB_R_SRAM_BAD] = 1;
    CHECK(pwh_check(&r) == PWH_SRAM_LOST);
    /* SDRAM and the frame are reported, not required */
    good(&r);
    r.w[HP_HIB_R_SDRAM_BAD] = 4;
    r.w[HP_HIB_R_FRAME_POST] = 0x4321;
    CHECK(pwh_check(&r) == PWH_OK);
    CHECK(pwh_sdram_kept(&r) == 1 && !pwh_frame_kept(&r));
    r.w[HP_HIB_R_SDRAM_BAD] = 0xFFFFFFFF;
    CHECK(pwh_sdram_kept(&r) == 2);
    r.w[HP_HIB_R_SDRAM_BAD] = 0;
    CHECK(pwh_sdram_kept(&r) == 0);
    CHECK(!strcmp(pwh_check_name(PWH_SRAM_LOST), "SRAM lost"));
    CHECK(!strcmp(pwh_check_name((enum pwh_check)99), "?"));
}

static void t_counter(void)
{
    struct pwh_reply r;

    good(&r);
    r.w[HP_HIB_R_HELD_US] = 1000000;
    CHECK(pwh_counter_ran(&r, 1000000));
    r.w[HP_HIB_R_HELD_US] = 985000;     /* the 1 s hold, 15 ms short */
    CHECK(pwh_counter_ran(&r, 1000000));
    r.w[HP_HIB_R_HELD_US] = 5000;       /* stopped in the dark */
    CHECK(!pwh_counter_ran(&r, 1000000));
    r.w[HP_HIB_R_HELD_US] = 55000000;   /* 60 s, 5 s short: within 10 % */
    CHECK(pwh_counter_ran(&r, 60000000));
    r.w[HP_HIB_R_HELD_US] = 30000000;
    CHECK(!pwh_counter_ran(&r, 60000000));
}

static void t_verdict(void)
{
    struct pwh_reply r;
    bool pass[PWH_HOLDS] = { true, false, false };

    good(&r);
    CHECK(pwh_cycle_pass(BCMDT_HIB_OK, HP_HIB_PARKED, &r));
    CHECK(!pwh_cycle_pass(BCMDT_HIB_E_WAKE, HP_HIB_PARKED, &r));
    CHECK(!pwh_cycle_pass(BCMDT_HIB_OK, 0xFFFFFFFF, &r));
    r.w[HP_HIB_R_SRAM_BAD] = 1;
    CHECK(!pwh_cycle_pass(BCMDT_HIB_OK, HP_HIB_PARKED, &r));
    /* PW5: the first hold decides; the longer ones are readings */
    CHECK(pwh_pw5_pass(pass, 2));
    CHECK(!pwh_pw5_pass(pass, 0));
    pass[0] = false;
    CHECK(!pwh_pw5_pass(pass, 3));
    /* the wake against today's 370 ms cold start */
    CHECK(pwh_speedup_x10(0) == 0);
    CHECK(pwh_speedup_x10(10000) == 370);
    CHECK(pwh_speedup_x10(370000) == 10);
}

static void t_raw(void)
{
    uint16_t v[PWH_RAW];
    int i;

    for (i = 0; i < PWH_RAW; i++)
        v[i] = 0xFFFF;
    CHECK(pwh_raw_class(v) == PWH_R_ONES);
    for (i = 0; i < PWH_RAW; i++)
        v[i] = 0;
    CHECK(pwh_raw_class(v) == PWH_R_ZEROS);
    v[4] = 0x0002;                  /* CONTROL's write-ready bit */
    CHECK(pwh_raw_class(v) == PWH_R_MIXED);
    CHECK(!strcmp(pwh_raw_name(PWH_R_ONES), "all ones"));
}

static void t_locks(void)
{
    struct pwh_lock l;

    pwh_lock_init(&l);
    CHECK(!pwh_pw5_open(&l) && !pwh_pw6_open(&l));
    pwh_note_pw5(&l, true);         /* PW5 can't run before PW1 ... */
    CHECK(!pwh_pw6_open(&l));       /* ... so PW6 stays shut too */
    pwh_lock_init(&l);
    pwh_note_pw1(&l, false);
    CHECK(!pwh_pw5_open(&l));
    pwh_note_pw1(&l, true);
    CHECK(pwh_pw5_open(&l) && !pwh_pw6_open(&l));
    pwh_note_pw5(&l, false);        /* refused before the pins */
    CHECK(!pwh_pw6_open(&l));
    pwh_note_pw5(&l, true);
    CHECK(pwh_pw6_open(&l));
    pwh_note_pw1(&l, false);        /* a later failure doesn't relock */
    pwh_note_pw5(&l, false);
    CHECK(pwh_pw5_open(&l) && pwh_pw6_open(&l));
}

int main(void)
{
    t_constants();
    t_holds();
    t_request();
    t_word();
    t_checks();
    t_counter();
    t_verdict();
    t_raw();
    t_locks();
    if (fails)
        printf("%d failed\n", fails);
    return fails != 0;
}
