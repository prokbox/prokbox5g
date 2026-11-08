/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Björn Stenberg
 * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
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
#include "system.h"
#include "kernel.h"
#ifdef HAVE_CORE_LAYOUT_A
#include "corelayout.h"
#include "cop_park.h"

/* ProkBox K-1 (corelayout.h, cop_park.h): the switch and the tick's
 * pulse counts.  The CPU alone reads and writes them (its tick, its
 * threads); single words */
static bool cop_parked;
static uint32_t k1_sent, k1_held;

void corelayout_set_park(bool on)
{
    cop_parked = on;
    if (!on)
        core_wake(COP);     /* back to the pulse: one at once */
}

bool corelayout_park(void)
{
    return cop_parked;
}

void corelayout_park_stats(uint32_t *sent, uint32_t *held)
{
    *sent = k1_sent;
    *held = k1_held;
}
#endif /* HAVE_CORE_LAYOUT_A */

#if !defined(BOOTLOADER) || defined(HAVE_BOOTLOADER_USB_MODE)
void TIMER1(void)
{
    /* Run through the list of tick tasks (using main core) */
    TIMER1_VAL; /* Read value to ack IRQ */

    /* Run through the list of tick tasks using main CPU core - 
       wake up the COP through its control interface to provide pulse */
    call_tick_tasks();

#if NUM_CORES > 1
#ifdef HAVE_CORE_LAYOUT_A
    /* K-1: parked, the COP is pulsed only when its next timeout check is
     * due; every other reason to run there wakes it already */
    if (!cop_park_tick_wakes(cop_parked, current_tick, cop_next_tmo_check()))
    {
        k1_held++;
        return;
    }
    k1_sent++;
#endif
    /* Pulse the COP */
    core_wake(COP);
#endif /* NUM_CORES */
}
#endif

/* Must be last function called init kernel/thread initialization */
void tick_start(unsigned int interval_in_ms)
{
#if !defined(BOOTLOADER) || defined(HAVE_BOOTLOADER_USB_MODE)
    TIMER1_CFG = 0x0;
    TIMER1_VAL;
    /* enable timer */
    TIMER1_CFG = 0xc0000000 | (interval_in_ms*1000 - 1);
    /* unmask interrupt source */
    CPU_INT_EN = TIMER1_MASK;
#else
    /* We don't enable interrupts in the bootloader */
    (void)interval_in_ms;
#endif
}

#ifdef HAVE_BOOTLOADER_USB_MODE
void tick_stop(void)
{
    CPU_INT_DIS = TIMER1_MASK;
    TIMER1_CFG = 0;
}
#endif
