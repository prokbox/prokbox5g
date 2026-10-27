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
 * iPod UI shell: the resident alarm engine (P12, docs/phase12-brief.md 3.3,
 * 5): alarms.dat, the PMU arm and its read-back, the ring and its beep,
 * the dark wait. UI thread only. The rules are alarm_core.c.
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
#include <string.h>
#include <stdlib.h>
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "backlight.h"
#include "audio.h"
#include "powermgmt.h"
#include "file.h"
#include "dir.h"
#include "rtc.h"
#include "timefuncs.h"
#if defined(HAVE_HARDWARE_CLICK) && !defined(SIMULATOR)
#include "piezo.h"
#elif defined(HAVE_HARDWARE_CLICK)
/* the sim's logging piezo (uisimulator/common/sim_cmd_extras.c) */
void piezo_play_for_tick(unsigned short inv_freq, unsigned char form,
                         unsigned int dur);
void piezo_stop(void);
unsigned int piezo_hz(unsigned int hz);
#endif
#include "ipodui/alarm.h"
#include "ipodui/alarm_ring.h"
#include "ipodui/anim.h"
#include "ipodui/safefile.h"
#include "ipodui/uilog.h"
#include "ipodui/unlit.h"
#include "ipodui/usbscreen.h"
#include "ipodui/xindex.h"

#define ALARMS_DIR  ROCKBOX_DIR "/ipodui"
#define ALARMS_DAT  ALARMS_DIR "/alarms.dat"
#define ALARMS_TMP  ALARMS_DIR "/alarms.tmp"
#define ALARMS_OLD  ALARMS_DIR "/alarms.old"

/* Exact date (one-shot) PMU matches, the brief's recommendation (5.2);
 * 1 = upstream's daily h:m:s match, if D-A3 finds the date not compared */
#define ALARM_PMU_DAILY 0
/* The bootloader alarm window (W1-B) is not on the unit yet: Q1(2) */
#define ALARM_BOOT_FIX  0

/* the PMU's "no alarm": RTCWAK off, and 2099-01-01 00:00:00 with the year
 * at upstream's off value 0x99 (rtc_alarm_keep.h reads it as none) */
static const uint8_t pmu_off[7] = { 0x00, 0x00, 0x00, 0x07, 0x01, 0x01,
                                    0x99 };

static struct alarms_file af;           /* the resident copy (208 B) */
static bool loaded;
static bool dirty;                      /* af differs from alarms.dat */
static bool file_owed;                  /* a write deferred by USB (M9),
                                         * or by the pre-light pass */
static bool prelight;                   /* alarm_poll_prelight() running */
static uint32_t next_rtc;               /* armed now; 0 = none */
static long due_tick;                   /* next_rtc on the tick clock: only
                                           a wait hint, re-derived from the
                                           RTC at every wake (alarm_wake())
                                           because a soft sleep freezes the
                                           tick; the RTC decides the ring */
static bool wake_due;                   /* WAKE_ALARM: ring however late the
                                           RTC reads (the platform said so) */

static struct {
    bool on;
    bool usb;                           /* piezo only, no screen (Q7) */
    bool boot;                          /* an alarm boot, not started yet */
    uint8_t mask;                       /* the alarms this ring covers */
    uint8_t step;                       /* burst 0..3 of the pattern */
    uint32_t rtc;                       /* the occurrence */
    long start, beep_due;
} ring;

/* -- clock ------------------------------------------------------------------ */

static uint32_t rtc_now(void)
{
    struct tm t, *tm = &t;

    /* the RTC itself, not get_time()'s copy (up to 1.2 s old): Date &
     * Time's set_time() just before alarm_rearm() must count, or a clock
     * moved past an alarm re-arms it at the old time and it rings late */
    if (rtc_read_datetime(&t) < 0)
        tm = get_time();
    return alarm_rtc_make(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                          tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static void set_due(uint32_t now)
{
    uint32_t d = next_rtc > now ? next_rtc - now : 0;

    if (d > 30u * 86400)                /* far: look again in a month */
        d = 30u * 86400;
    due_tick = current_tick + (long)d * HZ;
}

/* -- alarms.dat ------------------------------------------------------------- */

static void file_write(void)
{
    int fd;
    bool ok;

    if (ui_usb_screen_active() || prelight)
    {
        /* the host owns the disk, or the screen is still dark at a wake
         * (no spin-up before the first lit frame): the PMU is armed
         * already, the file follows at the loop's next alarm_poll() */
        if (!file_owed)
            ui_log("alarm", "file owed%s", prelight ? " prelight" : "");
        file_owed = true;
        return;
    }
    alarms_file_seal(&af, af.count);
    mkdir(ALARMS_DIR);
    fd = open(ALARMS_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    ok = fd >= 0 && write(fd, &af, sizeof(af)) == (ssize_t)sizeof(af);
    if (fd >= 0 && close(fd) < 0)
        ok = false;
    if (ok && safefile_replace(ALARMS_TMP, ALARMS_DAT, ALARMS_OLD) < 0)
        ok = false;
    if (!ok)
    {
        safefile_remove(ALARMS_TMP, ALARMS_DAT);
        ui_log("alarm", "file write failed");
        file_owed = false;              /* no retry loop: still dirty, the
                                           next change writes it */
        return;
    }
    UPDATE_TRACE("alarm file write n=%d armed=%lu", (int)af.count,
                 (unsigned long)af.armed_rtc);
    dirty = false;
    file_owed = false;
}

static int file_read(void)
{
    int fd = open(ALARMS_DAT, O_RDONLY);
    ssize_t got;
    int rc;

    if (fd < 0)
    {
        safefile_recover(ALARMS_DAT, ALARMS_TMP, ALARMS_OLD);
        fd = open(ALARMS_DAT, O_RDONLY);
        if (fd < 0)
            return 1;                   /* none: the first boot */
    }
    safefile_remove(ALARMS_TMP, ALARMS_DAT);
    safefile_remove(ALARMS_OLD, ALARMS_DAT);
    got = read(fd, &af, sizeof(af));
    close(fd);
    rc = alarms_file_check(&af, got > 0 ? (uint32_t)got : 0);
    if (rc != ALARMS_OK)
    {
        ui_log("alarm", "file bad rc=%d bytes=%ld", rc, (long)got);
        memset(&af, 0, sizeof(af));
    }
    return rc;
}

/* -- the PMU ---------------------------------------------------------------- */

static bool pmu_arm(uint32_t at)
{
#ifdef HAVE_RTC_ALARM
    uint8_t want[7], got[7];
    bool wake = at != 0, gw = false;
    int tries;

    if (at)
        alarm_pmu_regs(at, ALARM_PMU_DAILY, want);
    else
        memcpy(want, pmu_off, sizeof(want));
    for (tries = 0; tries < 2; tries++)
    {
        /* the full image, then RTCWAK; read back (M9) */
        rtc_set_alarm_full(want, wake);
        if (rtc_get_alarm_full(got, &gw) == 0 && !memcmp(got, want, 7)
            && gw == wake)
            return true;
        ui_log("alarm", "arm readback %02x%02x%02x %02x%02x%02x%02x w=%d",
               got[2], got[1], got[0], got[3], got[4], got[5], got[6],
               (int)gw);
    }
    return false;
#else
    (void)at;
    return true;
#endif
}

void alarm_rearm(void)
{
    uint32_t now, next;
    bool ok;

    if (!loaded || ring.on || ring.boot)
        return;                 /* a ring re-arms at its end (8, item 8) */
    now = rtc_now();
    next = alarm_next(af.a, af.count, now, NULL);
    ok = pmu_arm(next);
    ui_log("alarm", "arm rtc=%lu ok=%d", (unsigned long)next, (int)ok);
    next_rtc = next;
    set_due(now);
    if (af.armed_rtc != next)
    {
        af.armed_rtc = next;
        dirty = true;
    }
    if (dirty)
        file_write();
}

/* -- boot ------------------------------------------------------------------- */

/* the alarms whose occurrence at or after 'at' is no later than 'now' */
static uint8_t due_mask(uint32_t at, uint32_t now)
{
    uint8_t m = 0;
    int i;

    for (i = 0; i < af.count; i++)
    {
        uint32_t t = alarm_next(&af.a[i], 1, at - 1, NULL);
        if (t && t <= now)
            m |= 1u << i;
    }
    return m;
}

#ifdef SIMULATOR
/* tests only (tests/ui, "# env:"): IPODUI_SIM_ALARM_IN=<s>[,<snoozes>]
 * replaces whatever the simdisk's alarms.dat held with one daily alarm
 * (at the boot's minute) whose snooze ends <s> seconds after boot
 * (nothing armed before), so a
 * script rings it at a known time without the editor, from a known state */
static bool sim_seed(uint32_t now)
{
    const char *e = getenv("IPODUI_SIM_ALARM_IN");
    struct alarm *a = &af.a[0];
    uint32_t at;
    const char *c;
    int y, mo, d, h, m, s;
    uint8_t r[7];

    if (!e)
        return false;
    /* once per run: only on the sim PMU's power-on image (every field
     * at its ignore value, year 0x99); a PMU this run has armed (a
     * "reboot" part) keeps the set the first part left */
    rtc_get_alarm_full(r, NULL);
    if (r[0] != 0x7f || r[6] != ALARM_REG_YEAR_OFF)
        return false;
    memset(&af, 0, sizeof(af));
    at = now + (uint32_t)atoi(e);
    /* the daily time is the boot's own minute, already begun, so its
     * next occurrence is tomorrow and the snooze end rings first, once
     * (at's minute rang up to 59 s early, at the minute after a second
     * ring followed: P12 integration) */
    alarm_rtc_split(now - now % 60, &y, &mo, &d, &h, &m, &s);
    memset(a, 0, sizeof(*a));
    a->on = 1;
    a->repeat = AR_DAILY;
    a->snooze = 1;
    a->hour = h;
    a->min = m;
    a->label = 1;
    a->snooze_until = at;
    c = strchr(e, ',');
    if (c)
        a->snoozes = atoi(c + 1);
    af.count = 1;
    UPDATE_TRACE("alarm sim seed at=%lu snoozes=%d", (unsigned long)at,
                 (int)a->snoozes);
    return true;
}
#endif

void alarm_init(void)
{
    uint32_t now, armed;
    int rc, i;

    if (loaded)
        return;
    memset(&af, 0, sizeof(af));
    rc = file_read();
    loaded = true;
    now = rtc_now();
#ifdef SIMULATOR
    if (sim_seed(now))
        rc = ALARMS_OK;
    {
        /* tests only: IPODUI_SIM_SLEEP_TIMER=<min> starts the Sleep Timer
         * at boot (the menu's least is 15 min, too long for a script) */
        const char *st = getenv("IPODUI_SIM_SLEEP_TIMER");
        if (st && atoi(st) > 0)
            set_sleeptimer_duration(atoi(st));
    }
#endif
    armed = af.armed_rtc;
    ui_log("alarm", "init n=%d armed=%lu now=%lu rc=%d", (int)af.count,
           (unsigned long)armed, (unsigned long)now, rc);
    if (rc < 0)
    {
        /* no alarms, and the file is rewritten from the empty state at
         * the next edit. The PMU is disarmed now: rtc_init()'s keep-alive
         * may have turned a passed one-shot into a daily match, which
         * would wake the unit every day with nothing to ring (review
         * LOW-3) */
        bool ok = pmu_arm(0);

        ui_log("alarm", "arm rtc=0 ok=%d (file bad)", (int)ok);
        next_rtc = 0;
        return;
    }
    if (alarm_is_boot(armed, now) && (ring.mask = due_mask(armed, now)))
    {
        /* an alarm power-on: the first loop pass rings it (alarm_poll()),
         * and nothing is re-armed or written until it ends, so a power
         * cut mid-ring rings again at the next boot inside the window */
        ring.boot = true;
        ring.rtc = armed;
        ui_log("alarm", "boot armed=%lu late=%lu", (unsigned long)armed,
               (unsigned long)(now - armed));
        return;
    }
    if (armed && now >= armed)
    {
        /* Q17: passed while nothing could ring; shown inline, not late */
        alarm_mark_missed(af.a, af.count, armed, now);
        for (i = 0; i < af.count; i++)
            if (af.a[i].missed && af.a[i].missed_at >= armed)
            {
                dirty = true;
                ui_log("alarm", "missed i=%d at=%lu", i,
                       (unsigned long)af.a[i].missed_at);
            }
    }
    alarm_rearm();
}

/* -- ring ------------------------------------------------------------------- */

void alarm_beep_burst(unsigned hz)
{
#ifdef HAVE_HARDWARE_CLICK
    piezo_play_for_tick(piezo_hz(hz), 0x80, HZ / 10);
#else
    (void)hz;
#endif
}

/* M1: the ring lights the screen with Hold on too (backlight_on() alone
 * obeys the hidden "backlight on Hold" setting, Off by default here, and
 * would leave a ring dark under Hold). M2: in the prelight pass (a wake's
 * first frame, ui_main.c) the light waits for that frame: owed here,
 * taken by ui_sleep_lit() after the redraw (alarm_take_light()). */
static bool light_owed;

static void ring_light(bool first)
{
    if (prelight)
    {
        light_owed = true;
        return;
    }
#ifdef HAS_BUTTON_HOLD
    if (first)
        ui_log("alarm", "lit hold=%d", button_hold() ? 1 : 0);
#else
    (void)first;
#endif
    backlight_on_through_hold();
}

bool alarm_take_light(void)
{
    bool owed = light_owed;

    light_owed = false;
    return owed;
}

static void ring_show(void)
{
    ring.usb = false;
    if ((audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
        == AUDIO_STATUS_PLAY)
        audio_pause();                  /* Q6: paused, its spot kept */
    ring_light(true);
    /* Hold on keeps the unlit face up whatever the light does (#65); the
     * ring goes over it. The face's own "leave without a key" event. */
    if (ui_unlit_active())
        ui_unlit_dismiss("alarm");
    if (ui_top()->ops != &ui_alarm_ring_screen)
        ui_push_t(&ui_alarm_ring_screen, NULL, UI_T_SYSTEM);
}

static void ring_start(uint32_t at, uint32_t now)
{
    uint8_t mask = ring.boot ? ring.mask : due_mask(at, now);
    int i;

    ring.boot = false;
    if (!mask)
    {
        alarm_rearm();
        return;
    }
    ring.on = true;
    xindex_cancel();                    /* the ring wins over a sync build */
    reset_poweroff_timer();             /* a ring is activity: the idle
                                           timer starts again from here */
    ring.mask = mask;
    ring.rtc = at;
    ring.step = 0;
    ring.start = ring.beep_due = current_tick;
    for (i = 0; !(mask & (1u << i)); i++)
        ;
    ui_log("alarm", "ring i=%d at=%lu late=%lu usb=%d", i,
           (unsigned long)at, (unsigned long)(now - at),
           (int)ui_usb_screen_active());
    if (ui_usb_screen_active())
        ring.usb = true;                /* Q7: piezo only */
    else
        ring_show();
}

static void ring_service(void)
{
    if (!ring.on)
        return;
    if (TIME_AFTER(current_tick, ring.start + ALARM_RING_S * HZ))
    {
        alarm_ring_end(ALARM_END_CAP);
        return;
    }
    if (ring.usb && !ui_usb_screen_active())
        ring_show();                    /* USB ended mid-ring: the screen */
    else if (!ring.usb && ui_unlit_active())
        ui_unlit_dismiss("alarm");      /* Hold on mid-ring */
    if (TIME_BEFORE(current_tick, ring.beep_due))
        return;
    /* 5.3: 4 x 100 ms with 100 ms gaps, then 800 ms of silence */
    if (ring.step == 0 && !ring.usb)
        ring_light(false);              /* lit while ringing, Hold or not */
    alarm_beep_burst(2000);
    ring.step = (ring.step + 1) & 3;
    ring.beep_due = current_tick + (ring.step ? HZ / 5 : HZ);
}

void alarm_ring_end(enum alarm_end how)
{
    uint32_t now = rtc_now();
    bool snooze = how == ALARM_END_SNOOZE || how == ALARM_END_CAP;
    bool snoozed = false;
    int i;

    if (!ring.on)
        return;
#ifdef HAVE_HARDWARE_CLICK
    piezo_stop();
#endif
    for (i = 0; i < af.count; i++)
    {
        struct alarm *a = &af.a[i];
        if (!(ring.mask & (1u << i)))
            continue;
        alarm_after_ring(a, now, snooze);
        if (a->snooze_until)
            snoozed = true;
        else if (how == ALARM_END_CAP)
        {
            a->missed = 1;              /* rang unanswered: 5.5 */
            a->missed_at = ring.rtc;
        }
    }
    ring.on = false;
    ring.usb = false;
    dirty = true;
    ui_log("alarm", "%s how=%d s=%ld", snoozed ? "snooze" : "dismiss",
           (int)how, (current_tick - ring.start) / HZ);
    alarm_rearm();
}

bool alarm_ring_can_snooze(void)
{
    int i = alarm_ring_index();

    return i >= 0 && af.a[i].snooze && af.a[i].snoozes < ALARM_SNOOZE_MAX;
}

int alarm_ring_index(void)
{
    int i;

    if (!ring.on)
        return -1;
    for (i = 0; i < af.count; i++)
        if (ring.mask & (1u << i))
            return i;
    return -1;
}

uint32_t alarm_ring_rtc(void)
{
    return ring.rtc;
}

bool alarm_ringing(void)
{
    return ring.on || ring.boot;
}

/* -- the main loop's side -------------------------------------------------- */

static void poll_due(void)
{
    uint32_t now;

    if (ring.boot)
    {
        ring_start(ring.rtc, rtc_now());
        return;
    }
    if (!next_rtc || TIME_BEFORE(current_tick, due_tick))
        return;
    now = rtc_now();
    if (now < next_rtc)
    {
        wake_due = false;
        set_due(now);                   /* the tick ran ahead of the RTC */
        return;
    }
    if (now - next_rtc > ALARM_BOOT_WIN_S && !wake_due)
    {
        /* far past (the clock moved, a frozen tick): not rung late (Q17) */
        alarm_mark_missed(af.a, af.count, next_rtc, now);
        dirty = true;
        ui_log("alarm", "late %lus: missed", (unsigned long)(now - next_rtc));
        alarm_rearm();
        return;
    }
    wake_due = false;
    ring_start(next_rtc, now);
}

void alarm_wake(bool alarm)
{
    uint32_t now;

    if (!loaded || ring.on || ring.boot)
        return;
    /* the tick stood still while asleep: the deadline again from the
     * RTC, so a wake short of the alarm rings it on time, not late by the
     * sleep's length (and not marked missed) */
    now = rtc_now();
    set_due(now);
    if (alarm && next_rtc && now + 2 >= next_rtc)
    {
        wake_due = true;                /* the first loop pass rings it */
        due_tick = current_tick;
    }
    UPDATE_TRACE("alarm wake alarm=%d next=%lu now=%lu", (int)alarm,
                 (unsigned long)next_rtc, (unsigned long)now);
}

void alarm_poll(void)
{
    if (!loaded)
        return;
    if (file_owed && !ui_usb_screen_active())
        file_write();                   /* the post-USB path (M9) */
    if (ring.on)
        ring_service();
    else
        poll_due();
}

void alarm_poll_prelight(void)
{
    if (!loaded)
        return;
    prelight = true;
    if (ring.on)
        ring_service();
    else
        poll_due();
    prelight = false;
}

void alarm_poll_usb(long btn)
{
    if (!loaded)
        return;
    if (ring.on)
    {
        /* Q7: any button stops it: the event the loop just took (a short
         * tap is released before button_status() could see it), or one
         * still held */
        if ((btn != BUTTON_NONE && !(btn & (SYS_EVENT | BUTTON_REL)))
            || button_status())
            alarm_ring_end(ALARM_END_USB_KEY);
        else
            ring_service();
        return;
    }
    poll_due();
}

long alarm_timeout(void)
{
    long d;

    if (!loaded)
        return TIMEOUT_BLOCK;
    if (ring.boot || (file_owed && !ui_usb_screen_active()))
        return 1;
    if (ring.on)
    {
        long cap = ring.start + ALARM_RING_S * HZ - current_tick;
        d = ring.beep_due - current_tick;
        if (cap < d)
            d = cap;
    }
    else if (next_rtc)
        d = due_tick - current_tick;
    else
        return TIMEOUT_BLOCK;
    return d < 1 ? 1 : d;
}

uint32_t alarm_next_rtc(void)
{
    return next_rtc;
}

bool alarm_blocks_poweroff(void)
{
    uint32_t now;

    if (!next_rtc || ALARM_BOOT_FIX)
        return false;
    now = rtc_now();
    return next_rtc <= now || next_rtc - now <= ALARM_HOLD_OFF_S;
}

void alarm_dark_wait(void)
{
    if ((audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
        == AUDIO_STATUS_PLAY)
        audio_pause();
    ui_log("alarm", "darkwait next=%lu", (unsigned long)next_rtc);
    ui_unlit_suppress_next();           /* dark, not the unlit face */
    backlight_off();
}

/* -- the Alarms module's line (W1-A2) --------------------------------------- */

int alarm_count(void)
{
    return af.count;
}

const struct alarm *alarm_get(int i)
{
    return i >= 0 && i < af.count ? &af.a[i] : NULL;
}

int alarm_set(int i, const struct alarm *a)
{
    if (i < 0)
    {
        if (af.count >= ALARM_MAX)
            return -1;
        i = af.count++;
    }
    else if (i >= af.count)
        return -1;
    af.a[i] = *a;
    dirty = true;
    alarm_rearm();
    return i;
}

void alarm_delete(int i)
{
    if (i < 0 || i >= af.count)
        return;
    memmove(&af.a[i], &af.a[i + 1], (af.count - i - 1) * sizeof(af.a[0]));
    af.count--;
    memset(&af.a[af.count], 0, sizeof(af.a[0]));
    dirty = true;
    alarm_rearm();
}

void alarm_seen(void)
{
    bool any = false;
    int i;

    for (i = 0; i < af.count; i++)
        if (af.a[i].missed)
        {
            af.a[i].missed = 0;
            any = true;
        }
    if (any)
    {
        dirty = true;
        file_write();
    }
}

void alarm_reset(void)
{
    if (ring.on)
        alarm_ring_end(ALARM_END_DISMISS);
    ring.boot = false;
    memset(&af, 0, sizeof(af));
    dirty = false;
    file_owed = false;
    safefile_remove(ALARMS_TMP, ALARMS_DAT);
    safefile_remove(ALARMS_OLD, ALARMS_DAT);
    remove(ALARMS_DAT);
    ui_log("alarm", "reset");
    alarm_rearm();                      /* the PMU off; nothing to write */
}
