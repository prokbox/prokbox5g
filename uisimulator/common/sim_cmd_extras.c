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
 * P12 sim extras: PMU alarm stub, piezo trace, rtc_at, press_hold (brief 3.6). Test-only.
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
#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "sim_cmd_extras.h"

#ifndef HAVE_IPOD_UI
bool sim_cmd_extras(const char *cmd, char *argv[], int argc, int lineno)
{
    (void)argv; (void)argc;
    if (strcmp(cmd, "alarm_boot") && strcmp(cmd, "pmu_arm")
        && strcmp(cmd, "pmu_disarm") && strcmp(cmd, "pmu_fire")
        && strcmp(cmd, "pmu_dump") && strcmp(cmd, "expect_pmu")
        && strcmp(cmd, "rtc_at") && strcmp(cmd, "piezo_mark")
        && strcmp(cmd, "expect_piezo") && strcmp(cmd, "press_hold")
        && strcmp(cmd, "piezo_test") && strcmp(cmd, "sdk_bad")
        && strcmp(cmd, "sdk_clear") && strcmp(cmd, "wheel_at"))
        return false;
    fprintf(stderr, "sim_script:%d: %s unavailable\n", lineno, cmd);
    return true;
}
#else /* HAVE_IPOD_UI */

#include <SDL.h>
#include "system.h"
#include "kernel.h"
#include "button-sdl.h"
#include "pcf50605.h"
#include "sim_cmd_sleep.h"
#include "ipodui/alarm_core.h"      /* ALARM_REG_IGNORE (header only) */
#include "ipodui/modclass.h"
#include "ipodui/sdk/sdk_int.h"      /* sdk_sim_badmode() (W4-0) */

#define Y2000_EPOCH 946684800L

/* ---- the modclass accountant hook (brief 3.6) -------------------------
 * filesystem-sim.c calls modclass_io() for every sim file open/read/write.
 * W1-F's modclass.c defines the counter; this weak no-op keeps the sim
 * linking until it lands (the strong definition wins). */
__attribute__((weak)) void modclass_io(bool write, uint32_t bytes)
{
    (void)write; (void)bytes;
}

/* ---- the piezo: the sim has none, so a trace -------------------------- */
void piezo_init(void);
void piezo_play(unsigned short inv_freq, unsigned char form);
void piezo_play_for_tick(unsigned short inv_freq, unsigned char form,
                         unsigned int dur);
void piezo_play_for_usec(unsigned short inv_freq, unsigned char form,
                         unsigned int dur);
void piezo_stop(void);
void piezo_clear(void);
bool piezo_busy(void);
unsigned int piezo_hz(unsigned int hz);
void piezo_button_beep(bool beep, bool force);

static unsigned piezo_plays, piezo_mark_at;

static unsigned inv_to_hz(unsigned inv) { return inv ? 91225u / inv : 0; }

void piezo_init(void) { }
unsigned int piezo_hz(unsigned int hz) { return hz > 0 ? 91225u / hz : 0; }
bool piezo_busy(void) { return false; }
void piezo_clear(void) { }

void piezo_play(unsigned short inv_freq, unsigned char form)
{
    (void)form;
    piezo_plays++;
    fprintf(stderr, "ipodui: piezo play hz=%u ms=0\n", inv_to_hz(inv_freq));
}

void piezo_play_for_tick(unsigned short inv_freq, unsigned char form,
                         unsigned int dur)
{
    (void)form;
    piezo_plays++;
    fprintf(stderr, "ipodui: piezo play hz=%u ms=%u\n", inv_to_hz(inv_freq),
            (unsigned)((uint64_t)dur * 1000 / HZ));
}

void piezo_play_for_usec(unsigned short inv_freq, unsigned char form,
                         unsigned int dur)
{
    (void)form;
    piezo_plays++;
    fprintf(stderr, "ipodui: piezo play hz=%u ms=%u\n", inv_to_hz(inv_freq),
            dur / 1000);
}

void piezo_stop(void) { fprintf(stderr, "ipodui: piezo stop\n"); }

void piezo_button_beep(bool beep, bool force)
{
    (void)beep; (void)force;
    fprintf(stderr, "ipodui: piezo click\n");   /* never "piezo play": the
                                                  trace rule is the alarm's */
}

#ifdef HAVE_RTC_ALARM
/* ---- the PCF50605 PMU: alarm registers, INT1, RTCWAK -------------------
 * Register image per alarm_core.h (0x11..0x17: sec, min, hour, weekday,
 * mday, month, year, BCD; a field at its ALARM_REG_IGNORE value is
 * ignored; year 0x99 = disabled, upstream's mark). INT1 (0x02) bit 7 is
 * the alarm flag, read-to-clear. The flag is evaluated lazily against the
 * sim RTC (sim_sleep_rtc: rtc_set, sleep_warp) at every INT1 read, so the
 * power thread's rtc_check_alarm_flag() (powermgmt.c, ~1 Hz) sees it as
 * the device does. pcf50605_wakeup_flags (RTCWAK = 0x10) starts at 0 each
 * boot, as on the device; what it held at the last power-off is
 * "bootwake". State persists across the shots.sh "reboot" parts through
 * $SIM_PMU_STATE (one small file in the run's output folder). */
#define REG_INT1    0x02
#define REG_INT1M   0x05
#define REG_ALM     0x11
#define INT1_ALARM  0x80
#define RTCWAK      0x10

unsigned char pcf50605_wakeup_flags;

static const unsigned char reg_ignore[7] = ALARM_REG_IGNORE;
static unsigned char pmu_reg[0x80];
static uint32_t pmu_last;               /* RTC s of the last evaluation */
static uint32_t pmu_saved_rtc;          /* RTC s at the last register write */
static unsigned char bootwake;
static long boot_req = -1;              /* alarm_boot <s> pending */
static const char *state_path;
static char state_last[160];

static uint32_t rtc_now(void) { return sim_sleep_rtc(); }

static long days_from_civil(int y, int m, int d)      /* since 1970-01-01 */
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static void civil_from_days(long z, int *y, int *m, int *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

#define DAYS_2000 10957L                /* 1970-01-01 .. 2000-01-01 */

static int bcd(int v) { return (v >> 4) * 10 + (v & 15); }
static int dec2bcd(int v) { return (v / 10) << 4 | (v % 10); }

static bool pmu_enabled(void)
{
    if (pmu_reg[REG_ALM + 6] == ALARM_REG_YEAR_OFF)
        return false;
    /* h:m:s must all be set; a partly ignored time is not modelled */
    for (int i = 0; i < 3; i++)
        if (pmu_reg[REG_ALM + i] == reg_ignore[i])
            return false;
    return true;
}

/* the first match strictly after t, or 0 */
static uint32_t pmu_next_after(uint32_t t)
{
    if (!pmu_enabled())
        return 0;
    int s = bcd(pmu_reg[REG_ALM]), mi = bcd(pmu_reg[REG_ALM + 1]),
        h = bcd(pmu_reg[REG_ALM + 2]);
    long day0 = t / 86400;

    for (long d = day0; d <= day0 + 400; d++)
    {
        int y, mo, md;
        civil_from_days(d + DAYS_2000, &y, &mo, &md);
        unsigned char *r = pmu_reg + REG_ALM;
        if (r[3] != reg_ignore[3] && bcd(r[3]) != (int)((d + 6) % 7))
            continue;
        if (r[4] != reg_ignore[4] && bcd(r[4]) != md) continue;
        if (r[5] != reg_ignore[5] && bcd(r[5]) != mo) continue;
        if (r[6] != reg_ignore[6] && bcd(r[6]) != y - 2000) continue;
        uint32_t c = (uint32_t)(d * 86400 + h * 3600 + mi * 60 + s);
        if (c > t)
            return c;
    }
    return 0;
}

static void pmu_save(void)
{
    char buf[sizeof state_last];
    int n = snprintf(buf, sizeof buf, "v1 off=%ld wake=%u boot=%ld rtc=%u "
                     "int1=%u regs=", sim_rtc_offset_s,
                     pcf50605_wakeup_flags, boot_req, pmu_saved_rtc,
                     pmu_reg[REG_INT1]);
    for (int i = 0; i < 7 && n < (int)sizeof buf - 4; i++)
        n += snprintf(buf + n, sizeof buf - n, "%02x", pmu_reg[REG_ALM + i]);
    if (!state_path || !strcmp(buf, state_last))
        return;
    strcpy(state_last, buf);
    FILE *f = fopen(state_path, "w");
    if (f)
    {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
}

static void pmu_eval(void)
{
    uint32_t now = rtc_now();

    if (!pmu_last)
        pmu_last = now;                 /* first look: nothing is due yet */
    if (now > pmu_last)
    {
        uint32_t a = pmu_next_after(pmu_last);
        if (a && a <= now && !(pmu_reg[REG_INT1] & INT1_ALARM))
        {
            pmu_reg[REG_INT1] |= INT1_ALARM;
            fprintf(stderr, "sim_pmu: int1 alarm armed=%u now=%u\n", a, now);
        }
    }
    pmu_last = now;
    pmu_save();
}

int pcf50605_read(int address)
{
    int v;

    if (address == REG_INT1)
    {
        pmu_eval();
        v = pmu_reg[REG_INT1];
        pmu_reg[REG_INT1] = 0;          /* read-to-clear */
        pmu_save();
        return v;
    }
    return pmu_reg[address & 0x7f];
}

int pcf50605_read_multiple(int address, unsigned char *buf, int count)
{
    for (int i = 0; i < count; i++)
        buf[i] = (unsigned char)pcf50605_read(address + i);
    return count;
}

int pcf50605_write(int address, unsigned char val)
{
    address &= 0x7f;
    if (address >= REG_ALM && address < REG_ALM + 7)
        pmu_eval();                     /* the old image gets its due first */
    pmu_reg[address] = val;
    if (address >= REG_ALM && address < REG_ALM + 7)
    {
        pmu_saved_rtc = pmu_last = rtc_now();
        pmu_save();
    }
    return 1;
}

int pcf50605_write_multiple(int address, const unsigned char *buf, int count)
{
    for (int i = 0; i < count; i++)
        pcf50605_write(address + i, buf[i]);
    return count;
}

int pcf50605_a2d_read(int channel) { (void)channel; return 0; }
bool pcf50605_charger_inserted(void) { return false; }
void pcf50605_standby_mode(void) { }
void pcf50605_init(void) { }

/* what stubs.c's rtc_check_alarm_flag() returns: INT1 bit 7, read-to-clear.
 * Called by the power thread at its step cadence. */
bool sim_pmu_alarm_flag(void)
{
    return pcf50605_read(REG_INT1) & INT1_ALARM;
}

static void pmu_atexit(void)
{
    pmu_save();
}

/* before main(): restore the PMU and the RTC across a "reboot" part */
__attribute__((constructor)) static void pmu_boot(void)
{
    static const unsigned char off[7] = { 0x7f, 0x7f, 0x3f, 0x07, 0x3f, 0x1f,
                                          ALARM_REG_YEAR_OFF };

    memcpy(pmu_reg + REG_ALM, off, sizeof off);
    state_path = getenv("SIM_PMU_STATE");
    if (!state_path || !*state_path)
    {
        state_path = NULL;
        return;
    }
    atexit(pmu_atexit);
    FILE *f = fopen(state_path, "r");
    if (!f)
        return;
    long offv = 0, req = -1;
    unsigned wake = 0, rtc = 0, int1 = 0;
    char regs[32] = "";
    int n = fscanf(f, "v1 off=%ld wake=%u boot=%ld rtc=%u int1=%u regs=%31s",
                   &offv, &wake, &req, &rtc, &int1, regs);
    fclose(f);
    if (n != 6 || strlen(regs) != 14)
        return;
    for (int i = 0; i < 7; i++)
    {
        unsigned v;
        sscanf(regs + 2 * i, "%2x", &v);
        pmu_reg[REG_ALM + i] = (unsigned char)v;
    }
    sim_rtc_offset_s = offv;            /* the RTC kept running while off */
    bootwake = (unsigned char)(wake & RTCWAK);
    pmu_saved_rtc = rtc;
    if (req >= 0)
    {
        uint32_t armed = bootwake ? pmu_next_after(rtc) : 0;

        if (armed)
        {
            extern bool sim_alarm_wakeup;
            sim_rtc_set(armed + (uint32_t)req);
            sim_alarm_wakeup = true;
            fprintf(stderr, "sim_pmu: alarm_boot armed=%u +%lds\n", armed,
                    req);
        }
        else
            fprintf(stderr, "sim_pmu: alarm_boot: no wake (rtcwak=%u "
                    "enabled=%d); a plain boot\n", bootwake,
                    (int)pmu_enabled());
    }
    pmu_last = rtc_now();
    pcf50605_wakeup_flags = 0;          /* the device starts with 0 */
    boot_req = -1;
    state_last[0] = 0;
    pmu_save();
}

static int parse_hms(const char *s, int *h, int *m, int *sec)
{
    return sscanf(s, "%d:%d:%d", h, m, sec) == 3 && *h >= 0 && *h < 24
        && *m >= 0 && *m < 60 && *sec >= 0 && *sec < 60;
}

static int parse_ymd(const char *s, int *y, int *mo, int *d)
{
    return sscanf(s, "%d-%d-%d", y, mo, d) == 3 && *y >= 2000 && *y <= 2099
        && *mo >= 1 && *mo <= 12 && *d >= 1 && *d <= 31;
}

static void pmu_dump(void)
{
    fprintf(stderr, "sim_pmu: regs=");
    for (int i = 0; i < 7; i++)
        fprintf(stderr, "%02x", pmu_reg[REG_ALM + i]);
    fprintf(stderr, " int1=%u rtcwak=%d bootwake=%u rtc=%u\n",
            pmu_reg[REG_INT1], !!(pcf50605_wakeup_flags & RTCWAK), bootwake,
            rtc_now());
}

static void expect_pmu(char *argv[], int argc, int lineno)
{
    char why[160] = "";
    pmu_eval();                         /* a pending match latches first */
    bool on = pmu_enabled();

    if (argc >= 2 && !strcmp(argv[1], "on") && !on)
        snprintf(why, sizeof why, "want armed, PMU alarm is off");
    else if (argc >= 2 && !strcmp(argv[1], "off") && on)
        snprintf(why, sizeof why, "want disarmed, PMU alarm is armed "
                 "(regs %02x%02x%02x..)", pmu_reg[REG_ALM],
                 pmu_reg[REG_ALM + 1], pmu_reg[REG_ALM + 2]);
    for (int i = 2; i < argc && !why[0]; i++)
    {
        const char *a = argv[i];
        int h, m, s, y, mo, d;

        if (!strncmp(a, "hms=", 4) && parse_hms(a + 4, &h, &m, &s))
        {
            if (pmu_reg[REG_ALM] != dec2bcd(s) || pmu_reg[REG_ALM + 1]
                != dec2bcd(m) || pmu_reg[REG_ALM + 2] != dec2bcd(h))
                snprintf(why, sizeof why, "hms: want %s, regs %02x:%02x:%02x",
                         a + 4, pmu_reg[REG_ALM + 2], pmu_reg[REG_ALM + 1],
                         pmu_reg[REG_ALM]);
        }
        else if (!strcmp(a, "date=daily"))
        {
            for (int k = 3; k < 7; k++)
                if (pmu_reg[REG_ALM + k] != reg_ignore[k])
                    snprintf(why, sizeof why, "date: want daily, reg %d = "
                             "%02x", REG_ALM + k, pmu_reg[REG_ALM + k]);
        }
        else if (!strncmp(a, "date=", 5) && parse_ymd(a + 5, &y, &mo, &d))
        {
            if (pmu_reg[REG_ALM + 4] != dec2bcd(d) || pmu_reg[REG_ALM + 5]
                != dec2bcd(mo) || pmu_reg[REG_ALM + 6] != dec2bcd(y - 2000))
                snprintf(why, sizeof why, "date: want %s, regs d=%02x m=%02x "
                         "y=%02x", a + 5, pmu_reg[REG_ALM + 4],
                         pmu_reg[REG_ALM + 5], pmu_reg[REG_ALM + 6]);
        }
        else if (!strncmp(a, "rtcwak=", 7))
        {
            int got = !!(pcf50605_wakeup_flags & RTCWAK);
            if (got != atoi(a + 7))
                snprintf(why, sizeof why, "rtcwak: want %s, got %d", a + 7,
                         got);
        }
        else if (!strncmp(a, "bootwake=", 9))
        {
            if (!!bootwake != atoi(a + 9))
                snprintf(why, sizeof why, "bootwake (RTCWAK at the last "
                         "power-off): want %s, got %d", a + 9, !!bootwake);
        }
        else if (!strncmp(a, "int1=", 5))
        {
            int got = !!(pmu_reg[REG_INT1] & INT1_ALARM);
            if (got != atoi(a + 5))
                snprintf(why, sizeof why, "int1: want %s, got %d", a + 5,
                         got);
        }
        else if (!strncmp(a, "next=", 5))
        {
            uint32_t got = pmu_next_after(rtc_now());
            if (got != (uint32_t)strtoul(a + 5, NULL, 10))
                snprintf(why, sizeof why, "next: want %s, got %u", a + 5,
                         got);
        }
        else
            snprintf(why, sizeof why, "unknown or malformed argument '%s'",
                     a);
    }
    if (why[0])
    {
        pmu_dump();
        fprintf(stderr, "sim_script:%d: expect_pmu failed: %s\n", lineno,
                why);
    }
    else
        fprintf(stderr, "sim_script: expect_pmu ok\n");
}
#endif /* HAVE_RTC_ALARM */

/* the click wheel's absolute position for the SDK (sdk_wheel_pos(); the
 * sim has no HAVE_WHEEL_POSITION): `wheel_at` sets it, -1 untouched */
static volatile int sim_wheel = -1;

int sim_wheel_status(void)
{
    return sim_wheel;
}

/* wheel_at <pos> <ms> [KEY | to_pos]: a finger on the ring at pos (0..95,
 * 0 at MENU, clockwise) for ms, then lifted; with KEY, that button is
 * pressed 30 ms after the touch and let go before the lift (a press of a
 * button under the ring, not a tap); with a number, the finger slides to
 * to_pos half way through */
static void wheel_at(char *argv[], int argc, int lineno)
{
    extern void sim_wheel_touch(void);
    int pos = atoi(argv[1]), key = -1, to = -1;
    long ms = atol(argv[2]);

    if (argc >= 4)
    {
        if (argv[3][0] >= '0' && argv[3][0] <= '9')
            to = atoi(argv[3]) % 96;
        else
            key = sim_script_key(argv[3]);
    }
    if (pos < 0 || pos > 95 || ms < 1)
    {
        fprintf(stderr, "sim_script:%d: wheel_at: bad args\n", lineno);
        return;
    }
    sim_wheel = pos;
    sim_wheel_touch();
    if (key >= 0)
    {
        sleep(30 * HZ / 1000 + 1);
        sim_inject_key(key, true);
        sleep((ms * HZ + 999) / 1000);
        sim_inject_key(key, false);
        sleep(20 * HZ / 1000 + 1);
    }
    else if (to >= 0)
    {
        sleep((ms * HZ / 2 + 999) / 1000);
        sim_wheel = to;
        sleep((ms * HZ / 2 + 999) / 1000);
    }
    else
        sleep((ms * HZ + 999) / 1000);
    sim_wheel = -1;
    fprintf(stderr, "sim_script: wheel_at %d %ld%s%s\n", pos, ms,
            argc >= 4 ? " " : "", argc >= 4 ? argv[3] : "");
}

bool sim_cmd_extras(const char *cmd, char *argv[], int argc, int lineno)
{
    if (!strcmp(cmd, "wheel_at") && argc >= 3)
    {
        wheel_at(argv, argc, lineno);
        return true;
    }
    if (!strcmp(cmd, "piezo_mark"))
    {
        piezo_mark_at = piezo_plays;
        fprintf(stderr, "sim_script: piezo_mark\n");
        return true;
    }
    if (!strcmp(cmd, "expect_piezo"))
    {
        unsigned n = piezo_plays - piezo_mark_at;
        unsigned min = argc >= 2 ? (unsigned)atoi(argv[1]) : 1;
        unsigned max = argc >= 3 ? (unsigned)atoi(argv[2]) : ~0u;

        if (n < min || n > max)
            fprintf(stderr, "sim_script:%d: expect_piezo failed: %u plays "
                    "since the mark, want %u..%u\n", lineno, n, min,
                    max == ~0u ? 999999u : max);
        else
            fprintf(stderr, "sim_script: expect_piezo ok (%u)\n", n);
        return true;
    }
    if (!strcmp(cmd, "piezo_test") && argc >= 3)
    {
        /* what the alarm's ring does: piezo_play_for_tick(piezo_hz(hz),
         * 0x80, ticks); lets the tooling tests drive the trace */
        piezo_play_for_tick((unsigned short)piezo_hz(atoi(argv[1])), 0x80,
                            (unsigned)(atol(argv[2]) * HZ / 1000));
        return true;
    }
    if (!strcmp(cmd, "rtc_at") && argc >= 3)
    {
        int y, mo, d, h, m, s;
        long t;

        if (sscanf(argv[1], "%d-%d-%d", &y, &mo, &d) != 3
            || sscanf(argv[2], "%d:%d:%d", &h, &m, &s) != 3 || y < 2000)
        {
            fprintf(stderr, "sim_script:%d: rtc_at YYYY-MM-DD HH:MM:SS\n",
                    lineno);
            return true;
        }
        t = (long)((days_from_civil(y, mo, d) - DAYS_2000) * 86400L + h * 3600
                   + m * 60 + s);
        sim_rtc_set((uint32_t)t);
        fprintf(stderr, "sim_script: rtc_at %s %s (rtc %ld)\n", argv[1],
                argv[2], t);
        return true;
    }
    /* P12 W4-0: `sdk_bad <mode>` writes the test game's state record
     * (its mode, apps/ipodui/games/badgame/badgame.c) */
    if (!strcmp(cmd, "sdk_bad") && argc >= 2)
    {
        sdk_sim_badmode(atoi(argv[1]));
        fprintf(stderr, "sim_script: sdk_bad %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "sdk_clear"))
    {
        sdk_sim_clear();
        fprintf(stderr, "sim_script: sdk_clear\n");
        return true;
    }
    if (!strcmp(cmd, "press_hold") && argc >= 4)
    {
        int a = sim_script_key(argv[1]), b = sim_script_key(argv[2]);
        long ms = atol(argv[3]);
        long lead = argc >= 5 ? atol(argv[4]) : 20;

        if (a < 0 || b < 0)
        {
            fprintf(stderr, "sim_script:%d: press_hold: unknown key\n",
                    lineno);
            return true;
        }
        sim_inject_key(a, true);
        sleep((lead * HZ + 999) / 1000 + 1);
        sim_inject_key(b, true);
        sleep((ms * HZ + 999) / 1000);  /* both down for ms */
        sim_inject_key(a, false);
        sim_inject_key(b, false);
        fprintf(stderr, "sim_script: press_hold %s %s %ld lead %ld\n",
                argv[1], argv[2], ms, lead);
        return true;
    }
#ifdef HAVE_RTC_ALARM
    if (!strcmp(cmd, "alarm_boot") && argc >= 2)
    {
        boot_req = atol(argv[1]);
        pmu_save();
        fprintf(stderr, "sim_script: alarm_boot %ld (armed next boot; "
                "rtcwak=%d)\n", boot_req,
                !!(pcf50605_wakeup_flags & RTCWAK));
        return true;
    }
    if (!strcmp(cmd, "pmu_arm") && argc >= 2)
    {
        int h, m, s, y, mo, d;
        unsigned char img[7] = ALARM_REG_IGNORE;

        if (!parse_hms(argv[1], &h, &m, &s))
        {
            fprintf(stderr, "sim_script:%d: pmu_arm HH:MM:SS [YYYY-MM-DD]\n",
                    lineno);
            return true;
        }
        img[0] = dec2bcd(s); img[1] = dec2bcd(m); img[2] = dec2bcd(h);
        if (argc >= 3 && parse_ymd(argv[2], &y, &mo, &d))
        {
            img[4] = dec2bcd(d); img[5] = dec2bcd(mo);
            img[6] = dec2bcd(y - 2000);
        }
        pcf50605_write_multiple(REG_ALM, img, 7);
        pcf50605_wakeup_flags |= RTCWAK;
        pmu_save();
        fprintf(stderr, "sim_script: pmu_arm %s%s%s\n", argv[1],
                argc >= 3 ? " " : "", argc >= 3 ? argv[2] : "");
        return true;
    }
    if (!strcmp(cmd, "pmu_disarm"))
    {
        pcf50605_write(REG_ALM + 6, ALARM_REG_YEAR_OFF);
        pcf50605_wakeup_flags &= ~RTCWAK;
        pmu_save();
        fprintf(stderr, "sim_script: pmu_disarm\n");
        return true;
    }
    if (!strcmp(cmd, "pmu_fire"))
    {
        pmu_reg[REG_INT1] |= INT1_ALARM;
        fprintf(stderr, "sim_script: pmu_fire\n");
        return true;
    }
    if (!strcmp(cmd, "pmu_dump"))
    {
        pmu_dump();
        return true;
    }
    if (!strcmp(cmd, "expect_pmu"))
    {
        expect_pmu(argv, argc, lineno);
        return true;
    }
#endif
    return false;
}
#endif /* HAVE_IPOD_UI */
