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
 * docs/fit1-brief.md section C). No Rockbox calls; host-tested in
 * tests/clockpol/.
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
#include "ipodui/clockpol_core.h"

static bool struck(const struct cp_state *s, int fmt)
{
    return fmt >= 0 && fmt < CP_FMT_BITS
        && (s->strikes & ((uint64_t)1 << fmt));
}

bool cp_fits(const struct cp_state *s, const struct cp_track *t)
{
    return t->light && t->rate <= CP_MAX_RATE && t->speed_1x
        && !t->dsp_heavy && !struck(s, t->fmt);
}

enum cp_verdict cp_judge(unsigned long idle_s, unsigned long boosts,
                         unsigned long underruns)
{
    if (underruns)                  /* a dropout: no second chance */
        return CP_STRIKE;
    if (idle_s < CP_WINDOW_S)
        return CP_WAIT;
    if (boosts * 60 > (unsigned long)CP_MAX_BOOSTS_MIN * idle_s)
        return CP_STRIKE;
    return CP_OK;
}

void cp_strike(struct cp_state *s, int fmt)
{
    if (fmt >= 0 && fmt < CP_FMT_BITS)
        s->strikes |= (uint64_t)1 << fmt;
}

bool cp_lit_need(bool switch_30, bool cpu_stream, bool lit_strike,
                 bool playing)
{
    return switch_30 || cpu_stream || (lit_strike && playing);
}

bool cp_lit_step(struct cp_lit *l, bool raw, long now, long release)
{
    if (raw)
    {
        l->need = true;
        l->last = now;
    }
    else if (l->need && now - l->last >= release)
        l->need = false;
    return l->need;
}

bool cp_input_boost(bool lit, bool playing, bool decode_here)
{
    return lit && playing && decode_here;
}
