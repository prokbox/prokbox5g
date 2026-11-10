/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright © 2010 Thomas Martitz
 * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
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

#include <SDL_audio.h>
#include "config.h"
#include "sound.h"
#include "pcm_sampr.h"
#ifdef HAVE_SW_VOLUME_CONTROL
#include "pcm_sw_volume.h"
#include "fixedpoint.h"
#endif

/**
 * Audio Hardware api. Make some of them do nothing as we cannot properly
 * simulate with SDL. if we used DSP we would run code that doesn't actually
 * run on the target
 **/
#ifdef HAVE_SW_VOLUME_CONTROL
static int sdl_volume_level(int volume)
{
    int shift = (1 - sound_numdecimals(SOUND_VOLUME)) << 16;
    int minvol = fp_mul(sound_min(SOUND_VOLUME), fp_exp10(shift, 16), 16);
    return volume <= minvol ? INT_MIN : volume;
}
#endif /* HAVE_SW_VOLUME_CONTROL */

#if defined(AUDIOHW_HAVE_MONO_VOLUME)
void audiohw_set_volume(int volume)
{
#ifdef HAVE_SW_VOLUME_CONTROL
    volume = sdl_volume_level(volume);
    pcm_set_master_volume(volume, volume);
#else
    extern void pcm_set_mixer_volume(int volume);
    pcm_set_mixer_volume(volume);
#endif
    (void)volume;
}
#else /* !AUDIOHW_HAVE_MONO_VOLUME */
#ifdef HAVE_IPOD_UI
/* hardware Sound Check (dac_ext.h): the trim rides the software volume
 * here, fitted and rounded as on the device (dac_gain.h) */
#include "dac_gain.h"
static int sdl_req_l = DAC_GAIN_MUTE, sdl_req_r = DAC_GAIN_MUTE;
static int sdl_gain, sdl_gain_ceil = DAC_GAIN_VOL_MAX, sdl_gain_applied;

static int sdl_gain_level(int vol, int *applied)
{
    int dac, amp, g = dac_gain_split(vol, sdl_gain, sdl_gain_ceil,
                                     &dac, &amp);
    if (applied)
        *applied = g;
    if (vol <= DAC_GAIN_MUTE)
        return vol;
    vol += g;
    return vol > DAC_GAIN_VOL_MAX ? DAC_GAIN_VOL_MAX : vol;
}
#endif

void audiohw_set_volume(int vol_l, int vol_r)
{
#ifdef HAVE_IPOD_UI
    sdl_req_l = vol_l;
    sdl_req_r = vol_r;
    vol_l = sdl_gain_level(vol_l, &sdl_gain_applied);
    vol_r = sdl_gain_level(vol_r, NULL);
#endif
#ifdef HAVE_SW_VOLUME_CONTROL
    vol_l = sdl_volume_level(vol_l);
    vol_r = sdl_volume_level(vol_r);
    pcm_set_master_volume(vol_l, vol_r);
#endif
    (void)vol_l; (void)vol_r;
}
#endif /* AUDIOHW_HAVE_MONO_VOLUME */

#if defined(AUDIOHW_HAVE_PRESCALER)
void audiohw_set_prescaler(int value)
{
#ifdef HAVE_SW_VOLUME_CONTROL
    pcm_set_prescaler(value);
#endif
    (void)value;
}
#endif /* AUDIOHW_HAVE_PRESCALER */

#ifndef HAVE_SW_TONE_CONTROLS
#if defined(AUDIOHW_HAVE_BASS)
void audiohw_set_bass(int value)        { (void)value; }
#endif
#if defined(AUDIOHW_HAVE_TREBLE)
void audiohw_set_treble(int value)      { (void)value; }
#endif
#endif /* HAVE_SW_TONE_CONTROLS */
#if defined(AUDIOHW_HAVE_BASS_CUTOFF)
void audiohw_set_bass_cutoff(int value) { (void)value; }
#endif
#if defined(AUDIOHW_HAVE_TREBLE_CUTOFF)
void audiohw_set_treble_cutoff(int value){ (void)value; }
#endif
/* EQ-based tone controls */
#if defined(AUDIOHW_HAVE_EQ)
void audiohw_set_eq_band_gain(unsigned int band, int value)
    { (void)band; (void)value; }
#endif
#if defined(AUDIOHW_HAVE_EQ_FREQUENCY)
void audiohw_set_eq_band_frequency(unsigned int band, int value)
    { (void)band; (void)value; }
#endif
#if defined(AUDIOHW_HAVE_EQ_WIDTH)
void audiohw_set_eq_band_width(unsigned int band, int value)
    { (void)band; (void)value; }
#endif
#if defined(AUDIOHW_HAVE_DEPTH_3D)
void audiohw_set_depth_3d(int value)
    { (void)value; }
#endif
#if defined(AUDIOHW_HAVE_LINEOUT)
void audiohw_set_lineout_volume(int vol_l, int vol_r)
    { (void)vol_l; (void)vol_r; }
#endif
#if defined(AUDIOHW_HAVE_FILTER_ROLL_OFF)
void audiohw_set_filter_roll_off(int value)
    { (void)value; }
#endif
#if defined(AUDIOHW_HAVE_POWER_MODE)
void audiohw_set_power_mode(int value)
    { (void)value; }
#endif

void audiohw_close(void) {}

#ifdef HAVE_IPOD_UI
/* dac_ext.h: the simulator keeps the codec state the shell traces
 * (docs/phase15-brief.md section 2.1) with the device driver's semantics
 * (drivers/audio/wm8758.c): standby once PCM has been stopped
 * (!pcm_is_playing(), so after the mixer's idle tail) for DAC_IDLE_MS, a
 * wake on any PCM start, eq_writes counting only the registers that
 * change. Nothing reaches SDL audio. */
#include "dac_ext.h"
#include "kernel.h"
#include "pcm.h"
#include "pcm-internal.h"

static struct dac_stats dac_sim;
static long dac_sim_busy_tick;      /* last time PCM ran or was about to */
static uint16_t dac_sim_eq[DAC_EQ_BANDS] =
    { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff };
static int dac_sim_precut;

void dac_eq_set(const struct dac_eq_band *band, int precut_db)
{
    int i;

    for (i = 0; i < DAC_EQ_BANDS; i++)
    {
        /* the same register image as the device: gain code, cutoff, BW */
        uint16_t r = band ? ((12 - MIN(MAX(band[i].gain, -12), 12)) & 0x1f)
                            | ((band[i].freq & 3) << 5)
                            | ((i > 0 && i < 4 && band[i].wide) << 8)
                          : 0x02c;
        if (dac_sim_eq[i] != r)
        {
            dac_sim_eq[i] = r;
            dac_sim.eq_writes++;
        }
    }
    precut_db = band ? MIN(MAX(precut_db, 0), 12) : 0;
    if (dac_sim_precut != precut_db)
    {
        dac_sim_precut = precut_db;
        dac_sim.eq_writes += 2;     /* LDACVOL, RDACVOL */
    }
    dac_sim.hw_eq = band != NULL;
}

void dac_eq_set_m128(bool on)
{
    dac_sim.m128 = on;
}

int dac_reglog_read(unsigned int *seq, uint16_t *out, int max)
{
    (void)seq; (void)out; (void)max;
    return 0;                       /* no codec registers here */
}

static bool dac_sim_allow = true;

void dac_standby_allow(bool allow)
{
    dac_sim_allow = allow;
    if (!allow)
        dac_wake();
}

bool dac_standby_allowed(void)
{
    return dac_sim_allow;
}

void dac_idle_poll(void)
{
    if (dac_sim.standby || !dac_sim_allow)
        return;
    if (pcm_is_playing())
        dac_sim_busy_tick = current_tick;
    else if (!TIME_BEFORE(current_tick,
                          dac_sim_busy_tick + DAC_IDLE_MS * HZ / 1000))
    {
        dac_sim.standby = true;
        dac_sim.sleeps++;
    }
}

bool dac_idle_pending(void)
{
    return !dac_sim.standby && dac_sim_allow;
}

void dac_busy_now(void)
{
    dac_sim_busy_tick = current_tick;
}

void dac_wake(void)
{
    dac_sim_busy_tick = current_tick;
    if (!dac_sim.standby)
        return;
    dac_sim.standby = false;
    dac_sim.wakes++;
    dac_sim.last_wake_us = 0;       /* no PLL to wait for */
}

void dac_set_gain_db10(int db10, int ceil_db10)
{
    if (db10 == sdl_gain && ceil_db10 == sdl_gain_ceil)
        return;
    sdl_gain = db10;
    sdl_gain_ceil = ceil_db10;
    audiohw_set_volume(sdl_req_l, sdl_req_r);
}

void dac_get_stats(struct dac_stats *s)
{
    *s = dac_sim;
    s->gain_db10 = sdl_gain;
    s->gain_applied_db10 = sdl_gain_applied;
}
#endif /* HAVE_IPOD_UI */

#ifdef CONFIG_SAMPR_TYPES
unsigned int pcm_sampr_to_hw_sampr(unsigned int samplerate,
                                   unsigned int type)
    { return samplerate; (void)type; }
#endif /* CONFIG_SAMPR_TYPES */
