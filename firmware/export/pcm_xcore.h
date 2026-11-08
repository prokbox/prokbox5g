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

/* rc.6 (layout A): the PCM's FIQ, the mix and pcmbuf's consumer run on the
 * COP while the CPU produces.  Two helpers both sides share, header-only so
 * tests/audiocop/ runs them as they are (docs/design/cop-shared-state.md,
 * layout A):
 *
 *  - pcm_mix_src(): a channel's source as the mix reads it.  Producers
 *    write SDRAM through the CPU's cache and write it back before handing
 *    it over; the mix reads the uncached alias, so no line of the ring sits
 *    in the COP's cache to go stale on the next lap.  NULL ("no more"),
 *    IRAM and the uncached alias pass unchanged.
 *  - the position mailbox: the FIQ posts a chunk's position (elapsed,
 *    offset, key) to uncached memory and the CPU's tick applies it, so the
 *    FIQ never writes the playing id3 or calls the dock code.  seq is odd
 *    while a post is under way; uncached stores leave in program order
 *    (quirk 81), so a read that sees one even seq before and after holds a
 *    whole position. */
#ifndef PCM_XCORE_H
#define PCM_XCORE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef PCM_XCORE_HOOK
#define PCM_XCORE_HOOK(where) do { } while (0)  /* tests interleave here */
#endif

static inline const void *pcm_mix_src(const void *p)
{
    return p && (uintptr_t)p < UNCACHED_BASE_ADDR ? UNCACHED_ADDR(p) : p;
}

struct pcm_pos_mbox
{
    volatile unsigned long elapsed;
    volatile off_t offset;
    volatile unsigned int key;
    volatile unsigned int seq;
};

/* The FIQ: one writer */
static inline void pcm_pos_post(struct pcm_pos_mbox *m, unsigned long elapsed,
                                off_t offset, unsigned int key)
{
    m->seq++;
    PCM_XCORE_HOOK(1);
    m->elapsed = elapsed;
    PCM_XCORE_HOOK(2);
    m->offset = offset;
    PCM_XCORE_HOOK(3);
    m->key = key;
    PCM_XCORE_HOOK(4);
    m->seq++;
}

/* The CPU's tick: true and a whole position when a post it has not seen
   is complete; *seen tracks the last one taken */
static inline bool pcm_pos_take(struct pcm_pos_mbox *m, unsigned int *seen,
                                unsigned long *elapsed, off_t *offset,
                                unsigned int *key)
{
    unsigned int s1 = m->seq, s2;

    if (s1 == *seen || (s1 & 1))
        return false;
    PCM_XCORE_HOOK(11);
    *elapsed = m->elapsed;
    PCM_XCORE_HOOK(12);
    *offset = m->offset;
    PCM_XCORE_HOOK(13);
    *key = m->key;
    PCM_XCORE_HOOK(14);
    s2 = m->seq;
    if (s2 != s1)
        return false;               /* rewritten meanwhile: the next tick */
    *seen = s1;
    return true;
}

#endif /* PCM_XCORE_H */
