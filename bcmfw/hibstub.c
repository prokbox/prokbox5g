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

/* The hibernate park's stub (bcmfw/hibwake.h; power-contract.md section
 * 6.6).  NOT FOR SILICON until PW5 passes on the unit.
 *
 * Linked at HW_STUB and uploaded by the PP, outside every image, so one
 * copy serves the display-only and the merged image and costs neither
 * more than its PARK hook.  The image calls hw_entry() with interrupts
 * off, no transfer running and no session open; the stub reads the
 * parameter block at HW_PARAM and writes its results there.
 *
 * The park is PW5's (hostproto.c cmd_hib, the plain video image's probe)
 * without the probe's patterns, bars and allowlist: Apple's videocore=1
 * order (brief-map-power.md section 3, @14288), the PLLs B, C, D, then A
 * to pw 0 (the core already on the crystal, which the entry demands), the
 * 14 GPIO event words saved and cleared, then HP_HIB_PARKED and a busy
 * poll for HP_HIB_WAKE.  No SDRAM step: the hook refuses while SDRAM is
 * up.  The wake is PW5's restore (Apple's rule, @4cb8: each divider, its
 * control without the enable, 1 ms, then the enables; the event words
 * back), then the idle tick re-armed (the counter may have run or reset
 * in the dark, leaving compare 1 behind it: crt0.S's tick re-arms only
 * from a match) and the regions summed again.  A sum that differs means
 * SRAM didn't hold: HW_SPOILED, and the stub never returns into an image
 * it can't trust; the PP cuts and cold-boots.
 *
 * Nothing here touches a GPIO level or direction, so the panel's reset
 * line (GPIO 48) holds whatever the pins let it hold. */
#include <stdint.h>
#include "config.h"
#include "hw.h"
#include "hostproto.h"
#include "hibwake.h"

extern char __data_end[];
static void hw_entry(void);

/* the header, first at the base (.vectors): build.py checks its words */
const uint32_t hw_header[5] __attribute__((section(".vectors"))) =
{
    HW_STUB_MAGIC, HW_STUB_VERSION, HW_STUB, (uint32_t)__data_end,
    (uint32_t)hw_entry
};

#define P(off)      (*(volatile uint32_t *)(HW_PARAM + (off)))
#define HW_PW       0xA5A50000
#define HW_PLL_ON   0x400
#define HW_GPIO_EV  0x10002828          /* the 14 event words */
#define HW_CORE     0x10001C28
#define HW_STALL    1024                /* HP_PW_STALL_READS */
#define HW_PLL_US   1000                /* HP_PW_PLL_US */
#define HW_TICK_US  1000                /* config.h BCMFW_IDLE_TICK_US */

/* PW5's slots: the PLL words in the park's order (Apple's PLL i, 0-3 = B,
 * C, D, A, at +4 * ((i + 1) % 4); its divider, then its control at
 * +0x14), then the event words */
static uint32_t hw_addr(uint32_t s)
{
    if (s >= 8)
        return HW_GPIO_EV + 4 * (s - 8);
    return 0x10001C00 + 4 * ((s / 2 + 1) % 4) + (s & 1 ? 0x14 : 0);
}

/* us on the counter; 1 if it stopped first */
static uint32_t hw_wait(uint32_t us)
{
    uint32_t t0 = REG(TIMER_CLO), last = t0, now, same = 0;

    for (;;)
    {
        now = REG(TIMER_CLO);
        if (now - t0 >= us)
            return 0;
        if (now != last)
        {
            last = now;
            same = 0;
        }
        else if (++same == HW_STALL)
            return 1;
    }
}

/* the regions' sum into the block at off (PRE or POST); 0 if a region
 * is malformed */
static int hw_sums(uint32_t off)
{
    uint32_t a = 0, b = 0, k, base, bytes, stride;

    for (k = 0; k < P(HW_P_NREG); k++)
    {
        base = P(HW_P_REG + 12 * k);
        bytes = P(HW_P_REG + 12 * k + 4);
        stride = P(HW_P_REG + 12 * k + 8);
        if ((base | bytes) & 3 || !bytes || !stride || base >= 0x140000 ||
            bytes > 0x140000 - base)
            return 0;
        hw_sum(&a, &b, (const volatile uint32_t *)base, bytes / 4, stride);
    }
    P(off) = a;
    P(off + 4) = b;
    return 1;
}

static void hw_entry(void)
{
    uint32_t s, x = HW_X_OK, t0, now, last, same, ms, us, polls, limit, on;

    P(HW_P_EXIT) = HW_X_REFUSED;
    t0 = REG(TIMER_CLO);
    if (P(HW_P_MAGIC) != HW_PARAM_MAGIC || P(HW_P_NREG) - 1 >= HW_REGIONS ||
        P(HW_P_WAIT_MS) - 1 >= HW_WAIT_MS_MAX || (REG(HW_CORE) & 0x700) ||
        !hw_sums(HW_P_PRE_A))
    {
        MB(HW_WORD) = HW_REFUSED;
        return;
    }

    /* Apple's park: the PLLs, then the event words */
    for (s = 0; s < HW_SAVED_N; s++)
    {
        P(HW_P_SAVED + 4 * s) = REG(hw_addr(s));
        REG(hw_addr(s)) = s < 8 ? HW_PW : 0;
    }

    /* parked: the host's pins, then its wake word (or our bound) */
    now = last = REG(TIMER_CLO);
    P(HW_P_PARK_US) = now - t0;
    t0 = now;
    limit = P(HW_P_WAIT_MS);
    ms = us = same = polls = 0;
    MB(HW_WORD) = HP_HIB_PARKED;
    while (MB(HW_WORD) != HP_HIB_WAKE)
    {
        polls++;
        now = REG(TIMER_CLO);
        if (now != last)
        {
            us += now - last;           /* ms, so a long hold can't wrap */
            last = now;
            same = 0;
            if (us >= 1000)
            {
                ms += us / 1000;
                us %= 1000;
                if (ms >= limit)
                {
                    x = HW_X_GAVEUP;
                    break;
                }
            }
        }
        else if (++same == HW_STALL)
        {
            x = HW_X_STALL;
            break;
        }
    }
    now = REG(TIMER_CLO);
    P(HW_P_HELD_US) = now - t0;
    P(HW_P_POLLS) = polls;

    /* everything back: PLLs (A, D, C, B), 1 ms, the enables, the event
     * words in reverse */
    on = 0;
    for (s = 8; s != 0; s -= 2)
    {
        REG(hw_addr(s - 2)) = HW_PW | (P(HW_P_SAVED + 4 * (s - 2)) & 0xFFFF);
        REG(hw_addr(s - 1)) = HW_PW | (P(HW_P_SAVED + 4 * (s - 1)) & 0xFFFF & ~HW_PLL_ON);
        on |= P(HW_P_SAVED + 4 * (s - 1)) & HW_PLL_ON;
    }
    if (on)
    {
        hw_wait(HW_PLL_US);
        for (s = 8; s != 0; s -= 2)
            if (P(HW_P_SAVED + 4 * (s - 1)) & HW_PLL_ON)
                REG(hw_addr(s - 1)) = HW_PW | (P(HW_P_SAVED + 4 * (s - 1)) & 0xFFFF);
    }
    for (s = HW_SAVED_N; s-- != 8; )
        REG(hw_addr(s)) = P(HW_P_SAVED + 4 * s);
    REG(TIMER_C(1)) = REG(TIMER_CLO) + HW_TICK_US;
    REG(TIMER_CS) = 1 << 1;

    /* what the dark kept: the same regions, the same sum */
    t0 = REG(TIMER_CLO);
    hw_sums(HW_P_POST_A);
    P(HW_P_SUM_US) = REG(TIMER_CLO) - t0;
    if (P(HW_P_POST_A) != P(HW_P_PRE_A) || P(HW_P_POST_B) != P(HW_P_PRE_B))
        x = HW_X_SPOILED;
    P(HW_P_EXIT) = x;
    P(HW_P_RESTORE_US) = REG(TIMER_CLO) - now;
    if (x == HW_X_SPOILED)
    {
        MB(HW_WORD) = HW_SPOILED;
        for (;;)
            ;                           /* the PP cuts */
    }
    MB(HW_WORD) = x == HW_X_OK ? HP_HIB_AWAKE : HP_HIB_GAVEUP;
}
