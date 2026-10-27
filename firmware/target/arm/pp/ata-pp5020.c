/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2006 by Barry Wardell
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

/* ATA stuff was taken from the iPod code */

#include <stdbool.h>
#include "system.h"
#include "kernel.h"
#include "ata-driver.h"
#include "ata.h"
#include "devstats.h"
#if defined(HAVE_USB_PREFETCH) && !defined(BOOTLOADER)
#include "usb.h"
#endif

void ata_reset()
{

}

void ata_enable(bool on)
{
    /* TODO: Implement ata_enable() */
    (void)on;
}

bool ata_is_coldstart()
{
    return false;
    /* TODO: Implement coldstart variable */
}

/* These are PIO timings for 80 Mhz.  At 24 Mhz, the first value is 0 but the
   rest are the same. They go in IDE0_PRI_TIMING0.

   Rockbox used to use 0x10, and test_disk shows that leads to faster PIO.
   However when used with mSATA and some SD adapters this causes corrupt data
   so we now unconditionally use these timings from the OF.
*/
static const unsigned long pio80mhz[] = {
    0xC293, 0x43A2, 0x11A1, 0x7232, 0x3131
};

#ifdef HAVE_ATA_DMA_IRQ
/* ProkBox (Fit-1 C4b): the end of a DMA transfer raises the drive's
 * INTRQ, seen in IDE0_CFG bit 3 and as interrupt source 23 (IDE_IRQ). The
 * waiting thread sleeps on a semaphore the handler releases, so the core
 * idles through the transfer instead of yielding in a poll loop. Every
 * wait is sliced (DMA_IRQ_SLICE) and re-reads the bit itself, so a lost
 * or unrouted interrupt costs at most a slice; DMA_IRQ_MISS_MAX
 * consecutive transfers that end without the interrupt switch the driver
 * back to polling for the rest of the boot. The handler only masks the
 * source and releases; it never reads the ATA status (quirks.md #26). */
#define DMA_IRQ_SLICE       2       /* ticks */
#define DMA_IRQ_MISS_MAX    3
static struct semaphore dma_sem;
static bool dma_irq_off;            /* polling: the interrupt was not seen */
static unsigned char dma_irq_misses;

void ata_dma_irq(void)
{
    CPU_INT_DIS = IDE_MASK;
    semaphore_release(&dma_sem);
}
#endif

void ata_device_init()
{
#ifdef HAVE_ATA_DMA_IRQ
    CPU_INT_DIS = IDE_MASK;
    semaphore_init(&dma_sem, 1, 0);
#endif
#ifdef SAMSUNG_YH920
    CPU_INT_DIS = (1<<IDE_IRQ);
#endif
#ifdef HAVE_ATA_DMA
    IDE_DMA_CONTROL |= 2;
    IDE_DMA_CONTROL &= ~1;
    IDE0_CFG &= ~0x8010;
    IDE0_CFG |= 0x20;
#else

    /* From ipod-ide.c:ipod_ide_register() */
    IDE0_CFG |= (1<<5);
#ifdef IPOD_NANO
    IDE0_CFG |= (0x10000000); /* cpu > 65MHz */
#else
    IDE0_CFG &=~(0x10000000); /* cpu < 65MHz */
#endif
#endif

    IDE0_PRI_TIMING0 = pio80mhz[0];
    IDE0_PRI_TIMING1 = 0x80002150;
}

/* Setup the timing for PIO mode */
void ata_set_pio_timings(int mode)
{
    IDE0_PRI_TIMING0 = pio80mhz[mode];
}

#ifdef HAVE_ATA_DMA
/* Timings for multi-word and ultra DMA modes.
   These go in IDE0_PRI_TIMING1
 */
static const unsigned long tm_mwdma[] = {
    0xF9F92, 0x56562, 0x45451
};

static const unsigned long tm_udma[] = {
    0x800037C1, 0x80003491, 0x80003371,
#if ATA_MAX_UDMA > 2
    0x80003271, 0x80003071
#endif
};

#if ATA_MAX_UDMA > 2
static bool dma_boosted = false;
static bool dma_needs_boost;
#endif

static int ata_is_ssd = 0;

/* This function sets up registers for 80 Mhz.
   Ultra DMA mode 2 works at 30 Mhz.
 */
void ata_dma_set_mode(unsigned char mode) {
    int modeidx;

    (*(volatile unsigned long *)(0x600060C4)) = 0xC0000000; /* 80 Mhz */
#if !defined(IPOD_NANO)
    IDE0_CFG &= ~0x10000000;
#endif

    modeidx = mode & 7;
    mode &= 0xF8;
    if (mode == 0x40 && modeidx <= ATA_MAX_UDMA) {
        IDE0_PRI_TIMING1 = tm_udma[modeidx];
#if ATA_MAX_UDMA > 2
        if (modeidx > 2)
            dma_needs_boost = true;
        else
            dma_needs_boost = false;
#endif
    } else if (mode == 0x20 && modeidx <= ATA_MAX_MWDMA)
        IDE0_PRI_TIMING1 = tm_mwdma[modeidx];

#if !defined(IPOD_NANO)
    IDE0_CFG |= 0x20000000; /* >= 50 Mhz */
#endif

    ata_is_ssd = ata_disk_isssd();
}

#define IDE_CFG_INTRQ           8
#define IDE_DMA_CONTROL_READ    8

#if defined(HAVE_USB_PREFETCH) && !defined(BOOTLOADER)
/* ProkBox (USB reliability): while the computer owns the disk, a wait for
 * the end of a DMA transfer that sleeps or yields lets the main thread
 * run, and its next LCD push (~110 ms, no yield) delays the completion,
 * which macOS took for a dead device and answered with a bus reset (upstream
 * e13befb925 found the same on iFlash units). A wait made by the thread
 * that answers the host (a READ or WRITE it serves itself; nothing else
 * could use the time) therefore spins on INTRQ and runs nothing else. The
 * prefetch worker is the exception, `ata_usb_bg_thread`: the USB thread
 * must run while it waits (that overlap is what the ring is for), and a
 * late landing costs the host nothing, its READs are answered from the
 * ring. Outside USB mode nothing changes. */
unsigned int ata_usb_bg_thread;     /* set by usb_storage.c; 0: none */

static bool ata_usb_spin(void)
{
    return usb_exclusive_storage() && thread_self() != ata_usb_bg_thread;
}
#endif

/* This waits for an ATA interrupt using polling.
   In ATA_CONTROL, CONTROL_nIEN must be cleared.
 */
static ICODE_ATTR int ata_wait_intrq(void)
{
    long timeout = current_tick + HZ*10;

#if defined(HAVE_USB_PREFETCH) && !defined(BOOTLOADER)
    if (ata_usb_spin())
    {
        do
        {
            if (IDE0_CFG & IDE_CFG_INTRQ)
                return 1;
            ata_keep_active();
        } while (TIME_BEFORE(current_tick, timeout));
        return 0;
    }
#endif

#ifdef HAVE_ATA_DMA_IRQ
    if (!dma_irq_off)
    {
        do
        {
            int rc;

            if (IDE0_CFG & IDE_CFG_INTRQ)
                return 1;           /* done before we slept */
            CPU_INT_EN = IDE_MASK;  /* a pending INTRQ fires at once */
            rc = semaphore_wait(&dma_sem, DMA_IRQ_SLICE);
            CPU_INT_DIS = IDE_MASK;
            ata_keep_active();
            if (IDE0_CFG & IDE_CFG_INTRQ)
            {
                if (rc == OBJ_WAIT_SUCCEEDED)
                {
                    dma_irq_misses = 0;
                    DEVSTATS_ATA_IRQ(true);
                }
                else if (++dma_irq_misses >= DMA_IRQ_MISS_MAX)
                {
                    dma_irq_off = true;     /* not routed: poll */
                    DEVSTATS_ATA_IRQ(false);
                }
                return 1;
            }
            /* a stale release from a late interrupt: loop, re-check */
        } while (TIME_BEFORE(current_tick, timeout));
        return 0;
    }
#endif

    do
    {
        if (IDE0_CFG & IDE_CFG_INTRQ)
            return 1;
        ata_keep_active();
        yield();
    } while (TIME_BEFORE(current_tick, timeout));

    return 0; /* timeout */
}

/* This function checks if parameters are appropriate for DMA,
   and if they are, it sets up for DMA.

   If return value is false, caller may use PIO for this transfer.

   If return value is true, caller must issue a DMA ATA command
   and then call ata_dma_finish().
 */
bool ata_dma_setup(void *addr, unsigned long bytes, bool write) {
    /* Require cacheline alignment for reads to prevent interference. */
    if (!write && ((unsigned long)addr & 15))
        return false;

    /* Writes only need to be word-aligned, but by default DMA
     * is not used for writing on non-SSDs as it appears to be slower.
     */
    if (write) {
        if ((unsigned long)addr & 3)
            return false;
        if (!ata_is_ssd)
            return false;
#ifdef HAVE_ATA_UDMA_FORCE
        if (ata_udma_forced_now())
            return false;           /* C7: writes by PIO while forced */
#endif
    }

#ifdef HAVE_PLL_IDLE
    /* Fit-1 C6: UDMA2 is rated at 30 MHz, so no transfer at 24 MHz.
     * Held before a UDMA4 boost (below), so that boost starts from 30 MHz
     * and the 24 MHz policy does not count it as the codec being rescued:
     * S36 struck FLAC for ~56k boosts that were the library scan's
     * forced-UDMA4 transfers, one per command (clockpol.c) */
    cpu_pll_hold(CPU_PLL_HOLD_ATA, true);
#endif
#if ATA_MAX_UDMA > 2
    if (dma_needs_boost && !dma_boosted) {
        cpu_boost(true);
        dma_boosted = true;
    }
#endif

    if (write) {
        /* If unflushed, old data may be written to disk */
        commit_dcache();
    }
    else {
        /* Invalidate cache because new data may be present in RAM */
        commit_discard_dcache();
    }

    /* Clear pending interrupts so ata_dma_finish() can wait for an
       interrupt from this transfer
     */
    IDE0_CFG |= IDE_CFG_INTRQ;

    IDE_DMA_CONTROL |= 2;
    IDE_DMA_LENGTH = bytes - 4;

#if !defined(BOOTLOADER) || defined (HAVE_BOOTLOADER_USB_MODE)
    if ((unsigned long)addr < DRAM_START)
        /* Rockbox remaps DRAM to start at 0 */
        IDE_DMA_ADDR = (unsigned long)addr + DRAM_START;
    else
#endif
        IDE_DMA_ADDR = (unsigned long)addr;

    if (write)
        IDE_DMA_CONTROL &= ~IDE_DMA_CONTROL_READ;
    else
        IDE_DMA_CONTROL |= IDE_DMA_CONTROL_READ;

    IDE0_CFG |= 0x8000;

    return true;
}

/* This function waits for a DMA transfer to end.
   It must be called to finish what ata_dma_setup started.

   Return value is true if DMA completed before the timeout, and false
   if a timeout happened.
 */
bool ata_dma_finish(void) {
    bool res;

    /* It may be okay to put this at the end of setup */
    IDE_DMA_CONTROL |= 1;

    /* Wait for end of transfer.
       Reading standard ATA status while DMA is in progress causes
       failures and hangs.  Because of that, another wait is used.
     */
    res = ata_wait_intrq();

    IDE0_CFG &= ~0x8000;
    IDE_DMA_CONTROL &= ~0x80000001;

#if ATA_MAX_UDMA > 2
    if (dma_boosted) {
        cpu_boost(false);
        dma_boosted = false;
    }
#endif
#ifdef HAVE_PLL_IDLE
    cpu_pll_hold(CPU_PLL_HOLD_ATA, false);
#endif

    return res;
}

#endif /* HAVE_ATA_DMA */
