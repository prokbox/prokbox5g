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
 * bcmfw video sessions (BCMFW_VIDEO): the H.264 decoder behind the host
 * protocol, the display path and the memory map they share
 * (docs/research/bcm2722/pipeline.md).
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
#ifndef BCMFW_VIDEO_H
#define BCMFW_VIDEO_H

/* ---- Memory map (hostproto.md §7) -------------------------------------- */
/* SDRAM, 4 MB: the BCM's view and the host port's view of the same RAM */
#define VIDEO_SDRAM          0xFE000000
#define VIDEO_SDRAM_HOST     0xC0000000
#define VIDEO_FIFO_OFF       0x000000   /* bitstream FIFO, written by the PP */
#define VIDEO_FIFO_SIZE      0x100000
#define VIDEO_DPB_OFF        0x100000   /* the decoder's frames */
#define VIDEO_DPB_SIZE       0x1FC000   /* 2 MB less 16 KB: the display path
                                           reads up to 16 rows past a plane */
#define VIDEO_WRAP_OFF       0x300000   /* linear copy of a packet that wraps
                                           the FIFO's end */
#define VIDEO_WRAP_SIZE      0x40000
/* internal SRAM: the decoder's arena (dev11), context and per-MB info,
 * then the frames when they fit (320x240 with up to 4 references: 41 KB
 * + 5 x 113 KB); larger streams put their frames in SDRAM at VIDEO_DPB */
#define VIDEO_ARENA          BCMFW_VIDEO_ARENA
#define VIDEO_ARENA_SIZE     (0x0CC000 - VIDEO_ARENA)   /* to the stack */
#define VIDEO_STRIP          0x0DC000   /* the scaler's strip (7,680 B) */
#define VIDEO_FB             0x105800   /* the RGB565 picture, 320 x 240,
                                           rows 640 bytes apart */
#define VIDEO_FB_PITCH       640
#define VIDEO_QUEUE_DEPTH    16         /* FEEDs in flight (OPEN reply) */

#ifndef __ASSEMBLER__
/* Counters for STATUS / CLOSE */
struct video_stats
{
    uint32_t decoded;       /* pictures out of the decoder */
    uint32_t shown;         /* pictures pushed to the panel */
    uint32_t errors;        /* decode errors (NAL units dropped or concealed) */
    uint32_t concealed;     /* macroblocks concealed */
    uint32_t pts_lo, pts_hi;   /* PTS of the picture on screen */
    uint32_t queued;        /* FEEDs not yet decoded */
    uint32_t fatal;         /* 0, or the HP_E_* that stopped decoding */
    uint32_t work_us;       /* the last FEED: decode, convert, push (1 MHz) */
    uint32_t work_max_us;   /* the longest since OPEN */
    uint32_t mhz;           /* the core clock now: 27-150 */
    uint32_t hidden;        /* decoded under HP_FEED_HIDE, not shown */
};

/* OPEN with HP_CODEC_H264: w x h from OPEN (the SPS decides), mode
 * HP_MODE_FIT or HP_MODE_NATIVE.  Returns HP_OK or an HP_E_* code. */
/* flags: the OPEN word's HP_OPEN_* bits (HP_OPEN_FAST_CLOCK: 150 MHz for
 * the session, BCMFW_VIDEO_PLL builds; back to 27 MHz at close) */
int video_open(uint32_t w, uint32_t h, uint32_t mode, uint32_t flags);
void video_close(void);
#if BCMFW_VCLK
/* clock.c around a change of the core clock: SDRAM's refresh for 27 MHz
 * before (after 0), the new clock's after (1) */
void video_clock_refresh(int after);
#endif
/* Queue a checked FEED; HP_OK, or HP_E_BUSY when VIDEO_QUEUE_DEPTH are
 * already queued (the host keeps within the depth, so it never is) */
int video_queue(uint32_t fifo_off, uint32_t len, uint32_t pts_lo,
                uint32_t pts_hi, uint32_t flags);
/* OVERLAY: the host's frame (BCMFW_FB, panel coordinates) in x|y<<16,
 * w|h<<16 goes over the picture, now and after every picture, until a
 * call with w or h 0 puts the picture (and black borders) back */
void video_overlay(uint32_t xy, uint32_t wh, uint32_t slide);
/* the overlay's slide (HP_OVL_SLIDE_*): one step by the clock; the main
 * loop calls it every pass, and it returns at once when none runs */
void video_anim(void);
/* Decode the oldest queued packet and show its pictures; returns the
 * 64-byte FIFO units it occupied (the host may reuse them), 0 if none */
uint32_t video_work(void);
const struct video_stats *video_get_stats(void);
/* 1 once an OPEN has brought SDRAM up (this boot) */
int video_sdram_ready(void);
#if BCMFW_JOBS_STAGED
/* The staged upload's window (jobs.h J_ALLOC_STAGED; candidate images
 * only): SDRAM brought up in UI mode at the first staged ALLOC.  1 when
 * up (a later OPEN skips its own bring-up), 0 when the bring-up failed */
int video_sdram_up(void);
#endif
#if !BCMFW_JOBS
/* PW5 (hostproto.c cmd_hib): Apple's SDRAM idle before the hibernate
 * park (0, or 1 if bit 15 never cleared) and its bring-up after the wake
 * (0, or the VIDEO_SD_* step; SDRAM is then marked down) */
uint32_t video_sdram_idle(void);
uint32_t video_sdram_rewake(void);
#endif

/* SDRAM set-up and decoder open, as OPEN reports them (hostproto.md,
 * "OPEN"): [0] = VIDEO_SD_MAGIC | step << 12 | attempts << 8 | seen << 4
 * | flags, [1] the last +0x20 read, [2]/[3] the probe words read back.
 * Attempts 1-4: b = 8, 4, then both again with the cache on (Apple's
 * state); seen: bit n = attempt n+1 saw +0x20 bit 15. */
#define VIDEO_SD_WORDS   5      /* [4]: the memory test (VIDEO_MT_*) */
#define VIDEO_SD_MAGIC   0x53440000     /* "SD" */
#define VIDEO_SD_OK      0
#define VIDEO_SD_CMD     1      /* command 7: +0x20 bit 15 never set, and
                                   no probe passed */
#define VIDEO_SD_PROBE   2      /* bit 15 seen, but no probe passed */
#define VIDEO_SD_CTX     3      /* h264_open: the SRAM context */
#define VIDEO_SD_FRAMES  4      /* h264_set_frames: the DPB in SDRAM */
#define VIDEO_SD_TIMER   0x1    /* flags: the 1 MHz counter bounded the waits */
#define VIDEO_SD_FAST    0x2    /*        the core was on PLL A */
const uint32_t *video_sdram_diag(void);

/* The memory test at a diagnostic OPEN (HP_OPEN_SDRAM_DPB), word 4:
 * VIDEO_MT_MAGIC | size in 64 KB units << 16 | wrong grid words << 8 |
 * VIDEO_MT_* bits.  0 when it didn't run. */
#define VIDEO_MT_MAGIC   0x4D000000     /* "M" */
#define VIDEO_MT_VLD     0x01   /* a vector row loaded from SDRAM right */
#define VIDEO_MT_VST     0x02   /* a vector row stored to SDRAM right */
#define VIDEO_MT_RAN     0x80

/* Progress (dev11): where the image is, for the host to read when the
 * image stops answering.  PROG = phase << 24 | detail, N = packets
 * taken, A = an address for the phase, DPB = frame 0's luma plane. */
#define VIDEO_MB_PROG    0x1C4
#define VIDEO_MB_PROG_N  0x1C8
#define VIDEO_MB_PROG_A  0x1CC
#define VIDEO_MB_DPB     0x1D0
#define VIDEO_PH_OPEN     0x01  /* detail: VIDEO_SD_* step being run */
#define VIDEO_PH_MT_SIZE  0x10  /* A: the power of two being tried */
#define VIDEO_PH_MT_GRID  0x11  /* A: the address */
#define VIDEO_PH_MT_VLD   0x12  /* A: the SDRAM row */
#define VIDEO_PH_MT_VST   0x13
#define VIDEO_PH_PACKET   0x20  /* detail: queue index; A: the packet */
#define VIDEO_PH_WRAP     0x21
#define VIDEO_PH_NAL      0x30  /* detail: NAL type; A: the NAL */
#define VIDEO_PH_MB       0x31  /* detail: macroblock address (h264) */
#define VIDEO_PH_DEBLOCK  0x32  /* detail: macroblock row (h264) */
#define VIDEO_PH_CONVERT  0x50  /* A: the luma plane */
#define VIDEO_PH_BLIT     0x51
#define VIDEO_PH_SHOWN    0x5F  /* detail: pictures shown */
#define VIDEO_PH_IDLE     0x0F  /* detail: packets done */

/* The guard (dev14): session 12's VP header and mailbox (0x100-0x1FF)
 * were overwritten while the first picture was decoded and shown.  The
 * image checks the header's fixed words and the event index after every
 * NAL unit, conversion and panel push, and records the first failure in
 * a block far from low SRAM (0xDDE00 is the end of the scaler's strip;
 * hostproto.md section 7).  Words: */
#define VIDEO_GUARD        0x0DF000
#define VIDEO_GUARD_WORDS  60
#define VIDEO_GUARD_MAGIC  0x47554152      /* "GUAR", once the session opened */
/* [1] the progress word (VIDEO_MB_PROG's value) at the first failure, 0
 *     none; [2] the check that found it (VIDEO_GC_*); [3] packets taken;
 * [4..7] what was found at 0x100 (HP_VERSION), 0x108, 0x110 and 0x128
 *     (HP_EVT_WR); [8..10] the first picture's Y, U and V planes; [11] its
 *     luma stride; [12] the OPEN's HP_OPEN_C_* bits; [13] pictures
 *     decoded at the failure; [14] checks run; [15] a copy of [1] that is
 *     written last */
/* [48..55] (dev32) the session's refusal, cleared at every OPEN: [48]
 *     VIDEO_RF_MAGIC | the NAL's type, 0 none; [49] the check (h264_int.h
 *     h264_refused_at: file << 16 | line); [50] the NAL's length; [51],
 *     [52] its first 8 bytes, big-endian; [53] the packet's FIFO offset;
 *     [54] its length; [55] its first 4 bytes */
#define VIDEO_RF_MAGIC     0x52460000      /* "RF" */
/* dev37: the session's first other decode error in the same words, with
 * VIDEO_ER_MAGIC | -code << 8 | NAL type in [48] and its check
 * (h264_error_at) in [49]; a refusal overwrites it (RF also carries the
 * code: 2) */
#define VIDEO_ER_MAGIC     0x45520000      /* "ER" */
/* [56..59] (dev47, session 37: a fresh boot's first session found no PPS
 * for any slice, though its first packet carried SPS and PPS and the FIFO
 * matched the host's bytes), cleared at every OPEN: [56] SPS units seen |
 * PPS units seen << 8 | the last SPS's return & 0xFF << 16 | the last
 * PPS's << 24; [57] at the first error, the valid PPS ids (bit n) and
 * [58] the valid SPS ids; [59] packets decoded | the first packet's NAL
 * types (bit n: type n, types 0-15) << 16 */
#define VIDEO_GC_NAL       1
#define VIDEO_GC_CONVERT   2
#define VIDEO_GC_BLIT      3
#define VIDEO_GC_IDLE      4
#define VIDEO_GC_SELFTEST  5
/* [16] the self-test (HP_OPEN_SELFTEST, selftest.c): VIDEO_ST_MAGIC |
 *      failing tests (bit n = VIDEO_ST_* n), 0 not run; [17] the first
 *      wrong byte: test << 24 | offset; [18] the vector's byte, [19]
 *      C's; [20] comparisons made; [21] the tests that changed low SRAM
 *      (0x004-0x1FF, put back after each, dev15); [22] the first such
 *      word's address, [23] its value */
#define VIDEO_ST_MAGIC      0x53540000      /* "ST" */
#define VIDEO_ST_IDCT       0
#define VIDEO_ST_IDCT_DC    1
#define VIDEO_ST_PRED4      2
#define VIDEO_ST_PRED16     3
#define VIDEO_ST_PREDC      4
#define VIDEO_ST_MC_LUMA    5
#define VIDEO_ST_MC_CHROMA  6
#define VIDEO_ST_DBK_LUMA   7
#define VIDEO_ST_DBK_CHROMA 8
#define VIDEO_ST_DISP_FLAT  9
#define VIDEO_ST_DISP_RAMP  10
/* its scratch: three 64 x 24 areas between the scaler's strip and the
 * guard (0xDDE00-0xDF000) */
#define VIDEO_ST_AREA      0x0DDE00
/* [24 + n] test n's first wrong byte (dev16): offset | vector's byte << 16
 *      | C's << 24, 0 if right; [35] the transform's coefficient buffer
 *      (it must be 32-byte aligned) */
/* clean: the VRF cleared before every vector kernel (dev22), as vc2sim
 * starts; the VRF dumps come from the last run */
uint32_t video_selftest(uint32_t out[7], uint32_t each[11], int clean);
extern const void *video_selftest_coef;
/* [36] the scalar test (dev17): 0x414C << 16 | failing cases (bit n);
 *      [37] the first failing case, [38] what the chip computed;
 * [39] 0; [40-42] FNV-1a of the first picture's Y, U, V (its visible
 *      width, every row), [43-45] the second's; [46] FNV-1a of the first
 *      converted picture (ow x oh RGB565, row by row); [47] 0 */
uint32_t video_alutest(uint32_t out[2]);
/* [47] the self-test again from a cleared VRF (dev22): 0x5343 << 16 |
 *      failing tests (VIDEO_ST_* bits) */
/* [39] the dumps (dev18): VIDEO_DUMP_MAGIC | bit 0 the first picture
 *      (HP_OPEN_DUMP: Y, U, V rows of its visible size, tightly packed,
 *      at VIDEO_DUMP_PIC), bit 1 the self-test's areas at its first
 *      failing test (source, vector's, C's: 3 x 1,536 bytes at
 *      VIDEO_DUMP_ST).  SDRAM offsets, past the wrap copy's area; the host
 *      reads them at 0xC0000000 + offset. */
/* Stage 8a-1's tone buffer (dev41, hostproto.c cmd_tone): SRAM 0x12C000-
 * 0x1349D0, above VIDEO_FB; used only while the op runs */
#define VIDEO_TONE_SRAM    0x12C000
/* Audio, in the free SDRAM between the wrap copy and the dumps (the
 * overlay never uses SDRAM):
 *   0x340000-0x37FFFF  the PCM and access-unit rings: stage 8b's (dev39:
 *                      in at +0, out at +0x20000, 32 KB each) and the
 *                      player's later (audio-spike.md M5)
 *   0x380000-0x39BFFF  the AAC decoder's far data (M4; config.h
 *                      BCMFW_AAC_FAR): its tables (vc2cc .xdata, copied
 *                      in at the first H.264 OPEN), then its channel
 *                      elements (.sdbss); build.py checks the fit
 *   0x39C000-0x39FFFF  AAC test I/O: what HP_OP_AAC's OPEN and DECODE
 *                      may name besides the rings (an AudioSpecificConfig,
 *                      an access unit, 4 KB of PCM out) */
#define VIDEO_AUDIO_OFF    0x340000
#define VIDEO_AUDIO_SIZE   0x040000
#define VIDEO_AAC_OFF      0x380000
#define VIDEO_AAC_SIZE     0x01C000
#define VIDEO_AAC_IO       0x39C000
#define VIDEO_AAC_IO_SIZE  0x004000
/* HP_AAC_MEMPROBE's 16 SDRAM bytes (dev42): the I/O window's last line,
 * clear of an ASC, an AU and the PCM */
#define VIDEO_AAC_MEMPROBE 0x39FFF0
/* HP_AAC_MEMSTRESS's 4 KB (session 36c): after the PCM (0x39D000-0x39DFFF),
 * before the probe line */
#define VIDEO_AAC_STRESS   0x39E000
#define VIDEO_AAC_STRESS_SIZE 0x1000
#define VIDEO_DUMP_MAGIC   0x44000000
#define VIDEO_DUMP_PIC     0x3A0000
#define VIDEO_DUMP_ST      0x3E0000     /* 11 x 8 KB: each failing test */
#define VIDEO_DUMP_ST_STRIDE 0x2000
#define VIDEO_DUMP_VRF     0x3D0000     /* 11 x 2,048: the VRF after each
                                           self-test's vector kernel */
extern uint32_t video_st_dumped;
#endif

#endif /* BCMFW_VIDEO_H */
