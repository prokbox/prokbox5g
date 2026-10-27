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
 * iPod UI shell: frame clock, CPU boost, LCD push trace, offscreen strip
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
#ifdef SIMULATOR
#include <stdlib.h>
#endif
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "lcd.h"
#include "core_alloc.h"
#include "ipodui/anim.h"
#include "ipodui/litmiss.h"
#include "ipodui/modclass.h"
#include "ipodui/uilat.h"         /* np-lag probe */
#ifdef SIMULATOR
#include "ipodui/simclock.h"    /* sim_clock_cpu_us(): the frame CPU figure */
#endif

static bool boosted;            /* we hold exactly one cpu_boost(true) */
static long last_frame;         /* tick of the last frame drawn */
static long next_frame;         /* tick the next frame is due */
#ifdef SIMULATOR
static unsigned long last_cpu_us;   /* UI thread CPU time at the last frame */
static unsigned long last_pcpu_us;  /* whole-process CPU time at the last frame */
#endif
static int strip_handle;        /* 0: none (allocation failed or not yet) */
static int strip_owner;         /* last claimer, 0 = nobody */
static int owner_seq;

/* The boost count in the trace: on target the firmware's own counter
 * (every holder, not just us); the simulator has no CPU frequency
 * control, so it logs our own hold (1 while animating, else 0). */
#if defined(HAVE_ADJUSTABLE_CPU_FREQ) && !defined(SIMULATOR)
#define BOOST_COUNT()   get_cpu_boost_counter()
#else
#define BOOST_COUNT()   (boosted ? 1 : 0)
#endif

bool ui_anim_active(void)
{
    return boosted;
}

bool ui_anim_frame_due(void)
{
    return !boosted || !TIME_BEFORE(current_tick, next_frame);
}

int ui_anim_wait(void)
{
    if (!boosted || !TIME_BEFORE(current_tick, next_frame))
        return 0;
    return next_frame - current_tick;
}

void ui_anim_begin(void)
{
    if (boosted)
        return;
    cpu_boost(true);
    boosted = true;
    UPDATE_TRACE("anim start boost=%d", BOOST_COUNT());
    last_frame = next_frame = current_tick;
#ifdef SIMULATOR
    last_cpu_us = sim_clock_cpu_us();
    last_pcpu_us = sim_clock_proc_cpu_us();
#endif
}

void ui_anim_frame(void)
{
    long now = current_tick;

    if (!boosted)
    {
        ui_anim_begin();
        last_frame = now;
    }
#ifdef SIMULATOR
    /* the UI thread's CPU time since the previous frame: unlike the wall
     * interval it does not grow when the host is busy, so
     * scripts/check_trace.py can tell a costly frame from a starved one */
    unsigned long cpu_now = sim_clock_cpu_us();
    unsigned long pcpu_now = sim_clock_proc_cpu_us();
    /* pcpu: every thread of the simulator. A frame that is late while the
     * whole process was mostly idle was held up by the host; one that is
     * late with the process busy was held up by our own threads. */
    UPDATE_TRACE("anim frame %ld cpu=%lu pcpu=%lu",
                 (now - last_frame) * (1000 / HZ),
                 cpu_now - last_cpu_us, pcpu_now - last_pcpu_us);
    last_cpu_us = cpu_now;
    last_pcpu_us = pcpu_now;
#else
    UPDATE_TRACE("anim frame %ld", (now - last_frame) * (1000 / HZ));
#endif
    last_frame = now;
    /* bench lit24ui: a whole frame slot past its due time is missed */
    LITMISS_SCROLL(!TIME_BEFORE(now, next_frame + UI_ANIM_FRAME_TICKS));
    uilat_frame(!TIME_BEFORE(now, next_frame + UI_ANIM_FRAME_TICKS));
    /* keep the schedule (a late wakeup does not push the next frame
     * back), but never queue up frames we are already late for */
    next_frame += UI_ANIM_FRAME_TICKS;
    if (!TIME_AFTER(next_frame, now))
        next_frame = now + UI_ANIM_FRAME_TICKS;
}

void ui_anim_idle(void)
{
    if (!boosted)
        return;
    cpu_boost(false);
    boosted = false;
    UPDATE_TRACE("anim end boost=%d", BOOST_COUNT());
}

void ui_lcd_push(const char *kind, int x, int y, int w, int h)
{
    (void)kind;                 /* the trace compiles away on target */
    if (w <= 0 || h <= 0)
        return;
    lcd_set_viewport(NULL);
    if (x == 0 && w == LCD_WIDTH)
        UPDATE_TRACE("update %s y=%d h=%d", kind, y, h);
    else
        UPDATE_TRACE("update %s x=%d y=%d w=%d h=%d", kind, x, y, w, h);
    lcd_update_rect(x, y, w, h);
    uilat_pixels();                 /* np-lag probe: input to pixels */
    /* P12: the class accountant's fps. A slide's frames are the shell's
     * (a push or pop between screens), and an SDK frame's later bands
     * ("sdk+") belong to its first: neither is a frame of the module */
    modclass_push(strcmp(kind, "anim:slide") && strcmp(kind, "sdk+"));
}

bool ui_strip_init(void)
{
    if (strip_handle > 0)
        return true;
#ifdef SIMULATOR
    /* test knob (tests/ui/ipodui-p3-nostrip.script): the strip
     * allocation fails, so every motion falls back to snapping */
    if (getenv("IPODUI_NO_STRIP"))
    {
        logf("ipodui: IPODUI_NO_STRIP set, no strip");
        return false;
    }
#endif
    strip_handle = core_alloc(UI_STRIP_SIZE);
    if (strip_handle <= 0)
    {
        strip_handle = 0;
        logf("ipodui: no memory for the strip, animations off");
        return false;
    }
    strip_owner = 0;
    return true;
}

bool ui_strip_available(void)
{
    return strip_handle > 0;
}

void *ui_strip_claim(int owner, bool *kept)
{
    if (strip_handle <= 0)
        return NULL;
    *kept = (owner == strip_owner);
    strip_owner = owner;
    /* the buffer can move whenever someone else allocates (font glyph
     * loads during text rendering read from disk and yield); pinned
     * until ui_strip_put() */
    core_pin(strip_handle);
    return core_get_data(strip_handle);
}

bool ui_strip_owned_by(int owner)
{
    return strip_handle > 0 && owner == strip_owner;
}

void ui_strip_put(void)
{
    if (strip_handle > 0)
        core_unpin(strip_handle);
}

int ui_strip_new_owner(void)
{
    return ++owner_seq;
}
