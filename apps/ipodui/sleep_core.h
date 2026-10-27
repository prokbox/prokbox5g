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

#ifndef _IPODUI_SLEEP_CORE_H_
#define _IPODUI_SLEEP_CORE_H_

/* Phase 18-2, the pure half of the shell's sleep policy (brief §0.1,
 * §3.2): the tier, the deep-sleep time, the low-battery rule, Q11's boot
 * equivalent and the waking press's swallow mask. No Rockbox calls; the
 * wrappers in sleep.c pass IPOD_BATTERY_MAH, battery_level_disksafe and
 * the developer switches. Host test: tests/sleep/. */

#include <stdbool.h>
#include <stdint.h>
#include "sleep_wake_core.h"

/* Q1: the longest soft sleep before a power-off (owner default 24 h, kept
 * after Q0: the 7G wakes instantly after a night) */
#define SLEEP_DEEP_AFTER_S  (24u * 3600u)
/* Q1: shortened to the time that costs this share of the capacity */
#define SLEEP_DEEP_PCT      5u
/* The soft-sleep current assumed until D3 measures it: Q5's budget on
 * the reference 3000 mAh cell (2 %/day = 2.5 mA). An absolute current,
 * so a smaller cell gets a shorter deep time by the same rule */
#define SLEEP_UA_DEFAULT    2500u
/* (Rev 4a, accepted: SLEEP_UA_DEFAULT stands until D3 measures the
 * current; the floor below is 1 h) */
/* Floor: never a deep time of 0 ("no deep timer" in the request) and
 * never a boot after a short sleep, whatever the current measures */
#define SLEEP_DEEP_MIN_S    3600u
/* Q6: power off below this level, or under disk-safe + this */
#define SLEEP_LOW_PCT       10
#define SLEEP_LOW_MARGIN_MV 100
/* Q11: a soft sleep at least this long counts as a boot toward P16's
 * forced walk */
#define SLEEP_BOOT_EQUIV_S  (6u * 3600u)
/* Q11 (Revision 4 N4; owner 2026-10-22): a library check is owed once
 * this long has passed since the last completed one, a schedule, not a
 * forced run (it waits for the next window that may run it) */
#define SLEEP_WALK_DAYS     7u
#define SLEEP_WALK_S        (SLEEP_WALK_DAYS * 86400u)

/* Q1/Q5: the deep time for a cell of mah at a sleep current of ua (0 =
 * SLEEP_UA_DEFAULT) and the stock timeout stock_s (0 = unknown) */
uint32_t sleep_deep_after_s(uint32_t mah, uint32_t ua, uint32_t stock_s);

/* Q6: the request's low_mv for a disk-safe level */
uint16_t sleep_low_mv(uint16_t disksafe_mv);
/* Q6: true = power off, never soft sleep. pct < 0: unknown; mv <= 0: no
 * reading. At or under low_mv ("reaches", Rev 4a), the same comparison
 * power_sleep() makes */
bool sleep_battery_low(int mv, int pct, uint16_t low_mv);

/* ui_sleep_tier() with what it needs (§3.2) */
struct sleep_tier_in {
    int  mv;
    int  pct;
    bool charger;           /* a charger (not a USB host) is present */
    bool usb;               /* a USB host is present */
    bool supported;         /* power_sleep_supported() */
    bool dev_off;           /* developer "Sleep: off" */
    bool dev_on_charger;    /* developer: soft sleep with a charger (D3) */
    uint16_t low_mv;
};
enum sleep_pick {
    SLEEP_PICK_SOFT,        /* TIER_SOFT */
    SLEEP_PICK_OFF,         /* TIER_OFF: today's sys_poweroff() */
    SLEEP_PICK_IGNORE,      /* no sleep: a USB host (the USB screen stays)
                             * or a charger (upstream's idle power-off is
                             * inhibited on a charger too; hold PLAY there
                             * shows the charging screen, Q9) */
};
enum sleep_pick sleep_tier_pick(const struct sleep_tier_in *in);

/* Q11: does this wake count as a boot? total_slept_s sums the calls of
 * one sleep (a WAKE_BCM_CUT re-entry continues it). WAKE_DEEP and
 * WAKE_LOWBATT never count: the full boot after them counts once */
bool sleep_counts_as_boot(enum sleep_wake why, uint32_t total_slept_s);

/* The sleep-entry window's jobs (§4.3 table), for the trace line
 * "sleep job=<name> owed|ran|cut" the scripts check */
enum sleep_job {
    SJ_COMMIT,          /* a pending or suspended commit */
    SJ_REBUILD,         /* a dirty database's rebuild */
    SJ_WALK,            /* the Q11 library check */
    SJ_CHAPTERS,        /* chapter books (art thread) */
    SJ_TOPUP,           /* the art top-up step */
    SJ_ROTATION,        /* #100's panel-set rotation (Q8) */
    SJ_COUNT,
};
const char *sleep_job_name(enum sleep_job j);    /* "?" if out of range */

/* Q11: is a check owed by the calendar? last_walk_rtc is the RTC of the
 * last completed walk, 0 = none recorded (owed). An RTC behind it (the
 * clock was set back) is not owed: never an underflow */
bool sleep_walk_owed(uint32_t now_rtc, uint32_t last_walk_rtc);

/* The waking press's swallow mask (Rev 3 H1), in WBTN_* bits */
struct sleep_swallow {
    uint8_t mask;
};
/* Arms it with the waking buttons still down (a button already released
 * while asleep owes no release) and every button held into the sleep
 * (buttons_at_entry, PLAY after the hold, Rev 4 N6): those are swallowed
 * until their release, which the driver delivers after the wake whether
 * it came while asleep or later (clickwheel_sleep_exit() syncs it; Rev
 * 4a) */
void sleep_swallow_arm(struct sleep_swallow *sw, uint8_t wake_buttons,
                       uint8_t held, uint8_t entry_buttons);
/* One input event of button bit: true = drop it. Every event of an armed
 * button (press, repeat, long press) is dropped up to and including its
 * release, which disarms it. */
bool sleep_swallow_event(struct sleep_swallow *sw, uint8_t bit,
                         bool release);

#endif /* _IPODUI_SLEEP_CORE_H_ */
