/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Heikki Hannikainen, Uwe Freese
 * Revisions copyright (C) 2005 by Gerald Van Baren
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
#include <ctype.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "xcore.h"      /* ProkBox P19 S0: XCORE_BSS */
#include "thread.h"
#include "debug.h"
#include "adc.h"
#include "string.h"
#include "storage.h"
#include "power.h"
#include "audio.h"
#include "usb.h"
#include "powermgmt.h"
#include "backlight.h"
#ifdef HAVE_IPOD_UI
#include "button.h"                 /* Phase 18: SYS_SLEEP_REQUEST */
#include "quiet_wait_core.h"       /* P19 19-1a: qw_power_wait() */
#include "devstats.h"               /* P19 19-1a: DEVSTATS_POLL */
#endif
#include "lcd.h"
#include "rtc.h"
#if CONFIG_TUNER
#include "fmradio.h"
#endif
#include "sound.h"
#include "font.h"
#include "logf.h"
#ifdef HAVE_REMOTE_LCD
#include "lcd-remote.h"
#endif
#if (CONFIG_PLATFORM & PLATFORM_HOSTED)
#include <time.h>
#endif
#ifdef HAVE_PLL_IDLE
#include "pll_idle.h"
#endif
#ifdef HAVE_PP_GOV
#include "pgov.h"           /* ProkBox: the band governor */
#endif
#ifdef HAVE_POWER_SLEEP
#include "power_sleep.h"
#endif

#if (defined(IAUDIO_X5) || defined(IAUDIO_M5) || defined(COWON_D2)) \
    && !defined (SIMULATOR)
#include "pcf50606.h"
#endif

#if (BATTERY_CAPACITY_DEFAULT > 0)
extern unsigned short power_history[POWER_HISTORY_LEN];
extern unsigned short battery_level_disksafe;
extern unsigned short battery_level_shutoff;
extern unsigned short percent_to_volt_discharge[11];
#endif
#if CONFIG_CHARGING
extern unsigned short percent_to_volt_charge[11];
#endif

#ifndef BOOTLOADER
#include "misc.h"
#include "splash.h"

struct battery_tables_t device_battery_tables =
{
#if (BATTERY_CAPACITY_DEFAULT > 0)
    .history = power_history,
    .disksafe = &battery_level_disksafe,
    .shutoff = &battery_level_shutoff,
    .discharge = percent_to_volt_discharge,
#if CONFIG_CHARGING
    .charge = percent_to_volt_charge,
#endif
    .elems = ARRAYLEN(percent_to_volt_discharge),
#endif
    .isdefault = true,
};
#endif

static int last_sent_battery_level = 100;
static void set_sleep_timer(int seconds);

static bool sleeptimer_active = false;
static long sleeptimer_endtick;
/* Whether an active sleep timer should be restarted when a key is pressed */
static bool sleeptimer_key_restarts = false;
/* The number of seconds the sleep timer was last set to */
static unsigned int sleeptimer_duration = 0;

#if CONFIG_CHARGING
/* State of the charger input as seen by the power thread */
enum charger_input_state_type charger_input_state;
/* Power inputs as seen by the power thread */
unsigned int power_thread_inputs;
#if CONFIG_CHARGING >= CHARGING_MONITOR
/* Charging state (mode) as seen by the power thread */
enum charge_state_type charge_state = DISCHARGING;
#endif
#endif /* CONFIG_CHARGING */

static int shutdown_timeout = 0;

void handle_auto_poweroff(void);
static int poweroff_timeout = 0;
static long last_event_tick = 0;

#if BATTERY_CAPACITY_INC > 0
static int battery_capacity = BATTERY_CAPACITY_DEFAULT;
#else
# define battery_capacity BATTERY_CAPACITY_DEFAULT
#endif

/* Power history: power_history[0] is the newest sample */
unsigned short power_history[POWER_HISTORY_LEN] = {0};

#if (CONFIG_CPU == JZ4732) || (CONFIG_CPU == JZ4760B) || \
    (CONFIG_CPU == X1000) || (CONFIG_PLATFORM & PLATFORM_HOSTED)
static long power_stack[DEFAULT_STACK_SIZE/sizeof(long)];
#else
static long power_stack[DEFAULT_STACK_SIZE/2/sizeof(long)];
#endif
static const char power_thread_name[] = "power";

/* Time estimation requires 64 bit math so don't use it in the bootloader.
 * Also we need to be able to measure current, and not have a better time
 * estimate source available. */
#if (!defined(BOOTLOADER) && !(CONFIG_BATTERY_MEASURE & TIME_MEASURE) && \
     (defined(CURRENT_NORMAL) || (CONFIG_BATTERY_MEASURE & CURRENT_MEASURE)))
#define HAVE_TIME_ESTIMATION
#endif

#if !(CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE)
int _battery_level(void) { return -1; }
#endif
static int percent_now; /* Cached to avoid polling too often */

#if !(CONFIG_BATTERY_MEASURE & TIME_MEASURE)
int _battery_time(void) { return -1; }
#else
static int time_now; /* Cached to avoid polling too often */
#endif

#ifdef HAVE_TIME_ESTIMATION
static int time_now;     /* reported time in minutes */
static int64_t time_cnt; /* reported time in seconds */
static int64_t time_err; /* error... it's complicated */
#endif

#if !(CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE)
int _battery_voltage(void) { return -1; }
#else
/* Data for the digital exponential filter */
static int voltage_avg, voltage_now;
#endif

#if !(CONFIG_BATTERY_MEASURE & CURRENT_MEASURE)
int _battery_current(void) { return -1; }
#else
static int current_avg, current_now;
#endif

/* The battery level can be obtained in two ways. If the target reports
 * voltage, the battery level can be estminated using percent_to_volt_*
 * curves. If the target can report the percentage directly, then that
 * will be used instead of voltage-based estimation. */
int battery_level(void)
{
#ifdef HAVE_BATTERY_SWITCH
    if ((power_input_status() & POWER_INPUT_BATTERY) == 0)
        return -1;
#endif

    return percent_now;
}

/* The time remaining to full charge/discharge can be provided by the
 * target if it has an accurate way of doing this. Otherwise, if the
 * target defines a valid battery capacity and can report the charging
 * and discharging current, the time remaining will be estimated based
 * on the battery level and the actual current usage. */
int battery_time(void)
{
#if (CONFIG_BATTERY_MEASURE & TIME_MEASURE) || defined(HAVE_TIME_ESTIMATION)
    return time_now;
#else
    return -1;
#endif
}

/* Battery voltage should always be reported if available, but it is
 * optional if the the target reports battery percentage directly. */
int battery_voltage(void)
{
#if CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE
    return voltage_now;
#else
    return -1;
#endif
}

/* Battery current can be estimated if the target defines CURRENT_NORMAL
 * as the number of milliamps usually consumed by the device in a normal
 * state. The target can also define other CURRENT_* values to estimate
 * the power consumed by the backlight, remote display, SPDIF, etc. */
int battery_current(void)
{
#if CONFIG_BATTERY_MEASURE & CURRENT_MEASURE
    return current_now;
#elif defined(CURRENT_NORMAL)
    int current = CURRENT_NORMAL;

#ifndef BOOTLOADER
#if defined(HAVE_BACKLIGHT) && defined(CURRENT_BACKLIGHT)
    if (backlight_get_current_timeout() == 0) /* LED always on */
        current += CURRENT_BACKLIGHT;
#endif

#if defined(HAVE_RECORDING) && defined(CURRENT_RECORD)
    if (audio_status() & AUDIO_STATUS_RECORD)
        current += CURRENT_RECORD;
#endif

#if defined(HAVE_SPDIF_POWER) && defined(CURRENT_SPDIF_OUT)
    if (spdif_powered())
        current += CURRENT_SPDIF_OUT;
#endif

#if defined(HAVE_REMOTE_LCD) && defined(CURRENT_REMOTE)
    if (remote_detect())
        current += CURRENT_REMOTE;
#endif

#if defined(HAVE_ATA_POWER_OFF) && defined(CURRENT_ATA)
    if (ide_powered())
        current += CURRENT_ATA;
#endif

#if CONFIG_CHARGING >= CHARGING_MONITOR
    /* While charging we must report the charging current. */
    if (charging_state()) {
        current = CURRENT_MAX_CHG - current;
        current = MIN(current, 1);
    }
#endif

#endif /* BOOTLOADER */

    return current;
#else
    return -1;
#endif
}

#ifdef HAVE_IPOD_UI
static int seed_mv;     /* ProkBox: the boot guard's resting reading */
void powermgmt_seed_voltage(int mv)
{
    seed_mv = mv;
}
#endif

/* Initialize the battery voltage/current filters. This is called
 * once by the power thread before entering the main polling loop. */
static void average_init(void)
{
#if CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE
#ifdef HAVE_IPOD_UI
    /* ProkBox: seed from the boot guard's resting reading (taken before
     * any boot load, apps/ipodui/bootbatt.c), else the raw reading.
     * Upstream starts 15 mV (flash) or 35 mV (disk) above a reading taken
     * under the boot load; the guard's sample needs no such guess. */
    voltage_now = seed_mv > 0 ? seed_mv : _battery_voltage();
#else
    voltage_now = _battery_voltage() + 15;

    /* The battery voltage is usually a little lower directly after
       turning on, because the disk was used heavily. Raise it by 5% */
#ifdef HAVE_DISK_STORAGE
#if CONFIG_CHARGING
    if(!charger_inserted())
#endif
    {
        voltage_now += (percent_to_volt_discharge[6] -
                        percent_to_volt_discharge[5]) / 2;
    }
#endif /* HAVE_DISK_STORAGE */
#endif /* HAVE_IPOD_UI */

    voltage_avg = voltage_now * BATT_AVE_SAMPLES;
#endif /* CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE */

#if CONFIG_BATTERY_MEASURE & CURRENT_MEASURE
    current_now = _battery_current();
    current_avg = current_now * BATT_CURRENT_AVE_SAMPLES;
#endif
}

/* Sample the battery voltage/current and update the filters.
 * Updated once every POWER_THREAD_STEP_TICKS. */
static void average_step(bool low_battery)
{
#if CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE
    int millivolts = _battery_voltage();
    if(low_battery) {
        voltage_now = (millivolts + voltage_now + 1) / 2;
        voltage_avg += voltage_now - voltage_avg / BATT_AVE_SAMPLES;
    } else {
        voltage_avg += millivolts - voltage_avg / BATT_AVE_SAMPLES;
        voltage_now = voltage_avg / BATT_AVE_SAMPLES;
    }
#else
    (void)low_battery;
#endif

#if CONFIG_BATTERY_MEASURE & CURRENT_MEASURE
    current_avg += _battery_current() - current_avg / BATT_CURRENT_AVE_SAMPLES;
    current_now = current_avg / BATT_CURRENT_AVE_SAMPLES;
#endif
}

/* Send system battery level update events on reaching certain significant
 * levels. This is called by battery_status_update() and does not have to
 * be called separately. */
static void send_battery_level_event(int percent)
{
    static const int levels[] = { 5, 15, 30, 50, 0 };
    const int *level = levels;

    while (*level)
    {
        if (percent <= *level && last_sent_battery_level > *level) {
            last_sent_battery_level = *level;
            queue_broadcast(SYS_BATTERY_UPDATE, last_sent_battery_level);
            break;
        }

        level++;
    }
}

#if CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE
/* Look into the percent_to_volt_* table and estimate the battery level. */
static int voltage_to_percent(int voltage, const short* table)
{
    if (voltage <= table[0]) {
        return 0;
    }
    else if (voltage >= table[10]) {
        return 100;
    }
    else {
        /* search nearest value */
        int i = 0;

        while (i < 10 && table[i+1] < voltage)
            i++;

        /* interpolate linear between the smaller and greater value */
        /* Tens digit, 10% per entry,  ones digit: interpolated */
        return i*10 + (voltage - table[i])*10 / (table[i+1] - table[i]);
    }
}

/* Convert voltage to a battery level percentage using the appropriate
 * percent_to_volt_* lookup table. */
static int voltage_to_battery_level(int millivolts)
{
    int level;

    if (millivolts < 0)
        return -1;

#if CONFIG_CHARGING >= CHARGING_MONITOR
    if (charging_state()) {
        /* battery level is defined to be < 100% until charging is finished */
        level = voltage_to_percent(millivolts, percent_to_volt_charge);
        if (level > 99)
            level = 99;
    }
    else
#endif /* CONFIG_CHARGING >= CHARGING_MONITOR */
    {
        /* DISCHARGING or error state */
        level = voltage_to_percent(millivolts, percent_to_volt_discharge);
    }

    return level;
}
#endif /* CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE */

/* Update battery percentage and time remaining information.
 *
 * This will be called by the power thread after polling new battery data.
 * It must also be called if the battery type or capacity changes.
 */
static void battery_status_update(void)
{
#if ((CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE) && (CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE))
    int level = _battery_level();
    if (level == -1) {
        level = voltage_to_battery_level(voltage_now);
    }
#elif CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE
    int level = _battery_level();
#elif CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE
    int level = voltage_to_battery_level(voltage_now);
#else
    /* This should be a compile time error? */
    int level = -1;
#endif

#if CONFIG_BATTERY_MEASURE & TIME_MEASURE
    time_now = _battery_time();
#elif defined(HAVE_TIME_ESTIMATION)
    /* TODO: This is essentially a bad version of coloumb counting,
     * so in theory using coloumb counters when they are available
     * should provide a more accurate result. Also note that this
     * is hard-coded with a HZ/2 update rate to simplify arithmetic. */

    int current = battery_current();
    int resolution = battery_capacity * 36;

    int time_est = 0;
    if(level >= 0 && current > 0) {
#if CONFIG_CHARGING >= CHARGING_MONITOR
        if (charging_state())
            time_est = (100 - level) * battery_capacity * 36 / current;
        else
#endif
            time_est = level * battery_capacity * 36 / current;

        /* The first term nudges the counter toward the estimate. */
        time_err += current * (time_est - time_cnt);
    }

    /* The second term decrements the counter due to elapsed time. */
    time_err -= resolution;

    /* Arbitrary cutoff to ensure we don't get too far out
     * of sync. Seems to work well on synthetic tests. */
    if(time_err >  resolution * 12 ||
       time_err < -resolution * 13) {
        time_cnt = time_est;
        time_err = 0;
    }

    if(resolution > 0) {
        /* Convert the error into a time and adjust the counter. */
        int64_t adjustment = time_err / (2 * resolution);
        time_cnt += adjustment;
        time_err -= adjustment * (2 * resolution);
    }

    /* Update the reported time based on the counter. */
    time_now = (time_cnt + 30) / 60;
    if(time_now < 0)
        time_now = 0;
#endif

    percent_now = level;
    send_battery_level_event(level);
}

void battery_read_info(int *voltage, int *level)
{
    int millivolts = _battery_voltage();

    if (voltage)
        *voltage = millivolts;

    if (level)  {
#if ((CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE) && (CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE))
        *level = _battery_level();
        if (*level == -1) {
            *level = voltage_to_battery_level(millivolts);
        }
#elif (CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE)
        *level = _battery_level();
#elif (CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE)
        *level = voltage_to_battery_level(millivolts);
#else
        *level = -1;
#endif
    }
}

#if BATTERY_CAPACITY_INC > 0
void set_battery_capacity(int capacity)
{
    if (capacity > BATTERY_CAPACITY_MAX)
        capacity = BATTERY_CAPACITY_MAX;
    if (capacity < BATTERY_CAPACITY_MIN)
        capacity = BATTERY_CAPACITY_MIN;

    if (capacity != battery_capacity) {
        battery_capacity = capacity;
        battery_status_update(); /* recalculate the battery status */
    }
}
#endif

int get_battery_capacity(void)
{
    return battery_capacity;
}

/* Tells if the battery level is safe for disk writes */
bool battery_level_safe(void)
{
#if defined(NO_LOW_BATTERY_SHUTDOWN)
    return true;
#elif ((CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE) && (CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE))
    return voltage_now > battery_level_disksafe;
#elif CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE
    return percent_now > 0;
#elif defined(HAVE_BATTERY_SWITCH)
    /* Cannot rely upon the battery reading to be valid and the
     * device could be powered externally. */
    return input_millivolts() > battery_level_disksafe;
#else
    return voltage_now > battery_level_disksafe;
#endif
}

/* Check to see whether or not we've received an alarm in the last second */
#ifdef HAVE_RTC_ALARM
static void power_thread_rtc_process(void)
{
#ifdef HAVE_IPOD_UI
    /* ProkBox P12 (H2): reading INT1 clears its alarm flag (read to
     * clear) and nothing more. Disarming here (year 0x99, RTCWAK off)
     * raced the shell's alarm_rearm(), which is the only code that arms
     * or disarms the PMU alarm (apps/ipodui/alarm.c) */
    (void)rtc_check_alarm_flag();
#else
    if (rtc_check_alarm_flag())
        rtc_enable_alarm(false);
#endif
}
#endif

/* switch off unit if battery level is too low for reliable operation */
bool query_force_shutdown(void)
{
#if CONFIG_CHARGING
    /* It doesn't make sense to force shutdown when externally powered. */
    if (power_input_present())
        return false;
#endif

#if defined(NO_LOW_BATTERY_SHUTDOWN)
    return false;
#elif ((CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE) && (CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE))
    /* If we have both, prefer voltage */
    return voltage_now < battery_level_shutoff;
#elif CONFIG_BATTERY_MEASURE & PERCENTAGE_MEASURE
    return percent_now == 0;
#elif defined(HAVE_BATTERY_SWITCH)
    /* Cannot rely upon the battery reading to be valid and the
     * device could be powered externally. */
    return input_millivolts() < battery_level_shutoff;
#else
    return voltage_now < battery_level_shutoff;
#endif
}

#if defined(HAVE_BATTERY_SWITCH) || defined(HAVE_RESET_BATTERY_FILTER)
/*
 * Reset the battery voltage filter to a new value and update the
 * status.
 */
void reset_battery_filter(int millivolts)
{
    voltage_avg = millivolts * BATT_AVE_SAMPLES;
    voltage_now = millivolts;
#if CONFIG_BATTERY_MEASURE & CURRENT_MEASURE
    /* current would probably be inaccurate too */
    current_now = _battery_current();
    current_avg = current_now * BATT_CURRENT_AVE_SAMPLES;
#endif
    battery_status_update();
}
#endif /* HAVE_BATTERY_SWITCH */

#ifdef HAVE_POWER_SLEEP
/* ProkBox Phase 18-1: the filter holds the average from before a soft
 * sleep; restart it from the sleep's own raw reading (0: read now) so
 * the level and the glyph in the wake's first frame are current. The
 * scheduler is cooperative, so the power thread cannot sample between
 * this and the caller's frame. */
void battery_wake_reading(int millivolts)
{
    reset_battery_filter(millivolts > 0 ? millivolts : _battery_voltage());
}
#endif

/** Generic charging algorithms for common charging types **/
#if CONFIG_CHARGING == 0 || CONFIG_CHARGING == CHARGING_SIMPLE
static inline void powermgmt_init_target(void)
{
    /* Nothing to do */
}

static inline void charging_algorithm_step(void)
{
    /* Nothing to do */
}

static inline void charging_algorithm_close(void)
{
    /* Nothing to do */
}
#elif CONFIG_CHARGING == CHARGING_MONITOR
/*
 * Monitor CHARGING/DISCHARGING state.
 */
static inline void powermgmt_init_target(void)
{
    /* Nothing to do */
}

static inline void charging_algorithm_step(void)
{
    switch (charger_input_state)
    {
    case CHARGER_PLUGGED:
    case CHARGER:
        if (charging_state()) {
            charge_state = CHARGING;
            break;
        }
    /* Fallthrough */
    case CHARGER_UNPLUGGED:
    case NO_CHARGER:
        charge_state = DISCHARGING;
        break;
    }
}

static inline void charging_algorithm_close(void)
{
    /* Nothing to do */
}
#endif /* CONFIG_CHARGING == * */

#if CONFIG_CHARGING
/* Shortcut function calls - compatibility, simplicity. */

/* Returns true if any power input is capable of charging. */
bool charger_inserted(void)
{
#ifndef BOOTLOADER
    unsigned int data = power_thread_inputs;
#else
    unsigned int data = power_input_status();
#endif
    return data & POWER_INPUT_CHARGER;
}

/* Returns true if any power input is connected - charging-capable
 * or not. */
bool power_input_present(void)
{
#ifndef BOOTLOADER
    unsigned int data = power_thread_inputs;
#else
    unsigned int data = power_input_status();
#endif
    return data & POWER_INPUT;
}

/*
 * Detect charger inserted. Return true if the state is transistional.
 */
static inline bool detect_charger(unsigned int pwr)
{
    /*
     * Detect charger plugged/unplugged transitions.  On a plugged or
     * unplugged event, we return immediately, run once through the main
     * loop (including the subroutines), and end up back here where we
     * transition to the appropriate steady state charger on/off state.
     */
    if (pwr & POWER_INPUT_CHARGER) {
        switch (charger_input_state)
        {
        case NO_CHARGER:
        case CHARGER_UNPLUGGED:
            charger_input_state = CHARGER_PLUGGED;
            break;

        case CHARGER_PLUGGED:
            queue_broadcast(SYS_CHARGER_CONNECTED, 0);
            last_sent_battery_level = 0;
            charger_input_state = CHARGER;
            break;

        case CHARGER:
            /* Steady state */
            return false;
        }
    }
    else {    /* charger not inserted */
        switch (charger_input_state)
        {
        case NO_CHARGER:
            /* Steady state */
            return false;

        case CHARGER_UNPLUGGED:
            queue_broadcast(SYS_CHARGER_DISCONNECTED, 0);
            last_sent_battery_level = 100;
            charger_input_state = NO_CHARGER;
            break;

        case CHARGER_PLUGGED:
        case CHARGER:
            charger_input_state = CHARGER_UNPLUGGED;
            break;
        }
    }

    /* Transitional state */
    return true;
}
#endif /* CONFIG_CHARGING */

#if CONFIG_BATTERY_MEASURE & VOLTAGE_MEASURE
static int power_hist_item(void)
{
    return voltage_now;
}
#else
static int power_hist_item(void)
{
    return percent_now;
}
#endif

static void collect_power_history(void)
{
    /* rotate the power history */
    memmove(&power_history[1], &power_history[0],
            sizeof(power_history) - sizeof(power_history[0]));
    power_history[0] = power_hist_item();
}

#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
/* P18 18-A (hardware-fit-audit K1): the power thread waits for a deadline
 * instead of polling at 2 Hz.  Everything it did on that cadence either
 * has an event that kicks it (the charger and USB edges, the backlight
 * lighting, PCM starting: power_thread_kick()) or does not need 2 Hz while
 * nothing is going on.  Quiet = a settled discharge well above the
 * warnings: no charger, no USB, no shutdown pending and the level above
 * POWER_QUIET_MIN_PCT.  Then the step runs every POWER_QUIET_TICKS, and
 * the voltage filter takes one sample per elapsed 0.5 s (the same reading
 * repeated, so its time constant, 128 half-seconds, is unchanged).
 * Anything else keeps the stock HZ/2.  Not quiet at or under 15 %: the
 * 15 % and 5 % warnings and the low-battery shutdown keep their cadence. */
#define POWER_QUIET_TICKS   (2*HZ)
#define POWER_QUIET_MIN_PCT 15
/* P19 19-1a (review M2): the filter's catch-up cap covers the 10 s dark
 * step (20 half-second samples) with room for a late wake, so its time
 * constant holds at the longer step */
#define POWER_FILTER_STEPS_MAX \
    (QW_POWER_DARK(HZ) / POWER_THREAD_STEP_TICKS + 4)
static long shutdown_began;

static struct semaphore power_kick_sem XCORE_BSS;     /* P19 S0: either core */
static long power_last_step;

void power_thread_kick(void)
{
    semaphore_release(&power_kick_sem);
}

static bool power_is_quiet(void)
{
    return !shutdown_timeout && percent_now > POWER_QUIET_MIN_PCT
#if CONFIG_CHARGING
        && charger_input_state == NO_CHARGER && !power_input_present()
#endif
        && !usb_inserted();
}

/* P19 19-1a: quiet, playing and dark, the step is 10 s
 * (QW_POWER_DARK): the voltage moves ~14 mV/h in dark play, so the 64 s
 * filter loses nothing. The sleep timer's end is a deadline of its own;
 * the idle power-off needs none (it never runs while playing). Lit,
 * paused or stopped keep 18-A's step; the backlight lighting and PCM
 * starting still kick (power_thread_kick()), and so do a shutdown and a
 * sleep timer set. */
static bool power_wait_step(void)
{
    struct qw_power_in in = {
        .now = current_tick,
        .last_step = power_last_step,
        .quiet = power_is_quiet(),
        .dark_playing = audio_status() == AUDIO_STATUS_PLAY
                        && !is_backlight_on(true),
#ifndef BOOTLOADER
        .sleeptimer = sleeptimer_active,
        .sleep_end = sleeptimer_endtick,
#endif
    };
    long left = qw_power_wait(&in, HZ);
    bool due;

    if (left <= 0)
        return true;
    due = semaphore_wait(&power_kick_sem, left) == OBJ_WAIT_TIMEDOUT;
    DEVSTATS_POLL(DEVSTATS_POLL_POWER, due);
    return due;
}
#endif /* HAVE_IPOD_UI && !BOOTLOADER */

/*
 * Monitor the presence of a charger and perform critical frequent steps
 * such as running the battery voltage filter.
 */
static inline void power_thread_step(void)
{
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    /* the time since the last step, in half-second steps (at least 1) */
    long elapsed = current_tick - power_last_step;
    int steps = (elapsed + POWER_THREAD_STEP_TICKS/2) / POWER_THREAD_STEP_TICKS;

    long shut_from = power_last_step;   /* P19 19-1a: see below */

    power_last_step = current_tick;
    if (steps < 1)
        steps = 1;
    else if (steps > POWER_FILTER_STEPS_MAX)
        steps = POWER_FILTER_STEPS_MAX;
#else
    const int steps = 1;
#endif
    /* If the power off timeout expires, the main thread has failed
       to shut down the system, and we need to force a power off */
    if (shutdown_timeout) {
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
        /* P19 19-1a: only the time since the shutdown began counts, not
         * a whole 10 s step that started before it */
        if (TIME_AFTER(shutdown_began, shut_from))
            shut_from = shutdown_began;
        shutdown_timeout -= MAX(current_tick - shut_from, 1);
#else
        shutdown_timeout -= POWER_THREAD_STEP_TICKS * steps;
#endif

        if (shutdown_timeout <= 0)
            power_off();
    }

#ifdef HAVE_PLL_IDLE
    pll_idle_poll();    /* ProkBox Fit-1 C6: backlight and PCM holders */
#endif
#ifdef HAVE_PP_GOV
    pgov_apply();       /* ProkBox: the band governor's decision, if on */
#endif

#ifdef HAVE_RTC_ALARM
    power_thread_rtc_process();
#endif

    /*
     * Do a digital exponential filter.  We don't sample the battery if
     * the disk is spinning unless we are in USB mode (the disk will most
     * likely always be spinning in USB mode) or charging.
     */
    if (!storage_disk_is_active() || usb_inserted()
#if CONFIG_CHARGING >= CHARGING_MONITOR
            || charger_input_state == CHARGER
#endif
    ) {
        for (int i = 0; i < steps; i++)
            average_step(false);
        battery_status_update();
    }
    else if (percent_now < 8) {
        average_step(true);
        battery_status_update();

        /*
         * If battery is low, observe voltage during disk activity.
         * Shut down if voltage drops below shutoff level and we are not
         * using NiMH or Alkaline batteries.
         */
        if (!shutdown_timeout && query_force_shutdown()) {
            sys_poweroff();
        }
    }
} /* power_thread_step */

static void power_thread(void)
{
    long next_power_hist;

    /* Delay reading the first battery level */
#ifdef MROBE_100
    while (_battery_voltage() > 4200) /* gives false readings initially */
    {
#elif defined(DX50) || defined(DX90)
    while (_battery_voltage() < 1) /* can give false readings initially */
    {
#elif defined(EROS_QN) || defined(FIIO_M3K) || defined(SHANLING_Q1)

    /* wait until the first battery read is ready */
    while (_battery_voltage() <= 0)
    {
#else
    {
#endif
        sleep(HZ/100);
    }

#if CONFIG_CHARGING
    /* Initialize power input status before calling other routines. */
    power_thread_inputs = power_input_status();
#endif

    /* call target specific init now */
    powermgmt_init_target();
    /* initialize voltage averaging (if available) */
    average_init();
    /* get initial battery level value (in %) */
    battery_status_update();
    /* get some initial data for the power curve */
    collect_power_history();

    next_power_hist = current_tick + HZ*60;
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    power_last_step = current_tick;
#endif

    while (1)
    {
#if CONFIG_CHARGING
        unsigned int pwr = power_input_status();
#ifdef HAVE_BATTERY_SWITCH
        if ((pwr ^ power_thread_inputs) & POWER_INPUT_BATTERY) {
            sleep(HZ/10);
            reset_battery_filter(_battery_voltage());
        }
#endif
        power_thread_inputs = pwr;

        if (!detect_charger(pwr))
#endif /* CONFIG_CHARGING */
        {
            /* Steady state */
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
            if (!power_wait_step()) {
#ifdef HAVE_PLL_IDLE
                pll_idle_poll();    /* a kick: backlight or PCM changed */
#endif
#ifdef HAVE_PP_GOV
                pgov_apply();       /* or the governor's decision */
#endif
                continue;
            }
#else
            sleep(POWER_THREAD_STEP_TICKS);
#endif

            /* Do common power tasks */
            power_thread_step();
        }

        /* Perform target tasks */
        charging_algorithm_step();

        /* check if some idle or sleep timer wears off */
        handle_auto_poweroff();

        if (TIME_AFTER(current_tick, next_power_hist)) {
            /* increment to ensure there is a record for every minute
             * rather than go forward from the current tick */
            next_power_hist += HZ*60;
            collect_power_history();
        }
    }
} /* power_thread */

#if (BATTERY_CAPACITY_DEFAULT > 0) && !defined(BOOTLOADER)
static bool battery_table_readln(int fd, char * buf, size_t bufsz,
                        const char *name, char **value, int* linect) INIT_ATTR;
static bool battery_table_readln(int fd, char * buf, size_t bufsz,
                                    const char *name, char **value, int* linect)
{
    /* reads a line from user battery level file skips comments
    * if name is NULL and the line is a continuation (no setting:)
    * or name matches the found setting then remaining line contents are returned in value
    * name if supplied should contain the name of the setting you are searching for */
    int rd;
    char *setting;
    if (name)
    {
        /* DEBUGF("%s Searching for '%s'\n", __func__, name); */
        lseek(fd, 0, SEEK_SET);
        *linect = 0;
    }

    while(1)
    {
        rd = read_line(fd, buf, bufsz);
        if (rd > 0)
        {
            /*DEBUGF("\nREAD '%s'\n", buf);*/
            *linect = *linect + 1;
            if (buf[0] == '#' || buf[0] == '\0')
                continue; /* skip empty lines and comments to EOL */

            bool found = settings_parseline(buf, &setting, value);

            if (!name) /* if name is not supplied just return value */
            {
                *value = buf;
                if(found) /* expected more values but got a new setting instead */
                    return false; /* error */
            }
            else if (strcasecmp(name, setting) != 0)
                continue; /* not the correct setting */
        }
        break;
    }
    return rd > 0;
}
#endif

void init_battery_tables(void)
{
#if (BATTERY_CAPACITY_DEFAULT > 0) && !defined(BOOTLOADER)
    /* parse and load user battery levels file */
#define PWRELEMS (ARRAYLEN(percent_to_volt_discharge))


    unsigned short tmparr[PWRELEMS];
    char buf[MAX_PATH];
    unsigned int i, bl_op;

    enum { eSHUTOFF = 0, eDISKSAFE, eDISCHARGE, eCHARGE };
    static const char * const bl_options[4] = {
        [eSHUTOFF] = "shutoff",
        [eDISKSAFE] = "disksafe",
        [eDISCHARGE] = "discharge",
        [eCHARGE] = "charge"
    };

    int fd = open(BATTERY_LEVELS_USER, O_RDONLY);
    int line_num = 0;

    unsigned short val;
    unsigned short disksafe = *device_battery_tables.disksafe;
    unsigned short shutoff = *device_battery_tables.shutoff;
    char *value;

    if (fd < 0)
        return;

    DEBUGF("%s  %s\n", __func__, BATTERY_LEVELS_USER);
    /* force order of reads to do error checking of values */
    for(bl_op = 0; bl_op < ARRAYLEN(bl_options); bl_op++)
    {
        if(!battery_table_readln(fd, buf, sizeof(buf),
           bl_options[bl_op], &value, &line_num))
        {
            continue;
        }

        switch(bl_op)
        {
        default:
            goto error_loading;
        case eSHUTOFF:
            /*fall-through*/
        case eDISKSAFE:
            /* parse single short */
            DEBUGF("%s ", bl_options[bl_op]);

            while (*value != '\0' && !isdigit(*value))
                value++;
            if (*value)
            {
                val = atoi(value);
                DEBUGF("value = %u\n", val);
                if (bl_op == eDISKSAFE)
                {
                    if (val < shutoff)
                    {
                        goto error_loading;
                    }
                    disksafe = val;
                    break;
                }
                /* shutoff */
                shutoff = val;
                break;
            }
            goto error_loading;
        case eDISCHARGE:
            /*fall-through*/
        case eCHARGE:
            /* parse array of short */
            DEBUGF("%s = { ", bl_options[bl_op]);
            val = shutoff; /* don't allow a value lower than shutoff */
            i = 0;
            while(i < PWRELEMS)
            {
                while (*value != '\0' && !isdigit(*value))
                    {value++;}
                if (*value)
                {
                    tmparr[i] = atoi(value);
                    while (isdigit(*value)) /* skip digits just read */
                        {value++;}
                    if (tmparr[i] < val)
                    {
                        goto error_loading; /* value is not >= previous */
                    }
                    val = tmparr[i];
                    DEBUGF("%u, ", val);
                    i++;
                }
                else if (!battery_table_readln(fd, buf, sizeof(buf),
                                            NULL, &value, &line_num))
                {
                    goto error_loading; /* failed to get another line */
                }

            } /* while */
            DEBUGF("}\n");

            /* if we made it here, the values should be OK to use */
            if (bl_op == eCHARGE)
            {
#if CONFIG_CHARGING
                memcpy(device_battery_tables.charge, &tmparr, PWRELEMS);
#endif
                break;
            }
            memcpy(device_battery_tables.discharge, &tmparr, PWRELEMS);
            break;
        } /* switch */
    } /* for */
    close(fd);

    *device_battery_tables.disksafe = disksafe;
    *device_battery_tables.shutoff = shutoff;
    device_battery_tables.isdefault = false;
    battery_status_update();

    return;

error_loading:
    if (fd >= 0)
         close(fd);
    splashf(HZ * 2, "Error line:(%d) loading %s", line_num, BATTERY_LEVELS_USER);
    DEBUGF("Error line:(%d) loading %s\n", line_num, BATTERY_LEVELS_USER);
#undef PWRELEMS
#endif /* ndef BOOTLOADER*/
}

void powermgmt_init(void)
{
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    semaphore_init(&power_kick_sem, 1, 0);  /* before anything can kick */
#endif
    create_thread(power_thread, power_stack, sizeof(power_stack), 0,
                  power_thread_name IF_PRIO(, PRIORITY_SYSTEM)
                  IF_COP(, CPU));
}

/* Various hardware housekeeping tasks relating to shutting down the player */
void shutdown_hw(enum shutdown_type sd_type)
{
    charging_algorithm_close();
    audio_stop();

    if (battery_level_safe()) { /* do not save on critical battery */
        font_unload_all();

/* Commit pending writes if needed. Even though we don't do write caching,
   things like flash translation layers may need this to commit scattered
   pages to their final locations. So far only used for iPod Nano 2G. */
#ifdef HAVE_STORAGE_FLUSH
        storage_flush();
#endif

        if (storage_disk_is_active())
            storage_spindown(1);
    }

    audiohw_close();

    /* If HD is still active we try to wait for spindown, otherwise the
       shutdown_timeout in power_thread_step will force a power off */
    while (storage_disk_is_active())
        sleep(HZ/10);

#ifndef HAVE_LCD_COLOR
    lcd_set_contrast(0);
#endif
#ifdef HAVE_REMOTE_LCD
    lcd_remote_set_contrast(0);
#endif
#ifdef HAVE_LCD_SHUTDOWN
    lcd_shutdown();
#endif

    /* Small delay to make sure all HW gets time to flush. Especially
       eeprom chips are quite slow and might be still writing the last
       byte. */
    sleep(HZ/4);

    switch (sd_type) {
    case SHUTDOWN_POWER_OFF:
    default:
        power_off();
        break;

    case SHUTDOWN_REBOOT:
        system_reboot();
        break;
    }
}

void set_poweroff_timeout(int timeout)
{
    poweroff_timeout = timeout;
}

void reset_poweroff_timer(void)
{
    last_event_tick = current_tick;
    if (sleeptimer_active && sleeptimer_key_restarts)
        set_sleep_timer(sleeptimer_duration);
}

#ifndef BOOTLOADER
static void sys_shutdown_common(void)
{
    /* If the main thread fails to shut down the system, we will force a
       power off after an 20 second timeout - 28 seconds if recording */
    if (shutdown_timeout == 0) {
#if (defined(IAUDIO_X5) || defined(IAUDIO_M5) || defined(COWON_D2)) && !defined(SIMULATOR)
        pcf50606_reset_timeout(); /* Reset timer on first attempt only */
#endif
#ifdef HAVE_RECORDING
        if (audio_status() & AUDIO_STATUS_RECORD)
            shutdown_timeout += HZ*8;
#endif
#ifdef IPOD_NANO2G
        /* The FTL alone may take half a minute to shut down cleanly. */
        shutdown_timeout += HZ*60;
#else
        shutdown_timeout += HZ*20;
#endif
#if defined(HAVE_IPOD_UI)
        /* P19 19-1a: counted from now, and the thread steps at HZ/2 now
         * (not quiet), not at the end of a 10 s wait */
        shutdown_began = current_tick;
        power_thread_kick();
#endif
    }
}
#endif /* BOOTLOADER */

#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
/* ProkBox (owner, #63): while a computer is connected the device stays on
 * and mounted. Every software power-off funnels through here (the sleep
 * timer, auto power-off, the held PLAY key, the shell), so it is refused
 * while the host has the disk; the screen may still go dark. A
 * hardware reset (MENU+SELECT) does not pass through software and keeps
 * working. The shell logs the count (usbscreen.c). */
static unsigned int poweroff_blocked;

bool powermgmt_poweroff_inhibited(void)
{
    /* a critically low battery always shuts down cleanly (review M1), and
     * only a computer that owns the disk keeps the device on: charge-only
     * power or a stuck detect line does not (review M2) */
    if (query_force_shutdown())
        return false;
#ifdef USB_FULL_INIT
    if (!usb_exclusive_storage())
        return false;
#else
    if (!usb_inserted())
        return false;
#endif
    poweroff_blocked++;
    return true;
}

unsigned int powermgmt_poweroff_blocked(void)
{
    return poweroff_blocked;
}
#endif

void sys_poweroff(void)
{
#ifndef BOOTLOADER
#ifdef HAVE_IPOD_UI
    if (powermgmt_poweroff_inhibited())
        return;
#endif
    logf("sys_poweroff()");
#ifdef HAVE_PP_GOV_DVS
    pgov2_shutdown();   /* ProkBox: the planned end; the rail to normal */
#endif
    sys_shutdown_common();
    queue_broadcast(SYS_POWEROFF, 0);
#endif
}

/* not to be confused with system_reboot... :( */
void sys_reboot(void)
{
#ifndef BOOTLOADER
    logf("sys_reboot()");
#ifdef HAVE_PP_GOV_DVS
    pgov2_shutdown();   /* ProkBox: the planned end; the rail to normal */
#endif
    sys_shutdown_common();
    queue_broadcast(SYS_REBOOT, 0);
#endif
}

void cancel_shutdown(void)
{
    logf("cancel_shutdown()");

#if (defined(IAUDIO_X5) || defined(IAUDIO_M5) || defined(COWON_D2)) && !defined(SIMULATOR)
    /* TODO: Move some things to target/ tree */
    if (shutdown_timeout)
        pcf50606_reset_timeout();
#endif

    shutdown_timeout = 0;
#ifdef HAVE_PP_GOV_DVS
    pgov2_shutdown_cancelled();     /* ProkBox: stage 2 may arm again */
#endif
}

void set_sleeptimer_duration(int minutes)
{
    set_sleep_timer(minutes * 60);
}

static void set_sleep_timer(int seconds)
{
    if (seconds) {
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
        /* P19 19-1a: a deadline of its own; the thread is kicked only
         * when it moves earlier (review L4: not at every key, whose
         * restart only moves it later) */
        long end = current_tick + seconds * HZ;
        bool earlier = !sleeptimer_active
                       || TIME_BEFORE(end, sleeptimer_endtick);
        sleeptimer_active  = true;
        sleeptimer_endtick = end;
        if (earlier)
            power_thread_kick();
#else
        sleeptimer_active  = true;
        sleeptimer_endtick = current_tick + seconds * HZ;
#endif
    }
    else {
        sleeptimer_active  = false;
        sleeptimer_endtick = 0;
    }
    sleeptimer_duration = seconds;
}

bool get_sleep_timer_active(void)
{
    return sleeptimer_active;
}

int get_sleep_timer(void)
{
    if (sleeptimer_active && (sleeptimer_endtick >= current_tick))
        return (sleeptimer_endtick - current_tick) / HZ;
    else
        return 0;
}

void set_keypress_restarts_sleep_timer(bool enable)
{
    sleeptimer_key_restarts = enable;
}

#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
static bool sleep_handler = false;

void powermgmt_set_sleep_handler(bool registered)
{
    sleep_handler = registered;
}
#endif

#ifndef BOOTLOADER
static void handle_sleep_timer(void)
{
    if (TIME_AFTER(current_tick, sleeptimer_endtick)) {
        if (usb_inserted()
#if CONFIG_CHARGING
            || charger_input_state != NO_CHARGER
#endif
        ) {
            DEBUGF("Sleep timer timeout. Stopping...\n");
            audio_pause();
            set_sleep_timer(0);
            backlight_off(); /* Nighty, nighty... */
        }
        else {
            DEBUGF("Sleep timer timeout. Shutting off...\n");
#if defined(HAVE_IPOD_UI)
            /* P12 W2-T / Q9: the shell's sleep (soft sleep, the power-off
             * tier, or Q1(2)'s dark wait with an alarm due); the timer
             * ends here, so it asks once */
            if (sleep_handler)
            {
                set_sleep_timer(0);
                button_queue_post(SYS_SLEEP_REQUEST, SLEEP_TIMER);
            }
            else
#endif
            sys_poweroff();
        }
    }
}
#endif /* BOOTLOADER */

/*
 * We shut off in the following cases:
 * 1) The unit is idle, not playing music
 * 2) The unit is playing music, but is paused
 * 3) The battery level has reached shutdown limit
 *
 * We do not shut off in the following cases:
 * 1) The USB is connected
 * 2) The charger is connected
 * 3) We are recording, or recording with pause
 * 4) The radio is playing
 */

#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
static bool auto_off_held;      /* P19 19-1a: the last step saw it held */
#endif

void handle_auto_poweroff(void)
{
#ifndef BOOTLOADER
    long timeout = poweroff_timeout*60*HZ;
    int audio_stat = audio_status();
    long tick = current_tick;

    /*
     * Inhibit shutdown as long as the charger is plugged in.  If it is
     * unplugged, wait for a timeout period and then shut down.
     */
    if (audio_stat == AUDIO_STATUS_PLAY
#if CONFIG_CHARGING
        || charger_input_state == CHARGER
#endif
    ) {
        last_event_tick = current_tick;
#if defined(HAVE_IPOD_UI)
        auto_off_held = true;
    }
    else if (auto_off_held) {
        /* P19 19-1a: the dark play step is 10 s, so the idle timer
         * starts at the first step that sees playback stop (at most 10 s
         * late), never up to 10 s early from the last playing step */
        auto_off_held = false;
        last_event_tick = current_tick;
#endif
    }

    if (!shutdown_timeout && query_force_shutdown()) {
        backlight_on();
        sys_poweroff();
    }

    if (timeout &&
#if CONFIG_TUNER
        !(get_radio_status() & FMRADIO_PLAYING) &&
#endif
        !usb_inserted() &&
        (audio_stat == 0 ||
         audio_stat == (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE)))
    {
        if (TIME_AFTER(tick, last_event_tick + timeout)
#if !(CONFIG_PLATFORM & PLATFORM_HOSTED)
            && TIME_AFTER(tick, storage_last_disk_activity() + timeout)
#endif
        ) {
#if defined(HAVE_IPOD_UI)
            /* Phase 18 Q4: once the shell has said it handles them, the
             * idle timer asks for a soft sleep (the shell picks the tier,
             * or ignores it); the timer restarts, so an ignored request
             * comes back one timeout later, not at every pass. Without a
             * handler, today's power-off */
            if (sleep_handler)
            {
                last_event_tick = current_tick;
                button_queue_post(SYS_SLEEP_REQUEST, SLEEP_IDLE);
            }
            else
#endif
            sys_poweroff();
        }
    }

    if (sleeptimer_active)
        handle_sleep_timer();
#endif
}
