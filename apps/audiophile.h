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

/* Settings > Audiophile (#268, #270): the back end the shell's menu calls.
 *
 * The contract between the shell (apps/ipodui/screens/, the menu, the
 * warning, the self-test screen and the fallback popup) and playback.
 * Every function here runs on the CPU: the shell's calls cross from the
 * COP by remote call (apps/ipodui/febe_rpc.list).
 *
 * 24-bit output (#268) has no control: it is always on for sources deeper
 * than 16 bits.  This header only covers the two user choices:
 *   - native 88.2/96 kHz output (Off by default; out of the codec's rated
 *     clock, so a per-unit self-test gates it and a runtime fault falls
 *     back to 44.1/48 kHz for the rest of the session);
 *   - the codec's own 3D stereo width (Off by default; experimental). */
#ifndef AUDIOPHILE_H
#define AUDIOPHILE_H

#include <stdbool.h>

/* global_settings.ipodui_hirate_ok: the rates this unit passed */
#define AP_RATE_88      0x01    /* 88.2 kHz passed the self-test */
#define AP_RATE_96      0x02    /* 96 kHz passed the self-test */
#define AP_TESTED       0x80    /* the self-test has run on this unit */

/* Stereo width steps (global_settings.ipodui_width3d) */
#define AP_WIDTH_OFF    0
#define AP_WIDTH_LOW    1
#define AP_WIDTH_MEDIUM 2
#define AP_WIDTH_HIGH   3
#define AP_WIDTH_STEPS  4

/* The output-rate rule (audiophile_core.c, pure, host-tested): the output
 * rate for a source at src_hz when the high rates in allowed (AP_RATE_*)
 * may be used.  Each family plays at its highest enabled rate, so every
 * conversion is a whole-number ratio:
 *   44.1 family: 44.1 native; 88.2 native (on) or 2:1 to 44.1;
 *     176.4 2:1 to 88.2 (on) or 4:1 to 44.1; 352.8 4:1 / 8:1;
 *   48 family:   48 native; 96 native (on) or 2:1 to 48; 192 2:1 / 4:1;
 *     384 4:1 / 8:1;
 *   low rates (8 .. 32 kHz) go up to 44.1 or 48, never to 88.2/96. */
unsigned long audiophile_pick_rate(unsigned long src_hz, int allowed);

/* The high rates a saved setting allows (AP_RATE_*): High Sample Rate on,
 * the rates this unit's self-test passed (ok_mask), no fault this session
 * (failed) and the battery at AP_BATTERY_MIN % or more (battery_pct < 0:
 * unknown).  0.8.3 builds the feature out (IPODUI_AUDIOPHILE undefined,
 * so no HAVE_HIRATE_OUT): 0 whatever was saved. */
#define AP_BATTERY_MIN  10      /* %: below it, the rated clock only */
int audiophile_allowed_rates(bool on, int ok_mask, bool failed,
                             int battery_pct);

/* The output depth for a source of bits (0: unknown) on this path: 24
 * when the source carries more than 16 bits and nothing in the chain
 * needs 16-bit frames (crossfade, another sink), else 16 (pure). */
int audiophile_pick_depth(int src_bits, bool crossfade, bool builtin_sink);

/* 0.8.3: the DAC precut (whole dB) for a 3D depth (0..15): 0/5/10/15 ->
 * 0/2/4/6; and the precut the codec applies with an EQ's own precut on
 * too: the sum, at most 12 (wm8758.c applies the same rule) */
int audiophile_width_cut_db(int depth);
int audiophile_precut_total(int eq_db, int width_db);

#ifndef AUDIOPHILE_CORE_ONLY
#ifndef HAVE_HIRATE_OUT
/* 0.8.3 (IPODUI_AUDIOPHILE off): the rated 44.1/48 kHz only.  The rest of
 * this header is the 0.9.0 feature and is built only with the switch on. */
static inline int audiophile_hirate_allowed(void) { return 0; }
#else
/* The high rates allowed now (AP_RATE_*): audiophile_allowed_rates() on
 * the saved setting, this session's fault and the battery */
int audiophile_hirate_allowed(void);

/* The per-unit self-test (~2 s): if music plays it is stopped and resumed
 * at the same place.  Each high rate runs ~0.6 s of silence through the
 * real output path at that rate; it passes when the I2S drained at the
 * rate it should (within 3 %) with no stall.  Stores and returns the
 * result (AP_TESTED | the AP_RATE_* that passed).  Simulator: both pass,
 * unless PROKBOX_HIRATE_FAULT is "test96" (96 fails), "test" (both fail). */
int audiophile_selftest(void);

/* Turns native high-rate output on or off (saved).  Playing music moves
 * to the new output rate at once (a short gap) */
void audiophile_set_hirate(bool on);

/* Stereo width, 0..3 (saved), applied at once */
void audiophile_set_width(int level);

/* A runtime fault at a high rate dropped playback to 44.1/48 kHz this
 * session and the user has not seen the popup yet.  The shell polls it
 * each UI pass, shows the popup once and calls audiophile_fallback_ack().
 * Simulator: PROKBOX_HIRATE_FAULT=run makes the first high-rate track
 * fall back about 2 s after it starts. */
bool audiophile_fallback_pending(void);
void audiophile_fallback_ack(void);

/* The back end's per-pass service (CPU, fe_be.c): runs a pending
 * fallback's rate change.  Cheap when nothing is pending. */
void audiophile_service(void);

/* Boot: applies the saved stereo width */
void audiophile_init(void);
#endif /* HAVE_HIRATE_OUT */
#endif /* AUDIOPHILE_CORE_ONLY */

#endif /* AUDIOPHILE_H */
