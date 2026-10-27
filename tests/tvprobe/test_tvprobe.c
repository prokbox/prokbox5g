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
 * Host test of the TV probe rows' pure logic (apps/ipodui/devbcm_tv.h):
 * the deadline and the hang watchdog, the requests, the reply checks, the
 * PB-7 rate classes, the PB-9 verdict and the rows' locks.
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
#include "devbcm_tv.h"

#define BVP_E_TIMEOUT_FOR_TEST (-2)  /* bcmvp.h BVP_E_TIMEOUT */

static int fails;

#define CHECK(c) do { if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void t_timeouts(void)
{
    struct tvp_dog d;
    uint32_t t;

    /* the deadline covers the window, both waits and the hang rule */
    CHECK(tvp_deadline_us(TVP_PB7_MS) == 500000 + 20000 + 2000000);
    CHECK(tvp_deadline_us(HP_TV_OUTPUT_MS) > HP_TV_OUTPUT_MS * 1000u +
          2 * HP_TV_WAIT_US);
    CHECK(TVP_PB7_MS <= HP_TV_TIMING_MS && TVP_OUT_MS <= HP_TV_OUTPUT_MS);
    /* longer than the usual 2 s for every window we send */
    CHECK(tvp_deadline_us(TVP_OUT_MS) > 2000000 + TVP_OUT_MS * 1000u);

    /* the watchdog: moving words never hang, even over a long window */
    tvp_dog_start(&d, 0xFFFFF000u);         /* the counter wraps */
    for (t = 0; t < 8000000; t += TVP_LOOK_US)
        CHECK(!tvp_dog_hung(&d, 0xFFFFF000u + t, HP_TV_PROG(HP_TV_P_HOLD,
                            t / 1000), 0x12345));
    /* the progress word stuck but the heartbeat moving: alive */
    tvp_dog_start(&d, 0);
    for (t = 0; t < 5000000; t += TVP_LOOK_US)
        CHECK(!tvp_dog_hung(&d, t, 7, t / 1000));
    /* both stuck: a hang exactly at TVP_HANG_US after the last move */
    tvp_dog_start(&d, 100);
    CHECK(!tvp_dog_hung(&d, 100, 1, 1));
    CHECK(!tvp_dog_hung(&d, 100 + TVP_HANG_US - 1, 1, 1));
    CHECK(tvp_dog_hung(&d, 100 + TVP_HANG_US, 1, 1));
    CHECK(!tvp_dog_hung(&d, 100 + TVP_HANG_US + 5, 2, 1));  /* moved again */
    CHECK(!tvp_dog_hung(&d, 105 + 2 * TVP_HANG_US - 1, 2, 1));
    CHECK(tvp_dog_hung(&d, 105 + 2 * TVP_HANG_US, 2, 1));

    CHECK(tvp_prog_phase(HP_TV_PROG(HP_TV_P_HOLD, 1234)) == HP_TV_P_HOLD);
    CHECK(tvp_prog_ms(HP_TV_PROG(HP_TV_P_HOLD, 1234)) == 1234);
    CHECK(tvp_prog_phase(0x12345678) == 0 && tvp_prog_ms(0x12345678) == 0);
}

static void t_requests(void)
{
    uint32_t a[8];

    memset(a, 0, sizeof(a));
    CHECK(tvp_request(a, false, HP_TV_PAL, 0xC0DE, 500, 9, 9, 9) == 5);
    CHECK(a[0] == HP_TV_MAGIC && a[1] == HP_TV_TIMING && a[2] == HP_TV_PAL &&
          a[3] == 0xC0DE && a[4] == 500);
    CHECK(tvp_request(a, true, HP_TV_NTSC, 7, 4000, HP_TV_Y32, HP_TV_W704,
                      HP_TV_PAT_LINES) == 8);
    CHECK(a[1] == HP_TV_OUTPUT && a[5] == HP_TV_Y32 && a[6] == HP_TV_W704 &&
          a[7] == HP_TV_PAT_LINES);
    /* the image's own bounds: never sent */
    CHECK(tvp_request(a, false, 2, 7, 500, 0, 0, 0) == 0);
    CHECK(tvp_request(a, false, 0, 7, 0, 0, 0, 0) == 0);
    CHECK(tvp_request(a, false, 0, 7, HP_TV_TIMING_MS + 1, 0, 0, 0) == 0);
    CHECK(tvp_request(a, false, 0, 7, HP_TV_TIMING_MS, 0, 0, 0) == 5);
    CHECK(tvp_request(a, true, 0, 7, HP_TV_OUTPUT_MS + 1, 0, 0, 0) == 0);
    CHECK(tvp_request(a, true, 0, 7, 100, HP_TV_ARMS, 0, 0) == 0);
    CHECK(tvp_request(a, true, 0, 7, 100, 0, 2, 0) == 0);
    CHECK(tvp_request(a, true, 0, 7, 100, 0, 0, 2) == 0);
}

static void good(struct tvp_reply *r, uint32_t fields, uint32_t us)
{
    memset(r, 0, sizeof(*r));
    r->status = HP_OK;
    r->n = HP_TV_REPLY_WORDS;
    r->w[HP_TV_R_EXIT] = HP_TV_X_OK;
    r->w[HP_TV_R_PHASE] = HP_TV_P_HOLD;
    r->w[HP_TV_R_FIELDS] = fields;
    r->w[HP_TV_R_US] = us;
}

static void t_checks(void)
{
    struct tvp_reply r;

    good(&r, 30, 500000);
    CHECK(tvp_check(&r) == TVP_OK);
    r.status = BVP_E_TIMEOUT_FOR_TEST;
    CHECK(tvp_check(&r) == TVP_NOREPLY);
    good(&r, 30, 500000);
    r.status = HP_E_INVAL;
    CHECK(tvp_check(&r) == TVP_REFUSED);
    r.status = HP_E_UNSUPPORTED;
    CHECK(tvp_check(&r) == TVP_REFUSED);
    good(&r, 30, 500000);
    r.n = 16;                               /* a reply cut short */
    CHECK(tvp_check(&r) == TVP_SHORT);
    good(&r, 30, 500000);
    r.status = HP_E_STATE;
    r.w[HP_TV_R_EXIT] = HP_TV_X_TIMEOUT31;
    CHECK(tvp_check(&r) == TVP_EARLY);
    r.w[HP_TV_R_EXIT] = HP_TV_X_OK;         /* inconsistent: still early */
    CHECK(tvp_check(&r) == TVP_EARLY);
    r.status = HP_OK;
    r.w[HP_TV_R_EXIT] = HP_TV_X_CHAIN;
    CHECK(tvp_check(&r) == TVP_EARLY);
    good(&r, 30, 500000);
    r.w[HP_TV_R_MISMATCH] = HP_TV_M_GPIO19;
    CHECK(tvp_check(&r) == TVP_RESTORE);
    good(&r, 30, 500000);
    r.w[HP_TV_R_FLAGS] = HP_TV_F_MV | 1 << 8;
    CHECK(tvp_denied(&r) == 1 && tvp_check(&r) == TVP_DENIED);
    r.w[HP_TV_R_FLAGS] = HP_TV_F_MV | HP_TV_F_FSEL;   /* flags alone: fine */
    CHECK(tvp_check(&r) == TVP_OK);
    CHECK(strcmp(tvp_check_name(TVP_DENIED), "write denied") == 0);
    CHECK(strcmp(tvp_check_name((enum tvp_check)99), "?") == 0);
}

static void t_rates(void)
{
    struct tvp_reply r;

    CHECK(tvp_rate_mhz(30, 500000) == 60000);
    CHECK(tvp_rate_mhz(1, 0) == 0);
    /* NTSC 59.94 changes a second over 500 ms: 29 or 30 counted */
    CHECK(tvp_rate_class(HP_TV_NTSC, 30, 500000) == TVP_R_FIELD);
    CHECK(tvp_rate_class(HP_TV_NTSC, 29, 500000) == TVP_R_FIELD);
    CHECK(tvp_rate_class(HP_TV_NTSC, 30, 500123) == TVP_R_FIELD);
    CHECK(tvp_rate_class(HP_TV_NTSC, 15, 500000) == TVP_R_FRAME);
    CHECK(tvp_rate_class(HP_TV_NTSC, 14, 500000) == TVP_R_FRAME);
    /* PAL 50: 25 (field) or 12-13 (frame) */
    CHECK(tvp_rate_class(HP_TV_PAL, 25, 500000) == TVP_R_FIELD);
    CHECK(tvp_rate_class(HP_TV_PAL, 12, 500000) == TVP_R_FRAME);
    CHECK(tvp_rate_class(HP_TV_PAL, 13, 500000) == TVP_R_FRAME);
    /* the standards apart: PAL's count is not NTSC's and back */
    CHECK(tvp_rate_class(HP_TV_NTSC, 25, 500000) == TVP_R_ODD);
    CHECK(tvp_rate_class(HP_TV_PAL, 30, 500000) == TVP_R_ODD);
    CHECK(tvp_rate_class(HP_TV_NTSC, 0, 500000) == TVP_R_NONE);
    CHECK(tvp_rate_class(HP_TV_NTSC, 5, 0) == TVP_R_NONE);
    CHECK(tvp_rate_class(HP_TV_NTSC, 1000, 500000) == TVP_R_ODD);
    /* 1 s, the spec's window: 59-61 and 49-51 */
    CHECK(tvp_rate_class(HP_TV_NTSC, 60, 1000000) == TVP_R_FIELD);
    CHECK(tvp_rate_class(HP_TV_PAL, 50, 1000000) == TVP_R_FIELD);
    CHECK(tvp_rate_class(HP_TV_PAL, 55, 1000000) == TVP_R_ODD);

    good(&r, 30, 500000);
    CHECK(tvp_pb7_pass(HP_TV_NTSC, &r));
    CHECK(!tvp_pb7_pass(HP_TV_PAL, &r));
    good(&r, 0, 500000);
    CHECK(!tvp_pb7_pass(HP_TV_NTSC, &r));
    good(&r, 30, 500000);
    r.w[HP_TV_R_MISMATCH] = HP_TV_M_CLOCK;
    CHECK(!tvp_pb7_pass(HP_TV_NTSC, &r));
}

static void t_arms_and_locks(void)
{
    struct tvp_lock l;
    struct tvp_reply r;
    int arm, seen = 0;

    for (arm = 0; arm < TVP_ARMS; arm++)
    {
        CHECK(tvp_arm_std(arm) <= HP_TV_PAL);
        CHECK(tvp_arm_fmt(arm) < HP_TV_ARMS);
        seen |= 1 << (tvp_arm_std(arm) * HP_TV_ARMS + tvp_arm_fmt(arm));
    }
    CHECK(seen == (1 << TVP_ARMS) - 1);     /* every pair, once */
    CHECK(strcmp(tvp_fmt_name(HP_TV_RGB565), "rgb565") == 0);
    CHECK(strcmp(tvp_std_name(HP_TV_PAL), "pal") == 0);
    CHECK(strcmp(tvp_saw_name(-1), "no answer") == 0);

    tvp_lock_init(&l);
    CHECK(!tvp_pb8_open(&l) && !tvp_pb9_open(&l));
    tvp_note_pb7(&l, true, false);
    CHECK(!tvp_pb8_open(&l));
    tvp_note_pb7(&l, true, true);
    CHECK(tvp_pb8_open(&l) && !tvp_pb9_open(&l));
    tvp_note_pb7(&l, false, false);         /* a later fail keeps the pass */
    CHECK(tvp_pb8_open(&l));

    good(&r, 0, 4000000);
    tvp_note_pb8(&l, 2, &r, TVP_SAW_GARBLED);
    CHECK(!tvp_pb9_open(&l));
    tvp_note_pb8(&l, 2, &r, -1);            /* no answer */
    CHECK(!tvp_pb9_open(&l));
    r.status = HP_E_STATE;                  /* the window never ran */
    r.w[HP_TV_R_EXIT] = HP_TV_X_TIMEOUT30;
    tvp_note_pb8(&l, 2, &r, TVP_SAW_BARS);
    CHECK(!tvp_pb9_open(&l));
    good(&r, 0, 4000000);
    tvp_note_pb8(&l, TVP_ARMS, &r, TVP_SAW_BARS);
    CHECK(!tvp_pb9_open(&l));
    tvp_note_pb8(&l, 6, &r, TVP_SAW_BARS);
    CHECK(tvp_pb9_open(&l) && l.bars_arm == 6);
    tvp_note_pb8(&l, 1, &r, TVP_SAW_NOTHING);   /* keeps the bars arm */
    CHECK(l.bars_arm == 6);
    tvp_note_pb8(&l, 1, &r, TVP_SAW_BARS);      /* the last bars wins */
    CHECK(l.bars_arm == 1);

    CHECK(tvp_scale_verdict(TVP_EDGE_FULL, TVP_EDGE_FULL) == TVP_SCALER);
    CHECK(tvp_scale_verdict(TVP_EDGE_BAND, TVP_EDGE_FULL) == TVP_WINDOW);
    CHECK(tvp_scale_verdict(TVP_EDGE_FULL, TVP_EDGE_BAND) == TVP_UNCLEAR);
    CHECK(tvp_scale_verdict(TVP_EDGE_NOTHING, TVP_EDGE_FULL) == TVP_UNCLEAR);
    CHECK(tvp_scale_verdict(-1, -1) == TVP_UNCLEAR);
}

int main(void)
{
    t_timeouts();
    t_requests();
    t_checks();
    t_rates();
    t_arms_and_locks();
    if (fails)
        printf("%d failed\n", fails);
    return fails ? 1 : 0;
}
