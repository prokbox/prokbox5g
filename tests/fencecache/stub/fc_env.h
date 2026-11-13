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

/* tests/fencecache: the environment of the REAL cache section of
 * firmware/target/arm/pp/system-pp502x.c (run.sh slices it out verbatim):
 * the PP5022's cache registers and the ARM's interrupt mask as hooks of
 * the test (test_fencecache.c) that record every flush, every mask change
 * and how far the status rewrite had got at each. */
#ifndef FC_ENV_H
#define FC_ENV_H

#include <stdbool.h>
#include <stdint.h>

#define HAVE_CORE_LAYOUT_A
#define CPU                 0
#define COP                 1
#define CACHEALIGN_SIZE     16
#define ICODE_ATTR
#define IBSS_ATTR
#define nop                 ((void)0)

/* the ARM CPSR's mask bits (set: masked), as system-arm.h */
#define IRQ_STATUS          0x80
#define FIQ_STATUS          0x40
#define IRQ_FIQ_STATUS      0xc0
int  disable_interrupt_save(int mask);
void restore_interrupt(int cpsr);

/* the cache controller: CACHE_OPERATION is written (|= FLUSH), then
 * CACHE_CTL polled until not busy; the poll is where the test sees the
 * flush */
#define CACHE_CTL_ENABLE    0x00000001
#define CACHE_CTL_BUSY      0x00008000
#define CACHE_OP_FLUSH      0x00000002
extern volatile unsigned long fc_cache_op;
volatile unsigned long *fc_ctl(void);
#define CACHE_OPERATION     fc_cache_op
#define CACHE_CTL           (*fc_ctl())

/* the status words: one per 16-byte line, 8 KB of them per core */
extern volatile unsigned long fc_status[2][0x2000 / sizeof(unsigned long)];
#define CACHE_STATUS_BASE_CPU   (fc_status[0][0])
#define CACHE_STATUS_BASE_COP   (fc_status[1][0])

extern unsigned int fc_core;
#define CURRENT_CORE        fc_core
uint32_t fc_usec(void);
#define USEC_TIMER          fc_usec()

#endif /* FC_ENV_H */
