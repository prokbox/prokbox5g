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
 * bcmfw core clock (BCMFW_CLOCK builds: the 4b bench, video, and the
 * clock-command image of stage 4c).
 *
 * Up: Apple's start-up order and values exactly (hardware.md, "Clocks,
 * PLL and regulator"; bench_test compares the writes with vmcs.bin
 * 1.3's own).  Session 7's stage 4b ran it on the device: every kernel
 * 5.56 times faster, the 1 MHz counter unchanged.
 *
 * Down: ours, the reverse [L]: the core back onto the crystal (the same
 * +0x28 = 1 write Apple's start-up makes before it turns the PLLs off),
 * PLL A off, the regulator back to the level found before the switch.
 * Stage 4c is its first device run.  Callers that use SDRAM lower its
 * refresh interval first (video.c).
 *
 * The video images (BCMFW_VCLK, P19 PW4) replace both with clock_band():
 * the crystal or one of Apple's level bands, at Apple's level, in Apple's
 * order, the core's speed checked after; any failure lands on the crystal
 * at the level found.  The 4b bench and the 4c image keep the two above,
 * byte for byte (their pass flags name their bytes).
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
#include "bcmfw.h"
#include "hw.h"
#include "clock.h"
#if BCMFW_VCLK
#include "video/video.h"
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

#if BCMFW_CLOCK

#define CLK(o)      (0x10001C00 + (o))
#define REGULATOR   0x10001400
#define RUN_CTRL    0x10000400
#define PW(v)       (0xA5A50000 | (v))

static int fast;
static uint32_t reg_level = 0xFFFFFFFF;     /* level before the switch */

/* A wait on the 1 MHz counter that also ends after a spin count, in case
 * the switch stops the counter's clock (it is one of ten block clocks
 * Apple's start-up turns off; session 7 found it running throughout).
 * Returns 0 if the spin limit ended it. */
static NOINLINE int bounded_us(uint32_t us)
{
    uint32_t t0 = REG(TIMER_CLO), n = us * BCMFW_SPIN_PER_US * 4;

    while (REG(TIMER_CLO) - t0 <= us)
        if (!n--)
            return 0;
    return 1;
}

#if !BCMFW_VCLK
/* us: the switch's time in the low (up) or high (down) half */
static void publish(int stalled, uint32_t us, uint32_t keep)
{
    uint32_t r = REG(REGULATOR);

    MB(BCMFW_MB_CLOCK_STATE) = BCMFW_CLOCK_MAGIC | (fast ? BCMFW_CLOCK_FAST : 0)
                             | (stalled ? BCMFW_CLOCK_STALL : 0);
    MB(BCMFW_MB_CLOCK_RB0) = (reg_level & 0xFF) << 24 | (r & 0xFF) << 16
                           | (REG(CLK(0x28)) & 0xFFFF);
    MB(BCMFW_MB_CLOCK_RB1) = (REG(CLK(0x00)) & 0xFFFF) << 16
                           | (REG(CLK(0x14)) & 0xFFFF);
    MB(BCMFW_MB_CLOCK_US) = (MB(BCMFW_MB_CLOCK_US) & keep) | us;
}

/* 27 MHz x 600 / 108 = 150 MHz */
int clock_fast(void (*step)(uint32_t), struct clock_readback *rb)
{
    static const uint8_t off[9] = { 0x2C, 0x30, 0x34, 0x38, 0x40, 0x44, 0x3C,
                                    0x48, 0x50 };
    uint32_t t0 = REG(TIMER_CLO);
    int k, stalled = 0;

    if (fast)
        return 1;
    if (step)
        step(1);
    REG(RUN_CTRL) = PW(0x20);
    REG(RUN_CTRL) = PW(0x20);
    REG(CLK(0x4C)) = PW(0x501);
    REG(CLK(0x54)) = PW(0);
    stalled |= !bounded_us(1);          /* Apple: about 20 nops */
    REG(CLK(0x4C)) = PW(0x51B);
    REG(CLK(0x54)) = PW(0);
    if (step)
        step(2);                        /* block clocks off */
    for (k = 0; k < 9; k++)
        REG(CLK(off[k])) = PW(0);
    REG(CLK(0x28)) = PW(1);
    REG(CLK(0x58)) = PW(0x101);
    if (step)
        step(3);                        /* PLLs B, C, D, then A, off */
    for (k = 1; k <= 4; k++)
    {
        int p = k & 3;

        REG(CLK(4 * p)) = PW(0);
        REG(CLK(0x14 + 4 * p)) = PW(0);
    }
    if (step)
        step(4);                        /* regulator to level 15, 10 ms */
    rb->reg_before = REG(REGULATOR);
    reg_level = rb->reg_before & 0xF;
    REG(REGULATOR) = PW((rb->reg_before & 0xF0) | 15);
    stalled |= !bounded_us(10000);
    if (step)
        step(5);                        /* PLL A: M, N, 1 ms, enable */
    REG(CLK(0x00)) = PW(0x6C);
    REG(CLK(0x14)) = PW(0x258);
    stalled |= !bounded_us(1000);
    REG(CLK(0x14)) = PW(0x258 | 0x400);
    if (step)
        step(6);                        /* the core onto the PLL */
    REG(CLK(0x28)) = PW(0x101);
    if (step)
        step(7);
    rb->reg_after = REG(REGULATOR);
    rb->plla_m = REG(CLK(0x00));
    rb->plla_ctl = REG(CLK(0x14));
    rb->clk_sel = REG(CLK(0x28));
    fast = 1;
    publish(stalled, (REG(TIMER_CLO) - t0) & 0xFFFF, 0xFFFF0000);
    return !stalled;
}

int clock_slow(void)
{
    uint32_t t0 = REG(TIMER_CLO), r;
    int stalled = 0;

    if (!fast)
        return 1;
    REG(CLK(0x28)) = PW(1);             /* the core onto the crystal */
    REG(CLK(0x14)) = PW(0);             /* PLL A off */
    REG(CLK(0x00)) = PW(0);
    fast = 0;
    if (reg_level <= 15)
    {
        r = REG(REGULATOR);
        REG(REGULATOR) = PW((r & 0xF0) | reg_level);
        stalled |= !bounded_us(10000);
    }
    publish(stalled, (REG(TIMER_CLO) - t0) << 16, 0xFFFF);
    return !stalled;
}

#endif /* !BCMFW_VCLK */

int clock_is_fast(void)
{
    return fast;
}

#if BCMFW_VCLK
/* Apple's start-up up to the PLLs, as clock_fast's steps 1-3 (4b): run
 * control, the counter's clock, the block clocks off, the core on the
 * crystal, PLLs B, C, D, then A, off.  1 if a wait ended on its limit */
static NOINLINE int preamble(void)
{
    static const uint8_t off[9] = { 0x2C, 0x30, 0x34, 0x38, 0x40, 0x44, 0x3C,
                                    0x48, 0x50 };
    int k, stalled;

    REG(RUN_CTRL) = PW(0x20);
    REG(RUN_CTRL) = PW(0x20);
    REG(CLK(0x4C)) = PW(0x501);
    REG(CLK(0x54)) = PW(0);
    stalled = !bounded_us(1);           /* Apple: about 20 nops */
    REG(CLK(0x4C)) = PW(0x51B);
    REG(CLK(0x54)) = PW(0);
    for (k = 0; k < 9; k++)
        REG(CLK(off[k])) = PW(0);
    REG(CLK(0x28)) = PW(1);
    REG(CLK(0x58)) = PW(0x101);
    for (k = 1; k <= 4; k++)
    {
        int p = k & 3;

        REG(CLK(4 * p)) = PW(0);
        REG(CLK(0x14 + 4 * p)) = PW(0);
    }
    return stalled;
}
#endif

#if BCMFW_VCLK
/* ---- P19 PW4: the level bands -------------------------------------------
 * The bands: the crystal, then the top whole MHz of each of Apple's level
 * bands (@141ba's rows), at that level; PLL A by Apple's rule (@4cb8: M
 * 108, N = VCO / 250 kHz, the VCO at 75 MHz or more: divided by 2 below
 * 75).  The crystal keeps the level found there (the host's: 11 after a
 * start, 5 with the PW3 lever).  apps/video/vclk.c has the same table and
 * tests it. */
/* vclk_test.py's mutation check only: 1 raises the level after the clock
 * instead of before, 2 skips the speed check.  Never set for the device */
#ifndef VCLK_MUTANT
#define VCLK_MUTANT 0
#endif

static const uint8_t band_mhz[CLOCK_BANDS] =
    { 27, 46, 59, 74, 87, 102, 116, 123, 130, 140, 147, 150 };
static const uint8_t band_lvl[CLOCK_BANDS] =
    {  0,  5,  6,  7,  8,   9,  10,  11,  12,  13,  14,  15 };

struct clock_gov clock_gov;      /* mhz 0 before the first change: 27 */

/* us the calibration loop takes: 20,000 turns of spin() (741 us on the
 * crystal, one a cycle: hardware.md, the clock line) */
static NOINLINE uint32_t loop_us(void)
{
    uint32_t t0 = REG(TIMER_CLO);

    spin(20000);
    return REG(TIMER_CLO) - t0;
}

/* the regulator to level l (bits 7:4 kept), 10 ms; 1 if the wait stalled */
static NOINLINE int level_to(uint32_t l)
{
    uint32_t r = REG(REGULATOR);

    if ((r & 0xF) == l)
        return 0;
    REG(REGULATOR) = PW((r & 0xF0) | l);
    clock_gov.regs++;
    return !bounded_us(10000);
}

/* The core onto the crystal, PLL A stopped */
static NOINLINE void xtal(void)
{
    REG(CLK(0x28)) = PW(1);
    REG(CLK(0x14)) = PW(0);
    fast = 0;
    clock_gov.band = 0;
    clock_gov.mhz = 27;
}

/* ... and off, at the level found on the crystal: the switch down, and
 * every failure's way back.  r, or CLOCK_E_STALL if the wait stalled */
static NOINLINE int xtal_off(int r)
{
    xtal();
    REG(CLK(0x00)) = PW(0);
    if (reg_level <= 15 && level_to(reg_level))
        r = CLOCK_E_STALL;
    return r;
}

/* A clock f in band b (0 or out of the band: its top; the crystal is 27).
 * @5134's order: a higher level before the clock goes up, a lower one
 * after it came down, 10 ms each (inside a band the level stays: no
 * wait); PLL A locks with the core on the crystal.  Then the core's speed
 * is checked against the crystal's (the loop must take 27/f of its
 * crystal time, within 1/8).  Any failure: the crystal, PLL A off, the
 * level found before. */
int clock_band(uint32_t b, uint32_t f)
{
    static uint32_t us27;               /* the loop on the crystal */
    struct clock_gov *g = &clock_gov;
    uint32_t l, d;

    if (b >= CLOCK_BANDS)
        return CLOCK_E_POINT;
    if (f > band_mhz[b] || !b || f <= band_mhz[b - 1])
        f = band_mhz[b];
    if (b == g->band && (!b || f == g->mhz))
        return 0;
    l = band_lvl[b];
    if (!fast)
    {
        reg_level = REG(REGULATOR) & 0xF;
        if (!us27)
            us27 = loop_us();
        if (b && preamble())
            goto stall;
    }
    if (l > (REG(REGULATOR) & 0xF) && VCLK_MUTANT != 1 && level_to(l))
        goto stall;
    xtal();
    if (!b)
        return xtal_off(0);
    d = f >= 75 ? 1 : f >= 38 ? 2 : 3;  /* the divider: the VCO >= 75 */
    REG(CLK(0x00)) = PW(0x6C);
    REG(CLK(0x14)) = PW(f * d * 4);
    if (!bounded_us(1000))
        goto stall;
    REG(CLK(0x14)) = PW(f * d * 4 | 0x400);
    REG(CLK(0x28)) = PW(0x100 | d);
    fast = 1;
    g->band = b;
    g->mhz = f;
    g->plls++;
    if ((l < (REG(REGULATOR) & 0xF) || (VCLK_MUTANT == 1 &&
                                         l > (REG(REGULATOR) & 0xF))) &&
        level_to(l))
        goto stall;
    d = loop_us() * f * 8;
    if ((d >= us27 * 27 * 7 && d <= us27 * 27 * 9) || VCLK_MUTANT == 2)
        return 0;
    g->fails++;
    return xtal_off(CLOCK_E_SPEED);
stall:
    g->fails++;
    return xtal_off(CLOCK_E_STALL);
}

uint32_t clock_mhz(void)
{
    return clock_gov.mhz ? clock_gov.mhz : 27;
}

/* A change the host asked for (HP_OP_VCLK; the governor is the PP's:
 * apps/video/vclk.c), or OPEN's and CLOSE's */
int clock_gov_set(uint32_t b, uint32_t f)
{
    int r;

    lcd_blit_wait();                    /* no transfer across a switch */
    video_clock_refresh(0);
    r = clock_band(b, f);
    video_clock_refresh(1);
    lcd_clock_changed();
    return r;
}
#endif

#endif /* BCMFW_CLOCK */
