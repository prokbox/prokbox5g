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
 * BCM2722 device test: the stages (see bcm-devtest.h).  Plain C over
 * struct bcmdt_hal, so the bench runs this exact file against vc2sim.
 *
 *   0  Apple's vmcs running: read the panel ID and time one LCD_UPDATE.
 *   1  Our probe image: no panel or GPIO-output traffic at all.  Ready,
 *      counter, entry address, instruction self-check, a command round
 *      trip.
 *   2  Our hello image: test pattern, a partial update, sleep and wake.
 *   3  Our hello DMA image: the same pictures by DMA, a forced timeout.
 *   4  Our timing bench (needs stage 1 only; no panel I/O): on-chip
 *      checks and 69 timed kernels, read back and logged.
 *   4b The bench's 150 MHz build (optional; needs a stage 4 pass flag).
 *   4c The 150 MHz clock with the panel: up, frames, down (dev9; needs
 *      stage 1 and a 4b pass flag).
 *   5  The everyday path (dev7; needs stage 3).
 *   5b The drift offload A/B, power-log phases (dev9; needs a stage 5
 *      flag; mode B is a stub until an image carries the BCM job).
 *
 * Rules: a log line is flushed before every step that could hang; every
 * wait has a timeout; any failure (timeout, fault word, port error)
 * cuts the BCM's power and gives it back to Apple's vmcs.
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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "bcm-devtest.h"
#include "bcm-devtest-r3d.h"

#define W 320
#define H 240

/* BCM addresses and the mailbox (bcmfw/config.h, lcd-video.c) */
#define A_COMMAND   0x1F8
#define A_FAULT_PC  0x1E0
#define A_FAULT_SP  0x1E4
#define A_INFO      0x1E8
#define A_FRAMES    0x1EC
#define A_RECT_XY   0x1F0
#define A_RECT_WH   0x1F4
#define A_P_STEP    0x1C0
#define A_P_FROM0   0x1C4
#define A_P_PASS    0x1C8
#define A_P_ALL     0x1CC
#define A_P_TICKS   0x1D0
#define A_BENCH     0x1D4
#define A_P_FAULTED 0x1D8
#define A_P_RESULTS 0x1DC
/* Every image from hello v2 on (bcmfw/config.h) */
#define A_LIMIT     0x180       /* the bounded wait that ran out, 0 none */
#define A_LIMIT_VAL 0x184
#define A_CLK       0x188       /* clock-enable record */
#define A_CLK_4C    0x18C       /* 0x10001C4C/54/58 as the image found them */
#define A_CLK_54    0x190
#define A_CLK_58    0x194
#define CLK_VALID(v) (((v) >> 8 & 0xF) == 0xC)
#define CLK_RAN0    0x01
#define CLK_RAN_A   0x02
#define CLK_RAN_B   0x04
#define CLK_WROTE_A 0x10
#define CLK_WROTE_B 0x20
#define CLK_RATE_BAD 0x40
#define A_DMA_FORCE 0x1A0
#define A_DMA_US    0x1A4
#define A_DMA_DONE  0x1A8
#define A_DMA_TMO   0x1AC
#define A_DMA_CS    0x1B0
#define A_DMA_INT   0x1B4
#define A_DMA_CB    0x1B8
#define A_DMA_POLLS 0x1BC
#define A_TIMING_SEL 0x170      /* host: "APPL" = Apple's 150 MHz bus timing */
#define A_TIMING0   0x174       /* the bus timing words last applied */
#define A_TIMING1   0x178
#define A_TIMING_KHZ 0x17C      /* the clock the image's own words are for */
#define A_SPA       0x150       /* dev5: spin()'s loop at 16 alignments, */
#define A_SPA_SPIN  0x160       /*   ticks a byte each; "SP" << 16 | spin() */
#define SPA_MAGIC   0x53500000
#define TIMING_APPLE_MAGIC 0x4150504C
#define TIMING_SWEEP_MAGIC 0x53575000   /* "SWP" << 8 | k: sweep clock k */
#define SWEEP_N     3           /* bcmfw config.h BCMFW_SWEEP_KHZ_0..2 */
#define A_DMA_RUN0  0x198       /* LCD +0x00 << 20 | +0x04: one poll in, */
#define A_DMA_RUN1  0x19C       /*   and at the end of the push (dev4) */
#define DMA_INT_LCD 0x100       /* Apple's LCD completion bit (dev4) */
#define DMA_FORCE_MAGIC 0x464F5243
#define A_FB        0xE0000
#define A_STOP      0x164       /* dev7: arms CMD_TEST_STOP */
#define STOP_MAGIC  0x53544F50
#define A_REGULATOR 0x10001400
#define A_GPIO_LEV1 0x10002824

#define CMD(x)      ((~(uint32_t)(x) << 16) | (uint32_t)(x))
#define LCD_UPDATE      CMD(0)
#define LCD_UPDATERECT  CMD(5)
#define LCD_SLEEP       CMD(8)
#define POWERDOWN       CMD(0xC)
#define TEST_STOP       CMD(0x7F)  /* ours: stop as on a fault (armed) */
#define LCD_POWERON     CMD(0x40)  /* ours (dev8): panel power-on, no frame */
#define CMD_CLOCK_SW    CMD(0x41)  /* ours (dev9): the clock image's switch */
/* dev9, BCMFW_CLOCK images (bcmfw/config.h) */
#define A_CLOCK_STATE 0x140
#define A_CLOCK_RB0   0x144
#define A_CLOCK_RB1   0x148
#define A_CLOCK_US    0x14C
#define A_CLOCK_REQ   0x168
#define CLOCK_MAGIC   0x434B0000
#define CLOCK_FAST    0x1
#define CLOCK_STALL   0x2
#define CLOCK_REQ_MAGIC 0x434C4B00
#define ST_BUSY     0xFFFF
#define ST_DONE     0x50420001
#define ST_FAULT    0xFA170000
#define PROBE_READY 0x50520003
/* Probe v2's results block (bcmfw/config.h; bringup.md, "Probe v2") */
#define P2_RESULTS  0xCB000
#define P2_MAGIC    0x50524232
#define P2_NCHECKS  13
#define P2_HDR      8           /* header words */
#define P2_RW       12          /* words per record */
#define P2_FORMAT   (P2_NCHECKS | P2_RW << 8 | 2 << 16)
#define P2_REQUIRED 0x1FF       /* checks 0-8: PASS/ALL */
#define P2_INFO     0x1E00      /* checks 9-12: logged, never fail stage 1 */
#define P2_REC(i)   (P2_RESULTS + 4 * P2_HDR + 4 * P2_RW * ((i) + 1))
#define P2_TAG      0xC5000000
#define P2_PASSED   1
#define INFO_MAGIC  0x5042
#define INFO_PANEL_ON 0x100
#define INFO_TIMER_OK 0x200
#define INFO_IDLE   0x400       /* built with BCMFW_IDLE_SLEEP */
#define INFO_DMA    0x800
#define INFO_DMA_OFF 0x1000
/* Idle-sleep images (stage 5i, bcmfw/config.h; they share 0x140-0x14C
 * with the clock record, so no image has both) */
#define A_IDLE_TICKS  0x140     /* timer compare 1 handler runs */
#define A_IDLE_BELLS  0x144     /* doorbell (source 23) handler runs */
#define A_IDLE_SLEEPS 0x148     /* sleeps the idle loop returned from */
#define A_IDLE_BAD    0x14C     /* sleeps after which r0-r5 differed */
#define SELFTEST      CMD(1)    /* answered "done" with no work: a no-op */
/* Stage 5s's image (bcmfw/config.h: BCMFW_IDLE_GATED, BCMFW_LCD_STANDBY) */
#define A_IDLE_EN     0x16C     /* host: IDLE_EN_MAGIC = sleep allowed */
#define IDLE_EN_MAGIC 0x534C5031
#define A_STANDBY     0x1C8     /* host: STANDBY_MAGIC | depth */
#define A_STANDBY_ST  0x1CC     /* the depth the panel is in, refused bit */
#define A_PON_US      0x1D0     /* the last panel power-on, us; bit 31:
                                   it came out of standby */
#define STANDBY_MAGIC 0x53425900
#define STANDBY_LIGHT 1
#define STANDBY_DEEP  2
#define STANDBY_REFUSED 0x100
#define PON_FROM_SBY  0x80000000u
#define INFO_STANDBY  0x8000
#define LCD_STANDBY   CMD(0x43) /* ours: the panel dark, reset kept high */

/* Timeouts (ms).  Generous against the simulator's figures (power-on
 * 136-451 ms, ready in microseconds); a slower core only takes longer. */
#define T_READY     1000
#define T_POWERON   3000
#define T_FRAME     1000
#define T_SLEEP     2000
#define T_SHORT     500
/* Stage 4 and 4b, sized from the bench's own count (bringup.md, "Device
 * session 2"): about 38 M cycles if every instruction and vector row
 * costs one, 1.4 s on the 27 MHz crystal; the largest kernel (REP 16:
 * 137 per iteration, 3 runs of 4096) is 1.7 M, 62 ms there.  The step
 * word moves at least once per kernel, so a 3 s stall is 48 times that
 * kernel; the whole bench may take 20 s, 14 times its count at 27 MHz
 * (3.5 times if everything costs 4 cycles).  The 150 MHz build adds the
 * switch (11 ms of spin-limited waits) and a second table at 150 MHz. */
#define T_BENCH_STALL   3000
#define T_BENCH_TOTAL   20000
#define T_BENCH_PLL     25000
#define T_BENCH_POLL    10      /* bringup.md: poll the port only this often */
#define BENCH_RESULTS   0xC0000
#define BENCH_MAGIC     0x50424E31
#define BENCH_STEP_PROBE  0x50520000
#define BENCH_STEP_VEC    0x50521000
#define BENCH_STEP_SCALAR 0x50522000
#define BENCH_STEP_TIME   0x50523000
#define BENCH_STEP_PLL    0x50524000
#define BENCH_STEP_TIME2  0x50525000
#define BENCH_STEP_SLOC   0x50526000    /* dev4: store-locating check k */
#define BENCH_DONE        0x50520004
#define BENCH_HDR       32      /* header words (bcmfw/bench/bench.h) */
#define BENCH_EW        8       /* words per timing entry */
#define BENCH_VREC_W    12      /* words per vector check record (bench.h) */
#define BENCH_MAX_V     64      /* sanity bound on the vector check count */
#define BENCH_MAX_K     128     /* sanity bound on the kernel count */
#define BENCH_TAG_ALU1  0x414C5531      /* the clock reference */
#define BRF_TIMER_OK    0x1
#define BRF_PLL_BUILD   0x2
#define BRF_PLL_DONE    0x4
#define BRF_PLL_STALL   0x8

/* How long each picture stays up for a photo (SELECT moves on early) */
#define HOLD_PATTERN 15000
#define HOLD_RECT    10000
#define HOLD_DARK    2000
#define HOLD_WAKE    10000
#define HOLD_FORCED  10000
#define HOLD_SWEEP   8000
#define HOLD_EVERYDAY 5000
/* Stage 5: how many UPDATERECTs the scroll storm sends and how many
 * sleep/wake cycles follow (the simulator test builds smaller ones) */
#ifndef BCMDT_STORM
#define BCMDT_STORM  240
#endif
#ifndef BCMDT_WAKES
#define BCMDT_WAKES  3
#endif
/* dev8: as many wakes again with LCD_POWERON first, the frame uploaded
 * while the panel's power-on waits run */
#ifndef BCMDT_WAKES_OV
#define BCMDT_WAKES_OV BCMDT_WAKES
#endif
/* dev8's upload lab: words read back per full-frame upload */
#define LAB_CHECKS   48
#define BCMDT_FULLS  10
/* The boot path runs our image under lcd-video.c's timeouts: a frame
 * over BCM_UPDATE_TIMEOUT (50 ms) is re-kicked while still busy, and a
 * power-on frame over BCM_LCDINIT_TIMEOUT (750 ms with HAVE_BCMFW) too.
 * A stage that passes slower than this records no image for its pass
 * flag (the boot path stays off). */
#define BOOT_FRAME_US   40000
#define BOOT_FIRST_US   700000

enum { OK = 0, E_PORT = -1, E_TIMEOUT = -2, E_FAULT = -3, E_STATUS = -4 };

/* p19-relmods: the release boot path (HAVE_BCMFW without the device test)
 * logs what a field report needs (status, info, clock state, fault and
 * image words) but not the bring-up lines: the spin-loop probe and the
 * tick count, probe v2's results block, the upload lab.  Their format
 * strings stayed resident even where --gc-sections dropped their code.
 * The device test and the host test (neither define) keep every line. */
#if defined(HAVE_BCMFW) && !defined(HAVE_BCMFW_DEVTEST)
#define BCMDT_BOOT_ONLY
#endif

static const struct bcmdt_hal *h;
static struct bcmdt_result *res;
static const struct bcmdt_image *cur_img;      /* the last one booted */
static uint32_t t_start;
/* One row of pixels for the uploads: static, off the 8 KB UI-thread
 * stack (test build only, 640 B); word aligned, the ARM side writes it
 * with ldm/stm */
static uint16_t row[W] __attribute__((aligned(4)));
/* dev8: time spent drawing the test pattern into row[], so a timed
 * upload can report the bus alone (session 7 timed both together) */
static uint32_t draw_us;

static void say(const char *fmt, ...)
{
    char line[112];
    va_list ap;
    int n = snprintf(line, sizeof(line), "%6lu ",
                     (unsigned long)((h->now_us() - t_start) / 1000));

    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    h->log(line);
}

static int fail(const char *what, int err)
{
    static const char *const names[] = { "ok", "port", "timeout", "fault",
                                         "status" };

    if (res->why[0] == '\0')
        snprintf(res->why, sizeof(res->why), "%s: %s", what, names[-err]);
    say("FAIL %s (%s)", what, names[-err]);
    return err;
}

uint32_t bcmdt_fnv(const uint8_t *p, uint32_t n)
{
    uint32_t v = 2166136261u;

    while (n--)
        v = (v ^ *p++) * 16777619u;
    return v;
}

static int rd(uint32_t a, uint32_t *v)
{
    return h->rd(a, v) < 0 ? E_PORT : OK;
}

/* Wait until the command word leaves cmd/busy.  The fault word ends the
 * wait at once (the image has stopped). */
static int wait_done(uint32_t cmd, uint32_t ms, uint32_t *took)
{
    uint32_t t0 = h->now_us(), st;

    for (;;)
    {
        if (rd(A_COMMAND, &st))
            return E_PORT;
        if (st == ST_FAULT)
            return E_FAULT;
        if (st != cmd && st != ST_BUSY)
            break;
        if (h->now_us() - t0 > ms * 1000)
            return E_TIMEOUT;
        h->yield();
    }
    if (took)
        *took = h->now_us() - t0;
    return OK;
}

static int command(uint32_t cmd, uint32_t ms, uint32_t *took)
{
    if (h->wr(A_COMMAND, cmd) < 0)
        return E_PORT;
    h->doorbell();
    return wait_done(cmd, ms, took);
}

static int p2_block(void);

/* What a step word (0x1C0) names, for the log (readlog.py has the same
 * table): hello v2's 0x5053xxxx codes; the probe's and the bench's
 * 0x5052xxxx are named by their own lines */
static const char *hello_step_name(uint32_t step, char *buf, size_t n)
{
    uint32_t lo = step & 0xFFF;
    static const char *const fixed[] = { "?", "start", "clock 0x4C/0x54",
        "clock 0x58", "counter check", "panel ID pins", "idle (ready)" };

    if (step >> 16 != 0x5053)
        snprintf(buf, n, "%s", step ? "not a hello v2 step" : "none");
    else if ((step & 0xF000) == 0 && lo < 7)
        snprintf(buf, n, "%s", fixed[lo]);
    else if (step == 0x5053000F)
        snprintf(buf, n, "command done");
    else if ((step & 0xF000) == 0x1000)
        snprintf(buf, n, lo < 4 ? "power-on step %lu" : "power-on sync %lu",
                 (unsigned long)(lo < 4 ? lo : lo - 0x10));
    else if ((step & 0xF000) >= 0x2000 && (step & 0xF000) <= 0x4000)
        snprintf(buf, n, "panel %s script record %lu",
                 (step & 0xF000) == 0x2000 ? "init" :
                 (step & 0xF000) == 0x3000 ? "on" : "off", (unsigned long)lo);
    else if ((step & 0xF000) == 0x5000)
        snprintf(buf, n, "CPU push, row %lu", (unsigned long)lo);
    else if ((step & 0xF000) == 0x6000)
        snprintf(buf, n, "DMA wait, %lu polls left", (unsigned long)lo * 16);
    else if ((step & 0xF000) == 0x7000)
        snprintf(buf, n, "command %lu received", (unsigned long)lo);
    else if ((step & 0xF000) == 0x8000 || (step & 0xF000) == 0x9000)
        snprintf(buf, n, "panel light-standby %s record %lu",
                 (step & 0xF000) == 0x8000 ? "off" : "wake", (unsigned long)lo);
    else
        snprintf(buf, n, "unknown");
    return buf;
}

/* hello v2's own words: where it stopped, any bounded wait that ran out
 * and the clock record.  Bounded: 4 reads. */
static void log_image_words(void)
{
    uint32_t step = 0, lim = 0, val = 0, clk = 0;
    char name[40];

    rd(A_P_STEP, &step);
    rd(A_LIMIT, &lim);
    rd(A_LIMIT_VAL, &val);
    rd(A_CLK, &clk);
    say("image words: step %08lx (%s)", (unsigned long)step,
        hello_step_name(step, name, sizeof(name)));
    say("image words: limit %08lx val %08lx clk %08lx", (unsigned long)lim,
        (unsigned long)val, (unsigned long)clk);
}

/* The clock-enable record of an image that just reported ready */
static void log_clock(void)
{
    uint32_t clk = 0, r4c = 0, r54 = 0, r58 = 0, ticks, spa[5];
    int i;
    const char *how;

    if (rd(A_CLK, &clk) || rd(A_CLK_4C, &r4c) || rd(A_CLK_54, &r54) ||
        rd(A_CLK_58, &r58))
        return;
    if (!CLK_VALID(clk))
    {
        say("clock: no record (%08lx): an image before hello v2", (unsigned long)clk);
        return;
    }
    ticks = clk >> 16;
    how = (clk & CLK_RATE_BAD) ? "counts at an implausible rate: spin delays" :
          (clk & CLK_RAN0) ? "counter ran at start" :
          (clk & CLK_RAN_A) ? "counter started by 0x4C/0x54" :
          (clk & CLK_RAN_B) ? "counter started by 0x58" :
          (clk & CLK_WROTE_B) ? "still STOPPED after 0x4C and 0x58" : "STOPPED";
    say("clock %08lx: %s", (unsigned long)clk, how);
#ifndef BCMDT_BOOT_ONLY
    /* timer_init's spin(1000) is inlined as one fused addcmpbne per
     * iteration (not spin()'s 3 instructions): 1000 / ticks M iterations
     * per second, halved when that instruction sits at +0xc of a 16-byte
     * line (sessions 4-6, hardware.md).  Not a clock reading. */
    say("clock: %lu ticks over spin(1000) (%lu.%lu M loop iterations/s); "
        "found 4c %08lx 54 %08lx 58 %08lx",
        (unsigned long)ticks, (unsigned long)(ticks ? 1000 / ticks : 0),
        (unsigned long)(ticks ? 10000 / ticks % 10 : 0),
        (unsigned long)r4c, (unsigned long)r54, (unsigned long)r58);
    /* dev5: spin()'s loop at offsets 0, 2 .. 30 of a 64-byte block
     * (bcmfw spinprobe.S): alignment or a slower core?  Two lines, each
     * eight offsets, a byte of ticks each */
    if (rd(A_SPA_SPIN, &spa[4]) || (spa[4] >> 16) != (SPA_MAGIC >> 16))
        return;
    for (i = 0; i < 4; i++)
        if (rd(A_SPA + 4 * i, &spa[i]))
            return;
    for (i = 0; i < 2; i++)
        say("spin loop ticks at +%d..+%d: %lu %lu %lu %lu %lu %lu %lu %lu; spin() at %lx",
            16 * i, 16 * i + 14,
            (unsigned long)(spa[2 * i] & 255), (unsigned long)(spa[2 * i] >> 8 & 255),
            (unsigned long)(spa[2 * i] >> 16 & 255), (unsigned long)(spa[2 * i] >> 24),
            (unsigned long)(spa[2 * i + 1] & 255),
            (unsigned long)(spa[2 * i + 1] >> 8 & 255),
            (unsigned long)(spa[2 * i + 1] >> 16 & 255),
            (unsigned long)(spa[2 * i + 1] >> 24), (unsigned long)(spa[4] & 0xFFFF));
#else
    (void)ticks; (void)spa; (void)i; (void)r4c; (void)r54; (void)r58;
#endif
}

static void log_fault(void)
{
    uint32_t pc = 0, sp = 0, info = 0, step = 0;

    rd(A_FAULT_PC, &pc);
    rd(A_FAULT_SP, &sp);
    rd(A_INFO, &info);
    rd(A_P_STEP, &step);
    res->fault_pc = pc;
    res->fault_sp = sp;
    say("fault words: pc %08lx sp %08lx info %08lx step %08lx",
        (unsigned long)pc, (unsigned long)sp, (unsigned long)info,
        (unsigned long)step);
    log_image_words();
    /* a probe or bench image: its results block names the vector and
     * the registers (record U for a fault outside the checks) */
    if (cur_img && (cur_img == h->probe || cur_img == h->bench ||
                    cur_img == h->bench_pll))
        p2_block();
}

/* Power the BCM down.  alive: it still answers, so use the stock order
 * (lcd-video.c bcm_powerdown: regulator bits, LCD_SLEEP, 0xC), which
 * powers the panel off cleanly.  Otherwise cut the power directly.
 * Then stay off >= 50 ms, as lcd_awake() requires. */
static void bcm_off(bool alive)
{
    uint32_t v;

    h->light(false);
    if (alive)
    {
        say("power-down: stock order");
        if (rd(A_REGULATOR, &v) == OK)
            h->wr(A_REGULATOR, v & ~0xF0);
        if (command(LCD_SLEEP, T_SLEEP, NULL) != OK ||
            command(POWERDOWN, T_SHORT, NULL) != OK)
            say("power-down: no answer, cutting power");
    }
    else
        say("power-down: cutting power (image not answering)");
    h->power_cut();
    h->sleep_ms(100);
}

static int boot_ready(uint32_t *ready_us);

/* Upload and start an image; wait for any non-zero command word */
static int boot(const struct bcmdt_image *img, uint32_t *ready_us)
{
    if (!img || img->size < 0x200 || img->size > BCMDT_IMG_MAX ||
        bcmdt_fnv(img->data, img->size) != img->fnv)
        return fail("image check", E_STATUS);
    say("boot %s: %lu bytes, fnv %08lx", img->name,
        (unsigned long)img->size, (unsigned long)img->fnv);
    cur_img = img;
    if (h->boot(img->data, img->size) < 0)
        return fail("bootstrap/upload", E_PORT);
    return boot_ready(ready_us);
}

/* After the start write: any non-zero command word, then the info word */
static int boot_ready(uint32_t *ready_us)
{
    uint32_t t0, st = 0;
    int err;

    t0 = h->now_us();
    for (;;)
    {
        if (rd(A_COMMAND, &st))
            return fail("ready", E_PORT);
        if (st != 0)
            break;
        if (h->now_us() - t0 > T_READY * 1000)
        {
            log_fault();
            return fail("ready", E_TIMEOUT);
        }
        h->yield();
    }
    *ready_us = h->now_us() - t0;
    say("status %08lx after %lu us", (unsigned long)st,
        (unsigned long)*ready_us);
    if (st == ST_FAULT)
    {
        log_fault();
        return fail("ready", E_FAULT);
    }
    if (st != ST_DONE)
        return fail("ready value", E_STATUS);
    if ((err = rd(A_INFO, &res->info)))
        return fail("info word", err);
    say("info %08lx: panel %lu, counter %s", (unsigned long)res->info,
        (unsigned long)(res->info & 3),
        (res->info & INFO_TIMER_OK) ? "running" : "STOPPED (spin delays)");
    if ((res->info >> 16) != INFO_MAGIC)
        return fail("info magic", E_STATUS);
    log_clock();
    if ((int)(res->info & 3) != res->panel_id)
        return fail("panel ID differs from Apple's view", E_STATUS);
    return OK;
}

/* ---- Stage 0: Apple's vmcs, read-only plus one ordinary update ------- */
static int stage0(void)
{
    uint32_t reg = 0, st = 0;
    int err;

    say("stage 0: Apple vmcs running, read-only");
    if ((err = rd(A_GPIO_LEV1, &res->gpio_lev)) ||
        (err = rd(A_REGULATOR, &reg)) || (err = rd(A_COMMAND, &st)))
        return fail("stage 0 reads", err);
    /* panel ID = GPIO 51 + 2 * GPIO 50: bits 19 and 18 */
    res->panel_id = ((res->gpio_lev >> 19) & 1) + 2 * ((res->gpio_lev >> 18) & 1);
    say("gpio lev1 %08lx -> panel ID %d; 0x10001400 = %08lx; status %08lx",
        (unsigned long)res->gpio_lev, res->panel_id, (unsigned long)reg,
        (unsigned long)st);
    /* The frame already in BCM SRAM is today's screen: redraw it once */
    if ((err = command(LCD_UPDATE, T_FRAME, &res->apple_us)))
        return fail("Apple LCD_UPDATE", err);
    say("Apple LCD_UPDATE: %lu us", (unsigned long)res->apple_us);
    say("stage 0 PASS");
    return OK;
}

/* ---- Stage 1: probe image, no panel traffic -------------------------- */

#ifndef BCMDT_BOOT_ONLY
/* Log one record of probe v2's block (i = -1: record U) on two lines:
 *   P2C <i> <status> <pc> <sp> <sr> <detail> <detail2>
 *   P2R <i> <r0> <r1> <r2> <r3> <r4> <r5>
 * record U as "P2U <status> ..." and "P2UR <r0> ...".  All hex but i.
 * status = 0xC5 << 24 | index << 16 | vector << 8 | state (0 not run,
 * 1 pass, 2 fail, 3 fault).  Bounded: 12 reads. */
static int p2_record(int i, uint32_t status)
{
    uint32_t w[P2_RW];
    char c[8], r[8];
    int k, err;

    w[0] = status;
    for (k = 1; k < P2_RW; k++)
        if ((err = rd(P2_REC(i) + 4 * k, &w[k])))
            return err;
    if (i < 0)
    {
        strcpy(c, "P2U");
        strcpy(r, "P2UR");
    }
    else
    {
        snprintf(c, sizeof(c), "P2C %d", i);
        snprintf(r, sizeof(r), "P2R %d", i);
    }
    say("%s %08lx %08lx %08lx %08lx %08lx %08lx", c, (unsigned long)w[0],
        (unsigned long)w[2], (unsigned long)w[3], (unsigned long)w[1],
        (unsigned long)w[10], (unsigned long)w[11]);
    say("%s %08lx %08lx %08lx %08lx %08lx %08lx", r, (unsigned long)w[4],
        (unsigned long)w[5], (unsigned long)w[6], (unsigned long)w[7],
        (unsigned long)w[8], (unsigned long)w[9]);
    return OK;
}

/* Read and log probe v2's results block, if the image says it is valid
 * (0x1DC) and its header matches.  Summary line:
 *   P2 req <pass>/<required> info <pass>/<informational> flt <faulted>
 *      clo <before> <after> ticks <n>
 * then the records of every informational check, every required check
 * that did not pass, and record U if it is set.  Fills res->p_*.  Every
 * read is bounded (8 + 14 + 12 per logged record); a missing or foreign
 * block is logged as "P2 none ..." and is not an error. */
static int p2_block(void)
{
    uint32_t hdr[P2_HDR], ptr = 0, st, ticks = 0;
    int i, err;

    res->p_block = 0;
    if ((err = rd(A_P_RESULTS, &ptr)) || (err = rd(A_P_FAULTED, &res->p_faulted)))
        return err;
    hdr[0] = hdr[1] = 0;
    if (ptr == P2_RESULTS)
        for (i = 0; i < P2_HDR; i++)
            if ((err = rd(P2_RESULTS + 4 * i, &hdr[i])))
                return err;
    if (ptr != P2_RESULTS || hdr[0] != P2_MAGIC || hdr[1] != P2_FORMAT ||
        hdr[2] != P2_REQUIRED || hdr[3] != P2_INFO)
    {
        say("P2 none: block %08lx, words %08lx %08lx", (unsigned long)ptr,
            (unsigned long)hdr[0], (unsigned long)hdr[1]);
        return OK;
    }
    res->p_block = ptr;
    res->p_pass = hdr[4];
    rd(A_P_TICKS, &ticks);
    say("P2 req %03lx/%03lx info %04lx/%04lx flt %04lx clo %08lx %08lx ticks %lu",
        (unsigned long)(hdr[4] & hdr[2]), (unsigned long)hdr[2],
        (unsigned long)(hdr[4] & hdr[3]), (unsigned long)hdr[3],
        (unsigned long)hdr[5], (unsigned long)hdr[6], (unsigned long)hdr[7],
        (unsigned long)ticks);
    for (i = -1; i < P2_NCHECKS; i++)
    {
        uint32_t want = P2_TAG | (uint32_t)(i & 0xFF) << 16;

        if ((err = rd(P2_REC(i), &st)))
            return err;
        if (i < 0)
            res->p_u_status = st;
        /* U: set; required: anything but a pass; informational: always */
        if (i < 0 ? st == want :
            !(P2_INFO & (1u << i)) && st == (want | P2_PASSED))
            continue;
        if ((err = p2_record(i, st)))
            return err;
    }
    return OK;
}
#else
/* the boot path never boots the probe or a bench image */
static int p2_block(void)
{
    res->p_block = 0;
    return OK;
}
#endif /* BCMDT_BOOT_ONLY */

static int stage1(bool *alive)
{
    uint32_t n = 0, step = 0;
    int err;

    say("stage 1: probe (no panel I/O)");
    if ((err = boot(h->probe, &res->ready_us)))
        return err;
    *alive = true;
    if ((err = rd(A_P_STEP, &step)) || (err = rd(A_P_FROM0, &res->from0)) ||
        (err = rd(A_P_PASS, &res->selfcheck)) ||
        (err = rd(A_P_ALL, &res->selfcheck_all)) ||
        (err = rd(A_P_TICKS, &res->probe_ticks)))
        return fail("probe mailbox", err);
    say("probe: step %08lx, started at %s, counter +%lu over spin(1000)",
        (unsigned long)step, res->from0 ? "0x0 (reset branch)" : "0x200",
        (unsigned long)res->probe_ticks);
    if ((err = p2_block()))
        return fail("probe results block", err);
    say("probe: self-check %03lx of %03lx, faulted %04lx",
        (unsigned long)res->selfcheck, (unsigned long)res->selfcheck_all,
        (unsigned long)res->p_faulted);
    if (step != PROBE_READY)
        return fail("probe step", E_STATUS);
    /* the required checks only: 9-12 never fail the stage */
    if (res->selfcheck_all != P2_REQUIRED || res->selfcheck != res->selfcheck_all)
        return fail("instruction self-check", E_STATUS);
    if (res->p_block && (res->p_u_status & 0xFF))
        return fail("probe: unexpected fault (record U)", E_STATUS);
    if ((err = command(LCD_UPDATE, T_SHORT, &res->frame_us)) ||
        (err = rd(A_FRAMES, &n)))
    {
        if (err == E_FAULT || err == E_TIMEOUT)
        {
            *alive = false;
            log_fault();
        }
        return fail("probe round trip", err);
    }
    say("probe: command round trip %lu us, count %lu",
        (unsigned long)res->frame_us, (unsigned long)n);
    if (n != 1)
        return fail("probe count", E_STATUS);
    say("stage 1 PASS");
    return OK;
}

/* ---- Stage 2: hello image, test pattern ------------------------------ */
void bcmdt_pattern_row(int y, uint16_t *row)
{
    static const uint16_t bars[8] = { 0xF800, 0x07E0, 0x001F, 0xFFFF,
                                      0x07FF, 0xF81F, 0xFFE0, 0x8410 };
    int x;

    for (x = 0; x < W; x++)
    {
        uint16_t c = 0;

        if (y >= 8 && y < 88 && x >= 8 && x < 312)
            c = bars[(x - 8) / 38];             /* R G B W C M Y grey */
        else if (y >= 100 && y < 160 && x >= 32 && x < 288)
        {
            int k = x - 32;                     /* 0..255 */
            if (y < 120)
                c = (k >> 3) << 11;             /* red ramp, 32 steps */
            else if (y < 140)
                c = (k >> 2) << 5;              /* green ramp, 64 steps */
            else
                c = k >> 3;                     /* blue ramp, 32 steps */
        }
        /* an "F" (no symmetry: shows any flip or rotation) */
        else if ((x >= 8 && x < 20 && y >= 170 && y < 232) ||
                 (x >= 8 && x < 56 && y >= 170 && y < 182) ||
                 (x >= 8 && x < 44 && y >= 196 && y < 208))
            c = 0xFFFF;
        /* corner blocks inside the border: red, green / blue, white */
        if (y >= 2 && y < 6 && x >= 2 && x < 6)
            c = 0xF800;
        if (y >= 2 && y < 6 && x >= W - 6 && x < W - 2)
            c = 0x07E0;
        if (y >= H - 6 && y < H - 2 && x >= 2 && x < 6)
            c = 0x001F;
        if (y >= H - 6 && y < H - 2 && x >= W - 6 && x < W - 2)
            c = 0xFFFF;
        if (x == 0 || y == 0 || x == W - 1 || y == H - 1)
            c = 0xFFE0;                         /* 1-pixel yellow border */
        row[x] = c;
    }
}

void bcmdt_rect_row(int y, uint16_t *row)
{
    int x;

    for (x = 0; x < BCMDT_RECT_W; x++)
        row[x] = (x == 0 || y == 0 || x == BCMDT_RECT_W - 1 ||
                  y == BCMDT_RECT_H - 1) ? 0xFFFF : 0xF81F;
}

/* The sweep's fine pattern (dev6): the worst case for a marginal bus.
 * Top left, a one-pixel checkerboard of white and black (every data bit
 * toggles at every pixel); top right, the same with 0xAAAA and 0x5555
 * (neighbouring bits in opposite directions); bottom left, one-pixel
 * vertical lines; bottom right, one-pixel horizontal lines.  On a sound
 * bus each quarter is an even texture; a bus clocked too fast shows
 * speckles, coloured noise, streaks or a shifted line. */
void bcmdt_fine_row(int y, uint16_t *row)
{
    int x;

    for (x = 0; x < W; x++)
    {
        int odd = (x ^ y) & 1;

        if (y < H / 2)
            row[x] = x < W / 2 ? (odd ? 0xFFFF : 0x0000) : (odd ? 0xAAAA : 0x5555);
        else
            row[x] = x < W / 2 ? ((x & 1) ? 0xFFFF : 0x0000)
                               : ((y & 1) ? 0xFFFF : 0x0000);
    }
}

static int upload_fine(void)
{
    int y;

    for (y = 0; y < H; y++)
    {
        bcmdt_fine_row(y, row);
        if (h->wr_block(A_FB + y * W * 2, row, W) < 0)
            return fail("frame upload", E_PORT);
    }
    return OK;
}

static void draw_row(int y)
{
    uint32_t t0 = h->now_us();

    bcmdt_pattern_row(y, row);
    draw_us += h->now_us() - t0;
}

static int upload_pattern(void)
{
    int y;

    for (y = 0; y < H; y++)
    {
        draw_row(y);
        if (h->wr_block(A_FB + y * W * 2, row, W) < 0)
            return fail("frame upload", E_PORT);
    }
    return OK;
}

static int upload_rect(int x0, int y0)
{
    int y;

    for (y = 0; y < BCMDT_RECT_H; y++)
    {
        bcmdt_rect_row(y, row);
        if (h->wr_block(A_FB + (y0 + y) * W * 2 + x0 * 2, row, BCMDT_RECT_W) < 0)
            return fail("rect upload", E_PORT);
    }
    if (h->wr(A_RECT_XY, x0 | y0 << 16) < 0 ||
        h->wr(A_RECT_WH, BCMDT_RECT_W | BCMDT_RECT_H << 16) < 0)
        return fail("rect parameters", E_PORT);
    return OK;
}

static int stage2(bool *alive)
{
    const struct bcmdt_image *img = res->from0 ? h->hello_rb : h->hello;
    uint32_t n = 0, info = 0;
    int err;

    say("stage 2: hello image, test pattern");
    if ((err = boot(img, &res->ready_us)))
        return err;
    *alive = true;
    if ((err = upload_pattern()))
        return err;
    say("kick LCD_UPDATE 1 (panel power-on + frame)");
    if ((err = command(LCD_UPDATE, T_POWERON, &res->first_us)))
        goto bad;
    h->light(true);
    say("frame 1 done in %lu us: PHOTO 1 (pattern)",
        (unsigned long)res->first_us);
    if (rd(A_LIMIT, &n) == OK && n != 0)
        log_image_words();      /* a bounded wait ran out on the way */
    n = 0;
    h->hold(HOLD_PATTERN);

    say("kick LCD_UPDATE 2 (panel on)");
    if ((err = command(LCD_UPDATE, T_FRAME, &res->frame_us)))
        goto bad;
    say("frame 2 done in %lu us", (unsigned long)res->frame_us);

    if ((err = upload_rect(BCMDT_RECT_X, BCMDT_RECT_Y)))
        return err;
    say("kick LCD_UPDATERECT %d,%d %dx%d", BCMDT_RECT_X, BCMDT_RECT_Y,
        BCMDT_RECT_W, BCMDT_RECT_H);
    if ((err = command(LCD_UPDATERECT, T_FRAME, &res->rect_us)))
        goto bad;
    say("rect done in %lu us: PHOTO 2 (magenta box, bottom right)",
        (unsigned long)res->rect_us);
    h->hold(HOLD_RECT);

    h->light(false);
    say("kick LCD_SLEEP (panel off)");
    if ((err = command(LCD_SLEEP, T_SLEEP, &res->sleep_us)))
        goto bad;
    say("panel off in %lu us", (unsigned long)res->sleep_us);
    h->sleep_ms(HOLD_DARK);

    say("kick LCD_UPDATE 3 (wake: power-on + frame)");
    if ((err = command(LCD_UPDATE, T_POWERON, &res->wake_us)))
        goto bad;
    h->light(true);
    say("wake frame done in %lu us: PHOTO 3 (pattern with box)",
        (unsigned long)res->wake_us);
    h->hold(HOLD_WAKE);

    /* Bus timing before and after (dev4): the same frame with Apple's
     * 150 MHz words, then with the image's own (the picture is unchanged) */
    {
        uint32_t khz = 0, t0 = 0, t1 = 0, a0 = 0, a1 = 0, us_apple = 0, us_ours = 0;

        if ((err = rd(A_TIMING_KHZ, &khz)) || (err = rd(A_TIMING0, &t0)) ||
            (err = rd(A_TIMING1, &t1)))
            return fail("timing mailbox", err);
        if (h->wr(A_TIMING_SEL, TIMING_APPLE_MAGIC) < 0)
            return fail("timing select", E_PORT);
        say("kick LCD_UPDATE 4 (Apple's 150 MHz bus timing)");
        if ((err = command(LCD_UPDATE, T_FRAME, &us_apple)))
            goto bad;
        if ((err = rd(A_TIMING0, &a0)) || (err = rd(A_TIMING1, &a1)))
            return fail("timing mailbox", err);
        if (h->wr(A_TIMING_SEL, 0) < 0)
            return fail("timing select", E_PORT);
        say("kick LCD_UPDATE 5 (our bus timing)");
        if ((err = command(LCD_UPDATE, T_FRAME, &us_ours)))
            goto bad;
        say("bus timing: Apple's %08lx/%08lx frame %lu us", (unsigned long)a0,
            (unsigned long)a1, (unsigned long)us_apple);
        say("bus timing: ours (%lu kHz) %08lx/%08lx frame %lu us", (unsigned long)khz,
            (unsigned long)t0, (unsigned long)t1, (unsigned long)us_ours);
    }

    if ((err = rd(A_FRAMES, &n)) || (err = rd(A_INFO, &info)))
        return fail("final mailbox", err);
    say("frames %lu, info %08lx", (unsigned long)n, (unsigned long)info);
    if (n != 6 || !(info & INFO_PANEL_ON))
        return fail("frame count / panel state", E_STATUS);
    if (res->frame_us < BOOT_FRAME_US && res->first_us < BOOT_FIRST_US)
        res->img2 = img;
    else
        say("too slow for the boot path's timeouts: no pass flag");
    say("stage 2 PASS");
    return OK;

bad:
    if (err == E_FAULT || err == E_TIMEOUT)
    {
        *alive = false;         /* stopped or stuck: no more commands */
        log_fault();
    }
    return fail("stage 2 command", err);
}

/* ---- Stage 3: hello DMA image ---------------------------------------- */

/* The DMA image's report on the push that just ended (0x1A4-0x1BC) */
static int dma_read(const char *what, uint32_t *bcm_us)
{
    uint32_t us = 0;
    int err;

    if ((err = rd(A_DMA_US, &us)) || (err = rd(A_DMA_DONE, &res->dma_done)) ||
        (err = rd(A_DMA_TMO, &res->dma_tmo)) || (err = rd(A_DMA_CS, &res->dma_cs)) ||
        (err = rd(A_DMA_INT, &res->dma_int)) || (err = rd(A_DMA_CB, &res->dma_cb)) ||
        (err = rd(A_DMA_POLLS, &res->dma_polls)) || (err = rd(A_INFO, &res->info)))
        return fail("DMA mailbox", err);
    if (bcm_us)
        *bcm_us = us;
    /* two lines: one would pass say()'s 112 bytes and lose the registers */
    say("%s: BCM %lu us (counter %s), done %lu, timeouts %lu, polls %lu", what,
        (unsigned long)us, (res->info & INFO_TIMER_OK) ? "running" : "STOPPED",
        (unsigned long)res->dma_done, (unsigned long)res->dma_tmo,
        (unsigned long)res->dma_polls);
    say("%s: ch1 cs %08lx int %08lx sub0 %08lx; info %08lx", what,
        (unsigned long)res->dma_cs, (unsigned long)res->dma_int,
        (unsigned long)res->dma_cb, (unsigned long)res->info);
    return OK;
}

/* Did the interface clock the pixels out?  The image read the LCD run
 * length register one poll after arming and at the end.  Device session
 * 5: it counts the transfers up, so a full run ends at n (0x12c00 for a
 * frame; 0xa5 one poll in).  Below n at the end, the run stopped short;
 * n already at the first poll is a short run that finished at once. */
static void dma_run_read(const char *what, uint32_t n)
{
    uint32_t r0 = 0, r1 = 0, s, e;
    const char *v;

    if (rd(A_DMA_RUN0, &r0) || rd(A_DMA_RUN1, &r1))
        return;
    s = r0 & 0xFFFFF;
    e = r1 & 0xFFFFF;
    if (e == n && s < n)
        v = "counted up: all out";
    else if (e == n)
        v = "all out by the first poll";
    else if (e < n)
        v = "STOPPED SHORT";
    else
        v = "past the length: see photo";
    say("%s: run %lu: %05lx 1 poll in, %05lx at end, ctrl %03lx/%03lx: %s",
        what, (unsigned long)n, (unsigned long)(r0 & 0xFFFFF),
        (unsigned long)(r1 & 0xFFFFF), (unsigned long)(r0 >> 20),
        (unsigned long)(r1 >> 20), v);
}

/* The engine must have sent it: done went up by one, no timeout */
static int dma_sent(const char *what, uint32_t done, uint32_t *bcm_us)
{
    int err = dma_read(what, bcm_us);

    if (err)
        return err;
    if (res->dma_tmo != 0 || (res->info & INFO_DMA_OFF))
        return fail("DMA completion bit 8 never set (timed out, CPU re-sent)",
                    E_STATUS);
    if (res->dma_done != done)
        return fail("DMA push count", E_STATUS);
    return OK;
}

static int stage3(bool *alive)
{
    const struct bcmdt_image *img = res->from0 ? h->hello_dma_rb : h->hello_dma;
    uint32_t n = 0, k;
    bool swept_tmo = false;
    int err;

    say("stage 3: hello DMA image, test pattern by DMA");
    if ((err = boot(img, &res->ready_us)))
        return err;
    *alive = true;
    if (!(res->info & INFO_DMA))
        return fail("not a DMA build (info)", E_STATUS);
    if ((err = upload_pattern()))
        return err;
    say("kick LCD_UPDATE 1 (panel power-on + DMA frame)");
    if ((err = command(LCD_UPDATE, T_POWERON, &res->dma_first_us)))
        goto bad;
    h->light(true);
    say("frame 1 done in %lu us", (unsigned long)res->dma_first_us);
    if ((err = dma_sent("DMA frame 1", 1, NULL)))
        return err;
    dma_run_read("DMA frame 1", W * H);
    say("PHOTO 4 (pattern by DMA: no black band at the top)");
    h->hold(HOLD_PATTERN);

    say("kick LCD_UPDATE 2 (panel on)");
    if ((err = command(LCD_UPDATE, T_FRAME, &res->dma_frame_us)))
        goto bad;
    say("frame 2 done in %lu us", (unsigned long)res->dma_frame_us);
    if ((err = dma_sent("DMA frame 2", 2, &res->dma_bcm_us)))
        return err;
    dma_run_read("DMA frame 2", W * H);

    if ((err = upload_rect(BCMDT_RECT3_X, BCMDT_RECT3_Y)))
        return err;
    say("kick LCD_UPDATERECT %d,%d %dx%d (2-D DMA)", BCMDT_RECT3_X,
        BCMDT_RECT3_Y, BCMDT_RECT_W, BCMDT_RECT_H);
    if ((err = command(LCD_UPDATERECT, T_FRAME, &res->dma_rect_us)))
        goto bad;
    say("rect done in %lu us", (unsigned long)res->dma_rect_us);
    if ((err = dma_sent("DMA rect", 3, &res->dma_bcm_rect_us)))
        return err;
    dma_run_read("DMA rect", BCMDT_RECT_W * BCMDT_RECT_H);
    say("PHOTO 5 (magenta box, top left over the colour bars)");
    h->hold(HOLD_RECT);

    /* Bus-timing sweep (dev6): the same frames by DMA with Apple's
     * nanoseconds recomputed for 30, 25 and 20 MHz (config.h
     * BCMFW_SWEEP_KHZ_k), each timed and held for a photo: first the
     * bars with the box (5a-5c), then the fine pattern at the image's
     * own words as the reference (5d) and at each sweep clock (5e-5g);
     * then the image's own words again.  Display only: a frame the panel
     * mis-clocks shows wrong, nothing is written to it but pixels.  A
     * timeout ends the sweep (the image then pushes by CPU) and is
     * logged; it does not fail the stage, whose verdict is on the
     * default words above. */
    for (k = 0; k < 2 * SWEEP_N + 1 && !swept_tmo; k++)
    {
        static const char photo[2 * SWEEP_N + 1] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g' };
        bool fine = k >= SWEEP_N;
        /* the fine reference (k = SWEEP_N) uses the image's own words */
        uint32_t sel = !fine ? TIMING_SWEEP_MAGIC | k :
                       k == SWEEP_N ? 0 : TIMING_SWEEP_MAGIC | (k - SWEEP_N - 1);
        uint32_t us = 0, bcm_us = 0, khz = 0, t0 = 0, t1 = 0;

        if (k == SWEEP_N && (err = upload_fine()))
            return err;
        if (h->wr(A_TIMING_SEL, sel) < 0)
            return fail("timing select", E_PORT);
        say("kick LCD_UPDATE (bus sweep %lu%s)", (unsigned long)k, fine ? ", fine" : "");
        if ((err = command(LCD_UPDATE, T_FRAME, &us)))
            goto bad;
        if ((err = rd(A_TIMING_KHZ, &khz)) || (err = rd(A_TIMING0, &t0)) ||
            (err = rd(A_TIMING1, &t1)))
            return fail("timing mailbox", err);
        if ((err = dma_read("sweep DMA", &bcm_us)))
            return err;
        dma_run_read("sweep DMA", W * H);
        swept_tmo = res->dma_tmo != 0;
        say("bus sweep%s %lu kHz %08lx/%08lx: frame %lu us, BCM %lu us%s",
            fine ? " fine" : "", (unsigned long)khz, (unsigned long)t0,
            (unsigned long)t1, (unsigned long)us, (unsigned long)bcm_us,
            swept_tmo ? ", DMA TIMED OUT" : "");
        if (!fine)
            say("PHOTO 5%c (box over bars at %lu MHz timing: any colour, shift or "
                "doubled line?)", photo[k], (unsigned long)(khz / 1000));
        else if (k == SWEEP_N)
            say("PHOTO 5%c (fine pattern at our own %lu MHz timing: the reference)",
                photo[k], (unsigned long)(khz / 1000));
        else
            say("PHOTO 5%c (fine pattern at %lu MHz timing: speckles, streaks or a "
                "shifted line against 5d?)", photo[k], (unsigned long)(khz / 1000));
        h->hold(HOLD_SWEEP);
    }
    if (h->wr(A_TIMING_SEL, 0) < 0)
        return fail("timing select", E_PORT);
    say("bus sweep: %lu frames%s; our own words from the next frame on",
        (unsigned long)k, swept_tmo ? ", ended by a DMA timeout" : "");
    if (swept_tmo)
    {
        /* DMA is off in this image now: the forced timeout can't run */
        say("forced timeout skipped: the sweep's timeout already ran the CPU "
            "fallback");
        goto end;
    }

    /* Forced timeout: the next push posts no control block, so the wait
     * runs out and the image re-sends by CPU.  The frame has no box: it
     * shows only if the CPU re-send drew it. */
    if ((err = upload_pattern()))
        return err;
    if (h->wr(A_DMA_FORCE, DMA_FORCE_MAGIC) < 0)
        return fail("DMA force word", E_PORT);
    say("kick LCD_UPDATE 3 (forced DMA timeout, CPU re-send)");
    if ((err = command(LCD_UPDATE, T_FRAME, &res->forced_us)))
        goto bad;
    say("forced frame done in %lu us", (unsigned long)res->forced_us);
    if ((err = dma_read("forced timeout", &res->forced_bcm_us)))
        return err;
    if (res->dma_tmo != 1 || res->dma_done != 3 + k ||
        !(res->info & INFO_DMA_OFF))
        return fail("forced timeout not seen", E_STATUS);
    say("PHOTO 6 (pattern, no box, black band at the top: the CPU fallback)");
    h->hold(HOLD_FORCED);

end:
    say("kick LCD_UPDATE 4 (DMA off until the next upload: CPU)");
    if ((err = command(LCD_UPDATE, T_FRAME, NULL)))
        goto bad;
    if ((err = dma_read("after the timeout", NULL)) || (err = rd(A_FRAMES, &n)))
        return err ? err : fail("final mailbox", E_PORT);
    say("frames %lu", (unsigned long)n);
    /* 3 frames and the rect, k sweep frames, the forced one (unless a
     * sweep frame timed out instead), this one */
    if (n != 5 + k - (swept_tmo ? 1 : 0) ||
        res->dma_done != 3 + k - (swept_tmo ? 1 : 0) || res->dma_tmo != 1 ||
        !(res->info & INFO_PANEL_ON))
        return fail("frame count / DMA counts / panel state", E_STATUS);
    if (res->dma_frame_us < BOOT_FRAME_US && res->dma_first_us < BOOT_FIRST_US)
        res->img3 = img;
    else
        say("too slow for the boot path's timeouts: no pass flag");
    say("stage 3 PASS");
    return OK;

bad:
    if (err == E_FAULT || err == E_TIMEOUT)
    {
        *alive = false;
        log_fault();
    }
    return fail("stage 3 command", err);
}

/* ---- Stage 5: the everyday path (dev7) ------------------------------ *
 *
 * The DMA image used the way ProkBox's display driver uses it every day
 * (lcd-video.c with HAVE_BCMFW): partial updates as a list scrolls, full
 * frames, the backlight's sleep and wake, and an image that stops.  The
 * storm sends four kinds of rectangle in turn, each uploaded from the
 * host and then pushed by UPDATERECT: the list area, two rows (a
 * selection move), a progress bar and the clock.  Rows 0-19 outside the
 * clock's box are never redrawn, as a status bar isn't: they stay still
 * while the rest scrolls, by design (owner reports, builds A and probe:
 * "the top left and one row fixed").  Each kind is timed
 * twice: the host's upload over the bus, and kick to done.  Then full
 * frames, then BCMDT_WAKES sleep/wake cycles in the stock order (the
 * image reloaded each time, as lcd_awake() does), and last the image is
 * stopped on purpose (CMD_TEST_STOP: the real fault stop), so the host's
 * detection is timed; the stage's end then runs the fallback to Apple's
 * vmcs and times it.  Everything is logged; the pass flag (stage 5)
 * names the image that passed. */
struct e_kind
{
    const char *name;
    int x, y, w, h;
};

static const struct e_kind e_kinds[4] =
{
    { "list area", 0, 20, 320, 220 },
    { "two rows",  0, 20, 320, 56 },
    { "progress",  30, 200, 260, 8 },
    { "clock",     260, 2, 52, 16 },
};

/* Upload rows y0..y0+h of the pattern, scrolled by off, at x..x+w: *us
 * the bus alone, *dr the drawing (dev8; session 7 logged their sum) */
static int e_upload(const struct e_kind *k, int y0, int off, uint32_t *us,
                    uint32_t *dr)
{
    uint32_t t0 = h->now_us(), d0 = draw_us;
    int y;

    for (y = y0; y < y0 + k->h; y++)
    {
        draw_row((y + off) % H);
        if (h->wr_block(A_FB + y * W * 2 + k->x * 2, row + k->x, k->w) < 0)
            return fail("everyday upload", E_PORT);
    }
    *dr = draw_us - d0;
    *us = h->now_us() - t0 - *dr;
    return OK;
}

/* dev8, the upload lab: the host's framebuffer (whatever the UI drew last)
 * to the BCM's, a whole frame by each method, unboosted then boosted.
 * Nothing is drawn while timed, so this is the bus (and the PP's loop)
 * alone.  LAB_CHECKS words spread over the frame are read back and
 * compared; a method whose words differ is logged BAD (its time is still
 * logged) and the stage goes on: only a dead port fails it.  Then the
 * cost of one row's address setup: 240 two-pixel rows, one address each.
 * The BCM framebuffer is overwritten (not pushed); the wakes upload the
 * pattern again.  Before each method the checked words are filled with a
 * pattern no method writes (the complement of the frame's, salted by the
 * method and pass), so a method that moves nothing reads back BAD
 * (docs/data-paths.md D1: session 8's DMA "readback ok" was the CPU's
 * earlier write, read back). */
#ifdef BCMDT_BOOT_ONLY
static int e_lab(void)  /* stage 5 is the device test's */
{
    return OK;
}
#else
static const char *const lab_names[BCMDT_UP_N] = { "stock ldm/stm 2",
                                                   "burst ldm/stm 8",
                                                   "PP DMA ch3" };

static int e_lab(void)
{
    const uint16_t *fb = h->fb();
    uint32_t t0, us, v, want;
    int b, m, i, bad, y;

    say("upload lab: the host framebuffer to the BCM's, nothing drawn while "
        "timed");
    for (b = 0; b < 2; b++)
    {
        h->boost(b != 0);
        for (m = 0; m < BCMDT_UP_N; m++)
        {
            for (i = 0; i < LAB_CHECKS; i++)
            {
                uint32_t off = (uint32_t)i * (W * H / LAB_CHECKS) * 2 + 4 * (i & 7);

                want = fb[off / 2] | (uint32_t)fb[off / 2 + 1] << 16;
                if (h->wr(A_FB + off, ~want ^ (0x5A000000u | b << 8 | m)) < 0)
                {
                    h->boost(false);
                    return fail("upload lab fill", E_PORT);
                }
            }
            t0 = h->now_us();
            i = h->fb_upload(A_FB, 0, W * H * 2, m);
            us = h->now_us() - t0;
            if (i < 0)
            {
                h->boost(false);
                return fail("upload lab", E_PORT);
            }
            if (i > 0)
            {
                /* the method never finished (the PP DMA's time limit):
                 * logged, not a stage failure */
                res->e_lab_bad |= 1u << (3 * b + m);
                say("lab %s %-15s: did not finish (%lu us)",
                    b ? "boosted  " : "unboosted", lab_names[m],
                    (unsigned long)us);
                continue;
            }
            bad = 0;
            for (i = 0; i < LAB_CHECKS; i++)
            {
                uint32_t off = (uint32_t)i * (W * H / LAB_CHECKS) * 2 + 4 * (i & 7);

                if (rd(A_FB + off, &v))
                {
                    h->boost(false);
                    return fail("upload lab readback", E_PORT);
                }
                want = fb[off / 2] | (uint32_t)fb[off / 2 + 1] << 16;
                bad += v != want;
            }
            res->e_lab_us[b][m] = us;
            if (bad)
                res->e_lab_bad |= 1u << (3 * b + m);
            say("lab %s %-15s: full frame %lu us, %lu ns/px, readback %s "
                "(%d/%d bad)", b ? "boosted  " : "unboosted",
                lab_names[m], (unsigned long)us,
                (unsigned long)(us * 1000 / (W * H)), bad ? "BAD" : "ok", bad,
                LAB_CHECKS);
        }
    }
    h->boost(false);
    t0 = h->now_us();
    for (y = 0; y < H; y++)
        if (h->fb_upload(A_FB + y * W * 2 + 2 * 268, (y * W + 268) * 2, 4,
                         BCMDT_UP_STOCK) < 0)
            return fail("upload lab rows", E_PORT);
    us = h->now_us() - t0;
    res->e_row_us = us * 1000 / H;
    say("lab unboosted rows: %d two-pixel rows %lu us, %lu ns a row (address "
        "setup and wait)", H, (unsigned long)us, (unsigned long)res->e_row_us);
    return OK;
}
#endif /* BCMDT_BOOT_ONLY */

static int stage5(bool *alive)
{
    const struct bcmdt_image *img = res->from0 ? h->hello_dma_rb : h->hello_dma;
    uint32_t n_up[4] = { 0 }, t_up[4] = { 0 }, t_k[4] = { 0 }, m_k[4] = { 0 };
    uint32_t t_dr[4] = { 0 }, dr, d0;
    uint32_t frames = 0, f0 = 0, us, up, max_full = 0, max_wake = 0, t0, st;
    bool ok = true;
    int i, err;

    say("stage 5: the everyday path (DMA image as ProkBox uses it)");
    if ((err = boot(img, &res->ready_us)))
        return err;
    *alive = true;
    if (!(res->info & INFO_DMA))
        return fail("not a DMA build (info)", E_STATUS);
    t0 = h->now_us();
    d0 = draw_us;
    if ((err = upload_pattern()))
        return err;
    res->e_draw_us = draw_us - d0;
    res->e_host_us = h->now_us() - t0 - res->e_draw_us;
    say("host upload, whole frame: %lu us on the bus, %lu us drawing the "
        "pattern", (unsigned long)res->e_host_us, (unsigned long)res->e_draw_us);
    if ((err = command(LCD_UPDATE, T_POWERON, &us)))
        goto bad;
    h->light(true);
    if ((err = rd(A_FRAMES, &f0)))
        return fail("frame count", err);

    say("scroll storm: %d UPDATERECTs (the pattern moves up)", BCMDT_STORM);
    for (i = 0; i < BCMDT_STORM; i++)
    {
        int kind = i & 3;
        const struct e_kind *k = &e_kinds[kind];
        int y0 = kind == 1 ? k->y + 28 * ((i >> 2) % 7) : k->y;

        if ((err = e_upload(k, y0, 2 * i, &up, &dr)))
            return err;
        if (h->wr(A_RECT_XY, k->x | y0 << 16) < 0 ||
            h->wr(A_RECT_WH, k->w | k->h << 16) < 0)
            return fail("rect parameters", E_PORT);
        if ((err = command(LCD_UPDATERECT, T_FRAME, &us)))
            goto bad;
        n_up[kind]++;
        t_up[kind] += up;
        t_dr[kind] += dr;
        t_k[kind] += us;
        if (us > m_k[kind])
            m_k[kind] = us;
    }
    for (i = 0; i < 4; i++)
        if (n_up[i])
            say("storm %-9s %3dx%-3d: %lu, upload %lu us (drawing %lu us), "
                "push %lu us avg, %lu max", e_kinds[i].name, e_kinds[i].w,
                e_kinds[i].h, (unsigned long)n_up[i],
                (unsigned long)(t_up[i] / n_up[i]),
                (unsigned long)(t_dr[i] / n_up[i]),
                (unsigned long)(t_k[i] / n_up[i]), (unsigned long)m_k[i]);
    say("storm done: the last frame stays up 2 s");
    h->hold(HOLD_DARK);
    if ((err = dma_read("storm", NULL)) || (err = rd(A_FRAMES, &frames)))
        return err ? err : fail("frame count", E_PORT);
    res->e_rects = frames - f0;
    res->e_rect_max_us = m_k[0];
    if (res->e_rects != BCMDT_STORM || res->dma_tmo != 0)
    {
        say("storm: %lu of %d rects counted, %lu DMA timeouts",
            (unsigned long)res->e_rects, BCMDT_STORM,
            (unsigned long)res->dma_tmo);
        ok = false;
    }

    for (i = 0; i < BCMDT_FULLS; i++)
    {
        t0 = h->now_us();
        d0 = draw_us;
        if ((err = upload_pattern()))
            return err;
        dr = draw_us - d0;
        up = h->now_us() - t0 - dr;
        if ((err = command(LCD_UPDATE, T_FRAME, &us)))
            goto bad;
        if (us > max_full)
            max_full = us;
        if (i == 0)
            say("full frame: upload %lu us (drawing %lu us), push %lu us",
                (unsigned long)up, (unsigned long)dr, (unsigned long)us);
    }
    res->e_full_us = max_full;
    say("full frames: %d, slowest push %lu us", BCMDT_FULLS,
        (unsigned long)max_full);
    if (max_full >= BOOT_FRAME_US)
        ok = false;
    if (h->fb_upload && h->fb && h->boost && (err = e_lab()))
        return err;
    say("now %d sleep/wake cycles: dark, then the pattern again", BCMDT_WAKES);
    h->hold(HOLD_EVERYDAY);

    for (i = 0; i < BCMDT_WAKES; i++)
    {
        uint32_t ready;

        t0 = h->now_us();
        bcm_off(true);          /* lcd_sleep(): stock order, power cut */
        *alive = false;
        say("sleep %d: %lu us", i + 1, (unsigned long)(h->now_us() - t0));
        t0 = h->now_us();
        if ((err = boot(img, &ready)))
            return err;
        *alive = true;
        if ((err = upload_pattern()))
            return err;
        if ((err = command(LCD_UPDATE, T_POWERON, &us)))
            goto bad;
        h->light(true);
        us = h->now_us() - t0;
        say("wake %d: image ready %lu us, first frame shown %lu us after the "
            "upload began", i + 1, (unsigned long)ready, (unsigned long)us);
        if (us > max_wake)
            max_wake = us;
        h->hold(HOLD_DARK);
    }
    res->e_wakes = BCMDT_WAKES;
    res->e_wake_max_us = max_wake;
    if (max_wake >= BOOT_FIRST_US + 300000)
        ok = false;

    /* dev8: the same wakes with the panel's power-on kicked first
     * (LCD_POWERON) and the frame uploaded while its waits run, then the
     * frame pushed: what lcd_awake() can do with our image */
    max_wake = 0;
    for (i = 0; i < BCMDT_WAKES_OV; i++)
    {
        uint32_t ready, ov, pw;

        t0 = h->now_us();
        bcm_off(true);
        *alive = false;
        say("sleep %d: %lu us", BCMDT_WAKES + i + 1,
            (unsigned long)(h->now_us() - t0));
        t0 = h->now_us();
        if ((err = boot(img, &ready)))
            return err;
        *alive = true;
        if (h->wr(A_COMMAND, LCD_POWERON) < 0)
            return fail("power-on kick", E_PORT);
        h->doorbell();
        ov = h->now_us();
        if ((err = upload_pattern()))
            return err;
        ov = h->now_us() - ov;
        if ((err = wait_done(LCD_POWERON, T_POWERON, &pw)) ||
            (err = command(LCD_UPDATE, T_FRAME, &us)))
            goto bad;
        h->light(true);
        us = h->now_us() - t0;
        say("wake %d, power-on first: ready %lu us, upload %lu us inside it, "
            "frame shown at %lu us",
            BCMDT_WAKES + i + 1, (unsigned long)ready, (unsigned long)ov,
            (unsigned long)us);
        if (us > max_wake)
            max_wake = us;
        h->hold(HOLD_DARK);
    }
    res->e_ov_wake_max_us = max_wake;
    if (max_wake >= BOOT_FIRST_US + 300000)
        ok = false;

    /* the image stops on purpose: the host must see the fault word */
    if (h->wr(A_STOP, STOP_MAGIC) < 0)
        return fail("stop word", E_PORT);
    say("kick TEST_STOP (the image stops as on a fault)");
    t0 = h->now_us();
    err = command(TEST_STOP, T_SHORT, NULL);
    res->e_stop_us = h->now_us() - t0;
    if (err != E_FAULT)
    {
        rd(A_COMMAND, &st);
        say("stop: no fault word (%d, command word %08lx)", err,
            (unsigned long)st);
        *alive = err == OK;
        return fail("everyday stop", err == OK ? E_STATUS : err);
    }
    *alive = false;
    say("stop: fault word seen after %lu us; falling back to Apple's vmcs",
        (unsigned long)res->e_stop_us);
    say("the normal screen comes back by itself (Apple's vmcs)");
    if (!ok)
        return fail("everyday path: counts or times out of bounds", E_STATUS);
    res->img5 = img;
    say("stage 5 PASS");
    return OK;

bad:
    if (err == E_FAULT || err == E_TIMEOUT)
    {
        *alive = false;
        log_fault();
    }
    return fail("stage 5 command", err);
}

/* ---- Stage 4c: the 150 MHz clock with the panel (dev9) --------------- *
 *
 * Stage 4b proved the switch up with no panel traffic.  Before a video
 * session may run at 150 MHz (pipeline.md, "The 150 MHz clock"), two
 * things need the device: whether the LCD bus follows the core off the
 * crystal (the frame time with Apple's 150 MHz words tells: about 45 ms
 * if the bus stays at 27 MHz, about 15 ms if it runs at half the core,
 * about 8 ms at the core's own clock), and whether our switch back down
 * (clock.c, the reverse of Apple's, [L]) leaves the chip as it was.  The
 * clock-command image (hello_dma plus CMD_CLOCK): a DMA frame on the
 * crystal, up, a frame and a rectangle on the PLL, down, a frame again
 * (it must take what the first did), then up once more and LCD_SLEEP,
 * which must come down by itself.  Needs stage 1 in this run (where the
 * core starts) and a stage 4b pass flag; its first frame on the crystal
 * is its own check of the DMA path. */
static int clock_req(bool up, uint32_t *took)
{
    uint32_t st = 0, rb0 = 0, rb1 = 0, sw = 0;
    int err;

    if (h->wr(A_CLOCK_REQ, CLOCK_REQ_MAGIC | (up ? 1 : 0)) < 0)
        return fail("clock request word", E_PORT);
    say("kick CLOCK %s", up ? "up (PLL A, 150 MHz)" : "down (crystal, 27 MHz)");
    if ((err = command(CMD_CLOCK_SW, T_SHORT, took)))
        return err;
    if ((err = rd(A_CLOCK_STATE, &st)) || (err = rd(A_CLOCK_RB0, &rb0)) ||
        (err = rd(A_CLOCK_RB1, &rb1)) || (err = rd(A_CLOCK_US, &sw)))
        return fail("clock mailbox", err);
    say("clock %s: state %08lx, regulator %02lx -> %02lx, select %04lx, "
        "PLL A %04lx/%04lx, switch %lu us", up ? "up" : "down",
        (unsigned long)st, (unsigned long)(rb0 >> 24),
        (unsigned long)(rb0 >> 16 & 0xFF), (unsigned long)(rb0 & 0xFFFF),
        (unsigned long)(rb1 >> 16), (unsigned long)(rb1 & 0xFFFF),
        (unsigned long)(up ? sw & 0xFFFF : sw >> 16));
    if (up)
        res->c_up_us = sw & 0xFFFF;
    else
        res->c_down_us = sw >> 16;
    if ((st & 0xFFFF0000) != CLOCK_MAGIC || (st & CLOCK_STALL) ||
        !(st & CLOCK_FAST) != !up)
        return fail(up ? "clock up: state" : "clock down: state", E_STATUS);
    return OK;
}

static int c_frame(const char *what, uint32_t done, uint32_t *bcm_us)
{
    uint32_t us = 0, khz = 0, t1 = 0;
    int err;

    say("kick LCD_UPDATE (%s)", what);
    if ((err = command(LCD_UPDATE, T_FRAME, &us)))
        return err;
    if ((err = rd(A_TIMING_KHZ, &khz)) || (err = rd(A_TIMING1, &t1)))
        return fail("timing mailbox", err);
    if ((err = dma_sent(what, done, bcm_us)))
        return err;
    dma_run_read(what, W * H);
    say("%s: frame %lu us, BCM %lu us, timing %08lx (%s)", what,
        (unsigned long)us, (unsigned long)*bcm_us, (unsigned long)t1,
        khz ? "ours" : "Apple's 150 MHz words");
    return OK;
}

static int stage4c(bool *alive)
{
    const struct bcmdt_image *img = h->hello_dma_clk;
    uint32_t base = 0, fast = 0, slow = 0, rect = 0, st = 0;
    int err;

    say("stage 4c: the 150 MHz clock with the panel (clock-command image)");
    if (!img)
        return fail("no clock-command image in this build", E_STATUS);
    if (res->from0)
        return fail("the core starts at 0: no entry-at-0 clock image", E_STATUS);
    if ((err = boot(img, &res->ready_us)))
        return err;
    *alive = true;
    if (!(res->info & INFO_DMA))
        return fail("not a DMA build (info)", E_STATUS);
    if ((err = upload_pattern()))
        return err;
    say("kick LCD_UPDATE (panel power-on, crystal)");
    if ((err = command(LCD_UPDATE, T_POWERON, NULL)))
        goto bad;
    h->light(true);
    if ((err = c_frame("4c crystal", 2, &base)))
        goto bad;
    if ((err = clock_req(true, NULL)))
        goto bad;
    if ((err = c_frame("4c PLL", 3, &fast)))
        goto bad;
    res->c_base_us = base;
    res->c_fast_us = fast;
    /* the bus clock, from Apple's later word's cycles at 150 MHz */
    say("4c: the LCD bus %s (frame %lu us on the PLL, %lu us on the crystal)",
        fast > 35000 ? "stays on the crystal (27 MHz)" :
        fast > 11000 ? "runs at about half the core (75 MHz)" :
                       "runs at the core's clock (150 MHz)",
        (unsigned long)fast, (unsigned long)base);
    say("PHOTO 4c-1 (the pattern on the PLL: any colour, shift or streak?)");
    h->hold(HOLD_SWEEP);
    if ((err = upload_rect(BCMDT_RECT3_X, BCMDT_RECT3_Y)))
        return err;
    say("kick LCD_UPDATERECT (2-D DMA on the PLL)");
    if ((err = command(LCD_UPDATERECT, T_FRAME, NULL)))
        goto bad;
    if ((err = dma_sent("4c PLL rect", 4, &rect)))
        goto bad;
    dma_run_read("4c PLL rect", BCMDT_RECT_W * BCMDT_RECT_H);
    say("PHOTO 4c-2 (magenta box top left, on the PLL)");
    h->hold(HOLD_SWEEP);
    if ((err = clock_req(false, NULL)))
        goto bad;
    if ((err = upload_pattern()))
        return err;
    if ((err = c_frame("4c crystal again", 5, &slow)))
        goto bad;
    res->c_slow_us = slow;
    say("PHOTO 4c-3 (the pattern back on the crystal, no box)");
    h->hold(HOLD_SWEEP);
    /* asleep on the PLL: the image comes down by itself */
    if ((err = clock_req(true, NULL)))
        goto bad;
    say("kick LCD_SLEEP on the PLL");
    if ((err = command(LCD_SLEEP, T_SLEEP, NULL)))
        goto bad;
    if ((err = rd(A_CLOCK_STATE, &st)))
        return fail("clock state", err);
    say("after LCD_SLEEP: clock state %08lx", (unsigned long)st);
    if ((st & 0xFFFF0000) != CLOCK_MAGIC || (st & CLOCK_FAST))
        return fail("LCD_SLEEP left the core on the PLL", E_STATUS);
    /* the crystal frame after the round trip must take what the first
     * did: a bus left on another clock shows here */
    if (slow * 10 > base * 11 || slow * 10 < base * 9)
        return fail("crystal frame changed after the round trip", E_STATUS);
    res->img4c = img;
    say("stage 4c PASS");
    return OK;

bad:
    if (err == E_FAULT || err == E_TIMEOUT)
    {
        *alive = false;
        log_fault();
    }
    return fail("stage 4c command", err);
}

/* ---- Stage 5b: the drift offload A/B -------------------------------- *
 *
 * offload.md: the main menu's panel drift (a cross-fade into the next
 * cover, then one-pixel pans, about 7.3 frames a second while the menu is
 * lit) done by the PP (mode A) or by the BCM (mode B), in alternating
 * phases, each marked in the developer power log (hal->mark:
 * powerlog_mark) so a USB meter's readings or power.csv's voltage line up
 * with what ran.  Both modes run the drift image (BCMFW_DRIFT: hello_dma
 * plus the drift ops), so only the work differs, not the firmware, and
 * both draw the same frames (bit for bit: the BCM's cross-fade is the
 * shell's xfade_rect on the vector unit, bcmfw/video/xfade.c).
 *   A  the shell's way: each frame composed in the PP's framebuffer
 *      (xfade_rect's step, or the cover's window blitted), its 160 x 240
 *      panel uploaded and pushed by UPDATERECT.
 *   B  the covers uploaded to the BCM once per cycle; then each frame is
 *      a small command (a window shown, or a fade step).
 * The covers are generated (nothing read from the disk) into the HAL's
 * scratch memory, outside the timed work in both modes: they stand for
 * the art cache's reads, which the shell does ahead of time.  Controls
 * (offload.md section 6): backlight on throughout, the PP unboosted,
 * audio stopped (the runbook asks).  Logged per phase: frames, late
 * frames, the PP's busy time (composing and uploading), the BCM's (its
 * push, or its op and push) and the PP's wait for the BCM.  Needs stage
 * 5 in this run; gated like 4b (the debug menu entry asks).  Without the
 * drift image or the scratch memory, B is the dev9 stub on hello_dma. */
#ifndef BCMDT_5B_PHASE_MS
#define BCMDT_5B_PHASE_MS  120000
#endif
#ifndef BCMDT_5B_SETTLE_MS
#define BCMDT_5B_SETTLE_MS 20000
#endif
#ifndef BCMDT_5B_PAIRS
#define BCMDT_5B_PAIRS     3
#endif

/* The drift as the shell runs it: apps/ipodui/nav_core.h and nav_core.c
 * (NAV_DRIFT_*, drift_dir, drift_start, nav_xfade_weight, nav_drift_pos)
 * and preview.c (xfade_rect, drift_fade_step, pan_to, half_window, AVG2,
 * HALF_SLOW_*), as of s8-int 91b06b7784 (8b, p8b-smooth 9fcbf9aed4): each
 * pixel of a pan shown whole, then NAV_DRIFT_HALF later half-way to the
 * next.  A table, so a new definition drops in (xfade_test.py checks the
 * shell's source still reads as ported). */
static const struct drift_def
{
    int steps;              /* NAV_DRIFT_FADE_STEPS */
    uint32_t fade_us;       /* NAV_DRIFT_FADE */
    uint32_t pan_us;        /* NAV_DRIFT_STEP: one pixel */
    uint32_t half_us;       /* NAV_DRIFT_HALF: the half step, 0 none */
    uint32_t cycle_us;      /* NAV_DRIFT_CYCLE */
    int pan_max;            /* NAV_DRIFT_PAN_MAX */
    int side, x, w, h;      /* NAV_DRIFT_SRC; the panel (UI_PANEL_*) */
} drift = { 8, 400000, 150000, 80000, 7000000, 44, 300, 160, 160, 240 };
/* The half steps' guard: preview.c HALF_SLOW_US (a compose slower than
 * this) and HALF_SLOW_MAX (that many in a row turn them off).  Tests set
 * it; us 0 counts every half step as slow (the simulator's PP takes no
 * time). */
static uint32_t drift_slow_us = 25000;
static int drift_slow_max = 4;

void bcmdt_set_half_guard(uint32_t us, int max)
{
    drift_slow_us = us;
    drift_slow_max = max;
}

static const signed char drift_dirs[8][2] = {
    { -1, 0 }, { 1, 1 }, { 0, -1 }, { 1, 0 },
    { -1, -1 }, { 0, 1 }, { 1, -1 }, { -1, 1 },
};

/* The BCM side (bcmfw config.h BCMFW_DRIFT_*) */
#define CMD_DRIFT       CMD(0x42)
#define A_DRIFT_SLOT0   0x108000
#define A_DRIFT_SLOT1   0x010000
#define A_DRIFT_PARAM   0x106000
#define DRIFT_MAGIC     0x44520000
#define DRIFT_SHOW      1
#define DRIFT_PREP      2
#define DRIFT_FADE      3
#define DRIFT_HALF      4

static int drift_start(int range, int dir)
{
    int mid = range / 2;

    return dir > 0 ? mid - drift.pan_max / 2 : dir < 0 ? mid + drift.pan_max / 2 : mid;
}

/* The window of cycle c's cover at pan p (nav_drift_pos) */
static void drift_window(long c, int p, int *x, int *y)
{
    const signed char *d = drift_dirs[c % 8];

    *x = drift_start(drift.side - drift.w, d[0]) + d[0] * p;
    *y = drift_start(drift.side - drift.h, d[1]) + d[1] * p;
}

/* nav_xfade_weight() */
static int drift_weight(int k, int n)
{
    if (n < 1)
        n = 1;
    if (k >= n)
        return 256;
    if (k < 1)
        k = 1;
    return (256 + (n - k + 1) / 2) / (n - k + 1);
}

/* The last pan of a cycle (nav_drift_pos: t < cycle) */
static int drift_pans(void)
{
    int p = (int)((drift.cycle_us - 1 - drift.fade_us) / drift.pan_us);

    return p < drift.pan_max ? p : drift.pan_max;
}

/* A generated cover (seed n): gradients, a texture and flat areas, as
 * art has; bcmdt_drift_px is the test's view of the same picture */
uint16_t bcmdt_drift_px(unsigned n, int u, int v)
{
    unsigned r, g, b;

    if ((u / 40 + v / 30 + n) % 5 == 0)
        return (uint16_t)(0x2104 * (n % 7 + 1));
    r = (((unsigned)u * (3 + n % 5) + (unsigned)v + n * 11) >> 4) & 31;
    g = (((unsigned)v * (2 + n % 3) + (unsigned)u / 2 + n * 7) >> 3) & 63;
    b = ((((unsigned)u ^ (unsigned)v) >> (2 + n % 3)) + n) & 31;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static void drift_cover(uint16_t *slot, unsigned n)
{
    int u, v;

    for (v = 0; v < drift.side; v++)
        for (u = 0; u < drift.side; u++)
            slot[v * drift.side + u] = bcmdt_drift_px(n, u, v);
}

/* preview.c xfade_rect(), as the shell has it (xfade_test.py checks the
 * shell's source still reads so) */
static void drift_xfade(uint16_t *dst, int dstride, const uint16_t *src, int sstride,
                        int w, int h, int weight)
{
    unsigned w32 = (weight + 4) >> 3, iw = 32 - w32;
    int x;

    if (weight >= 256)
    {
        for (; h > 0; h--, dst += dstride, src += sstride)
            memcpy(dst, src, w * sizeof(uint16_t));
        return;
    }
    if (w32 == 0)
        w32 = 1, iw = 31;
    for (; h > 0; h--, dst += dstride, src += sstride)
    {
        for (x = 0; x < w; x++)
        {
            unsigned d = dst[x], s = src[x], r;
            if (d == s)
                continue;
            d = (d | (d << 16)) & 0x07e0f81f;
            s = (s | (s << 16)) & 0x07e0f81f;
            r = ((d * iw + s * w32 + 0x02008010) >> 5) & 0x07e0f81f;
            dst[x] = r | (r >> 16);
        }
    }
}

/* preview.c half_window()/avg_rows(): the panel as the average of the
 * windows at (x0, y0) and (x1, y1), two pixels a word, an odd start read
 * one pixel back and re-paired (little-endian, as the PP and the tests) */
#define DRIFT_AVG2(a, b) (((a) & (b)) + ((((a) ^ (b)) & 0xf7def7deu) >> 1))

static inline uint32_t drift_pair(const uint32_t *w, int i, bool odd)
{
    return odd ? (w[i] >> 16) | (w[i + 1] << 16) : w[i];
}

static void drift_avg(uint16_t *panel, const uint16_t *slot, int x0, int y0,
                      int x1, int y1)
{
    const uint16_t *a = slot + y0 * drift.side + x0, *b = slot + y1 * drift.side + x1;
    bool aodd = x0 & 1, bodd = x1 & 1;
    int h, i;

    for (h = 0; h < drift.h; h++, panel += W, a += drift.side, b += drift.side)
    {
        uint32_t *d = (uint32_t *)panel;
        const uint32_t *wa = (const uint32_t *)(a - aodd);
        const uint32_t *wb = (const uint32_t *)(b - bodd);

        for (i = 0; i < drift.w / 2; i++)
        {
            uint32_t pa = drift_pair(wa, i, aodd), pb = drift_pair(wb, i, bodd);
            d[i] = DRIFT_AVG2(pa, pb);
        }
    }
}

struct drift_run
{
    bool b;                     /* this phase's mode */
    bool real_b;                /* the drift image runs (else the stub) */
    uint16_t *slot[2];          /* the PP's covers (scratch) */
    uint16_t *fb;               /* the PP's framebuffer (mode A) */
    uint32_t pp_us, bcm_us, wait_us;
    bool half_off;              /* A: the guard turned half steps off */
    int slow;                   /* slow half steps in a row (A: compose,
                                   B: the BCM's op), for the guard */
    uint32_t halves, half_max_us, half_slow;  /* this phase's half steps */
    bool b_would_trip;          /* B: the guard would have fired */
};

/* Mode A: the panel up, then pushed (the shell's push_panel) */
static int drift_push_a(struct drift_run *d)
{
    uint32_t t0 = h->now_us(), us, v;
    int y, err;

    for (y = 0; y < drift.h; y++)
        if (h->wr_block(A_FB + 2 * (y * W + drift.x), d->fb + y * W + drift.x,
                        drift.w) < 0)
            return fail("5b upload", E_PORT);
    if (h->wr(A_RECT_XY, drift.x) < 0 || h->wr(A_RECT_WH, drift.w | drift.h << 16) < 0)
        return fail("5b rect parameters", E_PORT);
    d->pp_us += h->now_us() - t0;
    if ((err = command(LCD_UPDATERECT, T_FRAME, &us)))
        return fail("5b push", err);
    d->wait_us += us;
    if (h->rd(A_DMA_US, &v) == 0)
        d->bcm_us += v;
    return OK;
}

/* Mode B: one op on the BCM */
static int drift_op(struct drift_run *d, int op, int slot, int x, int y, int weight)
{
    uint32_t us, v, t0 = h->now_us();
    int err;

    if (h->wr(A_DRIFT_PARAM + 4, (uint32_t)slot << 24 | (uint32_t)y << 12 | (uint32_t)x) < 0 ||
        h->wr(A_DRIFT_PARAM + 8, (uint32_t)weight) < 0 ||
        h->wr(A_DRIFT_PARAM, DRIFT_MAGIC | (uint32_t)op) < 0)
        return fail("5b op parameters", E_PORT);
    d->pp_us += h->now_us() - t0;
    if ((err = command(CMD_DRIFT, T_FRAME, &us)))
        return fail("5b op", err);
    d->wait_us += us;
    if (h->rd(A_DRIFT_PARAM + 20, &v) < 0 || v != (DRIFT_MAGIC | (uint32_t)op))
    {
        say("5b: op %d refused (%08lx)", op, (unsigned long)v);
        return fail("5b op refused", E_STATUS);
    }
    if (h->rd(A_DRIFT_PARAM + 12, &v) == 0)
        d->bcm_us += v;
    return OK;
}

/* The cover in slot s up to the BCM (mode B, once per cycle) */
static int drift_upload(struct drift_run *d, int s)
{
    uint32_t base = s ? A_DRIFT_SLOT1 : A_DRIFT_SLOT0, t0 = h->now_us();
    int v;

    for (v = 0; v < drift.side; v++)
        if (h->wr_block(base + 2 * v * drift.side, d->slot[s] + v * drift.side,
                        drift.side) < 0)
            return fail("5b cover upload", E_PORT);
    d->pp_us += h->now_us() - t0;
    return OK;
}

/* A window shown: a pan, or the cover the phase starts on */
static int drift_show(struct drift_run *d, int s, int x, int y)
{
    uint32_t t0;

    if (d->b)
        return drift_op(d, DRIFT_SHOW, s, x, y, 0);
    t0 = h->now_us();
    drift_xfade(d->fb + drift.x, W, d->slot[s] + y * drift.side + x, drift.side,
                drift.w, drift.h, 256);
    d->pp_us += h->now_us() - t0;
    return drift_push_a(d);
}

static int drift_fade(struct drift_run *d, int s, int x, int y, int weight)
{
    uint32_t t0;

    if (d->b)
        return drift_op(d, DRIFT_FADE, s, x, y, weight);
    t0 = h->now_us();
    drift_xfade(d->fb + drift.x, W, d->slot[s] + y * drift.side + x, drift.side,
                drift.w, drift.h, weight);
    d->pp_us += h->now_us() - t0;
    return drift_push_a(d);
}

/* The 8b half step: half-way from the window on screen (x0, y0) to the
 * next pixel's (x1, y1).  A times its compose as the shell does and
 * turns half steps off after slow_max slow ones in a row (until the
 * stage ends: the shell's until-reboot); B never does, but logs whether
 * its op would have tripped the same guard. */
static int drift_half(struct drift_run *d, int s, int x0, int y0, int x1, int y1)
{
    uint32_t t0, us, bus;
    int err;

    if (d->b)
    {
        bus = d->bcm_us;
        if ((err = drift_op(d, DRIFT_HALF, s, x1, y1, 0)))
            return err;
        us = d->bcm_us - bus;
    }
    else
    {
        t0 = h->now_us();
        drift_avg(d->fb + drift.x, d->slot[s], x0, y0, x1, y1);
        us = h->now_us() - t0;
        d->pp_us += us;
        if ((err = drift_push_a(d)))
            return err;
    }
    d->halves++;
    if (us > d->half_max_us)
        d->half_max_us = us;
    if (us > drift_slow_us || !drift_slow_us)
    {
        d->slow++;
        d->half_slow++;
    }
    else
        d->slow = 0;
    if (d->slow >= drift_slow_max)
    {
        if (d->b && !d->b_would_trip)
        {
            d->b_would_trip = true;
            say("5b: B's half step would trip the shell's guard (%lu us)",
                (unsigned long)us);
        }
        if (!d->b && !d->half_off)
        {
            d->half_off = true;
            say("5b: half steps off (A: %d slow composes in a row, %lu us)",
                d->slow, (unsigned long)us);
        }
    }
    return OK;
}

/* The dev9 stub (no drift image or no scratch): B only pushes the panel
 * the BCM holds, the floor any B can reach; A composes generated rows */
static void drift_row(int y, int s, int p)
{
    int x;

    for (x = 0; x < drift.w; x++)
    {
        int u = x + p;
        uint32_t a = (uint32_t)((u * 7 + y * 3) & 31);
        uint32_t b = (uint32_t)((u * 3 + y * 5 + 16) & 31);
        uint32_t v = (a * (8 - s) + b * s) >> 3;

        row[x] = (uint16_t)(v << 11 | (v * 2) << 5 | (31 - v));
    }
}

static int drift_stub_frame(struct drift_run *d, uint32_t k)
{
    uint32_t t0 = h->now_us(), us;
    int y, err;

    if (!d->b)
    {
        int s = (int)(k % 52) < 8 ? (int)(k % 52) : 8;
        int p = (int)(k % 52) < 8 ? 0 : (int)(k % 52) - 8;

        for (y = 0; y < H; y++)
        {
            drift_row(y, s, p);
            if (h->wr_block(A_FB + 2 * (y * W + drift.x), row, drift.w) < 0)
                return fail("5b upload", E_PORT);
        }
    }
    if (h->wr(A_RECT_XY, drift.x) < 0 || h->wr(A_RECT_WH, drift.w | H << 16) < 0)
        return fail("5b rect parameters", E_PORT);
    d->pp_us += h->now_us() - t0;
    if ((err = command(LCD_UPDATERECT, T_FRAME, &us)))
        return fail("5b push", err);
    d->wait_us += us;
    return OK;
}

/* Wait for a frame's time; counts it late past 10 ms */
static void drift_due(uint32_t due, uint32_t *late)
{
    while ((int32_t)(h->now_us() - due) < 0)
        h->yield();
    if ((int32_t)(h->now_us() - due) > 10000)
        (*late)++;
}

/* Cycle c's cover ready: generated into its slot (the art cache's read,
 * not timed) and, in mode B, up to the BCM with the fade's planes made.
 * Runs in the gap after the last cycle's last pan, as the shell's
 * prefetch has the next cover in memory by then. */
static int drift_prepare(struct drift_run *d, uint32_t c)
{
    int s = (int)((c + 1) & 1), x, y, err;

    drift_cover(d->slot[s], c + 1);
    drift_window((long)c, 0, &x, &y);
    if (d->b && ((err = drift_upload(d, s)) ||
                 (err = drift_op(d, DRIFT_PREP, s, x, y, 0))))
        return err;
    return OK;
}

static int drift_phase(const char *tag, struct drift_run *d, uint32_t ms,
                       uint32_t *frames, uint32_t *late)
{
    uint32_t t0, k = 0, c;
    int x, y, p, n, err, pans = drift_pans();

    *frames = *late = 0;
    d->halves = d->half_max_us = d->half_slow = 0;
    d->slow = 0;
    if (d->real_b)
    {
        /* the cover the phase starts on (last cycle's, at its last pan)
         * and the first fade's, ready before the phase's clock starts */
        drift_cover(d->slot[0], 0);
        drift_window(7, pans, &x, &y);
        if ((d->b && (err = drift_upload(d, 0))) || (err = drift_show(d, 0, x, y)) ||
            (err = drift_prepare(d, 0)))
            return err;
    }
    if (h->mark)
        h->mark(tag);
    say("5b phase %s (%s), %lu ms", tag,
        !d->real_b ? (d->b ? "B stub: BCM pushes, PP idle" : "A: PP composes and uploads")
                   : (d->b ? "B: the BCM fades and pans" : "A: the PP fades and pans"),
        (unsigned long)ms);
    d->pp_us = d->bcm_us = d->wait_us = 0;
    t0 = h->now_us();
    for (c = 0; h->now_us() - t0 < ms * 1000; c++)
    {
        uint32_t cs = t0 + c * drift.cycle_us;
        int s = (int)((c + 1) & 1);

        if (!d->real_b)
        {
            /* the stub's schedule: 52 frames a cycle, 135 ms apart */
            for (n = 0; n < 52 && h->now_us() - t0 < ms * 1000; n++, k++)
            {
                drift_due(cs + (uint32_t)n * (drift.cycle_us / 52), late);
                if ((err = drift_stub_frame(d, k)))
                    return err;
            }
            continue;
        }
        drift_window((long)c, 0, &x, &y);
        for (n = 1; n <= drift.steps && h->now_us() - t0 < ms * 1000; n++, k++)
        {
            drift_due(cs + (uint32_t)(n - 1) * (drift.fade_us / drift.steps), late);
            if ((err = drift_fade(d, s, x, y, drift_weight(n, drift.steps))))
                return err;
        }
        /* pixel p whole, then (8b) half-way to p + 1 */
        for (p = 0; p <= pans && h->now_us() - t0 < ms * 1000; p++)
        {
            uint32_t pt = cs + drift.fade_us + (uint32_t)p * drift.pan_us;
            int x1, y1;

            drift_window((long)c, p, &x, &y);
            if (p)
            {
                drift_due(pt, late);
                if ((err = drift_show(d, s, x, y)))
                    return err;
                k++;
            }
            if (!drift.half_us || (!d->b && d->half_off) ||
                h->now_us() - t0 >= ms * 1000)
                continue;
            drift_window((long)c, p + 1, &x1, &y1);
            drift_due(pt + drift.half_us, late);
            if ((err = drift_half(d, s, x, y, x1, y1)))
                return err;
            k++;
        }
        if (h->now_us() - t0 < ms * 1000 && (err = drift_prepare(d, c + 1)))
            return err;
    }
    *frames = k;
    say("5b phase %s: %lu frames, %lu late, PP busy %lu ms, BCM busy %lu ms, "
        "PP waited %lu ms", tag, (unsigned long)k, (unsigned long)*late,
        (unsigned long)(d->pp_us / 1000), (unsigned long)(d->bcm_us / 1000),
        (unsigned long)(d->wait_us / 1000));
    if (d->real_b && drift.half_us)
        say("5b phase %s: %lu half steps, longest %lu us (%s), %lu over %lu us%s", tag,
            (unsigned long)d->halves, (unsigned long)d->half_max_us,
            d->b ? "the BCM's op" : "the PP's compose", (unsigned long)d->half_slow,
            (unsigned long)drift_slow_us,
            d->b ? (d->b_would_trip ? "; the guard would trip" : "; the guard would not trip")
                 : (d->half_off ? "; half steps off" : ""));
    return OK;
}

static int stage5b(bool *alive)
{
    const struct bcmdt_image *img;
    struct drift_run d;
    uint32_t f, l, i;
    char tag[16];
    void *mem = NULL;
    int err;

    memset(&d, 0, sizeof(d));
    if (h->drift && h->scratch && h->fb)
        mem = h->scratch(2u * 2 * drift.side * drift.side);
    d.real_b = mem != NULL;
    img = d.real_b ? h->drift : res->from0 ? h->hello_dma_rb : h->hello_dma;
    say("stage 5b: drift offload A/B (%d pairs, %lu s phases, %s)", BCMDT_5B_PAIRS,
        (unsigned long)(BCMDT_5B_PHASE_MS / 1000),
        d.real_b ? "B on the BCM's vector unit" :
        !h->drift ? "B is a stub: no drift image" : "B is a stub: no scratch memory");
    if (d.real_b)
    {
        d.slot[0] = (uint16_t *)mem;
        d.slot[1] = d.slot[0] + drift.side * drift.side;
        d.fb = (uint16_t *)h->fb();
        say("drift: fade %d steps over %lu ms, then %d pans %lu ms apart, a %lu ms "
            "cycle; the panel %dx%d at x %d", drift.steps,
            (unsigned long)(drift.fade_us / 1000), drift_pans(),
            (unsigned long)(drift.pan_us / 1000), (unsigned long)(drift.cycle_us / 1000),
            drift.w, drift.h, drift.x);
        if (drift.half_us)
            say("drift: half steps %lu ms into each pixel; off after %d over %lu us in a row "
                "(A)", (unsigned long)(drift.half_us / 1000), drift_slow_max,
                (unsigned long)drift_slow_us);
    }
    say("controls: backlight on, PP unboosted, no disk reads, audio stopped");
    if ((err = boot(img, &res->ready_us)))
        goto out;
    *alive = true;
    if ((err = upload_pattern()))
        goto out;
    if ((err = command(LCD_UPDATE, T_POWERON, NULL)))
        goto bad;
    h->light(true);
    if (h->boost)
        h->boost(false);
    say("settle %lu s", (unsigned long)(BCMDT_5B_SETTLE_MS / 1000));
    h->sleep_ms(BCMDT_5B_SETTLE_MS);
    for (i = 0; i < 2 * BCMDT_5B_PAIRS; i++)
    {
        d.b = i & 1;
        snprintf(tag, sizeof(tag), "s5_%c%lu", d.b ? 'B' : 'A', (unsigned long)(i / 2 + 1));
        if ((err = drift_phase(tag, &d, BCMDT_5B_PHASE_MS, &f, &l)))
            goto out;
        res->m_frames[d.b] += f;
        res->m_late[d.b] += l;
        res->m_pp_us[d.b] += d.pp_us;
        res->m_bcm_us[d.b] += d.bcm_us;
        res->m_wait_us[d.b] += d.wait_us;
    }
    if (h->mark)
        h->mark("s5_end");
    for (i = 0; i < 2; i++)
        say("5b totals %c%s: %lu frames, %lu late, PP %lu ms, BCM %lu ms, PP waited "
            "%lu ms", i ? 'B' : 'A', !i || d.real_b ? "" : " (stub)",
            (unsigned long)res->m_frames[i], (unsigned long)res->m_late[i],
            (unsigned long)(res->m_pp_us[i] / 1000),
            (unsigned long)(res->m_bcm_us[i] / 1000),
            (unsigned long)(res->m_wait_us[i] / 1000));
    say("stage 5b DONE (a measurement: read power.csv or the meter by phase)");
    err = OK;
    goto out;

bad:
    if (err == E_FAULT || err == E_TIMEOUT)
    {
        *alive = false;
        log_fault();
    }
    err = fail("stage 5b command", err);
out:
    if (mem)
        h->scratch(0);
    return err;
}

/* The framebuffer read back word by word against the test pattern
 * (upload_pattern's): the words that differ */
static int fb_check(uint32_t *bad)
{
    uint32_t v;
    int x, y;

    *bad = 0;
    for (y = 0; y < H; y++)
    {
        bcmdt_pattern_row(y, row);
        for (x = 0; x < W; x += 2)
        {
            if (rd(A_FB + (y * W + x) * 2, &v))
                return fail("framebuffer read-back", E_PORT);
            if (v != (row[x] | (uint32_t)row[x + 1] << 16))
                (*bad)++;
        }
    }
    return OK;
}

/* ---- Stage 5i: idle sleep (p14-bcm-idle) ---------------------------- *
 *
 * The everyday DMA image busy-polls the command word whenever it is up,
 * including the unlit clock face's 90 s after the backlight goes off.
 * hello_dma_idle is the same image asleep between commands
 * (BCMFW_IDLE_SLEEP): woken by timer compare 1 every millisecond and, if
 * the host's BCM_CONTROL write raises it, by the doorbell (source 23,
 * [M]).  The stage measures what that costs and whether it holds up:
 *   1. latency, both images: BCMDT_5I_N no-op commands (SELFTEST) and
 *      as many clock-sized UPDATERECTs, each after a gap long enough for
 *      the core to fall asleep, timed kick to done by a tight poll;
 *   2. the idle image held BCMDT_5I_HOLD_MS with the panel on and the
 *      backlight off (the unlit face), a clock UPDATERECT every
 *      BCMDT_5I_FACE_MS: the tick count must match the time, no sleep
 *      may come back with r0-r5 changed, no update may be late;
 *   3. BCMDT_5I_WAKES sleep/wake cycles on the idle image.
 * The doorbell's verdict is logged either way (it decides whether a
 * longer tick is free).  The pass flag ("5i") names the idle image; the
 * boot path never takes it by itself.  Gated like 5b. */
#ifndef BCMDT_5I_N
#define BCMDT_5I_N         60
#endif
#ifndef BCMDT_5I_HOLD_MS
#define BCMDT_5I_HOLD_MS   60000
#endif
#ifndef BCMDT_5I_FACE_MS
#define BCMDT_5I_FACE_MS   10000
#endif
#ifndef BCMDT_5I_WAKES
#define BCMDT_5I_WAKES     2
#endif
#define I_GAP_MS           12   /* the core is asleep again well before */
#define I_SPIN_US          20000    /* tight poll, then the yielding one */
#define I_TICK_US          1000     /* bcmfw BCMFW_IDLE_TICK_US */
#define I_SLACK_US         2000     /* idle may cost a tick plus this */

/* Kick and wait for done by polling without yielding (the host's own
 * scheduling would swamp a millisecond); after I_SPIN_US the ordinary
 * bounded wait takes over */
static int i_command(uint32_t cmd, uint32_t ms, uint32_t *took)
{
    uint32_t t0, st;
    int err;

    if (h->wr(A_COMMAND, cmd) < 0)
        return E_PORT;
    h->doorbell();
    t0 = h->now_us();
    while (h->now_us() - t0 < I_SPIN_US)
    {
        if (rd(A_COMMAND, &st))
            return E_PORT;
        if (st == ST_FAULT)
            return E_FAULT;
        if (st != cmd && st != ST_BUSY)
        {
            *took = h->now_us() - t0;
            return OK;
        }
    }
    err = wait_done(cmd, ms, took);
    if (err == OK)
        *took += I_SPIN_US;
    return err;
}

static int i_rect(uint32_t *us)
{
    const struct e_kind *k = &e_kinds[3];       /* the clock */

    if (h->wr(A_RECT_XY, k->x | k->y << 16) < 0 ||
        h->wr(A_RECT_WH, k->w | k->h << 16) < 0)
        return E_PORT;
    return i_command(LCD_UPDATERECT, T_FRAME, us);
}

/* Boot img with the pattern up, then step 1 on it: avg and max of the
 * no-ops ([0]) and the rects ([1]) */
static int i_latency(const struct bcmdt_image *img, bool *alive,
                     uint32_t avg[2], uint32_t max[2])
{
    uint32_t us, sum[2] = { 0, 0 };
    int i, j, err;

    if ((err = boot(img, &res->ready_us)))
        return err;
    *alive = true;
    if ((err = upload_pattern()) ||
        (err = command(LCD_UPDATE, T_POWERON, NULL)))
        return err;
    h->light(true);
    max[0] = max[1] = 0;
    for (j = 0; j < 2; j++)
    {
        for (i = 0; i < BCMDT_5I_N; i++)
        {
            h->sleep_ms(I_GAP_MS);
            err = j ? i_rect(&us) : i_command(SELFTEST, T_SHORT, &us);
            if (err)
                return err;
            sum[j] += us;
            if (us > max[j])
                max[j] = us;
        }
        avg[j] = sum[j] / BCMDT_5I_N;
    }
    say("5i %s: no-op %lu us avg %lu max; clock rect %lu us avg %lu max",
        img->name, (unsigned long)avg[0], (unsigned long)max[0],
        (unsigned long)avg[1], (unsigned long)max[1]);
    return OK;
}

static int i_counters(uint32_t c[4])
{
    return rd(A_IDLE_TICKS, &c[0]) || rd(A_IDLE_BELLS, &c[1]) ||
           rd(A_IDLE_SLEEPS, &c[2]) || rd(A_IDLE_BAD, &c[3]) ? E_PORT : OK;
}

static int stage5i(bool *alive)
{
    const struct bcmdt_image *idle = h->hello_dma_idle;
    uint32_t avg[2][2], max[2][2], c0[4], c1[4], t0, us, held, want, i;
    uint32_t cmds, late = 0, face_max = 0, wake_max = 0;
    bool ok = true;
    int err;

    say("stage 5i: idle sleep (%s against %s)", idle->name,
        h->hello_dma->name);
    if (res->from0)
        return fail("stage 5i: the core starts at 0, no idle _rb image",
                    E_STATUS);

    /* 1. the busy-poll image, then the idle one */
    if ((err = i_latency(h->hello_dma, alive, avg[0], max[0])))
        goto bad;
    bcm_off(true);
    *alive = false;
    if ((err = i_latency(idle, alive, avg[1], max[1])))
        goto bad;
    if (!(res->info & INFO_IDLE))
        return fail("not an idle-sleep build (info)", E_STATUS);
    if ((err = i_counters(c0)))
        return fail("idle counters", err);
    cmds = 2 * BCMDT_5I_N + 1;              /* the frame, no-ops, rects */
    say("5i counters after step 1: ticks %lu, doorbells %lu (%lu commands "
        "sent), sleeps %lu, bad %lu", (unsigned long)c0[0],
        (unsigned long)c0[1], (unsigned long)cmds, (unsigned long)c0[2],
        (unsigned long)c0[3]);
    say("5i doorbell: %s", c0[1] == 0 ? "never seen (source 23 not raised)" :
        c0[1] >= cmds ? "seen for every command (source 23 is the doorbell)" :
        "seen for some commands only");
    if (max[1][1] > max[0][1] + I_TICK_US + I_SLACK_US)
    {
        say("5i: idle rect max %lu us over busy %lu + a tick",
            (unsigned long)max[1][1], (unsigned long)max[0][1]);
        ok = false;
    }

    /* 2. the unlit face: panel on, backlight off, an update now and then */
    say("5i hold: %lu s, backlight off, a clock update every %lu s",
        (unsigned long)(BCMDT_5I_HOLD_MS / 1000),
        (unsigned long)(BCMDT_5I_FACE_MS / 1000));
    h->light(false);
    if (h->mark)
        h->mark("s5i_hold");
    t0 = h->now_us();
    if ((err = i_counters(c0)))
        return fail("idle counters", err);
    for (i = 0; i < BCMDT_5I_HOLD_MS / BCMDT_5I_FACE_MS; i++)
    {
        h->sleep_ms(BCMDT_5I_FACE_MS);
        if ((err = i_rect(&us)))
            goto bad;
        if (us > face_max)
            face_max = us;
        if (us > max[0][1] + I_TICK_US + I_SLACK_US)
            late++;
    }
    held = h->now_us() - t0;
    if ((err = i_counters(c1)))
        return fail("idle counters", err);
    if (h->mark)
        h->mark("s5i_end");
    want = held / I_TICK_US;
    say("5i hold: %lu ms, ticks %lu (want %lu), doorbells %lu, sleeps %lu, "
        "bad %lu", (unsigned long)(held / 1000), (unsigned long)(c1[0] - c0[0]),
        (unsigned long)want, (unsigned long)(c1[1] - c0[1]),
        (unsigned long)(c1[2] - c0[2]), (unsigned long)c1[3]);
    say("5i hold: %lu clock updates, slowest %lu us, %lu late",
        (unsigned long)i, (unsigned long)face_max, (unsigned long)late);
    if (c1[0] - c0[0] < want - want / 10 || c1[0] - c0[0] > want + want / 10)
    {
        say("5i: the tick count does not match the time held");
        ok = false;
    }
    if (c1[3] != 0 || late != 0)
        ok = false;
    /* SRAM across the sleeps: the pattern uploaded in step 1 */
    if ((err = fb_check(&res->i_fb_bad)))
        goto bad;
    say("5i hold: framebuffer %lu of %lu words changed (SRAM while the "
        "core slept)", (unsigned long)res->i_fb_bad,
        (unsigned long)(W * H / 2));
    if (res->i_fb_bad)
        ok = false;
    h->light(true);

    /* 3. sleep and wake on the idle image */
    for (i = 0; i < BCMDT_5I_WAKES; i++)
    {
        uint32_t ready;

        bcm_off(true);
        *alive = false;
        t0 = h->now_us();
        if ((err = boot(idle, &ready)))
            return err;
        *alive = true;
        if ((err = upload_pattern()) ||
            (err = command(LCD_UPDATE, T_POWERON, &us)))
            goto bad;
        us = h->now_us() - t0;
        say("5i wake %lu: ready %lu us, frame shown at %lu us",
            (unsigned long)(i + 1), (unsigned long)ready, (unsigned long)us);
        if (us > wake_max)
            wake_max = us;
        h->hold(HOLD_DARK);
    }
    if (wake_max >= BOOT_FIRST_US + 300000)
        ok = false;
    res->i_nop_us[0] = avg[0][0];
    res->i_nop_us[1] = avg[1][0];
    res->i_rect_max_us[0] = max[0][1];
    res->i_rect_max_us[1] = max[1][1];
    res->i_ticks = c1[0] - c0[0];
    res->i_bells = c1[1];
    res->i_sleeps = c1[2];
    res->i_bad = c1[3];
    res->i_late = late;
    if (!ok)
        return fail("5i out of bounds", E_STATUS);
    res->img5i = idle;
    say("stage 5i PASS");
    return OK;

bad:
    if (err == E_FAULT || err == E_TIMEOUT)
    {
        *alive = false;
        log_fault();
    }
    return fail("stage 5i command", err);
}

/* ---- Stage 5s: the sleep spikes (Phase 18-0) ------------------------- *
 *
 * What sleep and instant resume need to know from silicon, in one run on
 * hello_dma_sby (the idle image plus panel standby; it sleeps between
 * commands only when a stage 5i pass says the tick wakes it):
 *   1. panel standby: the panel dark with its reset line kept high, at
 *      two depths (light: display off; deep: the whole off script), each
 *      BCMDT_5S_REPS times; every wake timed by the host and by the image
 *      (panel power-on alone), the framebuffer read back after each dark
 *      spell; the cold power-on and a full panel power cycle with the BCM
 *      kept up for comparison;
 *   2. a meter phase per state (power-log marks s5s_on, s5s_light,
 *      s5s_deep, s5s_reset, s5s_cut), BCMDT_5S_HOLD_MS each: panel on and
 *      unlit, light standby, deep standby, panel off with the BCM up, the
 *      BCM's power cut;
 *   3. the boot upload by the PP's second DMA engine: a BCMDT_5S_BIG
 *      block (the image, then a known filler, the size of the resident
 *      video image) uploaded at address 0, by CPU and by engine 2 in
 *      turn, twice each; every word read back before the start, then the
 *      image must start and show a frame.
 * Whether each wake's picture is right is the owner's to see: the stage
 * holds it (BCMDT_5S_LOOK_MS, SELECT moves on) after the cold start and
 * after the first wake from each depth.  No pass flag: nothing takes the
 * results by itself.  Gated like 5i. */
#ifndef BCMDT_5S_HOLD_MS
#define BCMDT_5S_HOLD_MS   20000
#endif
#ifndef BCMDT_5S_LOOK_MS
#define BCMDT_5S_LOOK_MS   15000
#endif
#ifndef BCMDT_5S_REPS
#define BCMDT_5S_REPS      3
#endif
#ifndef BCMDT_5S_BIG
#define BCMDT_5S_BIG       0x30000  /* three engine pieces */
#endif
#ifndef BCMDT_5S_GAP_MS
#define BCMDT_5S_GAP_MS    500      /* dark between the later cycles */
#endif

static const char *const s_depth[3] = { "?", "light", "deep" };

static void s_mark(const char *tag)
{
    if (h->mark)
        h->mark(tag);
}

/* One standby at depth d, dark for dark_ms, then the framebuffer read
 * back and the wake (an LCD_UPDATE) timed: kick to done, and the panel's
 * power-on as the image timed it.  *refused: the image has no such depth
 * for this panel (nothing done). */
static int s_cycle(uint32_t d, uint32_t dark_ms, uint32_t *wake_us,
                   uint32_t *pon_us, bool *refused)
{
    uint32_t st, info, bad;
    int err;

    *refused = false;
    if (h->wr(A_STANDBY, STANDBY_MAGIC | d) < 0)
        return E_PORT;
    if ((err = command(LCD_STANDBY, T_SLEEP, NULL)) ||
        (err = rd(A_STANDBY_ST, &st)) || (err = rd(A_INFO, &info)))
        return err;
    if (st & STANDBY_REFUSED)
    {
        *refused = true;
        return OK;
    }
    if ((st & 0xFF) != d || (info & INFO_PANEL_ON))
    {
        say("5s %s: state %08lx, info %08lx after the command", s_depth[d],
            (unsigned long)st, (unsigned long)info);
        return E_STATUS;
    }
    h->sleep_ms(dark_ms);
    if ((err = fb_check(&bad)))
        return err;
    res->s_fb_bad += bad;
    if ((err = command(LCD_UPDATE, T_POWERON, wake_us)) ||
        (err = rd(A_PON_US, pon_us)))
        return err;
    if (!(*pon_us & PON_FROM_SBY))
    {
        say("5s %s: the wake was a cold power-on", s_depth[d]);
        return E_STATUS;
    }
    *pon_us &= ~PON_FROM_SBY;
    say("5s %s: dark %lu ms, framebuffer %lu words changed; wake %lu us "
        "(panel %lu us)", s_depth[d], (unsigned long)dark_ms,
        (unsigned long)bad, (unsigned long)*wake_us, (unsigned long)*pon_us);
    return OK;
}

/* Step 3's block: the image, then a filler every word of which differs */
static void s_fill(uint8_t *big, uint32_t bytes, const struct bcmdt_image *img)
{
    uint32_t i, x = 0x5A17C0DEu;

    memcpy(big, img->data, img->size);
    memset(big + img->size, 0, ((img->size + 3) & ~3u) - img->size);
    for (i = (img->size + 3) & ~3u; i < bytes; i += 4)
    {
        x = x * 1664525u + 1013904223u;
        memcpy(big + i, &x, 4);
    }
}

static int s_boot(const uint8_t *big, uint32_t bytes, int how, bool *alive)
{
    static const char *const names[] = { "CPU", "engine 2" };
    uint32_t out[4], us;
    int err;

    say("5s upload: %lu bytes by %s", (unsigned long)bytes, names[how]);
    cur_img = h->hello_dma_sby;
    if (h->boot_how(big, bytes, how, out) < 0)
        return fail(how ? "engine-2 upload" : "CPU upload", E_PORT);
    say("5s upload %s: %lu us, %lu words read back wrong (first at %08lx), "
        "%lu pieces", names[how], (unsigned long)out[0], (unsigned long)out[1],
        (unsigned long)out[2], (unsigned long)out[3]);
    if (out[0] > res->s_up_us[how])
        res->s_up_us[how] = out[0];
    res->s_up_bad[how] += out[1];
    if (how == BCMDT_BOOT_E2)
        res->s_pieces = out[3];
    if ((err = boot_ready(&us)))
        return err;
    *alive = true;
    if (us > res->s_ready_us[how])
        res->s_ready_us[how] = us;
    if ((err = upload_pattern()) ||
        (err = command(LCD_UPDATE, T_POWERON, &us)))
        return err;
    if (us > res->s_first_us[how])
        res->s_first_us[how] = us;
    say("5s upload %s: started, first frame %lu us", names[how],
        (unsigned long)us);
    return OK;
}

static int stage5s(bool *alive, bool may_sleep)
{
    const struct bcmdt_image *img = h->hello_dma_sby;
    uint32_t us, pon, c[4], d, i;
    bool refused, ok = true;
    uint8_t *big;
    int err;

    say("stage 5s: sleep spikes (%s; core sleep %s)", img->name,
        may_sleep ? "on, stage 5i passed" : "off, no stage 5i pass");
    if (res->from0)
        return fail("stage 5s: the core starts at 0, no _rb image", E_STATUS);

    /* 1 and 2: the panel standby, the meter phases */
    if ((err = boot(img, &res->ready_us)))
        return err;
    *alive = true;
    if (!(res->info & INFO_STANDBY))
        return fail("not a standby build (info)", E_STATUS);
    if (may_sleep && h->wr(A_IDLE_EN, IDLE_EN_MAGIC) < 0)
        return fail("idle enable", E_PORT);
    res->s_slept = may_sleep;
    if ((err = upload_pattern()) ||
        (err = command(LCD_UPDATE, T_POWERON, &res->s_cold_us)) ||
        (err = rd(A_PON_US, &res->s_cold_pon_us)))
        goto bad;
    say("5s cold: first frame %lu us (panel power-on %lu us)",
        (unsigned long)res->s_cold_us, (unsigned long)res->s_cold_pon_us);
    h->light(true);
    say("5s look 1: the test pattern, panel started cold");
    h->hold(BCMDT_5S_LOOK_MS);
    s_mark("s5s_on");
    say("5s meter: panel on, light off, %lu s",
        (unsigned long)(BCMDT_5S_HOLD_MS / 1000));
    h->light(false);
    h->sleep_ms(BCMDT_5S_HOLD_MS);
    for (d = STANDBY_LIGHT; d <= STANDBY_DEEP; d++)
    {
        for (i = 0; i < BCMDT_5S_REPS; i++)
        {
            if (i == 0)
            {
                s_mark(d == STANDBY_LIGHT ? "s5s_light" : "s5s_deep");
                say("5s meter: %s standby, %lu s", s_depth[d],
                    (unsigned long)(BCMDT_5S_HOLD_MS / 1000));
            }
            if ((err = s_cycle(d, i ? BCMDT_5S_GAP_MS : BCMDT_5S_HOLD_MS, &us, &pon,
                               &refused)))
                goto bad;
            if (refused)
            {
                res->s_refused |= 1u << d;
                say("5s %s: refused (no such standby for panel %d)",
                    s_depth[d], res->panel_id);
                break;
            }
            if (us > res->s_wake_us[d - 1])
                res->s_wake_us[d - 1] = us;
            if (pon > res->s_pon_us[d - 1])
                res->s_pon_us[d - 1] = pon;
            if (i == 0)
            {
                h->light(true);
                say("5s look %lu: the pattern after the %s wake",
                    (unsigned long)(d + 1), s_depth[d]);
                h->hold(BCMDT_5S_LOOK_MS);
                h->light(false);
            }
        }
    }
    if (res->s_refused & (1u << STANDBY_DEEP))
        ok = false;                     /* every panel has the deep one */

    /* the panel's own power cycle (reset low) with the BCM kept up */
    s_mark("s5s_reset");
    say("5s meter: panel off (reset low), BCM up, %lu s",
        (unsigned long)(BCMDT_5S_HOLD_MS / 1000));
    if ((err = command(LCD_SLEEP, T_SLEEP, NULL)))
        goto bad;
    h->sleep_ms(BCMDT_5S_HOLD_MS);
    if ((err = command(LCD_UPDATE, T_POWERON, &res->s_reset_us)) ||
        (err = rd(A_PON_US, &res->s_reset_pon_us)))
        goto bad;
    say("5s reset: wake %lu us (panel power-on %lu us), BCM kept up",
        (unsigned long)res->s_reset_us, (unsigned long)res->s_reset_pon_us);
    if (may_sleep)
    {
        if ((err = i_counters(c)))
            goto bad;
        res->s_ticks = c[0];
        res->s_sleeps = c[2];
        res->s_bad = c[3];
        say("5s idle: ticks %lu, doorbells %lu, sleeps %lu, bad %lu",
            (unsigned long)c[0], (unsigned long)c[1], (unsigned long)c[2],
            (unsigned long)c[3]);
        if (c[3])
            ok = false;
    }
    h->light(true);
    say("5s look 4: the pattern after a full panel power-on");
    h->hold(BCMDT_5S_LOOK_MS);
    say("5s wakes: light %lu us (panel %lu), deep %lu us (panel %lu)",
        (unsigned long)res->s_wake_us[0], (unsigned long)res->s_pon_us[0],
        (unsigned long)res->s_wake_us[1], (unsigned long)res->s_pon_us[1]);
    say("5s wakes: reset %lu us (panel %lu), cold %lu us (panel %lu); "
        "fb changed %lu",
        (unsigned long)res->s_reset_us, (unsigned long)res->s_reset_pon_us,
        (unsigned long)res->s_cold_us, (unsigned long)res->s_cold_pon_us,
        (unsigned long)res->s_fb_bad);
    if (res->s_fb_bad)
        ok = false;

    /* the power cut, for the meter */
    bcm_off(true);
    *alive = false;
    s_mark("s5s_cut");
    say("5s meter: BCM power cut, %lu s",
        (unsigned long)(BCMDT_5S_HOLD_MS / 1000));
    h->sleep_ms(BCMDT_5S_HOLD_MS);

    /* 3: the boot upload by engine 2 */
    if (!h->boot_how || !h->scratch ||
        !(big = h->scratch(BCMDT_5S_BIG)))
    {
        say("5s upload: skipped (no engine-2 boot or no memory)");
        ok = false;
    }
    else
    {
        s_fill(big, BCMDT_5S_BIG, img);
        res->s_up_bytes = BCMDT_5S_BIG;
        /* CPU, engine 2, CPU, engine 2; a failure leaves the BCM to the
         * caller's power-down (alive as the boot left it) */
        for (i = 0, err = OK; i < 2 * BCMDT_BOOT_N; i++)
        {
            if ((err = s_boot(big, BCMDT_5S_BIG, i % BCMDT_BOOT_N, alive)))
                break;
            bcm_off(true);
            *alive = false;
        }
        h->scratch(0);
        if (err)
            goto bad;
        say("5s upload: CPU %lu us, engine 2 %lu us (%lu pieces); wrong "
            "%lu / %lu", (unsigned long)res->s_up_us[0],
            (unsigned long)res->s_up_us[1], (unsigned long)res->s_pieces,
            (unsigned long)res->s_up_bad[0], (unsigned long)res->s_up_bad[1]);
        say("5s upload: ready %lu / %lu us, first frame %lu / %lu us",
            (unsigned long)res->s_ready_us[0], (unsigned long)res->s_ready_us[1],
            (unsigned long)res->s_first_us[0], (unsigned long)res->s_first_us[1]);
        if (res->s_up_bad[0] || res->s_up_bad[1])
            ok = false;
    }
    s_mark("s5s_end");
    if (!ok)
        return fail("5s out of bounds", E_STATUS);
    say("stage 5s PASS");
    return OK;

bad:
    if (err == E_FAULT || err == E_TIMEOUT)
    {
        *alive = false;
        log_fault();
    }
    return fail("stage 5s command", err);
}

/* ---- stage 3d: the 3D render test (bcmfw/r3d, render3d.md) ----------- *
 * The renderer image (hello_3d) runs each built-in scene for
 * BCMDT_3D_MS on the crystal (27 MHz), then, with 4b and 4c flags, again
 * on the PLL (150 MHz), each held to 30 fps (BCMDT_3D_PERIOD).  Each
 * run's two check frames (0 ms and BCMDT_3D_CHECK_MS on its camera
 * path) must carry the simulator's checksums (bcm-devtest-r3d.h). */
#ifndef BCMDT_3D_MS
#define BCMDT_3D_MS        10000
#endif
#ifndef BCMDT_3D_CHECK_MS
#define BCMDT_3D_CHECK_MS  5000
#endif
#ifndef BCMDT_3D_RUN
#define BCMDT_3D_RUN       BCMDT_3D_SCENES  /* scenes run (the harness: fewer) */
#endif
#ifndef BCMDT_3D_PERIOD
#define BCMDT_3D_PERIOD    33333    /* us: 30 fps, the panel's useful rate */
#endif
#define A_R3D          0x106000     /* bcmfw config.h BCMFW_R3D_PARAM */
#define R3D_W(k)       (A_R3D + 4 * (k))
#define R3D_OP         0
#define R3D_SCENE      1
#define R3D_MS         2
#define R3D_CHECK      3
#define R3D_STATE      4
#define R3D_FRAMES     5
#define R3D_US         6
#define R3D_RENDER     7
#define R3D_SETUP      8
#define R3D_RASTER     9
#define R3D_WAIT       10
#define R3D_MAX        11
#define R3D_MIN        12
#define R3D_TRIS       13
#define R3D_DRAWN      14
#define R3D_CULLED     15
#define R3D_CLIPPED    16
#define R3D_PIXELS     17
#define R3D_CRC0       18
#define R3D_CRC1       19
#define R3D_CHUNKS     20
#define R3D_INIT       21
#define R3D_PERIOD     22
#define R3D_LATE       23
#define R3D_SPARE      24
#define R3D_SYNC       25
#define R3D_TE_N       26
#define R3D_TE_MIN     27
#define R3D_TE_MAX     28
#define R3D_TE_SUM     29
#define R3D_TE_HIGH    30
#define R3D_TE_LEVEL   31
#define R3D_N          32
#define R3D_RUN        0x33440001   /* "3D" << 16 | run */
#define R3D_TE         0x33440002   /* ... | time the TE line */
#define BCMDT_3D_TE_MS 500          /* the TE probe's length */
#define BCMDT_3D_SYNC_SCENE 4       /* the TE-synced run: the village walk */
#define R3D_ST_DONE    0x33440002
#define CMD_R3D        CMD(0x44)

static const char *const d3_name[BCMDT_3D_SCENES] =
    { "courtyard", "village", "valley", "ruins", "village walk" };

/* The simulator's checksum for (scene, ms), or 0 if none is recorded */
static uint32_t d3_golden(uint32_t scene, uint32_t ms)
{
    unsigned i;

    for (i = 0; i < sizeof(bcmdt_r3d_golden) / sizeof(bcmdt_r3d_golden[0]); i++)
        if (bcmdt_r3d_golden[i].scene == scene && bcmdt_r3d_golden[i].ms == ms)
            return bcmdt_r3d_golden[i].crc;
    return 0;
}

static unsigned long per(uint32_t v, uint32_t n)
{
    return n ? (unsigned long)((v + n / 2) / n) : 0;
}

/* The panel's tearing-effect line (GPIO 49), timed by the image for
 * BCMDT_3D_TE_MS: its refresh period, into res->d_te_us (0: no edges) */
static int d3_te(void)
{
    uint32_t w[R3D_N], n;
    int err, k;

    if (h->wr(R3D_W(R3D_OP), R3D_TE) < 0 || h->wr(R3D_W(R3D_MS), BCMDT_3D_TE_MS) < 0)
        return fail("3d parameters", E_PORT);
    say("kick R3D: time the panel's TE line (GPIO 49) for %d ms", BCMDT_3D_TE_MS);
    if ((err = command(CMD_R3D, BCMDT_3D_TE_MS + 1000, NULL)))
        return err;
    for (k = R3D_STATE; k < R3D_N; k++)
        if ((err = rd(R3D_W(k), &w[k])))
            return fail("3d results", err);
    n = w[R3D_TE_N];
    res->d_te_edges = n;
    res->d_te_us = n > 2 ? w[R3D_TE_SUM] / (n - 1) : 0;
    say("TE: %lu rising edges, level %lu at the start, %lu changes", (unsigned long)n,
        (unsigned long)(w[R3D_TE_LEVEL] & 1), (unsigned long)(w[R3D_TE_LEVEL] >> 8));
    if (res->d_te_us)
        say("TE: period %lu us (%lu.%lu Hz), min %lu, max %lu; a pulse %lu us high",
            (unsigned long)res->d_te_us,
            (unsigned long)((10000000u + res->d_te_us / 2) / res->d_te_us / 10),
            (unsigned long)((10000000u + res->d_te_us / 2) / res->d_te_us % 10),
            (unsigned long)w[R3D_TE_MIN],
            (unsigned long)w[R3D_TE_MAX], (unsigned long)w[R3D_TE_HIGH]);
    else
        say("TE: no period (the line is idle or not enabled on this panel)");
    return OK;
}

/* One scene on one clock: run it, read the results back, log them.
 * sync: 0 the 30 fps slot timer; n: each frame on the n-th TE edge */
static int d3_run(int fast, uint32_t sc, uint32_t sync)
{
    uint32_t w[R3D_N], c0, c1, n;
    unsigned long fps10;
    int err, k;
    char tag[12];

    if (h->wr(R3D_W(R3D_OP), R3D_RUN) < 0 || h->wr(R3D_W(R3D_SCENE), sc) < 0 ||
        h->wr(R3D_W(R3D_MS), BCMDT_3D_MS) < 0 ||
        h->wr(R3D_W(R3D_CHECK), BCMDT_3D_CHECK_MS) < 0 ||
        h->wr(R3D_W(R3D_PERIOD), BCMDT_3D_PERIOD) < 0 ||
        h->wr(R3D_W(R3D_SYNC), sync) < 0)
        return fail("3d parameters", E_PORT);
    snprintf(tag, sizeof(tag), "s3d_%lu%c%s", (unsigned long)sc, fast ? 'f' : 's',
             sync ? "te" : "");
    if (h->mark)
        h->mark(tag);
    say("kick R3D: scene %lu (%s), %lu ms at %s%s", (unsigned long)sc, d3_name[sc],
        (unsigned long)BCMDT_3D_MS, fast ? "150 MHz" : "27 MHz",
        sync ? ", on the TE line" : "");
    if ((err = command(CMD_R3D, BCMDT_3D_MS + 8000, NULL)))
        return err;
    for (k = R3D_STATE; k < R3D_N; k++)
        if ((err = rd(R3D_W(k), &w[k])))
            return fail("3d results", err);
    if (w[R3D_STATE] != R3D_ST_DONE || !w[R3D_FRAMES])
    {
        say("3d scene %lu: state %08lx, %lu frames", (unsigned long)sc,
            (unsigned long)w[R3D_STATE], (unsigned long)w[R3D_FRAMES]);
        return fail("3d run", E_STATUS);
    }
    n = w[R3D_FRAMES];
    fps10 = w[R3D_US] ? (unsigned long)((uint64_t)n * 10000000u / w[R3D_US]) : 0;
    say("3d %lu @%d: %lu frames in %lu ms = %lu.%lu fps, %lu late; init %lu ms",
        (unsigned long)sc, fast ? 150 : 27, (unsigned long)n,
        (unsigned long)(w[R3D_US] / 1000), fps10 / 10, fps10 % 10,
        (unsigned long)w[R3D_LATE], (unsigned long)(w[R3D_INIT] / 1000));
    say("3d %lu @%d: render avg %lu us (setup %lu, raster %lu), max %lu, min %lu",
        (unsigned long)sc, fast ? 150 : 27, per(w[R3D_RENDER], n),
        per(w[R3D_SETUP], n), per(w[R3D_RASTER], n), (unsigned long)w[R3D_MAX],
        (unsigned long)w[R3D_MIN]);
    say("3d %lu @%d: a frame: %lu tris, %lu drawn, %lu culled, %lu clipped, %lu px",
        (unsigned long)sc, fast ? 150 : 27, (unsigned long)w[R3D_TRIS],
        per(w[R3D_DRAWN], n), per(w[R3D_CULLED], n), per(w[R3D_CLIPPED], n),
        per(w[R3D_PIXELS], n));
    say("3d %lu @%d: panel wait %lu us, idle %lu us a frame; chunks out %lu",
        (unsigned long)sc, fast ? 150 : 27, per(w[R3D_WAIT], n),
        per(w[R3D_SPARE], n), per(w[R3D_CHUNKS], n));
    c0 = d3_golden(sc, 0);
    c1 = d3_golden(sc, BCMDT_3D_CHECK_MS);
    say("3d %lu: crc 0 ms %08lx (sim %08lx) %s, %lu ms %08lx (sim %08lx) %s",
        (unsigned long)sc, (unsigned long)w[R3D_CRC0], (unsigned long)c0,
        w[R3D_CRC0] == c0 ? "ok" : "DIFFERS", (unsigned long)BCMDT_3D_CHECK_MS,
        (unsigned long)w[R3D_CRC1], (unsigned long)c1,
        w[R3D_CRC1] == c1 ? "ok" : "DIFFERS");
    res->d_crc_bad += (w[R3D_CRC0] != c0) + (w[R3D_CRC1] != c1);
    if (sync)
    {
        res->d_sync_frames = n;
        res->d_sync_us = w[R3D_US];
        res->d_sync_late = w[R3D_LATE];
        return OK;
    }
    res->d_tris[sc] = w[R3D_TRIS];
    res->d_frames[fast][sc] = n;
    res->d_us[fast][sc] = w[R3D_US];
    res->d_render[fast][sc] = w[R3D_RENDER];
    res->d_max[fast][sc] = w[R3D_MAX];
    res->d_late[fast][sc] = w[R3D_LATE];
    res->d_drawn[fast][sc] = w[R3D_DRAWN];
    return OK;
}

static int stage3d(bool *alive, bool fast)
{
    const struct bcmdt_image *img = h->hello_3d;
    uint32_t sc;
    int err, f;

    say("stage 3d: the 3D render test, %d scenes, %lu s each at 30 fps; 150 MHz %s",
        BCMDT_3D_RUN, (unsigned long)(BCMDT_3D_MS / 1000),
        fast ? "too (4b, 4c flags)" : "not run (needs 4b and 4c flags)");
    if (!img)
        return fail("no 3D image in this build", E_STATUS);
    if (res->from0)
        return fail("the core starts at 0: no entry-at-0 3D image", E_STATUS);
    if ((err = boot(img, &res->ready_us)))
        return err;
    *alive = true;
    if (!(res->info & INFO_DMA))
        return fail("not a DMA build (info)", E_STATUS);
    say("kick LCD_POWERON");
    if ((err = command(LCD_POWERON, T_POWERON, NULL)))
        goto bad;
    h->light(true);
    if (h->boost)
        h->boost(false);
    if ((err = d3_te()))
        goto bad;
    for (f = 0; f <= (fast ? 1 : 0); f++)
    {
        if (f && (err = clock_req(true, NULL)))
            goto bad;
        for (sc = 0; sc < BCMDT_3D_RUN; sc++)
            if ((err = d3_run(f, sc, 0)))
                goto bad;
        /* the walk once more, each frame on the TE edge nearest 30 fps */
        if (f == (fast ? 1 : 0) && res->d_te_us)
        {
            uint32_t k = (BCMDT_3D_PERIOD + res->d_te_us / 2) / res->d_te_us;

            res->d_sync = k ? k : 1;
            if ((err = d3_run(f, BCMDT_3D_SYNC_SCENE < BCMDT_3D_RUN ?
                              BCMDT_3D_SYNC_SCENE : 0, res->d_sync)))
                goto bad;
        }
        if (f && (err = clock_req(false, NULL)))
            goto bad;
    }
    res->d_fast = fast;
    if (h->mark)
        h->mark("s3d_end");
    if (res->d_crc_bad)
    {
        say("FAIL 3d: %lu check frames differ from the simulator's",
            (unsigned long)res->d_crc_bad);
        return fail("3d check frames", E_STATUS);
    }
    say("stage 3d PASS (every scene ran; check frames match the simulator)");
    return OK;

bad:
    if (err == E_FAULT || err == E_TIMEOUT)
    {
        *alive = false;
        log_fault();
    }
    return fail("stage 3d", err);
}

/* ---- Stages 4 and 4b: the timing bench ------------------------------ */

/* What a step word means, for the log (kernel numbers are bench_tab.c's
 * order; bcmfw/bench/readlog.py names them) */
static void step_name(uint32_t st, char *buf, int len)
{
    uint32_t k = st & 0xFFF;

    switch (st & ~0xFFFu)
    {
    case BENCH_STEP_VEC:
        snprintf(buf, len, "vector check %lu", (unsigned long)k);
        break;
    case BENCH_STEP_SCALAR:
        snprintf(buf, len, "scalar checks");
        break;
    case BENCH_STEP_TIME:
        snprintf(buf, len, "timing kernel %lu", (unsigned long)k);
        break;
    case BENCH_STEP_PLL:
        snprintf(buf, len, "150 MHz switch, step %lu", (unsigned long)k);
        break;
    case BENCH_STEP_TIME2:
        snprintf(buf, len, "timing kernel %lu at 150 MHz", (unsigned long)k);
        break;
    case BENCH_STEP_SLOC:
        snprintf(buf, len, "store-locating check %lu", (unsigned long)k);
        break;
    default:
        snprintf(buf, len, st == BENCH_DONE ? "done" :
                 (st & ~0xFu) == BENCH_STEP_PROBE ? "probe" : "unknown");
        break;
    }
}

/* Poll the step word until done: a fault, a step that stops moving or
 * the overall limit ends it.  Each new step is logged (flushed) before
 * the next poll, so a cut-off run still names where it was. */
static int bench_wait(uint32_t total_ms)
{
    uint32_t t0 = h->now_us(), tchg = t0, st = 0, step = 0, last = 0;
    char what[40];
    bool first = true;

    for (;;)
    {
        uint32_t now;

        if (rd(A_COMMAND, &st) || rd(A_P_STEP, &step))
            return fail("bench poll", E_PORT);
        now = h->now_us();
        if (first || step != last)
        {
            step_name(step, what, sizeof(what));
            say("bench step %08lx: %s", (unsigned long)step, what);
            res->b_step = last = step;
            tchg = now;
            first = false;
        }
        if (st == ST_FAULT)
        {
            log_fault();
            return fail("bench", E_FAULT);
        }
        if (step == BENCH_DONE)
            break;
        if (now - tchg > T_BENCH_STALL * 1000u ||
            now - t0 > total_ms * 1000u)
        {
            step_name(step, what, sizeof(what));
            say("bench stopped at step %08lx (%s): %s", (unsigned long)step,
                what, now - tchg > T_BENCH_STALL * 1000u ? "no progress for 3 s"
                                                         : "over the time limit");
            log_fault();
            return fail(now - tchg > T_BENCH_STALL * 1000u ? "bench stalled"
                                                           : "bench time limit",
                        E_TIMEOUT);
        }
        h->sleep_ms(T_BENCH_POLL);
    }
    res->b_us = h->now_us() - t0;
    return OK;
}

static int popcount32(uint32_t v)
{
    int n = 0;

    for (; v; v &= v - 1)
        n++;
    return n;
}

/* Log the vector check records (bench.h, BR_VREC), if the image wrote
 * them (header word 24 non-zero; older images leave it 0), two lines
 * per check (a log line holds 112 characters):
 *   B4V k TAG flags got0 got1 got2 got3 bad want
 *   B4VL k late0 late1 late2 late3
 * flags bit 0 pass, bit 1 late pass, bits 15:8 words, 31:16 first bad
 * word.  Bounded: 12 reads per check, at most BENCH_MAX_V checks. */
static int bench_vrecs(uint32_t base, const uint32_t *hdr)
{
    uint32_t k, i, v[BENCH_VREC_W];
    int err;

    if (hdr[24] == 0)
        return OK;
    if (hdr[25] != BENCH_VREC_W || hdr[23] == 0 || hdr[23] > BENCH_MAX_V ||
        base + 4 * (hdr[24] + hdr[23] * BENCH_VREC_W) > 0x100000)
    {
        say("B4V none: records at %lx, %lu words, %lu checks",
            (unsigned long)hdr[24], (unsigned long)hdr[25], (unsigned long)hdr[23]);
        return OK;
    }
    for (k = 0; k < hdr[23]; k++)
    {
        char tag[5];

        for (i = 0; i < BENCH_VREC_W; i++)
            if ((err = rd(base + 4 * (hdr[24] + k * BENCH_VREC_W + i), &v[i])))
                return fail("bench vector records", err);
        for (i = 0; i < 4; i++)
        {
            char c = (char)(v[0] >> (24 - 8 * i));
            tag[i] = (c > ' ' && c < 0x7F) ? c : '?';
        }
        tag[4] = '\0';
        say("B4V %lu %s %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
            (unsigned long)k, tag, (unsigned long)v[1], (unsigned long)v[2],
            (unsigned long)v[3], (unsigned long)v[4], (unsigned long)v[5],
            (unsigned long)v[10], (unsigned long)v[11]);
        say("B4VL %lu %08lx %08lx %08lx %08lx", (unsigned long)k,
            (unsigned long)v[6], (unsigned long)v[7], (unsigned long)v[8],
            (unsigned long)v[9]);
    }
    say("B4V late %lx", (unsigned long)hdr[26]);
    return OK;
}

/* Log the store-locating records (bench.h, BR_SLOC; dev4), if the image
 * wrote them (header word 30 non-zero), one line per stored row:
 *   B4S k row changed first last w_first w_target w_last
 * changed words in the buffer, offsets in bytes from the target (signed).
 * Bounded: 8 reads per record, at most 8 records. */
static int bench_slocs(uint32_t base, const uint32_t *hdr)
{
    uint32_t k, i, v[8];
    int err;

    if (hdr[30] == 0)
        return OK;
    if (hdr[31] == 0 || hdr[31] > 8 || base + 4 * (hdr[30] + 8 * hdr[31]) > 0x100000)
    {
        say("B4S none: records at %lx, %lu", (unsigned long)hdr[30],
            (unsigned long)hdr[31]);
        return OK;
    }
    for (k = 0; k < hdr[31]; k++)
    {
        for (i = 0; i < 8; i++)
            if ((err = rd(base + 4 * (hdr[30] + 8 * k + i), &v[i])))
                return fail("bench store-locating records", err);
        say("B4S %lu %lu %lu %ld %ld %08lx %08lx %08lx", (unsigned long)k,
            (unsigned long)(v[0] & 0xFFFF), (unsigned long)v[1], (long)(int32_t)v[2],
            (long)(int32_t)v[3], (unsigned long)v[4], (unsigned long)v[5],
            (unsigned long)v[6]);
    }
    return OK;
}

/* Read and log one timing table: *ok counts the entries with
 * 0 < fastest <= slowest, and ALU1's count and fastest run */
static int bench_table(uint32_t base, int t, uint32_t nk, uint32_t *ok,
                       uint32_t *alu1_insns, uint32_t *alu1)
{
    uint32_t k, i, e[BENCH_EW];
    int err;

    for (k = 0; k < nk; k++)
    {
        char tag[5];

        for (i = 0; i < 7; i++)
            if ((err = rd(base + 4 * (k * BENCH_EW + i), &e[i])))
                return fail("bench table", err);
        for (i = 0; i < 4; i++)
        {
            char c = (char)(e[0] >> (24 - 8 * i));
            tag[i] = (c > ' ' && c < 0x7F) ? c : '?';
        }
        tag[4] = '\0';
        /* B4K table kernel tag insns ops rows min max per_iter */
        say("B4K %d %lu %s %lu %lu %lu %lu %lu %lu", t, (unsigned long)k, tag,
            (unsigned long)e[1], (unsigned long)e[2], (unsigned long)e[3],
            (unsigned long)e[4], (unsigned long)e[5], (unsigned long)e[6]);
        if (e[1] != 0 && e[4] != 0 && e[4] <= e[5])
            (*ok)++;
        if (e[0] == BENCH_TAG_ALU1 && e[4] != 0)
        {
            *alu1_insns = e[1];
            *alu1 = e[4];
        }
    }
    return OK;
}

static uint32_t khz(uint32_t insns, uint32_t ticks)
{
    return ticks ? (uint32_t)((uint64_t)insns * 1000u / ticks) : 0;
}

/* ---- Bench row ALGN (stage 4, after the bench; bench-p19.md "ALGN") ---
 *
 * Part 1, the BCM's own accesses: CMD_ALGN runs bcmfw/bench/algn.S's
 * table on the bench image, each fault caught by the image; the block
 * (bcmfw/bench/algn.h) is self-describing, so a new check needs no change
 * here.  Lines (readlog.py names the reading each one fits):
 *   ALGN n N faults F src S buf B in U us
 *   ALGN k TAG status pc at r2 r3 r4 r5
 *   ALGNB k w0 .. w7          (the target buffer's window after the check)
 *   ALGN stopped at check k (TAG): ...   (no answer: that check hung)
 * Part 2, the host port's rules: writes in chosen forms into scratch SRAM
 * the bench image never uses, every halfword read back against a shadow
 * of what an exact port would hold:
 *   ALGNP k TAG bad N low2 M of T [+off got want]...
 * low2 counts wrong halfwords that hold the shadow's next halfword: the
 * write landed 2 bytes low (the address's bits 1:0 ignored). */
#define ALGN_CMD        CMD(0x45)
#define ALGN_T_MS       2000
#define ALGN_BLOCK      0xC3000
#define ALGN_MAGIC      0x414C4731
#define ALGN_HDR        8
#define ALGN_MAXN       60
#define PA_BASE         0xF0000     /* the host frame's area: the bench image
                                       draws nothing (no panel I/O) */
#define PA_STEP         0x2000      /* a fresh region a check */
#define PA_MAX_HW       0x0E00      /* halfwords a region (7 rows of 608 B) */
#define PA_FILL(i)      (0xF000u | ((i) & 0xFFF))
#define PA_DATA(i)      (0x1000u + ((i) * 7 & 0x7FF))

static int algn_bcm(void)
{
    uint32_t hdr[ALGN_HDR], v[16], took = 0, k, n, i, step = 0;
    char tag[5];
    int err, j;

    err = command(ALGN_CMD, ALGN_T_MS, &took);
    if (err == E_TIMEOUT || err == E_FAULT)
    {
        rd(ALGN_BLOCK + 4 * 3, &step);
        for (j = 0; j < 4; j++)
            tag[j] = '?';
        tag[4] = '\0';
        if (step < ALGN_MAXN && rd(ALGN_BLOCK + 4 * (ALGN_HDR + 16 * step), &v[0]) == OK)
            for (j = 0; j < 4; j++)
            {
                char c = (char)(v[0] >> (24 - 8 * j));
                tag[j] = (c > ' ' && c < 0x7F) ? c : '?';
            }
        say("ALGN stopped at check %lu (%s): %s; checks after it did not run",
            (unsigned long)step, tag, err == E_FAULT ? "the image stopped (a fault "
            "the hook could not catch)" : "no answer in 2 s (hung in it)");
        if (err == E_FAULT)
            log_fault();
        return err;
    }
    if (err)
        return fail("ALGN command", err);
    for (i = 0; i < ALGN_HDR; i++)
        if ((err = rd(ALGN_BLOCK + 4 * i, &hdr[i])))
            return fail("ALGN header", err);
    n = hdr[1] & 0xFF;
    if (hdr[0] != ALGN_MAGIC || ((hdr[1] >> 8) & 0xFF) != 16 || n > ALGN_MAXN ||
        hdr[2] != 2)
    {
        say("ALGN none: words %08lx %08lx %08lx", (unsigned long)hdr[0],
            (unsigned long)hdr[1], (unsigned long)hdr[2]);
        return OK;
    }
    say("ALGN n %lu faults %lu src %08lx buf %08lx in %lu us", (unsigned long)n,
        (unsigned long)hdr[4], (unsigned long)hdr[5], (unsigned long)hdr[6],
        (unsigned long)took);
    for (k = 0; k < n; k++)
    {
        for (i = 0; i < 16; i++)
            if ((err = rd(ALGN_BLOCK + 4 * (ALGN_HDR + 16 * k + i), &v[i])))
                return fail("ALGN records", err);
        for (j = 0; j < 4; j++)
        {
            char c = (char)(v[0] >> (24 - 8 * j));
            tag[j] = (c > ' ' && c < 0x7F) ? c : '?';
        }
        tag[4] = '\0';
        say("ALGN %lu %s %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
            (unsigned long)k, tag, (unsigned long)v[1], (unsigned long)v[2],
            (unsigned long)v[3], (unsigned long)v[4], (unsigned long)v[5],
            (unsigned long)v[6], (unsigned long)v[7]);
        say("ALGNB %lu %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
            (unsigned long)k, (unsigned long)v[8], (unsigned long)v[9],
            (unsigned long)v[10], (unsigned long)v[11], (unsigned long)v[12],
            (unsigned long)v[13], (unsigned long)v[14], (unsigned long)v[15]);
    }
    return OK;
}

/* Part 2.  Kinds: one write; a short write then a word elsewhere and a
 * block after (does a lone halfword leak into what follows?); 7 rows of
 * 301 pixels at a pitch (engine 2 for the words, then the tail); engine
 * 2 then at once another address and a word; a rectangle at x 1 by
 * read-modify-write ends (the rule R-A1/R-A3 writer) */
enum { PA_ONE, PA_THEN, PA_ROWS, PA_E2X, PA_RECT };

static const struct pa_case
{
    char tag[5];
    uint8_t kind;
    uint8_t how;
    uint16_t off;           /* bytes into the region (ROWS, RECT: 0) */
    uint16_t n;             /* halfwords; ROWS: the pitch in bytes; RECT: w */
} pa_cases[] =
{
    /* ALGN1: 64 words at +0, +2, +4, by words and by halfwords */
    { "PWD0", PA_ONE, 0, 0x20, 128 },
    { "PWD2", PA_ONE, 0, 0x22, 128 },
    { "PWD4", PA_ONE, 0, 0x24, 128 },
    { "PHD2", PA_ONE, BCMDT_PA_DATA16, 0x22, 128 },
    { "PAD2", PA_ONE, BCMDT_PA_DATA16 | BCMDT_PA_ADDR16 | BCMDT_PA_NOPOLL, 0x22, 128 },
    /* ALGN6: 16 bytes at +4, +8, +12 */
    { "P164", PA_ONE, 0, 0x24, 8 },
    { "P168", PA_ONE, 0, 0x28, 8 },
    { "P16C", PA_ONE, 0, 0x2C, 8 },
    /* ALGN2: 1, 3, 5 halfwords at an aligned address, by halfword
     * stores, then a word elsewhere and a block */
    { "PLH1", PA_THEN, BCMDT_PA_DATA16, 0x20, 1 },
    { "PLH3", PA_THEN, BCMDT_PA_DATA16, 0x20, 3 },
    { "PLH5", PA_THEN, BCMDT_PA_DATA16, 0x20, 5 },
    { "PLW3", PA_THEN, 0, 0x20, 3 },        /* ours: a word tail */
    /* ALGN3: 301 x 7 by engine 2 and a halfword tail, three pitches;
     * then 604 with our word tail */
    { "R602", PA_ROWS, BCMDT_PA_E2 | BCMDT_PA_DATA16, 0, 602 },
    { "R604", PA_ROWS, BCMDT_PA_E2 | BCMDT_PA_DATA16, 0, 604 },
    { "R608", PA_ROWS, BCMDT_PA_E2 | BCMDT_PA_DATA16, 0, 608 },
    { "R64W", PA_ROWS, BCMDT_PA_E2, 0, 604 },
    /* ALGN4: engine 2, then at once a new address and a word (no poll),
     * after the engine's end only, or after the port's write-ready too */
    { "E2ND", PA_E2X, BCMDT_PA_E2 | BCMDT_PA_NODRAIN, 0x20, 128 },
    { "E2DR", PA_E2X, BCMDT_PA_E2, 0x20, 128 },
    /* ALGN5: rectangles at x 1, w 3 and w 301, three rows at pitch 640 */
    { "RX13", PA_RECT, 0, 0, 3 },
    { "RX1W", PA_RECT, 0, 0, 301 },
};

static uint16_t pa_shadow[PA_MAX_HW] __attribute__((aligned(4)));
static uint16_t pa_src[PA_MAX_HW / 4] __attribute__((aligned(4)));

/* the shadow and the BCM region to the fill, by aligned word writes */
static int pa_fill(uint32_t base, uint32_t hw)
{
    uint32_t i;

    for (i = 0; i < hw; i++)
        pa_shadow[i] = PA_FILL(i);
    return h->wr_block(base, pa_shadow, hw) < 0 ? E_PORT : OK;
}

/* n halfwords of data (seed s) at byte offset off: the port's write in
 * form how, and the shadow as an exact port would leave it */
static int pa_write(uint32_t base, uint32_t off, uint32_t n, unsigned how,
                    uint32_t s)
{
    uint32_t i;

    for (i = 0; i < n; i++)
    {
        pa_src[i] = PA_DATA(s + i);
        pa_shadow[off / 2 + i] = pa_src[i];
    }
    /* our word tail writes a zero high half past the end */
    if ((n & 1) && !(how & BCMDT_PA_DATA16))
        pa_shadow[off / 2 + n] = 0;
    return h->port_algn(base + off, pa_src, n, how) < 0 ? E_PORT : OK;
}

/* A rectangle's row by the rule writer: whole aligned words only, the
 * ends merged by read-modify-write (R-A1, R-A3) */
static int pa_rmw_row(uint32_t base, uint32_t off, uint32_t w, uint32_t s)
{
    uint32_t a = off & ~3u, end = off + 2 * w, i, v;
    int err;

    for (i = 0; i < w; i++)
        pa_shadow[off / 2 + i] = PA_DATA(s + i);
    for (; a < end; a += 4)
    {
        if (a < off || a + 4 > end)
        {
            if ((err = rd(base + a, &v)))
                return err;
            if (a >= off)
                v = (v & 0xFFFF0000u) | pa_shadow[a / 2];
            if (a + 4 <= end)
                v = (v & 0xFFFFu) | (uint32_t)pa_shadow[a / 2 + 1] << 16;
        }
        else
            v = pa_shadow[a / 2] | (uint32_t)pa_shadow[a / 2 + 1] << 16;
        if (h->wr(base + a, v) < 0)
            return E_PORT;
    }
    return OK;
}

/* Every halfword of the region back, against the shadow */
static int pa_check(int k, const char *tag, uint32_t base, uint32_t hw)
{
    uint32_t i, w = 0, bad = 0, low2 = 0;
    char line[96];
    int n = 0, err;

    line[0] = '\0';
    for (i = 0; i < hw; i++)
    {
        uint16_t got;

        if (!(i & 1) && (err = rd(base + 2 * i, &w)))
            return err;
        got = i & 1 ? w >> 16 : w & 0xFFFF;
        if (got == pa_shadow[i])
            continue;
        bad++;
        if (i + 1 < hw && got == pa_shadow[i + 1])
            low2++;
        if (bad <= 3)
            n += snprintf(line + n, sizeof(line) - n, " +%lx %04x %04x",
                          (unsigned long)(2 * i), got, pa_shadow[i]);
    }
    say("ALGNP %d %s bad %lu low2 %lu of %lu%s", k, tag, (unsigned long)bad,
        (unsigned long)low2, (unsigned long)hw, line);
    return OK;
}

static int algn_port(void)
{
    int k, err = OK;

    if (!h->port_algn)
    {
        say("ALGNP none: no port_algn in this build");
        return OK;
    }
    for (k = 0; k < (int)(sizeof(pa_cases) / sizeof(pa_cases[0])); k++)
    {
        const struct pa_case *c = &pa_cases[k];
        uint32_t base = PA_BASE + PA_STEP * k, hw, y;

        switch (c->kind)
        {
        case PA_ONE:
            hw = (c->off + 2 * c->n + 32) / 2 & ~1u;
            if (!(err = pa_fill(base, hw)))
                err = pa_write(base, c->off, c->n, c->how, 0);
            break;
        case PA_THEN:
            hw = 0xC0 / 2;
            if (!(err = pa_fill(base, hw)) &&
                !(err = pa_write(base, c->off, c->n, c->how, 0)) &&
                !(err = pa_write(base, 0x40, 2, 0, 100)))
                err = pa_write(base, 0x80, 8, 0, 200);
            break;
        case PA_ROWS:
            hw = (7 * c->n + 32) / 2 & ~1u;
            err = pa_fill(base, hw);
            for (y = 0; y < 7 && !err; y++)
                err = pa_write(base, y * c->n, 301, c->how, 301 * y);
            break;
        case PA_E2X:
            hw = 0x240 / 2;
            if (!(err = pa_fill(base, hw)) &&
                !(err = pa_write(base, c->off, c->n, c->how, 0)))
                err = pa_write(base, 0x200, 2, BCMDT_PA_NOPOLL, 500);
            break;
        default:        /* PA_RECT */
            hw = (3 * 640 + 32) / 2;
            err = pa_fill(base, hw);
            for (y = 0; y < 3 && !err; y++)
                err = pa_rmw_row(base, 640 * y + 2, c->n, c->n * y);
            break;
        }
        if (!err)
            err = pa_check(k, c->tag, base, hw);
        if (err)
        {
            say("ALGNP %d %s: port error %d", k, c->tag, err);
            if (err == E_PORT)
                return err;
        }
    }
    return OK;
}

/* Both parts; never fails the stage.  Part 2 runs even if part 1 hung
 * (the port writes SRAM without the core) */
static void bench_algn(void)
{
    say("ALGN: alignment checks (bench row ALGN)");
    algn_bcm();
    algn_port();
}

static int bench_stage(bool pll, bool *alive)
{
    const struct bcmdt_image *img = pll ? h->bench_pll : h->bench;
    const char *name = pll ? "stage 4b" : "stage 4";
    uint32_t hdr[BENCH_HDR], base = 0, i, ok2 = 0;
    uint32_t a1_insns = 0, a1 = 0, a2_insns = 0, a2 = 0;
    int err;

    say("%s: timing bench%s (no panel I/O)", name,
        pll ? ", then the 150 MHz switch" : "");
    if ((err = boot(img, &res->ready_us)))
        return err;
    *alive = true;
    if ((err = bench_wait(pll ? T_BENCH_PLL : T_BENCH_TOTAL)))
    {
        if (err != E_PORT)
            *alive = false;     /* stopped or stuck: no more commands */
        return err;
    }
    say("bench done in %lu ms", (unsigned long)(res->b_us / 1000));
    if ((err = rd(A_BENCH, &base)))
        return fail("bench address", err);
    if (base != BENCH_RESULTS)
        say("bench: results at %08lx, not %08lx", (unsigned long)base,
            (unsigned long)BENCH_RESULTS);
    if (base < 0x200 || base > 0x100000 - 4 * BENCH_HDR || (base & 3))
        return fail("bench results address", E_STATUS);
    for (i = 0; i < BENCH_HDR; i++)
        if ((err = rd(base + 4 * i, &hdr[i])))
            return fail("bench header", err);
    /* B4H first-word w0 .. w7: the header as it is (bench.h) */
    for (i = 0; i < BENCH_HDR; i += 8)
        say("B4H %lu %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
            (unsigned long)i, (unsigned long)hdr[i], (unsigned long)hdr[i + 1],
            (unsigned long)hdr[i + 2], (unsigned long)hdr[i + 3],
            (unsigned long)hdr[i + 4], (unsigned long)hdr[i + 5],
            (unsigned long)hdr[i + 6], (unsigned long)hdr[i + 7]);
    res->b_flags = hdr[5];
    res->b_vpass = hdr[6];
    res->b_vall = hdr[7];
    res->b_spass = hdr[8];
    res->b_sall = hdr[9];
    res->b_ppass = hdr[10];
    res->b_pall = hdr[11];
    res->b_nk = hdr[2];
    say("bench checks: vector %lx/%lx (%d/%d), scalar %lx/%lx, probe %lx/%lx",
        (unsigned long)hdr[6], (unsigned long)hdr[7],
        popcount32(hdr[6] & hdr[7]), popcount32(hdr[7]), (unsigned long)hdr[8],
        (unsigned long)hdr[9], (unsigned long)hdr[10], (unsigned long)hdr[11]);
    say("bench first mismatch %08lx got %08lx want %08lx",
        (unsigned long)hdr[12], (unsigned long)hdr[13], (unsigned long)hdr[14]);
    if ((err = bench_vrecs(base, hdr)) || (err = bench_slocs(base, hdr)))
        return err;
    if (hdr[0] != BENCH_MAGIC || hdr[1] != 2 || hdr[2] == 0 ||
        hdr[2] > BENCH_MAX_K || hdr[20] != BENCH_HDR || hdr[22] != BENCH_EW ||
        !(hdr[5] & BRF_PLL_BUILD) != !pll ||
        (pll && hdr[21] != BENCH_HDR + hdr[2] * BENCH_EW) || (!pll && hdr[21]) ||
        base + 4 * (BENCH_HDR + (pll ? 2 : 1) * hdr[2] * BENCH_EW) > 0x100000)
        return fail("bench results header", E_STATUS);
    if ((err = bench_table(base + 4 * hdr[20], 1, hdr[2], &res->b_kernels_ok,
                           &a1_insns, &a1)))
        return err;
    if (pll)
    {
        if ((err = bench_table(base + 4 * hdr[21], 2, hdr[2], &ok2, &a2_insns,
                               &a2)))
            return err;
        say("150 MHz switch: flags %lx; regulator %08lx -> %08lx",
            (unsigned long)hdr[5], (unsigned long)hdr[15],
            (unsigned long)hdr[16]);
        say("150 MHz switch: PLL A %08lx %08lx, clock select %08lx",
            (unsigned long)hdr[17], (unsigned long)hdr[18],
            (unsigned long)hdr[19]);
    }
    res->b_clock_khz = khz(a1_insns, a1);
    say("B4R clock %lu kHz (ALU1: %lu instructions in %lu us, if one per cycle)",
        (unsigned long)res->b_clock_khz, (unsigned long)a1_insns,
        (unsigned long)a1);
    if (pll)
    {
        res->b_clock2_khz = khz(a2_insns, a2);
        res->b_start_khz = a1 ? (uint32_t)(150000ull * a2 / a1) : 0;
        say("B4R start-up clock %lu kHz (150 MHz x %lu / %lu us); ALU1 at "
            "150 MHz %lu kHz", (unsigned long)res->b_start_khz,
            (unsigned long)a2, (unsigned long)a1,
            (unsigned long)res->b_clock2_khz);
    }
    /* vector checks 32-63: header words 27 (pass) and 28 (all) */
    if (hdr[7] == 0 || hdr[6] != hdr[7] || hdr[27] != hdr[28] ||
        hdr[9] == 0 || hdr[8] != hdr[9] ||
        hdr[11] == 0 || hdr[10] != hdr[11])
        return fail("bench checks", E_STATUS);
    if (!(hdr[5] & BRF_TIMER_OK) || res->b_kernels_ok != hdr[2] || !a1)
        return fail("bench timing (counter or entries)", E_STATUS);
    if (pll && ((hdr[5] & (BRF_PLL_DONE | BRF_PLL_STALL)) != BRF_PLL_DONE ||
                ok2 != hdr[2] || !a2))
        return fail("150 MHz switch (flags)", E_STATUS);
    if (!pll)
        res->img4 = img;
    else
        res->img4b = img;       /* dev9: 4b's own flag gates 4c and video */
    say("%s PASS", name);
    return OK;
}

/* ---- The HAVE_BCMFW boot path ---------------------------------------- */

int bcmdt_boot_swap(const struct bcmdt_hal *hal, const struct bcmdt_image *img,
                    struct bcmdt_result *r)
{
    bool alive = false;
    uint32_t n = 0;
    int err;

    h = hal;
    res = r;
    cur_img = NULL;
    memset(r, 0, sizeof(*r));
    r->passed = -1;
    r->panel_id = -1;
    t_start = h->now_us();
    say("=== boot path: %s ===", img ? img->name : "?");
    /* Apple's view of the panel ID, to check ours against */
    if ((err = rd(A_GPIO_LEV1, &r->gpio_lev)))
    {
        fail("panel ID read", err);
        bcm_off(false);
        return err;
    }
    r->panel_id = ((r->gpio_lev >> 19) & 1) + 2 * ((r->gpio_lev >> 18) & 1);
    bcm_off(true);
    if ((err = boot(img, &r->ready_us)))
        goto out;
    alive = true;
    if (h->upload_fb() < 0)
    {
        err = fail("frame upload", E_PORT);
        goto out;
    }
    if ((err = command(LCD_UPDATE, T_POWERON, &r->first_us)))
    {
        if (err == E_FAULT || err == E_TIMEOUT)
        {
            alive = false;
            log_fault();
        }
        fail("first frame", err);
        goto out;
    }
    if ((err = rd(A_FRAMES, &n)) || (err = rd(A_INFO, &r->info)))
    {
        fail("first frame mailbox", err);
        goto out;
    }
    if (n != 1 || !(r->info & INFO_PANEL_ON))
    {
        err = fail("first frame count / panel state", E_STATUS);
        goto out;
    }
    if (r->info & INFO_DMA)
        dma_read("first frame", &r->dma_bcm_us);
    h->light(true);
    say("first frame confirmed in %lu us", (unsigned long)r->first_us);
    return OK;

out:
    if (err == E_PORT)
        alive = false;
    bcm_off(alive);
    return err;
}

#ifdef BCMDT_LADDER
/* The next rung (#62 level 1; the resident video image's), from a BCM
 * that a failed rung left powered off.  bcmdt_boot_swap without its first half (Apple's panel ID, the
 * stock power-down): the ID stays the first rung's. */
static int boot_rung(const struct bcmdt_image *img, struct bcmdt_result *r)
{
    bool alive = false;
    uint32_t n = 0;
    int err;

    say("=== boot path: next rung: %s ===", img ? img->name : "?");
    if ((err = boot(img, &r->ready_us)))
        goto out;
    alive = true;
    if (h->upload_fb() < 0)
    {
        err = fail("frame upload", E_PORT);
        goto out;
    }
    if ((err = command(LCD_UPDATE, T_POWERON, &r->first_us)))
    {
        if (err == E_FAULT || err == E_TIMEOUT)
        {
            alive = false;
            log_fault();
        }
        fail("first frame", err);
        goto out;
    }
    if ((err = rd(A_FRAMES, &n)) || (err = rd(A_INFO, &r->info)))
    {
        fail("first frame mailbox", err);
        goto out;
    }
    if (n != 1 || !(r->info & INFO_PANEL_ON))
    {
        err = fail("first frame count / panel state", E_STATUS);
        goto out;
    }
    if (r->info & INFO_DMA)
        dma_read("first frame", &r->dma_bcm_us);
    h->light(true);
    say("first frame confirmed in %lu us", (unsigned long)r->first_us);
    return OK;

out:
    if (err == E_PORT)
        alive = false;
    bcm_off(alive);
    return err;
}

int bcmdt_boot_ladder(const struct bcmdt_hal *hal,
                      const struct bcmdt_image *const *imgs, int n,
                      struct bcmdt_result *r)
{
    int k, err = E_STATUS, panel = -1;
    uint32_t lev = 0;

    for (k = 0; k < n; k++)
    {
        if (k == 0)
        {
            err = bcmdt_boot_swap(hal, imgs[0], r);
            panel = r->panel_id;
            lev = r->gpio_lev;
        }
        else if (imgs[k])
        {
            /* the failed rung left the BCM off: start this one cold */
            t_start = h->now_us();
            memset(r, 0, sizeof(*r));
            r->passed = -1;
            r->panel_id = panel;
            r->gpio_lev = lev;
            cur_img = NULL;
            err = boot_rung(imgs[k], r);
        }
        else
            continue;
        if (err == OK)
            return k;
        if (k + 1 < n && imgs[k + 1])
            say("boot path: %s failed (%s); next rung", imgs[k]->name,
                r->why);
    }
    return -1;
}
#endif /* BCMDT_LADDER */

#ifdef HAVE_BCMFW_VIDEO
/* ---- The video player's image (apps/video/vpcore.c) ------------------ */

/* Larger than the test images: the decoder's code and tables (pipeline.md;
 * the image region is 0x200-0x7FFFF) */
#define VIDEO_IMG_MAX   BCMDT_IMG_MAX

int bcmdt_video_boot(const struct bcmdt_hal *hal, const uint8_t *img,
                     uint32_t bytes, bool running)
{
    static struct bcmdt_result r;
    uint32_t t0, st = 0, n = 0;
    int err;

    h = hal;
    res = &r;
    cur_img = NULL;
    memset(&r, 0, sizeof(r));
    r.passed = -1;
    r.panel_id = -1;
    t_start = h->now_us();
    say("=== video image: %lu bytes, fnv %08lx ===", (unsigned long)bytes,
        (unsigned long)(img ? bcmdt_fnv(img, bytes) : 0));
    if (!img || bytes < 0x200 || bytes > VIDEO_IMG_MAX || (bytes & 3))
        return fail("video image check", E_STATUS);
    /* whatever runs (Apple's vmcs, our boot-path image) stops in the
     * stock order; after a cut (the display went dark) it's off already */
    if (running)
        bcm_off(true);
    if (h->boot(img, bytes) < 0)
        return fail("bootstrap/upload", E_PORT);
    t0 = h->now_us();
    for (;;)
    {
        if ((err = rd(A_COMMAND, &st)))
            return fail("ready", err);
        if (st != 0)
            break;
        if (h->now_us() - t0 > T_READY * 1000)
        {
            err = fail("ready", E_TIMEOUT);
            goto out;
        }
        h->yield();
    }
    if (st != ST_DONE)
    {
        err = fail(st == ST_FAULT ? "ready (fault)" : "ready value",
                   st == ST_FAULT ? E_FAULT : E_STATUS);
        goto out;
    }
    if ((err = rd(A_INFO, &r.info)) || (r.info >> 16) != INFO_MAGIC)
    {
        err = fail("info word", err ? err : E_STATUS);
        goto out;
    }
    /* the panel comes on with the first frame: today's screen, until the
     * session takes the display over */
    if (h->upload_fb() < 0)
    {
        err = fail("frame upload", E_PORT);
        goto out;
    }
    if ((err = command(LCD_UPDATE, T_POWERON, &r.first_us)))
    {
        fail("first frame", err);
        goto out;
    }
    if ((err = rd(A_FRAMES, &n)) || !n)
    {
        err = fail("first frame count", err ? err : E_STATUS);
        goto out;
    }
    h->light(true);
    say("video image up: first frame in %lu us", (unsigned long)r.first_us);
    return OK;

out:
    h->power_cut();
    return err;
}

void bcmdt_video_off(const struct bcmdt_hal *hal, bool alive)
{
    h = hal;
    bcm_off(alive);
}
#endif /* HAVE_BCMFW_VIDEO */

/* ---- PW5: Apple's hibernate pins (bcm-devtest.h) ---------------------- */

/* 0x1F0 until it reads want, a port error or the deadline: 0 when seen;
 * *last the last value read (untouched if none) */
static int hib_until(const struct bcmdt_hib_hal *hh, uint32_t want,
                     uint32_t ms, uint32_t *last, uint32_t *took)
{
    uint32_t t0 = hh->now_us(), v;

    for (;;)
    {
        if (hh->rd(BCMDT_HIB_WORD, &v) < 0)
            return -1;
        *last = v;
        if (v == want)
        {
            if (took)
                *took = hh->now_us() - t0;
            return 0;
        }
        if (hh->now_us() - t0 > ms * 1000)
            return -1;
        hh->yield();
    }
}

/* The standard cut: bit 15 low, then bit 14 low, then the wait */
static int hib_cut(const struct bcmdt_hib_hal *hh, struct bcmdt_hib_result *r,
                   int end)
{
    hh->gpo(BCMDT_GPO_RETAIN, 0);
    r->gpo_end = hh->gpo(BCMDT_GPO_RUN, 0) & (BCMDT_GPO_RUN | BCMDT_GPO_RETAIN);
    hh->sleep_ms(BCMDT_HIB_OFF_MS);
    r->cut = true;
    return r->end = end;
}

int bcmdt_hib_cycle(const struct bcmdt_hib_hal *hh, uint32_t hold_ms,
                    struct bcmdt_hib_result *r)
{
    const uint32_t both = BCMDT_GPO_RUN | BCMDT_GPO_RETAIN;
    uint32_t t0, t_on, v = 0, deadline;
    int rc;

    memset(r, 0, sizeof(*r));
    r->en0 = hh->gpo_enable();
    r->gpo0 = hh->gpo(0, 0);
    r->gpo_end = r->gpo0 & both;
    /* the pins as Rockbox leaves a running BCM: outputs, bit 14 high,
     * bit 15 low (never set bit 15 from a state with bit 14 low) */
    if ((r->en0 & both) != both || (r->gpo0 & both) != BCMDT_GPO_RUN)
        return r->end = BCMDT_HIB_REFUSED;
    if (hh->wr(BCMDT_HIB_WORD, 0) < 0 || hh->send(hh->ctx) < 0)
        return r->end = BCMDT_HIB_E_SEND;
    if (hib_until(hh, BCMDT_HIB_PARKED, BCMDT_HIB_PARK_MS, &v, &r->park_us))
    {
        r->word_after = v;
        return r->end = BCMDT_HIB_E_PARK;
    }

    /* retain: bit 15 high, 5 ms, bit 14 low; then nothing on the port */
    r->pins = true;
    hh->gpo(0, BCMDT_GPO_RETAIN);
    r->both_us[0] = hh->now_us();
    hh->udelay(BCMDT_HIB_PIN_US);
    hh->gpo(BCMDT_GPO_RUN, 0);
    r->both_us[0] = hh->now_us() - r->both_us[0];
    t0 = hh->now_us();
    hh->sleep_ms(hold_ms);

    /* on: bit 14 high, 5 ms, bit 15 low */
    hh->gpo(0, BCMDT_GPO_RUN);
    t_on = hh->now_us();
    r->hold_us = t_on - t0;
    hh->udelay(BCMDT_HIB_PIN_US);
    hh->gpo(BCMDT_GPO_RETAIN, 0);
    r->both_us[1] = hh->now_us() - t_on;

    /* the word: still parked means SRAM and the parked core held */
    deadline = hh->now_us();
    for (;;)
    {
        rc = hh->rd(BCMDT_HIB_WORD, &v);
        if (rc >= 0)
            break;
        if (hh->now_us() - deadline > BCMDT_HIB_WAKE_MS * 1000)
            return hib_cut(hh, r, BCMDT_HIB_E_DEAD);
        hh->yield();
    }
    r->word_after = v;
    if (v == 0xFFFFFFFFu)
        return hib_cut(hh, r, BCMDT_HIB_E_DEAD);
    if (v != BCMDT_HIB_PARKED)
        return hib_cut(hh, r, BCMDT_HIB_E_LOST);
    t0 = hh->now_us();
    if (hh->wr(BCMDT_HIB_WORD, BCMDT_HIB_WAKE) < 0 ||
        hib_until(hh, BCMDT_HIB_AWAKE, BCMDT_HIB_WAKE_MS, &v, NULL))
        return hib_cut(hh, r, BCMDT_HIB_E_WAKE);
    r->answer_us = hh->now_us() - t0;
    r->wake_us = hh->now_us() - t_on;
    r->gpo_end = hh->gpo(0, 0) & both;
    return r->end = BCMDT_HIB_OK;
}

/* ---- Pass flags ------------------------------------------------------- */

int bcmdt_strike_next(int on_disk, int real, int deliberate)
{
    (void)deliberate;           /* asked for: never a strike */
    if (real <= 0)
        return -1;
    if (on_disk < 0)
        on_disk = 0;
    return on_disk + 1 > 9 ? 9 : on_disk + 1;
}

bool bcmdt_strike_blocks(int on_disk)
{
    return on_disk >= BCMDT_STRIKES;
}

/* p19-bcm-release: the resident rung's gate (bcm-devtest.h) */
const char *bcmdt_res_gate(bool setting, bool offered, bool failed,
                           bool need_verified, bool verified)
{
    if (!setting)
        return "setting off";
    if (!offered)
        return "no image offered (missing, or not this build's)";
    if (failed)
        return "skipped: it failed at a boot";
    if (need_verified && !verified)
        return "not in verified.txt";
    return NULL;
}

/* p19-bcm-release: the once-a-build re-arm (bcm-devtest.h).  stored may
 * end in a newline (the file's); an empty build ID never re-arms. */
bool bcmdt_rearm_due(const char *stored, const char *build)
{
    size_t n;

    if (!build || !build[0])
        return false;
    if (!stored)
        return true;
    n = strlen(build);
    return strncmp(stored, build, n) ||
           (stored[n] != '\0' && stored[n] != '\n');
}

/* A flag's stage tag: "2".."5", or "4b"/"4c" for codes 41/42 */
static void flag_tag(int stage, char tag[4])
{
    if (stage >= 10)
        snprintf(tag, 4, "%d%c", stage / 10, 'a' + stage % 10);
    else
        snprintf(tag, 4, "%d", stage);
}

int bcmdt_flag_format(char *buf, int len, int stage,
                      const struct bcmdt_image *img)
{
    char tag[4];

    flag_tag(stage, tag);
    return snprintf(buf, len, "%s %s %08lx %08lx\n", tag, img->name,
                    (unsigned long)img->size, (unsigned long)img->fnv);
}

static const char *hex32(const char *p, uint32_t *v)
{
    int k;

    *v = 0;
    for (k = 0; k < 8; k++, p++)
    {
        if (*p >= '0' && *p <= '9')
            *v = *v << 4 | (uint32_t)(*p - '0');
        else if (*p >= 'a' && *p <= 'f')
            *v = *v << 4 | (uint32_t)(*p - 'a' + 10);
        else
            return NULL;
    }
    return p;
}

const struct bcmdt_image *bcmdt_flag_match(const char *t, int stage,
                                           const struct bcmdt_image *const *imgs,
                                           int n, const char **why)
{
    char name[16], tag[4];
    uint32_t size, fnv;
    int k, tn;

    flag_tag(stage, tag);
    tn = (int)strlen(tag);
    if (!t || strncmp(t, tag, tn) || t[tn] != ' ')
    {
        *why = "not this stage's flag";
        return NULL;
    }
    t += tn + 1;
    for (k = 0; k < (int)sizeof(name) - 1 && *t && *t != ' '; k++)
        name[k] = *t++;
    name[k] = '\0';
    if (*t++ != ' ' || !(t = hex32(t, &size)) || *t++ != ' ' ||
        !(t = hex32(t, &fnv)) || (*t != '\n' && *t != '\0'))
    {
        *why = "unreadable";
        return NULL;
    }
    for (k = 0; k < n; k++)
    {
        const struct bcmdt_image *img = imgs[k];

        if (!img || strcmp(img->name, name))
            continue;
        if (img->size != size || img->fnv != fnv ||
            bcmdt_fnv(img->data, img->size) != fnv)
        {
            *why = "void: the embedded image differs from the one that passed";
            return NULL;
        }
        *why = "ok";
        return img;
    }
    *why = "names no image of this stage";
    return NULL;
}

/* The stage each one needs to have passed earlier in the same run */
static int needs(int s)
{
    return s == 4 ? 1 : s == BCMDT_STAGE_4B ? 0 : s == BCMDT_STAGE_5 ? 3 :
           s == BCMDT_STAGE_4C ? 1 :
           s == BCMDT_STAGE_5B || s == BCMDT_STAGE_5I ||
           s == BCMDT_STAGE_5S || s == BCMDT_STAGE_3D ? 0 : s - 1;
}

/* Whether stage s may run now: what it needs passed earlier in this run;
 * 4c also needs 4b if this run asked for it (else its flag let it in),
 * 5b needs stage 5 if asked for (else its flag), not stage 3 */
static bool dep_ok(int s, unsigned stages, unsigned mask)
{
    if (s == BCMDT_STAGE_5B || s == BCMDT_STAGE_5I || s == BCMDT_STAGE_5S ||
        s == BCMDT_STAGE_3D)
        return !(stages & BCMDT_STAGE(BCMDT_STAGE_5)) ||
               (mask & BCMDT_STAGE(BCMDT_STAGE_5));
    if (s == BCMDT_STAGE_4C && (stages & BCMDT_STAGE(BCMDT_STAGE_4B)) &&
        !(mask & BCMDT_STAGE(BCMDT_STAGE_4B)))
        return false;
    return (mask & BCMDT_STAGE(needs(s))) != 0;
}

/* The line of flags (several pass flags, one a line) that starts with
 * this stage's tag, or "" */
static const char *flag_line(const char *flags, int stage, char *buf, int len)
{
    char tag[4];
    int n, k;

    flag_tag(stage, tag);
    n = (int)strlen(tag);
    for (; flags && *flags; flags = strchr(flags, '\n') ? strchr(flags, '\n') + 1 : "")
    {
        if (!strncmp(flags, tag, n) && flags[n] == ' ')
        {
            for (k = 0; k < len - 1 && flags[k] && flags[k] != '\n'; k++)
                buf[k] = flags[k];
            buf[k] = '\0';
            return buf;
        }
    }
    buf[0] = '\0';
    return buf;
}

/* A gated stage's precondition: a pass flag (in flags) naming img */
static bool gate_ok(const char *flags, int code, const struct bcmdt_image *img,
                    const char *name, struct bcmdt_result *r)
{
    const struct bcmdt_image *const one[1] = { img };
    const char *why = "no pass flag";
    char line[48];

    flag_line(flags, code, line, sizeof(line));
    if (!line[0] || !bcmdt_flag_match(line, code, one, 1, &why))
    {
        snprintf(r->why, sizeof(r->why), "stage %s refused: %s", name, why);
        say("FAIL %s; nothing done", r->why);
        return false;
    }
    say("stage %s allowed: %s %08lx passed", name, img->name,
        (unsigned long)img->fnv);
    return true;
}

/* The highest s (0..4) such that every requested stage up to s passed */
static int passed_through(unsigned stages, unsigned mask)
{
    int s, top = -1;

    for (s = 0; s <= BCMDT_LAST_STAGE; s++)
    {
        if (!(stages & BCMDT_STAGE(s)))
            continue;
        if (!(mask & BCMDT_STAGE(s)))
            break;
        top = s;
    }
    return top;
}

int bcmdt_run_stages(const struct bcmdt_hal *hal, unsigned stages,
                     const char *flags, struct bcmdt_result *r)
{
    static const char *const names[] = { "0", "1", "2", "3", "4", "4b", "5",
                                         "4c", "5b", "5i", "5s", "3d" };
    /* run order: 4c after 4b, 5b and 5i after 5, 5s after 5i, 3d last */
    static const int order[] = { 1, 2, 3, 4, BCMDT_STAGE_4B, BCMDT_STAGE_4C,
                                 BCMDT_STAGE_5, BCMDT_STAGE_5B, BCMDT_STAGE_5I,
                                 BCMDT_STAGE_5S, BCMDT_STAGE_3D };
    bool alive, taken, sleep_5s = false, fast_3d = false;
    uint32_t t0;
    int s, k, err = OK;

    h = hal;
    res = r;
    cur_img = NULL;
    memset(r, 0, sizeof(*r));
    r->passed = -1;
    r->panel_id = -1;
    t_start = h->now_us();
    stages |= BCMDT_STAGE(0);
    stages &= BCMDT_STAGE(BCMDT_STAGE_3D + 1) - 1;
    say("=== BCM test, stages %s%s%s%s%s%s%s%s%s%s%s%s ===", "0",
        stages & BCMDT_STAGE(1) ? " 1" : "", stages & BCMDT_STAGE(2) ? " 2" : "",
        stages & BCMDT_STAGE(3) ? " 3" : "", stages & BCMDT_STAGE(4) ? " 4" : "",
        stages & BCMDT_STAGE(BCMDT_STAGE_4B) ? " 4b" : "",
        stages & BCMDT_STAGE(BCMDT_STAGE_4C) ? " 4c" : "",
        stages & BCMDT_STAGE(BCMDT_STAGE_5) ? " 5" : "",
        stages & BCMDT_STAGE(BCMDT_STAGE_5B) ? " 5b" : "",
        stages & BCMDT_STAGE(BCMDT_STAGE_5I) ? " 5i" : "",
        stages & BCMDT_STAGE(BCMDT_STAGE_5S) ? " 5s" : "",
        stages & BCMDT_STAGE(BCMDT_STAGE_3D) ? " 3d" : "");

    /* the gated stages: 4b on a stage 4 flag, 4c on a 4b flag, 5b on a
     * stage 5 flag (or stage 5 in this run) */
    if ((stages & BCMDT_STAGE(BCMDT_STAGE_4B)) &&
        !gate_ok(flags, 4, h->bench, "4b", r))
        return -3;
    if ((stages & BCMDT_STAGE(BCMDT_STAGE_4C)) &&
        !(stages & BCMDT_STAGE(BCMDT_STAGE_4B)) &&
        !gate_ok(flags, BCMDT_FLAG_4B, h->bench_pll, "4c", r))
        return -3;
    if ((stages & BCMDT_STAGE(BCMDT_STAGE_5B)) &&
        !(stages & BCMDT_STAGE(BCMDT_STAGE_5)) &&
        !gate_ok(flags, 5, h->hello_dma, "5b", r))
        return -3;
    if (stages & BCMDT_STAGE(BCMDT_STAGE_5I))
    {
        if (!h->hello_dma_idle)
        {
            snprintf(r->why, sizeof(r->why), "stage 5i refused: no idle image");
            say("FAIL %s; nothing done", r->why);
            return -3;
        }
        if (!(stages & BCMDT_STAGE(BCMDT_STAGE_5)) &&
            !gate_ok(flags, 5, h->hello_dma, "5i", r))
            return -3;
    }

    if (stages & BCMDT_STAGE(BCMDT_STAGE_5S))
    {
        const struct bcmdt_image *const idle[1] = { h->hello_dma_idle };
        const char *why = "no pass flag";
        char line[48];

        if (!h->hello_dma_sby)
        {
            snprintf(r->why, sizeof(r->why), "stage 5s refused: no standby image");
            say("FAIL %s; nothing done", r->why);
            return -3;
        }
        if (!(stages & BCMDT_STAGE(BCMDT_STAGE_5)) &&
            !gate_ok(flags, 5, h->hello_dma, "5s", r))
            return -3;
        /* its image sleeps only where the tick is known to wake it */
        flag_line(flags, BCMDT_FLAG_5I, line, sizeof(line));
        sleep_5s = line[0] && h->hello_dma_idle &&
                   bcmdt_flag_match(line, BCMDT_FLAG_5I, idle, 1, &why);
        say("stage 5s: core sleep %s", sleep_5s ? "on (5i flag ok)" :
            "off unless 5i passes in this run");
        if (!sleep_5s)
            say("stage 5s: 5i flag: %s", why);
    }

    if (stages & BCMDT_STAGE(BCMDT_STAGE_3D))
    {
        const struct bcmdt_image *const b4[1] = { h->bench_pll };
        const struct bcmdt_image *const c4[1] = { h->hello_dma_clk };
        const char *why = "no pass flag";
        char line[48];

        if (!h->hello_3d)
        {
            snprintf(r->why, sizeof(r->why), "stage 3d refused: no 3D image");
            say("FAIL %s; nothing done", r->why);
            return -3;
        }
        if (!(stages & BCMDT_STAGE(BCMDT_STAGE_5)) &&
            !gate_ok(flags, 5, h->hello_dma, "3d", r))
            return -3;
        /* 150 MHz only where the switch and the panel on the PLL passed */
        flag_line(flags, BCMDT_FLAG_4B, line, sizeof(line));
        fast_3d = line[0] && h->bench_pll &&
                  bcmdt_flag_match(line, BCMDT_FLAG_4B, b4, 1, &why);
        if (fast_3d)
        {
            flag_line(flags, BCMDT_FLAG_4C, line, sizeof(line));
            fast_3d = line[0] && h->hello_dma_clk &&
                      bcmdt_flag_match(line, BCMDT_FLAG_4C, c4, 1, &why);
        }
        say("stage 3d: 150 MHz runs %s", fast_3d ? "on (4b, 4c flags ok)" : "off");
        if (!fast_3d)
            say("stage 3d: clock flags: %s", why);
    }

    if (h->take() < 0)
    {
        say("FAIL the display is busy or asleep; nothing done");
        return -2;
    }
    taken = true;
    err = stage0();
    if (err == OK)
        r->passed_mask |= BCMDT_STAGE(0);
    if (err == E_PORT || err == E_TIMEOUT)
    {
        /* Apple's vmcs is not answering: never hand a wedged chip back to
         * the stock (unbounded) driver; power-cycle it onto NOR vmcs */
        bcm_off(false);
        say("restore: Apple vmcs (stock wake path)");
        h->restore(false);
        say("restore: done");
        taken = false;
    }
    else if (err != OK || stages == BCMDT_STAGE(0))
    {
        h->restore(true);       /* Apple's vmcs was never stopped */
        taken = false;
    }

    for (k = 0; k < (int)(sizeof(order) / sizeof(order[0])); k++)
    {
        s = order[k];
        if (!(stages & BCMDT_STAGE(s)))
            continue;
        if (!dep_ok(s, stages, r->passed_mask))
        {
            say("stage %s skipped: needs stage %s to pass first", names[s],
                names[needs(s)]);
            continue;
        }
        if (!taken && h->take() < 0)
        {
            say("FAIL could not take the display back for stage %s", names[s]);
            break;
        }
        alive = false;
        bcm_off(true);          /* Apple's vmcs, stock order */
        switch (s)
        {
        case 1:  err = stage1(&alive); break;
        case 2:  err = stage2(&alive); break;
        case 3:  err = stage3(&alive); break;
        case 4:
            err = bench_stage(false, &alive);
            if (alive && err != E_PORT)
                bench_algn();     /* informational: never fails 4 */
            break;
        case BCMDT_STAGE_4B: err = bench_stage(true, &alive); break;
        case BCMDT_STAGE_4C: err = stage4c(&alive); break;
        case BCMDT_STAGE_5B: err = stage5b(&alive); break;
        case BCMDT_STAGE_5I: err = stage5i(&alive); break;
        case BCMDT_STAGE_3D: err = stage3d(&alive, fast_3d); break;
        case BCMDT_STAGE_5S:
            err = stage5s(&alive, sleep_5s ||
                          (r->passed_mask & BCMDT_STAGE(BCMDT_STAGE_5I)));
            break;
        default: err = stage5(&alive); break;
        }
        if (err == E_PORT)
            alive = false;
        t0 = h->now_us();
        bcm_off(alive);
        say("restore: Apple vmcs (stock wake path)");
        h->restore(false);
        if (s == BCMDT_STAGE_5)
        {
            r->e_fallback_us = h->now_us() - t0;
            say("restore: done; fallback to Apple's vmcs took %lu ms",
                (unsigned long)(r->e_fallback_us / 1000));
        }
        else
            say("restore: done");
        taken = false;
        if (err == OK)
            r->passed_mask |= BCMDT_STAGE(s);
    }
    if (taken)                  /* stage 0 passed but nothing could follow */
        h->restore(true);
    r->passed = passed_through(stages, r->passed_mask);
    say("=== end: passed through stage %d%s%s%s%s%s%s%s%s%s%s ===", r->passed,
        (stages & BCMDT_STAGE(4)) ? (r->passed_mask & BCMDT_STAGE(4) ?
                                     "; stage 4 passed" : "; stage 4 not passed")
                                  : "",
        (stages & BCMDT_STAGE(BCMDT_STAGE_4B)) ?
            (r->passed_mask & BCMDT_STAGE(BCMDT_STAGE_4B) ? "; stage 4b passed"
                                                          : "; stage 4b not passed")
            : "",
        (stages & BCMDT_STAGE(BCMDT_STAGE_4C)) ?
            (r->passed_mask & BCMDT_STAGE(BCMDT_STAGE_4C) ? "; stage 4c passed"
                                                          : "; stage 4c not passed")
            : "",
        (stages & BCMDT_STAGE(BCMDT_STAGE_5)) ?
            (r->passed_mask & BCMDT_STAGE(BCMDT_STAGE_5) ? "; stage 5 passed"
                                                         : "; stage 5 not passed")
            : "",
        (stages & BCMDT_STAGE(BCMDT_STAGE_5B)) ?
            (r->passed_mask & BCMDT_STAGE(BCMDT_STAGE_5B) ? "; stage 5b done"
                                                          : "; stage 5b not done")
            : "",
        (stages & BCMDT_STAGE(BCMDT_STAGE_5I)) ?
            (r->passed_mask & BCMDT_STAGE(BCMDT_STAGE_5I) ? "; stage 5i passed"
                                                          : "; stage 5i not passed")
            : "",
        (stages & BCMDT_STAGE(BCMDT_STAGE_5S)) ?
            (r->passed_mask & BCMDT_STAGE(BCMDT_STAGE_5S) ? "; stage 5s passed"
                                                          : "; stage 5s not passed")
            : "",
        (stages & BCMDT_STAGE(BCMDT_STAGE_3D)) ?
            (r->passed_mask & BCMDT_STAGE(BCMDT_STAGE_3D) ? "; stage 3d passed"
                                                          : "; stage 3d not passed")
            : "",
        r->why[0] ? "; " : "", r->why);
    return r->passed;
}

int bcmdt_run(const struct bcmdt_hal *hal, int last, struct bcmdt_result *r)
{
    if (last < 0)
        last = 0;
    if (last > BCMDT_LAST_STAGE)
        last = BCMDT_LAST_STAGE;
    return bcmdt_run_stages(hal, BCMDT_STAGE(last + 1) - 1, NULL, r);
}
