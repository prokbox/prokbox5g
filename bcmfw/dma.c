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
 * bcmfw DMA frame push: one control block per frame or rectangle, the
 * LCD interface armed for an automatic pixel run, then a bounded wait.
 * Frames come from internal SRAM (0xE0000); nothing here touches SDRAM.
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
#include "bcmfw.h"
#include "hw.h"
#include "dma.h"

#if BCMFW_LCD_DMA

static struct dma_cb lcd_cb __attribute__((aligned(32)));
static int dma_off;             /* set by a timeout: CPU push from then on */

uint32_t dma_info(void)
{
    return BCMFW_INFO_DMA | (dma_off ? BCMFW_INFO_DMA_OFF : 0);
}

/* Stop the channel and return the LCD interface to command mode */
static void dma_lcd_stop(void)
{
    REG(DMA_CS(DMA_LCD_CHAN)) = 0;
    REG(DMA_INT(DMA_LCD_CHAN)) = DMA_INT_ALL;
    REG(LCD_CTRL) = 1;
}

static uint32_t lcd_t0;         /* counter at the last start, for dma_report */
#if BCMFW_DMA_READBACK
/* The interface's control and run-length registers, for the host: does
 * the run length count down as pixels go out? (Apple reads +0x00 [H];
 * no read of +0x04 is known, so this is ours [L].) */
static uint32_t lcd_run_word(void)
{
    return (REG(LCD_CTRL) & 0xFFF) << 20 | (REG(LCD_RUNLEN) & 0xFFFFF);
}
#define RUN_SAMPLE(a)  (MB(a) = lcd_run_word())
#else
#define RUN_SAMPLE(a)  ((void)0)
#endif

/* What the host can read about the push that just ended (mailbox
 * 0x1A4-0x1BC): the device test logs it, nothing depends on it */
static void dma_report(uint32_t t0, uint32_t polls)
{
    MB(BCMFW_MB_DMA_US) = REG(TIMER_CLO) - t0;
    MB(BCMFW_MB_DMA_CS) = REG(DMA_CS(DMA_LCD_CHAN));
    MB(BCMFW_MB_DMA_INT) = REG(DMA_INT(DMA_LCD_CHAN));
    MB(BCMFW_MB_DMA_CB) = REG(DMA_CBADDR(DMA_LCD_CHAN, DMA_LCD_SUB));
    MB(BCMFW_MB_DMA_POLLS) = polls;
}

int dma_lcd_start(const uint16_t *src, uint32_t w, uint32_t h, uint32_t pitch_px)
{
    struct dma_cb *cb = &lcd_cb;
    uint32_t addr = (uint32_t)src;
    uint32_t n = w * h;
    uint32_t burst = 0;
    int force;

    /* The engine moves 4-byte units: two pixels, word aligned */
    if (dma_off || !n || (addr & 3) || (w & 1) || (pitch_px & 1))
        return 0;
    /* The device test's forced timeout: everything as usual except the
     * control block, so the engine never starts and the wait runs out */
    force = MB(BCMFW_MB_DMA_FORCE) == BCMFW_DMA_FORCE_MAGIC;
    if (force)
        MB(BCMFW_MB_DMA_FORCE) = 0;
    lcd_t0 = REG(TIMER_CLO);

    cb->next = 0;
    cb->src = addr;
    cb->dst = LCD_FIFO;
    cb->pad[0] = cb->pad[1] = 0;
    if (w == pitch_px)
    {
        /* contiguous: one run, 32-byte source bursts when aligned */
        burst = !(addr & 31) && !((2 * n) & 31);
        cb->len = n / 2;
        cb->ti = TI_LCD | TI_SRC_WIDTH(burst ? 5 : 2);
        cb->stride = 0;
    }
    else
    {
        /* a sub-rectangle: rows of w / 2 units, skip the rest of each row */
        cb->len = h << 16 | (w / 2);
        cb->ti = TI_LCD | TI_2D | TI_SRC_WIDTH(2);
        cb->stride = (pitch_px - w) / 2;
    }

    REG(DMA_CS(DMA_LCD_CHAN)) = DMA_CS_LCD_OTHER | DMA_CS_DREQ(DMA_DREQ_LCD)
                              | DMA_CS_ENABLE;
    /* a stale completion must not end this wait: clear the whole word,
     * as Apple's start-up does for every channel */
    REG(DMA_INT(DMA_LCD_CHAN)) = DMA_INT_ALL;
    if (!force)
        REG(DMA_CBADDR(DMA_LCD_CHAN, DMA_LCD_SUB)) = (uint32_t)cb;

    /* arm the interface: it raises its request line for n transfers */
    REG(LCD_CTRL) = LCD_RUN_PREPARE | burst << 8;
    REG(LCD_RUNLEN) = n;
    REG(LCD_RUNFMT) = LCD_RUN_FMT_VALUE;
    REG(LCD_CTRL) = LCD_RUN_GO | burst << 8;
    RUN_SAMPLE(BCMFW_MB_DMA_RUN0);      /* at once; again one poll in */
    return 1;
}

int dma_lcd_wait(void)
{
    uint32_t polls, st;

    /* bounded wait for Apple's LCD completion bit; counted in polls so
     * it holds without the counter */
    for (polls = BCMFW_DMA_TIMEOUT_US / BCMFW_DMA_POLL_US; polls; polls--)
    {
        if (!(polls & 15))
            bcmfw_step(BCMFW_STEP_DMA + (polls >> 4));
        st = REG(DMA_INT(DMA_LCD_CHAN));
        if (polls == BCMFW_DMA_TIMEOUT_US / BCMFW_DMA_POLL_US - 1)
            RUN_SAMPLE(BCMFW_MB_DMA_RUN0);      /* one poll interval in */
        if (st & DMA_INT_LCD)
        {
            RUN_SAMPLE(BCMFW_MB_DMA_RUN1);
            dma_report(lcd_t0, BCMFW_DMA_TIMEOUT_US / BCMFW_DMA_POLL_US - polls);
            REG(DMA_INT(DMA_LCD_CHAN)) = DMA_INT_ALL;
            REG(LCD_CTRL) = 1;
            MB(BCMFW_MB_DMA_DONE)++;
            return 1;
        }
        udelay(BCMFW_DMA_POLL_US);
    }
    RUN_SAMPLE(BCMFW_MB_DMA_RUN1);
    dma_report(lcd_t0, BCMFW_DMA_TIMEOUT_US / BCMFW_DMA_POLL_US);
    MB(BCMFW_MB_LIMIT) = BCMFW_LIMIT_DMA << 24 | BCMFW_DMA_TIMEOUT_US / BCMFW_DMA_POLL_US;
    MB(BCMFW_MB_LIMIT_VAL) = REG(DMA_INT(DMA_LCD_CHAN));
    dma_lcd_stop();
    dma_off = 1;
    MB(BCMFW_MB_DMA_TMO)++;
    return -1;
}

int dma_lcd_push(const uint16_t *src, uint32_t w, uint32_t h, uint32_t pitch_px)
{
    /* 0: not started (CPU push), 1: done, -1: timed out */
    return dma_lcd_start(src, w, h, pitch_px) ? dma_lcd_wait() : 0;
}

#endif /* BCMFW_LCD_DMA */
