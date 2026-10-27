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


/* Phase 18-2 (#86): the shell's sleep policy; see sleep.h and
 * docs/phase18-brief.md §4.3. One thread (the UI thread), no statics
 * beyond a few words, no allocation. */

#include "config.h"
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "backlight.h"
#include "lcd.h"
#include "powermgmt.h"
#include "power.h"
#include "ata_idle_notify.h"
#include "usb.h"
#include "audio.h"
#include "playlist.h"
#include "settings.h"
#include "storage.h"
#include "file.h"
#include "tagcache.h"
#include "timefuncs.h"
#include "buffering.h"
#include "pcm-internal.h"       /* pcm_is_playing() */
#include "ipodui/xindex.h"
#include "ipodui/sleep.h"
#include "ipodui/sleep_core.h"
#include "ipodui/usbbusy.h"
#include "ipodui/lib_shuffle.h"     /* lib_shuffle_settle() */
#include "ipodui/input.h"
#include "ipodui/maint.h"
#include "ipodui/unlit.h"
#include "ipodui/uilog.h"
#include "ipodui/fe_be.h"             /* P19 S2 */
#include "ipodui/powerlog.h"
#include "ipodui/battlog.h"
#include "ipodui/anim.h"
#include "ipodui/artcache.h"
#include "ipodui/alarm.h"
#include "ipodui/preview.h"     /* ui_fb_settle() (P19) */
#include "ipodui/alarm_ring.h"      /* alarm_dark_wait() */
#include "ipodui/sdk/sdk_int.h"     /* sdk_sleep_enter() */
#include "power_sleep.h"        /* SLEEP_REFUSE_*, struct sleep_req */
#ifdef HAVE_POWER_SLEEP
#include "stor_pwr.h"         /* #114: the drive's power-down */
#elif defined(SIMULATOR)
#include "sim_cmd_sleep.h"
#endif

extern unsigned short battery_level_disksafe;  /* powermgmt target file */

#define NO_REQUEST (-1)
static signed char request = NO_REQUEST;
static struct sleep_swallow swallow;
static bool running;            /* inside ui_sleep_run() */
static bool lcd_cut;            /* lcd_sleep() ran: lcd_awake() owed */
static bool window_open;        /* the entry window: boosted, maint open */
static long lit_from;           /* the tick a key ended the entry or the
                                 * sleep returned: "sleep lit ms=" */
static unsigned char boots_owed;    /* Q11: counted wakes, folded into the
                                     * boot stamp at the next entry */
static bool stay_dark;          /* power_sleep() refused twice: awake, the
                                 * backlight left off (review H1) */
static bool lit_through_hold;   /* P12 W2-W (owner): an alarm, USB or
                                 * charger ended it: lit once with Hold on */
#ifdef HAVE_SW_POWEROFF
static bool sw_off_owed, sw_off_val;    /* restored at PLAY's release (L3) */
#endif

/* The pause after a refused power_sleep() before its one retry */
#define SLEEP_RETRY_MS      1000

void ui_sleep_request(enum ui_sleep_reason r)
{
    /* a request from inside the entry (the idle timer firing in the
     * window) would sleep again at once after the wake */
    UPDATE_TRACE("sleep request why=%d running=%d", (int)r, (int)running);
    if (!running && (unsigned)r <= SLEEP_UNPLUGGED)
        request = (signed char)r;
}

bool ui_sleep_pending(void)
{
    return request != NO_REQUEST;
}

/* -- tier ------------------------------------------------------------------ */

/* The developer switch: soft sleep only when asked for (default Off) */
static bool soft_on;

void ui_sleep_soft_boot(void)
{
#ifdef HAVE_IPOD_UI_DEVMODE
    soft_on = file_exists(SOFT_SLEEP_MARKER);
#else
    soft_on = false;    /* nothing in a release writes the marker: a stray
                         * file must not change a release (2026-10-26) */
#endif
}

bool ui_sleep_soft_get(void)
{
#ifdef SIMULATOR
    if (sim_sleep_mode >= 0)        /* a script's sleep_mode soft|off */
        return sim_sleep_mode != 0;
#endif
    return soft_on;
}

void ui_sleep_soft_set(bool on)
{
    soft_on = on;
    if (on)
    {
        int fd = creat(SOFT_SLEEP_MARKER, 0666);
        if (fd >= 0)
            close(fd);
    }
    else
        remove(SOFT_SLEEP_MARKER);
    ui_log("sleep", "soft %d", on ? 1 : 0);
}

static bool sleep_supported(void)
{
    if (!ui_sleep_soft_get())
        return false;               /* Off: today's power-off, every path */
#ifdef HAVE_POWER_SLEEP
    return power_sleep_supported();
#elif defined(SIMULATOR)
    return true;
#else
    return false;                   /* no platform sleep: today's path */
#endif
}

static uint16_t low_mv(void)
{
    return sleep_low_mv(battery_level_disksafe);
}

static enum sleep_pick pick(void)
{
    struct sleep_tier_in in = {
        .mv = battery_voltage(), .pct = battery_level(),
#if CONFIG_CHARGING
        .charger = charger_inserted(),
#endif
        .usb = usb_inserted(), .supported = sleep_supported(),
        .low_mv = low_mv(),
    };
    return sleep_tier_pick(&in);
}

enum ui_sleep_tier ui_sleep_tier(int mv, int pct, bool charger,
                                 bool usb, bool supported)
{
    struct sleep_tier_in in = {
        .mv = mv, .pct = pct, .charger = charger, .usb = usb,
        .supported = supported, .low_mv = low_mv(),
    };
    return sleep_tier_pick(&in) == SLEEP_PICK_SOFT ? TIER_SOFT : TIER_OFF;
}

uint32_t ui_sleep_rtc_now(void)
{
    struct tm *tm = get_time();

    return sleep_rtc_from_date(tm->tm_year + 1900, tm->tm_mon + 1,
                               tm->tm_mday, tm->tm_hour, tm->tm_min,
                               tm->tm_sec);
}

uint32_t ui_sleep_deep_after_s(void)
{
#ifdef IPOD_BATTERY_MAH
    return sleep_deep_after_s(IPOD_BATTERY_MAH, 0, 0);
#else
    return sleep_deep_after_s(BATTERY_CAPACITY_DEFAULT, 0, 0);
#endif
}

/* -- the swallow mask ------------------------------------------------------ */

static uint8_t btn_bits(long btn)
{
    uint8_t b = 0;

    if (btn & BUTTON_SELECT) b |= WBTN_SELECT;
    if (btn & BUTTON_RIGHT)  b |= WBTN_RIGHT;
    if (btn & BUTTON_LEFT)   b |= WBTN_LEFT;
    if (btn & BUTTON_PLAY)   b |= WBTN_PLAY;
    if (btn & BUTTON_MENU)   b |= WBTN_MENU;
    return b;
}

bool ui_sleep_swallow(long btn)
{
    uint8_t bits;
    bool drop = false;

#ifdef HAVE_SW_POWEROFF
    if (sw_off_owed && !(button_status() & BUTTON_PLAY))
    {
        sw_off_owed = false;            /* PLAY up: upstream's long hold
                                         * may act again (review L3) */
        button_set_sw_poweroff_state(sw_off_val);
    }
#endif
    if (!swallow.mask || (btn & SYS_EVENT))
        return false;
    bits = btn_bits(btn);
    for (uint8_t b = WBTN_SELECT; b <= WBTN_MENU; b <<= 1)
        if ((bits & b)
            && sleep_swallow_event(&swallow, b, (btn & BUTTON_REL) != 0))
            drop = true;
    if (drop)
        UPDATE_TRACE("sleep swallow btn=%#lx mask=%#x", (unsigned long)btn,
                     (unsigned)swallow.mask);
    return drop;
}

/* -- entry ----------------------------------------------------------------- */

/* the window's jobs (§4.3 step 2): what is owed now */
static unsigned jobs_owed(void)
{
    unsigned j = 0;

    if (maint_wanted_job(MAINT_COMMIT) || tagcache_ui_sleep_owed())
        j |= 1u << (tagcache_ui_sleep_rebuild() ? SJ_REBUILD : SJ_COMMIT);
    if (maint_wanted_job(MAINT_CHAPTERS))
        j |= 1u << SJ_CHAPTERS;         /* the art thread's book job */
    if (tagcache_ui_sleep_walking())
        j |= 1u << SJ_WALK;
    if (artcache_rotate_owed())
        j |= 1u << SJ_ROTATION;         /* Q8: #100's rotation, due by the
                                         * RTC (asleep included); the art
                                         * thread runs it in this window */
    return j;
}

static void jobs_log(unsigned jobs, const char *what)
{
    for (int j = 0; j < SJ_COUNT; j++)
        if (jobs & (1u << j))
            ui_log("sleep", "job=%s %s", sleep_job_name(j), what);
}

/* The entry window's one exit (§3.2 boost discipline): maint's window
 * closed and the boost released, once. Also input.c's, before the USB
 * handler runs (the USB screen blocks inside it): SQ_USB never carries
 * the boost or an open window into the session (Rev 1 M2) */
void ui_sleep_window_close(void)
{
    if (!window_open)
        return;
    window_open = false;
    maint_sleep_window(false);
    cpu_boost(false);
}

/* Services the UI queue in HZ/10 slices until 'deadline', the window's
 * jobs done ('jobs'), or PLAY released ('play'): a key other than PLAY,
 * or a charger, is SQ_WOKEN (the key swallowed to its release); USB is
 * SQ_USB, its handler already run by ui_input_get(). */
static enum sleep_quiesce entry_wait(long deadline, unsigned *jobs,
                                     bool play)
{
    struct ui_input in;

    for (;;)
    {
        if (ui_input_get(&in, HZ/10))
        {
            uint8_t b = btn_bits(in.raw);

            if (b && !(b & WBTN_PLAY) && !in.release && !in.repeat)
            {
                lit_from = current_tick;
                swallow.mask |= b;      /* its repeats and release */
                return SQ_WOKEN;
            }
        }
        else if (in.raw == SYS_USB_CONNECTED)
        {
            lit_through_hold = true;
            return SQ_USB;
        }
#if CONFIG_CHARGING
        else if (in.raw == SYS_CHARGER_CONNECTED)
        {
            lit_through_hold = true;
            return SQ_WOKEN;
        }
#endif
        if (jobs && *jobs && !jobs_owed())
        {
            jobs_log(*jobs, "ran");
            *jobs = 0;
            return SQ_SLEEP;
        }
        if (play && !(button_status() & BUTTON_PLAY))
            return SQ_SLEEP;
        if (TIME_AFTER(current_tick, deadline))
            return SQ_SLEEP;
    }
}

/* §4.3 step 3, the sleep-entry subset: each item calls what the
 * power-off path calls, never clean_shutdown() (no audio_stop(), no
 * shutdown intent, no commit cancel) */
static void persist(void)
{
    struct mp3entry *id3 = audio_status() ? audio_current_track() : NULL;

    if (id3)
        playlist_update_resume_info(id3);   /* charging_sleep_prepare()'s
                                             * order */
    lib_shuffle_settle();                   /* npfix review L1: an open
                                             * refill's record */
    playlist_sync(NULL);                    /* the control file's pending
                                             * writes (review L4) */
    status_save(true);                      /* resume point + settings */
    sdk_persist();                          /* P12: a game's record */
    tagcache_ui_sleep_prepare(id3);         /* the resume point, stats
                                             * drained, the boot stamp */
    powerlog_mark("sleep");
    battlog_end("sleep");                   /* P19 PW4: the session's line */
    fe_idle_flush();                        /* ab_store, power log ring
                                             * (P19 S2: on be_svc) */
    ui_log_flush();
}

/* review M1: the storage users quiesced from sleep entry to the wake
 * (the Sleep lab's too) */
void ui_sleep_storage_hold(bool hold)
{
    buffering_sleep_hold(hold);
    artcache_sleep_hold(hold);
    maint_sleep_hold(hold);
}

/* -- what power_sleep() checks (review H1) --------------------------------- */

int sleep_quiet_why(void)
{
#ifdef HAVE_POWER_SLEEP
    if (get_cpu_boost_counter() != 0 || cpu_pll_holds() != 0)
        return SLEEP_REFUSE_BOOST;
    if (cpu_frequency != CPUFREQ_DEFAULT)
        return SLEEP_REFUSE_CLOCK;
    if (pcm_is_playing())
        return SLEEP_REFUSE_PCM;
    return SLEEP_REFUSE_NONE;
#elif defined(SIMULATOR)
    return sim_sleep_refusal();
#else
    return pcm_is_playing() ? SLEEP_REFUSE_PCM : SLEEP_REFUSE_NONE;
#endif
}

int sleep_quiesce_wait(long deadline, enum sleep_quiesce (*slice)(void),
                       enum sleep_quiesce *q)
{
    int why;

    if (q)
        *q = SQ_SLEEP;
    while ((why = sleep_quiet_why()) != SLEEP_REFUSE_NONE
           && !TIME_AFTER(current_tick, deadline))
    {
        if (!slice)
            sleep(HZ/10);
        else if ((*q = slice()) != SQ_SLEEP)
            break;
    }
    return why;
}

bool sleep_busy_read(struct sleep_busy *b)
{
    struct artcache_build_status art;
    int why = sleep_quiet_why();

    artcache_build_status(&art);
#ifdef HAVE_POWER_SLEEP
    b->boost = get_cpu_boost_counter();
    b->holds = cpu_pll_holds();
    b->clock = cpu_frequency != CPUFREQ_DEFAULT;
#else
    b->boost = why == SLEEP_REFUSE_BOOST;   /* the sim's pll_hold */
    b->holds = 0;
    b->clock = false;
#endif
    b->pcm = why == SLEEP_REFUSE_PCM;
    b->disk = TIME_BEFORE(current_tick, storage_last_disk_activity() + HZ/5);
    /* a pass at work (a held one waits, unboosted, off the disk) */
    b->art = art.running && art.wait == ARTCACHE_BUILD_READY;
    b->maint = maint_active();
    return why != SLEEP_REFUSE_NONE || b->boost || b->holds || b->clock
        || b->pcm || b->disk || b->art || b->maint;
}

/* one HZ/10 slice of the UI queue (sleep_quiesce_wait()'s, at entry) */
static enum sleep_quiesce entry_slice(void)
{
    return entry_wait(current_tick + HZ/10, NULL, false);
}

static enum sleep_quiesce quiesce(enum ui_sleep_reason r, uint8_t *entry)
{
    enum sleep_quiesce q = SQ_SLEEP;
    unsigned jobs;
    bool walk;
    long end;

    /* step 1: dark at once (no unlit face: this is not a timeout) */
    ui_unlit_suppress_next();
#ifdef HAVE_POWER_SLEEP
    backlight_sleep_hold(true);         /* no LCD-sleep countdown racing
                                         * step 4 (Rev 4 N1) */
#endif
    backlight_off();
    ui_usbbusy_dark();                  /* 3.20: the window's work is
                                         * hidden; a wake counts afresh */

    /* step 2: the window, for owed jobs only (Q7), one boost pair with
     * this one exit. The Q11 wakes fold into the boot count first; a
     * check they (or the calendar) owe gets the walk's cap */
    jobs = jobs_owed();
    walk = tagcache_ui_sleep_walk(boots_owed);
    boots_owed = 0;
    if (walk)
        jobs |= 1u << SJ_WALK;
    if (jobs)
    {
        jobs_log(jobs, "owed");
        cpu_boost(true);
        maint_sleep_window(true);
        window_open = true;
        q = entry_wait(current_tick + (walk ? SLEEP_WALK_MS : SLEEP_MAINT_MS)
                       * HZ / 1000, &jobs, false);
        ui_sleep_window_close();
        if (jobs)
            jobs_log(jobs, "cut");      /* stays owed */
        if (q != SQ_SLEEP)
            return q;
    }

    /* step 1a: PLAY's release; still held after SLEEP_QUIESCE_MS, the
     * sleep starts with it (Rev 4 N6). Its own deadline (review L1): a
     * long hold leaves the steps below their whole bound */
    if (r == SLEEP_HOLD_PLAY)
    {
        q = entry_wait(current_tick + SLEEP_QUIESCE_MS * HZ / 1000, NULL,
                       true);
        if (q != SQ_SLEEP)
            return q;
    }
    *entry = btn_bits(button_status()) & WBTN_PLAY;

    /* SLEEP_QUIESCE_MS bounds steps 3-4 from here (review L1) */
    end = current_tick + SLEEP_QUIESCE_MS * HZ / 1000;

    /* review M1: nothing new reaches storage until the wake: no
     * background refill, no art file operation, no maint take or regrow
     * (its silent restart refills the paused queue) */
    ui_sleep_storage_hold(true);

    /* review M2: a commit still running is cut first and its stop
     * waited for, so its thread is free to drain the statistics below
     * (§4.3 step 3: suspended at its next checkpoint, its candidate
     * dropped, the update owed to a later window; its last stage
     * finishes) */
    tagcache_ui_sleep_cut(true);
    while (tagcache_ui_sleep_busy())
    {
        if (TIME_AFTER(current_tick, end))
        {
            ui_log("sleep", "refused why=busy");
            return SQ_OFF;
        }
        q = entry_wait(current_tick + HZ/10, NULL, false);
        if (q != SQ_SLEEP)
            return q;
    }

    /* step 3, then the drain checked: what a clean power-off keeps is in
     * the journal before storage goes off (checklist 8) */
    persist();
    while (!tagcache_ui_sleep_drained())
    {
        if (TIME_AFTER(current_tick, end))
        {
            ui_log("sleep", "refused why=stats");
            return SQ_OFF;              /* the power-off path keeps them */
        }
        q = entry_wait(current_tick + HZ/10, NULL, false);
        if (q != SQ_SLEEP)
            return q;
    }

    /* step 4: storage idle (a transfer under way ends; storage_power_down()
     * then holds the driver's lock, so nothing starts in the dark) */
    while (TIME_BEFORE(current_tick, storage_last_disk_activity() + HZ/5))
    {
        if (TIME_AFTER(current_tick, end))
        {
            ui_log("sleep", "disk busy");   /* the driver's lock waits */
            break;
        }
        q = entry_wait(current_tick + HZ/10, NULL, false);
        if (q != SQ_SLEEP)
            return q;
    }
    /* review H1: what power_sleep() checks (the pause's mixer tail, the
     * lit PLL hold, the clock, the boosts) within the same bound, the UI
     * queue serviced (a USB plug here meets a lit-capable LCD); still
     * not quiet, power_sleep() refuses and sleep_once() retries once */
    {
        int why = sleep_quiesce_wait(end, entry_slice, &q);

        if (q != SQ_SLEEP)
            return q;
        ui_log("sleep", "quiet why=%d", why);
    }
#ifdef HAVE_POWER_SLEEP
    lcd_sleep();
    lcd_cut = true;
    sleep_quiesce_wait(current_tick + HZ/2, NULL, NULL);   /* the cut's own
                                                             * boost, if any */
#endif
    if (button_queue_count())           /* a last look */
        return entry_wait(current_tick, NULL, false);
    return SQ_SLEEP;
}

/* -- sleep and wake -------------------------------------------------------- */

struct wake {
    enum sleep_wake why;
    uint32_t s;
    uint16_t mv;                        /* the sleep's last battery reading */
    uint8_t buttons;
};

/* The platform sleep. False: it refused twice (review H1): no sleep, and
 * never a power-off for a refusal; the caller stays awake and dark. A
 * refusal is not a deep wake: power_sleep() returns WAKE_DEEP for both,
 * only 'refused' tells them apart. */
static bool sleep_once(uint8_t entry, struct wake *w)
{
    struct sleep_core_req rq = {
        .deep_after_s = ui_sleep_deep_after_s(),
        .hold_wake_s = SLEEP_HOLD_WAKE_S,
        .low_mv = low_mv(), .poll_s = SLEEP_POLL_S,
        .buttons_at_entry = entry,
        /* P12 W2-W (5.1): the poll is programmed for min(poll, alarm -
         * now), so the alarm wakes it on time from the RTC alone; the
         * PMU is armed too (alarm_rearm()) for the deep tier's off */
        .alarm_rtc = alarm_next_rtc(),
    };
    int tries = 0;
#ifdef HAVE_POWER_SLEEP
    struct sleep_req prq;
    struct sleep_result res;
    int rc;

    /* review M1 (§4.2): FLUSH CACHE, STANDBY IMMEDIATE, the drive's power
     * off, and the driver's lock held to the wake: the idle power-off
     * would wait for a tick that stops in a moment (#114's shared path,
     * firmware/export/stor_pwr.h; rc 1: the developer switch keeps it in
     * standby) */
    rc = storage_power_down(STOR_PWR_R_SLEEP, STOR_PWR_KEEP_LOCK);
    ui_log("sleep", "drive off rc=%d", rc);
    for (;;)
    {
        prq = (struct sleep_req){
            .deep_after_s = rq.deep_after_s, .alarm_rtc = rq.alarm_rtc,
            .bcm_cut_after_s = rq.bcm_cut_after_s,
            .hold_wake_s = rq.hold_wake_s, .low_mv = rq.low_mv,
            .poll_s = rq.poll_s, .buttons_at_entry = rq.buttons_at_entry,
        };
        power_sleep(&prq, &res);
        if (res.refused)
        {
            ui_log("sleep", "refused why=%u try=%d", res.refused, tries);
            if (tries++ || button_queue_count())
                break;
            sleep_quiesce_wait(current_tick + SLEEP_RETRY_MS * HZ / 1000,
                               NULL, NULL);
            continue;
        }
        ui_log("sleep", "wake why=%d s=%lu polls=%u spurious=%u crc=%lu",
               res.why, (unsigned long)res.slept_s, res.timer_wakes,
               res.spurious, (unsigned long)res.sdram_crc_ok);
        if (res.why != WAKE_BCM_CUT)
            break;
        lcd_sleep();
        sleep_req_remaining(&rq, res.slept_s, &rq);
    }
    storage_power_release();    /* the first access powers it on */
#elif defined(SIMULATOR)
    struct sim_sleep_result res;

    for (;;)
    {
        sim_sleep_run(&rq, &res);
        if (!res.refused)
            break;
        ui_log("sleep", "refused why=%u try=%d", res.refused, tries);
        if (tries++ || button_queue_count())
            break;
        sleep_quiesce_wait(current_tick + SLEEP_RETRY_MS * HZ / 1000,
                           NULL, NULL);
    }
#endif
#if defined(HAVE_POWER_SLEEP) || defined(SIMULATOR)
    if (res.refused)
    {
        *w = (struct wake){ WAKE_NONE, 0, 0, 0 };
        return false;
    }
    *w = (struct wake){ res.why, res.slept_s, res.last_mv,
                        res.wake_buttons };
    return true;
#else
    (void)rq; (void)tries;
    *w = (struct wake){ WAKE_DEEP, 0, 0, 0 };
    return true;
#endif
}

/* upstream's PLAY long-hold power-off, restored once PLAY is up (L3) */
static void sw_off_restore(bool v)
{
#ifdef HAVE_SW_POWEROFF
    if (button_status() & BUTTON_PLAY)
    {
        sw_off_val = v;
        sw_off_owed = true;             /* ui_sleep_swallow() restores */
    }
    else
    {
        sw_off_owed = false;
        button_set_sw_poweroff_state(v);
    }
#else
    (void)v;
#endif
}

/* P12 Q1(2): an alarm due within a day and no bootloader alarm window:
 * no power-off (Hold at the alarm's power-on would start the other
 * firmware), the dark wait instead. Every shell power-off route comes
 * here: hold PLAY (also ui_main.c), the idle timer, the Sleep Timer
 * (SLEEP_TIMER), a refused soft-sleep entry. Only the forced low-battery
 * shutdown (powermgmt.c) and the deep tier's own off still power off,
 * with RTCWAK armed. True: the dark wait was taken */
static bool alarm_holds_on(enum ui_sleep_reason r)
{
    if (!alarm_blocks_poweroff())
        return false;
    ui_log("sleep", "off held why=%d alarm=%lu", (int)r,
           (unsigned long)alarm_next_rtc());
    alarm_dark_wait();
    return true;
}

/* a power-off from the dark: black, unlit, so upstream's SYS_POWEROFF
 * lcd_awake() workaround shows nothing (Rev 4 N1) */
static void off_from_dark(void)
{
    ui_fb_settle(0, 0, LCD_WIDTH, LCD_HEIGHT);  /* P19 screen truth: a takeover */
    lcd_set_viewport(NULL);
    lcd_set_background(LCD_BLACK);
    lcd_clear_display();
    lcd_update();
    sys_poweroff();
}

bool ui_sleep_run(void)
{
    enum ui_sleep_reason r = (enum ui_sleep_reason)request;
    enum sleep_quiesce q;
    struct wake w = { WAKE_NONE, 0, 0, 0 };
    uint8_t entry = 0;
    bool sw_off = false;
    bool slept = false;

    request = NO_REQUEST;
    ui_fb_settle(0, 0, LCD_WIDTH, LCD_HEIGHT);  /* P19 screen truth: a takeover */
    lcd_cut = false;
    stay_dark = false;
    lit_through_hold = false;
    if (alarm_ringing())
    {
        /* P12: the idle timer or the Sleep Timer's end while ringing (hold
         * PLAY dismisses before it asks): the ring goes on */
        ui_log("sleep", "ignored why=%d ringing", (int)r);
        return false;
    }
    switch (pick())
    {
    case SLEEP_PICK_IGNORE:
        ui_log("sleep", "ignored why=%d", (int)r);
        return false;
    case SLEEP_PICK_OFF:
        if (alarm_holds_on(r))
            return false;
        ui_log("sleep", "tier=off why=%d", (int)r);
        sys_poweroff();             /* today's path, from the start */
        return false;
    case SLEEP_PICK_SOFT:
        break;
    }
    {
        /* P12 5.1: an alarm due within the entry window's longest job
         * (+5 s): no sleep, awake and dark, so the window never delays
         * the ring; the main loop's alarm deadline rings it */
        uint32_t at = alarm_next_rtc(), now = ui_sleep_rtc_now();

        if (at && at <= now + SLEEP_MAINT_MS / 1000 + 5)
        {
            ui_log("sleep", "refused why=alarm in=%ld",
                   (long)at - (long)now);
            alarm_dark_wait();
            return false;
        }
    }
    /* P12 5.6 (M10): the Sleep Timer counts in ticks, which stop while
     * asleep; the sleep did what it was for, and a frozen deadline would
     * fire at the wake */
    if (get_sleep_timer_active())
        set_sleeptimer_duration(0);
    if ((audio_status() & (AUDIO_STATUS_PLAY | AUDIO_STATUS_PAUSE))
        == AUDIO_STATUS_PLAY)
        audio_pause();              /* playing: paused first */
    ui_log("sleep", "enter why=%d batt=%d", (int)r, battery_level());

    /* PLAY held on in the dark is this sleep's hold, never upstream's
     * long-hold power-off (button.c HAVE_SW_POWEROFF; Rev 4 N6) */
#ifdef HAVE_SW_POWEROFF
    if (!sw_off_owed)
        sw_off = button_get_sw_poweroff_state();
    else
        sw_off = sw_off_val;            /* still owed from the last one */
    sw_off_owed = false;
    button_set_sw_poweroff_state(false);
#endif
    running = true;
    sdk_sleep_enter(r == SLEEP_TIMER);  /* P12: a game suspends; its
                                         * record goes with the sleep's
                                         * own saves */
    q = quiesce(r, &entry);
    if (q == SQ_SLEEP)
    {
        slept = sleep_once(entry, &w);
        lit_from = current_tick;
        /* P12 W2-W: the first lit frame's time (the header, Clocks, the
         * panels, the face) from the RTC, not get_time()'s copy from
         * before the tick stopped */
        get_time_stale();
        xindex_wake();                  /* L1: a month may have ended */
    }
    tagcache_ui_sleep_cut(false);       /* awake (or off): commits run */
    ui_sleep_storage_hold(false);
    switch (q)
    {
    case SQ_OFF:
        if (alarm_holds_on(r))
        {
            /* awake and dark through the common tail (the LCD back if
             * it was cut); the next key lights it (ui_sleep_lit()) */
            stay_dark = true;
            lit_from = 0;
            break;
        }
        running = false;
        sw_off_restore(sw_off);
        off_from_dark();
        return false;
    case SQ_USB:
    case SQ_WOKEN:
        ui_log("sleep", "abandoned q=%d", (int)q);
        break;
    case SQ_SLEEP:
        if (!slept)
        {
            /* review H1: refused twice: awake and dark (the ordinary
             * idle; the idle timer asks again later), never off */
            ui_log("sleep", "refused stay=awake");
            stay_dark = true;
            lit_from = 0;
            sleep_swallow_arm(&swallow, 0, 0, entry);
            break;
        }
        if (w.why == WAKE_DEEP || w.why == WAKE_LOWBATT)
        {
            ui_log("sleep", "deep s=%lu why=%d", (unsigned long)w.s,
                   (int)w.why);
            running = false;
            sw_off_restore(sw_off);
            off_from_dark();
            return false;
        }
        /* review M3, L2: the entry's stamp spoiled at the next natural
         * disk wake; a cut update waits for a natural moment */
        tagcache_ui_sleep_woke();
        if (w.why == WAKE_ALARM || w.why == WAKE_VBUS)
            lit_through_hold = true;    /* not a key: Hold can't stop it */
        if (sleep_counts_as_boot(w.why, w.s) && boots_owed < 255)
            boots_owed++;
        /* the waking press, still down for the driver
         * (clickwheel_sleep_exit()), and PLAY held into the sleep */
        sleep_swallow_arm(&swallow, w.buttons, w.buttons, entry);
        break;
    }
    running = false;
    sw_off_restore(sw_off);             /* L3: not while PLAY is down */
#ifdef HAVE_POWER_SLEEP
    /* frozen-kernel takeovers before the first frame (Rev 4 N1) */
    if (lcd_cut)
    {
        if (w.s >= 1)
            lcd_wake_aged();
        lcd_awake();                /* the resident image, from RAM */
        lcd_cut = false;
    }
    adc_invalidate();
    battery_wake_reading(w.mv);     /* 0: read now */
#endif
    /* timers the frozen tick left stale (Rev 1 M1) */
    reset_poweroff_timer();
    maint_pause_restart();
    /* P12 W2-W: the alarm's deadline again from the RTC, whatever woke
     * it; WAKE_ALARM rings at the loop's next pass (alarm_poll()) */
    alarm_wake(slept && w.why == WAKE_ALARM);
    return true;
}

void ui_sleep_lit(void)
{
    /* M2: a ring pushed by the prelight pass lights only now, after its
     * first frame: an alarm, so through Hold as below */
    if (alarm_take_light())
        lit_through_hold = true;
    /* review H1: a refused sleep stays dark; the next key lights it as
     * from any timeout. Not over an alarm, USB or a charger that came
     * meanwhile: those light, Hold or not (below). */
    if (stay_dark && !lit_through_hold)
    {
        stay_dark = false;
#ifdef HAVE_POWER_SLEEP
        backlight_sleep_hold(false);
#endif
        return;
    }
    stay_dark = false;
    /* P12 W2-W (owner): an alarm, USB or a charger lights the screen once
     * with Hold on too; a key under Hold stays dark (Hold's own rule) */
    if (lit_through_hold)
    {
        lit_through_hold = false;
        ui_log("sleep", "lit through hold=%d", button_hold() ? 1 : 0);
        backlight_on_through_hold();
    }
    else
        backlight_on();
    if (lit_from)
    {
        /* Rev 3 M2: a key in the entry window lights the screen within
         * 0.1 s, whatever the window's job is doing (it stops on its own
         * thread); a wake's figure is D4's software part */
        ui_log("sleep", "lit ms=%ld", (current_tick - lit_from) * 1000 / HZ);
        lit_from = 0;
    }
#ifdef HAVE_POWER_SLEEP
    backlight_sleep_hold(false);
#endif
}
