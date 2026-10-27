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
 * Host test of the alarm rules, apps/ipodui/alarm_core.c (P12, brief 3.3)
 * W0 skeleton: the cases are the contract; W1-A1 adds more
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
#include "ipodui/alarm_core.h"
#include "rtc_alarm_keep.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EQ(a, b) do { unsigned long _a = (a), _b = (b); if (_a != _b) { \
    fails++; fprintf(stderr, "FAIL %s:%d: %s = %lu, want %lu\n", \
    __FILE__, __LINE__, #a, _a, _b); } } while (0)

/* The firmware's crc_32 (firmware/common/crc32.c): MSB first, polynomial
 * 0x04C11DB7, no final xor; bitwise here */
uint32_t crc_32(const void *src, uint32_t len, uint32_t crc);
uint32_t crc_32(const void *src, uint32_t len, uint32_t crc)
{
    const unsigned char *p = src;
    while (len--)
    {
        int b;
        crc ^= (uint32_t)*p++ << 24;
        for (b = 0; b < 8; b++)
            crc = crc & 0x80000000u ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

/* Reference times (seconds since 2000-01-01 local, from Python) */
#define FRI_0659_59 846053999u          /* 2026-10-23 06:59:59 Fri */
#define FRI_0700    846054000u
#define FRI_0705    846054300u
#define FRI_070530  846054330u
#define FRI_0709    846054540u
#define FRI_0800    846057600u
#define FRI_0900    846061200u
#define FRI_2330    846113400u
#define SAT_0700    846140400u          /* 2026-10-24 */
#define MON_0700    846313200u          /* 2026-10-26 */
#define WED_0700    846486000u          /* 2026-10-28 */
#define OCT31_2300  846802800u
#define NOV01_0030  846808200u
#define DEC31_2300  852073200u
#define DEC31_2359  852076740u
#define JAN01_0030  852078600u          /* 2027 */
#define LEAP_EVE    888710399u          /* 2028-02-28 23:59:59 */
#define LEAP_0000   888710400u          /* 2028-02-29 00:00 */
#define LEAP_EVE_NOON 888667200u
#define LEAP_0600   888732000u

static struct alarm mk(int repeat, int h, int m)
{
    struct alarm a;

    memset(&a, 0, sizeof(a));
    a.on = 1; a.repeat = repeat; a.hour = h; a.min = m; a.snooze = 1;
    return a;
}

static void calendar(void)
{
    int y, mo, d, h, mi, s;

    EQ(alarm_rtc_make(2000, 1, 1, 0, 0, 0), 0);
    EQ(alarm_rtc_wday(0), 6);                       /* a Saturday */
    EQ(alarm_rtc_make(2026, 10, 23, 7, 5, 30), FRI_070530);
    EQ(alarm_rtc_wday(FRI_0700), 5);
    EQ(alarm_rtc_make(2028, 2, 29, 0, 0, 0), LEAP_0000);
    alarm_rtc_split(LEAP_0600, &y, &mo, &d, &h, &mi, &s);
    CHECK(y == 2028 && mo == 2 && d == 29 && h == 6 && mi == 0 && s == 0);
}

static void next(void)
{
    struct alarm a[3];
    int w = -1;

    /* daily: strictly after now */
    a[0] = mk(AR_DAILY, 7, 0);
    EQ(alarm_next(a, 1, FRI_0659_59, &w), FRI_0700);
    EQ(w, 0);
    EQ(alarm_next(a, 1, FRI_0700, NULL), SAT_0700);
    EQ(alarm_next(a, 1, FRI_2330, NULL), SAT_0700);     /* midnight */
    /* the week's modes */
    a[0].repeat = AR_WEEKDAYS;
    EQ(alarm_next(a, 1, FRI_0800, NULL), MON_0700);
    a[0].repeat = AR_WEEKENDS;
    EQ(alarm_next(a, 1, FRI_0800, NULL), SAT_0700);
    a[0].repeat = AR_DAYS; a[0].days = 1u << 3;         /* Wednesday */
    EQ(alarm_next(a, 1, FRI_0800, NULL), WED_0700);
    a[0].days = 0;                                      /* no day: none */
    EQ(alarm_next(a, 1, FRI_0800, NULL), 0);
    /* month and year ends */
    a[0] = mk(AR_DAILY, 0, 30);
    EQ(alarm_next(a, 1, OCT31_2300, NULL), NOV01_0030);
    EQ(alarm_next(a, 1, DEC31_2300, NULL), JAN01_0030);
    /* a leap day */
    a[0] = mk(AR_DAILY, 0, 0);
    EQ(alarm_next(a, 1, LEAP_EVE, NULL), LEAP_0000);
    a[0] = mk(AR_ONCE, 6, 0);
    a[0].year = 2028; a[0].mon = 2; a[0].mday = 29;
    EQ(alarm_next(a, 1, LEAP_EVE_NOON, NULL), LEAP_0600);
    /* once: in the future, and in the past (nothing) */
    a[0] = mk(AR_ONCE, 23, 59);
    a[0].year = 2026; a[0].mon = 12; a[0].mday = 31;
    EQ(alarm_next(a, 1, FRI_0800, NULL), DEC31_2359);
    EQ(alarm_next(a, 1, DEC31_2359, NULL), 0);
    /* off: nothing */
    a[0] = mk(AR_DAILY, 7, 0);
    a[0].on = 0;
    EQ(alarm_next(a, 1, FRI_0659_59, NULL), 0);
    /* a pending snooze before the next base time wins */
    a[0].on = 1; a[0].snooze_until = FRI_0709; a[0].snoozes = 1;
    EQ(alarm_next(a, 1, FRI_0705, &w), FRI_0709);
    /* two alarms at the same minute: the lower index */
    a[0] = mk(AR_DAILY, 7, 0);
    a[1] = mk(AR_DAILY, 7, 0);
    a[2] = mk(AR_DAILY, 6, 0);
    a[2].on = 0;
    EQ(alarm_next(a, 3, FRI_0659_59, &w), FRI_0700);
    EQ(w, 0);
}

static void ring(void)
{
    struct alarm a = mk(AR_DAILY, 7, 0);
    int i;

    for (i = 0; i < ALARM_SNOOZE_MAX; i++)
    {
        alarm_after_ring(&a, FRI_0700 + i * ALARM_SNOOZE_S, true);
        EQ(a.snoozes, i + 1);
        EQ(a.snooze_until, FRI_0700 + (i + 1) * ALARM_SNOOZE_S);
    }
    /* the fourth snooze is refused: a dismiss */
    alarm_after_ring(&a, FRI_0800, true);
    CHECK(a.snoozes == 0 && a.snooze_until == 0 && a.on);
    /* a dismissed one-shot switches off */
    a = mk(AR_ONCE, 7, 0);
    a.year = 2026; a.mon = 10; a.mday = 23;
    alarm_after_ring(&a, FRI_0700, false);
    CHECK(!a.on && a.snooze_until == 0);
}

static void boot(void)
{
    CHECK(!alarm_is_boot(FRI_0700, FRI_0700 - 1));
    CHECK(alarm_is_boot(FRI_0700, FRI_0700));
    CHECK(alarm_is_boot(FRI_0700, FRI_0700 + 119));
    CHECK(alarm_is_boot(FRI_0700, FRI_0700 + ALARM_BOOT_WIN_S));
    CHECK(!alarm_is_boot(FRI_0700, FRI_0700 + 121));
    CHECK(!alarm_is_boot(0, 30));
}

static void missed(void)
{
    struct alarm a[2];

    /* a missed one-shot: marked, switched off */
    a[0] = mk(AR_ONCE, 7, 0);
    a[0].year = 2026; a[0].mon = 10; a[0].mday = 23;
    alarm_mark_missed(a, 1, FRI_0700, FRI_0900);
    CHECK(a[0].missed && a[0].missed_at == FRI_0700 && !a[0].on);
    /* a missed weekday: marked, still on, Monday armed next */
    a[0] = mk(AR_WEEKDAYS, 7, 0);
    a[1] = mk(AR_DAILY, 8, 30);
    a[1].on = 0;
    alarm_mark_missed(a, 2, FRI_0700, FRI_0900);
    CHECK(a[0].missed && a[0].missed_at == FRI_0700 && a[0].on);
    CHECK(!a[1].missed);
    EQ(alarm_next(a, 2, FRI_0900, NULL), MON_0700);
    /* nothing armed: nothing missed */
    a[0] = mk(AR_DAILY, 7, 0);
    alarm_mark_missed(a, 1, 0, FRI_0900);
    CHECK(!a[0].missed);
}

static void pmu(void)
{
    static const uint8_t daily[7] = { 0x30, 0x05, 0x07, 0x07, 0x3f, 0x1f,
                                      0xff };
    static const uint8_t exact[7] = { 0x30, 0x05, 0x07, 0x07, 0x23, 0x10,
                                      0x26 };
    uint8_t r[7];

    alarm_pmu_regs(FRI_070530, true, r);
    CHECK(memcmp(r, daily, 7) == 0);
    alarm_pmu_regs(FRI_070530, false, r);
    CHECK(memcmp(r, exact, 7) == 0);
}

static void file(void)
{
    static struct alarms_file f, g;

    memset(&f, 0, sizeof(f));
    f.a[0] = mk(AR_WEEKDAYS, 6, 45);
    f.a[1] = mk(AR_ONCE, 9, 0);
    f.armed_rtc = FRI_0700;
    alarms_file_seal(&f, 2);
    CHECK(f.magic == ALARMS_MAGIC && f.version == ALARMS_VERSION
          && f.count == 2);
    EQ(alarms_file_check(&f, sizeof(f)), ALARMS_OK);
    g = f;                              /* the round trip */
    CHECK(memcmp(&g, &f, sizeof(f)) == 0);
    EQ(alarms_file_check(&f, 15), ALARMS_ESHORT);
    g.a[1].min ^= 1;
    EQ(alarms_file_check(&g, sizeof(g)), ALARMS_ECRC);
    g = f; g.magic ^= 1;
    EQ(alarms_file_check(&g, sizeof(g)), ALARMS_EMAGIC);
    g = f; g.version++;
    EQ(alarms_file_check(&g, sizeof(g)), ALARMS_EMAGIC);
    g = f; g.count = ALARM_MAX + 1;
    EQ(alarms_file_check(&g, sizeof(g)), ALARMS_ECOUNT);
}

/* -- W1-A1 additions (review test list, rev 2) ---------------------------- */

/* A model of the PCF50605's alarm side: the registers, OOCC1's RTCWAK as
 * pcf50605_wakeup_flags holds it, INT1's alarm flag (read to clear) */
struct pmu {
    uint8_t alm[7];
    bool rtcwak;
    bool int1;
};

static void pmu_arm(struct pmu *p, uint32_t at)
{
    alarm_pmu_regs(at, false, p->alm);  /* alarm_rearm()'s image */
    p->rtcwak = true;
}

/* powermgmt.c power_thread_rtc_process(): upstream disarms on the flag;
 * the shell (H2) reads INT1 only */
static void power_thread_step(struct pmu *p, bool shell)
{
    bool flag = p->int1;

    p->int1 = false;                    /* the read clears it */
    if (flag && !shell)
    {
        p->alm[6] = 0x99;               /* rtc_enable_alarm(false) */
        p->rtcwak = false;
    }
}

static void disarm_race(void)
{
    struct alarm a = mk(AR_DAILY, 7, 0);
    struct pmu p, up;
    uint8_t want[7];
    uint32_t next;
    int shell;

    for (shell = 0; shell < 2; shell++)
    {
        struct pmu *q = shell ? &p : &up;
        memset(q, 0, sizeof(*q));
        a = mk(AR_DAILY, 7, 0);
        pmu_arm(q, alarm_next(&a, 1, FRI_0659_59, NULL));
        q->int1 = true;                 /* 07:00: the PMU flags the match */
        /* the ring (UI thread) and the power thread interleave: the
         * snooze is armed first, the power thread's step comes after */
        alarm_after_ring(&a, FRI_0700, true);
        next = alarm_next(&a, 1, FRI_0700, NULL);
        EQ(next, FRI_0709);
        pmu_arm(q, next);
        power_thread_step(q, shell);
        alarm_pmu_regs(FRI_0709, false, want);
        if (shell)
        {
            /* hold PLAY, power off: the PMU still holds the snooze */
            CHECK(memcmp(q->alm, want, 7) == 0 && q->rtcwak);
        }
        else
        {
            /* upstream's race, kept as the reason for H2 */
            CHECK(q->alm[6] == 0x99 && !q->rtcwak);
        }
    }
}

/* H6: a power-off before alarm_init() (rtc_init(), the boot guard) */
static void keep_alive(void)
{
    uint8_t rtc[7], alm[7];
    static const uint8_t tail[4] = RTC_ALARM_DAILY_TAIL;

    alarm_pmu_regs(FRI_0659_59, false, rtc);    /* the clock's layout */
    rtc[3] = 5;
    /* an exact alarm still ahead: keep the wake */
    alarm_pmu_regs(FRI_0700, false, alm);
    EQ(rtc_alarm_keep(rtc, alm), RTC_KEEP_WAKE);
    /* the boot guard refuses at 07:00:30: just fired, converted to the
     * daily match of the same time (a repeat rings tomorrow) */
    alarm_pmu_regs(FRI_0700 + 30, false, rtc);
    EQ(rtc_alarm_keep(rtc, alm), RTC_KEEP_DAILY);
    alarm_pmu_regs(FRI_0700, false, rtc);       /* exactly now */
    EQ(rtc_alarm_keep(rtc, alm), RTC_KEEP_DAILY);
    /* a daily match: always ahead */
    alarm_pmu_regs(FRI_0700, true, alm);
    CHECK(memcmp(alm + 3, tail, 4) == 0);
    EQ(rtc_alarm_keep(rtc, alm), RTC_KEEP_WAKE);
    /* disarmed (year 0x99, ours and upstream's): nothing */
    alarm_pmu_regs(SAT_0700, false, alm);
    alm[6] = 0x99;
    EQ(rtc_alarm_keep(rtc, alm), RTC_KEEP_NONE);
    /* garbage (never armed by us): nothing */
    memset(alm, 0xee, 7);
    EQ(rtc_alarm_keep(rtc, alm), RTC_KEEP_NONE);
    alarm_pmu_regs(SAT_0700, false, alm);
    alm[5] = 0x13;                              /* month 13 */
    EQ(rtc_alarm_keep(rtc, alm), RTC_KEEP_NONE);
    /* the year, then the month, decide before the day and time */
    alarm_pmu_regs(JAN01_0030, false, alm);     /* 2027 */
    alarm_pmu_regs(DEC31_2359, false, rtc);     /* 2026 */
    EQ(rtc_alarm_keep(rtc, alm), RTC_KEEP_WAKE);
    EQ(rtc_alarm_keep(alm, rtc), RTC_KEEP_DAILY);
}

/* 5.5: a snoozed alarm survives a power-off: alarms.dat keeps
 * snooze_until and armed_rtc, and the boot at the snooze rings it */
static void snooze_poweroff(void)
{
    static struct alarms_file f, g;
    uint32_t armed;
    int w = -1;

    memset(&f, 0, sizeof(f));
    f.a[0] = mk(AR_WEEKDAYS, 7, 0);
    alarm_after_ring(&f.a[0], FRI_0700, true);
    armed = alarm_next(f.a, 1, FRI_0700, &w);
    EQ(armed, FRI_0709);
    f.armed_rtc = armed;
    alarms_file_seal(&f, 1);
    g = f;                              /* off, then the boot reads it */
    EQ(alarms_file_check(&g, sizeof(g)), ALARMS_OK);
    CHECK(alarm_is_boot(g.armed_rtc, FRI_0709 + 40));
    EQ(alarm_next(g.a, 1, g.armed_rtc - 1, NULL), FRI_0709);
    /* a boot 3 min late is not an alarm boot: missed, inline */
    CHECK(!alarm_is_boot(g.armed_rtc, FRI_0709 + 180));
    alarm_mark_missed(g.a, 1, g.armed_rtc, FRI_0709 + 180);
    CHECK(g.a[0].missed && g.a[0].missed_at == FRI_0709
          && g.a[0].snooze_until == 0 && g.a[0].on);
    EQ(alarm_next(g.a, 1, FRI_0709 + 180, NULL), MON_0700);
}

/* 5.5: missed, then the next repeat is armed at the same boot; a daily
 * alarm off for three days is one miss, not three */
static void missed_repeat(void)
{
    struct alarm a[2];

    a[0] = mk(AR_DAILY, 7, 0);
    a[1] = mk(AR_WEEKENDS, 8, 0);
    alarm_mark_missed(a, 2, FRI_0700, MON_0700 + 3600);
    CHECK(a[0].missed && a[0].missed_at == FRI_0700);
    CHECK(a[1].missed && a[1].missed_at == SAT_0700 + 3600);
    EQ(alarm_next(a, 2, MON_0700 + 3600, NULL), MON_0700 + 86400);
    /* booted before the armed time: nothing missed */
    a[0] = mk(AR_DAILY, 7, 0);
    alarm_mark_missed(a, 1, FRI_0700, FRI_0659_59);
    CHECK(!a[0].missed);
    /* the cap's miss is the engine's; a ring clears the mark */
    a[0].missed = 1;
    alarm_after_ring(&a[0], SAT_0700, false);
    CHECK(!a[0].missed && a[0].on);
}

/* the clock set backwards and forwards, a zone change: alarm_rearm()
 * recomputes from the new local time; nothing rings for the jump */
static void clock_moves(void)
{
    struct alarm a = mk(AR_DAILY, 7, 0);

    EQ(alarm_next(&a, 1, FRI_0800, NULL), SAT_0700);
    EQ(alarm_next(&a, 1, FRI_0800 - 2 * 3600, NULL), FRI_0700);   /* back */
    EQ(alarm_next(&a, 1, FRI_0659_59 + 3 * 3600, NULL), SAT_0700); /* fwd */
    /* a zone 5 h west: the RTC's local time moves back 5 h */
    EQ(alarm_next(&a, 1, FRI_0900 - 5 * 3600, NULL), FRI_0700);
    /* AR_DAYS with every day = daily; an invalid one-shot date: none */
    a.repeat = AR_DAYS; a.days = 0x7f;
    EQ(alarm_next(&a, 1, FRI_0800, NULL), SAT_0700);
    a = mk(AR_ONCE, 7, 0);
    a.year = 2026; a.mon = 2; a.mday = 30;
    EQ(alarm_next(&a, 1, FRI_0800, NULL), 0);
    a = mk(AR_DAILY, 24, 0);
    EQ(alarm_next(&a, 1, FRI_0800, NULL), 0);
}

static void file_more(void)
{
    static struct alarms_file f;

    memset(&f, 0, sizeof(f));
    f.a[0] = mk(AR_DAILY, 6, 0);
    f.a[1] = mk(AR_DAILY, 7, 0);
    alarms_file_seal(&f, 2);
    /* cut inside the last record */
    EQ(alarms_file_check(&f, 16 + 2 * 24 - 1), ALARMS_ESHORT);
    EQ(alarms_file_check(&f, 16 + 2 * 24), ALARMS_OK);
    /* a zero file (a cut before the first write): not ours */
    memset(&f, 0, sizeof(f));
    EQ(alarms_file_check(&f, sizeof(f)), ALARMS_EMAGIC);
    /* the empty set is a good file */
    alarms_file_seal(&f, 0);
    EQ(alarms_file_check(&f, 16), ALARMS_OK);
}

int main(void)
{
    calendar();
    next();
    ring();
    boot();
    missed();
    pmu();
    file();
    disarm_race();
    keep_alive();
    snooze_poweroff();
    missed_repeat();
    clock_moves();
    file_more();
    if (fails)
        return 1;
    printf("ok   alarm rules (next ring, repeat modes, month/year/leap, "
           "snooze cap, boot window, missed, PMU image, alarms.dat; disarm race, "
           "keep-alive, snooze across power-off, missed then repeat, clock "
           "moves, cut file)\n");
    return 0;
}
