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
 * bcmfw main loop: answers the commands Rockbox's lcd-video.c sends
 * through the command word at 0x1F8 (see README.md for the contract).
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
#include "hostproto.h"
#include "clock.h"
#if BCMFW_VIDEO
#include "video.h"
#endif
#if BCMFW_BENCH
#include "bench/algn.h"
#endif

static uint32_t info;
static uint32_t frames;

#if BCMFW_PROBE
#define PROBE_BLOCK ((volatile uint32_t *)BCMFW_PROBE_RESULTS)

/* The probe v2 results block: every record "not run", then the header,
 * then its address in the mailbox (bringup.md, "Probe v2") */
static void probe_block_init(void)
{
    int i, n = BCMFW_PROBE_HDR_WORDS + BCMFW_PROBE_REC_WORDS * (BCMFW_PROBE_NCHECKS + 1);

    MB(BCMFW_MB_PROBE_RESULTS) = 0;
    MB(BCMFW_MB_PROBE_FAULTED) = 0;
    for (i = 0; i < n; i++)
        PROBE_BLOCK[i] = 0;
    for (i = -1; i < BCMFW_PROBE_NCHECKS; i++)
        *(volatile uint32_t *)BCMFW_PROBE_REC(i) = BCMFW_PROBE_ST_TAG | (i & 0xFF) << 16;
    PROBE_BLOCK[1] = BCMFW_PROBE_NCHECKS | BCMFW_PROBE_REC_WORDS << 8
                   | BCMFW_PROBE_VERSION << 16;
    PROBE_BLOCK[2] = BCMFW_PROBE_REQUIRED;
    PROBE_BLOCK[3] = BCMFW_PROBE_INFO;
    PROBE_BLOCK[0] = BCMFW_PROBE_MAGIC;
    MB(BCMFW_MB_PROBE_RESULTS) = BCMFW_PROBE_RESULTS;
}
#endif

static void publish_info(void)
{
    MB(BCMFW_MB_INFO) = info | (lcd_is_on() ? BCMFW_INFO_PANEL_ON : 0)
#if BCMFW_LCD_DMA
                      | dma_info()
#endif
                      ;
#if BCMFW_LCD_STANDBY
    MB(BCMFW_MB_STANDBY_ST) = lcd_standby_state();
#endif
}

/* Interrupts: all sources off (we poll), except the idle tick */
static void irq_init(void)
{
    int k;

    for (k = 0; k < 4; k++)
        REG(IC_PRIO(k)) = 0;
#if BCMFW_IDLE_SLEEP
    MB(BCMFW_MB_IDLE_TICKS) = 0;
    MB(BCMFW_MB_IDLE_BELLS) = 0;
    MB(BCMFW_MB_IDLE_SLEEPS) = 0;
    MB(BCMFW_MB_IDLE_BAD) = 0;
    REG(TIMER_C(1)) = REG(TIMER_CLO) + BCMFW_IDLE_TICK_US;
    REG(TIMER_CS) = 1 << 1;
#if BCMFW_IDLE_DOORBELL && TIMER_IRQ(1) / 8 == BCMFW_DOORBELL_IRQ / 8
    REG(BCMFW_DOORBELL_ACK) = 1;
    REG(IC_PRIO(TIMER_IRQ(1) / 8)) = 1 << (4 * (TIMER_IRQ(1) % 8))
                                   | 1 << (4 * (BCMFW_DOORBELL_IRQ % 8));
#else
    REG(IC_PRIO(TIMER_IRQ(1) / 8)) = 1 << (4 * (TIMER_IRQ(1) % 8));
#endif
    REG(IC_LEVEL) = 0;
#endif
}

static inline void idle_wait(void)
{
#if BCMFW_IDLE_SLEEP
#if BCMFW_IDLE_GATED
    /* the resident video image: only while the host allows it */
    if (MB(BCMFW_MB_IDLE_EN) != BCMFW_IDLE_EN_MAGIC)
        return;
#endif
    /* The tick handler re-arms itself, so a wake-up is never lost.  r0-r5
     * hold known values across the sleep: an interrupt return that
     * disturbed one is counted (stage 5i reads the count) */
    register uint32_t a0 asm("r0") = BCMFW_IDLE_REG_MAGIC + 0;
    register uint32_t a1 asm("r1") = BCMFW_IDLE_REG_MAGIC + 1;
    register uint32_t a2 asm("r2") = BCMFW_IDLE_REG_MAGIC + 2;
    register uint32_t a3 asm("r3") = BCMFW_IDLE_REG_MAGIC + 3;
    register uint32_t a4 asm("r4") = BCMFW_IDLE_REG_MAGIC + 4;
    register uint32_t a5 asm("r5") = BCMFW_IDLE_REG_MAGIC + 5;

    asm volatile("! ei\n\t! sleep\n\t! di"
                 : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5)
                 : : "memory");
    MB(BCMFW_MB_IDLE_SLEEPS) += 1;
    if ((a0 ^ BCMFW_IDLE_REG_MAGIC) | (a1 ^ (BCMFW_IDLE_REG_MAGIC + 1)) |
        (a2 ^ (BCMFW_IDLE_REG_MAGIC + 2)) | (a3 ^ (BCMFW_IDLE_REG_MAGIC + 3)) |
        (a4 ^ (BCMFW_IDLE_REG_MAGIC + 4)) | (a5 ^ (BCMFW_IDLE_REG_MAGIC + 5)))
        MB(BCMFW_MB_IDLE_BAD) += 1;
#endif
}

static void command(uint32_t cmd)
{
    if (((cmd >> 16) ^ (cmd & 0xFFFF)) != 0xFFFF)
        return;                 /* not a BCM_CMD(): acknowledge only */

    switch (cmd & 0xFFFF)
    {
#if BCMFW_PROBE
    /* The probe never touches the panel: every command is counted and
     * acknowledged, so the host can time a round trip */
    case CMD_LCD_UPDATE:
    case CMD_LCD_UPDATERECT:
        MB(BCMFW_MB_FRAMES) = ++frames;
        break;
#if BCMFW_BENCH
    case CMD_ALGN:
        /* bench row ALGN, once the bench is done and read (bench/algn.h) */
        algn_run();
        break;
#endif
#else
    case CMD_LCD_UPDATE:
#if BCMFW_HOSTPROTO
        if (hp_owns_display())
            break;              /* a video session owns the panel */
#endif
        lcd_update();
        MB(BCMFW_MB_FRAMES) = ++frames;
        break;
    case CMD_LCD_UPDATERECT:
#if BCMFW_HOSTPROTO
        if (hp_owns_display())
            break;
#endif
        lcd_update_rect(MB(BCMFW_MB_RECT_XY), MB(BCMFW_MB_RECT_WH));
        MB(BCMFW_MB_FRAMES) = ++frames;
        break;
    case CMD_LCD_POWERON:
#if BCMFW_HOSTPROTO
        if (hp_owns_display())
            break;
#endif
        if (!lcd_is_on())
            lcd_power_on();
        break;
    case CMD_LCD_SLEEP:
#if BCMFW_HOSTPROTO
        hp_lcd_sleep();
#endif
        lcd_power_off();
#if BCMFW_CLOCK_CMD
        clock_slow();           /* asleep on the crystal */
#endif
        break;
#if BCMFW_CLOCK_CMD
    case CMD_CLOCK:
        /* stage 4c: switch as asked, then the bus timing for the clock */
        if ((MB(BCMFW_MB_CLOCK_REQ) & ~1u) == BCMFW_CLOCK_REQ_MAGIC)
        {
            struct clock_readback rb;

            if (MB(BCMFW_MB_CLOCK_REQ) & 1)
                clock_fast(0, &rb);
            else
                clock_slow();
            lcd_clock_changed();
        }
        break;
#endif
#if BCMFW_LCD_STANDBY
    case CMD_LCD_STANDBY:
        /* the sleep spikes (stage 5s): dark, reset held high */
        if ((MB(BCMFW_MB_STANDBY) & ~0xFFu) == BCMFW_STANDBY_MAGIC)
            lcd_standby(MB(BCMFW_MB_STANDBY) & 0xFF);
        else
            lcd_standby(0);     /* refused: no depth asked */
        break;
#endif
#if BCMFW_R3D
    case CMD_R3D:
        /* the 3D render test: one scene for its time, frames to the
         * panel as it renders them */
        r3d_command();
        break;
#endif
#if BCMFW_DRIFT
    case CMD_DRIFT:
        /* stage 5b's mode B: a pan, the fade's planes, or a fade step */
        if (drift_command())
            MB(BCMFW_MB_FRAMES) = ++frames;
        break;
#endif
    case CMD_TEST_STOP:
        /* the device test's stage 5: the real fault stop (fault word,
         * then nothing), so the host's detection and fallback run as
         * they would on a real fault; needs the armed mailbox word too */
        if (MB(BCMFW_MB_STOP) == BCMFW_STOP_MAGIC)
        {
            MB(BCMFW_MB_STOP) = 0;
            bcmfw_fault();
        }
        break;
#endif
    default:
        /* SELFTEST, the TV commands, 0xC before power-down and anything
         * else: nothing to do, report done so the host never waits */
        break;
    }
    publish_info();
}

#if BCMFW_JOBS && (BCMFW_JOBS_DECODE || BCMFW_JOBS_OVL)
/* (also the merged image's with overlays: DECODE's overlay yields here,
 * video/dec_ovl.c; MERGED IMAGE, next re-verify batch)
 * S3c: the main loop's service from inside a running DECODE (its yield,
 * video/jobs_bcm.c, rate-limited there): the heartbeat, the next frame
 * of a running animation, and a command word the host wrote (the LCD
 * contract's pushes), handled as the loop handles it.  Not hp_poll():
 * the command ring waits for the job, as ring order says, and nothing
 * re-enters the job executor. */
void bcmfw_service(void)
{
    uint32_t cmd = MB(BCMFW_MB_COMMAND);

    hp_heartbeat();
    jobs_tick();
    if (cmd == BCMFW_STATUS_DONE || cmd == BCMFW_STATUS_BUSY)
        return;
    MB(BCMFW_MB_COMMAND) = BCMFW_STATUS_BUSY;
    bcmfw_step(BCMFW_STEP_CMD | (cmd & 0xFFF));
    command(cmd);
    bcmfw_step(BCMFW_STEP_DONE);
    if (MB(BCMFW_MB_COMMAND) == BCMFW_STATUS_BUSY)
        MB(BCMFW_MB_COMMAND) = BCMFW_STATUS_DONE;
}
#endif

void bcmfw_main(void)
{
    int id;

    MB(BCMFW_MB_LIMIT) = 0;
    MB(BCMFW_MB_LIMIT_VAL) = 0;
#if BCMFW_PROBE
    MB(BCMFW_MB_PROBE_STEP) = BCMFW_PROBE_ENTERED;
    probe_block_init();
#else
    bcmfw_step(BCMFW_STEP_START);
#endif
    irq_init();
    info = BCMFW_INFO_MAGIC | (BCMFW_PANEL_MODEL << 4)
         | (BCMFW_IDLE_SLEEP ? BCMFW_INFO_IDLE : 0)
         | (BCMFW_LCD_STANDBY ? BCMFW_INFO_STANDBY : 0);
    /* the counter, starting its clock if it does not run (config.h) */
    info |= timer_init() ? BCMFW_INFO_TIMER_OK : BCMFW_INFO_SPIN;
    if (timer_clock_written())
        info |= BCMFW_INFO_CLK;
    bcmfw_step(BCMFW_STEP_GPIO);
    id = lcd_detect();          /* reads GPIO 50/51 levels only */
    info |= id;
#if BCMFW_HOSTPROTO
    {
        /* a warm swap's start (HP_OP_PARK): the host wrote GO | the last
         * image's flags; a cold start's upload left 0 here */
        uint32_t w = MB(BCMFW_MB_WARM);

        MB(BCMFW_MB_WARM) = 0;
        if ((w & ~0xFFu) == BCMFW_WARM_GO)
        {
            info |= BCMFW_INFO_WARM;
            if (w & BCMFW_WARM_PANEL_ON)
                lcd_warm(w & BCMFW_WARM_TIMING1 ? 1 : 0);
            MB(BCMFW_MB_WARM) = BCMFW_WARM_UP | (w & 0xFF);
        }
    }
#endif
    publish_info();
    MB(BCMFW_MB_FRAMES) = 0;
#if BCMFW_HOSTPROTO
    hp_init();
#endif
#if BCMFW_PROBE
    {
        uint32_t t0 = REG(TIMER_CLO), t1;

        spin(1000);
        t1 = REG(TIMER_CLO);
        MB(BCMFW_MB_PROBE_TICKS) = t1 - t0;
        PROBE_BLOCK[6] = t0;
        PROBE_BLOCK[7] = t1;
    }
    MB(BCMFW_MB_PROBE_ALL) = BCMFW_SELFCHECK_ALL;
    MB(BCMFW_MB_PROBE_PASS) = bcmfw_selfcheck();
    MB(BCMFW_MB_PROBE_STEP) = BCMFW_PROBE_READY;
#else
    bcmfw_step(BCMFW_STEP_IDLE);
#endif

    /* Ready: lcd-video.c's bcm_init() waits for any non-zero value */
    MB(BCMFW_MB_COMMAND) = BCMFW_STATUS_DONE;
#if BCMFW_BENCH
    /* The host polls the step word until BCMFW_BENCH_DONE, then reads
     * the results block; commands are answered after that */
    bench_run(info & BCMFW_INFO_TIMER_OK);
#endif

    for (;;)
    {
        uint32_t cmd = MB(BCMFW_MB_COMMAND);

#if BCMFW_HOSTPROTO
        hp_poll();
#endif
#if BCMFW_JOBS
        jobs_tick();            /* a BCM-timed animation's next frame */
#endif
#if BCMFW_VIDEO
        video_anim();           /* the overlay's slide, by the clock */
#endif
        if (cmd == BCMFW_STATUS_DONE || cmd == BCMFW_STATUS_BUSY)
        {
#if BCMFW_VIDEO
            /* decoder work runs between command passes, one packet at
             * a time, so LCD and protocol commands are never far off.
             * P19 PW4's busy meter is the packets' time (an idle pass
             * reads no counter) */
#if BCMFW_VCLK
            uint32_t t0 = REG(TIMER_CLO);

            if (hp_work())
            {
                clock_gov.busy += REG(TIMER_CLO) - t0;
                continue;
            }
#else
            if (hp_work())
                continue;
#endif
#endif
            idle_wait();
            continue;
        }
        MB(BCMFW_MB_COMMAND) = BCMFW_STATUS_BUSY;
#if !BCMFW_PROBE
        bcmfw_step(BCMFW_STEP_CMD | (cmd & 0xFFF));
#endif
        command(cmd);
#if !BCMFW_PROBE
        bcmfw_step(BCMFW_STEP_DONE);
#endif
        /* Done, unless the host re-kicked meanwhile (it rewrites the
         * command after 50 ms of busy): then that runs next. */
        if (MB(BCMFW_MB_COMMAND) == BCMFW_STATUS_BUSY)
            MB(BCMFW_MB_COMMAND) = BCMFW_STATUS_DONE;
    }
}
