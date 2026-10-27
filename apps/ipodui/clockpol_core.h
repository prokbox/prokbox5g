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
 * iPod UI shell: the 24 MHz idle policy's decisions (Fit-1 C6,
 * docs/fit1-brief.md section C), pure logic with no Rockbox calls.
 * apps/ipodui/clockpol.c feeds it; tests/clockpol/ checks it on the host.
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
#ifndef _IPODUI_CLOCKPOL_CORE_H_
#define _IPODUI_CLOCKPOL_CORE_H_

#include <stdbool.h>
#include <stdint.h>

#define CP_MAX_RATE         48000   /* Hz; above it the decode cost grows */
#define CP_WINDOW_S         60      /* seconds at 24 MHz per verdict */
#define CP_MAX_BOOSTS_MIN   4       /* rescues per minute: the PCM fell
                                     * under pcmbuf's boost watermark */
#define CP_FMT_BITS         64      /* format ids tracked for strikes */

/* What plays now. light: the codec decodes 44.1 kHz stereo in well under
 * 24 MHz (clockpol.c's table, from upstream's PP5022 benchmarks). */
struct cp_track {
    int fmt;                /* the codec id (AFMT_*), opaque here */
    bool light;
    unsigned long rate;     /* Hz */
    bool speed_1x;          /* time-stretch and pitch both at 100 % */
    bool dsp_heavy;         /* software EQ, crossfeed or surround on */
};

struct cp_state {
    uint64_t strikes;       /* formats that missed 24 MHz this session */
};

/* True: this track may play at 24 MHz with the PLL off */
bool cp_fits(const struct cp_state *s, const struct cp_track *t);

enum cp_verdict {
    CP_WAIT,                /* not enough time at 24 MHz yet */
    CP_OK,                  /* kept up: start a new window */
    CP_STRIKE,              /* missed: this format goes to 30 MHz */
};

/* A window of playback at 24 MHz: idle_s seconds spent there, the boosts
 * that started from it and the PCM underruns meanwhile */
enum cp_verdict cp_judge(unsigned long idle_s, unsigned long boosts,
                         unsigned long underruns);

/* Records a strike for fmt (ids outside 0..CP_FMT_BITS-1 are ignored) */
void cp_strike(struct cp_state *s, int fmt);

/* ---- The lit clock (p19-battery; docs/design/lit-24mhz.md section 3) ----
 * Whether a lit screen holds the PLL (30 MHz) or lets the PP idle at
 * 24 MHz.  Held while: the developer switch says 30 (the default); a
 * sustained CPU push stream runs (the drift drawn by the CPU, not by the
 * BCM's jobs); or a light format missed its minute in a lit window this
 * session and something plays. */
bool cp_lit_need(bool switch_30, bool cpu_stream, bool lit_strike,
                 bool playing);

/* The hysteresis: taken at once, released only after `release` (any
 * time unit, e.g. ticks) without a need.  need starts true. */
struct cp_lit {
    bool need;
    long last;              /* when the need was last seen */
};

bool cp_lit_step(struct cp_lit *l, bool raw, long now, long release);

/* ---- The input boost (np-lag; docs/design/p19-placement.md section 4.4)
 * A lit input (a wheel step, a press) boosts the PP while the codec
 * decodes on the same core as the UI thread and something plays: the
 * codec's band (13) is above the UI's (16), so on a shared core a heavy
 * decode (MP3 with its synthesis inline, 19-1c) leaves the UI only the
 * scheduler's aged turns. Held with cp_lit_step() for CP_INPUT_HOLD
 * after the last input. Dark, paused, or the codec on the other core
 * (the UI on the COP, 19-9): no boost. */
bool cp_input_boost(bool lit, bool playing, bool decode_here);

#endif /* _IPODUI_CLOCKPOL_CORE_H_ */
