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
 * The PP's second DMA engine writing the BCM's host port (e2-port.h).
 *
 * The sequence, as session 37's lab ran it on silicon (E2-3, E2-4,
 * E2-7(1): every word of 256 B to 64 KB landed):
 *  1. the BCM write address as two halfword stores to the port's address
 *     register, low half first, then the port's write-ready bit;
 *  2. the whole data cache written back (the engine reads memory, not the
 *     cache);
 *  3. engine on (master bit 31); channel 0's command initialised (bits 31,
 *     30, 28, 25, 24 and 23:16 clear, bit 26 set); source and destination
 *     configuration code 2; the source by its uncached address, the
 *     destination the port's data register; the command with bit 27 and
 *     the count (bytes - 4); a status read; then the start bit.
 * Bit 30 (interrupt) stays clear: then no interrupt source rises (E2-7(3)),
 * so nothing else (the audio's FIQ) sees the engine.  The end: the status
 * busy bit (31) and the command's start bit both clear.
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
#include "system.h"
#include "cpu.h"
#include "e2-port.h"
#include "xcore.h"

#define E2_MASTER     (*(volatile uint32_t *)0x60008000)
#define E2_CMD        (*(volatile uint32_t *)0x60009000)
#define E2_STATUS     (*(volatile uint32_t *)0x60009004)
#define E2_SRC        (*(volatile uint32_t *)0x60009010)
#define E2_SRC_CFG    (*(volatile uint32_t *)0x60009014)
#define E2_DST        (*(volatile uint32_t *)0x60009018)
#define E2_DST_CFG    (*(volatile uint32_t *)0x6000901C)
#define E2_START      (1u << 31)
#define E2_INTR       (1u << 30)
#define E2_BUSY       (1u << 31)        /* in STATUS */
#define E2_CFG_MASK   (7u << 24)

#define PORT_DATA     0x30000000u
#define PORT_ADDR_LO  (*(volatile unsigned short *)0x30010000)
#define PORT_ADDR_HI  (*(volatile unsigned short *)0x30010002)
#define PORT_CONTROL  (*(volatile unsigned short *)0x30030000)
#define PORT_WR_READY 0x2
#define PORT_US       20000             /* a handshake takes microseconds */

/* The engine's owner: the LCD's pushes and the video player may run on
 * different cores (P19), so the word is uncached and the claim is a
 * corelock'd test-and-set (P19 S0, xcore.h).  Interrupts off on the
 * calling core first: the corelock is not IRQ-safe. */
static volatile enum e2port_owner owner XCORE_BSS;
#if NUM_CORES > 1 && defined(HAVE_COP_COHERENCE)
#include "corelock.h"
static struct corelock owner_cl XCORE_BSS;    /* zeroed: unlocked */
#define OWNER_LOCK()    corelock_lock(&owner_cl)
#define OWNER_UNLOCK()  corelock_unlock(&owner_cl)
#else
#define OWNER_LOCK()    do { } while (0)
#define OWNER_UNLOCK()  do { } while (0)
#endif

bool e2port_claim(enum e2port_owner who)
{
    int level = disable_irq_save();
    bool ok;

    OWNER_LOCK();
    ok = owner == E2PORT_FREE || owner == who;
    if (ok)
        owner = who;
    OWNER_UNLOCK();
    restore_irq(level);
    return ok;
}

void e2port_release(enum e2port_owner who)
{
    int level = disable_irq_save();

    OWNER_LOCK();
    if (owner == who)
        owner = E2PORT_FREE;
    OWNER_UNLOCK();
    restore_irq(level);
}

bool e2port_fits(const void *src, uint32_t bytes)
{
    uintptr_t a = (uintptr_t)src;

    return bytes >= 4 && bytes <= E2PORT_PIECE_MAX && !(bytes & 3) &&
           !(a & 3) && a < 0x40000000;
}

static bool port_ready(void)
{
    unsigned long t0 = USEC_TIMER;

    while (!(PORT_CONTROL & PORT_WR_READY))
        if (USEC_TIMER - t0 > PORT_US)
            return false;
    return true;
}

bool e2port_port_ready(void)
{
    return port_ready();
}

int e2port_start(uint32_t bcm_addr, const void *src, uint32_t bytes)
{
    uintptr_t a = (uintptr_t)src;
    uint32_t v;

    /* the port's write address ignores bits 1:0 [silicon, CR1 2026-10-25]:
       a block from 2 mod 4 would land 2 bytes low; refused */
    if (bcm_addr & 3)
        return -1;
    /* whole words only: the engine moves 32-bit words, and a halfword
       tail would be the port's lone trailing halfword, which it loses
       [silicon, ALGNP PLH1/3/5]; every caller checks e2port_fits() */
    if ((bytes & 3) || bytes < 4)
        return -1;
    /* the high half at 0x30010002; Apple's own driver writes both halves
       to 0x30010000, low then high [apple-RE] (docs/data-paths.md).  Kept
       as is; bench row CR1 tries Apple's form beside ours */
    PORT_ADDR_LO = bcm_addr & 0xFFFF;
    PORT_ADDR_HI = bcm_addr >> 16;
    if (!port_ready())
        return -1;
    commit_dcache();
    E2_MASTER |= 1u << 31;
    v = E2_CMD;
    v &= ~(E2_START | E2_INTR | 1u << 28 | 1u << 25 | 1u << 24 |
           0xFFu << 16 | 0xFFFFu);
    v |= 1u << 26 | 1u << 27 | ((bytes - 4) & 0xFFFF);
    E2_SRC_CFG = (E2_SRC_CFG & ~E2_CFG_MASK) | 2u << 24;
    E2_DST_CFG = (E2_DST_CFG & ~E2_CFG_MASK) | 2u << 24;
    E2_SRC = a < UNCACHED_BASE_ADDR ? (uint32_t)(a | UNCACHED_BASE_ADDR)
                                    : (uint32_t)a;
    E2_DST = PORT_DATA;
    E2_CMD = v;
    (void)E2_STATUS;            /* a stale event, acknowledged */
    E2_CMD = v | E2_START;
    return 0;
}

bool e2port_busy(void)
{
    if ((E2_STATUS & E2_BUSY) || (E2_CMD & E2_START))
        return true;
    (void)E2_STATUS;
    return false;
}

void e2port_stop(void)
{
    int level = disable_interrupt_save(IRQ_FIQ_STATUS);

    E2_CMD &= ~E2_INTR;
    E2_CMD &= ~E2_START;
    restore_interrupt(level);
    (void)E2_STATUS;
}
