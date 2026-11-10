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

/* The one port lock's depth logic (bcmjob-dev.c bcm_port_lock and
 * bcm_port_unlock; lcd-video.c's comment at THE PORT LOCK has the rules):
 * the outermost lock takes lcd_port_mtx and keeps lcd_tick out
 * (lcd_state.blocked); inner locks only count.
 *
 * The includer defines, before including this file:
 *   bool bpl_mutex(void)          lcd_port_lock(): true if the mutex was
 *                                 taken (false with IRQs off, or before
 *                                 the mutex is set up: no exclusion)
 *   void bpl_mutex_give(bool m)   lcd_port_unlock(m)
 *   bool bpl_block(void)          lcd_state.blocked set, under its
 *                                 corelock with IRQs off; its value before
 *   void bpl_restore(bool was)    lcd_state.blocked = was, the same way
 * A host test (tests/bcmportlock) includes it against a model. */
#ifndef BCM_PORT_LOCK_H
#define BCM_PORT_LOCK_H

#include <stdbool.h>

struct bpl_state
{
    int depth;                  /* bcm_port_lock() nesting */
    bool mtx;                   /* the outermost lock took the mutex */
    bool was;                   /* lcd_state.blocked as the outermost found it */
};

static inline void bpl_lock(struct bpl_state *s)
{
    bool m = bpl_mutex();       /* recursive for its owner; others wait */

    if (s->depth++ == 0)
    {
        s->mtx = m;
        s->was = bpl_block();   /* the tick's reads of the command word */
    }
}

static inline void bpl_unlock(struct bpl_state *s)
{
    bool m = s->mtx;

    /* the block as the outermost lock found it: free (a pending update
       goes at the next tick), or still set for a holder around us (a push
       band; 2026-11-09: this put false back unconditionally, letting the
       tick in mid-band had anything nested) */
    if (--s->depth == 0)
        bpl_restore(s->was);
    bpl_mutex_give(m);
}

#endif /* BCM_PORT_LOCK_H */
