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
 * bcmfw internal interfaces.
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
#ifndef BCMFW_H
#define BCMFW_H

#include "config.h"

/* Host commands, as lcd-video.c encodes them: BCM_CMD(x) = ~x << 16 | x */
#define CMD_LCD_UPDATE      0x0
#define CMD_SELFTEST        0x1
#define CMD_TV_PALBMP       0x2
#define CMD_TV_NTSCBMP      0x3
#define CMD_LCD_UPDATERECT  0x5
#define CMD_LCD_SLEEP       0x8
#define CMD_POWERDOWN       0xC
#define CMD_TV_MVOFF        0xE
/* ours (dev7): stop as on a fault, only with BCMFW_MB_STOP armed */
#define CMD_TEST_STOP       0x7F
/* ours (dev8): the panel's power-on sequence alone, no frame, so the host
 * uploads the first frame while the panel's waits run (the wake path);
 * a later LCD_UPDATE finds the panel on.  Not one of Apple's codes. */
#define CMD_LCD_POWERON     0x40
/* ours (dev9, BCMFW_CLOCK_CMD images only): switch the core clock as
 * BCMFW_MB_CLOCK_REQ asks, then re-apply the LCD bus timing for it */
#define CMD_CLOCK           0x41

/* ours (BCMFW_DRIFT images only): run the op in BCMFW_DRIFT_PARAM */
#define CMD_DRIFT           0x42

/* ours (BCMFW_LCD_STANDBY images only): the panel to the standby depth in
 * BCMFW_MB_STANDBY; the next update, rect or LCD_POWERON wakes it */
#define CMD_LCD_STANDBY     0x43

/* ours (BCMFW_R3D images only): run the 3D render test's scene as
 * BCMFW_R3D_PARAM asks, to the end of its time */
#define CMD_R3D             0x44

/* Panel script: a stream of 16-bit words, top 4 bits the operation.
 * Our own format; the four panels' sequences are in panels.c. */
#define PS_REG(r, v)  (r), (v)      /* index r (< 0x1000), then value v */
#define PS_CMD(c)     (0x1000 | (c))  /* index (command) only */
#define PS_DCS(c, n)  (0x2000 | ((n) << 8) | (c))  /* then n byte words */
#define PS_WAIT(ms)   (0x3000 | (ms))
#define PS_TIMING(k)  (0x4000 | (k))  /* bus timing, second word k */
#define PS_LUT        0x5000        /* DCS colour table (0x2D) */
#define PS_END        0xF000

enum panel_window
{
    WIN_DCS,        /* MIPI DCS: 0x2A/0x2B window, 0x2C RAM write */
    WIN_R2XX,       /* 0x210-0x213 window, 0x200/0x201 address, 0x202 */
    WIN_R4X,        /* 0x44-0x46 window, 0x20/0x21 address, 0x22 */
};

struct panel
{
    const uint16_t *init;   /* shared by power-on and power-off */
    const uint16_t *on;     /* after init: up to display on */
    const uint16_t *off;    /* after init: down to display off */
    uint32_t timing0;       /* LCD_TIMING0, for BCMFW_LCD_BUS_KHZ */
    uint32_t timing1[2];    /* LCD_TIMING1: power-on value, later value */
    uint32_t apple0;        /* the same, Apple's words for 150 MHz */
    uint32_t apple1[2];
    uint32_t sweep[BCMFW_SWEEP_N][3];   /* the same for each sweep clock */
    uint16_t reset_ms;      /* wait after the reset pulse */
    uint8_t window;         /* enum panel_window */
};

/* panels.c */
const struct panel *panel_get(int id, int model);
#if BCMFW_LCD_STANDBY
/* the panel's light-standby script (on: the wake), NULL if it has none */
const uint16_t *panel_light(int id, int on);
#endif

/* timer.c */
int timer_init(void);
int timer_clock_written(void);
void spin(uint32_t n);
void bcmfw_step(uint32_t v);
/* crt0.S: the fault word, fault pc/sp, then stop (never returns) */
void bcmfw_fault(void) __attribute__((noreturn));
void udelay(uint32_t us);
void mdelay(uint32_t ms);

/* lcd.c */
int lcd_detect(void);
int lcd_is_on(void);
/* The warm swap (HP_OP_PARK): the timing word in use; a warm start */
int lcd_timing_word(void);
void lcd_warm(int k);
void lcd_power_on(void);
void lcd_power_off(void);
#if BCMFW_LCD_STANDBY
/* The panel to standby (BCMFW_STANDBY_*); the state for the mailbox */
void lcd_standby(uint32_t depth);
uint32_t lcd_standby_state(void);
#endif
void lcd_update(void);
void lcd_update_rect(uint32_t xy, uint32_t wh);
/* BCMFW_VIDEO and BCMFW_R3D builds: a rectangle from any buffer (pitch in pixels),
 * by DMA in the background; lcd_blit_wait() ends the transfer */
void lcd_blit(const uint16_t *src, uint32_t pitch, uint32_t x, uint32_t y,
              uint32_t w, uint32_t h);
void lcd_blit_wait(void);
/* BCMFW_DRIFT builds (video/drift.c): CMD_DRIFT's op; 1 if it pushed */
int drift_command(void);
/* BCMFW_JOBS builds (video/jobs_bcm.c): the display jobs */
void jobs_init(void);
void jobs_dispatch(uint32_t op, uint32_t tag, const uint32_t *a, uint32_t n,
                   uint32_t *rep, uint32_t *nrep);
void jobs_mode(uint32_t mode);
int jobs_tick(void);
/* BCMFW_JOBS_OVL builds: an overlay is loaded (its ops offered), so its
 * region is in use (the merged image's TONE buffer overlaps it) */
int jobs_ovl_live(void);
/* BCMFW_JOBS_DECODE builds (main.c, S3c): from inside a running DECODE,
 * what the main loop does between jobs: the heartbeat, the LCD command
 * word and the animation's next frame; never the command ring */
void bcmfw_service(void);
/* BCMFW_R3D builds (r3d/r3d_cmd.c): CMD_R3D's scene run */
void r3d_command(void);
/* BCMFW_CLOCK builds: re-apply the bus timing after a clock switch */
void lcd_clock_changed(void);

/* probe.S (BCMFW_PROBE builds): bit n set = required check n passed */
#define BCMFW_SELFCHECK_ALL BCMFW_PROBE_REQUIRED
uint32_t bcmfw_selfcheck(void);

/* bench/bench.c (BCMFW_BENCH builds) */
void bench_run(int timer_ok);

/* push.S */
void bcmfw_push(const uint16_t *src, uint32_t n, volatile uint32_t *port);

#endif /* BCMFW_H */
