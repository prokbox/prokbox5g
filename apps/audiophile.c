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
 *     in one run of the PCM (250 ms in the simulator, whose output clock
 *     is a host thread; see AP_STALL_TICKS) as a codec fault: the high
 *     rates are off for the rest of the
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
#include "corelayout.h"
#include "audiophile.h"
#include "ipodui/np_info.h"

#ifdef HAVE_HIRATE_OUT

/* the shell polls these (direct calls from either core): one writer each */
static volatile uint8_t ap_failed XCORE_BSS;    /* a fault this session */
static volatile uint8_t ap_popup XCORE_BSS;     /* not yet acknowledged */
static volatile uint8_t ap_logged XCORE_BSS;    /* the fault line written */
static unsigned long ap_fault_hz XCORE_BSS;

/* How long the output may go without a piece before it counts as a codec
 * fault.  The device: 50 ms, over three DMA pieces at 96 kHz (1,500
 * frames, 15.6 ms each), and the FIQ that counts them is never held off
 * that long.  The simulator's "DMA" is a host thread (SDL's audio
 * callback): at 96 kHz it pulls its pieces 20-40 ms apart (p090-apstall,
 * measured: 2-4 ticks, about 40 % of them 4), and a loaded host
 * deschedules the whole process for tens of ms more (the sim logs up to
 * 40 ms of run jitter), so 50 ms there tripped on host load alone and
 * turned Hi-Res off for the session.  250 ms covers that and still
 * catches a dead clock (PROKBOX_HIRATE_FAULT=stall) */
#ifdef SIMULATOR
#define AP_STALL_TICKS  (HZ / 4)
#else
#define AP_STALL_TICKS  (HZ / 20)
#endif
#define AP_STALL_MS     (AP_STALL_TICKS * 1000 / HZ)

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

/* A stall counts only inside one run of the PCM at one rate: the progress
 * word moves at every start as well as at every piece (pcm-pp.c and
 * pcm-sdl.c sink_dma_start()), and every tick that sees the PCM stopped,
 * at 48 kHz or below, or on another sink restarts the count.  A rate
 * change always stops the PCM first (mixer_set_frequency()), so the
 * re-clock, a track change, a pause's silence tail and an underrun's
 * silence (the mixer keeps the pieces coming) never count. */
static unsigned long ap_last_prog;
static int ap_stall;
#ifdef SIMULATOR
static int ap_sim_run;          /* PROKBOX_HIRATE_FAULT=run: ticks left */
static bool ap_sim_dead;        /* PROKBOX_HIRATE_FAULT=stall: the clock
                                 * died: the progress word stands still */
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
#ifdef SIMULATOR
    if (ap_sim_dead)
        p = ap_last_prog;
#endif
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
/* p090-osr128: the near-silent stretch of Hi-Res 128x's test: a square
   wave of +-1 LSB (16-bit, -96 dBFS, 750 Hz at 96 kHz), both channels.
   Never exact zeros, so the DAC's auto-mute (AMUTE) never engages and the
   interpolator and modulator idle on the smallest signal they get, where
   the owner heard the crackle (the silent tail of a 16/96 track) */
static const uint32_t ap_quiet[128] __attribute__((aligned(32))) = {
    [0 ... 63] = 0x00010001, [64 ... 127] = 0xffffffff
};
static const uint32_t * volatile ap_buf XCORE_BSS;
static volatile unsigned long ap_t_first XCORE_BSS, ap_t_last XCORE_BSS;
static volatile unsigned long ap_pieces XCORE_BSS;
static volatile unsigned long ap_gap_max XCORE_BSS;

static void ap_more(const void **start, size_t *size)
{
    unsigned long now = USEC_TIMER;

    if (!ap_pieces)
        ap_t_first = now;
    else if (now - ap_t_last > ap_gap_max)
        ap_gap_max = now - ap_t_last;
    ap_t_last = now;
    ap_pieces++;
    *start = ap_buf;
    *size = sizeof(ap_silence);
}

static enum pcm_dma_status ap_status(enum pcm_dma_status status)
{
    return status;
}

/* One rate: High Sample Rate's test ~0.6 s of silence; Hi-Res 128x's
   (p090-osr128) 4 s, its first AP_TEST_QUIET_MS the +-1 LSB stretch, the
   rest exact silence, so an intermittent fault has time to show.  The
   verdict is audiophile_rate_test_ok() (drain rate within 3 %, no gap of
   20 ms).  The log line adds what the PP side can see: the longest gap
   between pieces and, in layout A, the FIQs that found the I2S FIFO
   empty (e=).  The codec is write-only: nothing of its analogue output
   (a crackle) can be measured here */
static bool ap_try(unsigned long hz, bool longer)
{
    unsigned long us, frames, got, gap, empty = 0;
    long end, half;
    bool ok;
#ifdef HAVE_CORE_LAYOUT_A
    struct pcm_pp_stats st0, st1;
#endif

    pcm_set_frequency(hz);
    pcm_apply_settings();
    ap_log_osr();
    pcm_builtin_set_depth(16);
    ap_pieces = 0;
    ap_gap_max = 0;
    ap_buf = longer ? ap_quiet : ap_silence;
#ifdef HAVE_CORE_LAYOUT_A
    pcm_pp_get_stats(&st0);
#endif
    pcm_play_data(ap_more, ap_status, ap_buf, sizeof(ap_silence));
    end = current_tick + audiophile_test_ms(longer) * HZ / 1000;
    half = current_tick + AP_TEST_QUIET_MS * HZ / 1000;
    while (TIME_BEFORE(current_tick, end))
    {
        sleep(HZ / 20);
        if (longer && !TIME_BEFORE(current_tick, half))
            ap_buf = ap_silence;        /* the next piece: exact zeros */
    }
    pcm_play_stop();
#ifdef HAVE_CORE_LAYOUT_A
    pcm_pp_get_stats(&st1);
    empty = st1.fifo_empty - st0.fifo_empty;
#endif

    us = ap_t_last - ap_t_first;
    frames = ap_pieces > 1 ? (ap_pieces - 1) * (sizeof(ap_silence) / 4) : 0;
    got = us ? (unsigned long)((uint64_t)frames * 1000000 / us) : 0;
    gap = ap_gap_max;
    ok = audiophile_rate_test_ok(hz, got, gap);
    ui_log("audio", "high rate test %lu Hz%s: %lu pieces, %lu Hz measured, "
           "gap %lu us, e=%lu: %s", hz, longer ? " 128x 4 s" : "",
           ap_pieces, got, gap, empty, ok ? "pass" : "FAIL");
    return ok;
}
#endif /* !SIMULATOR */

/* Hi-Res 128x: the high rates that run the DAC at 128x now (the setting
   and the rates its own test passed); AP_RATE_* are DAC_OSR128_*'s bits */
#if AP_RATE_88 != DAC_OSR128_88 || AP_RATE_96 != DAC_OSR128_96
#error "AP_RATE_* and DAC_OSR128_* must match"
#endif
static unsigned int ap_osr128_rates(void)
{
    if (!global_settings.ipodui_osr128)
        return 0;
    return global_settings.ipodui_osr128_ok & (AP_RATE_88 | AP_RATE_96);
}

/* One self-test run: each high rate through the real output path with
   the DAC at 64x (High Sample Rate's test) or 128x (Hi-Res 128x's), the
   saved choice back after; returns AP_TESTED | the rates that passed */
static int ap_run_test(bool osr128)
{
    int mask = 0;
#ifdef SIMULATOR
    const char *f = getenv(osr128 ? "PROKBOX_OSR128_FAULT"
                                  : "PROKBOX_HIRATE_FAULT");

    sleep(osr128 ? 2 * HZ : HZ);    /* about as long as the device's */
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

    if (pcm_current_sink() != PCM_SINK_BUILTIN)    /* the stored result */
        return osr128 ? global_settings.ipodui_osr128_ok
                      : global_settings.ipodui_hirate_ok;
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
    dac_set_osr128(osr128 ? DAC_OSR128_88 | DAC_OSR128_96 : 0);
    if (ap_try(SAMPR_88, osr128))
        mask |= AP_RATE_88;
    if (ap_try(SAMPR_96, osr128))
        mask |= AP_RATE_96;
    /* the rated clock back through the driver (PLL, R6, R7), then the
       saved Hi-Res 128x choice (applied at the next high rate) */
    pcm_set_frequency(was_hz);
    pcm_apply_settings();
    dac_set_osr128(ap_osr128_rates());
    ap_log_osr();
    if (playing && playlist_amount() > 0)
        playlist_start(index < 0 ? 0 : index, elapsed, offset);
#endif
    return mask | AP_TESTED;
}

int audiophile_selftest(void)
{
    int mask = ap_run_test(false);

    global_settings.ipodui_hirate_ok = mask;
    if (!(mask & (AP_RATE_88 | AP_RATE_96)))
        global_settings.ipodui_hirate = false;
    settings_save();
    ui_log("audio", "high rate self-test: 88.2 %s, 96 %s",
           mask & AP_RATE_88 ? "pass" : "fail",
           mask & AP_RATE_96 ? "pass" : "fail");
    return mask;
}

int audiophile_osr128_selftest(void)
{
    int mask = ap_run_test(true);

    global_settings.ipodui_osr128_ok = mask;
    if (!(mask & (AP_RATE_88 | AP_RATE_96)))
        global_settings.ipodui_osr128 = false;
    settings_save();
    dac_set_osr128(ap_osr128_rates());
    ui_log("audio", "hi-res 128x self-test: 88.2 %s, 96 %s",
           mask & AP_RATE_88 ? "pass" : "fail",
           mask & AP_RATE_96 ? "pass" : "fail");
    return mask;
}

void audiophile_set_osr128(bool on)
{
    if (on == global_settings.ipodui_osr128)
        return;
    global_settings.ipodui_osr128 = on;
    settings_save();
    dac_set_osr128(ap_osr128_rates());
    ui_log("audio", "hi-res 128x %s (rates 0x%x)", on ? "on" : "off",
           ap_osr128_rates());
    ap_log_osr();
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
        ui_log("audio", "high rate fault at %lu Hz: no output for %d ms; "
               "standard rates for this session", ap_fault_hz, AP_STALL_MS);
    }
#ifdef SIMULATOR
    /* =run: arm once, when a high-rate track first plays (a forced
     * fault 2 s on); =stall: the progress word stops there instead, so
     * the watch itself must find it */
    static bool armed;
    if (!armed && pcm_is_playing() && pcm_get_frequency() > SAMPR_48)
    {
        const char *f = getenv("PROKBOX_HIRATE_FAULT");
        armed = true;
        if (f && !strcmp(f, "run"))
            ap_sim_run = 2 * HZ;
        else if (f && !strcmp(f, "stall"))
        {
            ap_sim_dead = true;
            ui_log("audio", "high rate sim: the output clock stops now");
        }
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
    dac_set_osr128(ap_osr128_rates());  /* Hi-Res 128x, saved */
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
