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
 * bcmfw: our own VideoCore II image for the BCM2722 (iPod Video).
 * Build-time configuration: memory layout, the host mailbox, and every
 * assumption that only a device run can settle (switchable here).
 * Shared by C and by the preprocessed assembly (.S).
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
#ifndef BCMFW_CONFIG_H
#define BCMFW_CONFIG_H

/* ---- Memory layout (internal SRAM 0x000000-0x13FFFF) ------------------ */

/* Where the host-started core begins executing.  The one place this
 * address lives: build.py reads it from here for the layout.  [M] Apple's
 * image starts here; BCMFW_RESET_BRANCH covers the other case. */
#define BCMFW_ENTRY          0x200
/* 1: also put "b _start" at address 0 (in case the core starts at 0 after
 * the host's 0x10000400 = 0xA5A50002).  It replaces exception vector 0. */
#ifndef BCMFW_RESET_BRANCH
#define BCMFW_RESET_BRANCH   0
#endif

#define BCMFW_SRAM_SIZE      0x140000
#define BCMFW_STACK_TOP      0xD0000    /* grows down, below the framebuffer */
#define BCMFW_STACK_SIZE     0x4000     /* reserved for the size check only */
#define BCMFW_FB             0xE0000    /* Rockbox's frame: 320x240 RGB565 */
#define BCMFW_LCD_WIDTH      320
#define BCMFW_LCD_HEIGHT     240

/* ---- Host mailbox (the unused end of the vector area) ----------------- */
/* Every build: what the device test reads when an image stops answering
 * (device session 2: hello timed out with nothing to say where) */
#define BCMFW_MB_LIMIT       0x180  /* BCMFW_LIMIT_* << 24 | count: the last
                                       bounded wait that ran out, 0 = none */
#define BCMFW_MB_LIMIT_VAL   0x184  /*   the register value it last read */
#define BCMFW_MB_CLK         0x188  /* clock enable: BCMFW_CLK_* bits, and
                                       counter ticks over spin(1000) << 16 */
#define BCMFW_MB_CLK_4C      0x18C  /* 0x10001C4C, 0x10001C54, 0x10001C58 as */
#define BCMFW_MB_CLK_54      0x190  /*   read at start, before any write */
#define BCMFW_MB_CLK_58      0x194
/* Every build: spin()'s loop timed at sixteen alignments (spinprobe.S;
 * session 5's 38 vs 75 ticks).  Byte k of the four words = ticks for
 * 1000 iterations with the loop 2k bytes into a 64-byte block (255 = or
 * more); the fifth = BCMFW_SPA_MAGIC | spin()'s address. */
#define BCMFW_MB_SPA         0x150  /* 0x150-0x15C: offsets 0-6, 8-14, 16-22, 24-30 */
#define BCMFW_MB_SPA_SPIN    0x160
/* Every image with a panel (dev7): the device test's stage 5 stops the
 * image on purpose, to time the host's detection and fallback.  Only
 * BCM_CMD(CMD_TEST_STOP) with this word set to BCMFW_STOP_MAGIC does it;
 * the command alone is acknowledged like any unknown one. */
#define BCMFW_MB_STOP        0x164
#define BCMFW_STOP_MAGIC     0x53544F50  /* "STOP" */
#define BCMFW_SPA_MAGIC      0x53500000  /* "SP" << 16 */
/* The step word: in probe and bench builds the probe's own codes
 * (BCMFW_PROBE_*, BCMFW_BENCH_STEP_*) once their check starts; the
 * BCMFW_STEP_* codes before that and in every other build */
#define BCMFW_MB_STEP        0x1C0
/* DMA build only (BCMFW_LCD_DMA): what the device test's stage 3 reads */
#define BCMFW_MB_DMA_FORCE   0x1A0  /* host: BCMFW_DMA_FORCE_MAGIC = the next
                                       push posts no control block, so it
                                       times out (the image clears it) */
#define BCMFW_MB_DMA_US      0x1A4  /* 1 MHz counter: last push, arm to end */
#define BCMFW_MB_DMA_DONE    0x1A8  /* pushes the engine completed */
#define BCMFW_MB_DMA_TMO     0x1AC  /* pushes that timed out */
#define BCMFW_MB_DMA_CS      0x1B0  /* last push's end: channel config, */
#define BCMFW_MB_DMA_INT     0x1B4  /*   status word, */
#define BCMFW_MB_DMA_CB      0x1B8  /*   sub-channel word, */
#define BCMFW_MB_DMA_POLLS   0x1BC  /*   polls it took */
#define BCMFW_MB_DMA_RUN0    0x198  /* LCD +0x00 << 20 | +0x04, one poll */
#define BCMFW_MB_DMA_RUN1    0x19C  /*   after arming, and at the end
                                       (BCMFW_DMA_READBACK) */
#define BCMFW_DMA_FORCE_MAGIC 0x464F5243    /* "FORC" */
/* Every build with a panel: the LCD bus timing (lcd.c, panels.c) */
#define BCMFW_MB_TIMING_SEL  0x170  /* host: BCMFW_TIMING_APPLE_MAGIC = use
                                       Apple's 150 MHz words from the next
                                       command on; BCMFW_TIMING_SWEEP_MAGIC
                                       | k = the words for sweep clock k
                                       (BCMFW_SWEEP_KHZ_k); anything else =
                                       ours */
#define BCMFW_MB_TIMING0     0x174  /* image: the words last applied to */
#define BCMFW_MB_TIMING1     0x178  /*   0x10004010 and 0x10004014 */
#define BCMFW_MB_TIMING_KHZ  0x17C  /* image: the clock the words in use are
                                       computed for (0: Apple's words) */
#define BCMFW_TIMING_APPLE_MAGIC 0x4150504C  /* "APPL" */
#define BCMFW_TIMING_SWEEP_MAGIC 0x53575000  /* "SWP" << 8 | k */
/* The bus-timing sweep (device stage 3): Apple's nanoseconds recomputed
 * for these clocks, chosen by the host one frame at a time; each clock
 * shows the bars, then a fine pattern.  Session 6 (50/40/30 MHz: 19.95,
 * 17.12, 14.28 ms) fits frame = 76,800 x 37.0 ns x the cycles of panel
 * 3's later word exactly: the bus runs at 27 MHz, with no per-pixel
 * overhead (hardware.md, "Device session 6").  That word bottoms out at
 * 5 cycles from 30 MHz down to 16 MHz (the 65 ns field keeps 2 cycles),
 * so 25 MHz would repeat 30's frame.  The sweep is therefore:
 *   27 MHz  Apple's nanoseconds exactly on the measured bus (14.2 ms):
 *           the candidate default
 *   20 MHz  the same later word, the first word about 25% under its
 *           nanoseconds
 *   15 MHz  the later word at 4 cycles (11.4 ms), its 65 ns field at
 *           37 ns: past the panel's minimum, to find the limit
 * Display only: a mis-clocked frame shows wrong, nothing else is
 * written. */
#define BCMFW_SWEEP_N        3
#define BCMFW_SWEEP_KHZ_0    27000
#define BCMFW_SWEEP_KHZ_1    20000
#define BCMFW_SWEEP_KHZ_2    15000
/* Probe build only (BCMFW_PROBE): progress, entry and self-check words */
#define BCMFW_MB_PROBE_STEP  0x1C0  /* last step reached (BCMFW_PROBE_*) */
#define BCMFW_MB_PROBE_FROM0 0x1C4  /* 1: the core started at address 0 */
#define BCMFW_MB_PROBE_PASS  0x1C8  /* self-check: bit n set = check n passed */
#define BCMFW_MB_PROBE_ALL   0x1CC  /* self-check: the bits that must be set */
#define BCMFW_MB_PROBE_TICKS 0x1D0  /* 1 MHz counter delta over a 1000 spin */
#define BCMFW_MB_PROBE_FAULTED 0x1D8 /* bit n set = check n faulted (any check) */
#define BCMFW_MB_PROBE_RESULTS 0x1DC /* BCMFW_PROBE_RESULTS once its header is valid */
#define BCMFW_MB_FAULT_PC    0x1E0  /* written by the fault stub */
#define BCMFW_MB_FAULT_SP    0x1E4
#define BCMFW_MB_INFO        0x1E8  /* see BCMFW_INFO_* */
#define BCMFW_MB_FRAMES      0x1EC  /* LCD updates completed */
#define BCMFW_MB_RECT_XY     0x1F0  /* UPDATERECT: x | y << 16 */
#define BCMFW_MB_RECT_WH     0x1F4  /* UPDATERECT: w | h << 16 */
#define BCMFW_MB_COMMAND     0x1F8  /* lcd-video.c BCMA_COMMAND */
/* The warm swap (HP_OP_PARK; jobs.md G9, bench row B13): the core parks
 * in a stub at BCMFW_PARK_STUB, outside every image's load and bss, and
 * says so here (PARKED | flags).  The host uploads the next image as at a
 * cold start (which zeroes this word), flushes the cache as at a cold
 * start, then writes GO | the same flags; the stub jumps to BCMFW_ENTRY.
 * The new image reads GO before anything else and, with PANEL_ON, takes
 * the panel as on: no reset pulse, no power-on script, GPIO 48 untouched.
 * It then writes UP | flags (and BCMFW_INFO_WARM).  0x13C is the last
 * word of the protocol header, unused by it. */
#define BCMFW_MB_WARM        0x13C
#define BCMFW_WARM_PARKED    0x50524B00  /* "PRK" << 8: the stub runs */
#define BCMFW_WARM_GO        0x57474F00  /* "WGO" << 8: host, jump */
#define BCMFW_WARM_UP        0x57555000  /* "WUP" << 8: the new image */
#define BCMFW_WARM_PANEL_ON  0x01        /* the panel is on (else off) */
#define BCMFW_WARM_TIMING1   0x02        /* its second timing word in use */
#define BCMFW_PARK_STUB      0xDE900     /* 256 bytes; free in both maps */
#define BCMFW_PARK_STUB_MAX  0x100
/* 1: PARK's hibernate form (HP_PARK_HIB; bcmfw/hibwake.h, power-contract.md
 * section 6.6): the core calls the PP-uploaded stub at HW_STUB and, after
 * a good wake, runs on with everything it held.  Both jobs images (the
 * merged image's hook came in its own commit, held back until the merged
 * image's device re-verify).  The PP never asks before PW5 passes on the
 * unit. */
#ifndef BCMFW_HIB_PARK
#define BCMFW_HIB_PARK       BCMFW_JOBS
#endif
/* The park runs Apple's sequence for code the core has just written
 * before it calls the stub (bench row B13, 2026-10-25: three builds never
 * parked; quirks.md #40).  Apple flushes the whole cache after writing
 * code and before running it (vmcs.bin @88a0 after a library's
 * relocations, @91a4 at a library's start; the flush itself @25a); our
 * stub was copied by data stores and called at once.  Every image; the
 * park also writes each step to BCMFW_MB_WARM_PROG, which the PP logs
 * when no "PRK" comes.  0 only in the sim test that shows the old park
 * refused (jobs_image_test.py). */
#ifndef BCMFW_PARK_FLUSH
#define BCMFW_PARK_FLUSH     1
#endif
#define BCMFW_MB_WARM_PROG   0x138       /* 'W' << 24 | step << 16 | detail
                                            (HP_WARM_PROG_*): the TV, PW and
                                            hibernate progress word, which
                                            no park overlaps */
#define BCMFW_CACHE_CTL      0x10000C00

/* Status values we write to BCMA_COMMAND.  lcd-video.c treats 0xFFFF0000
 * (its own LCD_UPDATE) and 0xFFFF as busy, anything else as idle. */
#define BCMFW_STATUS_BUSY    0x0000FFFF
#define BCMFW_STATUS_DONE    0x50420001  /* 'PB', mailbox protocol 1 */
#define BCMFW_STATUS_FAULT   0xFA170000

#define BCMFW_INFO_MAGIC     0x50420000  /* bits 31:16 */
#define BCMFW_INFO_PANEL_ON  0x0100      /* bits 1:0 panel ID, 7:4 model */
#define BCMFW_INFO_TIMER_OK  0x0200      /* 1 MHz counter seen running */
#define BCMFW_INFO_IDLE      0x0400      /* built with BCMFW_IDLE_SLEEP */
#define BCMFW_INFO_SPIN      0x2000      /* delays are spin loops (no counter) */
#define BCMFW_INFO_CLK       0x4000      /* the image wrote the timer clock */
#define BCMFW_INFO_WARM      0x0800      /* started by a warm swap */

/* Step word values (BCMFW_MB_STEP), 'PS' = 0x5053 */
#define BCMFW_STEP_START     0x50530001  /* bcmfw_main entered */
#define BCMFW_STEP_CLK_A     0x50530002  /* timer clock: 0x4C / 0x54 */
#define BCMFW_STEP_CLK_B     0x50530003  /* timer clock: 0x58 */
#define BCMFW_STEP_TIMER     0x50530004  /* counter check */
#define BCMFW_STEP_GPIO      0x50530005  /* panel ID pins */
#define BCMFW_STEP_IDLE      0x50530006  /* ready, waiting for a command */
#define BCMFW_STEP_DONE      0x5053000F  /* the last command finished */
#define BCMFW_STEP_POWER     0x50531000  /* + power-on step (lcd.c) */
#define BCMFW_STEP_SCRIPT    0x50532000  /* + script << 12 (0 init, 1 on,
                                            2 off; 6 light-standby off,
                                            7 its wake) + record index */
#define BCMFW_STEP_FRAME     0x50535000  /* + row: CPU push, every 16 rows */
#define BCMFW_STEP_DMA       0x50536000  /* + polls/16: DMA push wait */
#define BCMFW_STEP_CMD       0x50537000  /* + command number */

/* Clock enable record (BCMFW_MB_CLK) */
#define BCMFW_CLK_MAGIC      0x00000C00  /* bits 11:8: record valid */
#define BCMFW_CLK_RAN0       0x01        /* counter ran before any write */
#define BCMFW_CLK_RAN_A      0x02        /* ran after 0x4C / 0x54 */
#define BCMFW_CLK_RAN_B      0x04        /* ran after 0x58 */
#define BCMFW_CLK_WROTE_A    0x10
#define BCMFW_CLK_WROTE_B    0x20
#define BCMFW_CLK_RATE_BAD   0x40        /* it ran, at an implausible rate:
                                            delays stay spin-based */

/* Bounded waits (BCMFW_MB_LIMIT bits 31:24) */
#define BCMFW_LIMIT_DELAY    1   /* a counter delay ran out on spins: the
                                    counter stopped mid-delay (not fatal) */
#define BCMFW_LIMIT_DMA      2   /* DMA completion never set (CPU re-send) */

/* ---- Assumptions for the first device run ----------------------------- */

/* The clock (kHz) the LCD bus timing words are computed for, from
 * Apple's per-panel nanosecond tables with Apple's formula (@c87c;
 * panels.c).  A field is a cycle count, so the words give at least
 * Apple's nanoseconds on any bus clock up to this one, and fewer cycles
 * (a faster frame) below it.  Session 6 puts the bus at 27 MHz after a
 * host start.  Every image, test or boot path, uses these words unless
 * the host selects others (the stage 3 sweep does, per frame).
 *   27000   default (dev8; owner, session 7: the fine pattern at 27, 20
 *           and 15 MHz words showed no difference from 40): Apple's
 *           nanoseconds exactly on the measured bus, 14.2 ms a DMA frame
 *           (Apple's own LCD_UPDATE: 15.2 ms); 30000 gives the same frame
 *   40000   fallback (dev6/dev7's default): every field at least Apple's
 *           nanoseconds with margin, 17.1 ms a DMA frame
 *   100000  the safe fallback (dev4/dev5's words, 31.4 ms a frame)
 *   0       Apple's 150 MHz words as they are (dev3 and earlier)
 * 20000 and 15000 (14.2 and 11.4 ms) are below the panel's minimums and
 * are never a default: session 7's clean photos at room temperature are
 * evidence of headroom, not margin. */
#ifndef BCMFW_LCD_BUS_KHZ
#define BCMFW_LCD_BUS_KHZ    27000
#endif
/* -1: read the panel ID from GPIO 51/50 (hardware.md).  0-3: force it. */
#ifndef BCMFW_PANEL_ID
#define BCMFW_PANEL_ID       -1
#endif
/* Panel variant ("model") for IDs 1 and 3.  Apple's ARM side picks it;
 * which one the 5.5G uses is unknown (hardware.md). */
#ifndef BCMFW_PANEL_MODEL
#define BCMFW_PANEL_MODEL    1
#endif
/* 0: busy-poll the command word.  1: sleep between polls, woken by
 * timer compare 1 every BCMFW_IDLE_TICK_US (interrupt source 17). */
#ifndef BCMFW_IDLE_SLEEP
#define BCMFW_IDLE_SLEEP     0
#endif
#ifndef BCMFW_IDLE_TICK_US
#define BCMFW_IDLE_TICK_US   1000
#endif
/* 1 (with BCMFW_IDLE_SLEEP): also take interrupt source 23, the host
 * doorbell [M] (hardware.md, "Host port"; Rockbox writes BCM_CONTROL =
 * 0x31 after every command word), acknowledged at 0x10003800.  If the
 * host's write raises it, a command wakes the core at once and the tick
 * is only a backstop; if not, nothing changes but the count stays 0
 * (stage 5i logs it).  A source that stays pending after the ack would
 * wake every sleep at once: the sleep count then far exceeds the tick
 * count, and the build behaves like the busy-poll one. */
#ifndef BCMFW_IDLE_DOORBELL
#define BCMFW_IDLE_DOORBELL  0
#endif
#define BCMFW_DOORBELL_IRQ   23
#define BCMFW_DOORBELL_ACK   0x10003800
/* 1 (with BCMFW_IDLE_SLEEP): sleep only while the host has written
 * BCMFW_IDLE_EN_MAGIC to BCMFW_MB_IDLE_EN, busy-poll otherwise (the image
 * starts with the word zero: the mailbox is part of the upload).  The
 * resident video image (bcmvideo.bin as the display driver, p14-resident):
 * the host turns it on while the image serves the UI and off for a video
 * session, whose polling stays as proven. */
#ifndef BCMFW_IDLE_GATED
#define BCMFW_IDLE_GATED     0
#endif
#define BCMFW_MB_IDLE_EN     0x16C
#define BCMFW_IDLE_EN_MAGIC  0x534C5031  /* "SLP1" */
/* Idle-sleep builds (stage 5i, hello_dma_idle): counters for the host,
 * at 0x140-0x14C.  A build with the clock record there (BCMFW_CLOCK: the
 * video image) keeps them at 0x1D4-0x1DC and 0x1FC instead, the probe and
 * bench words, which those builds never have. */
#if BCMFW_VIDEO || BCMFW_BENCH_PLL || BCMFW_CLOCK_CMD
#define BCMFW_MB_IDLE_TICKS  0x1D4  /* timer compare 1 handler runs */
#define BCMFW_MB_IDLE_BELLS  0x1D8  /* doorbell handler runs */
#define BCMFW_MB_IDLE_SLEEPS 0x1DC  /* sleeps the idle loop returned from */
#define BCMFW_MB_IDLE_BAD    0x1FC  /* sleeps after which r0-r5 differed */
#else
#define BCMFW_MB_IDLE_TICKS  0x140  /* timer compare 1 handler runs */
#define BCMFW_MB_IDLE_BELLS  0x144  /* doorbell handler runs */
#define BCMFW_MB_IDLE_SLEEPS 0x148  /* sleeps the idle loop returned from */
#define BCMFW_MB_IDLE_BAD    0x14C  /* sleeps after which r0-r5 differed */
#endif
#define BCMFW_IDLE_REG_MAGIC 0x1D1E5100  /* r0-r5 hold this + 0..5 asleep */
/* 1: CMD_LCD_STANDBY (the sleep spikes, stage 5s): the panel goes dark
 * with its reset line (GPIO 48) held high, so the next power-on skips
 * the reset pulse, the wait after it and the bus resync.  The host picks
 * the depth in BCMFW_MB_STANDBY before the command:
 *   LIGHT  the display-off part of the panel's off script only, its
 *          power circuits left on; the wake is the display-on ramp
 *   DEEP   the whole off script (power circuits down), reset high; the
 *          wake is the panel's init and on scripts
 * Either sequence is our reading of the panel's own scripts [hypothesis]:
 * whether the panel comes back right is what the stage asks.  Every
 * panel power-on (cold or from standby) is timed into BCMFW_MB_PON_US.
 * The words are the probe's and the video image's (never built with it). */
#ifndef BCMFW_LCD_STANDBY
#define BCMFW_LCD_STANDBY    0
#endif
#define BCMFW_MB_STANDBY     0x1C8  /* host: BCMFW_STANDBY_MAGIC | depth */
#define BCMFW_MB_STANDBY_ST  0x1CC  /* image: the depth the panel is in
                                       (0: on or off), bit 8 the last
                                       request was refused */
#define BCMFW_MB_PON_US      0x1D0  /* image: the last panel power-on,
                                       1 MHz counter, bit 31 = from standby */
#define BCMFW_STANDBY_MAGIC  0x53425900  /* "SBY" << 8 */
#define BCMFW_STANDBY_LIGHT  1
#define BCMFW_STANDBY_DEEP   2
#define BCMFW_STANDBY_REFUSED 0x100
#define BCMFW_PON_FROM_SBY   0x80000000u
#define BCMFW_INFO_STANDBY   0x8000      /* built with BCMFW_LCD_STANDBY */
#if BCMFW_LCD_STANDBY && (BCMFW_PROBE || BCMFW_VIDEO || BCMFW_BENCH)
#error "the standby words are the probe's and the video image's"
#endif
/* 1: UPDATERECT sets a panel window and pushes only the rectangle.
 * 0: it pushes the whole frame (window registers outside the full-frame
 * values are our reading of the panels, [M]/[L]). */
#ifndef BCMFW_RECT_WINDOWED
#define BCMFW_RECT_WINDOWED  1
#endif
/* 0: pixels go out through the unrolled assembly loop at full CPU speed.
 * N > 0: one pixel per write followed by N spin iterations, in case the
 * LCD interface does not stall the core when its FIFO is full (Apple only
 * ever sends pixels by DMA paced by the LCD's request line). */
#ifndef BCMFW_PIXEL_PACE
#define BCMFW_PIXEL_PACE     0
#endif
/* 1: the timing bench (bench/, bringup.md "Device session 2").  A probe
 * build (below: no panel or GPIO output) that, once it has reported
 * ready, times instruction mixes against the 1 MHz counter, checks the
 * decoder's unproven instruction forms on known inputs, and fills the
 * results block at BCMFW_BENCH_RESULTS.  Implies BCMFW_PROBE. */
#ifndef BCMFW_BENCH
#define BCMFW_BENCH          0
#endif
#if BCMFW_BENCH
#undef BCMFW_PROBE
#define BCMFW_PROBE          1
#endif
/* 1 (bench builds only, off by default): after the first timing pass,
 * switch the core clock to 150 MHz with Apple's start-up sequence (clock
 * block, regulator to level 15) and time everything again.  It changes
 * clocks and the core supply: see bringup.md for the risks. */
#ifndef BCMFW_BENCH_PLL
#define BCMFW_BENCH_PLL      0
#endif
/* Loop iterations per timed run: 4096 makes a 33-instruction loop about
 * 135,000 instructions, near 1 ms at 150 MHz (0.1% on the counter) */
#ifndef BCMFW_BENCH_ITERS
#define BCMFW_BENCH_ITERS    4096
#endif
#define BCMFW_BENCH_RUNS     3
#define BCMFW_BENCH_RESULTS  0xC0000    /* the results block (bench/bench.h) */
#define BCMFW_BENCH_MAGIC    0x50424E31 /* 'PBN1' */
#define BCMFW_BENCH_RESULTS_SIZE 0x4000 /* reserved for it (build.py checks) */
#define BCMFW_MB_BENCH       0x1D4      /* bench builds: BCMFW_BENCH_RESULTS */
/* Step word (BCMFW_MB_PROBE_STEP) values while the bench runs */
#define BCMFW_BENCH_STEP_VEC    0x50521000  /* + vector check k */
#define BCMFW_BENCH_STEP_SCALAR 0x50522000  /* + scalar check n */
#define BCMFW_BENCH_STEP_TIME   0x50523000  /* + kernel k */
#define BCMFW_BENCH_STEP_PLL    0x50524000  /* + step of the clock switch */
#define BCMFW_BENCH_STEP_TIME2  0x50525000  /* + kernel k, at 150 MHz */
#define BCMFW_BENCH_STEP_SLOC   0x50526000  /* + store-locating check k */
#define BCMFW_BENCH_DONE        0x50520004

/* 1: video (docs/research/bcm2722/pipeline.md).  Our H.264 decoder
 * behind the host protocol: OPEN with codec 1 decodes the FEED packets
 * from the SDRAM bitstream FIFO, converts each picture to RGB565 on the
 * vector unit (scaling 2:1 above 320x240) and pushes it to the panel by
 * DMA.  Implies BCMFW_HOSTPROTO and BCMFW_LCD_DMA; sets up SDRAM at the
 * first H.264 OPEN. */
#ifndef BCMFW_VIDEO
#define BCMFW_VIDEO          0
#endif
#if BCMFW_VIDEO
#undef BCMFW_HOSTPROTO
#define BCMFW_HOSTPROTO      1
#undef BCMFW_LCD_DMA
#define BCMFW_LCD_DMA        1
#endif
/* SDRAM refresh interval in core cycles (SDRAM controller +0x24 bits
 * 31:16).  Apple's 0x493 is 7.8 us at 150 MHz; we don't switch the PLL,
 * and the start-up clock is unknown, so the default assumes it could be
 * as slow as the 27 MHz crystal: 210 cycles = 7.8 us at 27 MHz (a faster
 * clock only refreshes more often).  A device question (bringup.md). */
#ifndef BCMFW_SDRAM_REFRESH
#define BCMFW_SDRAM_REFRESH  0x00D2
#endif
/* The image must end below the decoder's working set (hostproto.md §7) */
#define BCMFW_IMAGE_LIMIT    0x80000
/* Video builds (dev11): the decoder's arena, context and frames, from
 * here to the stack; the image must end below it (video.h VIDEO_ARENA).
 * 0x31000 since the AAC decoder joined the image (M4), 0x32000 since
 * dev45 (8a-1's slave tone and stage 9's dump-and-diff): 616 KB, still
 * holding a 320x240 stream with 4 references in SRAM (617,056 bytes:
 * context 40,928 + 5 frames of 115,200 + slack; 13,728 to spare) */
#ifndef BCMFW_JOBS
#define BCMFW_JOBS           0           /* the display jobs (merged image) */
#endif
/* 1: a candidate image with the staged upload (jobs.h J_ALLOC_STAGED,
 * jobs rev 0.5, open question #67): the PP writes a slot's pixels into a
 * window in SDRAM, the BCM moves them into the slot by its own DMA at
 * COMMIT, then sums the slot.  Sim only since 2026-10-25: the COMMIT
 * failure it was to work around was the sum's halfword loads (quirks.md
 * #33), not the port; no image ships it, jobs_image_test still builds
 * it. */
#ifndef BCMFW_JOBS_STAGED
#define BCMFW_JOBS_STAGED    0
#endif
/* The display-only image's JPEG decode (J_OP_DECODE): its IDCTs on the
 * vector unit (jdec_vpu.c, jpeg-spike.md section 13) when its start-up
 * check passes; 0 keeps the C IDCT (the scalar path, for comparison) */
#ifndef BCMFW_JDEC_VPU_IDCT
#define BCMFW_JDEC_VPU_IDCT  1
#endif
/* Tests only: the IDCT's start-up check (jdec_vpu.c) expects the wrong
 * pixels, so it fails, J_VPU_MAC stays clear and jdec keeps the C IDCT
 * (jobs_image_test.py's gate row).  Never 1 in an image for the device. */
#ifndef BCMFW_JOBS_MAC_FAIL
#define BCMFW_JOBS_MAC_FAIL  0
#endif
/* Tests only: the jobs' lane-shift self-check (jobs_bcm.c) expects the
 * wrong pixels, so it fails and out-of-phase sources take the C path
 * (jobs_image_test.py's gate row).  Never 1 in an image for the device. */
/* Batch 4: the merged image's start-up checks of a vector loop filter's
 * forms (video/bschk.c, gen_bschk.py; jobs.h J_VPU2), on for sitting 4
 * (owner's go, 2026-10-26).  tools/bcmre/proven_forms.txt lets only
 * check-only code run them (the bench region jv_bschk_*, its seven forms
 * as class bench); the decoder keeps its C until silicon passes them.
 * The load timings (J_LAT_*) need no new form. */
#ifndef BCMFW_BSCHK
#define BCMFW_BSCHK          1
#endif
#ifndef BCMFW_JOBS_SHIFT_FAIL
#define BCMFW_JOBS_SHIFT_FAIL 0
#endif
/* 0x35400 for every video image since 2026-10-24: the merged display
 * image (P19, memory-contract.md section 2) needs ~15 KB more image, and
 * in UI mode the arena holds the job pools; the plain video image had
 * grown past 0x32000 (bss to 0x32b7c: OPEN's arena overwrote spec_data,
 * trace and aac_flags), which build.py's regex read of this file missed.
 * 617,472 bytes still hold 320x240 with 4 references. */
#define BCMFW_VIDEO_ARENA    0x35400
/* MERGED IMAGE (batch 3, 2026-10-26; album-render.md section 5.3, the
 * panel's end state): the merged image takes overlays (J_OP_OVERLAY), and
 * DECODE loads into it as one (BCMFW_JOBS_OVL below, video/dec_ovl.c;
 * mkvideo.py writes dec-ovl.bin and its record beside the image).  The
 * loader and the decode's service cost 640 B of image [measured,
 * 2026-10-26]; one AAC frame's spectral data (2 KB, video sessions only)
 * moves to the far bss for it (build.py AAC_FAR_MERGED_OVL), and the
 * arena doesn't move.  On since batch 3 (0 builds the image without the
 * loader, for comparisons) */
#ifndef BCMFW_JOBS_MERGED_OVL
#define BCMFW_JOBS_MERGED_OVL 1
#endif

/* AAC-LC on the chip (bcmfw/aac, #60, audio-spike.md section 7): every
 * video image carries the decoder (HP_OP_AAC, HP_CAP_AAC).  Its code,
 * hot buffers and small tables are in the image (SRAM); the inverse-
 * quantisation table, the FFT's bit-reversal table, the long windows
 * (vc2cc .xdata) and the channel elements (.sdbss) run from SDRAM at
 * BCMFW_AAC_FAR (video.h VIDEO_AAC_*).  .xdata rides in the load image
 * after .data; crt0 parks it at the top of the decoder arena (ending at
 * BCMFW_AAC_PARK_END) before it clears .bss, and the first H.264 OPEN
 * copies it into SDRAM once SDRAM is up (aac_image.c). */
#ifndef BCMFW_AAC
#define BCMFW_AAC            BCMFW_VIDEO
#endif
#define BCMFW_AAC_FAR        0xFE380000  /* video.h VIDEO_AAC_OFF, BCM view */
#define BCMFW_AAC_FAR_SIZE   0x1C000     /* to VIDEO_AAC_IO */
#if BCMFW_JOBS && BCMFW_VIDEO
/* merged image: parked at the top of SRAM, clear of the UI pools that
 * share the arena (0x134000-0x13FFFF; the picture ends at 0x12B000) */
#define BCMFW_AAC_PARK_END   0x140000
#else
#define BCMFW_AAC_PARK_END   0xCC000     /* the arena's end (the stack) */
#endif

/* 1: the first-device-session probe.  Reports ready, the counter, where
 * the core started and an on-chip check of the instruction forms Apple's
 * code never exercises (probe.S), then answers every command without any
 * panel or GPIO-output traffic.  Implies the reset branch at 0, which in
 * this build records BCMFW_MB_PROBE_FROM0 before going to _start. */
#ifndef BCMFW_PROBE
#define BCMFW_PROBE          0
#endif
#if BCMFW_PROBE && !BCMFW_RESET_BRANCH
#undef BCMFW_RESET_BRANCH
#define BCMFW_RESET_BRANCH   1
#endif
#define BCMFW_PROBE_ENTERED  0x50520001  /* _start reached ('PR') */
#define BCMFW_PROBE_CHECKING 0x50520002  /* in the self-check (+ check n << 8) */
#define BCMFW_PROBE_READY    0x50520003  /* reported ready */

/* Probe v2 results block (probe.S; bringup.md "Probe v2").  Words:
 * header [0] magic, [1] nchecks | REC_WORDS << 8 | version << 16,
 * [2] required mask, [3] informational mask, [4] pass bits (all checks),
 * [5] fault bits, [6]/[7] TIMER_CLO before/after the 1000-iteration spin;
 * then record U (a fault outside any check) and one record per check.
 * Record: [0] status (BCMFW_PROBE_ST_*), [1] frame sr, [2] frame pc,
 * [3] frame address (sp at handler entry), [4..9] r0-r5, [10] detail,
 * [11] detail2.  Away from the image, the stack (0xCC000-0xD0000), the
 * bench block (0xC0000, BCMFW_BENCH_RESULTS_SIZE), the host-protocol
 * rings (0xD0000-0xDC000) and the framebuffer: build.py checks. */
#ifndef BCMFW_PROBE_RESULTS
#define BCMFW_PROBE_RESULTS  0xCB000
#endif
#define BCMFW_PROBE_MAGIC    0x50524232  /* 'PRB2' */
#define BCMFW_PROBE_VERSION  2
#define BCMFW_PROBE_NCHECKS  13
#define BCMFW_PROBE_HDR_WORDS 8
#define BCMFW_PROBE_REC_WORDS 12
#define BCMFW_PROBE_REQUIRED 0x1FF       /* checks 0-8: bits in PASS/ALL */
#define BCMFW_PROBE_INFO     0x1E00      /* checks 9-12: never in PASS/ALL */
/* Address of record i (-1 = record U) */
#define BCMFW_PROBE_REC(i)   (BCMFW_PROBE_RESULTS + 4 * BCMFW_PROBE_HDR_WORDS \
                              + 4 * BCMFW_PROBE_REC_WORDS * ((i) + 1))
#define BCMFW_PROBE_SIZE     (4 * BCMFW_PROBE_HDR_WORDS + 4 * BCMFW_PROBE_REC_WORDS \
                              * (BCMFW_PROBE_NCHECKS + 1))
/* Record status: bits 7:0 state, 15:8 vector (fault only), 23:16 check
 * index (0xFF for U), 31:24 BCMFW_PROBE_ST_TAG */
#define BCMFW_PROBE_ST_TAG   0xC5000000
#define BCMFW_PROBE_NOT_RUN  0
#define BCMFW_PROBE_PASSED   1
#define BCMFW_PROBE_FAILED   2
#define BCMFW_PROBE_FAULT    3

/* Delay loop iterations per microsecond when the 1 MHz counter does not
 * run.  spin() is 3 instructions an iteration, so 50 is a full
 * microsecond for any core up to 150 MHz.  Device sessions 2-3 measured
 * about 27 MHz after a host start (bench step times): 50 waits about
 * 5.5 times too long there.  150 (the first value) waited 17 times too
 * long, and hello's 265 ms panel 3 power-on took over 4 s: the host's
 * 3 s limit ran out first. */
#ifndef BCMFW_SPIN_PER_US
#define BCMFW_SPIN_PER_US    50
#endif
/* 1: start the 1 MHz counter's clock as Apple's start-up does (0x10001C4C
 * = 0x501, 0x54 = 0, 0x4C = 0x51B, 0x54 = 0; if it still does not count,
 * 0x58 = 0x101), checking the counter after each.  No PLL, regulator or
 * run-control write.  0: touch no clock register. */
#ifndef BCMFW_CLK_ENABLE
#define BCMFW_CLK_ENABLE     1
#endif

/* ---- The 150 MHz core clock (clock.c; dev9) --------------------------
 * Session 7's stage 4b proved Apple's switch on the device (every kernel
 * 5.56 times faster).  Which images may make it:
 *   BCMFW_BENCH_PLL   the 4b bench (above)
 *   BCMFW_VIDEO_PLL   video builds: OPEN with HP_OPEN_FAST_CLOCK switches
 *                     up before the decoder starts; CLOSE, LCD_SLEEP and
 *                     a lost session switch down.  The host sets the flag
 *                     only with a stage 4b pass flag (pipeline.md, "The
 *                     150 MHz clock").
 *   BCMFW_CLOCK_CMD   the stage 4c image: CMD_CLOCK switches on request
 * The UI and boot images (hello, hello_dma) never switch: they run on the
 * crystal, 27 MHz. */
#ifndef BCMFW_VIDEO_PLL
#define BCMFW_VIDEO_PLL      BCMFW_VIDEO
#endif
#ifndef BCMFW_CLOCK_CMD
#define BCMFW_CLOCK_CMD      0
#endif
#if BCMFW_BENCH_PLL || (BCMFW_VIDEO && BCMFW_VIDEO_PLL) || BCMFW_CLOCK_CMD
#define BCMFW_CLOCK          1
#else
#define BCMFW_CLOCK          0
#endif
/* P19 PW4: the video images also move among the intermediate operating
 * points (clock.c clock_set, HP_OP_VCLK; 54, 81, 108 and 150 MHz, each at
 * Apple's level), the host's per-stream clock (apps/video/vclk.h) */
#ifndef BCMFW_VCLK
#define BCMFW_VCLK           (BCMFW_VIDEO && BCMFW_VIDEO_PLL)
#endif
#if BCMFW_CLOCK && BCMFW_IDLE_SLEEP && BCMFW_MB_IDLE_TICKS == 0x140
#error "the idle counters would share 0x140-0x14C with the clock record"
#endif
#if BCMFW_IDLE_SLEEP && BCMFW_PROBE && BCMFW_MB_IDLE_TICKS == 0x1D4
#error "the idle counters would share 0x1D4-0x1DC with the probe's words"
#endif
/* Mailbox, every BCMFW_CLOCK build: what the last switch did */
#define BCMFW_MB_CLOCK_STATE 0x140  /* BCMFW_CLOCK_MAGIC | BCMFW_CLOCK_* */
#define BCMFW_MB_CLOCK_RB0   0x144  /* regulator level before << 24 |
                                       regulator now (low byte) << 16 |
                                       clock select +0x28 */
#define BCMFW_MB_CLOCK_RB1   0x148  /* PLL A M (+0x00) << 16 | control +0x14 */
#define BCMFW_MB_CLOCK_US    0x14C  /* switch time, 1 MHz counter: up in
                                       bits 15:0, down in bits 31:16 */
#define BCMFW_MB_CLOCK_REQ   0x168  /* host (CMD_CLOCK): BCMFW_CLOCK_REQ_MAGIC
                                       | 1 = 150 MHz, | 0 = 27 MHz */
#define BCMFW_CLOCK_MAGIC    0x434B0000  /* "CK" << 16 */
#define BCMFW_CLOCK_FAST     0x1         /* the core is on PLL A */
#define BCMFW_CLOCK_STALL    0x2         /* a wait ended on its spin limit */
#define BCMFW_CLOCK_REQ_MAGIC 0x434C4B00 /* "CLK" << 8 */
/* SDRAM refresh interval in core cycles at 150 MHz: Apple's 0x493, 7.8 us
 * (video.c switches it with the clock: up after the switch, down before) */
#define BCMFW_SDRAM_REFRESH_FAST 0x0493

#ifndef __ASSEMBLER__
/* Integer-only firmware: no float or double anywhere (vc2cc --no-float
 * also rejects soft-float library calls). */
#pragma GCC poison float double

/* The compiler's own types, so a file may also include <stdint.h> and
 * <stddef.h> (the H.264 decoder does) */
typedef __UINT8_TYPE__     uint8_t;
typedef __UINT16_TYPE__    uint16_t;
typedef __UINT32_TYPE__    uint32_t;
typedef __INT32_TYPE__     int32_t;
typedef __SIZE_TYPE__      size_t;

/* ---- Stage 5b's drift image (BCMFW_DRIFT, docs/research/bcm2722/offload.md)
 * The main menu's panel drift done here: two covers (300x300 RGB565, the
 * art cache's panel size) held in SRAM, the cross-fade on the vector unit
 * (video/xfade.c) and the pans as window copies, each pushed with the
 * LCD rect path.  The host drives it with CMD_DRIFT and the parameter
 * block; it uploads the covers itself.  Implies BCMFW_LCD_DMA. */
#ifndef BCMFW_DRIFT
#define BCMFW_DRIFT          0
#endif
#if BCMFW_DRIFT
#undef BCMFW_LCD_DMA
#define BCMFW_LCD_DMA        1
#endif
#define BCMFW_DRIFT_SIDE     300         /* a cover's side, pixels */
#define BCMFW_DRIFT_X        160         /* the panel: x 160-319, full height */
#define BCMFW_DRIFT_SLOT0    0x108000    /* cover 0 (180,000 bytes, to 0x133F20) */
#define BCMFW_DRIFT_SLOT1    0x010000    /* cover 1 (to 0x03BF20); the image
                                            stays below it (build.py) */
#define BCMFW_DRIFT_STATE    0x040000    /* the panel as R, G, B planes */
#define BCMFW_DRIFT_SRC      0x078400    /* the fade's target, the same */
#define BCMFW_DRIFT_STG      0x0B0800    /* a window staged for the half step
                                            (160 x 240, to 0xC3400) */
#define BCMFW_DRIFT_PARAM    0x106000    /* the parameter block (words): */
#define BCMFW_DRIFT_P_OP     0           /*   host: BCMFW_DRIFT_MAGIC | op */
#define BCMFW_DRIFT_P_WIN    1           /*   host: slot << 24 | y << 12 | x */
#define BCMFW_DRIFT_P_WEIGHT 2           /*   host: 1..256 (FADE) */
#define BCMFW_DRIFT_P_US     3           /*   image: the op's time, 1 MHz */
#define BCMFW_DRIFT_P_COUNT  4           /*   image: ops done */
#define BCMFW_DRIFT_P_LAST   5           /*   image: the op word it last ran,
                                               0xBAD00000 | op if refused */
#define BCMFW_DRIFT_MAGIC    0x44520000  /* "DR" << 16 */
#define BCMFW_DRIFT_SHOW     1           /* the window of a slot, pushed */
#define BCMFW_DRIFT_PREP     2           /* planes: the panel now, and the
                                            window to fade to */
#define BCMFW_DRIFT_FADE     3           /* one step toward PREP's window,
                                            pushed (256: an exact copy) */
#define BCMFW_DRIFT_HALF     4           /* 8b: the panel averaged with the
                                            window (the next pixel's), pushed;
                                            a SHOW of that window next copies
                                            it from the staging buffer */
#ifndef BCMFW_DRIFT_REP_STORE
#define BCMFW_DRIFT_REP_STORE 0          /* 1: the REP 16 store kernel */
#endif

/* ---- The 3D render test (BCMFW_R3D, docs/research/bcm2722/render3d.md)
 * A PS1-style renderer on the core (bcmfw/r3d): four built-in scenes,
 * each rendered for the host's time into two framebuffers in turn, each
 * frame to the panel by DMA while the next renders.  Implies
 * BCMFW_LCD_DMA. */
#ifndef BCMFW_R3D
#define BCMFW_R3D            0
#endif
#if BCMFW_R3D
#undef BCMFW_LCD_DMA
#define BCMFW_LCD_DMA        1
#define BCMFW_R3D_TEX        0x010000    /* 8 textures, 64x64 bytes (32 KB) */
#define BCMFW_R3D_CLUT       0x018000    /* 48 x 256 RGB565 (24 KB) */
#define BCMFW_R3D_VERTS      0x020000    /* 2,048 x 8 bytes */
#define BCMFW_R3D_TRIS       0x024000    /* 4,096 x 16 bytes */
#define BCMFW_R3D_XVERTS     0x034000    /* 2,048 x 20 bytes */
#define BCMFW_R3D_OT         0x03E000    /* 512 x 2 bytes */
#define BCMFW_R3D_PRIMS      0x040000    /* 6,144 x 24 bytes, to 0x64000 */
#define BCMFW_R3D_FB2        0x108000    /* the second frame, to 0x12D800 */
#define BCMFW_R3D_PARAM      0x106000    /* the parameter block (words): */
#define BCMFW_R3D_P_OP       0           /*   host: BCMFW_R3D_MAGIC | op */
#define BCMFW_R3D_P_SCENE    1           /*   host: 0..4 */
#define BCMFW_R3D_P_MS       2           /*   host: how long to run */
#define BCMFW_R3D_P_CHECK_MS 3           /*   host: the second check frame */
#define BCMFW_R3D_P_STATE    4           /*   image: BCMFW_R3D_ST_* (the
                                            host's words 0-3 and PERIOD are
                                            read first; the rest cleared) */
#define BCMFW_R3D_P_FRAMES   5           /*   image: frames shown */
#define BCMFW_R3D_P_US       6           /*   image: the run, 1 MHz */
#define BCMFW_R3D_P_RENDER   7           /*   image: rendering, summed */
#define BCMFW_R3D_P_SETUP    8           /*   image: sky, transform, set-up */
#define BCMFW_R3D_P_RASTER   9           /*   image: drawing the table */
#define BCMFW_R3D_P_WAIT     10          /*   image: waiting for the panel */
#define BCMFW_R3D_P_MAX      11          /*   image: the slowest frame */
#define BCMFW_R3D_P_MIN      12          /*   image: the fastest frame */
#define BCMFW_R3D_P_TRIS     13          /*   image: the scene's triangles */
#define BCMFW_R3D_P_DRAWN    14          /*   image: drawn, summed */
#define BCMFW_R3D_P_CULLED   15          /*   image: culled, summed */
#define BCMFW_R3D_P_CLIPPED  16          /*   image: clipped, summed */
#define BCMFW_R3D_P_PIXELS   17          /*   image: pixels, summed */
#define BCMFW_R3D_P_CRC0     18          /*   image: r3d_crc at 0 ms */
#define BCMFW_R3D_P_CRC1     19          /*   image: ... at CHECK_MS */
#define BCMFW_R3D_P_CHUNKS   20          /*   image: chunks skipped, summed */
#define BCMFW_R3D_P_INIT     21          /*   image: textures and scene, us */
#define BCMFW_R3D_P_PERIOD   22          /*   host: us a frame (0: flat out) */
#define BCMFW_R3D_P_LATE     23          /*   image: frames past their slot */
#define BCMFW_R3D_P_SPARE    24          /*   image: idle before slots, us */
#define BCMFW_R3D_P_SYNC     25          /*   host: 0 the slot timer; n: each
                                            frame on the panel's n-th tearing
                                            edge (GPIO 49) after the last */
#define BCMFW_R3D_P_TE_N     26          /*   image (TE op): rising edges seen */
#define BCMFW_R3D_P_TE_MIN   27          /*   image: shortest edge-to-edge, us */
#define BCMFW_R3D_P_TE_MAX   28          /*   image: longest */
#define BCMFW_R3D_P_TE_SUM   29          /*   image: first to last edge, us */
#define BCMFW_R3D_P_TE_HIGH  30          /*   image: a pulse's width, us (last) */
#define BCMFW_R3D_P_TE_LEVEL 31          /*   image: the level at the start, and
                                            level changes << 8 */
#define BCMFW_R3D_P_N        32
#define BCMFW_R3D_MAGIC      0x33440000  /* "3D" << 16 */
#define BCMFW_R3D_RUN        1
#define BCMFW_R3D_TE         2           /* time GPIO 49 for MS ms, no drawing */
#define BCMFW_R3D_ST_DONE    0x33440002
#define BCMFW_R3D_ST_BAD     0x334400EE  /* refused: bad op or scene */
#endif

/* ---- The display jobs (BCMFW_JOBS: the merged image; jobs.md,
 * memory-contract.md).  Needs the video build (protocol, lcd_blit). */
#if BCMFW_JOBS
/* With BCMFW_VIDEO: the merged display image.  Without it: the
 * display-only image (gap G9: no decoder, ~20-25 KB, a video start loads
 * the video image by a warm swap).  Both need the protocol and the DMA. */
#if !BCMFW_HOSTPROTO
#error "BCMFW_JOBS needs BCMFW_HOSTPROTO (the rings)"
#endif
#undef BCMFW_LCD_DMA
#define BCMFW_LCD_DMA        1
#define BCMFW_JOBS_BLOCK     0xDE000     /* the job block (HP_JOBS) */
/* Block clocks a jobs image turns off at start (jobs_init): bit k = the
 * clock word 0x10001C2C + 4k, written pw 0 (Apple's off).  PW1 [silicon,
 * 2026-10-25] read 0x00000001 in +0x2C-+0x48 (+0x3C 0x8001) with our
 * image, where Apple's running vmcs reads 0 in every one; Apple's start-up
 * (@50a8) turns all eight off and runs the panel, DMA, host port, timer
 * and SDRAM without them (brief-map-power.md section 5).  Off: +0x2C
 * (id 0, no user), +0x30 (id 1, TV out: the plain image's probe only),
 * +0x40, +0x44 (ids 4, 5, no user), +0x48 (id 7, no user; its twin +0x50
 * already reads 0).  Kept, candidates: +0x34, +0x38 (ids 2, 3, the audio
 * block: the merged image's developer TONE, stage 8a-1, ran with them as
 * found) and +0x3C (id 6, USB, bit 15 a gate whose reset meaning is
 * unknown).  Each off is a [hypothesis] until PW1 reads it on silicon
 * with the panel, jobs and video still right (power-contract.md 6.8) */
#ifndef BCMFW_JOBS_CLK_OFF
#define BCMFW_JOBS_CLK_OFF   0xE3
#endif
/* The display-only image's layout: no decoder arena, so J_OP_DECODE's
 * work area (jdec_work(), jdec.h) sits between the image and the pools,
 * and J_CLS_THUMB moves to the top spare (no AAC tables are parked
 * there).  The pools' addresses are each image's own: the PP learns them
 * from ALLOC.  album-render.md section 5; build.py checks the layout. */
#if BCMFW_JOBS_STAGED
#define BCMFW_JOBS_DEC_WORK  0x00D400    /* the staged option (sim only): 2 KB
                                              more for its code; 232,448 B */
#else
#define BCMFW_JOBS_DEC_WORK  0x00CC00    /* to the drift pool: 234,496 B, far
                                              above jobs.h J_DEC_WORK_MIN (0xC200
                                              until 2026-10-25, then 0xC400; the
                                              image batch's 2 KB more image room:
                                              the colour stage's check, the lane
                                              shift by transposes) */
#endif
#define BCMFW_JOBS_DO_DRIFT  0x046000    /* J_CLS_DRIFT, 364,800, to 0x9F100 */
#define BCMFW_JOBS_DO_STRIP  0x09F200    /* J_CLS_STRIP, 131,072 */
#define BCMFW_JOBS_DO_APP    0x0BF200    /* J_CLS_APP, 52,736, to 0xCC000 */
#define BCMFW_JOBS_DO_THUMB  0x12B000    /* J_CLS_THUMB, 65,536, to 0x13B000 */
/* The visualizer image (BCMFW_VIZ; #126, proposed, sim only;
 * docs/research/bcm2722/visualizer.md): the display-only image's layout
 * and jobs with SPECTRUM and BARS (jobs/jviz.c, video/viz_vpu.c, the
 * vector FFT) in place of DECODE, which the visualizer doesn't need (the
 * PP's twin decodes meanwhile) and which leaves no room beside it.  Not
 * with the video build. */
#ifndef BCMFW_VIZ
#define BCMFW_VIZ            0
#endif
/* Tests only: the visualizer's start-up checks (viz_vpu.c) fail: the
 * FFT's expects the wrong powers and the bars' rejects its frames, so
 * J_VPU_FFT and J_VPU_BARS stay clear and SPECTRUM and BARS run the C */
#ifndef BCMFW_VIZ_FFT_FAIL
#define BCMFW_VIZ_FFT_FAIL   0
#endif
#if BCMFW_VIZ && BCMFW_VIDEO
#error "BCMFW_VIZ is a display-only image (no video)"
#endif
#if BCMFW_VIDEO
/* The merged image: the pools share the decoder arena (memory-contract.md
 * section 2), and DECODE has no room (album-render.md section 5) */
#define BCMFW_JOBS_DECODE    0
#define BCMFW_JOBS_DRIFT     0x035400    /* J_CLS_DRIFT, 364,800, to 0x8E500 */
#define BCMFW_JOBS_STRIP     0x08E600    /* J_CLS_STRIP, 131,072 */
#define BCMFW_JOBS_THUMB     0x0AE600    /* J_CLS_THUMB, 65,536 */
#define BCMFW_JOBS_APP       0x0BE600    /* J_CLS_APP, 52,736, to 0xCB400 */
#else
#define BCMFW_JOBS_DECODE    (!BCMFW_VIZ)
#define BCMFW_JOBS_DRIFT     BCMFW_JOBS_DO_DRIFT
#define BCMFW_JOBS_STRIP     BCMFW_JOBS_DO_STRIP
#define BCMFW_JOBS_THUMB     BCMFW_JOBS_DO_THUMB
#define BCMFW_JOBS_APP       BCMFW_JOBS_DO_APP
#define BCMFW_JOBS_DEC_WORK_SIZE (BCMFW_JOBS_DRIFT - BCMFW_JOBS_DEC_WORK)
#endif
/* The overlay region (jobs.h J_OP_OVERLAY; proposed, sim only;
 * visualizer.md section 8): the display-only layout's spare top 20 KB,
 * above J_CLS_THUMB.  Only the display-only image has one (0: none; -D
 * BCMFW_JOBS_OVL=0 builds the image without the stub); its overlays are
 * linked at its base (build.py build_overlay) */
#ifndef BCMFW_JOBS_OVL
#if BCMFW_JOBS_DECODE
#define BCMFW_JOBS_OVL       0x13B000
#elif BCMFW_VIDEO && BCMFW_JOBS_MERGED_OVL
/* MERGED IMAGE (next re-verify batch): the merged image's spare
 * 0x12B000-0x133FFF (memory-contract.md section 2: nothing there in UI
 * or video mode; the parked AAC tables start at 0x134000), for DECODE's
 * overlay (video/dec_ovl.c) */
#define BCMFW_JOBS_OVL       0x12B000
#else
#define BCMFW_JOBS_OVL       0
#endif
#endif
#if BCMFW_VIDEO
#define BCMFW_JOBS_OVL_SIZE  0x9000      /* 36,864, to 0x134000 */
#else
#define BCMFW_JOBS_OVL_SIZE  0x5000      /* 20,480, to 0x140000 */
#endif
#define BCMFW_JOBS_SCRATCH   0x0D4000    /* J_CLS_SCRATCH, 32,768 */
#define BCMFW_JOBS_BACK      0x105800    /* J_SURF_BACK in UI mode */
#if BCMFW_JOBS_STAGED
/* The staged upload's window (memory-contract.md section 3): 192 KB of
 * SDRAM, the spare above the AAC decoder's far data (VIDEO mode's dump
 * area, unused in UI mode), large enough for any slot of the SRAM pools
 * (a J_CLS_DRIFT cover: 182,400 B).  The core and the DMA engine read it
 * at the BCM's view, the PP writes it at the host port's; both proven
 * for SDRAM [silicon S12, S28-S34; PW8].  SDRAM comes up at the first
 * staged ALLOC (UI mode) */
#define BCMFW_JOBS_STAGE     0xFE3A0000  /* the BCM's view */
#define BCMFW_JOBS_STAGE_HOST 0xC03A0000 /* the host port's (ALLOC's reply) */
#define BCMFW_JOBS_STAGE_SIZE 0x30000    /* to 0x3D0000 */
/* COMMIT moves the window by the DMA engine's memcpy (PW8's words), the
 * core copying only after a timeout; 0: by the core alone (PW8's other
 * 0-wrong copy), in the merged image, which has no room for the DMA's
 * code below its arena */
#ifndef BCMFW_JOBS_STAGE_DMA
#define BCMFW_JOBS_STAGE_DMA (!BCMFW_VIDEO)
#endif
/* The merged image's room for the staged code: the self-test's low-SRAM
 * snapshot (selftest.c, 508 B, developer OPENs only) moves out of .bss
 * to the spare above the hibernate parameter block (memory-contract.md
 * section 2: 0xDF500-0xDFFFF, spare in every mode) */
#define BCMFW_JOBS_LOW_SNAP  0x0DF500
#endif
#endif

#define MB(a) (*(volatile uint32_t *)(a))
#endif

#endif /* BCMFW_CONFIG_H */
