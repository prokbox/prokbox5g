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
 * ProkBox (Fit-1 M0): the measurement kit's counters. See devstats.h.
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

#ifdef HAVE_DEVSTATS
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "devstats.h"
#include "string-extra.h"
#include "../kernel/thread-internal.h"

/* Both cores write here (each its own idle and its own threads' time):
 * uncached, or a 16-byte line written back from one core's cache would
 * undo the other's counts (Fit-2; cores-and-threads.md section 4) */
struct devstats devstats SHAREDBSS_ATTR;
#if !defined(SIMULATOR) && (CONFIG_PLATFORM & PLATFORM_NATIVE)
uint32_t devstats_last_us[NUM_CORES] SHAREDBSS_ATTR;
#endif

static inline uint32_t now_us(void)
{
#if !defined(SIMULATOR) && (CONFIG_PLATFORM & PLATFORM_NATIVE)
    return USEC_TIMER;
#else
    return (uint32_t)current_tick * (1000000 / HZ);
#endif
}

void devstats_io(bool write, unsigned long sector, unsigned long count,
                 bool wake, bool pio)
{
    unsigned int slot = THREAD_ID_SLOT(thread_self());
    struct devstats_thread *t;
    int old;

    if (slot >= DEVSTATS_SLOTS)
        return;
    old = disable_irq_save();
    t = &devstats.t[slot];
    t->cmds++;
    t->sectors += count;
    if (pio && t->pio != 0xffff)
        t->pio++;
    if (wake)
    {
        struct devstats_wake *w =
            &devstats.wake[devstats.wake_head++ % DEVSTATS_WAKES];

        if (t->wakes != 0xffff)
            t->wakes++;
        w->tick = current_tick;
        w->sector = (uint32_t)sector;
        w->count = count > 0xffff ? 0xffff : (uint16_t)count;
        w->slot = (uint8_t)slot;
        w->write = write ? 1 : 0;
    }
    restore_irq(old);
}

void devstats_disk_sleep(bool ok)
{
    if (ok)
        devstats.sleeps++;
    else
        devstats.sleep_fails++;
}

void devstats_disk_reset(void)
{
    devstats.resets++;
}

void devstats_ata_irq(bool woke)
{
    if (woke)
        devstats.irq_waits++;
    else
        devstats.irq_off = 1;
}

/* UI thread (the request APIs) */
void devstats_request(int why)
{
    int old = disable_irq_save();
    devstats.req_us = now_us();
    devstats.req_why = (uint8_t)why;
    devstats.req_seq++;
    restore_irq(old);
}

/* the audio or codec thread, or an unpause: closes an open request */
void devstats_pcm_start(void)
{
    int old = disable_irq_save();
    if (devstats.pcm_seq != devstats.req_seq)
    {
        devstats.pcm_us = now_us();
        devstats.pcm_seq = devstats.req_seq;
    }
    restore_irq(old);
}

void devstats_snapshot(struct devstats *out)
{
    int old = disable_irq_save();
    memcpy(out, &devstats, sizeof(*out));
    restore_irq(old);
}

/* No thread_get_debug_info(): it scans the stack with IRQs masked. A
 * thread's name is a string constant that outlives it, so a reused or
 * freed slot at worst names its last thread. */
void devstats_slot_name(unsigned int slot, char *buf, int size)
{
    const char *n = NULL;

    if (slot < DEVSTATS_SLOTS)
        n = __thread_slot_entry(slot)->name;
    strlcpy(buf, (n && *n) ? n : "-", size);
}
#endif /* HAVE_DEVSTATS */
