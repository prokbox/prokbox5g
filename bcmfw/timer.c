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
 * bcmfw delays on the 1 MHz system counter, with a spin-loop fallback
 * if the counter is found stopped (the core clock and timer state at a
 * host start are not proven yet).
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
#include "hw.h"
#include "bcmfw.h"

static int counter_runs;

void spin(uint32_t n)
{
    while (n--)
        asm volatile("");
}

void bcmfw_step(uint32_t v)
{
    MB(BCMFW_MB_STEP) = v;
}

/* Counter ticks over spin(1000): 3000 instructions, 20-300 us for a core
 * between 10 and 150 MHz */
static uint32_t ticks_over_spin(void)
{
    uint32_t t0 = REG(TIMER_CLO);

    spin(1000);
    return REG(TIMER_CLO) - t0;
}

#if BCMFW_CLK_ENABLE
/* The counter's clock, Apple's writes in Apple's order (hardware.md,
 * "Clocks"; vmcs.bin 1.3 @14562 and @53b4), a step at a time: each is
 * made only if the counter still does not count */
static uint32_t clock_enable(uint32_t rec)
{
    int i;

    bcmfw_step(BCMFW_STEP_CLK_A);
    REG(CLK_4C) = CLK_PW(0x501);
    REG(CLK_54) = CLK_PW(0);
    for (i = 0; i < 20; i++)        /* Apple: about 20 nops */
        asm volatile("nop");
    REG(CLK_4C) = CLK_PW(0x51B);
    REG(CLK_54) = CLK_PW(0);
    rec |= BCMFW_CLK_WROTE_A;
    if (ticks_over_spin())
        return rec | BCMFW_CLK_RAN_A;
    bcmfw_step(BCMFW_STEP_CLK_B);
    REG(CLK_58) = CLK_PW(0x101);
    rec |= BCMFW_CLK_WROTE_B;
    if (ticks_over_spin())
        rec |= BCMFW_CLK_RAN_B;
    return rec;
}
#endif

/* spin()'s loop at sixteen alignments (spinprobe.S): the ticks in the
 * mailbox, a byte each */
#define SPA(k) uint32_t bcmfw_spa_##k(void)
SPA(0); SPA(1); SPA(2); SPA(3); SPA(4); SPA(5); SPA(6); SPA(7);
SPA(8); SPA(9); SPA(10); SPA(11); SPA(12); SPA(13); SPA(14); SPA(15);

static void spin_alignments(void)
{
    static uint32_t (*const spa[16])(void) =
    {
        bcmfw_spa_0, bcmfw_spa_1, bcmfw_spa_2, bcmfw_spa_3,
        bcmfw_spa_4, bcmfw_spa_5, bcmfw_spa_6, bcmfw_spa_7,
        bcmfw_spa_8, bcmfw_spa_9, bcmfw_spa_10, bcmfw_spa_11,
        bcmfw_spa_12, bcmfw_spa_13, bcmfw_spa_14, bcmfw_spa_15,
    };
    uint32_t k, t, w = 0;

    for (k = 0; k < 16; k++)
    {
        t = spa[k]();
        w |= (t > 255 ? 255 : t) << 8 * (k & 3);
        if ((k & 3) == 3)
        {
            MB(BCMFW_MB_SPA + (k & ~3u)) = w;
            w = 0;
        }
    }
    MB(BCMFW_MB_SPA_SPIN) = BCMFW_SPA_MAGIC | ((uint32_t)spin & 0xFFFF);
}

/* Returns 1 if delays use the counter.  Mailbox: BCMFW_MB_CLK and the
 * clock registers as found. */
int timer_init(void)
{
    uint32_t rec = BCMFW_CLK_MAGIC, ticks;

    MB(BCMFW_MB_CLK) = 0;
    MB(BCMFW_MB_CLK_4C) = REG(CLK_4C);
    MB(BCMFW_MB_CLK_54) = REG(CLK_54);
    MB(BCMFW_MB_CLK_58) = REG(CLK_58);
    bcmfw_step(BCMFW_STEP_TIMER);
    if (ticks_over_spin())
        rec |= BCMFW_CLK_RAN0;
#if BCMFW_CLK_ENABLE
    else
        rec = clock_enable(rec);
#endif
    bcmfw_step(BCMFW_STEP_TIMER);
    ticks = ticks_over_spin();
    counter_runs = ticks != 0;
    /* A counter far from 1 MHz would make every delay wrong: then spin */
    if (counter_runs && (ticks < 10 || ticks > 400))
    {
        rec |= BCMFW_CLK_RATE_BAD;
        counter_runs = 0;
    }
    MB(BCMFW_MB_CLK) = rec | (ticks > 0xFFFF ? 0xFFFF : ticks) << 16;
    MB(BCMFW_MB_SPA_SPIN) = 0;
    if (counter_runs)
        spin_alignments();
    return counter_runs;
}

int timer_clock_written(void)
{
    return (MB(BCMFW_MB_CLK) & (BCMFW_CLK_WROTE_A | BCMFW_CLK_WROTE_B)) != 0;
}

static void spin_us(uint32_t us)
{
    spin(us * BCMFW_SPIN_PER_US);
}

void udelay(uint32_t us)
{
    if (counter_runs)
    {
        uint32_t t0 = REG(TIMER_CLO), t;
        /* bounded: a poll is at least 3 instructions, so this many polls
         * last at least us at up to 150 MHz.  A counter that stops
         * mid-wait ends it here; the wait is then done again as a spin,
         * and every later delay spins. */
        uint32_t n = us * BCMFW_SPIN_PER_US + 16;

        /* "<=": the first microsecond may be partial */
        while ((t = REG(TIMER_CLO)) - t0 <= us)
        {
            if (!n--)
            {
                MB(BCMFW_MB_LIMIT) = BCMFW_LIMIT_DELAY << 24 | (us & 0xFFFFFF);
                MB(BCMFW_MB_LIMIT_VAL) = t;
                counter_runs = 0;
                spin_us(us);
                return;
            }
        }
    }
    else
    {
        spin_us(us);
    }
}

void mdelay(uint32_t ms)
{
    while (ms--)
        udelay(1000);
}
