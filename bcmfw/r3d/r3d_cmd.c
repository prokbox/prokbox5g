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

#include <stdint.h>
#include "config.h"
#include "bcmfw.h"
#include "hw.h"
#include "r3d.h"

#if BCMFW_R3D

/* CMD_R3D: one scene of the 3D render test.  The check frame at
 * CHECK_MS is rendered off screen and its checksum kept; then the scene
 * runs live for MS milliseconds, its camera following the clock: each
 * frame renders into one framebuffer while the other goes to the panel
 * by DMA.  The first live frame is the one at 0 ms, checksummed too.
 * With PERIOD set (33,333 us: 30 fps, the panel's useful rate), each
 * frame goes to the panel at its slot, PERIOD after the one before, and
 * not earlier; a frame rendered after its slot is late, goes at once,
 * and the slots count on from it. */

#define PARAM(k)  MB(BCMFW_R3D_PARAM + 4 * (k))

static int ready;

static uint32_t clock_us(void)
{
    return REG(TIMER_CLO);
}

static void init_once(void)
{
    struct r3d_mem m;

    if (ready)
        return;
    m.tex = (uint8_t *)BCMFW_R3D_TEX;
    m.clut = (uint16_t *)BCMFW_R3D_CLUT;
    m.verts = (void *)BCMFW_R3D_VERTS;
    m.tris = (void *)BCMFW_R3D_TRIS;
    m.xverts = (void *)BCMFW_R3D_XVERTS;
    m.ot = (uint16_t *)BCMFW_R3D_OT;
    m.prims = (void *)BCMFW_R3D_PRIMS;
    r3d_init(&m);
    r3d_now = clock_us;
    ready = 1;
}

/* ---- the panel's tearing-effect line (GPIO 49) ----
 * The panel refreshes from its own memory on its own clock and pulses
 * TE at the start of each refresh; Apple times its writes from it
 * (@CB1C keeps a running average of the period, @4B4A races the scan).
 * Read here by polling the level: no interrupt, nothing written. */

#define TE_BIT  (1u << (49 - 32))

static uint32_t te_level(void)
{
    return REG(GPIO_LEV1) & TE_BIT;
}

static uint32_t te_period;          /* from the last probe, us; 0 none */

/* Poll the line for ms: its edges and their spacing, into the block */
static void te_probe(uint32_t ms)
{
    uint32_t t0 = clock_us(), now = t0, prev = te_level(), lev, rise = 0;
    uint32_t first = 0, last = 0, n = 0, mn = ~0u, mx = 0, changes = 0, high = 0;

    PARAM(BCMFW_R3D_P_TE_LEVEL) = prev ? 1 : 0;
    while (now - t0 < ms * 1000)
    {
        lev = te_level();
        now = clock_us();
        if (lev == prev)
            continue;
        changes++;
        prev = lev;
        if (lev)
        {
            if (n)
            {
                uint32_t d = now - last;

                if (d < mn) mn = d;
                if (d > mx) mx = d;
            }
            else
                first = now;
            last = rise = now;
            n++;
        }
        else if (n)
            high = now - rise;
    }
    PARAM(BCMFW_R3D_P_TE_N) = n;
    PARAM(BCMFW_R3D_P_TE_MIN) = n > 1 ? mn : 0;
    PARAM(BCMFW_R3D_P_TE_MAX) = mx;
    PARAM(BCMFW_R3D_P_TE_SUM) = last - first;
    PARAM(BCMFW_R3D_P_TE_HIGH) = high;
    PARAM(BCMFW_R3D_P_TE_LEVEL) |= changes << 8;
    te_period = n > 2 ? (last - first) / (n - 1) : 0;
}

/* The next rising edge, or 0 after 2 periods (or 50 ms) without one */
static uint32_t te_edge(void)
{
    uint32_t t0 = clock_us(), limit = te_period ? 2 * te_period : 50000;
    uint32_t prev = te_level(), lev;

    for (;;)
    {
        uint32_t now = clock_us();

        lev = te_level();
        if (lev && !prev)
            return now ? now : 1;
        prev = lev;
        if (now - t0 > limit)
            return 0;
    }
}

void r3d_command(void)
{
    uint16_t *fb[2] = { (uint16_t *)BCMFW_FB, (uint16_t *)BCMFW_R3D_FB2 };
    uint32_t op = PARAM(BCMFW_R3D_P_OP), scene = PARAM(BCMFW_R3D_P_SCENE);
    uint32_t ms = PARAM(BCMFW_R3D_P_MS), check = PARAM(BCMFW_R3D_P_CHECK_MS);
    uint32_t period = PARAM(BCMFW_R3D_P_PERIOD), late = 0, spare = 0, slot = 0;
    uint32_t sync = PARAM(BCMFW_R3D_P_SYNC), shown = 0;
    uint32_t t0, start, frames = 0, back = 1, k;
    uint32_t sum = 0, setup = 0, raster = 0, wait = 0, mx = 0, mn = ~0u;
    uint32_t drawn = 0, culled = 0, clipped = 0, pixels = 0, chunks = 0;
    struct r3d_stats st;

    for (k = BCMFW_R3D_P_STATE; k < BCMFW_R3D_P_N; k++)
        if (k != BCMFW_R3D_P_PERIOD && k != BCMFW_R3D_P_SYNC)
            PARAM(k) = 0;
    if (op == (BCMFW_R3D_MAGIC | BCMFW_R3D_TE) && ms && ms <= 2000)
    {
        te_probe(ms);
        PARAM(BCMFW_R3D_P_STATE) = BCMFW_R3D_ST_DONE;
        return;
    }
    if (sync && !te_period)
        sync = 0;               /* no probe found the line: the timer */
    if (op != (BCMFW_R3D_MAGIC | BCMFW_R3D_RUN) || scene >= R3D_SCENES)
    {
        PARAM(BCMFW_R3D_P_STATE) = BCMFW_R3D_ST_BAD;
        return;
    }
    t0 = clock_us();
    init_once();
    PARAM(BCMFW_R3D_P_TRIS) = r3d_scene(scene);
    /* the check frame, off screen (into the frame not yet shown) */
    r3d_render(check, fb[back], &st);
    PARAM(BCMFW_R3D_P_CRC1) = r3d_crc(fb[back]);
    PARAM(BCMFW_R3D_P_INIT) = clock_us() - t0;
    if (!lcd_is_on())
        lcd_power_on();
    start = clock_us();
    for (;;)
    {
        uint32_t now = clock_us(), t_ms = frames ? (now - start) / 1000 : 0;
        uint32_t f0, f1, w0;

        if (frames && now - start >= ms * 1000)
            break;
        f0 = clock_us();
        r3d_render(t_ms, fb[back], &st);
        f1 = clock_us();
        if (!frames)
            PARAM(BCMFW_R3D_P_CRC0) = r3d_crc(fb[back]);
        if (sync)
        {
            /* on the panel's tearing edge: the write (14.3 ms a frame at
             * the 27 MHz bus) then stays ahead of a slower scan.  The
             * sync-th edge since the last frame went; a later one is late */
            uint32_t e = te_edge(), want = sync * te_period - te_period / 2;

            while (e && frames && e - shown < want)
                e = te_edge();
            if (!e)
                late++;             /* the line stopped: go anyway */
            else
            {
                if (frames && e - shown > want + te_period)
                    late++;
                shown = e;
            }
        }
        else if (period)
        {
            /* hold the frame for its slot (the first one goes at once) */
            uint32_t now2 = clock_us() - start;

            if (now2 > slot + 1000)
                late++;
            else if (now2 < slot)
            {
                spare += slot - now2;
                while (clock_us() - start < slot)
                    ;
                now2 = slot;
            }
            slot = (now2 > slot ? now2 : slot) + period;
        }
        /* the frame before must be on the panel before this one goes */
        w0 = clock_us();
        lcd_blit_wait();
        wait += clock_us() - w0;
        lcd_blit(fb[back], R3D_W, 0, 0, R3D_W, R3D_H);
        back ^= 1;
        frames++;
        sum += f1 - f0;
        if (f1 - f0 > mx)
            mx = f1 - f0;
        if (f1 - f0 < mn)
            mn = f1 - f0;
        setup += st.setup_us;
        raster += st.raster_us;
        drawn += st.drawn;
        culled += st.culled;
        clipped += st.clipped;
        pixels += st.pixels;
        chunks += st.chunks_culled;
        PARAM(BCMFW_R3D_P_FRAMES) = frames;
    }
    lcd_blit_wait();
    PARAM(BCMFW_R3D_P_US) = clock_us() - start;
    PARAM(BCMFW_R3D_P_RENDER) = sum;
    PARAM(BCMFW_R3D_P_SETUP) = setup;
    PARAM(BCMFW_R3D_P_RASTER) = raster;
    PARAM(BCMFW_R3D_P_WAIT) = wait;
    PARAM(BCMFW_R3D_P_MAX) = mx;
    PARAM(BCMFW_R3D_P_MIN) = mn;
    PARAM(BCMFW_R3D_P_DRAWN) = drawn;
    PARAM(BCMFW_R3D_P_CULLED) = culled;
    PARAM(BCMFW_R3D_P_CLIPPED) = clipped;
    PARAM(BCMFW_R3D_P_PIXELS) = pixels;
    PARAM(BCMFW_R3D_P_CHUNKS) = chunks;
    PARAM(BCMFW_R3D_P_LATE) = late;
    PARAM(BCMFW_R3D_P_SPARE) = spare;
    PARAM(BCMFW_R3D_P_STATE) = BCMFW_R3D_ST_DONE;
}

#endif /* BCMFW_R3D */
