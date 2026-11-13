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

/* Settings > Audiophile, the back end (#268, #270; contract audiophile.h).
 *
 * Native 88.2/96 kHz output runs the WM8758's SYSCLK at twice its rating
 * (the family's PLL with R6 MCLKDIV 1; b11 hr96 played 88.2 kHz clean on
 * one unit [silicon], 96 kHz is untested).  So:
 *   - a per-unit self-test gates each rate: the real output path at that
 *     rate for ~0.6 s of silence, passing only when the I2S drained at the
 *     rate it should (within 3 %): a codec whose PLL or SYSCLK failed stops
 *     clocking the I2S and the drain stops or slows;
 *   - a stall watch (a tick task, ~10 instructions a tick, acting only
 *     while the PCM plays above 48 kHz) treats 50 ms without a DMA piece
 *     as a codec fault: the high rates are off for the rest of the
 *     session, playback re-picks its rate (a short gap, resumed in place)
 *     and the shell shows its popup once;
 *   - not below 10 % battery;
 *   - nothing out of spec is stored but the setting and the test's mask:
 *     every boot starts at the codec's rated clock.
 * Every function runs on the CPU (the shell's calls cross by remote call,
 * apps/ipodui/febe_rpc.list); the two words the shell polls are uncached
 * with one writer. */

#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "audio.h"
#include "audio_thread.h"
#include "playlist.h"
#include "settings.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "pcm_sink.h"
#include "pcm_mixer.h"
#include "pcm_sampr.h"
#include "powermgmt.h"
#include "xcore.h"
#include "ipodui/uilog.h"
#include "dac_ext.h"
#include "audiophile.h"
#include "ipodui/np_info.h"

#ifdef HAVE_HIRATE_OUT

/* the shell polls these (direct calls from either core): one writer each */
static volatile uint8_t ap_failed XCORE_BSS;    /* a fault this session */
static volatile uint8_t ap_popup XCORE_BSS;     /* not yet acknowledged */
static volatile uint8_t ap_logged XCORE_BSS;    /* the fault line written */
static unsigned long ap_fault_hz XCORE_BSS;

#define AP_STALL_TICKS  (HZ / 20)   /* 50 ms without a DMA piece */

int audiophile_hirate_allowed(void)
{
    if (!global_settings.ipodui_hirate || ap_failed)
        return 0;               /* no battery read when off */
    return audiophile_allowed_rates(global_settings.ipodui_hirate,
                                    global_settings.ipodui_hirate_ok,
                                    ap_failed, battery_level());
}

static void ap_repick(void)
{
    audio_queue_post(Q_AUDIO_RATE_REPICK, 0);
}

/* p083-dacosr64: one line per rate the driver set, with R10's
   oversampling rate (the driver can't log; dac_ext.h) */
static void ap_log_osr(void)
{
    static unsigned int seen;
    struct dac_stats st;

    dac_get_stats(&st);
    if (st.rate_seq == seen || !st.osr)
        return;
    seen = st.rate_seq;
    ui_log("audio", "dac osr=%d rate=%lu", st.osr, st.rate_hz);
}

/* ---- the stall watch (tick) ------------------------------------------ */

static unsigned long ap_last_prog;
static int ap_stall;
#ifdef SIMULATOR
static int ap_sim_run;          /* PROKBOX_HIRATE_FAULT=run: ticks left */
#endif

static void ap_fault(void)
{
    ap_fault_hz = pcm_get_frequency();
    ap_failed = 1;
    ap_popup = 1;
    ap_repick();
}

static void ap_tick(void)
{
    if (ap_failed || !pcm_is_playing() || pcm_get_frequency() <= SAMPR_48 ||
        pcm_current_sink() != PCM_SINK_BUILTIN)
    {
        ap_stall = 0;
        ap_last_prog = pcm_builtin_progress();
        return;
    }
#ifdef SIMULATOR
    if (ap_sim_run > 0 && --ap_sim_run == 0)
    {
        ap_fault();
        return;
    }
#endif
    unsigned long p = pcm_builtin_progress();
    if (p != ap_last_prog)
    {
        ap_last_prog = p;
        ap_stall = 0;
    }
    else if (++ap_stall >= AP_STALL_TICKS)
        ap_fault();
}

/* ---- the self-test ---------------------------------------------------- */

#ifndef SIMULATOR
/* the silence the test plays: 512 bytes (128 16-bit frames), never
   written, so no cache maintenance; the counters the PCM callback (the
   FIQ, on the COP in layout A) writes are uncached */
static const uint32_t ap_silence[128] __attribute__((aligned(32)));
static volatile unsigned long ap_t_first XCORE_BSS, ap_t_last XCORE_BSS;
static volatile unsigned long ap_pieces XCORE_BSS;

static void ap_more(const void **start, size_t *size)
{
    unsigned long now = USEC_TIMER;

    if (!ap_pieces)
        ap_t_first = now;
    ap_t_last = now;
    ap_pieces++;
    *start = ap_silence;
    *size = sizeof(ap_silence);
}

static enum pcm_dma_status ap_status(enum pcm_dma_status status)
{
    return status;
}

/* One rate: ~0.6 s; pass when the drain rate is within 3 % of nominal */
static bool ap_try(unsigned long hz)
{
    unsigned long us, frames, got;
    long end;

    pcm_set_frequency(hz);
    pcm_apply_settings();
    ap_log_osr();
    pcm_builtin_set_depth(16);
    ap_pieces = 0;
    pcm_play_data(ap_more, ap_status, ap_silence, sizeof(ap_silence));
    end = current_tick + HZ * 6 / 10;
    while (TIME_BEFORE(current_tick, end))
        sleep(HZ / 20);
    pcm_play_stop();

    us = ap_t_last - ap_t_first;
    frames = ap_pieces > 1 ? (ap_pieces - 1) * (sizeof(ap_silence) / 4) : 0;
    got = us ? (unsigned long)((uint64_t)frames * 1000000 / us) : 0;
    ui_log("audio", "high rate test %lu Hz: %lu pieces, %lu Hz measured: %s",
           hz, ap_pieces, got,
           got * 100 >= hz * 97 && got * 100 <= hz * 103 ? "pass" : "FAIL");
    return got * 100 >= hz * 97 && got * 100 <= hz * 103;
}
#endif /* !SIMULATOR */

int audiophile_selftest(void)
{
    int mask = 0;
#ifdef SIMULATOR
    const char *f = getenv("PROKBOX_HIRATE_FAULT");

    sleep(HZ);      /* about as long as the device's test */
    mask = AP_RATE_88 | AP_RATE_96;
    if (f && !strcmp(f, "test96"))
        mask = AP_RATE_88;
    else if (f && !strcmp(f, "test"))
        mask = 0;
#else
    struct mp3entry *id3 = audio_current_track();
    bool playing = (audio_status() & AUDIO_STATUS_PLAY) &&
                   !(audio_status() & AUDIO_STATUS_PAUSE) && id3;
    unsigned long elapsed = playing ? id3->elapsed : 0;
    unsigned long offset = playing ? id3->offset : 0;
    unsigned long was_hz;
    int index = 0;

    if (pcm_current_sink() != PCM_SINK_BUILTIN)
        return global_settings.ipodui_hirate_ok;
    playlist_get_resume_info(&index);
    audio_stop();
    long end = current_tick + 2 * HZ;
    while (pcm_is_playing() && TIME_BEFORE(current_tick, end))
    {
        pcm_play_stop();        /* the mixer's silence tail: not needed */
        sleep(HZ / 50);
    }
    was_hz = pcm_get_frequency();
#ifdef HAVE_DAC_STANDBY
    dac_wake();
#endif
    if (ap_try(SAMPR_88))
        mask |= AP_RATE_88;
    if (ap_try(SAMPR_96))
        mask |= AP_RATE_96;
    /* the rated clock back through the driver (PLL, R6, R7) */
    pcm_set_frequency(was_hz);
    pcm_apply_settings();
    ap_log_osr();
    if (playing && playlist_amount() > 0)
        playlist_start(index < 0 ? 0 : index, elapsed, offset);
#endif
    mask |= AP_TESTED;
    global_settings.ipodui_hirate_ok = mask;
    if (!(mask & (AP_RATE_88 | AP_RATE_96)))
        global_settings.ipodui_hirate = false;
    settings_save();
    ui_log("audio", "high rate self-test: 88.2 %s, 96 %s",
           mask & AP_RATE_88 ? "pass" : "fail",
           mask & AP_RATE_96 ? "pass" : "fail");
    return mask;
}

void audiophile_set_hirate(bool on)
{
    if (on == global_settings.ipodui_hirate)
        return;
    global_settings.ipodui_hirate = on;
    settings_save();
    ui_log("audio", "high rate %s (allowed %d)", on ? "on" : "off",
           audiophile_hirate_allowed());
    ap_repick();
}

bool audiophile_fallback_pending(void)
{
    return ap_popup;
}

void audiophile_fallback_ack(void)
{
    ap_popup = 0;
}

void audiophile_service(void)
{
    ap_log_osr();
    if (ap_failed && !ap_logged)
    {
        ap_logged = 1;
        ui_log("audio", "high rate fault at %lu Hz: no output for 50 ms; "
               "standard rates for this session", ap_fault_hz);
    }
#ifdef SIMULATOR
    /* =run: arm once, when a high-rate track first plays */
    static bool armed;
    if (!armed && pcm_is_playing() && pcm_get_frequency() > SAMPR_48)
    {
        const char *f = getenv("PROKBOX_HIRATE_FAULT");
        armed = true;
        if (f && !strcmp(f, "run"))
            ap_sim_run = 2 * HZ;
    }
#endif
}

#endif /* HAVE_HIRATE_OUT */

#ifdef IPODUI_AUDIOPHILE   /* 0.9.0 (ipodvideo.h scope switch) */
/* ---- stereo width ----------------------------------------------------- */

/* the codec's 3D depth for each step (0..15); b11's depth3d row played
   0/5/10/15 */
static const uint8_t ap_width_depth[AP_WIDTH_STEPS] = { 0, 5, 10, 15 };

void audiophile_set_width(int level)
{
    if (level < 0 || level >= AP_WIDTH_STEPS)
        level = AP_WIDTH_OFF;
    global_settings.ipodui_width3d = level;
#if !defined(SIMULATOR)
    dac_set_width3d(ap_width_depth[level]);
#endif
    /* 0.8.3: the headroom the 3D needs, on the EQ's precut */
    dac_set_width_cut(audiophile_width_cut_db(ap_width_depth[level]));
    settings_save();
    ui_log("audio", "stereo width %d (3D depth %d, cut %d dB)", level,
           ap_width_depth[level],
           audiophile_width_cut_db(ap_width_depth[level]));
}

void audiophile_init(void)
{
    int level = global_settings.ipodui_width3d;

    if (level > 0 && level < AP_WIDTH_STEPS)
    {
#if !defined(SIMULATOR)
        dac_set_width3d(ap_width_depth[level]);
#endif
        dac_set_width_cut(audiophile_width_cut_db(ap_width_depth[level]));
        ui_log("audio", "stereo width %d (3D depth %d, cut %d dB)", level,
               ap_width_depth[level],
               audiophile_width_cut_db(ap_width_depth[level]));
    }
#ifdef HAVE_HIRATE_OUT
    tick_add_task(ap_tick);
#endif
}
#endif /* IPODUI_AUDIOPHILE */

#ifdef IPODUI_NP_INFO      /* 0.9.0 (ipodvideo.h scope switch) */
/* The Now Playing info page's "Output" line (#277, apps/ipodui/np_info.h):
 * the rate the PCM runs at and the frame format the mix runs in now (the
 * real 24-bit path, not the request: 16 for a 24-bit source when the
 * chain needs 16-bit frames). CPU; the shell's calls cross (febe). */
int audio_output_rate(void)
{
    return (int)mixer_get_frequency();
}

int audio_output_bits(void)
{
#ifdef HAVE_PCM_24BIT_OUT
    return mixer_output_s24() ? 24 : 16;
#else
    return 16;
#endif
}
#endif /* IPODUI_NP_INFO */
