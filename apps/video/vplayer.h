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

/* The video player's thread (#64 piece 6; HAVE_BCMFW_VIDEO only): the
   core (vpcore.c) on the kernel tick, with the audio's clock, the audio's
   buffered copy of the file (bufpeek_at), the index from the .vix (built
   here if the library update hasn't), our video image from the disk, and
   the BCM from the LCD driver (target) or the model (simulator).

   The audio path plays the file; the video follows it and never owns
   playback.  Call vplayer_start() BEFORE starting the audio: its blocks
   come from core_alloc, and taken later they would shrink the audio
   buffer (player.md section 5). */

#ifndef VPLAYER_H
#define VPLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bcmres.h"

/* Our video images on the disk (hidden, like every shell file; never
   named on screen).  VPLAYER_IMAGE is the player's (playback, the boot
   path's resident rung, the stage and bench labs): the merged image (video
   and the display jobs) in a build with HAVE_BCMFW_MERGED_VIDEO, else the
   plain one.  VPLAYER_PROBE_IMAGE is always the plain image, the only one
   with the brief probes (HP_OP_PEEK ... HP_OP_HIB): vplayer_probe() and
   the E2 lab boot it.  bcmfw/mkvideo.py builds both. */
#define VPLAYER_IMAGE       BCMRES_IMAGE    /* bcmres.h */
#define VPLAYER_PROBE_IMAGE ROCKBOX_DIR "/bcmprobe.bin"
#define VPLAYER_IMAGE_MAX   BCMRES_IMAGE_MAX

/* The AAC priming the audio clock still counts, if any (player.md section
   3: a device check; Rockbox's AAC codec trims it, so 0 until measured) */
#define VPLAYER_PRIME_US    0

/* The video clock follows elapsed by at most 1/VPLAYER_SLEW of the time
   passed (5%: a 430 ms start-up leap is absorbed in about 9 s of slightly
   faster pictures instead of a dropped GOP) */
#define VPLAYER_SLEW        20
/* the thread this far past its planned wait is logged as late */
#define VPLAYER_LATE_US     100000
/* The thread's fixed level for the whole session: its own band, below
   the codec (13), above buffering (15) and the UI (16); the scheduler's
   time-bounded aging keeps the threads below it running (Fit-1 B,
   docs/fit1-brief.md; dev43 dropped dev33-39's following of pcmbuf's
   boosts).  Without the bands, the same place by number. */
#ifdef HAVE_SCHED_BANDS
#define VPLAYER_PRIO        PRIORITY_VIDEO_FEED
#else
#define VPLAYER_PRIO        (PRIORITY_BUFFERING - 1)
#endif

/* The player's state, as vpcore.h's enum vpc_state; -1 when not running */
struct vplayer_info
{
    int state;
    int fail_why;
    uint32_t restarts, wakes, recoveries;
    uint32_t posted, skipped, dropped_gops, lead_us;
    uint32_t direct_reads;      /* outside the audio's buffered window */
    bool ended;                 /* the last picture went up (VPC_DONE) */
    /* SDRAM (hostproto.md, "OPEN"): the last OPEN's words (sd_n of
       them), whether 150 MHz was dropped after a failed OPEN, and stage
       6's readback (first: offset, read, wanted) */
    bool clock_dropped;
    uint32_t sd[5], sd_n, sd_pll[5];    /* sd_pll: the failed 150 MHz OPEN */
    uint32_t sdc_done, sdc_words, sdc_bad, sdc_first[3], sdc_span,
             sdc_last_ok;
    int sdc_port_err;
    /* the image's words when it was last lost (vpcore.h VPC_PM_*) */
    uint32_t pm[9], pm_n, pm_losses;
    bool pm_fast;
    uint32_t dpb;               /* where the frames are (BCM address) */
    /* dev12: the port's handshakes (bcm_video_port_stats), the packets'
       readback (first wrong: address, got, want), lockstep's held
       pushes, and port stalls ridden out */
    uint32_t port[8];
    uint32_t vfy_words, vfy_bad, vfy_first[3];
    uint32_t lock_waits, stalls, stall_max_us;
    /* dev14: the image's guard block (bcmfw/video/video.h VIDEO_GUARD)
       as read at the last loss or the stop, and the last torn event
       index word */
    uint32_t guard[56], guard_n, evt_raw;
    /* dev32: why the last vplayer_start() failed (VPLAYER_OPEN_*, 0 it
       didn't; kept after the failure, B or not) and, for _DAMAGED, the
       index's or file's MP4_E_* (mp4demux.h); the image's refusal of the
       stream (fail_why HP_E_UNSUPPORTED): its check (file << 16 | line,
       bcmfw/h264/h264_int.h) and NAL type; still-picture restarts */
    int open_why, open_err;
    uint32_t refused_at, refused_nal, stills, reloads;
    /* P19 PW4: the chip's clock governor (vclk.h), this play: on, the
       time-weighted MHz, level x 10 and idle permille, ms governed, band
       and level changes, fallbacks, the chip's misses (skips, dropped
       GOPs), the band now, ms at each band */
    bool vclk_on;
    uint32_t errors, concealed; /* the image's decode errors, blocks
                                   concealed (STATUS words 8, 9) */
    uint32_t vclk_mhz, vclk_lvl10, vclk_idle_pm, vclk_ms;
    uint32_t vclk_changes, vclk_lvl_changes, vclk_fails, vclk_misses;
    uint32_t vclk_clock_changes;    /* band moves are vclk_changes */
    uint32_t vclk_policy;           /* the in-band policy (vclk.h) */
    uint32_t vclk_band, vclk_band_ms[12];
};

#define VPLAYER_OPEN_FILE       1   /* the video file didn't open */
#define VPLAYER_OPEN_INDEX      2   /* no index (.vix) for it, or stale */
#define VPLAYER_OPEN_IMAGE      3   /* the BCM image missing or not ours */
#define VPLAYER_OPEN_MEMORY     4
#define VPLAYER_OPEN_DAMAGED    5   /* the index or the file's tables
                                       refused: open_err says which */

/* P19 PW4: the band-verify row's flags (vclk.h vclk_flags_mask) */
#define VPLAYER_VCLK_FLAGS  ROCKBOX_DIR "/bcmpassvc.txt"
/* the bench: hold band b (0 the crystal .. 11 150 MHz) for the next plays,
   or -1: the governor over the verified bands */
void vplayer_vclk_force(int band);
/* the in-band policy for the next plays (vclk.h enum vclk_policy: 0 the
   band's top clock, the default; 1 proportional), and the one set */
void vplayer_vclk_policy(int policy);
int vplayer_vclk_get_policy(void);
/* the verified bands, as the last start read them */
uint32_t vplayer_vclk_mask(void);
/* every finished play's governor so far: MHz x ms, level x 10 x ms, busy
   and counted ms of the chip's meter, ms governed (the battery log) */
void vplayer_vclk_totals(uint64_t *mhz_ms, uint64_t *lvl10_ms,
                         uint64_t *busy_ms, uint64_t *el_ms, uint64_t *ms);

/* The first-use check (vcheck.h; docs/research/bcm2722/first-use-check.md):
   the unit's token, its temporary and the replace's old name, and the
   test stream, all in the hidden folder */
#define VPLAYER_VCHECK_TOKEN    ROCKBOX_DIR "/vcheck.tok"
#define VPLAYER_VCHECK_TMP      ROCKBOX_DIR "/vcheck.tmp"
#define VPLAYER_VCHECK_PREV     ROCKBOX_DIR "/vcheck.old"
#define VPLAYER_VCHECK_STREAM   ROCKBOX_DIR "/vcheck.bin"
/* The next vplayer_start() runs the check (the image's clock token opens
   the fast clock, this unit has no token for the image, and the test
   stream is there): the shell shows "Preparing video..." first.  Reads
   the small token file; no write. */
bool vplayer_check_due(void);
/* A bench or device-test play (devbench.c around each step, devbcm.c's
 * stage 6): the first-use check is skipped, logged, so the step measures
 * the player itself (sitting 4b: the check ran inside stage 6) */
void vplayer_bench_play(bool on);
/* The bench's first-use check step (devbench.c vplay): drops this
   unit's token (and this start's "inconclusive, not again"), logged, so
   the next play checks again.  vcheck_store_drop's return: > 0 dropped,
   0 none there, < 0 the drop failed */
int vplayer_check_reset(void);
/* The last check's verdict in one line ("" before any this boot), and
   the unit's token as last read, for Save Diagnostics */
void vplayer_check_summary(char *buf, size_t n);

/* Starts the video of path (an MP4/M4V the audio is about to play).
   false, logged, if it can't (no index and none can be built, no image,
   no memory): the audio plays alone, as today. */
bool vplayer_start(const char *path);

/* The audio was seeked to ms. */
void vplayer_seek(unsigned long ms);

/* Ends the video, gives the BCM back (the normal screen repaints) and
   frees everything. */
void vplayer_stop(void);

/* For a caller that waits on a play outside its input pump (the device
   test's stages): answers the player thread's requests to the job ring's
   owner (its take and release); nothing on another thread.  start and
   stop pump themselves. */
void vplayer_pump(void);

/* The shell's overlay over the video (dev34: the scrub bar).  While the
   video owns the panel, lcd_update() reaches nothing: draw into the frame
   as usual, then call this with the rectangle drawn (w or h 0 takes it
   away; the picture comes back).  The frame's pixels there must stay as
   they are while the overlay shows (a restart puts them up again).
   Waits until the player's thread has sent them; 0 or < 0. */
/* A video session holds the player (vplayer_start() until _stop()) */
bool vplayer_running(void);

int vplayer_overlay(int x, int y, int w, int h);
/* The same, sliding by the image's clock (hostproto.h HP_OVL_SLIDE_UP |
   ms: up from the rectangle's bottom edge; HP_OVL_SLIDE_DOWN | ms: the
   band up goes down and away, as w 0 at the end).  The frame's pixels
   must stay while it slides too. */
int vplayer_overlay_slide(int x, int y, int w, int h, unsigned slide);

/* The probes and labs below (8a-0 to stage 9): their struct bvp, taken
   from core_alloc while one runs, had no room */
#define VPLAYER_E_NOMEM     (-50)

/* Stage 8a-0: the BCM's GPIO levels (mask: pins 0-15) sampled as inputs for
   us microseconds (at most 1 s) by our image, outside any video: out[0]
   samples, [1] elapsed us, [2..5] edges of the mask's first four pins,
   [6] the levels' AND, [7] their OR.  0, or < 0 (a video holds the BCM, no
   image, a port or protocol error). */
int vplayer_probe_pins(uint32_t mask, uint32_t us, uint32_t pull,
                       uint32_t out[8]);       /* pull: 0 none, 1 down, 2 up */

/* Stage 8a-1 (dev42, dev45): one tone for ms (at most 1500) through the
   BCM's own audio block (rate divider div), at -40 dBFS, in sample layout
   0-3 (HP_OP_TONE), phase step Q16 of a 256-step period; start: the
   block's start bits (a subset of Apple's 0x6209), 0 for Apple's.  The
   block is a clock slave: the image muxes the clock pins only if the
   codec's frame clock is on GPIO 3, so the caller keeps the codec awake.
   DRIVES GPIO 1: only after 8a-0/0b, with the codec's volume turned down
   by the caller.  out: [0] the control word after the rate, [1] ready
   seen, [2] ms played, [3] the control word before, [4] while playing,
   [5] GPIO_FSEL0 as last set, [6] [7] edges on GPIO 2 and 3 in 3 ms;
   PB-5 (brief-map-peripherals.md section 11): [8] [9] the audio clock
   words 0x10001C34/38 before the rate, [10] [11] while playing, [12] [13]
   after the close (0 from an older image). */
#define VPLAYER_TONE_OUT 14
int vplayer_tone(uint32_t ms, uint32_t div, uint32_t layout, uint32_t step,
                 uint32_t start, uint32_t out[VPLAYER_TONE_OUT]);

/* The DMA lab (dev40): PP DMA blocks of 256 B to 32 KB into the FIFO, each
   read back, with the word-0 rewrite off and on; a line a case to log(). */
int vplayer_dma_lab(void (*log)(const char *line));

/* Bench row B13, the warm swap (docs/research/bcm2722/warm-swap.md): the
   display-only image (disp, the device test's jobs_display) booted cold,
   then VPLAYER_WARM_ROUNDS swaps between it and the video image, each by
   HP_OP_PARK, an upload and the go word, the panel kept lit; then one
   swap without the go word and the cold restart that must follow.  A
   line a step to log ("warm ..."). */
#define VPLAYER_WARM_ROUNDS 20
struct vplayer_warm_result
{
    uint32_t rounds, ok;        /* swaps made, swaps that passed */
    uint32_t park_us_max;       /* PARK sent -> the stub's word */
    uint32_t up_us_max[2];      /* the video image's upload: [CPU, engine 2] */
    uint32_t ready_us_max;      /* go -> the new image's ready word */
    uint32_t swap_us_max[2];    /* PARK -> ready, into the video image */
    uint32_t panel_off;         /* swaps whose info word lacked PANEL_ON */
    uint32_t no_go_ready;       /* 1: an image started without the go word */
    uint32_t cold_us;           /* the cold restart after the injected fault */
    int first_fail;             /* the round that first failed, -1 none */
    int first_why;              /* its code (vplayer.c WARM_E_*) */
};
int vplayer_warm_lab(const uint8_t *disp, uint32_t disp_bytes,
                     void (*log)(const char *line),
                     struct vplayer_warm_result *r);

/* Bench row B5 (open question #53): panel pushes straight from a slot at
   another pitch, through the display-only image's jobs.  A 300 x 300
   picture (pitch 300) and a 400 x 160 one (pitch 400) are presented as a
   160-wide window starting at an odd row, beside the framebuffer's own
   (pitch 320); each push timed by the chip's DMA counter, ten times.
   The 300-pitch window is left up for the photograph: a white frame, a
   diagonal and the stripes must come out straight. */
struct vplayer_window_result
{
    uint32_t us_min[3], us_max[3];  /* pitch 320, 300, 400 */
    uint32_t px[3];                 /* pixels a push moved */
    uint32_t dma_done, dma_tmo;     /* pushes the engine finished, timed out */
    int fail_step;                  /* 0 ok */
};
int vplayer_window_lab(const uint8_t *disp, uint32_t disp_bytes,
                       void (*log)(const char *line),
                       struct vplayer_window_result *r);

/* Bench row CR1, COMMIT readback (open question #67; bench-p19.md): on
   the display-only image, a pattern at each slot pool and a control in
   the host frame read back by the port, Apple's write forms where ours
   read wrong, then slots written, read back pixel by pixel and
   COMMITted, by the CPU and by engine 2.  Lines "cr1 ...", the last
   "cr1: PASS|FAIL: ...".  0, or the image never started. */
enum
{
    VPLAYER_CR_PASS = 0,
    VPLAYER_CR_REGION,              /* control right, a pool wrong */
    VPLAYER_CR_WRITE,               /* the control or a slot read back wrong */
    VPLAYER_CR_SUM,                 /* readback right, COMMIT J_E_SUM */
    VPLAYER_CR_OTHER,               /* another status or step */
    VPLAYER_CR_NOTRUN
};
struct vplayer_commit_result
{
    int control_bad;                /* the control's wrong words (-1 port) */
    uint32_t regions, regions_bad;  /* pool addresses tried, wrong */
    uint32_t variants_ok;           /* Apple's forms that landed where
                                       ours didn't (bit per form) */
    uint32_t cases, pass, write_bad, sum_bad, other;   /* slot cases */
    int verdict;                    /* VPLAYER_CR_* */
};
int vplayer_commit_lab(const uint8_t *disp, uint32_t disp_bytes,
                       void (*log)(const char *line),
                       struct vplayer_commit_result *r);

/* Bench rows B6 and B7 (open questions #54, #7, #44): HP_OP_LAB on the
   video image, at 27 MHz and, with the 4b/4c flags, at 150 MHz.  B6
   (fetch): random halfword fetches against the same arithmetic alone, in
   SRAM and SDRAM.  B7 (bandwidth): word loads, stores and copies in order
   in both, then a long load run alone and while the host streams 150 KB
   through the port into the same memory.  Lines "lab6 ..." / "lab7 ...". */
struct vplayer_mem_result
{
    uint32_t clocks;                /* clocks run: 1 or 2 */
    /* [clock 27/150][SRAM, SDRAM] */
    uint32_t fetch_ns10[2][2];      /* B6: one fetch, tenths of a ns */
    uint32_t load_kbs[2][2], store_kbs[2][2], copy_kbs[2][2];   /* B7 */
    uint32_t slow_pm[2][2];         /* B7: the load run's slow-down under
                                       the host's stream, per mille */
    int fail_step;
};
int vplayer_mem_lab(int row, void (*log)(const char *line),
                    struct vplayer_mem_result *r);

/* The brief probes (brief-map-peripherals.md section 11, PB-*; brief-map-
   power.md section 9, PW*): the BCM taken from the display driver, pre()
   (may be NULL: port reads of whatever ran, Apple's vmcs or the resident
   image), then the probe image (VPLAYER_PROBE_IMAGE) booted and HELLO'd,
   then fn() with the OPEN flag for 150 MHz (0 without the 4b/4c pass
   flags); at the end the BCM's power is cut and the display comes back by
   the stock wake path.
   Returns fn()'s value, or < 0 if the BCM couldn't be brought up. */
struct bvp;
int vplayer_probe(int (*pre)(void *ctx),
                  int (*fn)(struct bvp *b, uint32_t fast, void *ctx),
                  void *ctx);
/* Port reads of the running image only (the bench's jvpu: J_VPU and the
   job block's words): the BCM taken from the display driver, pre(ctx),
   then handed straight back (the resident image keeps running), or, if
   it can't be, the power cut and the stock wake path as vplayer_probe.
   No image file, no memory: sitting 4c's jvpu read "no probe image"
   because the 213 KB probe image's allocation was refused while the
   music sat paused.  pre()'s value, or -1 a video holds the BCM, -3 the
   BCM not free. */
int vplayer_peek(int (*pre)(void *ctx), void *ctx);

/* Stage 9 (M4, audio-spike.md section 7): the AAC decoder on the chip.
   Boots the image (outside any video), runs the 32-bit bcs/bcc probe,
   opens a session (fast: at 150 MHz when the 4b/4c pass flags allow, else
   27 MHz), sends the ASC and every access unit of au_path (an .au file)
   through HP_OP_AAC, reads each AU's PCM back word by word and compares
   its FNV-1a with ref_path's (bcmfw/aac/test/mkdevclip.sh makes both).
   Lines worth keeping go to log().  0, or < 0: -1 a video holds the BCM,
   -2 no image, -3 the BCM not free, -4 no memory, -5 a file missing,
   -6 a file malformed, VPLAYER_E_NOMEM, or a port/protocol error
   (res->rc). */
struct vplayer_aac_result
{
    uint32_t aus_ref;           /* AUs in the reference */
    uint32_t aus;               /* AUs the chip decoded */
    uint32_t matched;           /* whose frames and PCM hash were right */
    int32_t first_bad;          /* the first that wasn't, -1 none */
    uint32_t frames;            /* PCM frames back */
    uint32_t hash, ref_hash;    /* FNV-1a over all the PCM, the host's */
    uint32_t us_total, us_n, us_max;  /* chip time per AU with output */
    uint32_t mhz;               /* the chip's clock, from its reply */
    uint32_t rate, channels;    /* AAC OPEN's reply */
    bool probe_ran;
    uint32_t probe, probe_want; /* taken bits, vc2sim's model */
    /* dev42 (session 35: AU 0 refused, AU 1 wrong): */
    uint32_t errors;            /* AUs the chip refused (DECODE's code) */
    uint32_t au_bad;            /* AUs that didn't read back as uploaded */
    int open_code;              /* aac_open's code */
    int tables;                 /* the tables against the image file: 1
                                   same, 0 differ, -1 not reported */
    int mem_sram, mem_sdram;    /* MEMPROBE: 1 right, 0 wrong, -1 none */
    /* session 36c (155 of 200 refused, exact in vc2sim): the parse's
       checkpoints after each DECODE (HP_AAC_STATE); the second ic_stream
       must hash as the first right after channel_pair_element copies it */
    uint32_t copy_checked, copy_bad;
    uint32_t traced;            /* AUs whose checkpoints were logged */
    /* HP_AAC_MEMSTRESS: stores then immediate loads, the decoder's copy
       and clear; mismatches summed over its patterns, -1 not run */
    int32_t stress_sram, stress_sdram;
    int rc;
};
/* aac_flags: bcmfw/aac/aac.h AAC_FLAG_* for the image's AAC OPEN */
#define VPLAYER_AAC_BYTE_COPY   0x1     /* aac.h AAC_FLAG_BYTE_COPY */
#define VPLAYER_AAC_TRACE       0x2     /* aac.h AAC_FLAG_TRACE: the test
                                           always sets it */
int vplayer_aac_test(const char *au_path, const char *ref_path, bool fast,
                     uint32_t aac_flags, void (*log)(const char *line),
                     struct vplayer_aac_result *res);

/* Stage 8b: with a gain (Q16, 0 off) set before vplayer_start(), the
   session also sends a 440 Hz tone through the chip and back (HP_OP_AUDIO)
   and plays it on the mixer's voice channel; its blocks, underruns and
   per-block times close the session's log. */
void vplayer_set_audio_loop(uint32_t gain_q16);

/* The session's full log (its ring of lines, the clock trace, the watch
   lines; written to bcmtest.log after the session) on normal plays too;
   off by default, so a play logs only its events and end summary.  Stages
   6-8 keep it regardless. */
void vplayer_set_full_log(bool on);
bool vplayer_get_full_log(void);

bool vplayer_info(struct vplayer_info *info);

/* The device test's stage 6: fn sees every STATUS whose picture moved
   (vtrace.h), on the player's thread; NULL clears it.  Set it before
   vplayer_start(). */
struct vpc_frame;
void vplayer_set_frame_hook(void (*fn)(const struct vpc_frame *f));

/* Stage 6: after the first OPEN, a pattern through the host port into
   the FIFO and back (vplayer_info's sdc_*).  Set it before
   vplayer_start(). */
void vplayer_set_sdram_check(bool on);

/* Stage 6's diagnostic pass (dev11): OPEN with HP_OPEN_SDRAM_DPB (the
   frames in SDRAM, the image's memory test first) and the readback over
   all 4 MB.  While stage 6 runs (either switch on), the player's log
   lines also go to the BCM test log. */
void vplayer_set_sdram_diag(bool on);

/* Stage 6's write modes (dev12; session 11 lost the port while the BCM
   decoded and the PP wrote): PACE writes the FIFO in VPLAYER_PACE_HW
   halfword pieces, each with its own handshake; LOCK writes only while
   no posted packet is undecoded; VERIFY reads each piece's ends back.
   Set it before vplayer_start(); 0 is the everyday player. */
/* dev39's tuning, each switchable for an A/B (stage 7/8): the PP boosted
   while a session plays, and the port's blocks by PP DMA (else the CPU's
   copy).  Default (dev41, session 34's matrix): the PP unboosted, the
   CPU's copy.  DMA is kept only for the lab: it lands nothing on silicon
   (docs/data-paths.md D1).  Bit 0x2 was the thread's
   priority floor until the bands made it fixed (dev43); the others keep
   their values so old logs read the same.  Read at vplayer_start(). */
#define VPLAYER_TUNE_BOOST  0x1
#define VPLAYER_TUNE_DMA    0x4
#define VPLAYER_TUNE_CHIP27 0x8     /* the chip at 27 MHz, not 150 (A/B) */
/* the port's blocks by the PP's second DMA engine (session 37: it
   delivers, 64 KB in 5.8 ms with the CPU free), small ones by the CPU */
#define VPLAYER_TUNE_E2     0x10
#define VPLAYER_TUNE_ALL    0x1D
void vplayer_set_tuning(uint32_t flags);
uint32_t vplayer_get_tuning(void);

#define VPLAYER_PORT_PACE   0x1
#define VPLAYER_PORT_LOCK   0x2
#define VPLAYER_PORT_VERIFY 0x4
#define VPLAYER_PACE_HW     128     /* 256 bytes, Apple's PIO limit */
void vplayer_set_port_mode(uint32_t mode);

/* Stage 6's kernel A/B (dev14): HP_OPEN_C_* bits (bcmfw/hostproto.h),
   the decoder families and the display conversion run in scalar C on
   the chip, and HP_OPEN_SELFTEST runs the kernels against their C
   versions at OPEN; 0 is the vectors.  Set it before vplayer_start(). */
void vplayer_set_kernels(uint32_t hp_open_c);

/* Stage 6 (dev18): n > 0 saves the image's dumps before the session ends,
   as bcmtest/pic<n>.yuv (the first picture, 320x240 I420) and
   bcmtest/selftest<n>.bin (the self-test's areas at its first failing
   test: source, vector's, C's, 64 x 24 each) and bcmtest/vrf<n>.bin
   (dev21: the VRF's rows 0-31 after each self-test's vector kernel,
   11 x 2,048 bytes); 0 none */
void vplayer_set_dump(int n);

#endif /* VPLAYER_H */
