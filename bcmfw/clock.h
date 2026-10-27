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
 * bcmfw core clock: the crystal (27 MHz, as the host starts us) or PLL A
 * at 150 MHz (Apple's start-up sequence, device-verified by stage 4b in
 * session 7).  BCMFW_CLOCK builds only.
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
#ifndef BCMFW_CLOCK_H
#define BCMFW_CLOCK_H

#include "config.h"

#if BCMFW_CLOCK && !defined(__ASSEMBLER__)
#include <stdint.h>

/* What the switch read back (hardware.md, "Clocks, PLL and regulator") */
struct clock_readback
{
    uint32_t reg_before, reg_after;     /* 0x10001400 */
    uint32_t plla_m, plla_ctl, clk_sel; /* +0x00, +0x14, +0x28 */
};

#if !BCMFW_VCLK
/* step(n), n = 1..7, before each part of the switch (NULL: none).
 * Return 1, or 0 if a wait ended on its spin limit (the counter's clock
 * stopped).  Each also publishes BCMFW_MB_CLOCK_*. */
int clock_fast(void (*step)(uint32_t), struct clock_readback *rb);
int clock_slow(void);
#endif
int clock_is_fast(void);    /* on PLL A, at any band */
#if BCMFW_VCLK
/* P19 PW4: the level bands (clock.c band_mhz: the crystal, then 46, 59,
 * 74, 87, 102, 116, 123, 130, 140, 147 and 150 MHz, each at Apple's
 * level), and inside a band any whole MHz above the band below's top (the
 * PP's in-band policy).  clock_band: 0, or CLOCK_E_POINT (no such band:
 * nothing written), or CLOCK_E_STALL / CLOCK_E_SPEED (a wait ended on its
 * spin limit, or the core's speed was not f/27 of the crystal's: back on
 * the crystal at the level found before). */
#define CLOCK_BANDS    12
#define CLOCK_E_POINT  (-1)
#define CLOCK_E_STALL  (-2)
#define CLOCK_E_SPEED  (-3)
/* The band switch's record and the busy meter; HP_OP_VCLK's reply is its
 * first CLOCK_GOV_WORDS words, in order (hostproto.h HP_VCLK_R_*) */
struct clock_gov
{
    uint32_t band;              /* the band running */
    uint32_t mhz;               /* its clock (the band's top, or less) */
    uint32_t busy;              /* us busy: the decoder's packets (main.c
                                   around hp_work; wraps) */
    uint32_t now;               /* the 1 MHz counter at the reply */
    uint32_t regs, plls, fails; /* regulator writes, PLL locks, fallbacks */
};
#define CLOCK_GOV_WORDS 7
extern struct clock_gov clock_gov;
int clock_band(uint32_t b, uint32_t mhz);
uint32_t clock_mhz(void);
int clock_gov_set(uint32_t b, uint32_t mhz);
#endif
#endif

#endif /* BCMFW_CLOCK_H */
