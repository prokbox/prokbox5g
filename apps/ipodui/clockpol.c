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
 * iPod UI shell: the 24 MHz idle policy (Fit-1 C6, docs/fit1-brief.md
 * section C).
 *
 * Unboosted, the PP5022 runs from its 24 MHz crystal with the PLL powered
 * down unless something holds the PLL (firmware/system.c). The firmware
 * holds it itself for ATA DMA and USB, and firmware/pll_idle.c for a lit
 * screen and for PCM playing a track this file says needs more than
 * 24 MHz. Here: the track's needs (codec class, rate, speed, software
 * DSP: clockpol_core.c), a verdict per minute played at 24 MHz (the
 * PCM falling under pcmbuf's boost watermark means the codec needs
 * rescuing; a format that misses goes to 30 MHz for the rest of the
 * session), the developer switch, and a "clk" line in ipodui.log every
 * 5 min while the counters move (marks in power.csv on the rare events).
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

#ifdef HAVE_PLL_IDLE
#include <stdbool.h>
#include <stdio.h>
#include "system.h"
#include "kernel.h"
#include "audio.h"
#include "settings.h"
#include "sound.h"
#include "metadata.h"
#include "tdspeed.h"
#include "timefuncs.h"
#include "pll_idle.h"
#include "pcmbuf.h"
#include "backlight.h"
#include "ipodui/preview.h"
#include "ipodui/uilog.h"
#include "ipodui/powerlog.h"
#include "ipodui/clockpol.h"
#include "ipodui/clockpol_core.h"
#ifdef HAVE_PP_GOV
#include <stdarg.h>
#include "file.h"
#include "pgov.h"
/* The governor's markers: clockpol.h (written by the Developer row) */
#ifdef HAVE_PP_GOV_DVS
#include "ipodui/fe_be.h"       /* fe_be_mode(): apply refused on FE_COP */
#endif
#endif

#define CLK_LOG_TICKS   (5 * 60 * HZ)

static struct cp_state st;
static bool enabled;
static bool needs = true;           /* what pll_idle.c was last told */
static bool dirty = true;           /* re-read now: a strike, the switch */
static int cur_fmt = -1;
static long read_tick;

static struct {                     /* the minute being judged */
    bool open;
    bool lit;                       /* opened lit (lit24 only) */
    int fmt;
    unsigned long ticks0, boosts0, resc0, und0;
} win;

/* p19-battery: the lit clock (docs/design/lit-24mhz.md section 3).  The
 * developer switch "Lit screen: 30 / 24 MHz", not saved: 30 at every
 * boot, today's behaviour, until a meter shows the saving (bench row
 * "lit24", bench-p19.md).  At 24 the PLL is held lit only while the CPU
 * draws the drift, with a 2 s release, or after a light format missed
 * its minute in a lit window (lit_strike, for the session, while
 * something plays) */
#define CP_LIT_RELEASE  (2 * HZ)
static bool lit24;
static bool lit_strike;
static bool lit_told = true;        /* what pll_idle.c was last told */
static struct cp_lit lit = { true, 0 };

static unsigned long logged_entries = ~0ul;
static long log_tick;

/* Codecs that decode 44.1 kHz stereo under 24 MHz on the PP5022
 * (upstream's codec benchmarks). MP3 was judged with its synthesis on
 * the COP; since 19-1c it runs inline (~15-19 MHz with the DSP
 * [estimate], 60-80 % of 24 MHz: it fits dark, with little left for a
 * lit UI; np-lag's input boost covers that). The rest stay at 30 MHz. A
 * wrong entry costs boosts, never a dropout: the pcmbuf watermark
 * boosts, and the minute's verdict strikes it. */
static bool fmt_light(int fmt)
{
    switch (fmt)
    {
    case AFMT_MPA_L1: case AFMT_MPA_L2: case AFMT_MPA_L3:
    case AFMT_AIFF: case AFMT_PCM_WAV: case AFMT_WAVE64: case AFMT_AU:
    case AFMT_FLAC: case AFMT_WAVPACK: case AFMT_MP4_ALAC:
        return true;
    default:
        return false;
    }
}

static unsigned long underruns(void)
{
#ifdef HAVE_BCMFW_VIDEO
    return pcmbuf_underruns();      /* only these builds count them */
#else
    return 0;
#endif
}

static bool track_needs_pll(void)
{
    struct mp3entry *id3 = audio_current_track();
    struct cp_track t;

    if (!id3)
        return false;               /* nothing to decode */
    t.fmt = id3->codectype;
    t.light = fmt_light(t.fmt);
    t.rate = id3->frequency;
    t.speed_1x = true;
#ifdef HAVE_PITCHCONTROL
    t.speed_1x = sound_get_pitch() == PITCH_SPEED_100
        && (!dsp_timestretch_available()
            || dsp_get_timestretch() == PITCH_SPEED_100);
#endif
    t.dsp_heavy = global_settings.eq_enabled        /* software EQ only */
        || global_settings.crossfeed != 0
        || global_settings.surround_enabled != 0;
    cur_fmt = t.fmt;
    return !cp_fits(&st, &t);
}

static bool playing(void)
{
    int a = audio_status();

    return (a & AUDIO_STATUS_PLAY) && !(a & AUDIO_STATUS_PAUSE);
}

/* The lit need now; edge: a switch change, applied without the release */
static void lit_eval(bool edge)
{
    bool stream = ui_panel_drift_cpu();
    bool raw = cp_lit_need(!lit24, stream, lit_strike, playing());
    bool need;

    if (edge)
        lit.need = raw;
    need = cp_lit_step(&lit, raw, current_tick, CP_LIT_RELEASE);
    if (need == lit_told)
        return;
    lit_told = need;
    pll_idle_set_lit(need);
    ui_log("clk", "lit need=%d why=%s", (int)need, !lit24 ? "switch" :
           stream ? "stream" : lit_strike && need ? "strike" : "none");
}

static void tell(bool n)
{
    if (n != needs)
    {
        needs = n;
        pll_idle_set_load(n);
    }
}

static void log_line(const char *why)
{
    struct cpu_pll_stats ps;
    unsigned long now = current_tick;

    struct tm *tm = get_time();

    cpu_pll_get_stats(&ps);
    /* two lines: ui_log keeps 62 characters. rtc against the line's tick
     * seconds is the tick-accuracy check (docs/fit1-brief.md section C) */
    ui_log("clk", "%s rtc=%02d:%02d:%02d idle=%lu%% ent=%lu bfi=%lu",
           why, tm->tm_hour, tm->tm_min, tm->tm_sec,
           now ? ps.idle_ticks * 100 / now : 0, ps.idle_entries,
           ps.boosts_from_idle);
    ui_log("clk", "lock n=%lu us=%u max=%u holds=%x load=%d fmt=%d",
           ps.lock_count, ps.lock_us_last, ps.lock_us_max, ps.holds,
           (int)needs, cur_fmt);
    logged_entries = ps.idle_entries;
    log_tick = current_tick;
#ifdef HAVE_PP_GOV
    pgov_log_summary(why);          /* the session's time per band so far */
#endif
}

/* ui_main.c's track-change handler (the audio thread): the new track's
 * needs at once, since the UI thread may sleep for a minute while dark.
 * Strikes are written only by the UI thread; a read racing one just
 * decides from the previous set until the next re-read. */
void clockpol_track_changed(void)
{
    if (enabled)
        tell(track_needs_pll());
}

/* One window per format played at 24 MHz; any other state closes it */
static void judge(void)
{
    struct cpu_pll_stats ps;
    int a = audio_status();
    unsigned long idle_s, boosts, resc, und;

    bool lit_now = lit24 && is_backlight_on(true);

    if (!enabled || needs || !(a & AUDIO_STATUS_PLAY)
        || (a & AUDIO_STATUS_PAUSE))
    {
        win.open = false;
        return;
    }
    cpu_pll_get_stats(&ps);
    /* lit24: a window is lit or dark throughout (a lit edge closes it) */
    if (!win.open || win.fmt != cur_fmt || win.lit != lit_now)
    {
        win.open = true;
        win.lit = lit_now;
        win.fmt = cur_fmt;
        win.ticks0 = ps.idle_ticks;
        win.boosts0 = ps.boosts_from_idle;
        win.resc0 = pcmbuf_rescues();
        win.und0 = underruns();
        return;
    }
    idle_s = (ps.idle_ticks - win.ticks0) / HZ;
    /* S36: judged on the codec's rescues (pcmbuf.c), not on every boost
     * that started from 24 MHz: those were 56k library-scan transfers
     * in one minute, which struck FLAC while the codec kept up */
    resc = pcmbuf_rescues() - win.resc0;
    boosts = ps.boosts_from_idle - win.boosts0;
    und = underruns() - win.und0;
#ifdef HAVE_PP_GOV_DVS
    /* section 9.3: an underrun in a band-0 window is a stage-2 fault (a
     * no-op unless its apply mode is on): normal voltage, stage 1 */
    if (und)
        pgov2_fault(PGOV_FB_UNDERRUN);
#endif
    switch (cp_judge(idle_s, resc, und))
    {
    case CP_WAIT:
        break;
    case CP_OK:
        win.open = false;           /* a fresh window from the next pass */
        break;
    case CP_STRIKE:
    {
        char tag[16];
        if (win.lit)
        {
            /* missed at 24 MHz with the UI working beside it: lit and
             * playing goes back to 30 for the session; the format is
             * not struck for dark playback (lit-24mhz.md section 4) */
            lit_strike = true;
            win.open = false;
            ui_log("clk", "lit-strike fmt=%d idle_s=%lu resc=%lu und=%lu",
                   win.fmt, idle_s, resc, und);
            powerlog_mark("clk lit-strike");
            break;
        }
        cp_strike(&st, win.fmt);
        win.open = false;
        ui_log("clk", "strike fmt=%d idle_s=%lu resc=%lu bfi=%lu und=%lu",
               win.fmt, idle_s, resc, boosts, und);
        snprintf(tag, sizeof(tag), "clk strike %d", win.fmt);
        powerlog_mark(tag);
        dirty = true;
        break;
    }
    }
}

void clockpol_service(void)
{
    lit_eval(false);
    if (!enabled)
        return;
    if (dirty || TIME_AFTER(current_tick, read_tick + HZ))
    {
        dirty = false;
        read_tick = current_tick;
        tell(track_needs_pll());
    }
    judge();
    if (TIME_AFTER(current_tick, log_tick + CLK_LOG_TICKS))
    {
        struct cpu_pll_stats ps;
        cpu_pll_get_stats(&ps);
        if (ps.idle_entries != logged_entries)
            log_line("stats");
        else
            log_tick = current_tick;
    }
}

void clockpol_set_enabled(bool on)
{
    if (on == enabled)
        return;
    enabled = on;
    win.open = false;
    if (on)
    {
        dirty = true;
        clockpol_service();         /* the track's needs before the switch */
    }
    cpu_pll_hold(CPU_PLL_HOLD_DEV, !on);
    log_line(on ? "on" : "off");
    powerlog_mark(on ? "clk on" : "clk off");
}

bool clockpol_enabled(void)
{
    return enabled;
}

void clockpol_lit24_set(bool on)
{
    if (on == lit24)
        return;
    lit24 = on;
    win.open = false;
    lit_eval(true);
    ui_log("clk", "lit switch=%d", on ? 24 : 30);
    powerlog_mark(on ? "lit 24" : "lit 30");
}

bool clockpol_lit24(void)
{
    return lit24;
}

#ifdef HAVE_PP_GOV
/* The governor's lines (the power thread, a CPU thread) */
static void gov_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    ui_log_va("gov", fmt, ap);
    va_end(ap);
}
#endif

/* np-lag (docs/design/p19-placement.md section 4.4): a lit input boosts
 * while the codec decodes on the UI thread's core. The codec's band is
 * above the UI's, so on a shared core at 30 MHz a heavy decode (MP3 with
 * its synthesis inline since 19-1c: ~60-75 % of the core) leaves the UI
 * the scheduler's aged turns. At 80 MHz the decode's burst ends ~3x
 * sooner and the codec then sleeps on its full buffer (19-1c D), so the
 * UI has the core. Released CP_INPUT_HOLD after the last input, on the
 * main loop's next pass (clockpol_input_wait() bounds its sleep), or at
 * once by clockpol_input_drop(). Never while dark: no extra wakes in
 * dark playback (19-1a). With the UI on the COP (19-9) it applies too
 * (ucop-fix, 2026-10-26): one clock drives both cores, and the shell's
 * back-end calls run on the CPU beside the codec (ui_main.c
 * ui_competes_with_decode(); the note arrives by post, BE_INPUT_NOTE). */
#define CP_INPUT_HOLD   HZ
static struct cp_lit inb = { false, 0 };
static bool inb_held;
static unsigned long inb_count;     /* boosts taken (the probe's line) */
static long inb_t0;
static unsigned long inb_ticks;     /* ticks held */

static void inb_release(void)
{
    inb.need = false;
    if (!inb_held)
        return;
    inb_held = false;
    inb_ticks += current_tick - inb_t0;
    cpu_boost(false);
}

void clockpol_note_input(bool decode_here)
{
    if (!cp_input_boost(is_backlight_on(true), playing(), decode_here))
        return;
    cp_lit_step(&inb, true, current_tick, CP_INPUT_HOLD);
    if (inb_held)
        return;
    inb_held = true;
    inb_count++;
    inb_t0 = current_tick;
    cpu_boost(true);
}

int clockpol_input_wait(void)
{
    if (!inb_held)
        return TIMEOUT_BLOCK;
    if (is_backlight_on(true)
        && cp_lit_step(&inb, false, current_tick, CP_INPUT_HOLD))
        return MAX(inb.last + CP_INPUT_HOLD - current_tick, 1);
    inb_release();
    return TIMEOUT_BLOCK;
}

void clockpol_input_drop(void)
{
    inb_release();
}

void clockpol_input_stats(unsigned long *n, unsigned long *ticks)
{
    *n = inb_count;
    *ticks = inb_ticks + (inb_held ? current_tick - inb_t0 : 0);
}

void clockpol_init(void)
{
    clockpol_set_enabled(true);
#ifdef HAVE_PP_GOV
#ifdef HAVE_IPOD_UI_DEVMODE
    /* developer builds only: nothing in a release writes the markers, so
     * a stray file must not change a release's clock (devmode menu
     * re-layout, 2026-10-26) */
    if (file_exists(CLOCKPOL_PGOV_ON))
    {
        /* D1: band 0 lit or dark; the lit switch stays a developer floor
         * (it reads 24 MHz now; setting it to 30 holds the lit floor) */
        clockpol_lit24_set(true);
        pgov_enable(true);
    }
    pgov_set_log(gov_log);
#ifdef HAVE_PP_GOV_DVS
    /* after the log, so the stage-2 boot lines are kept */
    if (pgov_active() && file_exists(CLOCKPOL_PGOV2_ON))
    {
        bool apply = file_exists(CLOCKPOL_PGOV2_APPLY);

        /* review B1, belt and braces until silicon proves the cross-core
         * path: no apply while the shell runs (or will run) on the COP */
        if (apply && (fe_be_mode() == FE_COP || fe_be_mode_next() == FE_COP))
        {
            apply = false;
            ui_log("gov", "s2 apply refused: ui_split=cop");
        }
        pgov2_enable(apply ? PGOV2_APPLY : PGOV2_SHADOW);
    }
#endif
#else
    pgov_set_log(gov_log);
#endif /* HAVE_IPOD_UI_DEVMODE */
#endif
}
#endif /* HAVE_PLL_IDLE */
