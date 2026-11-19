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
 * Codec extensions for the iPod shell (HAVE_IPOD_UI, docs/phase15-brief.md
 * section 2.1): the WM8758's 5-band EQ driven directly (not through
 * upstream's EQ_CAP settings path) and DAC standby while PCM is idle.
 * Implemented by drivers/audio/wm8758.c; the simulator's drivers/audio/sdl.c
 * keeps the state so the shell's traces and scripts run there too.
 *
 * Firmware can't trace: the shell reads dac_get_stats() and logs it.
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
#ifndef DAC_EXT_H
#define DAC_EXT_H

#include <stdbool.h>
#include <stdint.h>

#define DAC_EQ_BANDS    5

/* One band: gain -12..12 dB; freq 0..3 (the datasheet's four corner or
 * centre frequencies for that band); wide 0/1 (bands 1..3 only) */
struct dac_eq_band {
    int8_t gain;
    uint8_t freq;
    uint8_t wide;
};

/* Program all 5 bands, the precut (whole dB of DAC digital attenuation,
 * 0..12, so a boost can't clip inside the codec) and the make-up (#203:
 * tenth dB of gain, 0..10*precut, added to Sound Check's trim and fitted
 * with it under the Volume Limit, mostly on the analogue amp; it gives
 * back the loudness the precut took: eqlab_level() computes both). The
 * writes go quiet side first: the larger precut and smaller make-up
 * before the EQ registers, the new levels after. While the EQ is on and
 * dac_set_limiter_allow() allows it, the DAC limiter (-1 dB, R24/R25)
 * runs as the safety net. NULL turns the hardware EQ off: bands flat, no
 * precut or make-up, limiter off, the bass/treble shadows restored. While
 * it is on, upstream's bass/treble writes only update their shadows. UI
 * thread. */
void dac_eq_set(const struct dac_eq_band *band, int precut_db,
                int makeup_db10);

/* #203: the DAC limiter while the hardware EQ is on. Default on
 * (DAC_LIMITER_DEFAULT 1 in wm8758.c, from b18's eqlim and limon rows);
 * the bench flips it here. Not saved. Thread context (I2C) */
void dac_set_limiter_allow(bool allow);

/* #327 study: the DAC's auto-mute on digital silence (R10 AMUTE). Default
 * off (DAC_AMUTE_DEFAULT) until the bench's amute row (hiss, clicks) is
 * heard. Not saved. Thread context (I2C) */
void dac_set_amute(bool on);

/* 0.9.0 Settings > Audiophile > Hi-Res 128x (HAVE_HIRATE_OUT): R10
 * DACOSR at 88.2/96 kHz output, 64x by default (p083-dacosr64); the rates
 * in this mask run 128x instead.  It keys on the output rate, so a 176.4
 * or 192 kHz source converted to 88.2/96 gets it too.  Takes effect at the
 * next rate set, or at once when a high rate is set: a soft-mute ramp
 * around the R10 write while PCM runs (wm8758.c).  dac_stats.osr and
 * rate_seq follow, so the back end's "dac osr=" line shows the change.
 * Not saved here (the shell saves the setting). Thread context (I2C) */
#define DAC_OSR128_88   0x01
#define DAC_OSR128_96   0x02
void dac_set_osr128(unsigned int rates);

/* p090-osr128: two developer switches for the high rates (not saved; the
 * b17 row "128x at 96 kHz" sets them per arm and reads them back from
 * dac_get_stats()).  Each applies at once: while music plays at 88.2/96
 * kHz the change goes in under a soft-mute ramp (~30 ms), else at the next
 * rate set.  Release defaults from b18 (wm8758.c):
 *   dac_set_m128_hirate(): R7 M128ENB (the DSP core at full rate) while
 *     the output is 88.2/96 kHz at 128x; one more user of #265's OR
 *     (dac_stats.m128_hr_allow the switch, .m128 the bit).  Default on
 *     (DAC_M128_HIRATE_DEFAULT 1: half rate left a faint crackle in the
 *     tail, full rate played clean);
 *   dac_set_hirate_eq_guard(): the codec EQ held flat at 88.2/96 kHz
 *     whatever the shell asks (#352: the 48 kHz coefficients, corners an
 *     octave up); the shell's last request comes back below 88.2 kHz or
 *     when the guard goes off (dac_stats.hr_guard the switch, .hr_bypass
 *     in force now, .hw_eq what the codec holds).  Default off
 *     (DAC_HR_GUARD_DEFAULT 0: the shell pauses presets there itself,
 *     #352, and custom slots play at every rate, the owner's choice).
 *     The 3D stage is never held: Stereo Width
 *     plays at every rate (dac_set_width3d(), .width3d).
 * Thread context (I2C; sleeps with the codec mutex held while it ramps) */
void dac_set_m128_hirate(bool on);
void dac_set_hirate_eq_guard(bool on);
#ifdef SIMULATOR
/* the simulator's PCM sink reports each rate it opens (pcm-sdl.c) */
void dac_sim_rate_set(unsigned long hz);
#endif

/* #265 (b11 row m128, silicon): EQ4 has no effect in the DSP's low-power
 * mode (ADDCTRL M128ENB = 0), while EQ2 and EQ3 do (S1). dac_eq_set()
 * keeps M128ENB set while band 4 (index 3) has a gain, and clears it
 * otherwise (EQ off included), unless the 3D stage holds it
 * (dac_eq_set_m128()); the driver's shadow carries it across rate changes.
 * Pure: the host tests use it too. */
static inline bool dac_eq_wants_m128(const struct dac_eq_band *band)
{
    return band != NULL && band[3].gain != 0;
}

/* The 3D stage's request for ADDCTRL M128ENB (dac_set_width3d()).  The
 * bit is this request ORed with dac_eq_set()'s band 4 (#265), kept across
 * sample-rate changes */
void dac_eq_set_m128(bool on);

/* Settings > Audiophile: the codec's 3D stereo depth, 0 (off) .. 15; on
 * turns M128ENB on with it.  Not saved here (audiophile_set_width() saves
 * the shell's choice), so the bench may set a depth for one row and read
 * it back in dac_stats.width3d. Thread context (I2C) */
void dac_set_width3d(int depth);

/* 0.8.3: the stereo width's headroom, whole dB (0..12), added to the EQ's
 * precut on the same DAC volume, the sum at most 12 dB (the rule
 * apps/audiophile_core.c audiophile_precut_total() states, host-tested) */
void dac_set_width_cut(int db);

/* Hardware Sound Check (brief section 3.5, wave 2b): a gain trim in tenth
 * dB, applied by the codec with the volume (amp 1 dB, DAC 0.5 dB, the
 * total rounded to 0.5 dB; arithmetic in dac_gain.h) and on top of the EQ
 * precut, so the digital path stays bit-perfect. A boost is capped so
 * volume + trim never exceeds ceil_db10 (the shell passes the Volume
 * Limit; never above the volume max); the fitted value is in
 * dac_stats.gain_applied_db10 and is refitted at every volume change.
 * Thread context (I2C). */
void dac_set_gain_db10(int db10, int ceil_db10);

/* Standby (HAVE_DAC_STANDBY). dac_idle_poll() runs on the audio thread: it
 * enters standby once PCM has been stopped (!pcm_is_playing()) for
 * DAC_IDLE_MS, re-checking under the codec mutex. dac_wake() runs before
 * any PCM start, outside pcm_play_lock; a no-op when on. Both thread
 * context only. dac_idle_pending() tells the audio thread whether it needs
 * a timed wait. */
#define DAC_IDLE_MS     5000    /* ⚠️ brief section 9 Q1 */
void dac_idle_poll(void);
bool dac_idle_pending(void);
void dac_wake(void);
/* P19 19-1a (review L2): PCM has just stopped (an underrun's end of
 * data, any stop): the idle time counts from now, even if no poll saw
 * it playing lately. Any context (a tick store). */
void dac_busy_now(void);
/* ProkBox #312: the codec is in standby (or entering it), one uncached
 * load, any context: pcm_play_data() wakes it only then, since the mixer
 * calls it under pcm_play_lock.  With dac_busy_now() stored first, a poll
 * that has not set the flag yet re-reads the idle time and stays out */
bool dac_in_standby(void);

/* Developer switch (not saved; on at boot): false wakes the codec and
 * keeps it on, for an A/B of standby pops on the device */
void dac_standby_allow(bool allow);
bool dac_standby_allowed(void);

struct dac_stats {
    bool standby;
    bool hw_eq;
    bool m128;
    unsigned int sleeps, wakes;
    unsigned int last_wake_us;  /* PLL-lock-to-unmute of the last wake */
    unsigned int eq_writes;     /* register writes by dac_eq_set() */
    int gain_db10;              /* dac_set_gain_db10()'s request */
    int gain_applied_db10;      /* after the cap, at the current volume
                                 * (left channel) */
    /* p083-dacosr64: R10 DACOSR at the last rate set (64 or 128; 0 before
     * the first), that rate, and a count of rate sets and of Hi-Res
     * 128x OSR changes (dac_set_osr128()) */
    int osr;
    unsigned long rate_hz;
    unsigned int rate_seq;
    /* #203: the DAC precut applied (whole dB, EQ + width), the EQ's
     * make-up (tenth dB, before the Volume Limit fit), the limiter;
     * #327: AMUTE */
    int precut_db;
    int makeup_db10;
    bool limiter;
    bool amute;
    /* p090-osr128: the two high-rate switches, the EQ guard in force
     * now, and R41's 3D depth as written */
    bool m128_hr_allow;
    bool hr_guard;
    bool hr_bypass;
    int width3d;
};
void dac_get_stats(struct dac_stats *s);

#if defined(HAVE_IPOD_UI_DEVMODE) && defined(IPOD_VIDEO) && !defined(BOOTLOADER)
/* Developer-only (b17 bench, #438): the codec supply D1REGC1.  dac_rail_set()
 * (true 3.0 V = 0xf5, false 2.5 V = 0xf0) refuses (-1) while PCM plays and
 * the codec is awake; both return the code read back from the PMU, -1 on
 * failure.  The release default stays 3.0 V (pcf50605_init) */
int dac_rail_get(void);
int dac_rail_set(bool full);
#endif

#endif /* DAC_EXT_H */
