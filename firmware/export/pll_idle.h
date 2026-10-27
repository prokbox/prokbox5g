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
 * section C). firmware/system.c owns the hold mask and the switching;
 * this samples the holders nobody posts: the backlight and PCM.
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
#ifndef _PLL_IDLE_H_
#define _PLL_IDLE_H_

#include <stdbool.h>

#ifdef HAVE_PLL_IDLE
/* Whether the current track needs more than 24 MHz while PCM plays (the
 * shell's policy, apps/ipodui/clockpol.c). True until it first says. */
void pll_idle_set_load(bool needs_pll);
/* Whether a lit screen needs more than 24 MHz unboosted (the shell's lit
 * clock policy, apps/ipodui/clockpol.c; docs/design/lit-24mhz.md).  True
 * until it first says: today's 30 MHz whenever the backlight is on. */
void pll_idle_set_lit(bool needs_pll);
/* Thread context only (it may switch the clock): the power thread calls
 * it every POWER_THREAD_STEP_TICKS (0.5 s) */
void pll_idle_poll(void);
#endif

#endif /* _PLL_IDLE_H_ */
