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

/* Program all 5 bands and the precut (whole dB of DAC digital attenuation,
 * 0..12, so a boost can't clip inside the codec; no make-up gain, as the
 * software EQ's precut). NULL turns the hardware EQ off: bands flat, no
 * precut, the bass/treble shadows restored. While it is on, upstream's
 * bass/treble writes only update their shadows. UI thread. */
void dac_eq_set(const struct dac_eq_band *band, int precut_db);

/* S1 spike only: the ADDCTRL M128ENB bit (kept across sample-rate changes) */
void dac_eq_set_m128(bool on);

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
};
void dac_get_stats(struct dac_stats *s);

#endif /* DAC_EXT_H */
