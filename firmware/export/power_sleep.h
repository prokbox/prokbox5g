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

#ifndef _POWER_SLEEP_H_
#define _POWER_SLEEP_H_

/* Soft sleep (Phase 18-1, docs/phase18-brief.md §3.1): RAM kept, kernel
 * frozen, both cores asleep. Called on the UI thread only (the main
 * thread, 8 KB stack in IRAM), after the shell's quiesce: screen dark,
 * codec in standby, drive idle, every button released (or PLAY alone,
 * still held from the hold that started the sleep), no cpu_boost and no
 * PLL hold. Returns when a wake source fires or a deadline ends.
 * current_tick does NOT advance while asleep: thread timeouts resume as
 * if no time passed; wall time comes from the RTC.
 *
 * The pure half (the wheel packet check, the classifier, the poll
 * deadline) is sleep_wake_core.h, shared with the simulator's stub. */

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "sleep_wake_core.h"    /* enum sleep_wake, enum wheel_pkt */

/* Developer-only (the Sleep lab, D2): which timer ends a poll, and a
 * record of every poll so the device log shows whether it woke on time */
#define SLEEP_LAB_TIMER2    0   /* Timer 2, one-shot (the default) */
#define SLEEP_LAB_WAITCNT   1   /* PROC_WAIT_CNT, seconds unit [hypothesis] */
#define SLEEP_LAB_POLLS     16
struct sleep_lab_poll {
    uint16_t asked_s;           /* the poll's programmed length */
    uint16_t rtc_s;             /* RTC seconds it really took */
    uint32_t us;                /* USEC_TIMER over the same poll: about
                                 * asked_s * 1e6 if it counts asleep */
};
struct sleep_lab {
    uint8_t timer;              /* SLEEP_LAB_* */
    bool    opto_kept;          /* H2's fallback: the wheel controller
                                 * powered through the whole sleep */
    uint8_t n;                  /* polls recorded (at most SLEEP_LAB_POLLS) */
    struct sleep_lab_poll poll[SLEEP_LAB_POLLS];
};

struct sleep_req {
    uint32_t deep_after_s;    /* 0 = none (power_sleep() still caps a
                               * sleep at POWER_SLEEP_MAX_S) */
    uint32_t alarm_rtc;       /* seconds since 2000, 0 = none */
    uint32_t bcm_cut_after_s; /* 0 = the BCM is already cut */
    uint32_t hold_wake_s;     /* Q12: Hold-off wakes while the time since
                               * entry is under this (SLEEP_HOLD_WAKE_S);
                               * 0 = never */
    uint16_t low_mv;          /* 0 = no battery check; wake at mv <= low_mv */
    uint16_t poll_s;          /* timer-wake period; 0 = SLEEP_POLL_S */
    uint8_t  buttons_at_entry;/* wheel button bits (>> 8): 0, or PLAY
                               * still held from the hold (Rev 4, N6) */
    bool     wake_on_hold;    /* developer: Hold-off always wakes */
    struct sleep_lab *lab;    /* developer: NULL */
};

/* Why power_sleep() did not sleep (sleep_result.refused) */
#define SLEEP_REFUSE_NONE   0
#define SLEEP_REFUSE_CORE   1   /* not on the CPU */
#define SLEEP_REFUSE_BOOST  2   /* a cpu_boost count or a PLL hold */
#define SLEEP_REFUSE_CLOCK  3   /* not at 24 MHz with the PLL off */
#define SLEEP_REFUSE_PCM    4   /* PCM still running */
#define SLEEP_REFUSE_KEYS   5   /* a button other than PLAY held */
#define SLEEP_REFUSE_COP    6   /* the COP did not reach its idle sleep */
#define SLEEP_REFUSE_TIMER2 7   /* Timer 2 in use */
#define SLEEP_REFUSE_RTC    8   /* the raw RTC read failed at entry */

/* The refusal's name for logs and the Sleep lab's screen */
static inline const char *sleep_refuse_name(unsigned int r)
{
    static const char * const names[] = {
        "NONE", "CORE", "BOOST", "CLOCK", "PCM", "KEYS", "COP", "TIMER2",
        "RTC",
    };
    return r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}

#define SLEEP_CRC_UNCHECKED 2   /* sdram_crc_ok when no guard region ran */

struct sleep_result {
    enum sleep_wake why;
    uint32_t slept_s;       /* RTC difference */
    uint16_t timer_wakes;   /* poll wakes taken */
    uint16_t spurious;      /* interrupts that were no wake source */
    uint16_t resyncs;       /* out-of-step packets resynchronised */
    uint8_t  wake_buttons;  /* WAKE_BUTTON: the pressed bits (>> 8) */
    uint8_t  hold_edges;    /* Hold changes seen while asleep */
    uint16_t last_mv;       /* last raw battery reading, 0 = none */
    uint8_t  refused;       /* SLEEP_REFUSE_*: why = WAKE_DEEP, no sleep */
    uint32_t sdram_crc_ok;  /* 1/0 after a guard-region check, else
                             * SLEEP_CRC_UNCHECKED */
};

/* A soft sleep never outlasts this, whatever deep_after_s says: a
 * request with no deep timer still ends in WAKE_DEEP */
#define POWER_SLEEP_MAX_S   (72 * 3600)

#ifdef HAVE_POWER_SLEEP
static inline bool power_sleep_supported(void) { return true; }
void power_sleep(const struct sleep_req *rq, struct sleep_result *out);

/* Kernel-free PMU access for power_sleep() only (interrupts masked, the
 * normal I2C driver idle and its mutex held by the caller). Register-level
 * I2C (the registers i2c-pp.c uses), at most 4 bytes per transaction
 * (quirk 22), every wait bounded by USEC_TIMER. False on a timeout.
 * pmu_raw-pp502x.c. */
bool pmu_raw_battery_mv(uint16_t *mv);  /* ADC ch. 2; raw x 6000 >> 10 */
bool pmu_raw_standby(void);            /* pcf50605_standby_mode(), no
                                         * mutex: the panic exit's */
#ifdef HAVE_PP_GOV_DVS
bool pmu_raw_core_voltage(uint8_t code); /* DCDC1 0xEC/0xEB, read back
                                         * (the band governor's reboot
                                         * and fallback raise) */
#endif
bool pmu_raw_rtc_seconds(uint32_t *s);  /* RTC 0x0A-0x10 (4 + 3 bytes),
                                         * seconds since 2000 */

/* button-clickwheel.c: the wheel controller's bring-up (opto_i2c_init()),
 * and the driver's state after a sleep: wheel position forgotten (no
 * phantom scroll), buttons as the last packet had them, the Hold
 * tracking synced to the switch; buttons held into the sleep read
 * released for the first tick (sleep_exit_first_buttons()) (the driver's next tick sees no change);
 * backlight_hold_changed() once if Hold moved while asleep (only the
 * shell's Hold event, no BACKLIGHT_ON, when light is false) */
void clickwheel_sleep_opto_init(void);
void clickwheel_sleep_exit(unsigned int wheel_buttons,
                           unsigned int entry_buttons, bool light);

/* Frozen-kernel takeovers (brief §3.2, Revision 4 N1), for the shell's
 * wake path: */
void lcd_wake_aged(void);           /* lcd-video.c: the BCM has been off
                                     * >= 50 ms (slept_s >= 1) */
void backlight_sleep_hold(bool hold);/* backlight.c: no LCD-sleep countdown
                                     * while held */
void adc_invalidate(void);          /* adc-ipod-pcf.c: next reads fresh */
void battery_wake_reading(int mv);  /* powermgmt.c: the filter from the
                                     * sleep's raw reading (0 = read now) */
#else
static inline bool power_sleep_supported(void) { return false; }
#endif

#endif /* _POWER_SLEEP_H_ */
