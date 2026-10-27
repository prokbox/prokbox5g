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
 * ProkBox Fit-1 C6: 24 MHz idle with the PLL off (docs/fit1-brief.md
 * section C). Samples the two holders that have no event of their own,
 * the backlight and PCM, from the power thread's 2 Hz step, which runs
 * anyway: no new wake-ups. A lit screen holds the PLL (the UI animates
 * and draws over the BCM host port, about 10 PP cycles a pixel), and so
 * does a playing track the shell's policy says doesn't fit in 24 MHz.
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
#include "system.h"
#include "backlight.h"
#include "pcm.h"
#include "pcm-internal.h"               /* pcm_is_playing() */
#include "pll_idle.h"
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
#include "powermgmt.h"   /* P18 18-A: power_thread_kick() */
#endif

static bool load_needs_pll = true;
/* p19-battery (docs/design/lit-24mhz.md): whether a lit screen needs the
 * PLL, from the shell's policy.  True until it says otherwise: the lit
 * screen holds 30 MHz as it always did */
static bool lit_needs_pll = true;
static unsigned int sampled = ~0u;      /* the LIT|LOAD bits last applied */

void pll_idle_set_load(bool needs_pll)
{
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    bool changed = load_needs_pll != needs_pll;
#endif
    load_needs_pll = needs_pll;
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    if (changed)
        power_thread_kick();    /* P18 18-A: sample the new need now */
#endif
}

void pll_idle_set_lit(bool needs_pll)
{
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    bool changed = lit_needs_pll != needs_pll;
#endif
    lit_needs_pll = needs_pll;
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    if (changed)
        power_thread_kick();    /* sampled now, not at the next 0.5 s step */
#endif
}

void pll_idle_poll(void)
{
    unsigned int want = 0;

    /* the "Off" setting counts as dark */
    if (lit_needs_pll && is_backlight_on(true))
        want |= CPU_PLL_HOLD_LIT;
    if (load_needs_pll && pcm_is_playing())
        want |= CPU_PLL_HOLD_LOAD;
    if (want == sampled)
        return;
    sampled = want;
    /* take first, drop second: one switch at most per poll */
    if (want)
        cpu_pll_hold(want, true);
    cpu_pll_hold(~want & (CPU_PLL_HOLD_LIT|CPU_PLL_HOLD_LOAD), false);
}
