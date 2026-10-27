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

#include <stdio.h>
#include <string.h>
#include "stor_pwr_core.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

#define HZ 100
/* the firmware's flash tuning (firmware/export/stor_pwr.h) */
static const struct stor_pwr_tune tune = {
    .hold_dark = 10 * HZ, .hold_lit = 60 * HZ, .min_off = 30 * HZ,
    .hold_max = 600 * HZ, .relax = 120 * HZ,
};

static void test_hold(void)
{
    struct stor_pwr s;

    stor_pwr_init(&s, 0, true);
    CHECK(stor_pwr_hold(&s, &tune, false) == 10 * HZ);
    CHECK(stor_pwr_hold(&s, &tune, true) == 60 * HZ);
    /* not due before the hold, due after; never while off */
    CHECK(!stor_pwr_off_due(&s, &tune, 500, 0, false));
    CHECK(!stor_pwr_off_due(&s, &tune, 10 * HZ, 0, false));
    CHECK(stor_pwr_off_due(&s, &tune, 10 * HZ + 1, 0, false));
    CHECK(!stor_pwr_off_due(&s, &tune, 10 * HZ + 1, 0, true));
    CHECK(stor_pwr_off_due(&s, &tune, 60 * HZ + 1, 0, true));
    /* an access moves the deadline */
    CHECK(!stor_pwr_off_due(&s, &tune, 20 * HZ, 15 * HZ, false));
    /* tick wrap */
    stor_pwr_init(&s, 0x7ffffff0L, true);
    CHECK(!stor_pwr_off_due(&s, &tune, (long)(0x7ffffff0UL + 100UL),
                            0x7ffffff0L, false));
    stor_pwr_note_off(&s, 0x7ffffff0L, STOR_PWR_R_IDLE, 10);
    CHECK(!stor_pwr_off_due(&s, &tune, 0x7ffffff0L + 5000L, 0, false));
}

static void test_backoff(void)
{
    struct stor_pwr s;
    long t = 0;
    int i;

    stor_pwr_init(&s, 0, true);
    /* cut at 10 s, back after 5 s: wasted, the hold doubles */
    t = 10 * HZ;
    CHECK(stor_pwr_note_off(&s, t, STOR_PWR_R_IDLE, 10) == 10 * HZ);
    t += 5 * HZ;
    CHECK(stor_pwr_note_on(&s, &tune, t, 40, STOR_PWR_R_ACCESS, 10)
          == 5 * HZ - 40);
    CHECK(s.wasted == 1);
    CHECK(stor_pwr_hold(&s, &tune, false) == 20 * HZ);
    /* more short gaps: 40, 80, ... capped at hold_max */
    for (i = 0; i < 10; i++)
    {
        t += stor_pwr_hold(&s, &tune, false) + 1;
        stor_pwr_note_off(&s, t, STOR_PWR_R_IDLE, 10);
        t += 2 * HZ;
        stor_pwr_note_on(&s, &tune, t, 40, STOR_PWR_R_ACCESS, 10);
    }
    CHECK(stor_pwr_hold(&s, &tune, false) == 600 * HZ);
    CHECK(stor_pwr_hold(&s, &tune, true) == 600 * HZ);
    /* a middling gap keeps it; a long one clears it */
    t += 600 * HZ + 1;
    stor_pwr_note_off(&s, t, STOR_PWR_R_IDLE, 10);
    t += 60 * HZ;
    stor_pwr_note_on(&s, &tune, t, 40, STOR_PWR_R_ACCESS, 10);
    CHECK(stor_pwr_hold(&s, &tune, false) == 600 * HZ);
    t += 600 * HZ + 1;
    stor_pwr_note_off(&s, t, STOR_PWR_R_IDLE, 10);
    t += 300 * HZ;
    stor_pwr_note_on(&s, &tune, t, 40, STOR_PWR_R_ACCESS, 10);
    CHECK(stor_pwr_hold(&s, &tune, false) == 10 * HZ);
    CHECK(s.ons == 13 && s.offs == 13);
    /* double notes are ignored */
    CHECK(stor_pwr_note_on(&s, &tune, t + 1, 0, STOR_PWR_R_API, 10) == 0);
    CHECK(s.ons == 13);
}

static void test_events(void)
{
    struct stor_pwr s;
    const struct stor_pwr_event *e;
    uint32_t k;

    stor_pwr_init(&s, 0, true);
    CHECK(stor_pwr_event_at(&s, 0) == NULL);
    stor_pwr_note_off(&s, 1000, STOR_PWR_R_SLEEP, 10);
    stor_pwr_note_on(&s, &tune, 9000, 37, STOR_PWR_R_USB, 10);
    e = stor_pwr_event_at(&s, 0);
    CHECK(e && !e->on && e->ms == 10000 && e->reason == STOR_PWR_R_SLEEP);
    e = stor_pwr_event_at(&s, 1);
    CHECK(e && e->on && e->ms == 370 && e->reason == STOR_PWR_R_USB);
    CHECK(!strcmp(stor_pwr_reason_name(e->reason), "usb"));
    CHECK(!strcmp(stor_pwr_reason_name(99), "?"));
    /* ms saturates */
    stor_pwr_note_off(&s, 9000 + 100000, STOR_PWR_R_IDLE, 10);
    CHECK(stor_pwr_event_at(&s, 2)->ms == 0xffff);
    /* the ring keeps the newest STOR_PWR_EVENTS */
    for (k = 0; k < 40; k++)
    {
        if (s.on)
            stor_pwr_note_off(&s, 200000 + k * 100, STOR_PWR_R_IDLE, 10);
        else
            stor_pwr_note_on(&s, &tune, 200000 + k * 100, 0,
                             STOR_PWR_R_ACCESS, 10);
    }
    CHECK(stor_pwr_event_at(&s, s.ev_head - STOR_PWR_EVENTS) != NULL);
    CHECK(stor_pwr_event_at(&s, s.ev_head - STOR_PWR_EVENTS - 1) == NULL);
    CHECK(stor_pwr_event_at(&s, s.ev_head) == NULL);
    /* on time */
    stor_pwr_init(&s, 0, true);
    stor_pwr_note_off(&s, 300, STOR_PWR_R_IDLE, 10);
    CHECK(stor_pwr_on_ticks(&s, 1000) == 300);
    stor_pwr_note_on(&s, &tune, 5000, 50, STOR_PWR_R_ACCESS, 10);
    CHECK(stor_pwr_on_ticks(&s, 5000) == 350);
    CHECK(stor_pwr_on_ticks(&s, 5100) == 450);
}

static void test_refill(void)
{
    CHECK(stor_pwr_refill_secs(1, 2, 100) == 3);
    CHECK(stor_pwr_refill_secs(1, 2, 50) == 3);     /* slow: no less */
    CHECK(stor_pwr_refill_secs(1, 2, 200) == 6);
    CHECK(stor_pwr_refill_secs(1, 2, 150) == 5);    /* 4.5 rounds up */
}

/* One hour of dark FLAC playback at a bitrate, the ~39 MB buffer refilled
 * from the low mark to full at 21 MB/s (S36c): cycles and on time, with
 * the policy as the ATA tick runs it (every HZ/2) */
static void model_hour(int kbps, unsigned *cycles, unsigned *on_pmil)
{
    const long buf = 39L * 1024 * 1024;
    const long rate = (long)kbps * 1000 / 8;        /* B/s */
    const long low = rate * 3;                      /* the 3 s margin */
    const long pwrup = HZ * 2 / 5;                  /* 0.4 s re-init [est] */
    struct stor_pwr s;
    long t, level = buf, last = 0, busy_until = -1;

    stor_pwr_init(&s, 0, true);
    for (t = 0; t < 3600 * HZ; t++)
    {
        if (busy_until < t && level < low)
        {
            long fill = (buf - level) * HZ / (21L * 1024 * 1024);
            long start = t;
            if (!s.on)
            {
                stor_pwr_note_on(&s, &tune, t + pwrup, pwrup,
                                 STOR_PWR_R_ACCESS, 10);
                start = t + pwrup;
            }
            busy_until = start + fill;
            level = buf;                /* filled by busy_until */
        }
        if (t <= busy_until)
            last = t;
        level -= rate / HZ;
        if (t % (HZ / 2) == 0 && t > busy_until
            && stor_pwr_off_due(&s, &tune, t, last, false))
            stor_pwr_note_off(&s, t, STOR_PWR_R_IDLE, 10);
    }
    *cycles = s.ons;
    *on_pmil = stor_pwr_on_ticks(&s, t) * 1000 / t;
}

static void test_model(void)
{
    unsigned c, p;

    model_hour(1100, &c, &p);
    printf("     model: FLAC 1100 kbit/s: %u cycles/h, on %u.%u %%\n",
           c, p / 10, p % 10);
    CHECK(c >= 11 && c <= 14);
    CHECK(p < 60);
    model_hour(800, &c, &p);
    printf("     model: FLAC  800 kbit/s: %u cycles/h, on %u.%u %%\n",
           c, p / 10, p % 10);
    CHECK(c >= 8 && c <= 10);
    CHECK(p < 50);
}

/* #114 H1/L4: a fake adapter whose power-on attempts fail on cue (-7 as
 * the 4-card size check would), and a driver that marks the drive off
 * after a failed power-up as ata.c does */
struct fake_hw {
    long clock;
    int fail_next;          /* attempts that fail before one works */
    int attempts, cuts;
    bool powered;
};

static int fake_on(void *ctx)
{
    struct fake_hw *h = ctx;
    h->attempts++;
    h->powered = true;
    if (h->fail_next > 0)
    {
        h->fail_next--;
        h->clock += 325;    /* 250 ms wait + the 3 s bounded reset */
        return -7;
    }
    h->clock += 40;         /* a good re-init, 0.4 s */
    return 0;
}

static void fake_off(void *ctx)
{
    struct fake_hw *h = ctx;
    h->powered = false;
    h->cuts++;
    h->clock += 10;         /* the 100 ms off */
}

static long fake_now(void *ctx)
{
    return ((struct fake_hw *)ctx)->clock;
}

struct fake_drv {
    struct fake_hw hw;
    struct stor_pwr_ops ops;
    struct stor_pwr s;
    bool on;                /* ata_state == ATA_ON, else ATA_OFF */
    bool allow;             /* spw_allow */
};

static void drv_init(struct fake_drv *d)
{
    memset(d, 0, sizeof *d);
    d->ops = (struct stor_pwr_ops){ fake_on, fake_off, fake_now, &d->hw };
    stor_pwr_init(&d->s, 0, true);
    d->on = d->allow = true;
    d->hw.powered = true;
}

static void drv_cut(struct fake_drv *d)
{
    d->hw.powered = false;
    d->on = false;
    stor_pwr_note_off(&d->s, d->hw.clock, STOR_PWR_R_IDLE, 10);
}

/* a read: powers up first when off; returns 0 or -2 */
static int drv_access(struct fake_drv *d)
{
    bool give_up = false;

    if (d->on)
        return 0;
    if (stor_pwr_power_on(&d->s, &tune, &d->ops, STOR_PWR_R_ACCESS, 10,
                          &give_up))
    {
        if (give_up)
            d->allow = false;
        return -2;          /* ata_state stays ATA_OFF */
    }
    d->on = true;
    return 0;
}

static void test_power_fail(void)
{
    struct fake_drv d;
    const struct stor_pwr_event *e;
    long t0;
    int i;

    /* once: the retry recovers, nothing counted as failed */
    drv_init(&d);
    d.hw.clock = 1000;
    drv_cut(&d);
    d.hw.clock += 50 * HZ;
    d.hw.fail_next = 1;
    CHECK(drv_access(&d) == 0);
    CHECK(d.on && d.hw.powered && d.hw.attempts == 2 && d.hw.cuts == 1);
    CHECK(d.s.on && d.s.ons == 1 && d.s.fails == 0 && d.s.fail_run == 0);
    e = stor_pwr_event_at(&d.s, d.s.ev_head - 1);
    CHECK(e && e->on && e->reason == STOR_PWR_R_ACCESS && e->ms == 3750);

    /* twice: a failed power-up, cut again and left off, the error back
     * after the two attempts (no wait on a drive half up); the next
     * access starts fresh and recovers */
    drv_init(&d);
    d.hw.clock = 1000;
    drv_cut(&d);
    d.hw.clock += 50 * HZ;
    d.hw.fail_next = 2;
    t0 = d.hw.clock;
    CHECK(drv_access(&d) == -2);
    CHECK(d.hw.clock - t0 <= 2 * 325 + 2 * 10);
    CHECK(!d.on && !d.hw.powered && d.hw.cuts == 2);
    CHECK(!d.s.on && d.s.fails == 1 && d.s.fail_run == 1 && !d.s.gave_up);
    e = stor_pwr_event_at(&d.s, d.s.ev_head - 1);
    CHECK(e && !e->on && e->reason == STOR_PWR_R_FAIL && e->ms == 6600);
    CHECK(!strcmp(stor_pwr_reason_name(e->reason), "fail"));
    CHECK(d.allow);
    CHECK(drv_access(&d) == 0);
    CHECK(d.on && d.hw.powered && d.s.on && d.s.fail_run == 0);
    CHECK(d.s.fails == 1 && d.s.ons == 1);
    /* the idle cut works again */
    drv_cut(&d);
    CHECK(!d.s.on && d.s.offs == 2);

    /* STOR_PWR_FAIL_MAX in a row: given up once, the switch off; a later
     * success powers on and stays (no idle cut while !allow) */
    drv_init(&d);
    d.hw.clock = 1000;
    drv_cut(&d);
    for (i = 0; i < STOR_PWR_FAIL_MAX; i++)
    {
        d.hw.fail_next = 2;
        CHECK(drv_access(&d) == -2);
        CHECK(!d.on && !d.hw.powered);
        CHECK(d.s.gave_up == (i == STOR_PWR_FAIL_MAX - 1));
        CHECK(d.allow == (i != STOR_PWR_FAIL_MAX - 1));
    }
    CHECK(d.s.fails == STOR_PWR_FAIL_MAX);
    e = stor_pwr_event_at(&d.s, d.s.ev_head - 1);
    CHECK(e && e->on && e->reason == STOR_PWR_R_GIVEUP);
    CHECK(!strcmp(stor_pwr_reason_name(e->reason), "giveup"));
    /* one more failure: no second give-up event */
    d.hw.fail_next = 2;
    CHECK(drv_access(&d) == -2);
    e = stor_pwr_event_at(&d.s, d.s.ev_head - 1);
    CHECK(e && e->reason == STOR_PWR_R_FAIL);
    CHECK(d.s.fail_run == STOR_PWR_FAIL_MAX + 1);
    CHECK(drv_access(&d) == 0);
    CHECK(d.on && d.s.on && d.s.fail_run == 0 && !d.allow);

    /* failures apart (a success between) never add up to a give-up */
    drv_init(&d);
    for (i = 0; i < 3 * STOR_PWR_FAIL_MAX; i++)
    {
        drv_cut(&d);
        d.hw.fail_next = (i % 2) ? 0 : 2;
        drv_access(&d);
        if (!d.on)
            CHECK(drv_access(&d) == 0);
    }
    CHECK(!d.s.gave_up && d.allow);
    CHECK(d.s.fails == (unsigned)(3 * STOR_PWR_FAIL_MAX + 1) / 2);
}

/* #114 M2: the cut only with the cache known safe */
static void test_cut_safe(void)
{
    struct stor_pwr s;

    /* flush offered: only a flush that worked */
    CHECK(stor_pwr_cut_safe(true, true, true, true));
    CHECK(stor_pwr_cut_safe(true, true, true, false));  /* SD adapter */
    CHECK(!stor_pwr_cut_safe(true, false, true, true));
    CHECK(!stor_pwr_cut_safe(true, false, true, false));
    /* a failed flush with no write cache: nothing to lose */
    CHECK(stor_pwr_cut_safe(true, false, false, false));
    /* no FLUSH CACHE: only with no write cache, and PM claimed
     * (upstream's guard: can_sleep || canflush) */
    CHECK(!stor_pwr_cut_safe(false, true, true, true));
    CHECK(stor_pwr_cut_safe(false, true, false, true));
    CHECK(!stor_pwr_cut_safe(false, true, false, false));
    CHECK(!stor_pwr_cut_safe(false, false, false, false));

    /* refusals: counted each time, one event per run */
    stor_pwr_init(&s, 0, true);
    stor_pwr_note_dirty(&s, 100);
    stor_pwr_note_dirty(&s, 150);
    stor_pwr_note_dirty(&s, 200);
    CHECK(s.dirty == 3 && s.ev_head == 1);
    CHECK(stor_pwr_event_at(&s, 0)->reason == STOR_PWR_R_DIRTY);
    CHECK(!strcmp(stor_pwr_reason_name(STOR_PWR_R_DIRTY), "dirty"));
    stor_pwr_note_clean(&s);
    stor_pwr_note_dirty(&s, 300);
    CHECK(s.dirty == 4 && s.ev_head == 2);
    CHECK(s.on && s.offs == 0);     /* no cut implied */
}

int main(void)
{
    test_power_fail();
    test_cut_safe();
    test_hold();
    test_backoff();
    test_events();
    test_refill();
    test_model();
    if (fails)
        fprintf(stderr, "%d check(s) failed\n", fails);
    return fails != 0;
}
