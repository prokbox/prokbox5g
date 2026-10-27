/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * LCD driver for iPod Video
 *
 * Based on code from the ipodlinux project - http://ipodlinux.org/
 * Adapted for Rockbox in December 2005
 *
 * Original file: linux/arch/armnommu/mach-ipod/fb.c
 *
 * Copyright (c) 2003-2005 Bernard Leach (leachbj@bouncycastle.org)
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

#include <sys/types.h> /* off_t */
#include "config.h"
#include "cpu.h"
#include "lcd.h"
#include "kernel.h"
#include "system.h"
#include "devstats.h"   /* Fit-3 step 0: empty unless HAVE_DEVSTATS */
#include "xcore.h"      /* P19 S0: XCORE_BSS/DATA (empty in the stock build) */
#ifdef HAVE_FE_FB_HANDOFF
#include "fe_fb.h"      /* P19 MF-4: the producer's fence */
#endif
#if defined(HAVE_LCD_E2) && !defined(BOOTLOADER)
#include "e2-port.h"
#endif
#ifdef HAVE_LCD_SLEEP
/* Included only for lcd_awake() prototype */
#include "backlight-target.h"
#ifdef HAVE_POWER_SLEEP
#include "power_sleep.h"
#endif
#endif
#ifdef HAVE_PP_GOV_DVS
#include "pgov.h"       /* the band governor's band-3 port cap */
#endif

/* The BCM bus width is 16 bits. But since the low address bits aren't decoded
 * by the chip (the 3 BCM address bits are mapped to address bits 16..18 of the
 * PP5022), writing 32 bits (and even more, using 'stmia') at once works. */
#define BCM_DATA      (*(volatile unsigned short*)(0x30000000))
#define BCM_DATA32    (*(volatile unsigned long *)(0x30000000))
#define BCM_WR_ADDR   (*(volatile unsigned short*)(0x30010000))
#define BCM_WR_ADDR32 (*(volatile unsigned long *)(0x30010000))
#define BCM_RD_ADDR   (*(volatile unsigned short*)(0x30020000))
#define BCM_RD_ADDR32 (*(volatile unsigned long *)(0x30020000))
#define BCM_CONTROL   (*(volatile unsigned short*)(0x30030000))

#define BCM_ALT_DATA      (*(volatile unsigned short*)(0x30040000))
#define BCM_ALT_DATA32    (*(volatile unsigned long *)(0x30040000))
#define BCM_ALT_WR_ADDR   (*(volatile unsigned short*)(0x30050000))
#define BCM_ALT_WR_ADDR32 (*(volatile unsigned long *)(0x30050000))
#define BCM_ALT_RD_ADDR   (*(volatile unsigned short*)(0x30060000))
#define BCM_ALT_RD_ADDR32 (*(volatile unsigned long *)(0x30060000))
#define BCM_ALT_CONTROL   (*(volatile unsigned short*)(0x30070000))

/* Time until the BCM is considered stalled and will be re-kicked.
 * Must be guaranteed to be >~ 20ms. */
#define BCM_UPDATE_TIMEOUT (HZ/20)
/* An LCD update command done while the LCD is off needs >~ 200ms */
#ifdef HAVE_BCMFW
#define BCM_LCDINIT_TIMEOUT (HZ*3/4)    /* our image: panel 2 takes 451 ms */
#else
#define BCM_LCDINIT_TIMEOUT (HZ/2)
#endif

/* Addresses within BCM */
#define BCMA_SRAM_BASE   0
#define BCMA_COMMAND     0x1F8
#define BCMA_STATUS      0x1FC
#define BCMA_CMDPARAM    0xE0000    /* Parameters/data for commands */
#define BCMA_SDRAM_BASE  0xC0000000
#define BCMA_TV_FB       0xC0000000 /* TV out framebuffer */
#define BCMA_TV_BMPDATA  0xC0200000 /* BMP data for TV out functions */

/* BCM commands.  Write them to BCMA_COMMAND.  Note BCM_CMD encoding. */
#define BCM_CMD(x) ((~((unsigned long)x) << 16) | ((unsigned long)x))
#define BCMCMD_LCD_UPDATE     BCM_CMD(0)
/* Execute "M25 Diagnostics".  Status displayed on LCD.  Takes <40s */
#define BCMCMD_SELFTEST       BCM_CMD(1)
#define BCMCMD_TV_PALBMP      BCM_CMD(2)
#define BCMCMD_TV_NTSCBMP     BCM_CMD(3)
/* BCM_CMD(4) may be another TV-related command */
/* The following might do more depending on word at 0xE00000 */
#define BCMCMD_LCD_UPDATERECT BCM_CMD(5)
#define BCMCMD_LCD_SLEEP      BCM_CMD(8)
/* BCM_CMD(12) involved in shutdown */
/* Macrovision analog copy prevention is on by default on TV output.
   Execute this command after enabling TV out to turn it off.
 */
#define BCMCMD_TV_MVOFF       BCM_CMD(14)

enum lcd_status
{
    LCD_IDLE,
    LCD_INITIAL,
    LCD_NEED_UPDATE,
    LCD_UPDATING
};

struct
{
    long update_timeout;  /* also used to ensure BCM stays off for >= 50 ms */
    enum lcd_status state;
    bool blocked;
#if NUM_CORES > 1
    struct corelock cl;   /* inter-core sync */
#endif
#ifdef HAVE_LCD_SLEEP
    bool display_on;
    bool waking;
    struct semaphore initwakeup;
#endif
#ifdef HAVE_BCMFW
    /* With our image: the union of the rectangles written since the last
     * kick (x0 >= x1: none), sent by UPDATERECT */
    short x0, y0, x1, y1;
#endif
} lcd_state IBSS_ATTR;

#ifdef HAVE_LCD_SLEEP
const fb_data *flash_vmcs_offset;
unsigned flash_vmcs_length;
#if defined(HAVE_BCMFW_DEVTEST) || defined(HAVE_BCMFW)
/* bcm-devtest-hal.c's host-port layer, #included at the end: the device
 * test's (HAVE_BCMFW_DEVTEST) and the boot path's (HAVE_BCMFW, which a
 * release build has without the device test) */
#define BCM_HAL
#endif
#ifdef BCM_HAL
/* P19 S0: what both cores' port users read lives uncached (xcore.h) */
static bool bcm_devtest_owned XCORE_BSS;  /* bcm-devtest-hal.c has the BCM */
#endif
#ifdef HAVE_BCMFW
/* The opt-in boot path (bcm-devtest-hal.c): while bcmfw_active, our image
 * is the one flash_vmcs_* point at */
#include "bcm-devtest.h"
#define BCMFW_ST_DONE      0x50420001
#define BCMFW_ST_FAULT     0xFA170000
#define BCMFW_HANG_TIMEOUT (2*HZ)   /* one update busy this long: stopped */
#define BCMA_RECT_XY       0x1F0    /* UPDATERECT: x | y << 16 (bcmfw) */
#define BCMA_RECT_WH       0x1F4    /*             w | h << 16 */
/* Ours (dev8): the panel's power-on alone, so a wake uploads the first
 * frame while its waits run (bcmfw/bcmfw.h, CMD_LCD_POWERON) */
#define BCMCMD_LCD_POWERON BCM_CMD(0x40)
/* The upload loop for the framebuffer: 0 Rockbox's lcd_write_data (2-reg
 * ldm/stm), 1 lcd_write_data_burst8 (8-reg; lcd-as-video.S).  0 until
 * the device test's upload lab (stage 5, dev8) shows 1 is faster. */
#ifndef BCMFW_UPLOAD
#define BCMFW_UPLOAD 0
#endif
#if BCMFW_UPLOAD == 1
extern void lcd_write_data_burst8(const fb_data *addr, int pixelcount);
#define lcd_write_fb lcd_write_data_burst8
#endif
static bool bcmfw_poweron XCORE_BSS;    /* a wake's LCD_POWERON is running */
static const struct bcmdt_image *bcmfw_active XCORE_BSS;
static volatile bool bcmfw_hung XCORE_BSS;  /* set by lcd_tick, handled in threads */
static long bcmfw_kick_tick XCORE_BSS;  /* the update in flight was kicked */
static void bcmfw_fallback(const char *why);
static void bcmfw_drop(const char *why);
#ifdef HAVE_BCMFW_RESIDENT
static void bcmfw_woke(void);       /* the resident image: its idle gate */
#endif
#ifdef HAVE_BCM_JOBS
/* ProkBox P19 W0-6: the display job driver (bcmjob-dev.c, below): every
 * power-up moves bcmjob_gen, so the driver runs discovery again */
#include "bcmjob-core.h"
#include "bcmjob_pp.h"
static uint32_t bcmjob_gen XCORE_BSS;
/* the job driver is in job mode for this power-up (bcmjob-dev.c's
 * jd_mode, the ring owner's thread its only writer): bcmjob_gen + 1 then,
 * 0 otherwise, so a new power-up ends it without a second writer.
 * lcd_tick's hang check is the job layer's heartbeat check meanwhile. */
static volatile uint32_t bcmjob_on XCORE_BSS;
#define BCMJOB_ON() (bcmjob_on && bcmjob_on == bcmjob_gen + 1)
static void jd_dark(bool dark);
static bool jd_suspend(bool on);
#endif
#ifndef BCMJOB_ON
#define BCMJOB_ON() false
#endif
#ifdef HAVE_BCMFW_L1
/* #62 level 1 (bcmfw-boot.c): no host-port wait is unbounded.  A port
 * that never answers marks itself dead until the next power-up, so every
 * later access returns at once; with our image running, that is its
 * fallback (one rung down). */
#define BCM_PORT_US 20000           /* a handshake takes microseconds */
static volatile bool bcm_port_dead XCORE_BSS;
static bool bcm_entry_powered;      /* the BCM was powered at lcd_init */
static bool bcmfw_inject_wake;      /* debug: the next wake never answers */
static void bcmfw_note(const char *why);
#endif
#endif

#if !defined(BOOTLOADER) && defined(HAVE_IPOD_UI) && defined(HAVE_LCD_SLEEP)
/* ProkBox (the freeze audit, 2026-10-26: three silent freezes on bench
 * sitting 4): no thread waits on the BCM without a bound.  Each wait that
 * runs out records its cause here, from any context (the tick too: no
 * formatting, no I/O), and takes its recovery: the chip marked stopped
 * (bcmfw_hung: the next push power-cycles it, one rung down with our
 * image, Apple's vmcs reloaded otherwise), any lock released.  The job
 * owner writes each cause's line once a boot, "FAULT <who> wait=<what>
 * ms=<n>" (bcm_wait_faults_log(), from its input pump: RAM only), kicked
 * at once; the shell's log writes a FAULT line out at its next pass.
 * The stock build is unchanged. */
#define BCM_WAIT_BOUNDS
#include <stdio.h>
#include "button.h"                 /* button_queue_post: the owner's kick */
#define BCM_AWAKE_WAIT  (3 * HZ)    /* the first update: the tick re-kicks
                                       a stall every 50 ms and calls an
                                       image stopped after 2 s */
#define BCM_SLEEP_WAIT  HZ          /* the update in flight at a sleep */
#define BCMFW_JOB_HANG_TIMEOUT (6 * HZ) /* job mode: the heartbeat's check
                                       runs only while a job wait does; a
                                       BCM that dies between waits is the
                                       tick's after this */
enum
{
    BW_AWAKE = 0,
    BW_SLEEP,
    BW_POWERON,
    BW_COMMAND,
    BW_TICK,
    BW_TICK_JOBS,
    BW_TICK_VMCS,
    BW_N
};
#ifdef HAVE_BCM_JOBS
static const char *const bw_name[BW_N] =
{
    "lcd_awake wait=first-update",
    "lcd_sleep wait=update-idle",
    "lcd_awake wait=poweron",
    "bcm_command wait=done",
    "lcd_tick wait=update image=ours",
    "lcd_tick wait=update image=ours-jobs",
    "lcd_tick wait=update image=vmcs",
};
static void (*bw_trace)(const char *line) XCORE_BSS;  /* the job driver's */
#endif
static volatile uint32_t bw_pend XCORE_BSS;    /* recorded, not yet logged */
static uint32_t bw_logged XCORE_BSS;           /* logged this boot */
static uint32_t bw_ms[BW_N] XCORE_BSS;

/* A wait ran out after ticks: recorded once a boot per cause */
static void bcm_wait_fault(int k, long ticks)
{
    int oldlevel = disable_irq_save();
    bool first = !((bw_pend | bw_logged) & (1u << k));

    if (first)
    {
        bw_ms[k] = ticks > 0 ? (uint32_t)ticks * (1000 / HZ) : 0;
        bw_pend |= 1u << k;
    }
    restore_irq(oldlevel);
#ifdef HAVE_BCM_JOBS
    if (first)
        button_queue_post(SYS_BCMJOB_REQ, 0);   /* the owner logs it now */
#endif
}

#ifdef HAVE_BCM_JOBS
/* Thread context, the job owner's pump: the recorded causes' lines
 * (without the job driver they stay recorded, unlogged) */
static void bcm_wait_faults_log(void)
{
    char line[80];
    uint32_t p;
    int k, oldlevel;

    if (!bw_pend)
        return;
    oldlevel = disable_irq_save();
    p = bw_pend;
    bw_pend = 0;
    bw_logged |= p;
    restore_irq(oldlevel);
    for (k = 0; k < BW_N; k++)
        if (p & (1u << k))
        {
            snprintf(line, sizeof(line), "FAULT %s ms=%lu", bw_name[k],
                     (unsigned long)bw_ms[k]);
            if (bw_trace)
                bw_trace(line);
        }
}
#endif
#endif

#define ROM_BASE        0x20000000
#define ROM_ID(a,b,c,d) (unsigned int)(  ((unsigned int)(d))        | \
                                        (((unsigned int)(c)) << 8)  | \
                                        (((unsigned int)(b)) << 16) | \
                                        (((unsigned int)(a)) << 24) )

/* Get address and length of iPod flash section.
   Based on part of FS#6721.  This may belong elsewhere.
   (BCM initialization uploads the vmcs section to the BCM.)
 */
static bool flash_get_section(const unsigned int imageid,
                              void **offset,
                              unsigned int *length)
{
    unsigned long *p = (unsigned long*)(ROM_BASE + 0xffe00);
    unsigned char *csp, *csend;
    unsigned long checksum;

    /* Find the image in the directory */
    while (1)
    {
        if (p[0] != ROM_ID('f','l','s','h'))
            return false;
        if (p[1] == imageid)
            break;
        p += 10;
    }

    *offset = (void *)(ROM_BASE + p[3]);
    *length = p[4];

    /* Verify checksum.  Probably unnecessary, but it's fast. */
    checksum = 0;
    csend = (unsigned char *)(ROM_BASE + p[3] + p[4]);
    for(csp = (unsigned char *)(ROM_BASE + p[3]); csp < csend; csp++)
    {
        checksum += *csp;
    }

    return checksum == p[7];
}
#endif /* HAVE_LCD_SLEEP */

#ifdef HAVE_BCMFW_L1
/* Wait for (reg & mask) to be set (or clear), at most BCM_PORT_US */
static bool bcm_port_wait(volatile unsigned short *reg, unsigned mask, bool set)
{
    unsigned long t0;

    if (bcm_port_dead)
        return false;
    t0 = USEC_TIMER;
    while (((*reg & mask) != 0) != set)
        if (USEC_TIMER - t0 > BCM_PORT_US)
        {
            bcm_port_dead = true;
            if (bcmfw_active)
                bcmfw_hung = true;  /* handled in a thread: one rung down */
            return false;
        }
    return true;
}
#endif

static inline void bcm_write_addr(unsigned address)
{
    BCM_WR_ADDR32 = address;       /* write destination address */

#ifdef HAVE_BCMFW_L1
    bcm_port_wait(&BCM_CONTROL, 0x2, true);
#else
    while (!(BCM_CONTROL & 0x2));  /* wait for it to be write ready */
#endif
}

static inline void bcm_write32(unsigned address, unsigned value)
{

    bcm_write_addr(address);       /* set destination address */

    BCM_DATA32 = value;            /* write value */
}

static inline unsigned bcm_read32(unsigned address)
{
#ifdef HAVE_BCMFW_L1
    /* a dead port reads as busy: nothing is kicked into it */
    if (!bcm_port_wait(&BCM_RD_ADDR, 1, true))
        return 0xFFFF;
    BCM_RD_ADDR32 = address;
    if (!bcm_port_wait(&BCM_CONTROL, 0x10, true))
        return 0xFFFF;
    return BCM_DATA32;
#else
    while (!(BCM_RD_ADDR & 1));

    BCM_RD_ADDR32 = address;       /* write source address */

    while (!(BCM_CONTROL & 0x10)); /* wait for it to be read ready */

    return BCM_DATA32;             /* read value */
#endif
}

#if !defined(BOOTLOADER) && defined(HAVE_BCMFW)
/* Is the BCM still on the last update?  It answers a kick by leaving the
 * command word (stock: LCD_UPDATE; ours also UPDATERECT) and writes
 * 0xFFFF while busy. */
static inline bool bcm_busy(unsigned data)
{
    if (bcmfw_active && (data == BCMCMD_LCD_UPDATERECT ||
                         data == BCMCMD_LCD_POWERON))
        return true;
    return data == BCMCMD_LCD_UPDATE || data == 0xFFFF;
}

/* Kick the next update (tick blocked or in the tick).  On the first
 * update after a wake, to re-kick a stalled update, and with Apple's
 * vmcs: LCD_UPDATE, the whole frame.  With our image: UPDATERECT with
 * the union of what was written since the last kick, so the panel only
 * gets those pixels (2-D DMA). */
static void bcm_kick(bool rekick)
{
    unsigned cmd = BCMCMD_LCD_UPDATE;

    if (bcmfw_active && !rekick && lcd_state.state != LCD_INITIAL &&
        lcd_state.x0 < lcd_state.x1 &&
        (lcd_state.x1 - lcd_state.x0 < LCD_WIDTH ||
         lcd_state.y1 - lcd_state.y0 < LCD_HEIGHT))
    {
        bcm_write32(BCMA_RECT_XY, lcd_state.x0 | (lcd_state.y0 << 16));
        bcm_write32(BCMA_RECT_WH, (lcd_state.x1 - lcd_state.x0) |
                                  ((lcd_state.y1 - lcd_state.y0) << 16));
        cmd = BCMCMD_LCD_UPDATERECT;
    }
    lcd_state.x0 = lcd_state.x1 = 0;    /* sent (or covered): none pending */
    bcm_write32(BCMA_COMMAND, cmd);
    BCM_CONTROL = 0x31;
}

/* Add a rectangle just written to the pending union.  Called with the
 * tick blocked (lcd_block_tick), the only other user of the union. */
static void bcm_post_rect(int x, int y, int w, int h)
{
    if (lcd_state.x0 >= lcd_state.x1)
    {
        lcd_state.x0 = x;
        lcd_state.y0 = y;
        lcd_state.x1 = x + w;
        lcd_state.y1 = y + h;
        return;
    }
    if (x < lcd_state.x0)
        lcd_state.x0 = x;
    if (y < lcd_state.y0)
        lcd_state.y0 = y;
    if (x + w > lcd_state.x1)
        lcd_state.x1 = x + w;
    if (y + h > lcd_state.y1)
        lcd_state.y1 = y + h;
}

/* A wake kicked LCD_POWERON before uploading its first frame: the frame's
 * kick waits for it (one command word), bounded like a first update; an
 * image that never finishes is handled as stopped. */
static void bcm_poweron_wait(void)
{
    long t0 = current_tick;
    unsigned data;

    while ((data = bcm_read32(BCMA_COMMAND)) == BCMCMD_LCD_POWERON ||
           data == 0xFFFF)
    {
        if (TIME_AFTER(current_tick, t0 + BCM_LCDINIT_TIMEOUT))
        {
            bcmfw_hung = true;
#ifdef BCM_WAIT_BOUNDS
            bcm_wait_fault(BW_POWERON, current_tick - t0);
#endif
            break;
        }
        yield();
    }
    bcmfw_poweron = false;
}
#endif /* !BOOTLOADER && HAVE_BCMFW */

#ifdef HAVE_LCD_SLEEP
static void continue_lcd_awake(void)
{
    lcd_state.waking = false;
    semaphore_release(&(lcd_state.initwakeup));
}
#endif

#ifndef BOOTLOADER
static void lcd_tick(void)
{
    /* No core level interrupt mask - already in interrupt context */
#if NUM_CORES > 1
    corelock_lock(&lcd_state.cl);
#endif

    if (!lcd_state.blocked && lcd_state.state >= LCD_NEED_UPDATE)
    {
        unsigned data = bcm_read32(BCMA_COMMAND);
        DEVSTATS_RD_TICK();
#ifdef HAVE_BCMFW
        bool bcm_is_busy = bcm_busy(data);
#else
        bool bcm_is_busy = (data == BCMCMD_LCD_UPDATE || data == 0xFFFF);
#endif

#ifdef HAVE_BCMFW
#ifdef BCM_WAIT_BOUNDS
        /* the freeze audit: one update busy too long is a stopped BCM
         * with every image.  Job mode (the heartbeat's check, which runs
         * only inside a job wait) gets a longer bound, not none, and
         * Apple's vmcs one as ours (stock re-kicks a stall forever: the
         * screen froze, and lcd_sleep() then waited forever for idle) */
        long busy_for = current_tick - bcmfw_kick_tick;
        int bw = !bcmfw_active ? BW_TICK_VMCS
               : BCMJOB_ON() ? BW_TICK_JOBS : BW_TICK;

        if ((bcmfw_active && data == BCMFW_ST_FAULT) || (bcm_is_busy &&
            busy_for > (bw == BW_TICK_JOBS ? BCMFW_JOB_HANG_TIMEOUT
                                           : BCMFW_HANG_TIMEOUT)))
        {
            if (data != BCMFW_ST_FAULT)
                bcm_wait_fault(bw, busy_for);
#else
        if (bcmfw_active && (data == BCMFW_ST_FAULT || (bcm_is_busy &&
            !BCMJOB_ON() &&     /* job mode: the heartbeat's check */
            TIME_AFTER(current_tick, bcmfw_kick_tick + BCMFW_HANG_TIMEOUT))))
        {
#endif
            /* our image stopped: no more kicks; the next update falls
             * back to Apple's vmcs (lcd_update_rect, thread context) */
            bcmfw_hung = true;
            lcd_state.state = LCD_IDLE;
            if (lcd_state.waking)
                continue_lcd_awake();
        }
        else
#endif
        if (((lcd_state.state == LCD_NEED_UPDATE) && !bcm_is_busy)
            /* Update requested and BCM is no longer busy. */
         || (TIME_AFTER(current_tick, lcd_state.update_timeout) && bcm_is_busy))
            /* BCM still busy after timeout, i.e. stalled. */
        {
#ifdef HAVE_BCMFW
            if (!bcm_is_busy)
                bcmfw_kick_tick = current_tick;     /* a new update */
            bcm_kick(bcm_is_busy);  /* UPDATERECT with our image */
#else
            bcm_write32(BCMA_COMMAND, BCMCMD_LCD_UPDATE);  /* Kick off update */
            BCM_CONTROL = 0x31;
#endif
            lcd_state.update_timeout = current_tick + BCM_UPDATE_TIMEOUT;
            lcd_state.state = LCD_UPDATING;
#ifdef HAVE_LCD_SLEEP
            if (lcd_state.waking)
                continue_lcd_awake();
#endif
        }
        else if ((lcd_state.state == LCD_UPDATING) && !bcm_is_busy)
        {
            /* Update finished properly and no new update pending. */
            lcd_state.state = LCD_IDLE;
#ifdef HAVE_LCD_SLEEP
            if (lcd_state.waking)
                continue_lcd_awake();
#endif
        }
    }
#if NUM_CORES > 1
    corelock_unlock(&lcd_state.cl);
#endif
}

static inline void lcd_block_tick(void)
{
    int oldlevel = disable_irq_save();

#if NUM_CORES > 1
    corelock_lock(&lcd_state.cl);
    lcd_state.blocked = true;
    corelock_unlock(&lcd_state.cl);
#else
    lcd_state.blocked = true;
#endif
    restore_irq(oldlevel);
}

static void lcd_unblock_and_update(void)
{
    unsigned data;
    bool bcm_is_busy;
    int oldlevel = disable_irq_save();

#if NUM_CORES > 1
    corelock_lock(&lcd_state.cl);
#endif
    data = bcm_read32(BCMA_COMMAND);
#ifdef HAVE_BCMFW
    bcm_is_busy = bcm_busy(data);
#else
    bcm_is_busy = (data == BCMCMD_LCD_UPDATE || data == 0xFFFF);
#endif

#ifdef HAVE_BCMFW
    if (bcmfw_active && data == BCMFW_ST_FAULT)
    {
        bcmfw_hung = true;      /* keep the fault word; the next update or
                                 * wake falls back */
        lcd_state.state = LCD_IDLE;
    }
    else
#endif
    if (!bcm_is_busy || (lcd_state.state == LCD_INITIAL) ||
        TIME_AFTER(current_tick, lcd_state.update_timeout))
    {
#ifdef HAVE_BCMFW
        if (!bcm_is_busy || lcd_state.state == LCD_INITIAL)
            bcmfw_kick_tick = current_tick;         /* a new update */
        /* UPDATERECT with our image; a stalled one: the whole frame */
        bcm_kick(bcm_is_busy && lcd_state.state != LCD_INITIAL);
#else
        bcm_write32(BCMA_COMMAND, BCMCMD_LCD_UPDATE);  /* Kick off update */
        BCM_CONTROL = 0x31;
#endif
        lcd_state.update_timeout = current_tick + BCM_UPDATE_TIMEOUT;
        lcd_state.state = LCD_UPDATING;
#ifdef HAVE_LCD_SLEEP
        if (lcd_state.waking)
            continue_lcd_awake();
#endif
    }
    else
    {
#if defined(BCM_WAIT_BOUNDS) && defined(HAVE_BCMFW)
        /* busy with no update of ours in flight: the hang check's clock
           starts at this request, not at a kick long ago */
        if (lcd_state.state == LCD_IDLE)
            bcmfw_kick_tick = current_tick;
#endif
         lcd_state.state = LCD_NEED_UPDATE; /* Post update request */
    }
    lcd_state.blocked = false;

#if NUM_CORES > 1
    corelock_unlock(&lcd_state.cl);
#endif
    restore_irq(oldlevel);
}

#else /* BOOTLOADER */

#define lcd_block_tick()

static void lcd_unblock_and_update(void)
{
    unsigned data;

    if (lcd_state.state != LCD_INITIAL)
    {
        data = bcm_read32(BCMA_COMMAND);
        while (data == BCMCMD_LCD_UPDATE || data == 0xFFFF)
        {
            yield();
            data = bcm_read32(BCMA_COMMAND);
        }
    }
    bcm_write32(BCMA_COMMAND, BCMCMD_LCD_UPDATE);  /* Kick off update */
    BCM_CONTROL = 0x31;
    lcd_state.state = LCD_IDLE;
}
#endif /* BOOTLOADER */

/*** hardware configuration ***/

void lcd_set_contrast(int val)
{
  /* TODO: Implement lcd_set_contrast() */
  (void)val;
}

void lcd_set_invert_display(bool yesno)
{
  /* TODO: Implement lcd_set_invert_display() */
  (void)yesno;
}

/* turn the display upside down (call lcd_update() afterwards) */
void lcd_set_flip(bool yesno)
{
  /* TODO: Implement lcd_set_flip() */
  (void)yesno;
}

#if defined(HAVE_LCD_E2) && !defined(BOOTLOADER)
/* The UI's pushes by the PP's second DMA engine (#75/#76): the CPU yields
 * while each piece moves, so a push no longer holds the single core for
 * the ~27 ms of a whole frame (in USB mode, long enough for the host to
 * reset the device).  Because the push now yields, the port is guarded by
 * a (recursive) mutex for every thread-level user: the pushes, sleep and
 * wake here, and the device test's take (bcm-devtest-hal.c); the tick's
 * reads are kept out by lcd_block_tick() as before.  Never from IRQ-off
 * code (a panic screen): then the CPU's copy, unlocked, as before.
 *
 * THE PORT LOCK (P19 S0; jobs.md section 6, "One port lock").  The host
 * port has one auto-incrementing address register, so a "port piece" (any
 * address write plus the data or reads that follow it: a push band, an
 * engine-2 piece, a bcm_command(), a job ring record WITH its doorbell, a
 * video-feed packet, a devtest stage's access) must never interleave with
 * another.  Every port piece, on either core:
 *   1. holds lcd_port_mtx (recursive, per thread; uncached under
 *      HAVE_COP_COHERENCE, so a COP holder excludes a CPU holder), and
 *   2. keeps lcd_tick out: lcd_state.blocked set (lcd_block_tick(), what
 *      lcd_update_rect and bcm_port_lock() do) or lcd_state.cl held.
 *      lcd_tick is a CPU tick ISR that reads BCMA_COMMAND, which moves the
 *      shared address register; the mutex alone cannot keep an ISR out.
 *      lcd_state is IRAM (uncached) and lcd_state.cl a corelock, so this
 *      holds across cores.
 * bcm_port_lock()/bcm_port_unlock() (bcmjob-dev.c) are exactly 1 + 2.
 * The ring write and the doorbell are one piece, in one hold (the BCM's
 * auto-increment after a control write is unproven, and "ring visible
 * before the doorbell" must hold).  In job mode (bcmjob_on) lcd_tick
 * keeps its UPDATERECT kicks but drops the busy-too-long hang check: the
 * job layer's heartbeat check replaces it.  IRQ-off callers (a panic
 * screen) cannot take the mutex and copy unlocked, as before. */
#define LCD_E2_MIN  (LCD_WIDTH * 2 * 16)  /* smaller: the CPU's copy */
static bool lcd_e2_on XCORE_DATA = true;   /* lcd_e2_enable(); off after a failure */
static bool lcd_port_ready XCORE_BSS;      /* the mutex is initialised */
static struct mutex lcd_port_mtx XCORE_BSS;

void lcd_e2_enable(bool on)
{
    lcd_e2_on = on;
}

static bool lcd_port_lock(void)
{
    if (!lcd_port_ready || !irq_enabled())
        return false;
    mutex_lock(&lcd_port_mtx);
    return true;
}

static void lcd_port_unlock(bool locked)
{
    if (locked)
        mutex_unlock(&lcd_port_mtx);
}

/* As much of a contiguous push as the engine moved (whole pieces); the
 * caller copies the rest by CPU.  The tick is blocked and the port lock
 * held.  min: smaller pushes are the CPU's (LCD_E2_MIN for the screen;
 * the job driver's DECODE ring pieces choose their own, bcmjob-dev.c) */
static uint32_t lcd_e2_push_min(unsigned bcmaddr, const fb_data *src,
                                uint32_t bytes, uint32_t min)
{
    uint32_t done = 0;

    if (!lcd_e2_on || bytes < min || !e2port_claim(E2PORT_LCD))
        return 0;
    while (done < bytes)
    {
        uint32_t n = MIN(bytes - done, E2PORT_PIECE_MAX);
        const void *p = (const char *)src + done;
        unsigned long t0;

        if (!e2port_fits(p, n) || e2port_start(bcmaddr + done, p, n) < 0)
        {
            DEVSTATS_LCD_E2_FAIL();
            break;
        }
        t0 = USEC_TIMER;
        /* Unboosted, a big piece outlasts a tick: sleep through one so the
           core can idle (yield() alone returns at once when nothing else
           is runnable, and the loop below would spin at full clock).
           Boosted (animations), pieces are shorter than a tick and the
           frame's latency matters more: yield only */
        if (n >= E2PORT_PIECE_MAX / 2 && cpu_frequency < CPUFREQ_MAX)
            sleep(1);
        while (e2port_busy())
        {
            if (USEC_TIMER - t0 > E2PORT_TIMEOUT_US)
            {
                /* never ended: the CPU from now on, this piece again */
                e2port_stop();
                lcd_e2_on = false;
                DEVSTATS_LCD_E2_FAIL();
                goto out;
            }
            yield();
        }
        DEVSTATS_LCD_E2(n, USEC_TIMER - t0);
        done += n;
        if (!e2port_port_ready())
            break;              /* the CPU's path waits and copes */
    }
out:
    e2port_release(E2PORT_LCD);
    return done;
}
#define lcd_e2_push(a, s, n) lcd_e2_push_min(a, s, n, LCD_E2_MIN)
#else
#define lcd_port_lock()     false
#define lcd_port_unlock(l)  ((void)(l))
#endif

/* LCD init */
void lcd_init_device(void)
{
    /* These port initializations are supposed to be done when initializing
       the BCM.  None of it is changed when shutting down the BCM.
     */
    GPO32_ENABLE |= 0xC000;
    GPIO_CLEAR_BITWISE(GPIOC_ENABLE, 0x80);
    /* This pin is used for BCM interrupts */
    GPIOC_ENABLE |= 0x40;
    GPIOC_OUTPUT_EN &= ~0x40;
    GPO32_ENABLE &= ~1;

    lcd_state.blocked = false;
    lcd_state.state = LCD_INITIAL;
#ifndef BOOTLOADER
#ifdef HAVE_LCD_E2
    mutex_init(&lcd_port_mtx);
    lcd_port_ready = true;
#endif
#if NUM_CORES > 1
    corelock_init(&lcd_state.cl);
#endif
#ifdef HAVE_LCD_SLEEP
    if (!flash_get_section(ROM_ID('v', 'm', 'c', 's'),
                           (void **)(&flash_vmcs_offset), &flash_vmcs_length))
        /* BCM cannot be shut down because firmware wasn't found */
        flash_vmcs_length = 0;
    else
    {
        /* lcd_write_data needs an even number of 16 bit values */
        flash_vmcs_length = ((flash_vmcs_length + 3) >> 1) & ~1;
    }
    semaphore_init(&(lcd_state.initwakeup), 1, 0);
    lcd_state.waking = false;

#ifdef HAVE_BCMFW_L1
    /* logged by the early swap (independence.md, session 9 step 1).  The
     * ROLO case below wakes onto Apple's vmcs: no filesystem yet for the
     * gates; the early swap then takes over from it as from the ROM's. */
    bcm_entry_powered = (GPO32_VAL & 0x4000) != 0;
#endif
    if (GPO32_VAL & 0x4000)
    {
        /* BCM is powered.  Assume it is initialized. */
        lcd_state.display_on = true;
        tick_add_task(&lcd_tick);
    }
    else
    {
        /* BCM is not powered, so it needs to be initialized.
           This can only happen when loading Rockbox via ROLO.
         */
        lcd_state.update_timeout = current_tick;
        lcd_state.display_on = false;
        lcd_awake();
    }
#else /* !HAVE_LCD_SLEEP */
    tick_add_task(&lcd_tick);
#endif
#endif /* !BOOTLOADER */
}

/*** update functions ***/

#ifndef lcd_write_fb
#define lcd_write_fb lcd_write_data     /* stock */
#endif

/* Update a fraction of the display. */
#ifdef HAVE_FE_FB_HANDOFF
/* MF-4: the fence, and our own log line when it timed out (fe_fb_publish
 * logs its own too): the push goes on, reading uncached, so a timeout
 * shows stale pixels at worst */
static void lcd_fb_fence_noted(int x, int y, int w, int h)
{
    if (lcd_fb_fence(x, y, w, h) == FE_FB_TIMEOUT)
    {
#ifdef HAVE_BCMFW_L1
        bcmfw_note("fb fence timeout: pushed uncached (MF-4)");
#endif
    }
}

/* MF-4 (xcore.h): from a core other than the frame's producer (a CPU
 * thread while the shell draws on the COP), the producer's fence first,
 * before the port lock (the producer may be waiting for it; lcd_awake
 * fences before its own hold and passes fence = false), then every read
 * through the uncached alias of the default frame */
static void lcd_update_rect_x(int x, int y, int width, int height,
                              bool fence)
#else
void lcd_update_rect(int x, int y, int width, int height)
#endif
{
    const fb_data *addr;
    unsigned bcmaddr;
    bool locked;
#ifdef HAVE_FE_FB_HANDOFF
    bool cross = lcd_fb_cross();

    /* never with interrupts off (a panic screen): the fence waits on the
     * producer; the read below still writes back this core's own drawing */
    if (cross && fence && irq_enabled())
        lcd_fb_fence_noted(x, y, width, height);
#endif
#if defined(HAVE_BCMFW) && !defined(BOOTLOADER)
    int rect_h;
#endif
#ifdef HAVE_DEVSTATS
    uint32_t ds_t0, ds_bytes;
#endif

    /* before the display_on test: a take or sleep that waited for this
       lock has turned the display off meanwhile */
    locked = lcd_port_lock();
#ifdef HAVE_BCMFW
    if (bcmfw_hung && irq_enabled())    /* never from a panic screen */
        bcmfw_fallback("stopped (fault word, or one update busy 2 s)");
#endif
#ifdef HAVE_LCD_SLEEP
    if (!lcd_state.display_on)
    {
        lcd_port_unlock(locked);
        return;
    }
#endif

    if (x + width >= LCD_WIDTH)
        width = LCD_WIDTH - x;
    if (y + height >= LCD_HEIGHT)
        height = LCD_HEIGHT - y;
#if defined(HAVE_BCM_JOBS) && !defined(BOOTLOADER)
    {
        /* P19 screen truth (19-2): a rect the BCM owns (the drift's
         * panel, full height at the right edge) is not the framebuffer's
         * to push: a full-width band keeps only its left part.  Other
         * shapes push as they are (their callers settle first).  Gated
         * on the owned rect and its power-up stamp, not on job mode (off
         * between frames, when the panel is still the BCM's). */
        int ox, oy, ow, oh;

        if (bcmjob_owned_get(&ox, &oy, &ow, &oh)
            && oy == 0 && oh >= LCD_HEIGHT && ox + ow >= LCD_WIDTH
            && x + width > ox)
            width = x < ox ? ox - x : 0;
    }
#endif

    if ((width <= 0) || (height <= 0))
    {
        lcd_port_unlock(locked);
        return; /* Nothing left to do. */
    }

    /* Ensure x and width are both even. The BCM doesn't like small unaligned
     * writes and would just ignore them. */
    width = (width + (x & 1) + 1) & ~1;
    x &= ~1;

    /* Prevent the tick from triggering BCM updates while we're writing. */
    lcd_block_tick();
#ifdef HAVE_DEVSTATS
    ds_bytes = width * height * 2;
    ds_t0 = USEC_TIMER;
#endif

#if defined(HAVE_BCMFW) && !defined(BOOTLOADER)
    rect_h = height;
#endif
#ifdef HAVE_FE_FB_HANDOFF
    addr = cross ? (const fb_data *)lcd_fb_read(x, y) : FBADDR(x, y);
#else
    addr = FBADDR(x, y);
#endif
    bcmaddr = BCMA_CMDPARAM + (LCD_WIDTH*2) * y + (x << 1);

    if (width == LCD_WIDTH)
    {
#if defined(HAVE_LCD_E2) && !defined(BOOTLOADER)
        /* whole rows: one contiguous run, the engine's when it can */
        uint32_t bytes = width * height * 2, done = 0;

        if (locked)
            done = lcd_e2_push(bcmaddr, addr, bytes);
        if (done < bytes)
        {
            bcm_write_addr(bcmaddr + done);
            lcd_write_fb(addr + done / 2, (bytes - done) / 2);
        }
#else   /* the bootloader's bytes stay main's (scripts/stock_boot.sha256) */
        bcm_write_addr(bcmaddr);
        lcd_write_fb(addr, width * height);
#endif
    }
    else
    {
        do
        {
            bcm_write_addr(bcmaddr);
            bcmaddr += (LCD_WIDTH*2);
            lcd_write_fb(addr, width);
            addr += LCD_WIDTH;
        }
        while (--height > 0);
    }
#if defined(HAVE_BCMFW) && !defined(BOOTLOADER)
    bcm_post_rect(x, y, width, rect_h);
#endif
#ifdef HAVE_DEVSTATS
    /* the push only: a wake's bcm_poweron_wait() never sets the max */
    DEVSTATS_LCD(ds_bytes, USEC_TIMER - ds_t0);
#endif
#if defined(HAVE_BCMFW) && !defined(BOOTLOADER)
    if (bcmfw_poweron)
        bcm_poweron_wait();
#endif
    lcd_unblock_and_update();
    lcd_port_unlock(locked);
}

#ifdef HAVE_FE_FB_HANDOFF
void lcd_update_rect(int x, int y, int width, int height)
{
    lcd_update_rect_x(x, y, width, height, true);
}

static void lcd_update_x(bool fence)
{
#ifdef HAVE_BCM_JOBS
    bcmjob_owned_settle();      /* P19 screen truth: a full-screen push */
#endif
    lcd_update_rect_x(0, 0, LCD_WIDTH, LCD_HEIGHT, fence);
}
#endif

/* Update the display.
   This must be called after all other LCD functions that change the display. */
void lcd_update(void)
{
#ifdef HAVE_FE_FB_HANDOFF
    lcd_update_x(true);
#else
#if defined(HAVE_BCM_JOBS) && !defined(BOOTLOADER)
    bcmjob_owned_settle();      /* P19 screen truth: a full-screen push */
#endif
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
#endif
}

/* Line write helper function for lcd_yuv_blit. Writes two lines of yuv420. */
extern void lcd_write_yuv420_lines(unsigned char const * const src[3],
                                   unsigned bcmaddr,
                                   int width,
                                   int stride);

/* Performance function to blit a YUV bitmap directly to the LCD */
void lcd_blit_yuv(unsigned char * const src[3],
                  int src_x, int src_y, int stride,
                  int x, int y, int width, int height)
{
    unsigned bcmaddr;
    off_t z;
    unsigned char const * yuv_src[3];
    bool locked = lcd_port_lock();
#if defined(HAVE_BCMFW) && !defined(BOOTLOADER)
    int rect_h = height & ~1;
#endif

#ifdef HAVE_LCD_SLEEP
    if (!lcd_state.display_on)
    {
        lcd_port_unlock(locked);
        return;
    }
#endif

#ifdef HAVE_IPOD_UI
    /* The port's write address ignores bits 1:0 [silicon, CR1 and ALGNP
     * PWD2, 2026-10-25]: an odd x would land every line pair one pixel
     * to the left.  Start at the next even column instead (its source
     * column too), so nothing outside the rect is written. */
    if (x & 1)
    {
        x++;
        src_x++;
        width--;
    }
#endif
    /* Sorry, but width and height must be >= 2 or else */
    width &= ~1;
#ifdef HAVE_IPOD_UI
    if (width <= 0 || height < 2)
    {
        lcd_port_unlock(locked);
        return;
    }
#endif

    z = stride * src_y;
    yuv_src[0] = src[0] + z + src_x;
    yuv_src[1] = src[1] + (z >> 2) + (src_x >> 1);
    yuv_src[2] = src[2] + (yuv_src[1] - src[1]);

    /* Prevent the tick from triggering BCM updates while we're writing. */
    lcd_block_tick();

    bcmaddr = BCMA_CMDPARAM + (LCD_WIDTH*2) * y + (x << 1);
    height >>= 1;

    do
    {
        lcd_write_yuv420_lines(yuv_src, bcmaddr, width, stride);
        bcmaddr += (LCD_WIDTH*4);  /* Skip up two lines */
        yuv_src[0] += stride << 1;
        yuv_src[1] += stride >> 1; /* Skip down one chroma line */
        yuv_src[2] += stride >> 1;
    }
    while (--height > 0);

#if defined(HAVE_BCMFW) && !defined(BOOTLOADER)
    bcm_post_rect(x, y, width, rect_h);
#endif
    lcd_unblock_and_update();
    lcd_port_unlock(locked);
}

#ifdef HAVE_LCD_SLEEP
/* Executes a BCM command immediately and waits for it to complete.
   Other BCM commands (eg. LCD updates or lcd_tick) must not interfere.
 */
static void bcm_command(unsigned cmd)
{
    unsigned status;
#if defined(HAVE_BCMFW) || defined(BCM_WAIT_BOUNDS)
    long t0 = current_tick;
#endif

    bcm_write32(BCMA_COMMAND,  cmd);

    BCM_CONTROL = 0x31;

    while (1)
    {
        status = bcm_read32(BCMA_COMMAND);
        if (status != cmd && status != 0xFFFF)
            break;
#if defined(HAVE_BCMFW) || defined(BCM_WAIT_BOUNDS)
        if (TIME_AFTER(current_tick, t0 + HZ))
        {
            /* our image stopped: power cut, Apple's vmcs at the next wake */
#ifdef HAVE_BCMFW
            if (bcmfw_active)
                bcmfw_hung = true;
#endif
#ifdef BCM_WAIT_BOUNDS
            bcm_wait_fault(BW_COMMAND, current_tick - t0);
#endif
            break;
        }
#endif
        yield();
    }
}

static void bcm_powerdown(void)
{
    /* Immediately switch off the backlight to avoid flashing. */
    _backlight_hw_enable(false);
#ifdef HAVE_BCMFW
    if (bcmfw_hung)
    {
        GPO32_VAL &= ~0x4000;   /* no commands to an image that stopped */
        return;
    }
#endif
    
    /* Not sure what this does. */
    bcm_write32(0x10001400, bcm_read32(0x10001400) & ~0xF0);

    /* Blanks the LCD and decreases power consumption
       below what clearing the LCD would achieve.
       Executing an LCD update command wakes it.
     */
    bcm_command(BCMCMD_LCD_SLEEP);

    /* Not sure if this does anything */
    bcm_command(BCM_CMD(0xC));

    /* Further cuts power use, probably by powering down BCM.
       After this point, BCM needs to be bootstrapped
     */
    GPO32_VAL &= ~0x4000;
}

/* Data written to BCM_CONTROL and BCM_ALT_CONTROL */
const unsigned char bcm_bootstrapdata[] =
{
    0xA1, 0x81, 0x91, 0x02, 0x12, 0x22, 0x72, 0x62
};

#ifdef HAVE_BCMFW_L1
/* bcm_init()'s bootstrap stages 1-3 with every wait bounded: the same
 * writes in the same order.  false: a wait expired (the port never
 * answered), or the debug menu's injected wake failure. */
static bool bcm_bootstrap_bounded(void)
{
    int i;
    unsigned long t0;

    bcm_port_dead = false;
    GPO32_VAL |= 0x4000;
    sleep(HZ/20);

    STRAP_OPT_A &= ~0xF00;
    outl(0x1313, 0x70000040);

    if (bcmfw_inject_wake && bcmfw_active)
    {
        bcmfw_inject_wake = false;
        bcm_port_dead = true;
        return false;
    }
    if (!bcm_port_wait(&BCM_ALT_CONTROL, 0x80, false) ||
        !bcm_port_wait(&BCM_ALT_CONTROL, 0x40, true))
        return false;
    for (i = 0; i < 8; i++)
        BCM_CONTROL = bcm_bootstrapdata[i];
    for (i = 3; i < 8; i++)
        BCM_ALT_CONTROL = bcm_bootstrapdata[i];
    if (!bcm_port_wait(&BCM_RD_ADDR, 1, true) ||
        !bcm_port_wait(&BCM_ALT_RD_ADDR, 1, true))
        return false;
    (void)BCM_WR_ADDR;
    (void)BCM_ALT_WR_ADDR;
    if (!bcm_port_wait(&BCM_ALT_CONTROL, 0x80, false) ||
        !bcm_port_wait(&BCM_ALT_CONTROL, 0x40, true))
        return false;

    bcm_write_addr(BCMA_SRAM_BASE);
    lcd_write_data(flash_vmcs_offset, flash_vmcs_length);
    bcm_write32(BCMA_COMMAND, 0);
    bcm_write32(0x10000C00, 0xC0000000);
    t0 = USEC_TIMER;
    while (!(bcm_read32(0x10000C00) & 1))   /* a dead port reads 0xFFFF */
    {
        if (USEC_TIMER - t0 > 100000)
            return false;
        yield();
    }
    if (bcm_port_dead)
        return false;
    bcm_write32(0x10000C00, 0);
    bcm_write32(0x10000400, 0xA5A50002);
    return !bcm_port_dead;
}
#endif

static void bcm_init(void)
{
    int i;

#ifdef HAVE_BCM_JOBS
    bcmjob_gen++;               /* a new power-up: the job driver rediscovers */
#endif

#ifdef HAVE_BCMFW
    /* stopped before the panel slept: this wake loads Apple's vmcs, so the
     * fallback never runs nested inside lcd_awake() */
    if (bcmfw_hung)
        bcmfw_drop("stopped before the panel slept");
#endif

#ifdef HAVE_BCMFW_L1
    (void)i;
    if (!bcm_bootstrap_bounded())
    {
        GPO32_VAL &= ~0x4000;
        if (bcmfw_active)
        {
            /* one rung down (our CPU-push image, then Apple's vmcs) */
            bcmfw_drop("wake: host-port handshake never answered");
            sleep(HZ/10);
            bcm_init();
            return;
        }
        /* Apple's vmcs too: the display stays off until the next wake
         * tries again (MENU+SELECT resets); never a hang */
        bcm_port_dead = true;
        bcmfw_note("Apple's vmcs: host-port handshake never answered");
        return;
    }
#else
    /* Power up BCM */
    GPO32_VAL |= 0x4000;
    sleep(HZ/20);

    /* Bootstrap stage 1 */

    STRAP_OPT_A &= ~0xF00;
    outl(0x1313, 0x70000040);

    /* Interrupt-related code for future use
       GPIOC_INT_LEV |= 0x40;
       GPIOC_INT_EN |= 0x40;
       CPU_HI_INT_EN |= 0x40000;
    */

    /* Bootstrap stage 2 */

    while (BCM_ALT_CONTROL & 0x80);
    while (!(BCM_ALT_CONTROL & 0x40));

    for (i = 0; i < 8; i++)
    {
        BCM_CONTROL = bcm_bootstrapdata[i];
    }

    for (i = 3; i < 8; i++)
    {
        BCM_ALT_CONTROL = bcm_bootstrapdata[i];
    }

    while ((BCM_RD_ADDR & 1) == 0 || (BCM_ALT_RD_ADDR & 1) == 0);

    (void)BCM_WR_ADDR;
    (void)BCM_ALT_WR_ADDR;

    /* Bootstrap stage 3: upload firmware */

    while (BCM_ALT_CONTROL & 0x80);
    while (!(BCM_ALT_CONTROL & 0x40));

    /* Upload firmware to BCM SRAM */
    bcm_write_addr(BCMA_SRAM_BASE);
    lcd_write_data(flash_vmcs_offset, flash_vmcs_length);

    bcm_write32(BCMA_COMMAND,  0);
    bcm_write32(0x10000C00, 0xC0000000);

    while (!(bcm_read32(0x10000C00) & 1));

    bcm_write32(0x10000C00, 0);
    bcm_write32(0x10000400, 0xA5A50002);
#endif /* !HAVE_BCMFW_L1 */

#ifdef BCM_HAL
    /* bounded: an image that never reports ready must not hang us */
    long t0 = current_tick;
    while (bcm_read32(BCMA_COMMAND) == 0 && TIME_BEFORE(current_tick, t0 + HZ))
        yield();
#ifdef HAVE_BCMFW
    if (bcmfw_active && bcm_read32(BCMA_COMMAND) != BCMFW_ST_DONE)
    {
        /* our image never got ready, or faulted: Apple's vmcs, this wake
         * and the rest of this boot */
        GPO32_VAL &= ~0x4000;
        bcmfw_drop("wake: never ready, or the fault word");
        sleep(HZ/10);
        bcm_init();
        return;
    }
#endif
#else
    while (bcm_read32(BCMA_COMMAND) == 0)
        yield();
#endif

    /* sleep(HZ/2) apparently unneeded */
}

#ifdef BCM_HAL
/* The body below runs for the device test's restore while it still owns
 * the BCM, so a backlight_on() meanwhile cannot start a second bcm_init() */
static void lcd_awake_body(void);
void lcd_awake(void)
{
    if (!bcm_devtest_owned)
        lcd_awake_body();
}
static void lcd_awake_body(void)
#else
void lcd_awake(void)
#endif
{
    bool locked;
#ifdef HAVE_PP_GOV_DVS
    /* the band governor: off 100 MHz before the port's first access
     * (band 3 runs only while the BCM2722 sleeps; pp-governor.md 16.2) */
    if (!lcd_state.display_on)
        pgov_port_cap(true);
#endif
#ifdef HAVE_FE_FB_HANDOFF
    /* MF-4: the wake frame is the shell's.  From the CPU while the shell
     * draws on the COP, its fence before the port lock is taken (the ui
     * thread may be waiting for that lock); the push below then reads
     * uncached and does not fence again under the lock */
    if (!lcd_state.display_on && flash_vmcs_length != 0 && irq_enabled())
        lcd_fb_fence_noted(0, 0, LCD_WIDTH, LCD_HEIGHT);
#endif
    locked = lcd_port_lock();

    if (!lcd_state.display_on && flash_vmcs_length != 0)
    {
        /* Ensure BCM has been off for >= 50 ms */
        long sleepwait = lcd_state.update_timeout + HZ/20 - current_tick;
        if (sleepwait > 0 && sleepwait < HZ/20)
            sleep(sleepwait);

        bcm_init();
#ifdef HAVE_BCMFW_L1
        if (bcm_port_dead)
        {
            lcd_port_unlock(locked);
            return;             /* nothing answered: the display stays off */
        }
#endif
#ifdef HAVE_BCMFW
        /* our image: the panel's power-on runs while the frame uploads */
        if (bcmfw_active)
        {
#ifdef HAVE_BCMFW_RESIDENT
            bcmfw_woke();
#endif
            bcm_write32(BCMA_COMMAND, BCMCMD_LCD_POWERON);
            BCM_CONTROL = 0x31;
            bcmfw_poweron = true;
        }
#endif

        /* Start the first LCD update, which also initializes the LCD */
        lcd_state.state = LCD_INITIAL;
        lcd_state.display_on = true;
#ifdef HAVE_BCM_JOBS
        jd_dark(false);
#endif
#ifdef HAVE_FE_FB_HANDOFF
        lcd_update_x(false);    /* fenced above */
#else
        lcd_update();
#endif
        lcd_state.update_timeout = current_tick + BCM_LCDINIT_TIMEOUT;

        /* Wait for end of first LCD update, so LCD isn't white
           when the backlight turns on.
         */
        lcd_state.waking = true;
        tick_add_task(&lcd_tick);
        /* the port lock is held across this wait: the tick (and
           continue_lcd_awake) must never take lcd_port_mtx */
#ifdef BCM_WAIT_BOUNDS
        {
            long t0 = current_tick;

            /* bounded (the freeze audit): the tick ends it within about
               2 s on every path; past BCM_AWAKE_WAIT the tick is not
               running or never saw the update, so the chip is called
               stopped (the next push power-cycles it) and this wake ends
               with the lock released */
            if (semaphore_wait(&(lcd_state.initwakeup), BCM_AWAKE_WAIT)
                == OBJ_WAIT_TIMEDOUT)
            {
                int oldlevel = disable_irq_save();

                lcd_state.waking = false;   /* no late release */
                restore_irq(oldlevel);
                (void)semaphore_wait(&(lcd_state.initwakeup),
                                     TIMEOUT_NOBLOCK);
#ifdef HAVE_BCMFW
                bcmfw_hung = true;
#endif
                bcm_wait_fault(BW_AWAKE, current_tick - t0);
            }
        }
#else
        semaphore_wait(&(lcd_state.initwakeup), TIMEOUT_BLOCK);
#endif

        lcd_port_unlock(locked);
        locked = false;
        send_event(LCD_EVENT_ACTIVATION, NULL);
    }
    lcd_port_unlock(locked);
}

void lcd_sleep(void)
{
    bool locked;

#ifdef BCM_HAL
    if (bcm_devtest_owned)
        return;
#endif
    locked = lcd_port_lock();       /* a push in flight finishes first */
    if (lcd_state.display_on && flash_vmcs_length != 0)
    {
        lcd_state.display_on = false;
#ifdef HAVE_BCM_JOBS
        jd_dark(true);          /* dark means cut (power-contract.md) */
#endif

        /* Wait for BCM to finish work */
#ifdef BCM_WAIT_BOUNDS
        {
            /* bounded (the freeze audit): a BCM that never goes idle
               (Apple's vmcs wedged, or ours dead in job mode) held the
               port lock here forever, and every push after it.  Past
               the bound: called stopped, so the power-down below cuts
               without commands; the next wake loads it afresh */
            long t0 = current_tick;

            while (lcd_state.state != LCD_INITIAL &&
                   lcd_state.state != LCD_IDLE)
            {
                if (TIME_AFTER(current_tick, t0 + BCM_SLEEP_WAIT))
                {
#ifdef HAVE_BCMFW
                    bcmfw_hung = true;
#endif
                    bcm_wait_fault(BW_SLEEP, current_tick - t0);
                    break;
                }
                yield();
            }
        }
#else
        while (lcd_state.state != LCD_INITIAL && lcd_state.state != LCD_IDLE)
            yield();
#endif

        tick_remove_task(&lcd_tick);
        bcm_powerdown();
#ifdef HAVE_PP_GOV_DVS
        pgov_port_cap(false);   /* the port quiet: band 3 may run */
#endif

        /* Remember time to ensure BCM stays off for >= 50 ms */
        lcd_state.update_timeout = current_tick;
    }
    lcd_port_unlock(locked);
}

bool lcd_active(void)
{
    return lcd_state.display_on;
}

#ifdef HAVE_POWER_SLEEP
/* ProkBox Phase 18-1: after a soft sleep of at least a second (by the
 * RTC) the BCM has been off far longer than lcd_awake()'s 50 ms, but the
 * tick stood still, so the guard would still wait up to 50 ms */
void lcd_wake_aged(void)
{
    if (!lcd_state.display_on)
        lcd_state.update_timeout = current_tick - HZ/20;
}
#endif

#ifdef HAVE_LCD_SHUTDOWN
void lcd_shutdown(void)
{
    lcd_sleep();
}
#endif /* HAVE_LCD_SHUTDOWN */

#ifdef BCM_HAL
#include "bcm-devtest-hal.c"    /* the BCM's host-port layer; needs the statics above */
#endif
#ifdef HAVE_BCM_JOBS
#include "bcmjob-dev.c"         /* P19: the job driver; needs the HAL's too */
#endif
#endif /* HAVE_LCD_SLEEP */
