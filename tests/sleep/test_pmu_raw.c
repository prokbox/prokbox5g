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

/* Phase 18-1 host test: the raw PMU path (pmu_raw-pp502x.c) against a
 * scripted bus: the transactions each read makes (the RTC as one address
 * write and block reads of 4 and 3 bytes, quirk 22), the values, a bus
 * held busy (false, never a hang), and the RTC's BCD to seconds since
 * 2000 (sleep_rtc_from_bcd/_date). */
#define PMU_RAW_HOST_TEST
#include <stdio.h>
#include <string.h>
#include "../../firmware/target/arm/pp/pmu_raw-pp502x.c"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* the bus */
static unsigned char ctrl, addr, data[4], regs[256];
static unsigned ptr;
static int busy_left;           /* status reads still busy after a SEND */
static int busy_each = 2;       /* per SEND; < 0 = held for ever */
static int hold_from = -1;      /* b12: from this SEND (0-based) on, the
                                 * bus stays busy; -1 never */
static int hold_until = 1 << 30;/* ... up to this SEND (exclusive) */
static int nsend;               /* SENDs since reset() */
static uint32_t held_to;        /* a held SEND: busy until usec reaches
                                 * this */
static uint32_t hold_us;        /* a held SEND's length: by default just
                                 * past one timed-out wait, over before the
                                 * retry's */
static bool held_ever;          /* or for ever */
static uint32_t usec;
struct tr { bool read; int len; unsigned char d[4]; };
static struct tr log_[16];
static int ntr;

unsigned char host_i2c_rd(unsigned off)
{
    if (off == R_STATUS)
    {
        if (held_ever || (int32_t)(held_to - usec) > 0)
            return STATUS_BUSY;
        if (busy_left < 0)
            return STATUS_BUSY;
        if (busy_left > 0)
        {
            busy_left--;
            return STATUS_BUSY;
        }
        return 0;
    }
    if (off == R_CTRL)
        return ctrl;
    if (off >= R_DATA(0) && off <= R_DATA(3))
        return data[(off - R_DATA(0)) / 4];
    return 0;
}

void host_i2c_wr(unsigned off, unsigned char v)
{
    if (off == R_ADDR)
        addr = v;
    else if (off >= R_DATA(0) && off <= R_DATA(3))
        data[(off - R_DATA(0)) / 4] = v;
    else if (off == R_CTRL)
    {
        ctrl = v & ~CTRL_SEND;
        if (v & CTRL_SEND)
        {
            struct tr *t = &log_[ntr < 16 ? ntr++ : 15];
            int len = ((v >> 1) & 3) + 1, i;

            t->read = (v & CTRL_READ) != 0;
            t->len = len;
            CHECK((addr >> 1) == PMU_ADDR, "address %#x", addr);
            CHECK(((addr & 1) != 0) == t->read, "R/W bit vs CTRL_READ");
            if (t->read)
                for (i = 0; i < len; i++)
                    data[i] = regs[(ptr++) & 0xFF];
            else
            {
                memcpy(t->d, data, 4);
                ptr = data[0];
                if (len == 2)
                    regs[data[0]] = data[1];
            }
            busy_left = busy_each;
            if (hold_from >= 0 && nsend >= hold_from && nsend < hold_until)
            {
                if (hold_until == 1 << 30)
                    held_ever = true;
                else
                    held_to = usec + hold_us;
            }
            nsend++;
        }
    }
}

uint32_t host_usec(void)
{
    return usec += 10;
}

static void reset(int busy)
{
    ntr = 0;
    nsend = 0;
    held_to = usec;
    held_ever = false;
    hold_us = PMU_RAW_WAIT_US + 500;
    hold_from = -1;
    hold_until = 1 << 30;
    busy_each = busy;
    busy_left = 0;
    ctrl = 0;
}

static void test_rtc(void)
{
    static const unsigned char bcd[7] = { 0x30, 0x45, 0x13, 0x04, 0x22,
                                          0x10, 0x26 };
    uint32_t s = 0;

    memcpy(regs + 0x0A, bcd, 7);
    reset(2);
    CHECK(pmu_raw_rtc_seconds(&s), "rtc read");
    CHECK(s == 845991930u, "rtc %u want 845991930", (unsigned)s);
    CHECK(ntr == 3, "rtc transactions %d want 3", ntr);
    CHECK(!log_[0].read && log_[0].len == 1 && log_[0].d[0] == 0x0A,
          "rtc: address write 0x0A");
    CHECK(log_[1].read && log_[1].len == 4, "rtc: block read of 4");
    CHECK(log_[2].read && log_[2].len == 3, "rtc: block read of 3");

    regs[0x0B] = 0x6A;          /* not BCD */
    reset(0);
    CHECK(!pmu_raw_rtc_seconds(&s), "bad BCD refused");
    /* b12: tried twice (the retry), 3 transactions each */
    CHECK(ntr == 6, "bad BCD: transactions %d want 6 (one retry)", ntr);
#ifdef PMU_RAW_DIAG
    CHECK(pmu_raw_rtc_last.tries == 2 &&
          pmu_raw_rtc_last.step[0] == PMU_RAW_STEP_BCD &&
          pmu_raw_rtc_last.step[1] == PMU_RAW_STEP_BCD,
          "bad BCD: steps %u/%u tries %u", pmu_raw_rtc_last.step[0],
          pmu_raw_rtc_last.step[1], pmu_raw_rtc_last.tries);
    CHECK(pmu_raw_rtc_last.r[1][0] == 0x30 && pmu_raw_rtc_last.r[1][1] == 0x6A
          && pmu_raw_rtc_last.r[1][6] == 0x26, "bad BCD: the bytes kept");
    CHECK(!strcmp(pmu_raw_step_name(PMU_RAW_STEP_BCD), "bcd") &&
          !strcmp(pmu_raw_step_name(PMU_RAW_STEP_READ1), "read1") &&
          !strcmp(pmu_raw_step_name(9), "?"), "step names");
#endif
    memcpy(regs + 0x0A, bcd, 7);
}

/* b12 round 2 (#255): a slow bus.  Each transaction busy for 6 ms (a
 * 4-byte read at about 8 kHz) still reads in one try: the bound is
 * Apple's 100 ms, not the old 2 ms (pinned: 100000) */
static void test_rtc_slow_bus(void)
{
    static const unsigned char bcd[7] = { 0x30, 0x45, 0x13, 0x04, 0x22,
                                          0x10, 0x26 };
    uint32_t s = 0;

    CHECK(PMU_RAW_WAIT_US == 100000, "the bound is Apple's 100 ms (%u)",
          (unsigned)PMU_RAW_WAIT_US);
    memcpy(regs + 0x0A, bcd, 7);
    reset(2);
    hold_from = 0;              /* every SEND busy for 6 ms */
    hold_until = 1000;
    hold_us = 6000;
    CHECK(pmu_raw_rtc_seconds(&s), "slow bus: read");
    CHECK(s == 845991930u, "slow bus: rtc %u", (unsigned)s);
    CHECK(ntr == 3, "slow bus: one try, 3 transactions (%d)", ntr);
#ifdef PMU_RAW_DIAG
    CHECK(pmu_raw_rtc_last.tries == 1 &&
          pmu_raw_rtc_last.wait_us[0] >= 6000 &&
          pmu_raw_rtc_last.wait_us[0] < 7000,
          "slow bus: tries %u wait %u", pmu_raw_rtc_last.tries,
          (unsigned)pmu_raw_rtc_last.wait_us[0]);
#endif
}

/* b12 (#255): the retry after ~1 ms, and the step each failure names */
static void test_rtc_retry(void)
{
    static const unsigned char bcd[7] = { 0x30, 0x45, 0x13, 0x04, 0x22,
                                          0x10, 0x26 };
    uint32_t s = 0, t0;

    memcpy(regs + 0x0A, bcd, 7);
    /* the first try's address write held busy, the retry clean */
    reset(2);
    hold_from = 0;
    hold_until = 1;
    t0 = usec;
    CHECK(pmu_raw_rtc_seconds(&s), "retry: second try reads");
    CHECK(s == 845991930u, "retry: rtc %u", (unsigned)s);
    CHECK(ntr == 4, "retry: transactions %d want 1 + 3", ntr);
    CHECK(usec - t0 >= PMU_RAW_WAIT_US + 1000u, "retry: waited ~1 ms "
          "after the timeout (%u us)", (unsigned)(usec - t0));
    CHECK(usec - t0 < 3u * PMU_RAW_WAIT_US + 2000u, "retry: bounded (%u us)",
          (unsigned)(usec - t0));
#ifdef PMU_RAW_DIAG
    CHECK(pmu_raw_rtc_last.tries == 2 &&
          pmu_raw_rtc_last.step[0] == PMU_RAW_STEP_SEND &&
          pmu_raw_rtc_last.step[1] == PMU_RAW_STEP_OK,
          "retry: steps %u/%u tries %u", pmu_raw_rtc_last.step[0],
          pmu_raw_rtc_last.step[1], pmu_raw_rtc_last.tries);
    CHECK(pmu_raw_rtc_last.status[0] == STATUS_BUSY &&
          pmu_raw_rtc_last.wait_us[0] == PMU_RAW_WAIT_US + 1,
          "retry: try 1 status %#x wait %u", pmu_raw_rtc_last.status[0],
          (unsigned)pmu_raw_rtc_last.wait_us[0]);
    CHECK(pmu_raw_rtc_last.wait_us[1] > 0 &&
          pmu_raw_rtc_last.wait_us[1] < PMU_RAW_WAIT_US,
          "retry: try 2 wait %u", (unsigned)pmu_raw_rtc_last.wait_us[1]);
#endif

    /* the first block read held (both tries): read1 */
    reset(2);
    hold_from = 1;
    hold_until = 2;
    CHECK(pmu_raw_rtc_seconds(&s), "read1 held once: the retry reads");
#ifdef PMU_RAW_DIAG
    CHECK(pmu_raw_rtc_last.step[0] == PMU_RAW_STEP_READ1,
          "read1: step %u", pmu_raw_rtc_last.step[0]);
#endif
    reset(2);
    hold_from = 2;              /* the second block read, then for ever */
    CHECK(!pmu_raw_rtc_seconds(&s), "read2 held: false");
#ifdef PMU_RAW_DIAG
    CHECK(pmu_raw_rtc_last.tries == 2 &&
          pmu_raw_rtc_last.step[0] == PMU_RAW_STEP_READ2 &&
          pmu_raw_rtc_last.step[1] == PMU_RAW_STEP_SEND,
          "read2: steps %u/%u", pmu_raw_rtc_last.step[0],
          pmu_raw_rtc_last.step[1]);
    CHECK(pmu_raw_rtc_last.r[0][0] == 0x30 && pmu_raw_rtc_last.r[0][3] == 0x04
          && pmu_raw_rtc_last.r[0][4] == 0, "read2: block 1 kept, 2 zero");
#endif
}

static void test_battery(void)
{
    uint16_t mv = 0;

    regs[0x30] = 0xB0;
    regs[0x31] = 0x02;
    reset(3);
    CHECK(pmu_raw_battery_mv(&mv), "battery read");
    CHECK(mv == (((0xB0 << 2) | 2) * 6000u >> 10), "mv %u", mv);
    CHECK(ntr == 3, "battery transactions %d want 3", ntr);
    CHECK(!log_[0].read && log_[0].len == 2 && log_[0].d[0] == 0x2F
          && log_[0].d[1] == 0x05, "battery: ADC start ch 2");
    CHECK(!log_[1].read && log_[1].len == 1 && log_[1].d[0] == 0x30,
          "battery: result address");
    CHECK(log_[2].read && log_[2].len == 2, "battery: read 2");
}

static void test_stuck(void)
{
    uint32_t s;
    uint16_t mv;
    uint32_t t0;

    reset(-1);
    busy_left = -1;             /* the line held low from the start */
    t0 = usec;
    CHECK(!pmu_raw_rtc_seconds(&s), "stuck bus: rtc false");
    CHECK(usec - t0 < 10 * PMU_RAW_WAIT_US, "stuck bus: bounded wait");
    CHECK(!pmu_raw_battery_mv(&mv), "stuck bus: battery false");
    CHECK(ntr == 0, "stuck bus: nothing sent (%d)", ntr);

    reset(-1);                  /* idle at first, held after the SEND */
    CHECK(!pmu_raw_rtc_seconds(&s), "held after send: rtc false");
    CHECK(ntr == 1, "held after send: one transaction (%d), the retry "
          "found it busy", ntr);
}

static void test_dates(void)
{
    CHECK(sleep_rtc_from_date(2000, 1, 1, 0, 0, 0) == 0, "epoch");
    CHECK(sleep_rtc_from_date(2000, 3, 1, 0, 0, 0) == 60u * 86400,
          "2000 is a leap year");
    CHECK(sleep_rtc_from_date(2001, 3, 1, 0, 0, 0) == 36720000u,
          "2001-03-01");
    CHECK(sleep_rtc_from_date(2024, 2, 29, 12, 0, 0) == 762523200u,
          "2024-02-29 12:00");
    CHECK(sleep_rtc_from_date(2099, 12, 31, 23, 59, 59) == 3155759999u,
          "2099-12-31 23:59:59");
}

/* review M3: a frozen RTC still ends the sleep by the polls' sum */
static void test_frozen_rtc(void)
{
    struct sleep_core_req rq = { .deep_after_s = 300, .poll_s = 60 };
    struct sleep_core_state st;
    struct sleep_core_ev ev;
    enum sleep_wake w = WAKE_NONE;
    int i;

    sleep_core_begin(&st, &rq, 1000, false, false);
    for (i = 0; i < 5; i++)
    {
        ev = (struct sleep_core_ev){ .src = SRC_TIMER, .rtc_ok = true,
                                     .rtc = 1000, .batt_ok = true,
                                     .mv = 3900, .poll_s = 60 };
        w = sleep_classify(&st, &ev);
        CHECK((i < 4) == (w == WAKE_NONE), "frozen RTC poll %d: why %d",
              i, w);
    }
    CHECK(w == WAKE_DEEP && st.slept_s == 300, "frozen RTC: deep at 5 x 60"
          " (why %d slept %u)", w, (unsigned)st.slept_s);

    /* a running RTC ahead of the sum wins */
    sleep_core_begin(&st, &rq, 1000, false, false);
    ev = (struct sleep_core_ev){ .src = SRC_TIMER, .rtc_ok = true,
                                 .rtc = 1300, .batt_ok = true, .mv = 3900,
                                 .poll_s = 60 };
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_DEEP && st.slept_s == 300, "RTC ahead: deep (%d)", w);
}

/* review M4: no-edge packets are rate-limited per poll, never capped */
static void test_quiet_packets(void)
{
    struct sleep_core_req rq = { .deep_after_s = 3600, .poll_s = 60 };
    struct sleep_core_state st;
    struct sleep_core_ev ev;
    enum sleep_wake w;
    int i, mutes = 0;

    sleep_core_begin(&st, &rq, 1000, false, false);
    for (i = 0; i < 3 * SLEEP_QUIET_PKTS_MAX; i++)
    {
        /* a finger resting on the wheel: bit 30, no button */
        ev = (struct sleep_core_ev){ .src = SRC_WHEEL, .pkt_ready = true,
                                     .pkt = 0xC000001Au | (5u << 16) };
        w = sleep_classify(&st, &ev);
        CHECK(w == WAKE_NONE, "touch %d woke (%d)", i, w);
        if (st.act & SACT_WHEEL_MUTE)
            mutes++;
    }
    CHECK(mutes == 1, "one mute per poll interval (%d)", mutes);
    CHECK(st.win_bad == 0, "touches never count toward the cap");
    ev = (struct sleep_core_ev){ .src = SRC_TIMER, .rtc_ok = true,
                                 .rtc = 1060, .batt_ok = true, .mv = 3900,
                                 .poll_s = 60 };
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_NONE && st.quiet_pkts == 0, "the poll resets the count");
    /* after the poll the wheel is read again: a press still wakes */
    ev = (struct sleep_core_ev){ .src = SRC_WHEEL, .pkt_ready = true,
                                 .pkt = 0xC000011Au };
    w = sleep_classify(&st, &ev);
    CHECK(w == WAKE_BUTTON && st.wake_buttons == WBTN_SELECT,
          "a press after the mute's poll wakes (%d)", w);
}

/* 18-2's device case: PLAY held into the sleep, released, then PLAY
 * pressed to wake; the driver's first tick must show it released */
static void test_exit_release(void)
{
    CHECK(sleep_exit_first_buttons(WBTN_PLAY, WBTN_PLAY) == 0,
          "PLAY re-pressed: released for the first tick");
    CHECK(sleep_exit_first_buttons(WBTN_PLAY, WBTN_PLAY | WBTN_SELECT)
          == WBTN_SELECT, "another button stays pressed");
    CHECK(sleep_exit_first_buttons(0, WBTN_SELECT) == WBTN_SELECT,
          "nothing held into the sleep: as the packet");
    CHECK(sleep_exit_first_buttons(WBTN_PLAY, 0) == 0, "all released");
}

int main(void)
{
    test_exit_release();
    test_frozen_rtc();
    test_quiet_packets();
    test_dates();
    test_rtc();
    test_rtc_retry();
    test_rtc_slow_bus();
    test_battery();
    test_stuck();
    printf("pmu_raw: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
