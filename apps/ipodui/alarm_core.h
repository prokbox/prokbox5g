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
 * iPod UI shell: alarms, pure half (P12 contract, docs/phase12-brief.md 3.3)
 * Host-tested in tests/alarm; owner W1-A1
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
#ifndef IPODUI_ALARM_CORE_H
#define IPODUI_ALARM_CORE_H

/* Times are RTC seconds since 2000-01-01 00:00 LOCAL time (the RTC holds
 * local time: a zone change shifts it, datetime.c). 2000-01-01 was a
 * Saturday. Nothing here touches the PMU, a file or the clock: the
 * engine (alarm.h) does, with these as its rules. */

#include <stdint.h>
#include <stdbool.h>

#define ALARM_MAX        8
#define ALARM_SNOOZE_S   (9*60)         /* Q4 */
#define ALARM_SNOOZE_MAX 3              /* Q4, rev 2 */
#define ALARM_RING_S     (5*60)         /* Q5 */
#define ALARM_BOOT_WIN_S 120            /* an RTC power-on this close after
                                           the armed time is an alarm boot */
#define ALARM_HOLD_OFF_S (24*3600)      /* Q1(2): no deep power-off while an
                                           alarm is due within this */

enum alarm_repeat { AR_ONCE, AR_DAILY, AR_WEEKDAYS, AR_WEEKENDS, AR_DAYS,
                    AR__N };
#define ALARM_DAYS_WEEKDAYS 0x3e        /* Mon..Fri (bit 0 = Sunday) */
#define ALARM_DAYS_WEEKENDS 0x41        /* Sun, Sat */
#define ALARM_SOUND_BEEP    0           /* Q3: the piezo (= haptic) only */

struct alarm {
    uint8_t  on;
    uint8_t  repeat;                    /* enum alarm_repeat */
    uint8_t  days;                      /* AR_DAYS: bit 0 = Sunday .. 6 */
    uint8_t  snooze;                    /* 0/1: Snooze allowed */
    uint8_t  hour, min;
    uint8_t  label;                     /* index into the fixed label list
                                           (LANG_IPODUI_X_LABEL_*) */
    uint8_t  sound;                     /* ALARM_SOUND_BEEP (Q3) */
    uint16_t year; uint8_t mon, mday;   /* AR_ONCE: the date (mon 1..12) */
    uint8_t  snoozes;                   /* taken in this ring, <= MAX */
    uint8_t  missed;                    /* Q17: shown inline until seen */
    uint32_t snooze_until;              /* RTC s since 2000; 0 = none */
    uint32_t missed_at;                 /* RTC s of the missed ring */
};
/* The brief counted 20 B; natural alignment makes it 24 B (14 B of
 * bytes, then two aligned words). The file and the resident copy use
 * this one layout. */
_Static_assert(sizeof(struct alarm) == 24, "struct alarm layout");

/* M9 (rev 2): alarms.dat = this header + ALARM_MAX records, one safefile
 * write (alarms: at once, they must survive a power cut). */
#define ALARMS_MAGIC   0x4d4c4150u      /* "PALM" */
#define ALARMS_VERSION 1
struct alarms_file {
    uint32_t magic; uint16_t version, count;
    uint32_t armed_rtc;                 /* what the PMU was last armed for
                                           (0 = disabled): read at boot to
                                           decide an alarm boot and a miss */
    uint32_t crc;                       /* crc_32(a, count * 24, ~0):
                                           firmware/common/crc32.c */
    struct alarm a[ALARM_MAX];
};
_Static_assert(sizeof(struct alarms_file) == 16 + ALARM_MAX * 24,
               "alarms.dat layout");

/* The next ring time strictly after 'now' over every enabled alarm and
 * pending snooze; 0 = none. *which = its index (may be NULL). An AR_ONCE
 * alarm whose date and time are not after 'now' yields nothing. Two
 * alarms at the same minute: the lower index. */
uint32_t alarm_next(const struct alarm *a, int n, uint32_t now, int *which);
/* The state after a ring at 'now': snoozed (and a snooze left):
 * snooze_until = now + ALARM_SNOOZE_S, snoozes + 1 (up to
 * ALARM_SNOOZE_MAX; a refused snooze dismisses). Dismissed: snooze_until
 * and snoozes cleared, and AR_ONCE switches off. */
void alarm_after_ring(struct alarm *a, uint32_t now, bool snoozed);
/* Was this start an alarm power-on? armed != 0 and
 * 0 <= now - armed <= ALARM_BOOT_WIN_S. */
bool alarm_is_boot(uint32_t armed, uint32_t now);
/* Q17: every enabled alarm whose first occurrence (or snooze) at or
 * after 'armed' lies in [armed, now] gets missed = 1 and missed_at = that
 * time (armed is the occurrence the PMU was armed for and never rang:
 * alarm_rearm() moves it on after every ring). AR_ONCE ones switch off,
 * repeating ones keep their schedule (the caller arms the next
 * occurrence with alarm_next()); a pending snooze is cleared. armed = 0:
 * nothing. An alarm boot that will ring (alarm_is_boot()) is not a miss:
 * the engine rings it instead of calling this for it. */
void alarm_mark_missed(struct alarm *a, int n, uint32_t armed,
                       uint32_t now);
/* The PCF50605 alarm register image for 'armed', in register order
 * 0x11..0x17: sec, min, hour (BCD), weekday, mday, month, year. The
 * weekday register is always at its ignore value (ALARM_REG_IGNORE[3]);
 * daily = true puts mday/month/year at their ignore values too (an
 * h:m:s match every day), else the exact date (one-shot; D-A3). */
void alarm_pmu_regs(uint32_t armed, bool daily, uint8_t regs[7]);
/* the "ignore this field" values (rtc_pcf50605.c alarm_disable[]) */
#define ALARM_REG_IGNORE { 0x7f, 0x7f, 0x3f, 0x07, 0x3f, 0x1f, 0xff }
#define ALARM_REG_YEAR_OFF 0x99         /* upstream's "disabled" year */

/* alarms.dat: seal sets magic, version, count and crc; check returns 0
 * for a good image of 'bytes' bytes, or <0: ALARMS_ESHORT, EMAGIC
 * (magic or version), ECRC, ECOUNT. A bad file reads as "no alarms"
 * (logged; rewritten from the empty state at the next edit). */
enum { ALARMS_OK = 0, ALARMS_ESHORT = -1, ALARMS_EMAGIC = -2,
       ALARMS_ECRC = -3, ALARMS_ECOUNT = -4 };
void alarms_file_seal(struct alarms_file *f, int count);
int  alarms_file_check(const struct alarms_file *f, uint32_t bytes);

/* Calendar helpers on the same time base, for the engine and the
 * screens: (y, mon 1..12, mday, h, m, s) <-> seconds since 2000, and
 * the weekday (0 = Sunday). Years 2000..2099. */
uint32_t alarm_rtc_make(int year, int mon, int mday, int h, int m, int s);
void     alarm_rtc_split(uint32_t t, int *year, int *mon, int *mday,
                         int *h, int *m, int *s);
int      alarm_rtc_wday(uint32_t t);

#endif /* IPODUI_ALARM_CORE_H */
