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

#ifndef SIM_CMD_SLEEP_H
#define SIM_CMD_SLEEP_H

#include <stdbool.h>
#include <stdint.h>

/* Phase 18 soft sleep in the simulator (brief §3.3, §4.0 "Sim commands").
 * Handles these sim_script.c commands:
 *
 *   sleep_warp <s>     <s> seconds pass: the sim RTC (rtc_read_datetime,
 *                      a pinned sim_clock) moves on, and while asleep the
 *                      sleep sees them as polls at the device's deadlines
 *                      (min(poll, alarm, deep, bcm_cut)), so an alarm or
 *                      the deep timer wakes on time, not at the next poll
 *   rtc_set <s>        the RTC reads <s> seconds since 2000 from now on
 *   alarm_in <s>       an RTC alarm <s> seconds after the next sleep's
 *                      entry (0: none), for a request that has none,
 *                      standing in for P12's alarm until it exists;
 *                      cleared by its wake
 *   pll_hold <ms>      a PLL hold (or a boost) for <ms> from now: the
 *                      sleep refuses (power_sleep()'s checks, review H1)
 *                      until it ends; PCM still running refuses too
 *   power_cut          the battery dies now: the sim exits with no
 *                      shutdown path (follow it with "reboot")
 *   vbus <0|1>         a bare VBUS level (no USB host, no charger state);
 *                      "usb_connect" and "charger on" are VBUS too
 *   batt_mv <mv>|off   pins the battery voltage (the level follows the
 *                      sim's linear curve); "battery <pct>" still pins by
 *                      level
 *   wheel_touch        a finger on the wheel: no button, no turn (awake:
 *                      the driver's touch count, as before)
 *   idle_poweroff <min> the idle power-off timeout (upstream's setting,
 *                      set_poweroff_timeout(); Q4 routes it to sleep), and
 *                      the idle timer restarted
 *   sleep_mode soft|off|auto
 *                      the developer "Sleep: soft / off" row without the
 *                      menu: sim_sleep_mode 1 / 0 / -1 (auto: the row's
 *                      own value), for sleep.c to read in the sim
 *   expect_sleep <state> [ms]
 *   sleep_expect <state> [ms]
 *                      waits up to ms (default 3000) for <state>: asleep,
 *                      awake (not asleep), or a wake reason (button,
 *                      vbus, alarm, deep, lowbatt, hold, bcm_cut, or its
 *                      number) of the last wake since the last
 *                      expect_sleep; "expect_sleep failed" otherwise
 *
 * While asleep, "key", "hold" and "wheel" go to the sleep as wheel
 * packets (press and release separately, a turn with no button), and
 * "hold_switch" as the Hold level, instead of the button queue. The
 * sleep logs "sleep enter ..." and "sleep wake why=%d s=%lu polls=%u
 * spurious=%u resyncs=%u buttons=%#x holds=%u" (# log:). In a build
 * without the shell every command reports "unavailable".
 * Returns false for any other command. */
bool sim_cmd_sleep(const char *cmd, char *argv[], int argc, int lineno);

/* sim_script.c: true = the sleep took this key or wheel event */
bool sim_sleep_key(int sdl_key, bool down);
bool sim_sleep_wheel(int clicks);
bool sim_sleep_touch(void);     /* wheel_touch while asleep */

/* The sim RTC's offset in seconds (sleep_warp, rtc_set), for
 * firmware/target/hosted/rtc.c */
extern long sim_rtc_offset_s;
/* sleep_mode: 1 soft, 0 off (today's power-off), -1 the developer row */
extern int sim_sleep_mode;

#ifdef HAVE_IPOD_UI
#include "sleep_wake_core.h"

/* The sim's power_sleep() body (§3.3): blocks the calling (UI) thread
 * until a wake, deciding it with sleep_classify() as the device does.
 * The waking buttons still held at the wake are re-pressed into the
 * button driver, as clickwheel_sleep_exit() leaves them on the device,
 * so the shell's swallow mask sees their release. */
struct sim_sleep_result {
    enum sleep_wake why;
    uint32_t slept_s;
    uint16_t timer_wakes;
    uint16_t spurious;
    uint16_t resyncs;
    uint8_t  wake_buttons;
    uint8_t  hold_edges;
    /* from here as struct sleep_result (power_sleep.h, 18-1): the same
     * fields in the same order, so the shell fills one from the other */
    uint16_t last_mv;       /* the battery at the wake, 0 = none */
    uint8_t  refused;       /* SLEEP_REFUSE_*: why = WAKE_DEEP, no sleep */
    uint32_t sdram_crc_ok;  /* SLEEP_CRC_UNCHECKED (2) */
};
void sim_sleep_run(const struct sleep_core_req *rq,
                   struct sim_sleep_result *out);
/* power_sleep()'s refusal checks in the sim (review H1): PCM running
 * (the pause's fade), or a pll_hold standing; 0 or SLEEP_REFUSE_* */
int sim_sleep_refusal(void);
/* The sim RTC in seconds since 2000: the iPod's RTC holds local time,
 * so the host's clock plus its UTC offset (taken once, fixed: the RTC
 * keeps no DST) plus the scripted moves (sleep_warp, rtc_set, set_time).
 * One model for the hosted rtc.c, the PMU stub and the sleep's clock. */
uint32_t sim_sleep_rtc(void);
/* the RTC set to rtc (seconds since 2000, local time) */
void sim_rtc_set(uint32_t rtc);
/* the host's UTC offset in seconds, fixed at its first use */
long sim_rtc_utc_off(void);
#endif

#endif
