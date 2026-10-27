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

/* Every display job's CPU path (bcmjob_pp.h): the reference executor
 * (bcmjob-ref.c) on PP memory.  J_SURF_FRONT is the PP framebuffer; there
 * is no back frame; PRESENT is the LCD contract's push (lcd_update_rect).
 * The executor's state (~3 KB: the slot table and pool descriptors) is
 * one pinned core_alloc block taken at shell start, never a static.
 * Built for the device (HAVE_BCM_JOBS) and the simulator
 * (HAVE_BCM_JOBS_MODEL) alike, so both run the same CPU path. */
#include "config.h"
#include <string.h>
#include "lcd.h"
#include "kernel.h"
#include "core_alloc.h"
#include "bcmjob_pp.h"
#include "bcmjob.h"
#include "hostproto.h"
#include "jref.h"
#include "xcore.h"

static struct jref *cpu;        /* the block's data (pinned); NULL: none */

/* Screen truth's owned rect (bcmjob_pp.h): written by the UI thread, read
 * by every push on either core (lcd_update_rect), so uncached */
static volatile int16_t owned[4] XCORE_BSS;
static volatile uint32_t owned_gen XCORE_BSS;  /* bcmjob_power_gen() then */
static volatile uint8_t drift_mode XCORE_BSS;
static void (*owned_hook)(void);
static unsigned int hook_thread;

void bcmjob_owned_set(int x, int y, int w, int h)
{
    owned[2] = 0;               /* never a half-written rect: w last */
    owned[0] = x;
    owned[1] = y;
    owned[3] = h;
    owned_gen = bcmjob_power_gen();
    owned[2] = w > 0 && h > 0 && owned_gen ? w : 0;
}

bool bcmjob_owned_get(int *x, int *y, int *w, int *h)
{
    int ow = owned[2];

    if (ow <= 0 || owned_gen != bcmjob_power_gen())
        return false;
    *x = owned[0];
    *y = owned[1];
    *w = ow;
    *h = owned[3];
    return true;
}

void bcmjob_owned_hook(void (*settle)(void))
{
    hook_thread = thread_self();
    owned_hook = settle;
}

void bcmjob_owned_settle(void)
{
    int x, y, w, h;

    if (owned_hook && thread_self() == hook_thread
        && bcmjob_owned_get(&x, &y, &w, &h))
        owned_hook();
}

void bcmjob_drift_mode_set(enum bcmjob_drift_mode m)
{
    if ((unsigned)m < BCMJOB_DRIFT_MODES)
        drift_mode = m;
}

enum bcmjob_drift_mode bcmjob_drift_mode(void)
{
    return (enum bcmjob_drift_mode)drift_mode;
}

static void cpu_present(const struct jref_surf *s, uint32_t xy, uint32_t wh,
                        uint32_t flags)
{
    (void)flags;
    if (s == &cpu->front)       /* the framebuffer itself: today's push */
        lcd_update_rect(J_X(xy), J_Y(xy), J_X(wh), J_Y(wh));
    /* a slot to the panel: the CPU path never presents a slot directly
     * (the shell blits it to the framebuffer first) */
}

bool bcmjob_cpu_init(void)
{
    struct jref_surf fb = { (uint16_t *)FBADDR(0, 0), LCD_WIDTH, LCD_HEIGHT,
                            LCD_WIDTH };
    struct jref_surf none = { NULL, 0, 0, 0 };
    int h;

    if (cpu)
        return true;
    XCORE_ASSERT_CPU("bcmjob_cpu_init");    /* allocates and pins */
    h = core_alloc(sizeof(struct jref));
    if (h <= 0)
        return false;
    core_pin(h);
    cpu = core_get_data(h);
    jref_init(cpu, fb, none, fb);
    cpu->present = cpu_present;
    return true;
}

int bcmjob_cpu_slot(uint32_t slot, uint16_t *px, uint32_t w, uint32_t h,
                    uint32_t pitch)
{
    struct jref_slot *s;

    if (!cpu || slot >= JREF_SLOTS)
        return -1;
    s = &cpu->slot[slot];
    memset(s, 0, sizeof(*s));
    if (px)
    {
        s->surf.px = px;
        s->surf.w = s->w = w;
        s->surf.h = s->h = h;
        s->surf.pitch = pitch;
        s->bytes = w * h * 2;
        s->fmt = J_FMT_RGB565;
        s->epoch = cpu->epoch;
        s->state = J_ST_READY;
    }
    return 0;
}

int bcmjob_cpu_run(uint32_t op, uint32_t tag, const uint32_t *payload,
                   uint32_t n)
{
    uint32_t rec[1 + HP_MAX_PAYLOAD], rep[1 + HP_MAX_PAYLOAD], nrep;

    if (!cpu || n > HP_MAX_PAYLOAD)
        return -1;
    rec[0] = HP_REC(op, n, tag);
    memcpy(&rec[1], payload, 4 * n);
    return (int)jref_exec(cpu, rec, rep, &nrep);
}

/* P19 S0 (MF-5): the pixels pinned for the whole upload, the engine-2
 * waits and the checksum included (a compaction can run in any yield) */
int bcmjob_upload_handle(const struct bcmjob_slot *s, int h,
                         uint32_t block_bytes, uint32_t offset,
                         uint32_t px_bytes)
{
    char *base;
    int r;

    XCORE_ASSERT_CPU("bcmjob_upload_handle");
    /* the pixels inside the block, halfword-aligned (S0 review L5) */
    if (h <= 0 || (offset & 1) || offset > block_bytes ||
        px_bytes > block_bytes - offset)
        return BCMJOB_FAILED;
    base = core_get_data_pinned(h);
    r = bcmjob_upload(s, (const uint16_t *)(base + offset));
    core_put_data_pinned(base);
    return r;
}
