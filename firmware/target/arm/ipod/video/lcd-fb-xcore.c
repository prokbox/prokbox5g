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
 * The pixel hand-off across the PP5022's cores (BCM MF-4, P19 19-9;
 * docs/design/p19-ui-on-cop.md section 6.3; the contract in xcore.h).
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

/* The host test (tests/lcdxcore) includes this file with its own LFX_*
 * definitions: two model cores with incoherent write-back caches and a
 * model of the shell's fence.  On the device they are the core id, the
 * uncached alias, xcore.h's whole-cache helpers and fe_fb_publish(). */
#ifndef LFX_HOST
#include "config.h"
#include "system.h"
#include "lcd.h"
#include "xcore.h"
#include "fe_fb.h"

#ifdef HAVE_FE_FB_HANDOFF
extern struct frame_buffer_t lcd_framebuffer_default;

typedef fb_data lfx_px;
#define LFX_W           LCD_FBWIDTH
#define LFX_H           LCD_FBHEIGHT
#define LFX_FB          ((char *)lcd_framebuffer_default.fb_ptr)
#define LFX_CORE()      CURRENT_CORE
#define LFX_CPU         CPU
#define LFX_UNCACHED(p) UNCACHED_ADDR(p)
#define LFX_PUBLISH()   xcore_publish()     /* this core: write back */
#define LFX_ACQUIRE()   xcore_acquire()     /* ... and drop */
#define LFX_FENCE(r)    fe_fb_publish((r), FE_FB_TIMEOUT_MS)
#define LFX_STATE       XCORE_BSS

/* the rows are whole cache lines, so full-width rects (all engine 2 ever
 * pushes) are line aligned at both ends: the e2-port contract */
typedef char lfx_rows_on_lines[(LCD_FBWIDTH * sizeof(fb_data))
                               % XCORE_LINE == 0 ? 1 : -1];
#endif
#endif /* !LFX_HOST */

#if defined(HAVE_FE_FB_HANDOFF) || defined(LFX_HOST)

/* The producer's core.  Uncached (XCORE_BSS): written by the claim (the
 * producer, once, before it draws) and the take (a consumer that becomes
 * the producer); read by both cores.  Zero = the CPU, the producer until
 * a claim says otherwise. */
static unsigned int lfx_core LFX_STATE;
volatile unsigned lcd_fb_fence_fails LFX_STATE;

void lcd_fb_claim(void)
{
    unsigned int core = LFX_CORE();

    /* a new producer on the COP drops whatever stale lines of the frame
     * its cache holds; the old one (the CPU) wrote its own back when it
     * created this thread (create_thread() on another core does) */
    if (core != LFX_CPU)
        LFX_ACQUIRE();
    lfx_core = core;
}

bool lcd_fb_cross(void)
{
    return LFX_CORE() != lfx_core;
}

/* The rect's lines: rows x pitch, from the rect's first pixel to past its
 * last, rounded out to whole cache lines and clipped to the frame */
void lcd_fb_lines(int x, int y, int w, int h,
                  uintptr_t *start, uint32_t *bytes)
{
    const uintptr_t fb = (uintptr_t)LFX_FB;
    const uintptr_t end_fb = fb + (uintptr_t)LFX_W * LFX_H * sizeof(lfx_px);
    uintptr_t a, b;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LFX_W)
        w = LFX_W - x;
    if (y + h > LFX_H)
        h = LFX_H - y;
    if (w <= 0 || h <= 0)
    {
        *start = fb;
        *bytes = 0;
        return;
    }
    a = fb + ((uintptr_t)y * LFX_W + x) * sizeof(lfx_px);
    b = fb + ((uintptr_t)(y + h - 1) * LFX_W + x + w) * sizeof(lfx_px);
    a &= ~(uintptr_t)(XCORE_LINE - 1);
    b = (b + XCORE_LINE - 1) & ~(uintptr_t)(XCORE_LINE - 1);
    if (b > end_fb)
        b = end_fb;
    *start = a;
    *bytes = (uint32_t)(b - a);
}

/* The producer's write-back of the rect, asked from the consumer's thread
 * (fe_fb_publish(): bounded, it may block; never with interrupts off).
 * The rect is recorded only: the PP5022 writes back its whole cache. */
int lcd_fb_fence(int x, int y, int w, int h)
{
    struct fe_fb_rect r = { x, y, w, h };
    uintptr_t a;
    uint32_t n;
    int res;

    if (!lcd_fb_cross())
        return FE_FB_LOCAL;
    lcd_fb_lines(x, y, w, h, &a, &n);
    if (!n)
        return FE_FB_LOCAL;
    res = LFX_FENCE(&r);
    if (res == FE_FB_TIMEOUT)
        lcd_fb_fence_fails++;   /* the consumer's only; it pushes anyway */
    return res;
}

const void *lcd_fb_read(int x, int y)
{
    char *p = LFX_FB + ((uintptr_t)y * LFX_W + x) * sizeof(lfx_px);

    if (!lcd_fb_cross())
        return p;
    /* the consumer's own drawing (a panic screen) to SDRAM too, then
     * every read through the alias: no stale line of its own, and after
     * a fence that timed out, at worst a pixel the producer has not
     * written back yet: an old pixel, never a corrupted one */
    LFX_PUBLISH();
    return LFX_UNCACHED(p);
}

void lcd_fb_publish(int x, int y, int w, int h)
{
    (void)x; (void)y; (void)w; (void)h;     /* whole cache on the PP5022 */
    if (lfx_core != LFX_CPU && LFX_CORE() == lfx_core)
        LFX_PUBLISH();
}

void lcd_fb_take(void)
{
    if (!lcd_fb_cross())
        return;
    lcd_fb_fence(0, 0, LFX_W, LFX_H);
    LFX_ACQUIRE();      /* the frame and the drawing state, fresh */
    lfx_core = LFX_CORE();
}

#endif /* HAVE_FE_FB_HANDOFF || LFX_HOST */
