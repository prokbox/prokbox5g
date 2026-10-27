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
 * BCM2722 device test (HAVE_BCMFW_DEVTEST builds only, issue #60):
 * staged first-light test of our own VideoCore image (bcmfw/), started by
 * hand from the developer debug menu.  Every stage ends by itself with
 * Apple's NOR vmcs and the normal display back.  Runbook:
 * docs/research/bcm2722/device-session-1.md (sessions 1 and 2).
 *
 * Stages and what each needs to have passed first (in the same run,
 * except 4b):
 *
 *   0 (Apple's vmcs, read-only) --> 1 (probe) --> 2 (picture) --> 3 (DMA)
 *                                         \
 *                                          +--> 4 (timing bench, no panel I/O)
 *
 *   4b (150 MHz switch, optional): stage 0 in the same run, plus a stage
 *      4 pass flag on disk for the embedded bench image.
 *   5 (the everyday path, dev7): stage 3 in the same run.  The DMA image
 *      as the display driver uses it: a scroll storm of UPDATERECTs,
 *      full frames, sleep/wake cycles, a stop on purpose and the
 *      fallback to Apple's vmcs.
 *
 * HAVE_BCMFW (configure --bcmfw, needs --bcmfw-devtest) adds the boot
 * path: with the debug menu's toggle set and the needed stage's pass flag
 * on disk (stage 2: CPU push, stage 3 or 5: DMA), the shell's start swaps
 * Apple's vmcs for the image that passed (bcmdt_boot_swap), and the stock
 * sleep/wake then reloads that image.  Off by default; HAVE_BCMFW_DEFAULT
 * (configure --bcmfw-default) makes the DMA image the default display
 * driver, with a stage 5 pass flag or a device-verified build
 * (pipeline.md, "The everyday path").
 *
 * The stage logic (bcm-devtest-core.c) is plain C over the small hardware
 * layer below, so the bcmfw bench runs the very same code against vc2sim
 * (bcmfw/tests/devtest_test.py).
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
#ifndef BCM_DEVTEST_H
#define BCM_DEVTEST_H

#include <stdint.h>
#include <stdbool.h>

struct bcmdt_image
{
    const uint8_t *data;        /* word aligned, padded to a multiple of 4 */
    uint32_t size;              /* bytes */
    uint32_t fnv;               /* FNV-1a 32 of the size bytes, from the build */
    const char *name;
};

/* What the stages need from the hardware.  Every call returns in bounded
 * time: the port calls give up after a few ms and return < 0. */
struct bcmdt_hal
{
    int  (*rd)(uint32_t addr, uint32_t *val);
    int  (*wr)(uint32_t addr, uint32_t val);
    int  (*wr_block)(uint32_t addr, const uint16_t *src, uint32_t halfwords);
    void (*doorbell)(void);     /* BCM_CONTROL = 0x31 after a command */
    /* power on, bootstrap, upload to 0, 0x1F8 = 0, cache flush, start */
    int  (*boot)(const uint8_t *img, uint32_t bytes);
    void (*power_cut)(void);    /* GPO32 &= ~0x4000 */
    /* take the BCM from the LCD driver (display on, Apple's vmcs up) */
    int  (*take)(void);
    /* give the BCM back: running = Apple's vmcs still up (stage 0 only);
     * otherwise the BCM is off and the stock wake path reloads it */
    void (*restore)(bool running);
    void (*light)(bool on);     /* backlight */
    uint32_t (*now_us)(void);
    void (*yield)(void);
    void (*sleep_ms)(uint32_t ms);
    void (*hold)(uint32_t ms);  /* keep the picture up; SELECT ends it */
    void (*log)(const char *line);  /* append one line, flushed to disk */
    const struct bcmdt_image *probe;    /* BCMFW_PROBE=1 */
    const struct bcmdt_image *hello;    /* default build */
    const struct bcmdt_image *hello_rb; /* BCMFW_RESET_BRANCH=1 */
    const struct bcmdt_image *hello_dma;    /* BCMFW_LCD_DMA=1 */
    const struct bcmdt_image *hello_dma_rb; /* ... and BCMFW_RESET_BRANCH=1 */
    /* the boot path's first frame: the host's framebuffer to the BCM's */
    int  (*upload_fb)(void);
    const struct bcmdt_image *bench;    /* BCMFW_BENCH=1 (stage 4) */
    const struct bcmdt_image *bench_pll; /* ... BCMFW_BENCH_PLL=1 (4b) */
    /* dev8, stage 5's upload lab (any of these NULL: the lab is skipped).
     * fb_upload: bytes of the host framebuffer, from byte offset off, to
     * BCM address addr, by method how (BCMDT_UP_*); fb: that framebuffer
     * (the lab reads words back to check them); boost: the PP's clock. */
    int  (*fb_upload)(uint32_t addr, uint32_t off, uint32_t bytes, int how);
    const uint16_t *(*fb)(void);
    void (*boost)(bool on);
    /* dev9: stage 4c's image (BCMFW_LCD_DMA=1 BCMFW_CLOCK_CMD=1), and a
     * phase mark in the developer power log (stage 5b; NULL: none) */
    const struct bcmdt_image *hello_dma_clk;
    void (*mark)(const char *tag);
    /* stage 5b's mode B: the drift image (BCMFW_DRIFT=1), and memory for
     * the PP's two covers: scratch(bytes) gives 'bytes' (NULL: none),
     * scratch(0) gives it back.  Either NULL: B is the dev9 stub. */
    const struct bcmdt_image *drift;
    void *(*scratch)(uint32_t bytes);
    /* stage 5i: the DMA image asleep between commands (BCMFW_IDLE_SLEEP,
     * BCMFW_IDLE_DOORBELL); NULL: stage 5i is refused */
    const struct bcmdt_image *hello_dma_idle;
    /* stage 5s, the sleep spikes: the idle image with panel standby
     * (BCMFW_LCD_STANDBY, its sleep gated by the host); and a boot whose
     * upload goes by method how (BCMDT_BOOT_*), timed, then every word
     * read back before the start: out[0] the upload in us, out[1] the
     * words that differ, out[2] the first one's address (~0: none),
     * out[3] the pieces the engine moved.  Either NULL: 5s is refused. */
    const struct bcmdt_image *hello_dma_sby;
    int  (*boot_how)(const uint8_t *img, uint32_t bytes, int how,
                     uint32_t out[4]);
    /* stage 3d, the 3D render test: the renderer image (BCMFW_R3D, with
     * CMD_CLOCK); NULL: stage 3d is refused */
    const struct bcmdt_image *hello_3d;
    /* bench row ALGN's port checks (stage 4, after the bench): n
     * halfwords to BCM address addr in one of the port's forms (or'd
     * BCMDT_PA_*), any n, any addr; NULL: the port checks are skipped */
    int  (*port_algn)(uint32_t addr, const uint16_t *src, uint32_t n,
                      unsigned how);
};

/* port_algn's forms (docs/research/bcm2722/bench-p19.md, "ALGN") */
#define BCMDT_PA_ADDR16   0x01  /* WR_ADDR as two halfword stores (Apple's) */
#define BCMDT_PA_DATA16   0x02  /* data as halfword stores (Apple's PIO);
                                   else 32-bit stores, a lone last halfword
                                   as a word with a zero high half (ours) */
#define BCMDT_PA_NOPOLL   0x04  /* no CONTROL poll after the address */
#define BCMDT_PA_E2       0x08  /* the whole words by engine 2, waited for,
                                   then the tail (if any) as above */
#define BCMDT_PA_NODRAIN  0x10  /* with E2: the engine's end only, not the
                                   port's write-ready, before the tail */

/* Boot uploads (stage 5s): the CPU copy, or the PP's second DMA engine
 * (e2-port.c, session 37's recipe) in pieces of up to 64 KB */
enum { BCMDT_BOOT_CPU, BCMDT_BOOT_E2, BCMDT_BOOT_N };

/* Upload methods (bcm-devtest-hal.c, bcm-upload.S): Rockbox's own
 * lcd_write_data (2-register ldm/stm pairs), 8-register bursts, and the
 * PP5022's DMA engine (channel 3, memory to the fixed data port) */
enum { BCMDT_UP_STOCK, BCMDT_UP_BURST8, BCMDT_UP_PPDMA, BCMDT_UP_N };

#define BCMDT_3D_SCENES  5     /* stage 3d's scenes (bcmfw/r3d) */

struct bcmdt_result
{
    int passed;                 /* highest stage passed, -1 none */
    int panel_id;               /* -1 until stage 0 read it */
    uint32_t gpio_lev;          /* 0x10002824 with Apple's vmcs */
    uint32_t info;              /* our image's info word, 0x1E8 */
    uint32_t selfcheck, selfcheck_all, from0, probe_ticks;
    uint32_t fault_pc, fault_sp;
    uint32_t apple_us;          /* Apple's LCD_UPDATE, kick to done */
    uint32_t ready_us, first_us, frame_us, rect_us, sleep_us, wake_us;
    /* stage 3 (and the boot path's DMA image): host-timed kick to done,
     * then what the image reports (bcmfw/config.h, 0x1A4-0x1BC) */
    uint32_t dma_first_us, dma_frame_us, dma_rect_us, forced_us;
    uint32_t dma_bcm_us, dma_bcm_rect_us, forced_bcm_us;
    uint32_t dma_done, dma_tmo, dma_cs, dma_int, dma_cb, dma_polls;
    char why[64];               /* the first failure, or "" */
    /* the images that passed stages 2 and 3 (for the pass flags) */
    const struct bcmdt_image *img2, *img3;
    /* --- stages 4 and 4b: the timing bench ---------------------------- */
    unsigned passed_mask;       /* bit s: stage s passed (4b: bit 5) */
    const struct bcmdt_image *img4;     /* passed stage 4 (its flag) */
    uint32_t b_step;            /* the last step word seen (0x1C0) */
    uint32_t b_us;              /* ready to done, host-timed */
    uint32_t b_flags;           /* results word 5 */
    uint32_t b_vpass, b_vall, b_spass, b_sall, b_ppass, b_pall;
    uint32_t b_nk, b_kernels_ok;        /* kernels, and with 0 < min <= max */
    uint32_t b_clock_khz;       /* ALU1 instructions / fastest ticks */
    uint32_t b_clock2_khz;      /* 4b: ALU1 at 150 MHz, same estimate */
    uint32_t b_start_khz;       /* 4b: start-up clock, 150 MHz x t2 / t1 */
    /* --- probe v2 (stage 1, and a probe/bench image's fault) ---------- */
    uint32_t p_faulted;         /* 0x1D8: bit n = check n faulted */
    uint32_t p_block;           /* 0x1DC: the results block, 0 if none */
    uint32_t p_pass;            /* block word 4: pass bits, all checks */
    uint32_t p_u_status;        /* record U's status word (state 0: none) */
    /* --- stage 5: the everyday path (dev7) ---------------------------- */
    const struct bcmdt_image *img5;     /* passed stage 5 (its flag) */
    uint32_t e_host_us;         /* host upload of a whole frame */
    uint32_t e_rects;           /* storm rectangles the image counted */
    uint32_t e_rect_max_us;     /* slowest list-area push */
    uint32_t e_full_us;         /* slowest full-frame push */
    uint32_t e_wakes, e_wake_max_us;    /* sleep/wake cycles, slowest wake */
    uint32_t e_stop_us;         /* stop command to fault word */
    uint32_t e_fallback_us;     /* power cut to Apple's vmcs back */
    /* --- stage 5, dev8 ------------------------------------------------ */
    uint32_t e_draw_us;         /* drawing the test pattern, one frame */
    uint32_t e_lab_us[2][BCMDT_UP_N];   /* full frame by method, [boost] */
    uint32_t e_lab_bad;         /* bit 3 * boost + method: readback differs */
    uint32_t e_row_us;          /* per-row address setup + 2 pixels, x 1000 */
    uint32_t e_ov_wake_max_us;  /* slowest wake with LCD_POWERON first */
    /* --- dev9: stage 4c (the 150 MHz clock with the panel), 5b -------- */
    const struct bcmdt_image *img4b;    /* passed stage 4b (its flag) */
    const struct bcmdt_image *img4c;    /* passed stage 4c (its flag) */
    uint32_t c_up_us, c_down_us;        /* the switches, image-timed */
    uint32_t c_base_us, c_fast_us, c_slow_us;  /* DMA frames: crystal,
                                                  PLL, crystal again */
    uint32_t m_frames[2], m_late[2], m_pp_us[2];   /* 5b: mode A, B */
    uint32_t m_bcm_us[2], m_wait_us[2]; /* 5b: the BCM's busy time (its push,
                                           or its op), the PP's wait for it */
    /* --- stage 5i: idle sleep ------------------------------------------ */
    const struct bcmdt_image *img5i;    /* passed stage 5i (its flag) */
    uint32_t i_nop_us[2];       /* no-op kick to done, avg: busy, idle */
    uint32_t i_rect_max_us[2];  /* slowest clock rect: busy, idle */
    uint32_t i_ticks;           /* idle ticks over the hold */
    uint32_t i_bells, i_sleeps, i_bad;  /* idle image's counters at the end */
    uint32_t i_late;            /* hold updates later than busy + a tick */
    uint32_t i_fb_bad;          /* framebuffer words changed over the hold */
    /* --- stage 5s: the sleep spikes (Phase 18-0) ----------------------- */
    uint32_t s_cold_us, s_cold_pon_us;  /* first frame, cold panel: host
                                           kick to done, the image's
                                           power-on time */
    uint32_t s_reset_us, s_reset_pon_us;    /* the same after LCD_SLEEP with
                                               the BCM kept up */
    uint32_t s_wake_us[2], s_pon_us[2];     /* light, deep standby: the
                                               slowest wake, both clocks */
    uint32_t s_refused;         /* bit d: standby depth d refused */
    uint32_t s_fb_bad;          /* framebuffer words changed in standby */
    uint32_t s_slept;           /* 1: the image slept between commands */
    uint32_t s_ticks, s_sleeps, s_bad;  /* its idle counters at the end */
    uint32_t s_up_us[BCMDT_BOOT_N];     /* the big upload by method */
    uint32_t s_ready_us[BCMDT_BOOT_N], s_first_us[BCMDT_BOOT_N];
    uint32_t s_up_bad[BCMDT_BOOT_N];    /* words read back wrong */
    uint32_t s_up_bytes, s_pieces;
    /* --- stage 3d: the 3D render test ---------------------------------- */
    uint32_t d_tris[BCMDT_3D_SCENES];   /* each scene's triangles */
    /* [crystal 27 MHz, PLL 150 MHz][scene] */
    uint32_t d_frames[2][BCMDT_3D_SCENES];  /* frames shown */
    uint32_t d_us[2][BCMDT_3D_SCENES];      /* the run's length */
    uint32_t d_render[2][BCMDT_3D_SCENES];  /* rendering, summed */
    uint32_t d_max[2][BCMDT_3D_SCENES];     /* the slowest frame */
    uint32_t d_late[2][BCMDT_3D_SCENES];    /* frames past their 30 fps slot */
    uint32_t d_drawn[2][BCMDT_3D_SCENES];   /* triangles drawn, summed */
    uint32_t d_crc_bad;         /* check frames unlike the simulator's */
    uint32_t d_fast;            /* 1: the 150 MHz runs were made */
    uint32_t d_te_edges;        /* TE (GPIO 49) rising edges in the probe */
    uint32_t d_te_us;           /* the panel's refresh period, 0 unknown */
    uint32_t d_sync;            /* the TE-synced run: edges a frame, 0 none */
    uint32_t d_sync_frames, d_sync_us, d_sync_late;
};

#define BCMDT_LAST_STAGE 4
#define BCMDT_STAGE_4B   5      /* bit index in stages / passed_mask */
#define BCMDT_STAGE_5    6      /* the everyday path (needs stage 3) */
#define BCMDT_STAGE_4C   7      /* dev9: 150 MHz with the panel (stage 1,
                                   a 4b flag) */
#define BCMDT_STAGE_5B   8      /* dev9: drift offload A/B (stage 5 flag) */
#define BCMDT_STAGE_5I   9      /* idle sleep (stage 5 flag, like 5b) */
#define BCMDT_STAGE_5S   10     /* the sleep spikes (stage 5 flag; the
                                   image sleeps only with a 5i flag) */
#define BCMDT_STAGE_3D   11     /* the 3D render test (stage 5 flag; 150 MHz
                                   runs only with 4b and 4c flags) */
/* Pass-flag codes for the lettered stages: "4b", "4c", "5i" in the files */
#define BCMDT_FLAG_4B    41
#define BCMDT_FLAG_4C    42
#define BCMDT_FLAG_5I    58
#define BCMDT_STAGE(s)   (1u << (s))

/* Run stages 0..last (0 to 4): bcmdt_run_stages() with stages 0..last */
int bcmdt_run(const struct bcmdt_hal *hal, int last, struct bcmdt_result *res);

/* Run the stages in the mask (stage 0 always), each only if the one it
 * needs passed earlier in this run (see the graph above); a skipped stage
 * is logged and counts as not passed.  The gated stages need a pass flag
 * in flags (the pass flag files' lines, one a line): 4b the stage 4 flag
 * naming hal->bench, 4c a 4b flag naming hal->bench_pll (or 4b passing
 * in this run), 5b, 5i and 5s a stage 5 flag naming hal->hello_dma (or
 * stage 5 in this run; 5s lets its image sleep only on a 5i flag naming
 * hal->hello_dma_idle, or 5i passing in this run); without it nothing is done and -3 is returned.  Returns the highest stage s (0..4) such
 * that every requested stage up to s passed (-1 none), or -2 if the BCM
 * could not be taken (nothing done).  Otherwise always returns with the
 * BCM given back. */
int bcmdt_run_stages(const struct bcmdt_hal *hal, unsigned stages,
                     const char *flags, struct bcmdt_result *res);

/* The stage 2 picture, one row (320 RGB565 pixels); and the rectangle
 * that the partial update draws (x, y, w, h and its pixels by row). */
void bcmdt_pattern_row(int y, uint16_t *row);
/* stage 5b's generated cover n, pixel (u, v) (the tests' view of it) */
uint16_t bcmdt_drift_px(unsigned n, int u, int v);
/* stage 5b's half-step guard (the shell's HALF_SLOW_US/MAX; tests only) */
void bcmdt_set_half_guard(uint32_t us, int max);
void bcmdt_rect_row(int y, uint16_t *row);
void bcmdt_fine_row(int y, uint16_t *row);
#define BCMDT_RECT_X 200
#define BCMDT_RECT_Y 170
#define BCMDT_RECT_W 100
#define BCMDT_RECT_H 50
/* stage 3's rectangle (same pixels), over the colour bars: a sheared
 * 2-D DMA shows against them */
#define BCMDT_RECT3_X 10
#define BCMDT_RECT3_Y 20

/* ---- The HAVE_BCMFW boot path ------------------------------------------ */

/* With Apple's vmcs running and the BCM taken: power it down, start img,
 * send the host's frame (hal->upload_fb) and wait for it to be done.
 * 0 = our image runs and showed the frame.  Otherwise < 0, the BCM is
 * powered off and the caller restores Apple's vmcs (hal->restore(false)).
 * Every wait is bounded. */
int bcmdt_boot_swap(const struct bcmdt_hal *hal, const struct bcmdt_image *img,
                    struct bcmdt_result *res);

/* The fallback ladder (#62 level 1, and the resident image's rung in a
 * build with it) */
/* HAVE_BCMFW_RESIDENT comes from config/ipodvideo.h, which the device
 * test's core (compiled for the host too) doesn't include: its condition
 * is mirrored here from the command-line defines, so every unit agrees
 * (a release with the player, --bcmfw-video without --bcmfw-l1, linked
 * bcmfw-boot.c's ladder call against a core that left it out) */
#if defined(HAVE_BCMFW_L1) || defined(HAVE_BCMFW_RESIDENT) || \
    ((defined(HAVE_BCMFW_VIDEO) || defined(HAVE_BCMFW_MERGED_VIDEO)) && \
     !defined(BOOTLOADER) && !defined(SIMULATOR))
#define BCMDT_LADDER 1
#endif

/* Images up to this size boot (the video image's region ends here; the
 * test images are far smaller) */
#define BCMDT_IMG_MAX 0x80000

#ifdef BCMDT_LADDER
/* The fallback ladder: imgs[0] by bcmdt_boot_swap (from Apple's vmcs),
 * then each later non-NULL rung started cold after the one before failed
 * (the resident video image, our DMA image, our CPU-push image).  The
 * index of the rung whose first frame was confirmed; -1 when every rung
 * failed: the BCM is off and the caller restores Apple's vmcs. */
int bcmdt_boot_ladder(const struct bcmdt_hal *hal,
                      const struct bcmdt_image *const *imgs, int n,
                      struct bcmdt_result *res);
#endif

/* bcmtest.log this boot: lines, failed opens, failed writes or closes,
 * the last failure's code (-1000 a write, -2000 a close, else open's),
 * the bytes written this boot and the file's size now */
void bcm_devtest_log_stats(uint32_t out[6]);

#ifdef HAVE_BCMFW_VIDEO
/* The video player's image (apps/video/vpcore.c's backend): stop what
 * runs in the stock order (running) or start from power off, upload and
 * start img, then the panel on with the host's frame.  0 once it reports
 * ready; otherwise < 0 with the BCM powered off.  Every wait is bounded. */
int bcmdt_video_boot(const struct bcmdt_hal *hal, const uint8_t *img,
                     uint32_t bytes, bool running);
/* Power off: the stock order while the image answers (alive), else cut */
void bcmdt_video_off(const struct bcmdt_hal *hal, bool alive);

/* lcd-video.c (target): the video player's hold on the BCM, for
 * apps/video/vplayer.c.  take: from the LCD driver, as the device test
 * takes it (its updates stop); boot: bcmdt_video_boot(); off: power off;
 * release: back to the LCD driver through the stock wake path, which
 * repaints the screen; the port calls are the device test's, bounded. */
int bcm_video_take(void);
int bcm_video_boot(const uint8_t *img, uint32_t bytes);
void bcm_video_off(void);
/* Bench row B13: power cut now, no stock-order power-down (the image is
 * parked and answers nothing), then the 50 ms off rule */
void bcm_video_cut(void);
void bcm_video_release(void);
int bcm_video_rd(uint32_t addr, uint32_t *val);
int bcm_video_wr(uint32_t addr, uint32_t val);
int bcm_video_wr_block(uint32_t addr, const uint16_t *src, uint32_t n);
/* dev39: blocks of 256 bytes and up by PP DMA, left running (the next port
   access waits); flush waits now.  The load: bytes moved, the time the
   CPU copied (and its blocks), the time from each DMA's start to its end
   seen (and its blocks), and DMAs that never ended (the CPU from then) */
struct bcm_video_port_load
{
    uint32_t bytes, cpu_us, cpu_blocks, dma_us, dma_blocks, dma_fail;
};
void bcm_video_set_dma(bool on);
/* The port's blocks by the PP's second DMA engine (session 37), with the
 * CPU's copy for small blocks and after any failure; false: the CPU */
void bcm_video_set_dma_e2(bool on);
void bcm_video_port_load(struct bcm_video_port_load *out, bool reset);
int bcm_video_flush(void);
/* Bench row CR1: a write in Apple's forms, by the CPU (n even); or'd */
#define BCM_WRV_ADDR16  0x1     /* WR_ADDR as two halfwords, low then high */
#define BCM_WRV_DATA16  0x2     /* the data as halfword stores */
#define BCM_WRV_NOPOLL  0x4     /* no CONTROL poll after the address */
int bcm_video_wr_variant(uint32_t addr, const uint16_t *src, uint32_t n,
                         unsigned how);
void bcm_video_set_dma_fixup(bool on);   /* the DMA lab: word 0 as DMA left it */
void bcm_devtest_iis_tx(bool on);   /* 8a-0b: the PP's I2S transmitter off/on */
uint32_t bcm_video_now_us(void);
/* The port's handshakes since the last reset (dev12): longest wait in us
 * for a read's address, a read's data and a write; timeouts; then at the
 * last timeout its kind (1-3), address, CONTROL and RD_ADDR */
void bcm_video_port_stats(uint32_t out[8], bool reset);
/* Stage 6 (p14-player): its precondition, a stage 5 pass flag naming the
 * embedded DMA image (false with *why otherwise), and one line to the
 * device test's log (bcmtest.log) */
bool bcm_devtest_6_allowed(const char **why);
void bcm_devtest_log(const char *line);
/* p14-resident: the panel back to the LCD driver with the image left
 * running, when it is the boot path's resident image and answers (after
 * the session's CLOSE); the driver repaints the whole frame.  < 0: not
 * possible, the caller cuts the power and releases as before. */
int bcm_video_hand_back(void);
/* The resident image's idle counters as read at the last take: ticks,
 * doorbells, sleeps, bad sleeps; c[4] 1 if read (the image was resident) */
void bcm_video_idle_counters(uint32_t c[5]);
/* The power map's probes (brief-map-power.md section 9).  took_ours: the
 * last take found our image running, not Apple's vmcs (PW1's context).
 * pp_regs: GPO32_VAL, GPO32_ENABLE, 0x70000088, STRAP_OPT_A, 0x70000040,
 * read only (PW1 step 6).  alt_lab: PW7, the ALT bank, with an image of
 * ours idle: writes by the main bank, by the ALT bank and interleaved
 * into 0x12C000-0x12CFFF, each area read back by both banks (halfwords
 * right out of 256), then a frame into the host frame by each; 0 or < 0
 * (fail_step set). */
struct bcm_alt_result
{
    uint32_t main_ok[4], alt_ok[4];     /* areas: main, ALT, inter. main,
                                           inter. ALT */
    uint32_t frame_us[2];               /* main, ALT */
    bool alt_ready;                     /* ALT_CONTROL bit 1 ever set */
    bool dead;                          /* a read gave 0xFFFFFFFF */
    int fail_step;
};
bool bcm_video_took_ours(void);
void bcm_video_pp_regs(uint32_t out[5]);
/* PW5: GPO32 bits 14 and 15 (bcmdt_hib_hal's gpo and gpo_enable); PW6:
 * the port's registers read raw (DATA x4, CONTROL, ALT_DATA x4,
 * ALT_CONTROL), no address written */
uint32_t bcm_video_gpo32(uint32_t clear, uint32_t set);
uint32_t bcm_video_gpo32_enable(void);
void bcm_video_port_raw(uint16_t out[10]);
int bcm_video_alt_lab(struct bcm_alt_result *r);
#endif

/* ---- PW5: Apple's hibernate pins (brief-map-power.md sections 3, 9) ----
 *
 * The PP's half of the hibernate probe, as plain C over its own small
 * HAL so the host test drives it against vc2sim.  Apple's order only
 * (osos 1.3's state machine, host 0x1028_92D4, and its three-mode helper,
 * host 0x1026_668C): retain = GPO32 bit 15 high, 5,000 us, bit 14 low;
 * on = bit 14 high, 5,000 us, bit 15 low; then the 0x1F0 handshake
 * (0xA5A50001 written, 0xA5A50002 awaited).  Nothing undocumented is
 * added: no other pin, no other order, no extra wait between the pins.
 *
 * bcmdt_hib_cycle: refuses (nothing done) unless GPO32_ENABLE has bits
 * 14 and 15 and GPO32 reads bit 14 high, bit 15 low; writes 0x1F0 = 0,
 * sends the park (hal->send), waits up to BCMDT_HIB_PARK_MS for
 * 0xA5A50000, then the retain pins, hold_ms with no port access at all,
 * the on pins, and the handshake (BCMDT_HIB_WAKE_MS for the word to
 * answer, as much again for 0xA5A50002).  Any failure after the first
 * pin ends in the standard cut: bit 15 low, bit 14 low, 100 ms; the
 * caller then cold-boots the BCM as after any probe.  A failure before
 * it leaves the pins alone.  Returns BCMDT_HIB_*: 0 awake, < 0 not. */
#define BCMDT_HIB_WORD      0x1F0       /* bcmfw hostproto.h HP_HIB_* */
#define BCMDT_HIB_PARKED    0xA5A50000u
#define BCMDT_HIB_WAKE      0xA5A50001u
#define BCMDT_HIB_AWAKE     0xA5A50002u
#define BCMDT_HIB_GAVEUP    0xA5A500FFu
#define BCMDT_GPO_RUN       0x4000u     /* GPO32 bit 14: the BCM's power */
#define BCMDT_GPO_RETAIN    0x8000u     /* GPO32 bit 15: Apple's retain */
#define BCMDT_HIB_PIN_US    5000        /* Apple's wait between the pins */
#define BCMDT_HIB_PARK_MS   300         /* the image's patterns, bars and
                                           park; the spec's 100 ms counted
                                           none of them */
#define BCMDT_HIB_WAKE_MS   200
#define BCMDT_HIB_OFF_MS    100         /* the standard cut's wait */

struct bcmdt_hib_hal
{
    int  (*rd)(uint32_t addr, uint32_t *val);   /* bounded; < 0: port */
    int  (*wr)(uint32_t addr, uint32_t val);
    int  (*send)(void *ctx);            /* the park request; < 0: not sent */
    /* GPO32_VAL: clear, then set, bits 14 and 15 only (others ignored),
     * read-modify-write with interrupts off; returns GPO32_VAL after.
     * (0, 0) only reads. */
    uint32_t (*gpo)(uint32_t clear, uint32_t set);
    uint32_t (*gpo_enable)(void);       /* GPO32_ENABLE, read only */
    uint32_t (*now_us)(void);
    void (*udelay)(uint32_t us);        /* busy: nothing else runs */
    void (*sleep_ms)(uint32_t ms);      /* the hold: other threads may run */
    void (*yield)(void);
    void *ctx;
};

enum
{
    BCMDT_HIB_OK = 0,       /* awake: the image answered 0xA5A50002 */
    BCMDT_HIB_REFUSED = -1, /* the pins not as expected: nothing done */
    BCMDT_HIB_E_SEND = -2,  /* the request not sent: pins untouched */
    BCMDT_HIB_E_PARK = -3,  /* no 0xA5A50000 in time: pins untouched */
    BCMDT_HIB_E_DEAD = -4,  /* after the pins: the port gave nothing, or
                               all ones (a chip back in its boot ROM) */
    BCMDT_HIB_E_LOST = -5,  /* after the pins: the word read something
                               else (SRAM not kept) */
    BCMDT_HIB_E_WAKE = -6,  /* the word held, but no 0xA5A50002 */
};

struct bcmdt_hib_result
{
    int end;                /* BCMDT_HIB_* */
    uint32_t gpo0, en0;     /* GPO32_VAL and _ENABLE at entry */
    uint32_t park_us;       /* sent to 0xA5A50000 seen */
    uint32_t both_us[2];    /* bits 14 and 15 both high: in, out */
    uint32_t hold_us;       /* bit 14 low to bit 14 high */
    uint32_t word_after;    /* 0x1F0's first read after the pins */
    uint32_t answer_us;     /* wake word written to 0xA5A50002 seen */
    uint32_t wake_us;       /* bit 14 high to 0xA5A50002 seen */
    uint32_t gpo_end;       /* GPO32_VAL & 0xC000 at the end: 0x4000
                               awake, 0 after the cut */
    bool pins;              /* bit 15 was driven */
    bool cut;               /* the standard cut ran */
};
int bcmdt_hib_cycle(const struct bcmdt_hib_hal *h, uint32_t hold_ms,
                    struct bcmdt_hib_result *r);

/* Pass flags: one line, "<stage> <image name> <size hex> <fnv hex>".
 * format returns the length written. */
int bcmdt_flag_format(char *buf, int len, int stage,
                      const struct bcmdt_image *img);
/* The image the flag names, only if it is one of imgs[n], its size and
 * FNV-1a match the flag, and the FNV-1a recomputed over the embedded data
 * matches too; NULL otherwise, with *why set. */
const struct bcmdt_image *bcmdt_flag_match(const char *text, int stage,
                                           const struct bcmdt_image *const *imgs,
                                           int n, const char **why);

uint32_t bcmdt_fnv(const uint8_t *p, uint32_t n);

/* The boot path's strikes (bcm-devtest-hal.c; dev8).  A fallback from our
 * image to Apple's vmcs at run time is a strike, at most one a boot,
 * unless it was asked for (the debug menu's "test fallback", or the
 * device test taking the BCM): those exercise the same path and count
 * nothing.  strike_next: the count to write at the idle flush, given the
 * count on disk and this boot's real and deliberate fallbacks; -1 when
 * nothing is to be written.  strike_blocks: the boot swap stops. */
#define BCMDT_STRIKES 3
int bcmdt_strike_next(int on_disk, int real, int deliberate);
bool bcmdt_strike_blocks(int on_disk);

/* p19-bcm-release.  res_gate: the resident rung's gate, NULL when it may
 * be tried, else why not; need_verified (a build without the device test)
 * takes only an image verified.txt lists.  rearm_due: a build without the
 * device test (no debug menu to choose a mode) clears the attempt marker
 * and the strikes once, at the first boot of a new build: true when the
 * build ID on disk (stored, NULL if none) is not this build's.  Recorded
 * before anything is cleared, so a build that crashes the boot never
 * clears them again. */
const char *bcmdt_res_gate(bool setting, bool offered, bool failed,
                           bool need_verified, bool verified);
bool bcmdt_rearm_due(const char *stored, const char *build);

/* bcm-devtest-images.c (generated by bcmfw/mkdevtest.py) */
extern const struct bcmdt_image bcmdt_probe_image, bcmdt_hello_image,
                                bcmdt_hello_rb_image, bcmdt_hello_dma_image,
                                bcmdt_hello_dma_rb_image, bcmdt_bench_image,
                                bcmdt_bench_pll_image, bcmdt_hello_dma_clk_image,
                                bcmdt_drift_image, bcmdt_hello_dma_idle_image,
                                bcmdt_hello_dma_sby_image, bcmdt_hello_3d_image,
                                bcmdt_jobs_display_image,
                                bcmdt_hibstub_image;  /* not an image: the
                                   hibernate stub (bcm-hibwake.h) */
/* The visualizer's overlay (#126) built with jobs_display, shipped beside
 * it as viz-ovl.bin (tools/buildzip.pl; bcmfw/ovlpair.py checks the pair):
 * the file's size, FNV-1a and header ABI word, the image's own copy of
 * that word (its byte offset: the stub's compare), the image's FNV-1a and
 * the region it was linked at (the image's J_OVL base).  The job driver
 * checks the file and the running image against it before a load
 * (bcmjob-core.h bjc_ovl_begin) */
struct bcmdt_ovl
{
    uint32_t bytes, fnv, abi, abi_at, image_fnv, base;
};
extern const struct bcmdt_ovl bcmdt_viz_ovl;
/* The FNV-1a of the images bcmfw/verified.txt lists as passed on a
 * device (stage 5); the default boot path takes the DMA image without a
 * pass flag only if it is one of them */
extern const uint32_t bcmdt_verified_fnv[];
extern const int bcmdt_n_verified;

/* lcd-video.c (target): take the BCM, run, give it back.  Writes or voids
 * the pass flags of the stages it ran. */
int bcm_devtest_run(int last, struct bcmdt_result *res);
/* The same for a stage mask (BCMDT_STAGE()); reads the stage 4 flag for
 * 4b and writes or voids the flags of stages 2, 3 and 4 */
int bcm_devtest_run_stages(unsigned stages, struct bcmdt_result *res);
/* PB-6 (brief-map-peripherals.md): Apple's NOR vmcs shows a BMP on the
 * TV.  Our image, if it drives the display, steps down to Apple's vmcs
 * first (for the rest of this boot); then the BCM is taken, bmp (bytes,
 * a multiple of 4, word aligned) goes to host 0xC020_0000, TV_PALBMP or
 * TV_NTSCBMP to 0x1F8, then TV_MVOFF, each polled up to limit_us; then
 * held hold_ms (SELECT ends it); then a power cut and the stock wake, on
 * every path once taken.  0: it ran (r->upload says whether the BMP
 * went through); -1 the take was refused, -2 bad arguments, -3 our image
 * could not be dropped: nothing touched in all three. */
struct bcm_tvpat_result
{
    bool forced;                /* our image was dropped for Apple's */
    bool ran;                   /* the commands went out */
    int upload;                 /* 0 or < 0 (the port) */
    uint32_t head[2];           /* 0xC020_0000's first two words, read back */
    bool cmd_port, mv_port;     /* every port access went through */
    uint32_t cmd_st, cmd_us;    /* 0x1F8 at the end of the BMP command */
    uint32_t mv_st, mv_us;      /* and of TV_MVOFF */
    uint32_t held_ms;
};
int bcm_devtest_tv_pattern(const uint16_t *bmp, uint32_t bytes, bool pal,
                           uint32_t limit_us, uint32_t hold_ms,
                           struct bcm_tvpat_result *r);
/* Stage 4b's precondition: a stage 4 pass flag that names the embedded
 * bench image.  false with *why otherwise. */
bool bcm_devtest_4b_allowed(const char **why);
/* dev9: stage 4c's precondition (a 4b flag naming the embedded 150 MHz
 * bench), and a video session's at 150 MHz (4b and 4c flags) */
bool bcm_devtest_4c_allowed(const char **why);
bool bcm_devtest_fast_clock_allowed(const char **why);
/* Removes the 4c pass flag (the bench session, on the owner's "wrong") */
void bcm_devtest_forget_4c(void);
/* Stage 5i's precondition: a stage 5 pass flag naming the DMA image */
bool bcm_devtest_5i_allowed(const char **why);
/* Stage 5s's: the same; *sleep: a 5i pass flag names the idle image (the
 * spikes then let their image sleep between commands) */
bool bcm_devtest_5s_allowed(const char **why, bool *sleep);
/* Stage 3d's: the same; *fast: 4b and 4c flags allow the 150 MHz runs */
bool bcm_devtest_3d_allowed(const char **why, bool *fast);
/* dev9: the developer power log's phase mark for stage 5b (the shell
 * sets it: powerlog_mark); NULL clears it */
void bcm_devtest_set_mark(void (*fn)(const char *tag));

#ifdef HAVE_BCMFW
/* The boot path's toggle (debug menu), in /.rockbox/bcmfw.cfg */
enum bcmfw_mode { BCMFW_MODE_OFF = 0, BCMFW_MODE_APPLE = 1, BCMFW_MODE_CPU = 2,
                  BCMFW_MODE_DMA = 3 };
/* Set the toggle and clear the attempt marker and the strikes (re-arms
 * the path).  OFF removes the file: the build's default, which is DMA
 * with HAVE_BCMFW_DEFAULT; APPLE forces Apple's vmcs in any build. */
void bcm_boot_set_mode(enum bcmfw_mode mode);
enum bcmfw_mode bcm_boot_get_mode(void);
/* The shell's start, disk up: the swap when toggled on and allowed */
void bcm_boot_switch(void);
/* Mark our running image as stopped, as lcd_tick() does on the fault
 * word: the next update takes the real fallback (power cut, Apple's NOR
 * vmcs through the stock wake path).  No-op unless active. */
void bcm_boot_test_fallback(void);
/* Status for the debug menu: one line per call, k = 0.. until NULL */
const char *bcm_boot_status(int k, char *buf, int len);
#ifdef HAVE_BCMFW_RESIDENT
/* p14-resident: the video image (/.rockbox/bcmvideo.bin) as the display
 * driver, in place of the boot image, so a video starts with no power
 * cycle.  The developer setting (/.rockbox/bcmres.cfg; no file: on, idle
 * sleep off) applies at the next boot.  *idle: the image may sleep its
 * core between commands while it serves the UI. */
bool bcm_boot_resident_setting(bool *idle);
void bcm_boot_resident_set(bool on, bool idle);
/* apps/video/bcmres.c, before the swap: the image it read and verified
 * (size and FNV-1a as built), in memory that stays put for good (pinned);
 * read_us: what reading it took.  Without an offer there is no resident
 * rung. */
void bcm_boot_offer_resident(const uint8_t *data, uint32_t bytes,
                             uint32_t fnv, uint32_t read_us);
/* The image driving the display now is the resident video image */
bool bcm_boot_resident_active(void);
/* Its FNV-1a is listed in bcmfw/verified.txt (checked once a boot) */
bool bcm_boot_resident_verified(void);
/* p19-battery, lever PW3 (brief-map-power.md section 4): the resident
 * image's core regulator at level 5 (Apple's for 27 MHz) instead of the
 * 11 a host start leaves; applied at the swap and every wake, only on
 * the crystal.  Same file as above ("level 5"); default off until PW3
 * and the bench row pass on silicon.  Next boot. */
bool bcm_boot_level5_setting(void);
void bcm_boot_level5_set(bool on);
/* The bench's A/B, now and not saved (a wake keeps it until the boot):
 * idle -1 keep, 0 poll, 1 sleep (the 0x16C gate); level 0 keep, else
 * the regulator level (5..15), *level_rb the level read back (-1).
 * 0 done; -1 the resident image isn't driving a lit display; -2 the
 * level was refused (not on the crystal, or a port read failed) */
int bcm_boot_res_live(int idle, int level, int *level_rb);
/* The idle counters (ticks, doorbells, sleeps, bad: bcmfw/config.h) and
 * the regulator word, read live; -1 as above */
int bcm_boot_res_read(uint32_t c[4], uint32_t *reg);
/* One bare doorbell (BCM_CONTROL = 0x31, no ring record: the image wakes,
 * finds nothing new and sleeps again), for the bench's lost-doorbell
 * count; -1 as above */
int bcm_boot_res_bell(void);
/* What this boot's resident image runs with now (RAM) */
void bcm_boot_res_now(bool *idle, bool *level5);
#endif
#ifdef HAVE_BCMFW_L1
#include "bcmfw-boot.h"         /* #62 level 1 */
#endif
#endif

#endif /* BCM_DEVTEST_H */
