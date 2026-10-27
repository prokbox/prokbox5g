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

/* The SDRAM bring-up (docs/data-paths.md R4, bcm2722-reference/sdram.md):
 * static functions, included once by each image that brings SDRAM up, at
 * the point where they belong in its file.  video.c includes it where the
 * code always stood (so the video images' bytes are unchanged), and the
 * staged display-only image (BCMFW_JOBS_STAGED without BCMFW_VIDEO:
 * jobs_bcm.c) for its upload window.  The includer declares memset() and
 * sd_diag[VIDEO_SD_WORDS] (video.h) first. */

#ifndef BCMFW_SDRAM_UP_H
#define BCMFW_SDRAM_UP_H

#define SDRAM_CTL(o)     (0x10003400 + (o))
#define CACHE_CTL        0x10000C00
#define SDRAM_PROBE_A    0xFE000000
#define SDRAM_PROBE_B    0xFE000200
#define SDRAM_WAIT_LOOPS 72000
#define SDRAM_WAIT_US    5000      /* command 7: Apple polls 12,000 times */
#define SDRAM_POWERUP_US 200       /* clock and timing words stable first */
#define SD_ATTEMPTS      4

/* Bounded waits: the 1 MHz counter when it runs, a poll count when not
 * (12,000 polls was Apple's bound; six times that at 150 MHz) */
static int timer_ok(void)
{
    return !!(MB(BCMFW_MB_INFO) & BCMFW_INFO_TIMER_OK);
}

static void wait_us(uint32_t us)
{
    uint32_t t0 = REG(TIMER_CLO), k;

    if (timer_ok())
    {
        while (REG(TIMER_CLO) - t0 < us)
            ;
        return;
    }
    for (k = 0; k < us * 40; k++)       /* at most 40 polls a us */
        (void)REG(SDRAM_CTL(0x20));
}

/* Command 7 with a mode word, then poll +0x20 bit 15.  Apple's @12d90
 * ignores a timeout and lets the probe decide; so do we, but sd_diag[1]
 * keeps the last value read.  Returns 1 when bit 15 was seen. */
static int sdram_cmd(uint32_t mode_word)
{
    uint32_t k, t0, v = 0;
    int timer = timer_ok();

    REG(SDRAM_CTL(0x28)) = mode_word;
    REG(SDRAM_CTL(0x20)) = 7;
    t0 = REG(TIMER_CLO);
    for (k = 0; timer ? REG(TIMER_CLO) - t0 < SDRAM_WAIT_US
                      : k < SDRAM_WAIT_LOOPS; k++)
    {
        v = REG(SDRAM_CTL(0x20));
        if (v & 0x8000)
            break;
    }
    sd_diag[1] = v;
    return !!(v & 0x8000);
}

/* Apple's @296: write back and invalidate (bit 30, wait for bit 0), then
 * the cache-enable bit 31 as asked */
static void cache_flush(uint32_t on)
{
    uint32_t k, old = REG(CACHE_CTL);

    REG(CACHE_CTL) = old | 0x40000000;
    for (k = 0; k < SDRAM_WAIT_LOOPS && !(REG(CACHE_CTL) & 1); k++)
        ;
    REG(CACHE_CTL) = (old & ~0xC0000000u) | on;
}

/* Apple's @14892, before a retry: the controller back to idle (+0x20 = 0,
 * bit 15 clear) */
static void sdram_reset(uint32_t on)
{
    uint32_t k;

    cache_flush(on);
    REG(CACHE_CTL) &= ~0x40010000u;
    REG(SDRAM_CTL(0x20)) = 0;
    for (k = 0; k < SDRAM_WAIT_LOOPS && (REG(SDRAM_CTL(0x20)) & 0x8000); k++)
        ;
}

/* The refresh interval in core cycles for the clock now running: 7.8 us
 * either way (a faster clock with the slow value only refreshes more
 * often, so it goes up after a switch up and down before a switch down) */
static void sdram_refresh(void)
{
    uint32_t r = BCMFW_SDRAM_REFRESH;

#if BCMFW_VCLK
    r = clock_mhz() * 125 >> 4;     /* f / 128,000: 210 at 27, 0x493 at 150 */
#elif BCMFW_CLOCK
    if (clock_is_fast())
        r = BCMFW_SDRAM_REFRESH_FAST;
#endif
    REG(SDRAM_CTL(0x24)) = (REG(SDRAM_CTL(0x24)) & 0xFFFF) | r << 16;
}

static void sd_note(uint32_t step, uint32_t att, uint32_t seen)
{
    uint32_t f = timer_ok() ? VIDEO_SD_TIMER : 0;

#if BCMFW_CLOCK
    if (clock_is_fast())
        f |= VIDEO_SD_FAST;
#endif
    sd_diag[0] = VIDEO_SD_MAGIC | step << 12 | att << 8 | seen << 4 | f;
}

/* Returns 0, or the VIDEO_SD_* step that failed (sd_diag says more).
 * Apple's order (vmcs @14406, @1410a, @147ca): refresh, timing words,
 * cache flushed and off, +0x30 = 0x80E8, command 7 with the mode word
 * 0x30570000 | a << 12 | b << 8 | c << 2 | 0x30 (a = 12, c = 1; b = 8,
 * and @12d90 turns Apple's fallback 7 into 4), the geometry probe, a
 * reset and the other b if it fails, then +0x30 = 0x64 and bit 16.
 * Ours: a power-up wait, a bounded number of attempts instead of Apple's
 * endless loop, and the cache left off (attempts 3 and 4 try Apple's
 * cache-on state only if ours fails, and the cache goes off again after
 * the probe; sd_diag says which passed). */
static uint32_t sdram_init(void)
{
    static const uint8_t field[SD_ATTEMPTS] = { 8, 4, 8, 4 };
    uint32_t i, seen = 0, on = 0, a, c;

    memset(sd_diag, 0, sizeof(sd_diag));
    sdram_refresh();
    REG(SDRAM_CTL(0x10)) = 0x0105008C;
    REG(SDRAM_CTL(0x14)) = 0x0105000C;
    cache_flush(0);
    REG(CACHE_CTL) = 0;
    cache_flush(0);
    REG(SDRAM_CTL(0x30)) = 0x80E8;
    wait_us(SDRAM_POWERUP_US);
    for (i = 0; i < SD_ATTEMPTS; i++)
    {
        on = i >= 2 ? 0x80000000u : 0;
        if (i)
            sdram_reset(on);
        if (sdram_cmd(0x30570000 | 12 << 12 | field[i] << 8 | 1 << 2 | 0x30))
            seen |= 1 << i;
        MB(SDRAM_PROBE_A) = 0x12345678;
        MB(SDRAM_PROBE_B) = 0x87654321;
        if (on)
            cache_flush(on);    /* read SDRAM, not the lines just written */
        a = MB(SDRAM_PROBE_A);
        c = MB(SDRAM_PROBE_B);
        sd_diag[2] = a;
        sd_diag[3] = c;
        if (a == 0x12345678 && c == 0x87654321)
            break;
    }
    if (i == SD_ATTEMPTS)
    {
        sd_note(seen ? VIDEO_SD_PROBE : VIDEO_SD_CMD, SD_ATTEMPTS, seen);
        return seen ? VIDEO_SD_PROBE : VIDEO_SD_CMD;
    }
    REG(SDRAM_CTL(0x30)) = 0x64;
    /* the cache off for the session even when a cache-on attempt passed
       (docs/data-paths.md: whether the vector unit and the host port see
       through it is unknown, BCM #6): written back, then off */
    if (on)
        cache_flush(0);
    REG(CACHE_CTL) = (REG(CACHE_CTL) & ~0xC0000000u) | 0x10000;
    sd_note(VIDEO_SD_OK, i + 1, seen);
    return VIDEO_SD_OK;
}

#endif /* BCMFW_SDRAM_UP_H */
