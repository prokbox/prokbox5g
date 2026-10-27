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

#ifndef _SLEEP_WAKE_CORE_H_
#define _SLEEP_WAKE_CORE_H_

/* Phase 18-1, the pure half of the platform's soft sleep (brief §3.1):
 * the wheel-packet check, the wake classifier and the poll deadline.
 * No kernel, hardware or Rockbox calls: power_sleep() feeds it what it
 * read from the registers and the raw PMU path, and the sim stub feeds it
 * script events, so both decide wakes by the same code. Host test:
 * tests/sleep/. In firmware/ so power_sleep-pp502x.c never reaches into
 * apps/ (Rev 4a); it includes only <stdbool.h> and <stdint.h>. */

#include <stdbool.h>
#include <stdint.h>

#define SLEEP_POLL_S        60      /* timer-wake period (brief §3.1) */
#define SLEEP_SPURIOUS_MAX  1000    /* per hour of RTC time: interrupts with
                                     * no valid packet (no packet ready,
                                     * 0x3A, 0xFFFFFFFF, out of step) or a
                                     * GPIO with no change; over it is the
                                     * failure rule, WAKE_DEEP. Valid
                                     * packets without a press edge
                                     * (touch, turn, release, a press
                                     * under Hold) never count (Rev 4a) */
#define SLEEP_SPURIOUS_WIN_S 3600
#define SLEEP_QUIET_PKTS_MAX 64     /* valid packets with no press edge in
                                     * one poll interval (a finger resting
                                     * on the wheel streams them); at this
                                     * count the wheel's source is masked
                                     * until the next poll (SACT_WHEEL_MUTE):
                                     * bounds the time awake without ever
                                     * counting toward the power-off cap */
#define SLEEP_HOLD_WAKE_S   600     /* Q12: Hold-off wakes only this long
                                     * after sleep entry, as the 7G does.
                                     * The owner's value, 2026-10-22 (10
                                     * min), to revisit after the final
                                     * release */

/* The wheel packet (pp5022-reference/input.md §3) */
#define WHEEL_PKT_MASK      0x800000FFu
#define WHEEL_PKT_BUTTONS   0x8000001Au
#define WHEEL_PKT_TYPE_3A   0x3Au
#define WHEEL_PKT_ALL_ONES  0xFFFFFFFFu
#define WHEEL_BTN_SHIFT     8
#define WHEEL_BTN_BITS      0x1Fu   /* bits 8-12 >> 8 */
/* The button bits after >> 8 */
#define WBTN_SELECT 0x01
#define WBTN_RIGHT  0x02
#define WBTN_LEFT   0x04
#define WBTN_PLAY   0x08
#define WBTN_MENU   0x10

#ifndef POWER_SLEEP_ENUMS
#define POWER_SLEEP_ENUMS
/* As firmware/export/power_sleep.h (contract, §3.1); the values are the
 * trace's why=%d. */
enum sleep_wake {
    WAKE_NONE = 0,
    WAKE_BUTTON,        /* 1: a valid packet with a button press edge */
    WAKE_VBUS,          /* 2: USB or charger inserted */
    WAKE_ALARM,         /* 3: the RTC reached alarm_rtc */
    WAKE_DEEP,          /* 4: deep_after_s elapsed, or the failure rule */
    WAKE_LOWBATT,       /* 5: a timer-wake battery read under low_mv */
    WAKE_HOLD,          /* 6: Hold slid off (Q12 window, or wake_on_hold) */
    WAKE_BCM_CUT,       /* 7: bcm_cut_after_s elapsed */
};
enum wheel_pkt { PKT_NONE, PKT_VALID, PKT_IGNORED, PKT_OUT_OF_STEP };
#endif

/* The request fields the classifier reads: struct sleep_req (§3.1) plus
 * hold_wake_s (Q12, owner 2026-10-22). 0 disables a term. */
struct sleep_core_req {
    uint32_t deep_after_s;
    uint32_t alarm_rtc;         /* seconds since 2000 */
    uint32_t bcm_cut_after_s;
    uint32_t hold_wake_s;       /* Q12: Hold-off wakes while the time
                                 * since entry is under this; 0 = never.
                                 * power_sleep() reads the RTC by the raw
                                 * path at every Hold edge (Rev 4a) */
    uint16_t low_mv;            /* WAKE_LOWBATT at mv <= low_mv (Q6:
                                 * "reaches", Rev 4a); 0 = no check */
    uint16_t poll_s;            /* 0 = SLEEP_POLL_S */
    uint8_t  buttons_at_entry;  /* 0, or PLAY still held from the hold
                                 * that started the sleep (Rev 4 N6) */
    bool     wake_on_hold;      /* developer: Hold-off always wakes */
};

/* Interrupt sources of one wake from PROC_SLEEP (a bit set) */
#define SRC_WHEEL   0x01    /* high source 40 (wheel or I2C) */
#define SRC_GPIO    0x02    /* Hold (GPIOA bit 5) or VBUS (GPIOL bit 4) */
#define SRC_TIMER   0x04    /* the poll timer */

/* What power_sleep() read at one wake */
struct sleep_core_ev {
    uint8_t  src;           /* SRC_* */
    bool     pkt_ready;     /* WHEEL_STAT bit 26 (SRC_WHEEL only) */
    uint32_t pkt;           /* WHEEL_DATA when pkt_ready */
    bool     hold_on;       /* GPIOA bit 5 low, read at this wake */
    bool     vbus;          /* GPIOL bit 4, read at this wake */
    bool     rtc_ok;        /* rtc valid (read by the raw path; always on a
                             * timer wake and a Hold edge) */
    uint32_t rtc;           /* seconds since 2000 */
    bool     batt_ok;       /* mv valid */
    uint16_t mv;
    uint16_t poll_s;        /* SRC_TIMER: the poll's programmed length;
                             * their sum backs up the RTC for the deep
                             * timer (a frozen RTC never stalls it) */
};

/* Driver actions the caller owes after sleep_classify() (bits in act) */
#define SACT_OPTO_OFF   0x01    /* Hold went on: DEV_EN &= ~DEV_OPTO */
#define SACT_OPTO_INIT  0x02    /* Hold went off: full opto bring-up */
#define SACT_RESYNC     0x04    /* out-of-step packet: Rockbox's resync */
#define SACT_WHEEL_MUTE 0x08    /* SLEEP_QUIET_PKTS_MAX reached: mask the
                                 * wheel's source until the next poll,
                                 * which reads the buttons again */

struct sleep_core_state {
    const struct sleep_core_req *rq;
    uint32_t rtc_entry;
    uint32_t slept_s;       /* max(the last RTC read, timer_s) */
    uint32_t timer_s;       /* the polls' programmed lengths, summed */
    uint32_t win_start;     /* spurious window start (RTC) */
    uint16_t win_bad;       /* this hour's cap count (SLEEP_SPURIOUS_MAX) */
    uint16_t timer_wakes;
    uint16_t spurious;      /* whole sleep: every interrupt that was no
                             * wake (struct sleep_result's spurious) */
    uint16_t resyncs;
    uint16_t quiet_pkts;    /* this poll interval's no-edge packets */
    uint8_t  prev_buttons;
    uint8_t  wake_buttons;  /* WAKE_BUTTON: the newly pressed bits */
    uint8_t  hold_edges;
    uint8_t  act;           /* SACT_*, set by the last sleep_classify() */
    bool     hold_on;
    bool     vbus;
};

/* One wheel event against the previous button bits (§3.1). Sets
 * *press_edge to the newly pressed bits (0 = no wake) and updates *prev
 * on a valid packet. */
enum wheel_pkt sleep_wheel_packet(bool pkt_ready, uint32_t pkt,
                                  uint8_t *prev, uint8_t *press_edge);

/* Starts a sleep: the entry RTC and the Hold/VBUS levels at entry */
void sleep_core_begin(struct sleep_core_state *st,
                      const struct sleep_core_req *rq, uint32_t rtc_entry,
                      bool hold_on, bool vbus);

/* One wake from PROC_SLEEP: WAKE_NONE (sleep on) or the wake, chosen by
 * priority LOWBATT > VBUS > ALARM > BUTTON > HOLD > DEEP > BCM_CUT. A
 * timer wake without a good RTC read, or without a good battery read
 * while low_mv is set, is the failure rule (WAKE_DEEP). */
enum sleep_wake sleep_classify(struct sleep_core_state *st,
                               const struct sleep_core_ev *ev);

/* Seconds to program the next poll for: min(poll, alarm - now,
 * deep - slept, bcm_cut - slept), absent terms skipped; 0 = due now */
uint32_t sleep_poll_deadline(const struct sleep_core_req *rq,
                             uint32_t slept_s, uint32_t rtc_now);

/* The request for re-entering after WAKE_BCM_CUT: the BCM is cut, the
 * relative deadlines shortened by slept_s (an elapsed one stays due: 1) */
void sleep_req_remaining(const struct sleep_core_req *in, uint32_t slept_s,
                         struct sleep_core_req *out);

/* The buttons the driver is shown for its first tick after a sleep:
 * a button held into the sleep (entry, PLAY from the hold) reads
 * released for that tick even if the packet at the wake has it pressed
 * again, so button.c sees PLAY's release and then the waking press,
 * never one merged "still held" (wheel bits >> 8) */
static inline uint8_t sleep_exit_first_buttons(uint8_t entry, uint8_t now)
{
    return now & (uint8_t)~entry;
}

/* The PMU's RTC registers 0x0A-0x10 (BCD: sec, min, hour, weekday, day,
 * month, year from 2000; pp5022-reference/clocks-power.md §6) as seconds
 * since 2000-01-01 00:00. False on a value that is not BCD or out of
 * range (a bad raw read: the caller's failure rule). The weekday is not
 * used. */
bool sleep_rtc_from_bcd(const uint8_t r[7], uint32_t *s);
/* The same count from a calendar date (year 2000-2099, month 1-12) */
uint32_t sleep_rtc_from_date(int year, int mon, int day,
                             int hour, int min, int sec);

#endif /* _SLEEP_WAKE_CORE_H_ */
