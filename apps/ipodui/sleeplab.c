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

/* Phase 18-1: the developer "Sleep lab" (docs/phase18-brief.md §4.0 D2,
 * docs/device-session-p18-0.md). Each row runs one soft sleep with a
 * fixed request, logs the result to ipodui.log and shows one line on the
 * lab's own row. Developer mode only, playback paused or stopped.
 *
 * The entry quiesces as the real one does (sleep.c), without its
 * persistence or the drive's power-off: the storage users held
 * (ui_sleep_storage_hold()), dark, then up to SLEEPLAB_QUIESCE_S for
 * everything power_sleep() refuses on plus the drive, the art builder
 * and maint (sleep_busy_read()), logging what is still busy. A button
 * or a USB/charger plug in that wait abandons the run; still busy at
 * its end, no sleep ("lab busy ..."). Every exit restores the same way:
 * the LCD, the backlight (through Hold too, so the result can be read),
 * the holds released, the waking press swallowed to its release, and the
 * system events the wait took posted back for the menu (a USB plug's
 * ack). The simulator runs the same code over its sleep stub
 * (sim_sleep_run(): blocks until a scripted wake, refuses while a
 * script's pll_hold stands or PCM runs). */

#include "config.h"
#include "ipodui/sleeplab.h"

#ifdef IPODUI_SLEEPLAB
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "bcmjob_pp.h"          /* P19: bcmjob_owner_event() */
#include "backlight.h"
#include "lcd.h"
#include "audio.h"
#include "powermgmt.h"
#include "storage.h"
#include "power_sleep.h"          /* SLEEP_REFUSE_*, sleep_refuse_name() */
#include "ipodui/uilog.h"
#include "ipodui/sleep.h"         /* sleep_busy_read(), the storage hold */
#include "ipodui/maint.h"
#ifdef HAVE_POWER_SLEEP
#include "dac_ext.h"
#else
#include "sim_cmd_sleep.h"        /* sim_sleep_run() */
#endif

static char last[64] = "No run yet";

const char *sleeplab_last(void)
{
    return last;
}

#ifdef HAVE_POWER_SLEEP
extern uintptr_t stackbegin[];
extern uintptr_t stackend[];

/* The main (UI) thread's stack high-water in bytes, from crt0's
 * 0xDEADBEEF fill */
static unsigned int main_stack_hw(void)
{
    const uintptr_t *p = stackbegin;

    while (p < stackend && *p == 0xDEADBEEF)
        p++;
    return (unsigned int)((stackend - p) * sizeof(*p));
}

/* The codec in standby, or standby turned off by its developer switch */
static bool dac_quiet(void)
{
    struct dac_stats d;

    dac_get_stats(&d);
    return d.standby || !dac_standby_allowed();
}
#endif

/* -- the queue while the lab owns the UI thread ----------------------------- */

/* System events the lab's waits took, posted back for the menu: a USB
 * plug must reach its handler (the USB thread waits for its ack) */
#define SAVED_MAX 4
struct saved {
    int n;
    long id[SAVED_MAX];
    intptr_t data[SAVED_MAX];
};

static void save_event(struct saved *sv, long id)
{
    if (sv->n < SAVED_MAX)
    {
        sv->id[sv->n] = id;
        sv->data[sv->n] = button_get_data();
        sv->n++;
    }
}

/* Empties the queue: buttons dropped, system events kept */
static void drain(struct saved *sv)
{
    while (button_queue_count() > 0)
    {
        long b = button_get(false);

        if (bcmjob_owner_event(b))
            continue;               /* P19: the job owner's pass */
        if (b != BUTTON_NONE && (b & SYS_EVENT))
            save_event(sv, b);
    }
}

/* Every button released, then the queue emptied of its presses and
 * releases: the row's own SELECT at entry; at the end the waking (or
 * abandoning) press, so the stock list never sees it (its SELECT release
 * is the list's OK: a waking SELECT would start the row again). After a
 * wake the driver sees the waking press only at its next tick (the tick
 * stood still; clickwheel_sleep_exit() left it down): 'wake' waits for
 * it first. Up to 30 s for the release. */
static void keys_up(struct saved *sv, bool wake)
{
    long end = current_tick + HZ/5;

    while (wake && button_status() == BUTTON_NONE
           && TIME_BEFORE(current_tick, end))
        sleep(1);
    end = current_tick + 30 * HZ;
    while (button_status() != BUTTON_NONE && TIME_BEFORE(current_tick, end))
        sleep(HZ/20);
    sleep(2);                   /* the release's tick has posted it */
    drain(sv);
}

/* The driver's buttons as the wheel's bits (>> 8); power_sleep() refuses
 * any but PLAY */
static uint8_t entry_buttons(void)
{
    int b = button_status();

    return ((b & BUTTON_SELECT) ? WBTN_SELECT : 0)
         | ((b & BUTTON_RIGHT) ? WBTN_RIGHT : 0)
         | ((b & BUTTON_LEFT) ? WBTN_LEFT : 0)
         | ((b & BUTTON_PLAY) ? WBTN_PLAY : 0)
         | ((b & BUTTON_MENU) ? WBTN_MENU : 0);
}

/* -- the entry's wait ------------------------------------------------------- */

enum lab_stop { LAB_QUIET, LAB_BUSY, LAB_KEY, LAB_EVENT };

static bool busy_same(const struct sleep_busy *a, const struct sleep_busy *b)
{
    return a->boost == b->boost && a->holds == b->holds
        && a->clock == b->clock && a->pcm == b->pcm && a->disk == b->disk
        && a->art == b->art && a->maint == b->maint;
}

/* Lines stay under the ring's 61 characters ("sleep " included):
 * "lab t=12 boost=..." while waiting, "lab busy boost=..." at its end */
static void log_busy(const char *what, const struct sleep_busy *b)
{
    ui_log("sleep", "lab %s boost=%d holds=%#x disk=%d pcm=%d art=%d"
           " maint=%d", what, b->boost, b->holds, b->disk, b->pcm, b->art,
           b->maint);
#ifdef HAVE_POWER_SLEEP
    if (b->clock)
        ui_log("sleep", "lab clock mhz=%ld", cpu_frequency / 1000000);
#endif
}

static enum lab_stop lab_wait(struct saved *sv, struct sleep_busy *b,
                              long *waited, long *event)
{
    const long start = current_tick;
    const long end = start + SLEEPLAB_QUIESCE_S * HZ;
    long next = start + HZ, logged = start;
    struct sleep_busy was;
    bool first = true;

    while (sleep_busy_read(b))
    {
        long btn;

        *waited = (current_tick - start) / HZ;
        if (TIME_AFTER(current_tick, end))
            return LAB_BUSY;
        if (!TIME_BEFORE(current_tick, next))
        {
            /* each second the state changed, else every SLEEPLAB_LOG_S:
             * the ring holds 12 lines and nothing flushes it here */
            if (first || !busy_same(b, &was)
                || current_tick - logged >= SLEEPLAB_LOG_S * HZ)
            {
                char t[24];

                snprintf(t, sizeof(t), "t=%ld", *waited);
                log_busy(t, b);
                was = *b;
                logged = current_tick;
                first = false;
            }
            next += HZ;
        }
        btn = button_get_w_tmo(HZ/10);
        if (btn == BUTTON_NONE || bcmjob_owner_event(btn))
            continue;               /* P19: the job owner's pass */
        if (btn & SYS_EVENT)
        {
            save_event(sv, btn);
            *event = btn;
            if (btn == SYS_USB_CONNECTED || btn == SYS_POWEROFF
#if CONFIG_CHARGING
                || btn == SYS_CHARGER_CONNECTED
#endif
                )
                return LAB_EVENT;
            continue;
        }
        /* a press (as the real entry: no wheel turn, no release) */
        if (!(btn & (BUTTON_REL | BUTTON_REPEAT))
            && (btn & ~(BUTTON_SCROLL_BACK | BUTTON_SCROLL_FWD)))
            return LAB_KEY;
    }
    *waited = (current_tick - start) / HZ;
    return LAB_QUIET;
}

/* -- the sleep -------------------------------------------------------------- */

/* power_sleep()'s result, or the simulator's (the same fields) */
struct lab_result {
    int why;
    unsigned long slept_s;
    unsigned timer_wakes, spurious, resyncs, wake_buttons, hold_edges;
    unsigned last_mv, refused;
    unsigned long crc;
};

static void lab_sleep(const struct sleep_core_req *crq, int test,
                      struct lab_result *r)
{
#ifdef HAVE_POWER_SLEEP
    struct sleep_lab lab;
    struct sleep_req rq;
    struct sleep_result res;
    unsigned int i;

    memset(&lab, 0, sizeof(lab));
    if (test == SLAB_POLL_WAITCNT)
        lab.timer = SLEEP_LAB_WAITCNT;
    lab.opto_kept = test == SLAB_WAKE_OPTO;
    rq = (struct sleep_req){
        .deep_after_s = crq->deep_after_s, .alarm_rtc = crq->alarm_rtc,
        .bcm_cut_after_s = crq->bcm_cut_after_s,
        .hold_wake_s = crq->hold_wake_s, .low_mv = crq->low_mv,
        .poll_s = crq->poll_s, .buttons_at_entry = crq->buttons_at_entry,
        .lab = &lab,
    };
    power_sleep(&rq, &res);
    *r = (struct lab_result){
        res.why, res.slept_s, res.timer_wakes, res.spurious, res.resyncs,
        res.wake_buttons, res.hold_edges, res.last_mv, res.refused,
        res.sdram_crc_ok };
    for (i = 0; i < lab.n; i++)
        ui_log("sleep", "poll i=%u asked=%u rtc=%u us=%lu", i,
               lab.poll[i].asked_s, lab.poll[i].rtc_s,
               (unsigned long)lab.poll[i].us);
#else
    struct sim_sleep_result res;

    (void)test;
    sim_sleep_run(crq, &res);
    *r = (struct lab_result){
        res.why, res.slept_s, res.timer_wakes, res.spurious, res.resyncs,
        res.wake_buttons, res.hold_edges, res.last_mv, res.refused,
        res.sdram_crc_ok };
#endif
}

static const char *wake_name(int why)
{
    switch (why)
    {
    case WAKE_BUTTON:   return "button";
    case WAKE_VBUS:     return "USB/charger";
    case WAKE_ALARM:    return "alarm";
    case WAKE_DEEP:     return "deep timer";
    case WAKE_LOWBATT:  return "low battery";
    case WAKE_HOLD:     return "Hold";
    case WAKE_BCM_CUT:  return "BCM cut";
    default:            return "?";
    }
}

/* "boost/disk/art": the busy parts, for the row */
static void busy_names(const struct sleep_busy *b, char *buf, size_t len)
{
    static const char * const n[] = {
        "boost", "pll", "clock", "pcm", "disk", "art", "maint" };
    const bool on[] = { b->boost != 0, b->holds != 0, b->clock, b->pcm,
                        b->disk, b->art, b->maint };
    size_t k = 0;

    buf[0] = '\0';
    for (unsigned i = 0; i < sizeof(on) / sizeof(on[0]) && k + 1 < len; i++)
        if (on[i])
            k += snprintf(buf + k, len - k, "%s%s", k ? "/" : "", n[i]);
}

int sleeplab_run(int test)
{
    struct sleep_core_req crq;
    struct lab_result r;
    struct sleep_busy b;
    struct saved sv = { .n = 0 };
    enum lab_stop stop;
    long waited = 0, event = 0;
    bool ran = false, lcd_cut = false;
#ifdef HAVE_POWER_SLEEP
    unsigned int hw0;
#endif

    memset(&r, 0, sizeof(r));
    if ((audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
        == AUDIO_STATUS_PLAY)
    {
        ui_log("sleep", "lab test=%d refused why=playing", test);
        snprintf(last, sizeof(last), "Didn't sleep: playing");
        return 0;
    }
    memset(&crq, 0, sizeof(crq));
    crq.poll_s = 60;
    crq.deep_after_s = 1800;
    crq.hold_wake_s = SLEEP_HOLD_WAKE_S;
    crq.low_mv = 3600;
    switch (test)
    {
    case SLAB_POLL_TIMER2:      /* D2 first: does the poll wake on time? */
        crq.poll_s = 30;
        crq.deep_after_s = 600;
        break;
    case SLAB_POLL_WAITCNT:     /* the same with PROC_WAIT_CNT seconds */
        crq.poll_s = 5;
        crq.deep_after_s = 120;
        break;
    case SLAB_WAKE_FAST:        /* D2 (viii): a press during a poll */
        crq.poll_s = 2;
        crq.deep_after_s = 600;
        break;
    case SLAB_ALARM:            /* an alarm 2 min ahead, on time */
        crq.alarm_rtc = ui_sleep_rtc_now() + 120;
        crq.deep_after_s = 600;
        break;
    case SLAB_DEEP:             /* D2 (vi): the deep tier, in the dark */
        crq.deep_after_s = 120;
        break;
    default:                    /* SLAB_WAKE, SLAB_WAKE_OPTO (its opto
                                 * kept powered: lab_sleep()) */
        break;
    }

    keys_up(&sv, false);        /* the row's own SELECT */
#ifdef HAVE_POWER_SLEEP
    hw0 = main_stack_hw();
#endif
    ui_log("sleep", "lab test=%d start", test);

    /* the real entry's order (sleep.c quiesce()): dark, nothing new to
     * storage, then wait for quiet */
    ui_sleep_storage_hold(true);
#ifdef HAVE_POWER_SLEEP
    backlight_sleep_hold(true);
#endif
    backlight_off();
#ifdef HAVE_POWER_SLEEP
    sleep(HZ/5);                /* the backlight thread turns it off */
    power_thread_kick();        /* pll_idle drops the lit hold now */
#endif
    stop = lab_wait(&sv, &b, &waited, &event);
#ifdef HAVE_POWER_SLEEP
    if (stop == LAB_QUIET)
    {
        lcd_sleep();            /* the BCM cut, on this thread */
        lcd_cut = true;
        sleep_quiesce_wait(current_tick + HZ/2, NULL, NULL);  /* the cut's
                                                                * own boost */
    }
#endif

    switch (stop)
    {
    case LAB_BUSY:
    {
        char names[40];

        log_busy("busy", &b);
        busy_names(&b, names, sizeof(names));
        snprintf(last, sizeof(last), "Didn't sleep: busy (%s)", names);
        break;
    }
    case LAB_KEY:
        ui_log("sleep", "lab abandoned why=key t=%ld", waited);
        snprintf(last, sizeof(last), "Didn't sleep: key pressed");
        break;
    case LAB_EVENT:
        ui_log("sleep", "lab abandoned why=event %#lx t=%ld",
               (unsigned long)event, waited);
        snprintf(last, sizeof(last), "Didn't sleep: %s",
                 event == SYS_POWEROFF ? "power off" : "USB/charger");
        break;
    case LAB_QUIET:
        crq.buttons_at_entry = entry_buttons();
#ifdef HAVE_POWER_SLEEP
        ui_log("sleep", "lab test=%d boost=%d holds=%#x mhz=%ld disk=%d",
               test, get_cpu_boost_counter(), cpu_pll_holds(),
               cpu_frequency / 1000000, storage_disk_is_active() ? 1 : 0);
        ui_log("sleep", "lab dac=%d keys=%#x waited=%ld", dac_quiet() ? 1 : 0,
               crq.buttons_at_entry, waited);
#else
        ui_log("sleep", "lab test=%d keys=%#x waited=%ld", test,
               crq.buttons_at_entry, waited);
#endif
        lab_sleep(&crq, test, &r);
        ran = true;
        ui_log("sleep", "wake why=%d refused=%u %s s=%lu", r.why, r.refused,
               sleep_refuse_name(r.refused), r.slept_s);
        ui_log("sleep", "wake polls=%u spurious=%u resyncs=%u buttons=%#x",
               r.timer_wakes, r.spurious, r.resyncs, r.wake_buttons);
        ui_log("sleep", "wake hold_edges=%u mv=%u crc=%lu", r.hold_edges,
               r.last_mv, r.crc);
        if (r.refused)
            snprintf(last, sizeof(last), "Refused: %s",
                     sleep_refuse_name(r.refused));
        else
            snprintf(last, sizeof(last), "Slept %lu s, woke: %s", r.slept_s,
                     wake_name(r.why));
        break;
    }
#ifdef HAVE_POWER_SLEEP
    ui_log("sleep", "stack hw=%u before=%u", main_stack_hw(), hw0);
#endif

    if (test == SLAB_DEEP && ran && r.why == WAKE_DEEP && !r.refused)
    {
        /* the deep tier as 18-2 runs it: storage free for the shutdown,
         * the SYS_POWEROFF handler's lcd_awake() pushes this black frame,
         * the backlight stays off */
        ui_sleep_storage_hold(false);
        ui_log("sleep", "deep s=%lu", r.slept_s);
        ui_log_flush();
        lcd_set_background(LCD_BLACK);
        lcd_clear_display();
        sys_poweroff();
        return 0;
    }

    /* every other exit: as after a real wake (sleep.c ui_sleep_run()) */
#ifdef HAVE_POWER_SLEEP
    if (lcd_cut)
    {
        if (r.slept_s >= 1)
            lcd_wake_aged();
        lcd_awake();
    }
    if (ran)
    {
        adc_invalidate();
        battery_wake_reading(r.last_mv);
    }
    backlight_sleep_hold(false);
#else
    (void)lcd_cut;
#endif
    ui_sleep_storage_hold(false);
    reset_poweroff_timer();
    if (ran && !r.refused)
        maint_pause_restart();  /* the tick stood still */
    backlight_on_through_hold();    /* lit with Hold on too: the result */
    keys_up(&sv, ran && !r.refused && r.wake_buttons);  /* the waking (or
                                 * abandoning) press: one, swallowed to its
                                 * release */
    for (int i = 0; i < sv.n; i++)
        button_queue_post(sv.id[i], sv.data[i]);
    /* the backlight thread has taken the BACKLIGHT_ON (keys_up() slept) */
    ui_log("sleep", "lab end lit=%d hold=%d", is_backlight_on(true) ? 1 : 0,
           button_hold() ? 1 : 0);
    ui_log_flush();
    return 0;
}
#endif /* IPODUI_SLEEPLAB */
