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

/* The video player's thread (see vplayer.h). */

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "lcd.h"
#include "xcore.h"      /* MF-4: the overlay's pixel hand-off */
#include "core_alloc.h"
#include "file.h"
#include "dir.h"
#include "pathfuncs.h"
#include "backlight.h"
#include "audio.h"
#include "pcm_mixer.h"
#include "settings.h"
#include "buffering.h"
#include "codecs.h"
#include "rbpaths.h"
#include "string-extra.h"
#include "ipodui/uilog.h"
#include "vplayer.h"
#include "vpcore.h"
#include "vix_file.h"
#include "bcmres.h"
#include "bcmvideo_image.h"     /* and bcmprobe_image.h */
#include "bcmvideo_clock.h"     /* the clock150 tokens */
#include "vclock_gate.h"
#include "vcheck.h"
#include "vcheck_store.h"
#include "vcheck_stream.h"      /* the test stream's size, FNV and sums */
#include "vclock_ladder.h"
#include "version.h"
#include "timefuncs.h"
#include "../../bcmfw/hostproto.h"
#include "../../bcmfw/jobs.h"
#if defined(HAVE_BCM_JOBS) && !defined(SIMULATOR)
#include "bcmjob_pp.h"
#endif
#ifdef SIMULATOR
#include "sim_video.h"
#else
#include "bcm-devtest.h"
#endif

extern struct codec_api ci;     /* the codec's handle on the playing file */

#define VQ_STOP     1
#define VQ_SEEK     2
#define VQ_OVERLAY  3

/* vplayer_overlay()'s request to the thread, and its answer: the caller
   (the shell, on the COP in 19-9) writes the request and polls ovl_done;
   the thread (CPU) reads it and answers, so all four are uncached */
static volatile bool ovl_done XCORE_BSS;
static int ovl_rc XCORE_BSS;
static uint32_t ovl_rect[4] XCORE_BSS;
static uint32_t ovl_slide XCORE_BSS;    /* HP_OVL_SLIDE_* | ms, or 0 */
/* 1 KB by default on the PP: the client's records and ui_log need more;
   the sim adds its signal stack */
#define STACK_SIZE  (DEFAULT_STACK_SIZE + 0x1000)
/* .vix bytes per read when the index is scanned (and its window refills) */
#define BUILD_SCRATCH 4096

/* Stage 6/7's buffers, allocated only when they run (dev33, budget
   review: 7.8 KB of the audio's buffer on every play otherwise) */
struct vdebug
{
    /* the start-up trace: the clock's inputs at each change */
    struct { uint32_t t, el, us, gap; int32_t err; uint8_t on; } tr[48];
    uint32_t tr_n;
    /* the player's lines, written after the session (be_log): the last
       log_cap of them (128 with the full log, else 32: a normal play's
       events and end summary survive in bcmtest.log too, dev41) */
    uint32_t log_n;             /* lines logged; the ring keeps the last */
    uint32_t log_cap;
    char log[][104];
};
#define VLOG_FULL   128
#define VLOG_QUIET  32

/* The player's block: the core, its memory and the thread's stack,
   pinned together while the thread runs (statics stay small: rule 3) */
struct vblock
{
    struct vpc core;
    char path[MAX_PATH];
    int fd, vix_fd;
    uint64_t fd_pos, vix_pos;
    uint32_t direct;
    /* the audio clock: elapsed (ms) as last seen and when it moved */
    unsigned long el;
    uint32_t el_at_us;
    /* the video clock (dev28): wall time since the PCM started, pulled
       toward elapsed by at most VPLAYER_SLEW of the time passed */
    bool clk_on;
    int64_t clk;                /* us, at clk_t */
    uint32_t clk_t;
    uint32_t tr_t0;             /* the start-up clock trace's zero */
    struct vdebug *dbg;         /* the session's log (full or quiet) */
    bool full;                  /* stage 6-8 or the developer's full log */
    /* the thread's loop: the longest gap between turns (stage 6's clock
       trace) */
    uint32_t loop_t, loop_gap;
    bool boosted;               /* our cpu_boost() is held */
    uint32_t work_t0, busy_us;  /* the thread's working time (dev39) */
    /* watch(): the last values logged */
    int w_bl, w_audio;
    uint32_t w_drops, w_skipped;
    long w_freq;
    uint32_t late_said, late_at;
    bool held;                  /* the backlight is held on */
    int64_t last_a;             /* the clock at the last tick */
    bool ended;                 /* the last picture went up */
    volatile bool up;           /* vpc_start returned: the audio may start */
    long stop_after;            /* the audio left the file: stop then */
    uint8_t mem[VPC_MEM_SIZE + 8];
    uint8_t stack[STACK_SIZE] __attribute__((aligned(8)));
};

static int blk_h, keys_h, img_h, dbg_h;  /* core_alloc handles, 0 free */
static int al_h;                /* stage 8b's loop, 0 none */
static int bvp_h;               /* a probe's struct bvp, 0 none */
static struct aloop *AL;
static struct vblock *B;
static uint8_t *img;
static uint32_t img_bytes;
static unsigned int thread_id;
static volatile bool thread_over;   /* the thread is at its exit */
static struct event_queue vq SHAREDBSS_ATTR;

/* ---- the BCM ----------------------------------------------------------- */

static int be_boot(void)
{
    return bcm_video_boot(img, img_bytes);
}

static bool sd_check, sd_diag;      /* stage 6's switches */

/* The logging's own cost (dev41, the owner: "have we accounted for the
   cost of logging?"): lines, the time spent formatting and appending
   them, the busiest second's lines; reported at the session's end */
static uint32_t lg_lines, lg_us, lg_sec_t, lg_sec_n, lg_peak;
static bool full_log;           /* vplayer_set_full_log: the session's log */

void vplayer_set_full_log(bool on)
{
    full_log = on;
}

bool vplayer_get_full_log(void)
{
    return full_log;
}

static void be_log(const char *what, long a, long b)
{
    char line[128];
    uint32_t t0 = bcm_video_now_us();

    /* the frequent watch lines exist for the session's own log only: a
       build without one (no device test) never formats them */
    if (!strncmp(what, "watch:", 6) && !(B && B->full))
        return;
    if (t0 - lg_sec_t >= 1000000)
    {
        lg_sec_t = t0;
        lg_sec_n = 0;
    }
    if (++lg_sec_n > lg_peak)
        lg_peak = lg_sec_n;
    lg_lines++;
    if (!strncmp(what, "sdram:", 6))    /* register words */
        snprintf(line, sizeof(line), "%s %08lx %08lx", what,
                 (unsigned long)a, (unsigned long)b);
    else if (!strncmp(what, "vclk ", 5))    /* vclk.c's lines, as they are */
        snprintf(line, sizeof(line), "%s", what);
    else
        snprintf(line, sizeof(line), "%s (%ld, %ld)", what, a, b);
    /* the guard's detail is stage 6's (its own log, below); the shell's
       log has vpcore's one-line summary (dev32: five lines a read, up to
       three reads a session, crowded session 28's log) */
    /* the shell's log has vpcore's guard summary, not the detail, and
       not the player's watch lines (often: the CPU clock, priorities):
       those go to the session's own log only (stage 6, bcmtest.log) */
    if ((strncmp(what, "sdram: guard:", 13) || sd_check || sd_diag) &&
        strncmp(what, "watch:", 6))
        ui_log("video", "%s", line);
#ifdef SIMULATOR
    if (getenv("SIM_VLOG"))         /* every line, stamped (ui_log drops) */
        fprintf(stderr, "vplayer: +%lu %s\n",
                (unsigned long)((bcm_video_now_us() - (B ? B->tr_t0 : 0)) /
                                1000), line);
#endif
#ifndef SIMULATOR
    if (B && B->dbg)            /* bcmtest.log too, after the session */
    {
        /* kept, and written once the session is over: a file write per
           line on this thread stalled the start for ~0.5 s (session 26:
           the guard's ten lines at the third picture) */
        /* a ring of the latest lines, each with its time since the start
           (dev38: a stall late in a play needs the lines before it) */
        snprintf(B->dbg->log[B->dbg->log_n++ % B->dbg->log_cap],
                 sizeof(B->dbg->log[0]), "player: +%lu %s",
                 (unsigned long)((bcm_video_now_us() - B->tr_t0) / 1000),
                 line);
    }
#endif
    lg_us += bcm_video_now_us() - t0;
}

static const struct bvp_port port =
{
    .rd = bcm_video_rd, .wr = bcm_video_wr, .wr_block = bcm_video_wr_block,
    .now_us = bcm_video_now_us, .yield = yield,
};

/* HP_OPEN_FAST_CLOCK when the 4b and 4c pass flags name this build's
   images (pipeline.md, "The 150 MHz clock"); read once per start, so a
   session restart never touches the disk */
static uint32_t fast_flags;

/* dev41 (session 34's matrix: unboosted, CPU copy, the thread above
   buffering PASS at 16 ms): the PP unboosted, the CPU's copy; the thread
   sits in its own band (dev43, PRIORITY_VIDEO_FEED).  The port's PP DMA
   is a dead end (docs/data-paths.md D1: sessions 34-35, nothing lands),
   kept for the lab only. */
static uint32_t tuning = 0;

/* The image's clock gate (vclock_gate.h): may this image run the chip at
   150 MHz?  *why: the reason when not.  The simulator never, as before,
   but with SIM_VCHECK set (the first-use check's scripts), when its model
   image counts as one with a token. */
static enum vclock_why gate_now(const char **why)
{
#ifdef SIMULATOR
    *why = "simulator";
    return getenv("SIM_VCHECK") ? VCLOCK_TOKEN : VCLOCK_NONE;
#else
    bool dev = false;

    *why = "no clock150 token for this image";
#ifdef HAVE_BCMFW_DEVTEST
    dev = bcm_devtest_fast_clock_allowed(why);
#endif
    return vclock_gate(BCMVIDEO_IMAGE_FNV, bcmvideo_clock150_fnv,
                       BCMVIDEO_N_CLOCK150,
#ifdef HAVE_BCMFW_DEVTEST
                       true,
#else
                       false,
#endif
                       dev);
#endif
}

/* ---- the first-use check (vcheck.h; first-use-check.md) ----------------
 * The image's token opens 150 MHz for every unit; this unit's own token
 * (VPLAYER_VCHECK_TOKEN) says it was checked with this image, and at
 * which band.  None: this play checks first (be_check, inside its own
 * start-up), and a play never runs the chip above the unit's band. */
static const struct vcheck_files vc_files =
{
    VPLAYER_VCHECK_TOKEN, VPLAYER_VCHECK_TMP, VPLAYER_VCHECK_PREV,
};
static int unit_band = -1;          /* this play's top band, -1: the top */
static bool check_now;              /* this play checks first */
static bool bench_play;             /* vplayer_bench_play(): never check */
/* An inconclusive check runs again at the next play (the existing reboot
   limit and VCHECK_BUDGET_US each time); after VPLAYER_VCHECK_INC_MAX of
   them in a start, no more checks until the next start (a unit that is
   always inconclusive mustn't add a check to every play) */
#define VPLAYER_VCHECK_INC_MAX  2
static int vc_inconclusive;         /* inconclusive checks this start */
static bool unit_said;              /* this play: unchecked reason logged */
static bool top_said;               /* this play's top band logged */
static char vc_said[112];           /* the last verdict (Save Diagnostics) */
#if defined(HAVE_BCM_JOBS) && !defined(SIMULATOR)
static uint32_t jobs_failed_seen;   /* bcmjob_chip_failures() as last seen */
#endif

static bool stream_there(void)
{
    int fd = open(VPLAYER_VCHECK_STREAM, O_RDONLY);
    uint64_t n;

    if (fd < 0)
        return false;
    n = ffilesize(fd);
    close(fd);
    return n == VCHECK_FILE_BYTES;
}

/* The token as it stands: the unit's band for this image (>= 1), or -1
   with *what saying why the check is due */
static int unit_token(const char **what)
{
    struct vcheck_token t;
    int rc = vcheck_store_read(&vc_files, &t), band;

    band = rc == 0 ? vcheck_unit_band(&t, BCMVIDEO_IMAGE_FNV) : -1;
    *what = rc == VCHECK_STORE_NONE ? "no token" : rc ? "token corrupt"
          : band < 0 ? "token for another image" : "token";
    return band;
}

static const char *bad_name(int why)
{
    return vcheck_bad_name[why > 0 && why < VCHECK_BADS ? why : 0];
}

/* Live invalidation: the unit's token goes at once, with the reason in
   the log (the next play checks again) */
static void unit_invalidate(int why, long a)
{
    int rc = vcheck_store_drop(&vc_files);

    ui_log("video", "unit check: token %s (%s, %ld)", rc > 0 ? "dropped"
           : rc ? "drop FAILED" : "none to drop", bad_name(why), a);
    snprintf(vc_said, sizeof(vc_said), "invalidated: %s (%ld)",
             bad_name(why), a);
}

/* The job driver saw the chip fail since the last look: the token goes
   (the device; the job driver's count is per boot) */
static void jobs_failed_poll(void)
{
#if defined(HAVE_BCM_JOBS) && !defined(SIMULATOR)
    uint32_t n = bcmjob_chip_failures();

    if (n != jobs_failed_seen)
    {
        if (n > jobs_failed_seen)
            unit_invalidate(VCHECK_BAD_JOBS, (long)n);
        jobs_failed_seen = n;
    }
#endif
}

void vplayer_bench_play(bool on)
{
    bench_play = on;
}

int vplayer_check_reset(void)
{
    int rc = vcheck_store_drop(&vc_files);

    vc_inconclusive = 0;
    vc_said[0] = '\0';
    ui_log("video", "unit check: token %s (bench reset)", rc > 0 ? "dropped"
           : rc ? "drop FAILED" : "none to drop");
    return rc;
}

bool vplayer_check_due(void)
{
    const char *why;

    jobs_failed_poll();
    /* as check_fast_clock: no check once the start gave up on it, nor in
       a bench play (no "Preparing Video..." for a check that won't run) */
    return gate_now(&why) != VCLOCK_NONE &&
           vc_inconclusive < VPLAYER_VCHECK_INC_MAX &&
           !bench_play && unit_token(&why) < 0 && stream_there();
}

void vplayer_check_summary(char *buf, size_t n)
{
    const char *what, *said = vc_said[0] ? vc_said : "not run this start";
    int band = unit_token(&what);

    if (band > 0)
        snprintf(buf, n, "video check: %s; token %d MHz", said,
                 (int)vclk_band_mhz[band]);
    else
        snprintf(buf, n, "video check: %s; %s", said, what);
}

/* An unchecked play (the gate open, no token for this image, no check to
   decide: the stream missing or wrong, no memory, inconclusive, or the
   start gave up): it plays at the gate's 150 MHz, Apple's stock clock for
   every unit (first-use-check.md, "Unchecked plays"), with the live
   invalidation's step-down behind it.  The reason once a play in the log
   (a ring line holds 55 characters: why <= 14).  Not for a bench play or
   a lab: the bench sets its own clock. */
static void unit_unchecked(const char *why)
{
    if (!unit_said)
        ui_log("video", "unit check: unchecked (%s), 150 MHz (stock)", why);
    unit_said = true;
}

/* play: a play's start (vplayer_start); false for the labs, the probe and
   the AAC test, which measure the chip at the clock the gate allows and
   never check the unit (the bench's own clock, as a bench play) */
static void check_fast_clock(bool play)
{
    /* read at every play's start, before its OPEN: a token in this build
       (vclock_gate.h, bcmfw/verified.txt clock150) or, in a developer
       build, the unit's stage 4b and 4c flags; either opens 150 MHz from
       the next play on, with no restart */
    const char *why, *what;
    enum vclock_why g = gate_now(&why);

    fast_flags = g != VCLOCK_NONE ? HP_OPEN_FAST_CLOCK : 0;
#ifndef SIMULATOR
    ui_log("video", "clock: %s", g == VCLOCK_TOKEN ? "150 MHz (verified image)"
           : g == VCLOCK_DEV_FLAGS ? "150 MHz (4b and 4c flags)" : why);
#endif
    unit_band = -1;
    check_now = false;
    unit_said = false;
    top_said = false;
    jobs_failed_poll();
    if (g == VCLOCK_NONE)
        return;                 /* the crystal: nothing to check */
    unit_band = unit_token(&what);
    if (unit_band > 0)
    {
        if (unit_band < VCLK_TOP)
            fast_flags = 0;     /* the governor, at most the unit's band */
        ui_log("video", "unit check: token, %d MHz",
               vclk_band_mhz[unit_band]);
        return;
    }
    if (!stream_there())
    {
        /* loud, and no token: the next play looks again */
        ui_log("video", "unit check: due (%s), but the test stream is "
               "missing or wrong: playing unchecked", what);
        snprintf(vc_said, sizeof(vc_said), "due, test stream missing");
        if (play && !bench_play)
            unit_unchecked("stream");
        return;
    }
    if (vc_inconclusive >= VPLAYER_VCHECK_INC_MAX)
    {
        ui_log("video", "unit check: due (%s), not again this start "
               "(inconclusive %d times)", what, vc_inconclusive);
        if (play && !bench_play)
            unit_unchecked("gave up");
        return;
    }
    if (bench_play)
    {
        /* a bench step measures the player, not the unit check (and the
           check's band reboots would wreck the step's own readings); its
           clock is the bench's own (the gate's top, or vclk_force) */
        ui_log("video", "unit check: due (%s), skipped in a bench play", what);
        snprintf(vc_said, sizeof(vc_said), "due, skipped in a bench play");
        return;
    }
    check_now = true;
    ui_log("video", "unit check: due (%s)", what);
}

static uint32_t c_kernels;          /* stage 6's HP_OPEN_C_* bits */

static int play_top_band(void);

static uint32_t be_open_flags(void)
{
    uint32_t fast = tuning & VPLAYER_TUNE_CHIP27 ? 0 : fast_flags;

    if (!top_said)
    {
        /* once a play, at its first OPEN (after a check): the highest
           band it may ask (tests/ui/ipodui-p19-vclkcap.script) */
        top_said = true;
        ui_log("video", "clock: play top %d MHz",
               (int)vclk_band_mhz[play_top_band()]);
    }
    return fast | (sd_diag ? HP_OPEN_SDRAM_DPB : 0) | c_kernels;
}

/* ---- P19 PW4: the chip's clock governor (vclk.h) -----------------------
 * The bands this unit verified: the band-verify row's flags (bcmpassvc.txt,
 * naming the player's image) and 150 MHz from stages 4b and 4c; read with
 * the 4b/4c flags, once a start.  The bench can hold one band for a play
 * (vplayer_vclk_force). */
static uint32_t vclk_mask;
static int vclk_force = -1;
static int vclk_policy = VCLK_POLICY_TOP;   /* the in-band policy (A/B) */
/* every play's governor, summed for the battery log (vplayer_vclk_totals) */
static uint64_t vt_mhz_ms, vt_lvl10_ms, vt_busy_ms, vt_el_ms, vt_ms;

static void check_vclk(void)
{
    vclk_mask = 1;
#if !defined(SIMULATOR) && defined(HAVE_BCMFW_DEVTEST)
    {
        char text[400];
        int fd = open(VPLAYER_VCLK_FLAGS, O_RDONLY), n = 0;

        if (fd >= 0)
        {
            n = read(fd, text, sizeof(text) - 1);
            close(fd);
        }
        text[n > 0 ? n : 0] = '\0';
        vclk_mask = vclk_flags_mask(text, BCMVIDEO_IMAGE_BYTES,
                                    BCMVIDEO_IMAGE_FNV);
    }
#endif
    if (fast_flags)
        vclk_mask |= 1u << VCLK_TOP;
    if (unit_band > 0)
    {
        /* the unit's own band: never above it (the first-use check) */
        vclk_mask &= (2u << unit_band) - 1;
        vclk_mask |= 1u << unit_band;
    }
    ui_log("video", "clock bands verified: %03lx", (unsigned long)vclk_mask);
}

/* The highest band this play may run: be_open_flags' fast clock, else
   the governor's top band (be_vclk's mask), else the crystal */
static int play_top_band(void)
{
    uint32_t m = vclk_mask & ~(1u | 1u << VCLK_TOP) ? vclk_mask : 0;
    int b;

    if (tuning & VPLAYER_TUNE_CHIP27)
        return 0;
    if (vclk_force >= 0)
        return vclk_force;
    if (fast_flags)
        return VCLK_TOP;
    for (b = VCLK_TOP; b > 0 && !(m >> b & 1); b--)
        ;
    return b;
}

static uint32_t be_vclk(unsigned *xtal_level, unsigned *policy)
{
    *xtal_level = 11;
    *policy = (unsigned)vclk_policy;
#if defined(HAVE_BCMFW_RESIDENT) && !defined(SIMULATOR)
    {
        bool idle, level5;

        bcm_boot_res_now(&idle, &level5);
        if (level5)
            *xtal_level = 5;
    }
#endif
    if (tuning & VPLAYER_TUNE_CHIP27)
        return 0;
    if (vclk_force >= 0)
        return 1u | 1u << vclk_force;
    /* the crystal and 150 MHz only: nothing to govern (today's session) */
    return vclk_mask & ~(1u | 1u << VCLK_TOP) ? vclk_mask : 0;
}

void vplayer_vclk_force(int band)
{
    vclk_force = band >= 0 && band < VCLK_BANDS ? band : -1;
}

void vplayer_vclk_policy(int policy)
{
    vclk_policy = policy >= 0 && policy < VCLK_POLICIES ? policy
                                                        : VCLK_POLICY_TOP;
}

int vplayer_vclk_get_policy(void)
{
    return vclk_policy;
}

uint32_t vplayer_vclk_mask(void)
{
    return vclk_mask;
}

/* the finished play's governor into the totals */
static void vclk_total(void)
{
    uint32_t mhz, lvl10, idle, ms;

    if (!B || !vpc_vclk_means(&B->core, &mhz, &lvl10, &idle, &ms))
        return;
    vt_mhz_ms += (uint64_t)mhz * ms;
    vt_lvl10_ms += (uint64_t)lvl10 * ms;
    vt_busy_ms += B->core.gov.busy_us / 1000;
    vt_el_ms += B->core.gov.el_us / 1000;
    vt_ms += ms;
}

void vplayer_vclk_totals(uint64_t *mhz_ms, uint64_t *lvl10_ms,
                         uint64_t *busy_ms, uint64_t *el_ms, uint64_t *ms)
{
    *mhz_ms = vt_mhz_ms;
    *lvl10_ms = vt_lvl10_ms;
    *busy_ms = vt_busy_ms;
    *el_ms = vt_el_ms;
    *ms = vt_ms;
}

void vplayer_set_kernels(uint32_t hp_open_c)
{
    c_kernels = hp_open_c & (HP_OPEN_C_MASK | HP_OPEN_SELFTEST |
                             HP_OPEN_DUMP);
}

/* Stage 6's dumps (dev18): before the session ends, what the image left
   in SDRAM (bcmfw/video/video.h VIDEO_DUMP_*) to files beside the clips */
static int dump_n;

void vplayer_set_dump(int n)
{
    dump_n = n;
}

#ifndef SIMULATOR
static void dump_one(const char *fmt, uint32_t off, uint32_t bytes,
                     uint8_t *buf)
{
    char path[MAX_PATH];
    uint32_t k, n = 0, w;
    int fd;

    snprintf(path, sizeof(path), fmt, dump_n);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    for (k = 0; k < bytes; k += 4)
    {
        if (bcm_video_rd(0xC0000000u + off + k, &w) < 0)
            break;
        memcpy(buf + n, &w, 4);
        n += 4;
        if (n == VPC_PIECE)
        {
            write(fd, buf, n);
            n = 0;
        }
    }
    if (n)
        write(fd, buf, n);
    close(fd);
    ui_log("video", "dump %s: %lu bytes", path, (unsigned long)k);
}

static void dump_if_asked(const struct vpc *v)
{
    uint32_t flags;

    if (!dump_n)
        return;
    flags = 0;
    if ((v->state != VPC_PLAYING && v->state != VPC_DONE) ||
        bcm_video_rd(0xDF09C, &flags) < 0 || (flags >> 24) != 0x44)
    {
        /* sessions 24-25 wrote no dumps: say why, where it's kept */
        char line[96];

        snprintf(line, sizeof(line), "player: dump %d skipped: state %d, "
                 "flags %08lx", dump_n, (int)v->state, (unsigned long)flags);
        bcm_devtest_log(line);
        return;
    }
    if (flags & 1)
        dump_one(ROCKBOX_DIR "/bcmtest/pic%d.yuv", 0x3A0000, 115200,
                 v->piece);
    if (flags & 2)
        dump_one(ROCKBOX_DIR "/bcmtest/selftest%d.bin", 0x3E0000,
                 11 * 0x2000, v->piece);
    if (flags & 4)
        dump_one(ROCKBOX_DIR "/bcmtest/vrf%d.bin", 0x3D0000, 11 * 2048,
                 v->piece);
}
#else
#define dump_if_asked(v) ((void)(v))
#endif

/* stage 6's trace (devbcm.c): every STATUS whose picture moved */
static void (*frame_hook)(const struct vpc_frame *f);

void vplayer_set_frame_hook(void (*fn)(const struct vpc_frame *f))
{
    frame_hook = fn;
}

/* stage 6's SDRAM readback after the first OPEN (vpcore.c) */
void vplayer_set_sdram_check(bool on)
{
    sd_check = on;
}

/* No caller since dev12 dropped stage 6's 6d pass; kept for the next
   SDRAM diagnostic (the 640x480 frames) */
void vplayer_set_sdram_diag(bool on)
{
    sd_diag = on;
}

static uint32_t port_mode;          /* stage 6's VPLAYER_PORT_* */


void vplayer_set_tuning(uint32_t flags)
{
    tuning = flags & VPLAYER_TUNE_ALL;
}

uint32_t vplayer_get_tuning(void)
{
    return tuning;
}

void vplayer_set_port_mode(uint32_t mode)
{
    port_mode = mode;
}

static void be_frame(const struct vpc_frame *f)
{
    if (frame_hook)
        frame_hook(f);
}

/* ---- the first-use check, run (vcheck.h) ------------------------------ */

static void vc_log(void *ctx, const char *line)
{
    (void)ctx;
    ui_log("video", "%s", line);
#ifdef SIMULATOR
    if (getenv("SIM_VLOG"))
        fprintf(stderr, "vplayer: %s\n", line);
#endif
}

/* a band left the image dead: power, boot, attach, HELLO again */
static int vc_reboot(void *ctx)
{
    struct bvp *b = ctx;

    bcm_video_off();
    if (bcm_video_boot(img, img_bytes) < 0 || bvp_attach(b, &port) ||
        bvp_hello(b) != HP_OK)
        return -1;
    return 0;
}

/* The play's band from here on: the token's, and the next start's */
static void unit_band_set(int band)
{
    unit_band = band;
    if (band < VCLK_TOP)
        fast_flags = 0;         /* the governor, at most the unit's band */
    vclk_mask &= (2u << band) - 1;
    vclk_mask |= 1u << band;
}

static void token_write(int band)
{
    struct vcheck_token t;
    struct tm *tm = get_time();

    memset(&t, 0, sizeof(t));
    t.image_fnv = BCMVIDEO_IMAGE_FNV;
    t.band = band;
    strlcpy(t.build, rbversion, sizeof(t.build));
    if (tm)
        snprintf(t.date, sizeof(t.date), "%04d-%02d-%02d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    if (vcheck_store_write(&vc_files, &t) < 0)
        ui_log("video", "unit check: token write FAILED");
}

/* vpcore's check hook (vpcore.h struct vpc_backend .check): between
   HELLO and the play's OPEN, once */
static int be_check(struct bvp *b)
{
    struct vcheck_stream s;
    struct vcheck_result r;
    struct vcheck_io io;
    uint8_t *mem;
    int h, fd, n = -1, v;
    bool boosted = false;

    if (!check_now)
        return 0;
    check_now = false;
#ifdef SIMULATOR
    /* the unchecked cap (ipodui-p19-vclkcap): .sim_vcheck_nomem and
       .sim_vcheck_wrong stand for no memory and a stream of the right
       size but wrong bytes */
    if (file_exists(ROCKBOX_DIR "/.sim_vcheck_nomem"))
        h = 0;
    else
#endif
    /* the stream and the readback's scratch: core_alloc'd for the check
       only (rule 3), given back before the play's buffers fill */
    h = core_alloc(VCHECK_FILE_BYTES + 1024 + 8);
    if (h <= 0)
    {
        ui_log("video", "unit check: no memory, playing unchecked");
        snprintf(vc_said, sizeof(vc_said), "inconclusive: no memory");
        unit_unchecked("no memory");
        return 0;
    }
    core_pin(h);
    mem = (uint8_t *)ALIGN_UP((uintptr_t)core_get_data(h), 8);
    fd = open(VPLAYER_VCHECK_STREAM, O_RDONLY);
    if (fd >= 0)
    {
        n = read(fd, mem, VCHECK_FILE_BYTES);
        close(fd);
    }
#ifdef SIMULATOR
    if (n > 0 && file_exists(ROCKBOX_DIR "/.sim_vcheck_wrong"))
        mem[n / 2] ^= 0x5a;
#endif
    if (n != (int)VCHECK_FILE_BYTES ||
        vcheck_fnv_bytes(mem, VCHECK_FILE_BYTES) != VCHECK_FILE_FNV ||
        vcheck_stream_parse(&s, mem, VCHECK_FILE_BYTES) < 0 ||
        s.pics != VCHECK_PICS)
    {
        ui_log("video", "unit check: test stream wrong (%d B), unchecked",
               n);
        snprintf(vc_said, sizeof(vc_said), "inconclusive: stream wrong");
        core_unpin(h);
        core_free(h);
        unit_unchecked("stream wrong");
        return 0;
    }
    memset(&io, 0, sizeof(io));
    io.port = &port;
    io.bvp = b;
    io.reboot = vc_reboot;
    io.log = vc_log;
    io.ctx = b;
    io.scratch = (uint16_t *)(mem + ALIGN_UP(VCHECK_FILE_BYTES, 8));
    io.sums = vcheck_sums;
    /* the shell's frame ("Preparing video...") over the test pictures */
    io.screen = (const uint16_t *)FBADDR(0, 0);
#ifdef HAVE_FE_FB_HANDOFF
    if (lcd_fb_cross())
        io.screen = (const uint16_t *)lcd_fb_read(0, 0);
#endif
    io.pitch = LCD_FBWIDTH;
#ifdef SIMULATOR
    /* tests (ipodui-p19-vcheck): /.rockbox/.sim_vcheck_band makes the
       top band's switch fall back, .sim_vcheck_bad every picture from
       the sixth decode wrong (every band fails); the check's pictures
       take 20 ms each, so a script sees its screen.  ipodui-p19-vclkcap:
       .sim_vcheck_nopic, each packet taken with no picture (inconclusive) */
    sim_video_vcheck_faults(file_exists(ROCKBOX_DIR "/.sim_vcheck_band")
                            ? 1u << VCLK_TOP : 0,
                            file_exists(ROCKBOX_DIR "/.sim_vcheck_bad")
                            ? 6 : 0, 20000);
    sim_video_vcheck_nopic(file_exists(ROCKBOX_DIR "/.sim_vcheck_nopic"));
#endif
    /* once a unit and image, about a second: the port's reads go faster
       boosted (balanced: off again at once) */
    if (!(B && B->boosted))
    {
        cpu_boost(true);
        boosted = true;
    }
    v = vcheck_run(&io, &s, vclock_ladder, VCLOCK_LADDER_N, &r);
    if (boosted)
    {
        cpu_boost(false);
    }
#ifdef SIMULATOR
    sim_video_vcheck_faults(0, 0, 0);   /* the film plays as before */
    sim_video_vcheck_nopic(false);
#endif
    core_unpin(h);
    core_free(h);
    if (v == VCHECK_PASS)
    {
        unit_band_set(r.band);
        token_write(r.band);
        vc_inconclusive = 0;
        snprintf(vc_said, sizeof(vc_said), "pass %d MHz, %lu ms, %d tried",
                 vclk_band_mhz[r.band], (unsigned long)(r.us / 1000), r.n);
        return 0;
    }
    if (v == VCHECK_INCONCLUSIVE)
    {
        /* not proven the unit's fault: no token, the play goes on at the
           stock clock, and the next play checks again, up to
           VPLAYER_VCHECK_INC_MAX times a start (each check can cost the
           user up to VCHECK_BUDGET_US) */
        vc_inconclusive++;
        snprintf(vc_said, sizeof(vc_said), "inconclusive: %s at %d MHz",
                 vcheck_why_name[r.tried[r.n - 1].why],
                 vclk_band_mhz[r.tried[r.n - 1].band]);
        unit_unchecked("inconclusive");
        if (vc_inconclusive >= VPLAYER_VCHECK_INC_MAX)
            ui_log("video", "unit check: inconclusive %d times, no more "
                   "this start", vc_inconclusive);
        return 0;
    }
    snprintf(vc_said, sizeof(vc_said), "FAIL at every band, %d tried, "
             "last %s", r.n, r.n ? vcheck_why_name[r.tried[r.n - 1].why]
             : "-");
    ui_log("video", "unit check: %.40s", vc_said);
    return BVP_E_CHECK;
}

/* vpcore's live invalidation hook: the token goes; the play steps down a
   band where it can (the image lost reloads on its own terms) */
static void be_unit_bad(int why, long a)
{
    int top = unit_band > 0 ? unit_band : fast_flags ? VCLK_TOP : -1;
    int next = top > 0 ? vcheck_ladder_next(vclock_ladder, VCLOCK_LADDER_N,
                                            top) : -1;

    unit_invalidate(why, a);
    if (next > 0)
    {
        unit_band_set(next);    /* this play's later sessions too */
        if (why != VCHECK_BAD_FAULT && B &&
            vpc_vclk_cap(&B->core, next) < 0)
            ui_log("video", "unit check: step down at the next play");
    }
}

static const struct vpc_backend backend =
{
    .port = &port, .take = bcm_video_take, .boot = be_boot,
    .cut = bcm_video_off, .release = bcm_video_release, .log = be_log,
    .open_flags = be_open_flags, .frame = be_frame,
    .hand_back = bcm_video_hand_back, .vclk = be_vclk,
    .check = be_check, .unit_bad = be_unit_bad,
};

/* ---- the file and the index -------------------------------------------- */

static long read_at(int fd, uint64_t *pos, uint64_t off, void *buf,
                    uint32_t n)
{
    long got;

    if (off != *pos && lseek(fd, (off_t)off, SEEK_SET) != (off_t)off)
        return -1;
    got = read(fd, buf, n);
    *pos = got >= 0 ? off + (unsigned long)got : (uint64_t)-1;
    return got;
}

/* The audio's handle on our file, if it is the one playing */
static int audio_handle(void)
{
    struct mp3entry *id3 = audio_current_track();

    if (!id3 || strcmp(id3->path, B->path))
        return -1;
    return ci.audio_hid;
}

static long src_read(void *ctx, uint64_t off, void *buf, uint32_t n)
{
    int hid = audio_handle();

    (void)ctx;
    if (hid >= 0 && !B->core.read_direct)
    {
        ssize_t rc = bufpeek_at(hid, (off_t)off, n, buf);

        if (rc == (ssize_t)n)
            return n;
        if (rc == ERR_HANDLE_NOT_DONE)
            return VPC_NOT_BUFFERED;
    }
    /* reclaimed behind the audio, or the audio isn't on this file yet:
       the file itself (counted; a long FIFO stall or a seek) */
    B->direct++;
    return read_at(B->fd, &B->fd_pos, off, buf, n);
}

static long idx_read(void *ctx, uint64_t off, void *buf, uint32_t n)
{
    (void)ctx;
    return read_at(B->vix_fd, &B->vix_pos, off, buf, n);
}

static void src_wait(void)
{
    sleep(1);
}

static const struct vpc_source source =
{
    .ctx = NULL, .read = src_read, .index_read = idx_read, .wait = src_wait,
};

/* The file's mtime as the library has it (its directory entry) */
static uint32_t file_mtime(const char *path)
{
    char dirpath[MAX_PATH];
    const char *name = strrchr(path, '/');
    struct dirent *e;
    uint32_t mtime = 0;
    DIR *d;

    if (!name || (size_t)(name - path) >= sizeof(dirpath))
        return 0;
    memcpy(dirpath, path, name - path);
    dirpath[name - path] = '\0';
    d = opendir(dirpath[0] ? dirpath : "/");
    if (!d)
        return 0;
    while ((e = readdir(d)))
        if (!strcmp(e->d_name, name + 1))
        {
            mtime = dir_get_info(d, e).mtime;
            break;
        }
    closedir(d);
    return mtime;
}

/* The .vix, built now if the library update hasn't (player.md section 1:
   parse at open, once) */
static int open_index(const char *path, int fd)
{
    char vix[MAX_PATH];
    uint64_t size = ffilesize(fd);
    uint32_t mtime = file_mtime(path);
    int h, vfd;

    if (vix_path(VIX_DIR, path, size, mtime, vix, sizeof(vix)) < 0)
        return -1;
    vfd = open(vix, O_RDONLY);
    if (vfd >= 0)
        return vfd;
    h = core_alloc(BUILD_SCRATCH);
    if (h <= 0)
        return -1;
    if (vix_build_file(path, VIX_DIR, size, mtime, core_get_data(h),
                       BUILD_SCRATCH) < 0)
    {
        core_free(h);
        ui_log("video", "no index for %s", path);
        return -1;
    }
    core_free(h);
    return open(vix, O_RDONLY);
}

/* The player's image (bcmres_image: VPLAYER_IMAGE, the resident one) or
   the probe image (VPLAYER_PROBE_IMAGE), with the size and FNV-1a this
   build was made with; read by bcmres_read() (bcmres.c) */
#ifndef SIMULATOR
static const struct vimage probe_image =
    { VPLAYER_PROBE_IMAGE, BCMPROBE_IMAGE_BYTES, BCMPROBE_IMAGE_FNV };
#endif

/* The image for a play (resident_ok: the one kept from boot will do) or
   for a lab that boots it afresh: the player's, or (vi) the probe image */
static bool load_image_from(const struct vimage *vi, bool resident_ok)
{
#ifdef HAVE_BCMFW_RESIDENT
    if (resident_ok && bcmres_img && vi == &bcmres_image)
    {
        img = bcmres_img;       /* not ours to free: img_h stays 0 */
        img_bytes = bcmres_bytes;
        return true;
    }
#else
    (void)resident_ok;
#endif
#ifdef SIMULATOR
    /* which image a stop hands the chip back to (sim_video.h): on the
       device only the resident image's own play is handed back to it;
       a bench or stage play, a lab or a probe boots its image from the
       file, and its stop leaves the chip on Apple's vmcs, the jobs off
       (bcm2722-reference quirks #48) */
    sim_video_resident(resident_ok && vi == &bcmres_image && !bench_play
                       && !sd_check && !sd_diag);
    /* the model needs no image; a file, if there is one, is still read */
    {
        int fd = open(vi->path, O_RDONLY);

        if (fd < 0)
        {
            img = NULL;
            img_bytes = 0;
            return true;
        }
        close(fd);
    }
#endif
    img_h = bcmres_read(vi, &img, &img_bytes);
    if (!img_h)
        img = NULL;
    return img_h > 0;
}

static bool load_image(bool resident_ok)
{
    return load_image_from(&bcmres_image, resident_ok);
}

/* ---- the clock --------------------------------------------------------- */

/* The audio position, finer than the elapsed field's steps: it moves
   when the PCM buffer plays a chunk, and between moves the clock runs
   on (at most 200 ms: a stall shows as a stop, not as drift) */
static int64_t audio_us(bool playing)
{
    struct mp3entry *id3 = audio_current_track();
    uint32_t now = bcm_video_now_us(), since;
    int64_t target;
    bool pcm_on;

    if (!id3)
        return (int64_t)B->el * 1000;
    /* elapsed is the DAC's position at the start of the PCM chunk now
       playing (pcmbuf.c stamps chunks; 8 KB, about 46 ms), so the clock
       extrapolates from its last change, but only while the playback
       channel really plays: before the PCM starts (the codec buffering
       after "play"), dev24's extrapolation ran picture 0 to its 200 ms
       cap (session 23), and dev26's "wait for elapsed to move" froze the
       clock through a first coarse update, a 420 ms leap (session 24) */
    pcm_on = playing &&
        mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) == CHANNEL_PLAYING;

    if (id3->elapsed != B->el || !pcm_on)
    {
        B->el = id3->elapsed;   /* not playing: the clock restarts from here */
        B->el_at_us = now;
    }
    since = pcm_on ? now - B->el_at_us : 0;
    if (since > 600000)
        since = 600000;         /* a stalled stream holds, it doesn't run on */
    target = (int64_t)B->el * 1000 + since - VPLAYER_PRIME_US;
    /* The PCM chunks written during the prefill carry the decoder's
       position, ahead of the speaker, so elapsed leaps ~430 ms about 100
       ms in and drains back over a second (sessions 24-25: a GOP dropped
       every start).  The clock is the wall time since the PCM started,
       pulled toward elapsed by at most 1/VPLAYER_SLEW of the time passed;
       only a jump past 1.5 s (a seek the shell didn't announce) snaps. */
    if (!pcm_on)
    {
        B->clk_on = false;
        return target;
    }
    if (!B->clk_on)
    {
        B->clk_on = true;
        B->clk = target;
        B->clk_t = now;
    }
    else
    {
        uint32_t dt = now - B->clk_t;
        int64_t err, lim = dt / VPLAYER_SLEW;

        B->clk += dt;
        B->clk_t = now;
        err = target - B->clk;
        if (err > 1500000 || err < -1500000)
            B->clk = target;
        else
            B->clk += err > lim ? lim : err < -lim ? -lim : err;
    }
    if (B->full && B->dbg->tr_n < ARRAYLEN(B->dbg->tr) &&
        now - B->tr_t0 < 3000000 &&
        (!B->dbg->tr_n || B->dbg->tr[B->dbg->tr_n - 1].el != id3->elapsed ||
         B->dbg->tr[B->dbg->tr_n - 1].on != pcm_on))
    {
        B->dbg->tr[B->dbg->tr_n].t = now - B->tr_t0;
        B->dbg->tr[B->dbg->tr_n].el = id3->elapsed;
        B->dbg->tr[B->dbg->tr_n].on = pcm_on;
        B->dbg->tr[B->dbg->tr_n].us = (uint32_t)B->clk;
        B->dbg->tr[B->dbg->tr_n].err = (int32_t)(target - B->clk);
        B->dbg->tr[B->dbg->tr_n].gap = B->loop_gap;
        B->loop_gap = 0;
        B->dbg->tr_n++;
    }
    return B->clk;
}

/* Stage 6: the start-up clock trace to the test's log, once the session
   is over (no file I/O while the video plays) */
static void clock_trace(void)
{
#ifndef SIMULATOR
    char line[192];
    uint32_t k;

    if (!B->dbg)
        return;
    snprintf(line, sizeof(line), "player: === %s ===", B->path);
    bcm_devtest_log(line);
    {
        uint32_t n = B->dbg->log_cap, first = 0;

        if (B->dbg->log_n > n)
        {
            first = B->dbg->log_n - n;
            snprintf(line, sizeof(line), "player: %lu earlier lines not kept",
                     (unsigned long)first);
            bcm_devtest_log(line);
        }
        for (k = first; k < B->dbg->log_n; k++)
            bcm_devtest_log(B->dbg->log[k % n]);
    }
    B->dbg->log_n = 0;
    for (k = 0; k < B->dbg->tr_n; k++)
    {
        snprintf(line, sizeof(line), "player: clock at %lu ms: elapsed %lu, "
                 "pcm %s, video clock %lu ms, elapsed-based %+ld ms; "
                 "longest loop gap %lu ms",
                 (unsigned long)(B->dbg->tr[k].t / 1000),
                 (unsigned long)B->dbg->tr[k].el, B->dbg->tr[k].on ? "on" : "off",
                 (unsigned long)(B->dbg->tr[k].us / 1000),
                 (long)(B->dbg->tr[k].err / 1000),
                 (unsigned long)(B->dbg->tr[k].gap / 1000));
        bcm_devtest_log(line);
    }
#endif
}

/* ---- the thread -------------------------------------------------------- */

static void hold_backlight(bool hold)
{
    if (hold == B->held)
        return;
    B->held = hold;
    /* the 7G keeps a playing video lit; paused, the timeout applies */
    if (hold)
    {
        backlight_set_timeout(0);
#if CONFIG_CHARGING
        backlight_set_timeout_plugged(0);
#endif
    }
    else
    {
        backlight_set_timeout(global_settings.backlight_timeout);
#if CONFIG_CHARGING
        backlight_set_timeout_plugged(global_settings.backlight_timeout_plugged);
#endif
    }
}

/* What the thread sees around it, logged on change (dev38: the owner's
   freeze at the same time after every start; the shell's timers, the CPU
   clock, the backlight, and this thread running late) */
static void watch(uint32_t now, uint32_t wait_ms)
{
    uint32_t over = 0;
    int bl = is_backlight_on(true), st = audio_status(), was = B->w_audio;

    if (B->loop_t)
    {
        uint32_t gap = now - B->loop_t;

        over = gap > wait_ms * 1000 ? gap - wait_ms * 1000 : 0;
    }
    if (over > VPLAYER_LATE_US &&
        (over > 2 * B->late_said || now - B->late_at > 5000000))
    {
        be_log("video: thread late (ms past its wait, audio ms)",
               (long)(over / 1000), (long)(B->last_a / 1000));
        B->late_said = over;
        B->late_at = now;
    }
    if (bl != B->w_bl)
    {
        be_log("video: backlight (on, audio ms)", bl, (long)(B->last_a / 1000));
        B->w_bl = bl;
    }
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    if (cpu_frequency != B->w_freq)
    {
        be_log("watch: CPU MHz (now, was)", cpu_frequency / 1000000,
               B->w_freq / 1000000);
        B->w_freq = cpu_frequency;
    }
#endif
    if (st != B->w_audio)
    {
        be_log("watch: audio status (now, was)", st, B->w_audio);
        B->w_audio = st;
    }
    if (B->core.eng.stats.dropped_gops != B->w_drops ||
        B->core.eng.stats.skipped > B->w_skipped + 30)
    {
        /* a GOP dropped (pictures too late), or a run of skips */
        be_log("video: dropped GOPs, skipped (audio ms)",
               (long)B->core.eng.stats.dropped_gops,
               (long)(B->last_a / 1000));
        B->w_drops = B->core.eng.stats.dropped_gops;
        B->w_skipped = B->core.eng.stats.skipped;
    }
    if ((st ^ was) & AUDIO_STATUS_PAUSE)
        be_log(st & AUDIO_STATUS_PAUSE ? "video: paused (clock ms, elapsed)"
                                       : "video: resumed (clock ms, elapsed)",
               (long)(B->last_a / 1000),
               (long)(audio_current_track() ?
                      audio_current_track()->elapsed : 0));
}

/* ---- stage 8b: PCM through the chip and back (dev39) ------------------ */

/* A 440 Hz tone, made here, written into the chip's SDRAM ring, passed
   through the chip at a gain and read back, then played on the mixer's
   voice channel over the soundtrack.  The owner hears the round trip; the
   log has its cost.  Blocks of 1024 stereo frames (23 ms at 44.1 kHz). */
#define AL_BLOCKS   8
#define AL_FRAMES   1024
#define AL_BYTES    (AL_FRAMES * 4)
#define AL_IN       0x340000    /* bcmfw video.h VIDEO_AUDIO_OFF */
#define AL_OUT      0x360000
#define AL_RING     (AL_BLOCKS * AL_BYTES)

struct aloop
{
    int16_t ring[AL_BLOCKS][AL_FRAMES * 2];
    volatile uint32_t produced, next;   /* blocks made; the next to play */
    volatile uint32_t underruns;
    bool playing;                       /* the mixer channel runs */
    uint32_t sessions;                  /* the core's opens when configured */
    uint32_t phase;                     /* Q16 into the sine table */
    uint32_t blocks, write_us, call_us, chip_us, read_us, fails;
};

static uint32_t al_gain;                /* Q16; 0: off (vplayer_set_audio_loop) */
static int16_t al_silence[256];

void vplayer_set_audio_loop(uint32_t gain_q16)
{
    al_gain = gain_q16;
}

static int16_t al_sine(uint32_t i)      /* i in 0..255: one period */
{
    /* a parabola per half period (close enough to hear a pure tone) */
    int32_t x = (int32_t)(i & 127) - 64, y = 8000 - (8000 * x * x) / 4096;

    return (int16_t)(i & 128 ? -y : y);
}

static void al_more(const void **start, size_t *size)
{
    struct aloop *a = AL;

    if (a && a->next < a->produced)
    {
        *start = a->ring[a->next % AL_BLOCKS];
        *size = AL_BYTES;
        a->next++;
        return;
    }
    if (a)
        a->underruns++;
    *start = al_silence;
    *size = sizeof(al_silence);
}

static const struct mixer_play_cbs al_cbs = { .get_more = al_more };

/* One block a turn, at most, while the session plays */
static void al_tick(struct vpc *v)
{
    struct aloop *a = AL;
    struct vpc_audio_times t;
    int16_t *blk;
    uint32_t k, opens = v->stats.restarts + v->stats.wakes +
                        v->stats.reloads + v->stats.recoveries + 1;

    if (!a || v->state != VPC_PLAYING || !v->bvp.session)
        return;
    if (a->sessions != opens)
    {
        /* each OPEN forgets the rings */
        if (vpc_audio_config(v, AL_IN, AL_OUT, AL_RING))
        {
            a->fails++;
            return;
        }
        a->sessions = opens;
    }
    if (a->produced >= a->next + AL_BLOCKS - 2)
        return;                         /* the ring is full */
    blk = a->ring[a->produced % AL_BLOCKS];
    for (k = 0; k < AL_FRAMES; k++)
    {
        int16_t y = al_sine(a->phase >> 16 & 255);

        blk[2 * k] = blk[2 * k + 1] = y;
        a->phase += 167393;             /* 440 Hz at 44.1 kHz, 256 a period */
    }
    if (vpc_audio_block(v, AL_IN, AL_OUT,
                        (a->produced % AL_BLOCKS) * AL_BYTES,
                        (uint16_t *)blk, AL_BYTES, al_gain, &t))
    {
        a->fails++;
        return;
    }
    a->blocks++;
    a->write_us += t.write_us;
    a->call_us += t.call_us;
    a->chip_us += t.chip_us;
    a->read_us += t.read_us;
    a->produced++;
    if (!a->playing && a->produced >= 3)
    {
        mixer_channel_set_amplitude(PCM_MIXER_CHAN_VOICE, MIX_AMP_UNITY);
        mixer_channel_play_data(PCM_MIXER_CHAN_VOICE, &al_cbs, NULL, 0);
        a->playing = true;
    }
}

static void al_stop(void)
{
    struct aloop *a = AL;

    if (!a)
        return;
    if (a->playing)
        mixer_channel_stop(PCM_MIXER_CHAN_VOICE);
    be_log("video: audio loop blocks, underruns", (long)a->blocks,
           (long)a->underruns);
    if (a->blocks)
    {
        be_log("video: audio loop us a block: write, call",
               (long)(a->write_us / a->blocks), (long)(a->call_us / a->blocks));
        be_log("video: audio loop us a block: chip, read back",
               (long)(a->chip_us / a->blocks), (long)(a->read_us / a->blocks));
    }
    be_log("video: audio loop failures, gain Q16", (long)a->fails,
           (long)al_gain);
    AL = NULL;
    core_unpin(al_h);
    core_free(al_h);
    al_h = 0;
}

/* The session's port and thread load (dev39: PP DMA against the CPU's
   copy, boosted or not) */
static void port_report(void)
{
    struct bcm_video_port_load l;
    uint32_t wall = bcm_video_now_us() - B->tr_t0;

    bcm_video_flush();
    bcm_video_port_load(&l, true);
    be_log("video: port KB, MB/s (busy time)", (long)(l.bytes >> 10),
           (long)(l.cpu_us + l.dma_us ?
                  (uint64_t)l.bytes / (l.cpu_us + l.dma_us) : 0));
    be_log("video: port CPU ms, blocks", (long)(l.cpu_us / 1000),
           (long)l.cpu_blocks);
    be_log("video: port DMA ms, blocks", (long)(l.dma_us / 1000),
           (long)l.dma_blocks);
    be_log("video: port DMA failures, tuning", (long)l.dma_fail,
           (long)tuning);
    {
        /* the logging itself, before its own two lines add to it */
        uint32_t lines = lg_lines, us = lg_us, peak = lg_peak;

        be_log("video: log lines, us formatting and appending",
               (long)lines, (long)us);
        be_log("video: log peak lines a second, permille of the session",
               (long)peak, (long)(wall ? (uint64_t)us * 1000 / wall : 0));
    }
    be_log("video: thread active permille, session s",
           (long)(wall ? (uint64_t)B->busy_us * 1000 / wall : 0),
           (long)(wall / 1000000));
}

static void vplayer_thread(void)
{
    struct vpc *v = &B->core;
    struct queue_event ev;
    uint32_t wait_ms = 10;
    bool playing;
    int64_t a;
    int st;

    st = audio_status();
    playing = (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
    B->last_a = audio_us(playing);
    B->tr_t0 = bcm_video_now_us();     /* the start-up clock trace */
    /* boosted while the session plays (dev39: session 31's PP sat at
       30 MHz between the audio's boosts and the thread fell seconds
       behind); the boot and OPEN too */
    if (tuning & VPLAYER_TUNE_BOOST)
    {
        cpu_boost(true);
        B->boosted = true;
    }
    be_log("video: tuning (boost|dma<<2|chip27<<3|e2<<4)", tuning, 0);
    vpc_start(v, B->last_a);
    B->up = true;
    for (;;)
    {
        long tmo = (wait_ms * HZ + 999) / 1000;
        uint32_t t_loop = bcm_video_now_us();

        if (B->loop_t && t_loop - B->loop_t > B->loop_gap)
            B->loop_gap = t_loop - B->loop_t;
        watch(t_loop, wait_ms);
        B->loop_t = t_loop;

        if (B->work_t0)
            B->busy_us += t_loop - B->work_t0;  /* the last turn's work */
        /* a DMA block's port lock (held per piece, jobs.md section 6)
           ends before the thread blocks: nothing waits a whole turn on
           it.  The CPU's copy (the default) has nothing in flight. */
        bcm_video_flush();
        queue_wait_w_tmo(&vq, &ev, tmo > 0 ? tmo : 1);
        B->work_t0 = bcm_video_now_us();
        if (ev.id == VQ_STOP)
            break;
        st = audio_status();
        playing = (st & AUDIO_STATUS_PLAY) && !(st & AUDIO_STATUS_PAUSE);
        if (ev.id == VQ_OVERLAY)
        {
            /* the shell's frame, where it drew the overlay (its lcd_update
               reaches nothing while the video owns the panel) */
            const uint16_t *ovl_px =
                (const uint16_t *)FBADDR(ovl_rect[0], ovl_rect[1]);
#ifdef HAVE_FE_FB_HANDOFF
            /* MF-4: the shell drew it on the COP and published it before
               the post (vplayer_overlay_slide): read it uncached */
            if (lcd_fb_cross())
                ovl_px = (const uint16_t *)lcd_fb_read(ovl_rect[0],
                                                       ovl_rect[1]);
#endif
            ovl_rc = vpc_overlay_slide(v, ovl_rect[0], ovl_rect[1],
                                       ovl_rect[2], ovl_rect[3], ovl_px,
                                       LCD_FBWIDTH, ovl_slide);
            ovl_done = true;
            be_log("video: overlay (h, rc)", (long)ovl_rect[3], ovl_rc);
        }
        if (ev.id == VQ_SEEK)
        {
            be_log("video: seek (to, from ms)", (long)ev.data,
                   (long)(B->last_a / 1000));
            B->el = (unsigned long)ev.data;
            B->el_at_us = bcm_video_now_us();
            B->clk_on = false;          /* re-anchored at the new place */
            B->last_a = (int64_t)ev.data * 1000;
            vpc_seek(v, B->last_a);
        }
        if (v->state == VPC_IDLE || v->state == VPC_FAILED)
        {
            hold_backlight(false);
            wait_ms = 250;
            continue;
        }
        a = audio_us(playing);
        /* the audio jumped while playing our file (a seek the shell made,
           a repeat): follow.  Not at its end: the clock can reset there */
        if (playing && audio_handle() >= 0 &&
            (a + 300000 < B->last_a || a > B->last_a + 1500000))
        {
            be_log("video: audio jumped (from, to ms)",
                   (long)(B->last_a / 1000), (long)(a / 1000));
            vpc_seek(v, a);
        }
        B->last_a = a;
        vpc_tick(v, a, playing, is_backlight_on(true), &wait_ms);
        if (playing)
        {
            al_tick(v);         /* two blocks a turn at most: 46 ms of */
            al_tick(v);         /* tone a 10 ms turn keeps ahead */
        }
        hold_backlight(playing && v->state == VPC_PLAYING);
        {
            /* paused, dark, failed or done: the boost goes */
            bool want = playing && v->state == VPC_PLAYING &&
                        (tuning & VPLAYER_TUNE_BOOST);

            if (want != B->boosted)
            {
                cpu_boost(want);
                B->boosted = want;
                be_log("watch: boost held (now)", want, 0);
            }
        }
        if (v->state == VPC_DONE)
            B->ended = true;
        /* the audio moved on to another file, or stopped: video ends
           (the shell's screen calls vplayer_stop() too) */
        if (!(st & AUDIO_STATUS_PLAY) || audio_handle() < 0)
        {
            if (!B->stop_after)
                B->stop_after = current_tick + HZ;
            else if (TIME_AFTER(current_tick, B->stop_after))
            {
                dump_if_asked(v);
                vpc_stop(v);
                ui_log("video", "audio left the file: video stopped");
            }
        }
        else
            B->stop_after = 0;
    }
    dump_if_asked(v);
    al_stop();
    port_report();
    vpc_stop(v);
    bcm_video_flush();      /* no port lock left held by this thread */
    hold_backlight(false);
    if (B->boosted)
    {
        cpu_boost(false);
        B->boosted = false;
    }
    clock_trace();
    thread_over = true;
    thread_exit();              /* vplayer_stop() waits, then frees */
}

/* The job ring's owner is the shell's UI thread (bcmjob_init's), which
 * starts and stops plays: this thread's take and release (dt_take,
 * dt_restore: jd_suspend) are requests it must acknowledge within
 * BJC_ACK_MS, from its input pump.  While it waits on this thread here
 * (the pre-roll, the stop) it pumps itself; without that the take timed
 * out and was refused (bench 2026-10-25: stage 6, "the BCM is busy") */
void vplayer_pump(void)
{
#if defined(HAVE_BCM_JOBS) && !defined(SIMULATOR)
    bcmjob_apply();             /* nothing unless this is the owner */
#endif
}

/* A probe's struct bvp (about 1.9 KB: off the UI thread's stack), pinned
   because it's held across yields and zeroed; NULL when there's no room.
   Taken after load_image(), so it can't split the image's block; given
   back by probe_bvp_free(), which free_all() calls */
static struct bvp *probe_bvp(void)
{
    struct bvp *bp;

    if (bvp_h > 0)
        return NULL;            /* one probe at a time */
    bvp_h = core_alloc(sizeof(struct bvp));
    if (bvp_h <= 0)
    {
        bvp_h = 0;
        return NULL;
    }
    core_pin(bvp_h);
    bp = core_get_data(bvp_h);
    memset(bp, 0, sizeof(*bp));
    return bp;
}

static void probe_bvp_free(void)
{
    if (bvp_h > 0)
    {
        core_unpin(bvp_h);
        core_free(bvp_h);
    }
    bvp_h = 0;
}

static void free_all(void)
{
    vclk_total();
    if (B)
    {
        if (B->fd >= 0)
            close(B->fd);
        if (B->vix_fd >= 0)
            close(B->vix_fd);
    }
    B = NULL;
    probe_bvp_free();
    if (al_h > 0)
    {
        AL = NULL;
        core_unpin(al_h);
        core_free(al_h);
        al_h = 0;
    }
    if (keys_h > 0)
        core_free(keys_h);
    if (dbg_h > 0)
    {
        core_unpin(dbg_h);
        core_free(dbg_h);
    }
    if (img_h > 0)
    {
        core_unpin(img_h);
        core_free(img_h);
    }
    if (blk_h > 0)
    {
        core_unpin(blk_h);
        core_free(blk_h);
    }
    blk_h = keys_h = img_h = dbg_h = 0;
    img = NULL;
}

static int open_why, open_err;      /* the last start's failure */

bool vplayer_start(const char *path)
{
    long nk, preroll;
    uint32_t t[5];

    vplayer_stop();
    open_why = VPLAYER_OPEN_MEMORY;
    open_err = 0;
    blk_h = core_alloc(sizeof(struct vblock) + 8);
    if (blk_h <= 0)
    {
        blk_h = 0;
        ui_log("video", "no memory");
        return false;
    }
    core_pin(blk_h);
    B = (struct vblock *)ALIGN_UP((uintptr_t)core_get_data(blk_h), 8);
    memset(B, 0, offsetof(struct vblock, mem));
    strlcpy(B->path, path, sizeof(B->path));
    /* stage 6-8, and a normal play only with the developer's full log on
       (dev41: every video build is a device-test build, so dev35's "every
       play" meant every play; by default a play now logs its events and
       the end summary, nothing per picture or per tick) */
    B->full = sd_check || sd_diag || full_log;
    {
        uint32_t cap = B->full ? VLOG_FULL : VLOG_QUIET;
        size_t sz = sizeof(struct vdebug) + cap * sizeof(B->dbg->log[0]);

        dbg_h = core_alloc(sz + 8);
        if (dbg_h > 0)
        {
            core_pin(dbg_h);
            B->dbg = (struct vdebug *)ALIGN_UP(
                (uintptr_t)core_get_data(dbg_h), 8);
            memset(B->dbg, 0, sz);
            B->dbg->log_cap = cap;
        }
        else
            dbg_h = 0;
    }
    B->vix_fd = -1;
    t[0] = bcm_video_now_us();
    B->fd = open(path, O_RDONLY);
    open_why = VPLAYER_OPEN_FILE;
    if (B->fd < 0)
        goto fail;
    open_why = VPLAYER_OPEN_INDEX;
    if ((B->vix_fd = open_index(path, B->fd)) < 0)
        goto fail;
    t[1] = bcm_video_now_us();
    open_why = VPLAYER_OPEN_IMAGE;
    if (!load_image(true))
        goto fail;
    t[2] = bcm_video_now_us();
    check_fast_clock(true);
    check_vclk();
    if (al_gain)
    {
        al_h = core_alloc(sizeof(struct aloop) + 8);
        if (al_h > 0)
        {
            core_pin(al_h);
            AL = (struct aloop *)ALIGN_UP((uintptr_t)core_get_data(al_h), 8);
            memset(AL, 0, sizeof(*AL));
        }
        else
            al_h = 0;
    }
    if (tuning & VPLAYER_TUNE_E2)
        bcm_video_set_dma_e2(true);
    else
        bcm_video_set_dma(!!(tuning & VPLAYER_TUNE_DMA));
    lg_lines = lg_us = lg_peak = lg_sec_n = 0;      /* this play's logging */
    {
        struct bcm_video_port_load l;

        bcm_video_port_load(&l, true);  /* this play's load only */
    }
    nk = vpc_init(&B->core, &source, &backend, B->mem, sizeof(B->mem));
    B->core.sd_check = sd_check || sd_diag;
    B->core.sd_whole = sd_diag;
    B->core.wire.pace = port_mode & VPLAYER_PORT_PACE ? VPLAYER_PACE_HW : 0;
#ifdef HAVE_BCMFW_DEVTEST
    /* every play (dev36): each piece's first and last word read back,
       two reads a 2 KB piece; session 30's grey pictures and stalls
       may be bytes gone wrong on the port */
    B->core.wire.verify = true;
#else
    B->core.wire.verify = !!(port_mode & VPLAYER_PORT_VERIFY);
#endif
    B->core.lockstep = !!(port_mode & VPLAYER_PORT_LOCK);
    B->core.vclk_force = vclk_force >= 0 ? (uint32_t)vclk_force + 1 : 0;
    B->core.check_due = check_now;
    {
        uint32_t w[8];

        bcm_video_port_stats(w, true);  /* this clip's handshakes only */
    }
    if (nk <= 0)
    {
        ui_log("video", "index refused (%ld)", nk);
        open_why = VPLAYER_OPEN_DAMAGED;
        open_err = nk < 0 ? (int)nk : MP4_E_TABLE;
        goto fail;
    }
    open_why = VPLAYER_OPEN_MEMORY;
    keys_h = core_alloc(nk * sizeof(struct vp_key));
    if (keys_h <= 0)
    {
        keys_h = 0;
        goto fail;
    }
    core_pin(keys_h);
    if ((open_err = vpc_keys(&B->core, core_get_data(keys_h),
                             (uint32_t)nk)) < 0)
    {
        ui_log("video", "index keys refused (%d)", open_err);
        open_why = VPLAYER_OPEN_DAMAGED;
        goto fail;
    }
    open_err = 0;
    t[3] = bcm_video_now_us();

    /* the first-use check runs inside the pre-roll: about a second a
       band (vcheck.h), so its bound grows by the ladder's worst case */
    preroll = 2 * HZ + (B->core.check_due ? VCLOCK_LADDER_N * 3 * HZ / 2
                                          : 0);
    queue_init(&vq, true);
    thread_over = false;
    thread_id = create_thread(vplayer_thread, B->stack, sizeof(B->stack), 0,
                              "video" IF_PRIO(, VPLAYER_PRIO)
                              IF_COP(, CPU));
    if (!thread_id)
    {
        queue_delete(&vq);
        goto fail;
    }
    /* pre-roll (session 22: every pass skipped its first 14 pictures):
       the image's boot and OPEN (300-600 ms) finish before the caller
       starts the audio, so picture 0 is not judged late.  Bounded: a
       stuck boot gives up after 2 s and the audio starts anyway. */
    {
        int i;

        for (i = 0; i < preroll && !B->up; i++)
        {
            vplayer_pump();     /* the thread's take (vplayer_pump) */
            sleep(1);
        }
    }
    t[4] = bcm_video_now_us();
    ui_log("video", "start %s: %lu samples, %ld keys", path,
           (unsigned long)B->core.hdr.count, nk);
    /* where a start's time goes (dev37: the owner waits 1-2 s): the file
       and its index opened, the image read, the index scanned and its
       keys, then the BCM's boot, HELLO and OPEN (the pre-roll) */
    ui_log("video", "start ms: open %lu, image %lu, scan %lu, boot %lu",
           (unsigned long)(t[1] - t[0]) / 1000,
           (unsigned long)(t[2] - t[1]) / 1000,
           (unsigned long)(t[3] - t[2]) / 1000,
           (unsigned long)(t[4] - t[3]) / 1000);
#ifdef HAVE_BCMFW_RESIDENT
    {
        /* p14-resident: the image was the display driver (boot then 0:
           HELLO and OPEN only), and what its idle sleep did meanwhile */
        uint32_t c[5];

        bcm_video_idle_counters(c);
        if (c[4])
            ui_log("video", "resident: no boot; idle sleeps %lu, ticks %lu, "
                   "bells %lu, bad %lu", (unsigned long)c[2],
                   (unsigned long)c[0], (unsigned long)c[1],
                   (unsigned long)c[3]);
    }
#endif
    B->core.start_us = t[0];
    open_why = 0;
    return true;

fail:
    free_all();
    return false;
}

bool vplayer_running(void)
{
    return thread_id != 0;
}

int vplayer_overlay(int x, int y, int w, int h)
{
    return vplayer_overlay_slide(x, y, w, h, 0);
}

int vplayer_overlay_slide(int x, int y, int w, int h, unsigned slide)
{
    int i;

    if (!thread_id)
        return -1;
    ovl_slide = slide;
    ovl_rect[0] = x < 0 ? 0 : x;
    ovl_rect[1] = y < 0 ? 0 : y;
    ovl_rect[2] = w < 0 ? 0 : w;
    ovl_rect[3] = h < 0 ? 0 : h;
    ovl_done = false;
    /* MF-4: the rect's pixels to SDRAM before the thread reads them (it
       cannot ask for a fence: this thread sleeps below until it is done) */
    lcd_fb_publish(ovl_rect[0], ovl_rect[1], ovl_rect[2], ovl_rect[3]);
    queue_post(&vq, VQ_OVERLAY, 0);
    /* the player's thread writes the pixels: the caller's frame must not
       change under it, so wait (bounded) */
    for (i = 0; i < HZ / 2 && !ovl_done; i++)
        sleep(1);
    return ovl_done ? ovl_rc : -1;
}

/* Stage 8a-0 (dev39): the image booted without a session, its GPIO
   levels sampled for us microseconds (HP_OP_PINS: inputs, never driven),
   then the BCM back.  0 with out[8] filled, or < 0. */
int vplayer_probe_pins(uint32_t mask, uint32_t us, uint32_t pull,
                       uint32_t out[8])
{
    struct bvp *bp;
    uint32_t a[3], n = 0;
    int rc;

    if (thread_id || B)
        return -1;              /* a video holds the BCM */
    if (!load_image(false))
        return -2;
    bp = probe_bvp();
    if (!bp)
    {
        free_all();
        return VPLAYER_E_NOMEM;
    }
    if (bcm_video_take() < 0)
    {
        free_all();
        return -3;
    }
    rc = bcm_video_boot(img, img_bytes);
    if (rc == 0)
    {
        rc = bvp_attach(bp, &port);
        if (rc == 0)
            rc = bvp_hello(bp);
        if (rc == HP_OK)
        {
            a[0] = mask;
            a[1] = us;
            a[2] = 1 | (pull & 3) << 2;     /* inputs; a pull, or none */
            memset(out, 0, 8 * sizeof(uint32_t));
            rc = bvp_call(bp, HP_OP_PINS, a, 3, out, 8, &n, us + 300000);
            if (rc == HP_OK && n < 8)
                rc = BVP_E_PROTO;
        }
    }
    bcm_video_off();
    bcm_video_release();
    free_all();
    return rc;
}

/* Stage 8a-1 (dev41, dev45): the image's TONE op, outside any video.
   DRIVES the BCM's audio pins: the caller must have checked they float.
   0 with out[] (vplayer.h), or < 0. */
int vplayer_tone(uint32_t ms, uint32_t div, uint32_t layout, uint32_t step,
                 uint32_t start, uint32_t out[VPLAYER_TONE_OUT])
{
    struct bvp *bp;
    uint32_t a[6], n = 0;
    int rc;

    if (thread_id || B)
        return -1;
    if (!load_image(false))
        return -2;
    bp = probe_bvp();
    if (!bp)
    {
        free_all();
        return VPLAYER_E_NOMEM;
    }
    if (bcm_video_take() < 0)
    {
        free_all();
        return -3;
    }
    rc = bcm_video_boot(img, img_bytes);
    if (rc == 0)
        rc = bvp_attach(bp, &port);
    if (rc == 0)
        rc = bvp_hello(bp);
    if (rc == HP_OK)
    {
        a[0] = HP_TONE_MAGIC;
        a[1] = ms;
        a[2] = div;
        a[3] = layout;
        a[4] = step;
        a[5] = start;
        memset(out, 0, VPLAYER_TONE_OUT * sizeof(uint32_t));
        rc = bvp_call(bp, HP_OP_TONE, a, start ? 6 : 5, out,
                      VPLAYER_TONE_OUT, &n, ms * 1000 + 500000);
    }
    bcm_video_off();
    bcm_video_release();
    free_all();
    return rc;
}

/* The DMA lab (dev40): blocks written by PP DMA into the session's FIFO,
   each read back word by word, by length, with the word-0 rewrite off and
   on; one line per case through log().  0, or < 0 if the BCM couldn't be
   brought up. */
int vplayer_dma_lab(void (*log)(const char *line))
{
    static const uint32_t lens[] = { 256, 1024, 2048, 8192, 32768 };
    struct bvp *bp;
    uint32_t k, f, i;
    char line[160];
    int h, rc;
    uint32_t *buf;

    if (thread_id || B)
        return -1;
    if (!load_image(false))
        return -2;
    bp = probe_bvp();
    if (!bp)
    {
        free_all();
        return VPLAYER_E_NOMEM;
    }
    h = core_alloc(32768 + 32);
    if (h <= 0)
    {
        free_all();
        return -4;
    }
    core_pin(h);
    buf = (uint32_t *)ALIGN_UP((uintptr_t)core_get_data(h), 32);
    if (bcm_video_take() < 0)
    {
        core_unpin(h);
        core_free(h);
        free_all();
        return -3;
    }
    rc = bcm_video_boot(img, img_bytes);
    if (rc == 0)
        rc = bvp_attach(bp, &port);
    if (rc == 0)
        rc = bvp_hello(bp);
    if (rc == HP_OK)
        rc = bvp_open(bp, 320, 240, 0);     /* SDRAM up: the FIFO */
    /* f: 0/1 channel 3 with the word-0 rewrite off/on into the SDRAM
       FIFO; 2 channel 3 into SRAM (the host frame at 0xE0000); 3 the
       second engine into the SDRAM FIFO, the player's own path (session
       37's E2 lab: it delivers).  Every block's pattern names its row and
       size, so a block that moves nothing reads back wrong */
    for (f = 0; rc == HP_OK && f < 4; f++)
    {
        if (f == 3)
            bcm_video_set_dma_e2(true);
        else
            bcm_video_set_dma(true);
        bcm_video_set_dma_fixup(f == 1);
        for (k = 0; k < ARRAYLEN(lens); k++)
        {
            uint32_t base = f == 2 ? 0xE0000 + 0x8000 * (k & 3)
                                   : bp->sess_fifo + 0x10000 * k;
            uint32_t bad = 0, first = ~0u;
            uint32_t got, t0;

            for (i = 0; i < lens[k] / 4; i++)
                buf[i] = 0xA0000000u | f << 24 | k << 20 | i;
            t0 = bcm_video_now_us();
            if (bcm_video_wr_block(base, (const uint16_t *)buf, lens[k] / 2) ||
                bcm_video_flush())
            {
                snprintf(line, sizeof(line), "dma lab: fixup %lu, %lu bytes: "
                         "port error", (unsigned long)f,
                         (unsigned long)lens[k]);
                log(line);
                continue;
            }
            t0 = bcm_video_now_us() - t0;
            for (i = 0; i < lens[k] / 4; i++)
            {
                if (bcm_video_rd(base + 4 * i, &got) < 0)
                    break;
                if (got != buf[i])
                {
                    if (!bad++)
                        first = i;
                }
            }
            if (f == 2 && lens[k] > 0x8000)
                continue;
            snprintf(line, sizeof(line), "dma lab: %s, %5lu bytes in "
                     "%lu us: %lu of %lu words wrong, first at word %ld",
                     f == 3 ? "engine 2, SDRAM" : f == 2 ? "SRAM" :
                     f ? "SDRAM fixup" : "SDRAM",
                     (unsigned long)lens[k],
                     (unsigned long)t0, (unsigned long)bad,
                     (unsigned long)(lens[k] / 4),
                     first == ~0u ? -1L : (long)first);
            log(line);
        }
    }
    bcm_video_set_dma_fixup(true);
    bcm_video_set_dma(false);           /* the CPU's copy again (D1) */
    bcm_video_off();
    bcm_video_release();
    core_unpin(h);
    core_free(h);
    free_all();
    return rc == HP_OK ? 0 : rc;
}

#ifndef SIMULATOR
/* ---- bench row B13: the warm swap ---------------------------------------

   warm-swap.md section 2, from the host's side.  Words in the chip's
   mailbox (bcmfw/config.h): the ready word, the info word and two of its
   bits, the cache control */
#define WARM_READY      0x1F8       /* BCMFW_MB_COMMAND */
#define WARM_INFO       0x1E8       /* BCMFW_MB_INFO */
#define WARM_INFO_ON    0x0100      /* BCMFW_INFO_PANEL_ON */
#define WARM_INFO_WARM  0x0800      /* BCMFW_INFO_WARM */
#define WARM_CACHE      0x10000C00
#define WARM_PIECE      32768
#define WARM_PARK_US    60000       /* PARK's reply -> the stub's word:
                                       the image first waits out the LCD
                                       DMA's frame (lcd_blit_wait, 14.26 ms
                                       a frame on silicon), then copies and
                                       runs the stub; 10 ms timed out with
                                       the panel on (bench 2026-10-25, B13:
                                       PARK st=0 f=3, the word still 0 at
                                       10.1 ms, builds A and probe) */
#define WARM_READY_US   100000      /* go -> the ready word */
#define WARM_CALL_US    50000       /* a reply (bcmvp.c's T_SHORT) */
/* why a swap failed (vplayer_warm_result.first_why) */
enum { WARM_E_PARK = 1, WARM_E_PARKED, WARM_E_UPLOAD, WARM_E_CACHE,
       WARM_E_READY, WARM_E_WORD, WARM_E_INFO, WARM_E_ATTACH, WARM_E_USE };
/* the last PARK's own record, for a failed round's line (bench
   2026-10-25: B13 "never parked", nothing more): PARK's status, its reply
   words and flags, the warm word last read (or the read's -1), and the
   microseconds waited */
static struct { int st; uint32_t n, f, w, us; } warm_park;
static uint32_t warm_park_prog;     /* HP_WARM_PROG read once parked */
/* the host's flush of the round (warm_flush): the cache word before it
   and after it, its re-requests << 12 | polls, its microseconds */
static struct { uint32_t before, after, rp, us; } warm_fl;
/* the upload checked against the image (warm_check): 1 KB pieces whose
   first or last word read back wrong straight after the upload, and
   again after the host's flush, and the pieces written again */
static struct { uint32_t up_bad, fl_bad, rewrites, passes; } warm_ck;

/* B13's diagnostics after a PARK (bench 2026-10-25: three builds answered
   PARK and never wrote "PRK", and nothing more was known): what the chip
   holds where the park leaves its traces.  The park's last step (the
   image's HP_WARM_PROG word: 'W' << 24 | step << 16 | detail; images
   without it leave another probe's word or noise there), the protocol
   heartbeat twice 5 ms apart (moving: the image's loop still runs), the
   ready and fault words and the last bounded wait, the stub's first four
   words at its copy (mov r1, #0x13C = 0x013C2E61 first), the cache
   control word, and the LCD DMA channel's state and status words.  A read
   that fails shows as ffffffff */
#define WARM_DIAG_FAULT_PC  0x1E0       /* BCMFW_MB_FAULT_PC */
#define WARM_DIAG_LIMIT     0x180       /* BCMFW_MB_LIMIT */
#define WARM_DIAG_DMA_CS    0x10000880  /* DMA_CS(DMA_LCD_CHAN) */
#define WARM_DIAG_DMA_INT   0x10000884  /* DMA_INT(DMA_LCD_CHAN) */
static uint32_t warm_rd(uint32_t addr)
{
    uint32_t v;

    return bcm_video_rd(addr, &v) < 0 ? ~0u : v;
}

static void warm_diag(uint32_t round, void (*log)(const char *line))
{
    char line[200];
    uint32_t prog = warm_rd(HP_WARM_PROG), hb0 = warm_rd(HP_STATE), hb1;
    uint32_t t0 = bcm_video_now_us();

    while (bcm_video_now_us() - t0 < 5000)
        yield();
    hb1 = warm_rd(HP_STATE);
    snprintf(line, sizeof(line), "warm %lu: diag prog %08lx (step %lu) state "
             "%08lx then %08lx, ready %08lx, fault pc %08lx, limit %08lx, "
             "info %08lx", (unsigned long)round, (unsigned long)prog,
             (unsigned long)((prog >> 24) == HP_WARM_PROG_MAGIC ?
                             (prog >> 16) & 0xFF : 0),
             (unsigned long)hb0, (unsigned long)hb1,
             (unsigned long)warm_rd(WARM_READY),
             (unsigned long)warm_rd(WARM_DIAG_FAULT_PC),
             (unsigned long)warm_rd(WARM_DIAG_LIMIT),
             (unsigned long)warm_rd(WARM_INFO));
    log(line);
    snprintf(line, sizeof(line), "warm %lu: diag stub %08lx %08lx %08lx "
             "%08lx, warm word %08lx, cache %08lx, lcd dma cs %08lx int %08lx",
             (unsigned long)round,
             (unsigned long)warm_rd(HP_WARM_STUB_ADDR),
             (unsigned long)warm_rd(HP_WARM_STUB_ADDR + 4),
             (unsigned long)warm_rd(HP_WARM_STUB_ADDR + 8),
             (unsigned long)warm_rd(HP_WARM_STUB_ADDR + 12),
             (unsigned long)warm_rd(HP_WARM_WORD),
             (unsigned long)warm_rd(WARM_CACHE),
             (unsigned long)warm_rd(WARM_DIAG_DMA_CS),
             (unsigned long)warm_rd(WARM_DIAG_DMA_INT));
    log(line);
}

/* After a failure past the upload: the host's flush of the round and the
   upload read back at every 1 KB (and its last word), against the image:
   a wrong word says the upload, a right one with no ready says the start */
#define WARM_RB_STEP    1024
static void warm_diag_up(uint32_t round, const uint8_t *im, uint32_t bytes,
                         void (*log)(const char *line))
{
    char line[200];
    uint32_t a, v, want, bad = 0, n = 0, first = ~0u, got0 = 0, want0 = 0;
    uint32_t end = bytes & ~3u;     /* the images are whole words */

    for (a = 0; a + 4 <= end; a = a + WARM_RB_STEP + 4 > end && a != end - 4
                                  ? end - 4 : a + WARM_RB_STEP)
    {
        memcpy(&want, im + a, 4);
        if (bcm_video_rd(a, &v) < 0)
            v = ~want;
        n++;
        if (v != want)
        {
            if (!bad++)
            {
                first = a;
                got0 = v;
                want0 = want;
            }
        }
    }
    snprintf(line, sizeof(line), "warm %lu: diag flush word %08lx -> %08lx, "
             "re-requests %lu, polls %lu, %lu us; check pieces %lu/%lu wrong, "
             "%lu rewritten in %lu passes; upload read back %lu of %lu "
             "words wrong, first at %08lx (got %08lx, want %08lx)",
             (unsigned long)round, (unsigned long)warm_fl.before,
             (unsigned long)warm_fl.after, (unsigned long)(warm_fl.rp >> 12),
             (unsigned long)(warm_fl.rp & 0xFFF), (unsigned long)warm_fl.us,
             (unsigned long)warm_ck.up_bad, (unsigned long)warm_ck.fl_bad,
             (unsigned long)warm_ck.rewrites, (unsigned long)warm_ck.passes,
             (unsigned long)bad, (unsigned long)n, (unsigned long)first,
             (unsigned long)got0, (unsigned long)want0);
    log(line);
}

/* The host's flush before the go word, on a running core (the stub spins),
   in Apple's form for a running core (vmcs.bin @25a, boot-and-clocks.md
   section 2) where the cold start's (0xC0000000, wait, 0) had the core
   held in reset: the request (bit 30) over the word with the enables
   (bits 31, 30) cleared, so the cache is never switched on under the
   running stub; if bit 0 already reads 1 straight after the request (the
   park's own flush leaves it set, PW1: a "done" from before), the word
   back and the request again, at most 15 times as hp_code_flush; then bit
   0 polled (5 ms); then 0, the word every image starts with after a cold
   start.  0, or -1 on a port failure or a flush that never ended. */
#define WARM_FLUSH_US   5000
static int warm_flush(void)
{
    uint32_t keep, v, t0 = bcm_video_now_us(), t, k = 0;

    memset(&warm_fl, 0, sizeof(warm_fl));
    if (bcm_video_rd(WARM_CACHE, &v) < 0)
        return -1;
    warm_fl.before = v;
    keep = v & 0x3FFFFFFFu;
    for (t = 0; t != 15; t++)
    {
        if (bcm_video_wr(WARM_CACHE, keep | 0x40000000u) ||
            bcm_video_rd(WARM_CACHE, &v) < 0)
            return -1;
        if (!(v & 1))
            break;
        if (bcm_video_wr(WARM_CACHE, keep))
            return -1;
    }
    /* still "done" after every request: never a flush taken as made
       (@25a re-requests without a bound; the host gives up loudly) */
    warm_fl.rp = t << 12;
    if (t == 15)
        return -1;
    while (!(v & 1))
    {
        if (bcm_video_now_us() - t0 > WARM_FLUSH_US ||
            bcm_video_rd(WARM_CACHE, &v) < 0)
            return -1;
        k++;
    }
    if (bcm_video_wr(WARM_CACHE, 0))
        return -1;
    warm_fl.after = warm_rd(WARM_CACHE);
    warm_fl.rp = t << 12 | (k > 0xFFF ? 0xFFF : k);
    warm_fl.us = bcm_video_now_us() - t0;
    return 0;
}

/* The upload as the image has it, just before the go word (sitting 4c,
   round 0: the upload's last 48 KB read back 0 after the host's flush,
   from 0x28800 to the end, the flush word stuck at 1).  Either the
   port's writes never landed (open question #73: posted writes) or the
   flush wrote the old image's cached lines back over them.  So each 1 KB
   piece's first and last word is read back (the read also puts the
   port's writes behind it, #73's rule) and a piece that differs is
   written again, by the CPU, until every piece reads right or
   WARM_CK_PASSES passes; the counts go to the round's diag line.  bad:
   the pieces that read wrong on the first pass.  0, or -1 (a port
   failure or a piece that never read right). */
#define WARM_CK_PIECE   1024
#define WARM_CK_PASSES  4
static int warm_piece_bad(const uint8_t *im, uint32_t a, uint32_t n)
{
    uint32_t k, v, want, at;

    for (k = 0; k < 2; k++)
    {
        at = k ? a + ((n - 4) & ~3u) : a;
        memcpy(&want, im + at, 4);
        if (bcm_video_rd(at, &v) < 0)
            return -1;
        if (v != want)
            return 1;
    }
    return 0;
}

static int warm_check(const uint8_t *im, uint32_t bytes, bool rewrite,
                      uint32_t *bad)
{
    uint32_t a, n, pass, wrong = 0;
    int r;

    *bad = 0;
    for (pass = 0; pass < (rewrite ? WARM_CK_PASSES : 1); pass++)
    {
        wrong = 0;
        for (a = 0; a < bytes; a += n)
        {
            n = MIN(bytes - a, WARM_CK_PIECE);
            r = warm_piece_bad(im, a, n);
            if (r < 0)
                return -1;
            if (!r)
                continue;
            wrong++;
            if (rewrite &&
                (bcm_video_wr_block(a, (const uint16_t *)(im + a), n / 2) ||
                 bcm_video_flush()))
                return -1;
            if (rewrite)
                warm_ck.rewrites++;
        }
        if (!pass)
            *bad = wrong;
        if (rewrite)
            warm_ck.passes = pass + 1;
        if (!wrong)
            return 0;
    }
    return rewrite ? -1 : 0;
}

/* One swap into im: t[0] PARK -> parked, t[1] the upload, t[2] go ->
   ready, t[3] the whole; *flags the park's.  go false stops before the
   go word (the injected fault).  0, or WARM_E_*. */
static int warm_swap(struct bvp *b, const uint8_t *im, uint32_t bytes,
                     bool e2, bool flush, bool go, uint32_t t[4],
                     uint32_t *flags, uint32_t *info)
{
    uint32_t a = HP_PARK_MAGIC, f = 0, n = 0, w = 0, t0, t1, i;

    t0 = bcm_video_now_us();
    memset(&warm_park, 0, sizeof(warm_park));
    warm_park_prog = 0;
    warm_park.st = bvp_call(b, HP_OP_PARK, &a, 1, &f, 1, &n, WARM_CALL_US);
    warm_park.n = n;
    warm_park.f = f;
    if (warm_park.st != HP_OK || !n)
        return WARM_E_PARK;
    for (;;)
    {
        warm_park.us = bcm_video_now_us() - t0;
        if (bcm_video_rd(HP_WARM_WORD, &w) < 0)
        {
            warm_park.w = ~0u;
            return WARM_E_PARKED;
        }
        warm_park.w = w;
        if (HP_WARM_STATE(w) == HP_WARM_PARKED)
            break;
        if (warm_park.us > WARM_PARK_US)
            return WARM_E_PARKED;
        yield();
    }
    t[0] = bcm_video_now_us() - t0;
    *flags = w & 0xFF;
    warm_park_prog = warm_rd(HP_WARM_PROG);
    /* the image over the old one, as a cold start uploads it; the core
       spins in the stub above it meanwhile */
    if (e2)
        bcm_video_set_dma_e2(true);
    else
        bcm_video_set_dma(false);
    t1 = bcm_video_now_us();
    for (i = 0; i < bytes; i += n)
    {
        n = MIN(bytes - i, WARM_PIECE);
        if (bcm_video_wr_block(i, (const uint16_t *)(im + i), n / 2))
            break;
    }
    if (i < bytes || bcm_video_flush())
    {
        bcm_video_set_dma(false);
        return WARM_E_UPLOAD;
    }
    t[1] = bcm_video_now_us() - t1;
    bcm_video_set_dma(false);
    if (bcm_video_wr(WARM_READY, 0))
        return WARM_E_UPLOAD;
    memset(&warm_fl, 0, sizeof(warm_fl));
    memset(&warm_ck, 0, sizeof(warm_ck));
    /* as uploaded (counted only), then the flush, then made right */
    if (warm_check(im, bytes, false, &warm_ck.up_bad))
        return WARM_E_UPLOAD;
    if (flush && warm_flush())
        return WARM_E_CACHE;
    if (warm_check(im, bytes, true, &warm_ck.fl_bad))
        return WARM_E_UPLOAD;
    if (!go)
        return 0;
    t1 = bcm_video_now_us();
    if (bcm_video_wr(HP_WARM_WORD, HP_WARM_GO | *flags))
        return WARM_E_READY;
    for (;;)
    {
        if (bcm_video_rd(WARM_READY, &a) < 0)
            return WARM_E_READY;
        if (a)
            break;
        if (bcm_video_now_us() - t1 > WARM_READY_US)
            return WARM_E_READY;
        yield();
    }
    t[2] = bcm_video_now_us() - t1;
    t[3] = bcm_video_now_us() - t0;
    if (bcm_video_rd(HP_WARM_WORD, &w) < 0 || w != (HP_WARM_UP | *flags))
        return WARM_E_WORD;
    if (bcm_video_rd(WARM_INFO, info) < 0 || !(*info & WARM_INFO_WARM))
        return WARM_E_INFO;
    if (bvp_attach(b, &port))
        return WARM_E_ATTACH;
    return 0;
}

/* The new image in use: the video image opens and closes a session (its
   SDRAM set up after a warm start); the display-only image fills a
   square and shows it, through a fence.  The display rounds are the odd
   ones: the k-th (round >> 1) puts its square k places along the row,
   green and red in turn (round & 1 is 1 in every one of them) */
static int warm_use(struct bvp *b, bool video, uint32_t round)
{
    uint32_t a[5], r[3], n, k = round >> 1;
    int st;

    if (video)
    {
        if (bvp_hello(b) != HP_OK || bvp_open(b, 320, 240, 0) != HP_OK)
            return WARM_E_USE;
        return bvp_close(b, 0) == HP_OK ? 0 : WARM_E_USE;
    }
    a[0] = HP_MAJOR << 8 | HP_MINOR;
    st = bvp_call(b, HP_OP_HELLO, a, 1, r, 3, &n, WARM_CALL_US);
    if (st != HP_OK || n < 2 || !(r[1] & HP_CAP_JOBS_2D))
        return WARM_E_USE;
    a[0] = J_SURF_FRONT;
    a[1] = (8 + 24 * (k % 12)) | 200u << 16;
    a[2] = 16 | 16u << 16;
    a[3] = k & 1 ? 0xF800 : 0x07E0;
    if (bvp_send(b, J_OP_FILL, a, 4, NULL))
        return WARM_E_USE;
    a[3] = 0;
    if (bvp_send(b, J_OP_PRESENT, a, 4, NULL))
        return WARM_E_USE;
    a[0] = round + 1;
    st = bvp_call(b, J_OP_FENCE, a, 1, r, 1, &n, WARM_CALL_US);
    return st == HP_OK && n && r[0] == round + 1 ? 0 : WARM_E_USE;
}

int vplayer_warm_lab(const uint8_t *disp, uint32_t disp_bytes,
                     void (*log)(const char *line),
                     struct vplayer_warm_result *r)
{
    static const char *const why[] = { "ok", "PARK refused", "never parked",
        "upload", "cache flush", "never ready", "warm word", "info word",
        "attach", "use after the swap" };
    struct bvp *bp;
    uint32_t round, t[4], flags = 0, info = 0, t0, v;
    char line[160];
    int rc, e;

    memset(r, 0, sizeof(*r));
    r->first_fail = -1;
    if (thread_id || B)
        return -1;
    if (!disp || !load_image(false))
        return -2;
    bp = probe_bvp();
    if (!bp)
    {
        free_all();
        return VPLAYER_E_NOMEM;
    }
    if (bcm_video_take() < 0)
    {
        free_all();
        return -3;
    }
    rc = bcm_video_boot(disp, disp_bytes);      /* cold, panel on */
    if (rc == 0)
        rc = bvp_attach(bp, &port);
    if (rc == 0 && !(bp->caps & HP_CAP_PARK))
        rc = -4;
    snprintf(line, sizeof(line), "warm: display-only image %lu bytes, video "
             "image %lu bytes, cold boot %s", (unsigned long)disp_bytes,
             (unsigned long)img_bytes, rc ? "FAILED" : "ok");
    log(line);
    for (round = 0; rc == 0 && round < VPLAYER_WARM_ROUNDS; round++)
    {
        bool video = !(round & 1);              /* in, then back out */
        bool e2 = round % 4 >= 2;               /* the upload's method */
        bool flush = round < VPLAYER_WARM_ROUNDS - 4;

        memset(t, 0, sizeof(t));
        e = warm_swap(bp, video ? img : disp, video ? img_bytes : disp_bytes,
                      e2, flush, true, t, &flags, &info);
        if (!e)
            e = warm_use(bp, video, round);
        /* a swap that failed before its info read left the last round's
           in info (sitting 3's "never ready" showed the video image's) */
        if (e && e < WARM_E_INFO)
            info = warm_rd(WARM_INFO);
        r->rounds++;
        if (!(info & WARM_INFO_ON))
            r->panel_off++;
        snprintf(line, sizeof(line), "warm %lu: to %s by %s%s: %s; park %lu "
                 "us, upload %lu us, ready %lu us, all %lu us, flags %lx, "
                 "info %08lx", (unsigned long)round,
                 video ? "video" : "display", e2 ? "engine 2" : "CPU",
                 flush ? "" : ", no cache flush", e ? why[e] : "PASS",
                 (unsigned long)t[0], (unsigned long)t[1],
                 (unsigned long)t[2], (unsigned long)t[3],
                 (unsigned long)flags, (unsigned long)info);
        log(line);
        if (e == WARM_E_PARK || e == WARM_E_PARKED)
        {
            snprintf(line, sizeof(line), "warm %lu: PARK st=%d n=%lu f=%lx, "
                     "warm word %08lx after %lu us", (unsigned long)round,
                     warm_park.st, (unsigned long)warm_park.n,
                     (unsigned long)warm_park.f, (unsigned long)warm_park.w,
                     (unsigned long)warm_park.us);
            log(line);
        }
        else if (round < 2 && warm_park_prog)
        {
            /* the first park of each image: its last step and the flush
               (detail: retries << 12 | polls), read while it was parked;
               then the host's flush (re-requests << 12 | polls) */
            snprintf(line, sizeof(line), "warm %lu: diag parked, prog %08lx, "
                     "host flush %08lx -> %08lx, %lx; check %lu/%lu, "
                     "rewritten %lu", (unsigned long)round,
                     (unsigned long)warm_park_prog,
                     (unsigned long)warm_fl.before,
                     (unsigned long)warm_fl.after, (unsigned long)warm_fl.rp,
                     (unsigned long)warm_ck.up_bad,
                     (unsigned long)warm_ck.fl_bad,
                     (unsigned long)warm_ck.rewrites);
            log(line);
        }
        if (e)
        {
            /* every failure (sitting 3's round 9 "never ready" left no
               more than its line): the chip's words, the park's record
               for a park failure, and past the park the host's flush and
               the upload read back */
            if (e != WARM_E_PARK)
                warm_diag(round, log);
            if (e > WARM_E_PARKED)
            {
                snprintf(line, sizeof(line), "warm %lu: diag park %lu us, "
                         "prog %08lx", (unsigned long)round,
                         (unsigned long)warm_park.us,
                         (unsigned long)warm_park_prog);
                log(line);
                warm_diag_up(round, video ? img : disp,
                             video ? img_bytes : disp_bytes, log);
            }
            if (r->first_fail < 0)
            {
                r->first_fail = round;
                r->first_why = e;
            }
            break;                  /* the chip's state is unknown now */
        }
        r->ok++;
        if (t[0] > r->park_us_max)
            r->park_us_max = t[0];
        if (t[2] > r->ready_us_max)
            r->ready_us_max = t[2];
        if (video)
        {
            if (t[1] > r->up_us_max[e2])
                r->up_us_max[e2] = t[1];
            if (t[3] > r->swap_us_max[e2])
                r->swap_us_max[e2] = t[3];
        }
    }
    /* the injected fault: park and upload, no go word; nothing may start,
       and the host's fallback is a cold start */
    if (rc == 0 && r->first_fail < 0)
    {
        e = warm_swap(bp, disp, disp_bytes, false, true, false, t, &flags,
                      &info);
        sleep(HZ / 10);             /* time enough for a stray start */
        if (!e && bcm_video_rd(WARM_READY, &v) == 0 && v)
            r->no_go_ready = 1;
        t0 = bcm_video_now_us();
        bcm_video_cut();
        e = e ? e : bcm_video_boot(disp, disp_bytes) ? WARM_E_READY : 0;
        r->cold_us = bcm_video_now_us() - t0;
        snprintf(line, sizeof(line), "warm fault: no go word, %s; cold "
                 "restart %s in %lu us", r->no_go_ready ?
                 "an image STARTED anyway" : "nothing started",
                 e ? "FAILED" : "ok", (unsigned long)r->cold_us);
        log(line);
        if (e || r->no_go_ready)
            r->first_fail = VPLAYER_WARM_ROUNDS;
    }
    snprintf(line, sizeof(line), "warm: %s (%lu of %lu swaps; park max %lu "
             "us, ready max %lu us, into video max %lu us CPU, %lu us engine "
             "2; panel off %lu)", rc == 0 && r->first_fail < 0 &&
             r->ok == VPLAYER_WARM_ROUNDS ? "PASS" : "FAIL",
             (unsigned long)r->ok, (unsigned long)r->rounds,
             (unsigned long)r->park_us_max, (unsigned long)r->ready_us_max,
             (unsigned long)r->swap_us_max[0], (unsigned long)r->swap_us_max[1],
             (unsigned long)r->panel_off);
    log(line);
    bcm_video_off();
    bcm_video_release();
    free_all();
    return rc;
}

/* ---- bench row B5: panel pushes from a slot at another pitch ---------- */

#define WIN_X       70              /* the window: 160 wide, odd first row */
#define WIN_W       160
#define WIN_PUSHES  10
#define WIN_HOLD_S  5               /* the picture left up to photograph */
#define WIN_DMA_US  0x1A4           /* bcmfw/config.h BCMFW_MB_DMA_US, */
#define WIN_DMA_DONE 0x1A8          /*   BCMFW_MB_DMA_DONE, */
#define WIN_DMA_TMO 0x1AC           /*   BCMFW_MB_DMA_TMO */

/* The picture: blue and dark red stripes by x + y, and in the window a
   white frame on its edge pixels and a white diagonal; a wrong pitch or
   start shears them, a lost row breaks the frame */
static uint16_t win_px(int x, int y, int wy, int wh)
{
    int u = x - WIN_X, v = y - wy, d;

    if (u >= 0 && u < WIN_W && v >= 0 && v < wh)
    {
        d = u * wh - v * WIN_W;
        if (u == 0 || u == WIN_W - 1 || v == 0 || v == wh - 1 ||
            (d > -WIN_W && d < WIN_W))
            return 0xFFFF;
    }
    return ((x + y) >> 3) & 1 ? 0x001F : 0x8000;
}

/* COMMIT's wait: twice its budget for the slot's bytes (jobs.h
   J_T_COMMIT, ms; a 300 x 300 drift slot's is 95 ms, so the 50 ms
   WARM_CALL_US could time a sound COMMIT out) */
static uint32_t commit_us(uint32_t bytes)
{
    return 2000u * J_T_COMMIT(bytes);
}

/* win_slot's steps (lab5 logs "step 5.k st=..."): the status is the
   reply's (HP_*, J_E_*), a BVP_E_* code, or the port call's -1 */
enum { WIN_S_ALLOC = 1, WIN_S_WRITE, WIN_S_FLUSH, WIN_S_COMMIT };

/* A w x h slot of cls holding the picture (its window at row wy, wh
   rows): ALLOC, the rows through the port, COMMIT with their F16w
   (jobs.h j_f16w, a row at a time as it is written).  0, or the step
   that failed (WIN_S_*) with its status in *st */
static int win_slot(struct bvp *b, uint32_t cls, int w, int h, int wy,
                    int wh, uint32_t *slot, int *st)
{
    uint32_t a[5], r[4], n = 0, addr, pitch, sa = 0, sb = 0;
    uint16_t row[400];
    int x, y;

    a[0] = cls;
    a[1] = w | h << 16;
    a[2] = J_FMT_RGB565;
    a[3] = 0xB5000000u | w << 12 | h;      /* a key of its own */
    a[4] = 0xB5;
    *st = bvp_call(b, J_OP_ALLOC, a, 5, r, 4, &n, WARM_CALL_US);
    if (*st != HP_OK || n < 3)
        return WIN_S_ALLOC;
    *slot = r[0];
    addr = r[1];
    pitch = r[2];
    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
            row[x] = win_px(x, y, wy, wh);
        j_f16w(&sa, &sb, row, w);
        if ((*st = bcm_video_wr_block(addr + y * pitch, row, w)))
            return WIN_S_WRITE;
    }
    if ((*st = bcm_video_flush()))
        return WIN_S_FLUSH;
    a[0] = *slot;
    a[1] = sa;
    a[2] = sb;
    *st = bvp_call(b, J_OP_COMMIT, a, 3, r, 2, &n, commit_us(h * pitch));
    return *st == HP_OK ? 0 : WIN_S_COMMIT;
}

/* PRESENT surf's window (WIN_X, wy) w x wh, waited for; *us from the
   chip's DMA counter (arm to end) */
static int win_push(struct bvp *b, uint32_t surf, int wy, int wh,
                    uint32_t *us)
{
    uint32_t a[4], r[2], n;

    a[0] = surf;
    a[1] = WIN_X | wy << 16;
    a[2] = WIN_W | wh << 16;
    a[3] = J_PRES_WAIT;
    if (bvp_call(b, J_OP_PRESENT, a, 4, r, 2, &n, WARM_CALL_US) != HP_OK)
        return -1;
    return bcm_video_rd(WIN_DMA_US, us) < 0 ? -1 : 0;
}

int vplayer_window_lab(const uint8_t *disp, uint32_t disp_bytes,
                       void (*log)(const char *line),
                       struct vplayer_window_result *r)
{
    /* [pitch 320 (the framebuffer), 300, 400]: first row, rows */
    static const int wy[3] = { 31, 31, 41 }, wh[3] = { 200, 200, 100 };
    static const int pitch[3] = { 320, 300, 400 };
    struct bvp *bp;
    uint32_t surf[3], us, d0 = 0, t0 = 0, a[4];
    char line[140];
    int k, i, rc;

    memset(r, 0, sizeof(*r));
    if (thread_id || B)
        return -1;
    if (!disp)
        return -2;
    bp = probe_bvp();
    if (!bp)
        return VPLAYER_E_NOMEM;
    if (bcm_video_take() < 0)
    {
        probe_bvp_free();
        return -3;
    }
    rc = bcm_video_boot(disp, disp_bytes);
    if (rc == 0)
        rc = bvp_attach(bp, &port);
    bp->tag = 0x8000;               /* every job's tag asks for its reply
                                       (J_TAG_REPLY) */
    surf[0] = J_SURF_FRONT;
    if (rc == 0)
    {
        /* the display-only image's vector checks (J_VPU: 7 expected, it
           decodes), for the bench session's record beside the merged
           image's (bench-p19.md) */
        uint32_t blk = 0, vpu = 0;

        if (bcm_video_rd(HP_JOBS, &blk) == 0 && blk && !(blk & 3) &&
            bcm_video_rd(blk + J_VPU, &vpu) == 0)
        {
            snprintf(line, sizeof(line), "lab5: J_VPU %lx (display-only "
                     "image)", (unsigned long)vpu);
            log(line);
        }
    }
    if (rc == 0)
    {
        /* the framebuffer's own window, as the slots' */
        a[0] = J_SURF_FRONT;
        a[1] = 0;
        a[2] = 320 | 240u << 16;
        a[3] = 0;
        rc = bvp_send(bp, J_OP_FILL, a, 4, NULL);
    }
    if (rc == 0)
    {
        int st = 0, step = win_slot(bp, J_CLS_DRIFT, 300, 300, wy[1], wh[1],
                                    &surf[1], &st);
        bool strip = false;

        if (!step)
        {
            strip = true;
            step = win_slot(bp, J_CLS_STRIP, 400, 160, wy[2], wh[2],
                            &surf[2], &st);
        }
        if (step)
        {
            static const char *const what[] = { "", "alloc", "write",
                                                "flush", "commit" };

            snprintf(line, sizeof(line), "lab5: step 5.%d st=%d (%s, %s)",
                     step, st, what[step], strip ? "400x160" : "300x300");
            log(line);
            rc = -5;
        }
    }
    if (rc == 0 && (bcm_video_rd(WIN_DMA_DONE, &d0) < 0 ||
                    bcm_video_rd(WIN_DMA_TMO, &t0) < 0))
        rc = -6;
    for (k = 0; rc == 0 && k < 3; k++)
    {
        r->px[k] = WIN_W * wh[k];
        r->us_min[k] = ~0u;
        for (i = 0; i < WIN_PUSHES; i++)
        {
            if (win_push(bp, surf[k], wy[k], wh[k], &us))
            {
                rc = -7;
                r->fail_step = 10 + k;
                break;
            }
            if (us < r->us_min[k])
                r->us_min[k] = us;
            if (us > r->us_max[k])
                r->us_max[k] = us;
        }
        snprintf(line, sizeof(line), "lab5 pitch %d: %lu px from row %d, "
                 "%lu-%lu us, %lu ns/px", pitch[k], (unsigned long)r->px[k],
                 wy[k], (unsigned long)r->us_min[k],
                 (unsigned long)r->us_max[k], r->px[k] ?
                 (unsigned long)(1000u * r->us_min[k] / r->px[k]) : 0ul);
        log(line);
    }
    if (rc == 0 && win_push(bp, surf[1], wy[1], wh[1], &us) == 0)
    {
        uint32_t d1 = 0, t1 = 0;

        bcm_video_rd(WIN_DMA_DONE, &d1);
        bcm_video_rd(WIN_DMA_TMO, &t1);
        r->dma_done = d1 - d0;
        r->dma_tmo = t1 - t0;
        snprintf(line, sizeof(line), "lab5: %s (%lu pushes by DMA of %d, %lu "
                 "timed out); the pitch-300 window is up for the photograph",
                 r->dma_tmo || r->dma_done != 3 * WIN_PUSHES + 1 ? "FAIL" :
                 "PASS", (unsigned long)r->dma_done, 3 * WIN_PUSHES + 1,
                 (unsigned long)r->dma_tmo);
        log(line);
        sleep(WIN_HOLD_S * HZ);
    }
    else
    {
        if (!r->fail_step)
            r->fail_step = -rc;
        snprintf(line, sizeof(line), "lab5: FAIL at step %d (%d)",
                 r->fail_step, rc);
        log(line);
    }
    bcm_video_off();
    bcm_video_release();
    probe_bvp_free();
    return rc;
}

/* ---- bench row CR1: COMMIT readback (open question #67) ----------------
   Bench 2026-10-25: COMMIT failed on both images and both write paths,
   status unknown.  On the display-only image, as lab5 boots it: a short
   pattern at each pool's base (and the drift pool's middle and end), the
   scratch pool and a control in a region proven host-writable after the
   start (memory-access.md section 8: the host frame), each read back by
   the port and restored; Apple's write forms at the first pool address
   that reads wrong; then slots as the drift and B5 use them, written,
   read back pixel by pixel, COMMITted, by the CPU and by engine 2. */

#define CR_WORDS    64              /* a region's pattern, words */
#define CR_CONTROL  0x105700        /* the host frame's last 256 bytes
                                       (0xE0000 + 320 x 240 x 2 - 256) */
#define CR_ROW_MAX  400             /* the widest slot's row */
#define CR_SHOW     4               /* mismatches logged a case */

/* The display-only image's pools (bcmfw/config.h BCMFW_JOBS_DO_*,
   BCMFW_JOBS_SCRATCH) */
static const struct
{
    uint32_t a;
    const char *what;
} cr_regions[] =
{
    { 0x046000, "drift" },          /* 364,800 B to 0x9F100 */
    { 0x072880, "drift middle" },
    { 0x09F000, "drift end" },      /* its last 256 bytes */
    { 0x09F200, "strip" },
    { 0x0BF200, "app" },
    { 0x12B000, "thumb" },
    { 0x0D4000, "scratch" },
};

/* Apple's write forms (bcm_video_wr_variant), alone and together */
static const struct
{
    unsigned how;
    const char *name;
} cr_variants[] =
{
    { BCM_WRV_ADDR16, "addr16" },
    { BCM_WRV_DATA16, "data16" },
    { BCM_WRV_NOPOLL, "nopoll" },
    { BCM_WRV_ADDR16 | BCM_WRV_DATA16 | BCM_WRV_NOPOLL, "all" },
};

/* The slots: the drift's and B5's sizes, an odd width, a single pixel
   pair */
static const struct
{
    uint32_t cls;
    int w, h;
    const char *what;
} cr_cases[] =
{
    { J_CLS_DRIFT, 300, 300, "drift" },
    { J_CLS_STRIP, 400, 160, "strip" },
    { J_CLS_STRIP, 301, 7, "strip" },
    { J_CLS_STRIP, 8, 1, "strip" },
};

static uint16_t cr_px(uint32_t seed, uint32_t i)
{
    return (uint16_t)(((i + seed) * 2654435761u) >> 13);
}

/* CR_WORDS words of seed's pattern at a, by our path (how < 0) or one
   of Apple's forms, read back by the port, then the old words put back.
   The wrong words, or -1: the port gave up */
static int cr_region(uint32_t a, uint32_t seed, int how, uint32_t *got,
                     uint32_t *want)
{
    uint32_t old[CR_WORDS], px32[CR_WORDS], v, i;
    uint16_t *px = (uint16_t *)px32;
    int bad = 0;

    for (i = 0; i < CR_WORDS; i++)
        if (bcm_video_rd(a + 4 * i, &old[i]) < 0)
            return -1;
    for (i = 0; i < 2 * CR_WORDS; i++)
        px[i] = cr_px(seed, i);
    if ((how < 0 ? bcm_video_wr_block(a, px, 2 * CR_WORDS)
                 : bcm_video_wr_variant(a, px, 2 * CR_WORDS, how)) ||
        bcm_video_flush())
        bad = -1;
    for (i = 0; bad >= 0 && i < CR_WORDS; i++)
    {
        if (bcm_video_rd(a + 4 * i, &v) < 0)
            bad = -1;
        else if (v != px32[i] && !bad++)
        {
            *got = v;
            *want = px32[i];
        }
    }
    if (bcm_video_wr_block(a, (const uint16_t *)old, 2 * CR_WORDS) ||
        bcm_video_flush())
        bad = -1;
    return bad;
}

/* One row of w pixels at a (a halfword address), as bcmjob-core.c's
   write_row: the port writes whole words at the address rounded down, so
   an odd first or last pixel is merged into its word (read, modify,
   write).  CR1 [silicon] 2026-10-25: a 301-wide slot (pitch 602) written
   as a block from row 1's mid-word start landed one pixel early */
static int cr_write_row(uint32_t a, const uint16_t *s, uint32_t w)
{
    uint32_t v, k = 0, n;

    if (a & 2)
    {
        if (bcm_video_rd(a - 2, &v) < 0 ||
            bcm_video_wr(a - 2, (v & 0xFFFF) | (uint32_t)s[0] << 16))
            return -1;
        k = 1;
        a += 2;
    }
    n = (w - k) & ~1u;
    if (n && bcm_video_wr_block(a, s + k, n))
        return -1;
    k += n;
    a += 2 * n;
    if (k < w && (bcm_video_rd(a, &v) < 0 ||
                  bcm_video_wr(a, (v & 0xFFFF0000u) | s[k])))
        return -1;
    return 0;
}

/* One slot case: ALLOC, write (rows as bcmjob_upload's row path,
   cr_write_row), flush, read every
   pixel back, COMMIT; on J_E_SUM a COMMIT without the check for the
   BCM's own sums.  e2: the blocks by engine 2 (rows under 256 bytes go
   by the CPU, as the port's rule has it).  Returns VPLAYER_CR_* */
static int cr_case(struct bvp *b, int k, bool e2, uint16_t *rows,
                   void (*log)(const char *line))
{
    const int w = cr_cases[k].w, h = cr_cases[k].h;
    const uint32_t seed = 0xC1000000u + (e2 ? 0x10000u : 0) + k * 0x1000u;
    const char *by = e2 ? "E2" : "CPU";
    uint32_t a[5], r[4], n = 0, slot = ~0u, addr, pitch, sa = 0, sb = 0;
    uint32_t ra = 0, rb = 0, bad = 0, bad_w = 0, wa = ~0u, bw = ~0u, word = 0;
    char line[140];
    int x, y, st, step = 0, v = VPLAYER_CR_PASS;

    a[0] = cr_cases[k].cls;
    a[1] = w | h << 16;
    a[2] = J_FMT_RGB565;
    a[3] = seed | w << 4 | h;           /* a key of its own */
    a[4] = 0xC1;
    st = bvp_call(b, J_OP_ALLOC, a, 5, r, 4, &n, WARM_CALL_US);
    if (st != HP_OK || n < 3)
    {
        step = 1;
        goto out;
    }
    slot = r[0];
    addr = r[1];
    pitch = r[2];
    snprintf(line, sizeof(line), "cr1 %s %dx%d %s: slot %lu at %06lx pitch "
             "%lu", cr_cases[k].what, w, h, by, (unsigned long)slot,
             (unsigned long)addr, (unsigned long)pitch);
    log(line);
    if (e2)
        bcm_video_set_dma_e2(true);
    else
        bcm_video_set_dma(false);
    for (y = 0; y < h && !step; y++)
    {
        /* two row buffers: an engine-2 block still reads the other */
        uint16_t *row = rows + (y & 1) * CR_ROW_MAX;

        for (x = 0; x < w; x++)
            row[x] = cr_px(seed, y * w + x);
        j_f16w(&sa, &sb, row, w);
        if ((st = cr_write_row(addr + y * pitch, row, w)))
            step = 2;
    }
    if ((st = bcm_video_flush()) && !step)
        step = 3;
    bcm_video_set_dma(false);
    if (step)
        goto out;
    /* every pixel back, a word read once for its two */
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            uint32_t p = addr + y * pitch + 2 * x, want = cr_px(seed, y * w + x);
            uint32_t got;

            if ((p & ~3u) != wa)
            {
                wa = p & ~3u;
                if ((st = bcm_video_rd(wa, &word)) < 0)
                {
                    step = 4;
                    goto out;
                }
            }
            got = p & 2 ? word >> 16 : word & 0xFFFF;
            ra += got;
            rb += ra;
            if (got == want)
                continue;
            if (bad++ < CR_SHOW)
            {
                snprintf(line, sizeof(line), "cr1 bad @%06lx got %04lx want "
                         "%04lx (x %d, y %d)", (unsigned long)p,
                         (unsigned long)got, (unsigned long)want, x, y);
                log(line);
            }
            if (wa != bw)
            {
                bad_w++;            /* a word counted once */
                bw = wa;
            }
        }
    a[0] = slot;
    a[1] = sa;
    a[2] = sb;
    n = 0;
    r[0] = r[1] = 0;
    st = bvp_call(b, J_OP_COMMIT, a, 3, r, 2, &n, commit_us(h * pitch));
    snprintf(line, sizeof(line), "cr1 %s %dx%d %s: bad px %lu (words %lu); "
             "commit st=%d n=%lu r=%lx %lx, ours %lx %lx", cr_cases[k].what,
             w, h, by, (unsigned long)bad, (unsigned long)bad_w, st,
             (unsigned long)n, (unsigned long)r[0], (unsigned long)r[1],
             (unsigned long)sa, (unsigned long)sb);
    log(line);
    if (st == J_E_SUM)
    {
        int st2;

        a[3] = J_COMMIT_NOCHECK;
        n = 0;
        r[0] = r[1] = 0;
        st2 = bvp_call(b, J_OP_COMMIT, a, 4, r, 2, &n, commit_us(h * pitch));
        snprintf(line, sizeof(line), "cr1 %s %dx%d %s: unchecked st=%d, the "
                 "BCM's sums %lx %lx, readback's %lx %lx", cr_cases[k].what,
                 w, h, by, st2, (unsigned long)r[0], (unsigned long)r[1],
                 (unsigned long)ra, (unsigned long)rb);
        log(line);
    }
    a[0] = slot;
    bvp_send(b, J_OP_FREE, a, 1, NULL);
    v = bad ? VPLAYER_CR_WRITE : st == HP_OK ? VPLAYER_CR_PASS :
        st == J_E_SUM ? VPLAYER_CR_SUM : VPLAYER_CR_OTHER;
out:
    if (step)
    {
        static const char *const what[] = { "", "alloc", "write", "flush",
                                            "readback" };

        snprintf(line, sizeof(line), "cr1 %s %dx%d %s: FAIL at %s st=%d",
                 cr_cases[k].what, w, h, by, what[step], st);
        bcm_video_set_dma(false);
        if (slot != ~0u)
        {
            a[0] = slot;
            bvp_send(b, J_OP_FREE, a, 1, NULL);
        }
        log(line);
        return VPLAYER_CR_OTHER;
    }
    snprintf(line, sizeof(line), "cr1 %s %dx%d %s: %s", cr_cases[k].what, w,
             h, by, v == VPLAYER_CR_PASS ? "PASS" : v == VPLAYER_CR_WRITE ?
             "FAIL (write path: readback wrong)" : v == VPLAYER_CR_SUM ?
             "FAIL (BCM-side sum: readback right, J_E_SUM)" :
             "FAIL (COMMIT refused, readback right)");
    log(line);
    return v;
}

int vplayer_commit_lab(const uint8_t *disp, uint32_t disp_bytes,
                       void (*log)(const char *line),
                       struct vplayer_commit_result *r)
{
    struct bvp *bp;
    uint32_t got = 0, want = 0, at;
    char line[160];
    int rc, k, e2, bad, rows_h = 0;
    unsigned i;

    memset(r, 0, sizeof(*r));
    r->verdict = VPLAYER_CR_NOTRUN;
    if (thread_id || B)
        return -1;
    if (!disp)
        return -2;
    bp = probe_bvp();
    if (!bp)
        return VPLAYER_E_NOMEM;
    rows_h = core_alloc(2 * CR_ROW_MAX * sizeof(uint16_t));
    if (rows_h <= 0)
    {
        probe_bvp_free();
        return VPLAYER_E_NOMEM;
    }
    core_pin(rows_h);
    if (bcm_video_take() < 0)
    {
        rc = -3;
        goto done;
    }
    rc = bcm_video_boot(disp, disp_bytes);
    if (rc == 0)
        rc = bvp_attach(bp, &port);
    bp->tag = 0x8000;               /* every job replies (J_TAG_REPLY) */
    bcm_video_set_dma(false);
    if (rc)
    {
        snprintf(line, sizeof(line), "cr1: FAIL: the display-only image did "
                 "not start (%d)", rc);
        log(line);
        goto off;
    }
    /* 1. the regions: the control first, then the pools */
    r->control_bad = cr_region(CR_CONTROL, 0xC0, -1, &got, &want);
    snprintf(line, sizeof(line), "cr1 region 0x%06lx wrong=%d/%d (control, "
             "host frame)", (unsigned long)CR_CONTROL, r->control_bad,
             CR_WORDS);
    if (r->control_bad > 0)
        snprintf(line + strlen(line), sizeof(line) - strlen(line),
                 " first got %08lx want %08lx", (unsigned long)got,
                 (unsigned long)want);
    log(line);
    at = cr_regions[0].a;           /* the variants' address */
    for (i = 0; i < ARRAYLEN(cr_regions); i++)
    {
        bad = cr_region(cr_regions[i].a, 0xC1 + i, -1, &got, &want);
        r->regions++;
        if (bad)
        {
            if (!r->regions_bad)
                at = cr_regions[i].a;
            r->regions_bad++;
        }
        snprintf(line, sizeof(line), "cr1 region 0x%06lx wrong=%d/%d (%s)",
                 (unsigned long)cr_regions[i].a, bad, CR_WORDS,
                 cr_regions[i].what);
        if (bad > 0)
            snprintf(line + strlen(line), sizeof(line) - strlen(line),
                     " first got %08lx want %08lx", (unsigned long)got,
                     (unsigned long)want);
        log(line);
    }
    /* 2. Apple's forms at the first pool address that read wrong (the
       drift pool's base if none did); ours there again for the record */
    bad = cr_region(at, 0xD0, -1, &got, &want);
    snprintf(line, sizeof(line), "cr1 variant ours at 0x%06lx wrong=%d/%d",
             (unsigned long)at, bad, CR_WORDS);
    log(line);
    for (i = 0; i < ARRAYLEN(cr_variants); i++)
    {
        int vb = cr_region(at, 0xD1 + i, cr_variants[i].how, &got, &want);

        if (vb == 0 && bad != 0)
            r->variants_ok |= 1u << i;
        snprintf(line, sizeof(line), "cr1 variant %s at 0x%06lx wrong=%d/%d",
                 cr_variants[i].name, (unsigned long)at, vb, CR_WORDS);
        log(line);
    }
    /* 3. the slots, by the CPU, then by engine 2 */
    for (e2 = 0; e2 < 2; e2++)
        for (k = 0; k < (int)ARRAYLEN(cr_cases); k++)
        {
            int v = cr_case(bp, k, e2, core_get_data(rows_h), log);

            r->cases++;
            if (v == VPLAYER_CR_PASS)
                r->pass++;
            else if (v == VPLAYER_CR_WRITE)
                r->write_bad++;
            else if (v == VPLAYER_CR_SUM)
                r->sum_bad++;
            else
                r->other++;
        }
    /* the verdict: the control wrong too is the write path itself; a
       pool wrong with the control right is SRAM the host can't write
       after the start; all readbacks right but J_E_SUM is the BCM's sum */
    r->verdict = r->control_bad != 0 ? VPLAYER_CR_WRITE
               : r->regions_bad ? VPLAYER_CR_REGION
               : r->write_bad ? VPLAYER_CR_WRITE
               : r->sum_bad ? VPLAYER_CR_SUM
               : r->other ? VPLAYER_CR_OTHER : VPLAYER_CR_PASS;
    if (r->variants_ok)
    {
        int n = snprintf(line, sizeof(line), "cr1: variants landing where "
                         "ours doesn't:");

        for (i = 0; i < ARRAYLEN(cr_variants); i++)
            if (r->variants_ok & 1u << i)
                n += snprintf(line + n, sizeof(line) - n, " %s",
                              cr_variants[i].name);
        log(line);
    }
    snprintf(line, sizeof(line), "cr1: %s (control %d wrong, pools %lu of "
             "%lu wrong; slots %lu pass, %lu write, %lu sum, %lu other)",
             r->verdict == VPLAYER_CR_PASS ? "PASS" :
             r->verdict == VPLAYER_CR_REGION ?
             "FAIL: region not writable after start" :
             r->verdict == VPLAYER_CR_WRITE ? "FAIL: write path" :
             r->verdict == VPLAYER_CR_SUM ? "FAIL: BCM-side sum" :
             "FAIL: other", r->control_bad,
             (unsigned long)r->regions_bad, (unsigned long)r->regions,
             (unsigned long)r->pass, (unsigned long)r->write_bad,
             (unsigned long)r->sum_bad, (unsigned long)r->other);
    log(line);
off:
    bcm_video_off();
    bcm_video_release();
done:
    core_unpin(rows_h);
    core_free(rows_h);
    probe_bvp_free();
    return rc;
}

/* ---- bench rows B6 and B7: the memory lab (HP_OP_LAB) ------------------ */

#define LAB_CALL_US     3000000     /* the longest run, at 27 MHz */
#define LAB_COUNT       (1u << 18)
#define LAB_LONG        (1u << 20)  /* the load run the stream overlaps */
#define LAB_STREAM      153600      /* a full frame through the port */
#define LAB_PIECE       4096

static int lab_call(struct bvp *b, uint32_t kind, uint32_t base,
                    uint32_t log2, uint32_t count, uint32_t out[3])
{
    uint32_t a[5] = { HP_LAB_MAGIC, kind, base, log2, count }, n;

    return bvp_call(b, HP_OP_LAB, a, 5, out, 3, &n, LAB_CALL_US) == HP_OK &&
           n >= 3 ? 0 : -1;
}

static uint32_t lab_kbs(uint32_t bytes_per, uint32_t count, uint32_t us)
{
    return us ? (uint32_t)((uint64_t)bytes_per * count * 1000000u / 1024u /
                           us) : 0;
}

/* The long load run alone, then again while the host writes LAB_STREAM
   bytes into the same memory (host address dst); *pm the slow-down */
static int lab_contend(struct bvp *b, uint32_t base, uint32_t log2,
                       uint32_t dst, uint32_t *pm, uint32_t *stream_us,
                       uint32_t *alone_us, uint32_t *with_us)
{
    uint32_t a[5] = { HP_LAB_MAGIC, HP_LAB_SEQ32, base, log2, LAB_LONG };
    uint32_t out[3], n, t0, i;
    int tag = 0, st;

    if (lab_call(b, HP_LAB_SEQ32, base, log2, LAB_LONG, out))
        return -1;
    *alone_us = out[0];
    if (bvp_send(b, HP_OP_LAB, a, 5, &tag))
        return -1;
    t0 = bcm_video_now_us();
    for (i = 0; i < LAB_STREAM; i += LAB_PIECE)
        if (bcm_video_wr_block(dst + i, (const uint16_t *)(img + i),
                               LAB_PIECE / 2))
            return -1;
    if (bcm_video_flush())
        return -1;
    *stream_us = bcm_video_now_us() - t0;
    for (;;)
    {
        if (bvp_poll(b) < 0)
            return -1;
        st = bvp_take(b, tag, out, 3, &n);
        if (st != BVP_AGAIN)
            break;
        if (bcm_video_now_us() - t0 > LAB_CALL_US)
            return -1;
        yield();
    }
    if (st != HP_OK || n < 3)
        return -1;
    *with_us = out[0];
    *pm = out[0] > *alone_us ? (out[0] - *alone_us) * 1000u / *alone_us : 0;
    return 0;
}

int vplayer_mem_lab(int row, void (*log)(const char *line),
                    struct vplayer_mem_result *r)
{
    static const char *const mem_name[2] = { "SRAM", "SDRAM" };
    static const uint32_t base[2] = { HP_LAB_SRAM, HP_LAB_SDRAM };
    static const uint32_t log2[2] = { HP_LAB_SRAM_LOG2, HP_LAB_SDRAM_LOG2 };
    struct bvp *bp;
    uint32_t out[3], e[3], c, m, mhz, dst;
    char line[160];
    int rc;

    memset(r, 0, sizeof(*r));
    if (thread_id || B)
        return -1;
    if (!load_image(false))
        return -2;
    bp = probe_bvp();
    if (!bp)
    {
        free_all();
        return VPLAYER_E_NOMEM;
    }
    if (bcm_video_take() < 0)
    {
        free_all();
        return -3;
    }
    check_fast_clock(false);
    rc = bcm_video_boot(img, img_bytes);
    if (rc == 0)
        rc = bvp_attach(bp, &port);
    if (rc == 0)
        rc = bvp_hello(bp);
    for (c = 0; rc == HP_OK && c < (fast_flags ? 2u : 1u); c++)
    {
        rc = bvp_open(bp, 320, 240, c ? HP_OPEN_FAST_CLOCK : 0);
        if (rc != HP_OK)
        {
            r->fail_step = 1;
            break;
        }
        for (m = 0; m < 2; m++)
        {
            if (lab_call(bp, HP_LAB_EMPTY, base[m], log2[m], LAB_COUNT, e))
            {
                r->fail_step = 2;
                break;
            }
            mhz = e[2] ? 150 : 27;
            if (row == 6)
            {
                if (lab_call(bp, HP_LAB_RAND16, base[m], log2[m], LAB_COUNT,
                             out))
                {
                    r->fail_step = 3;
                    break;
                }
                r->fetch_ns10[c][m] = out[0] > e[0] ?
                    (uint32_t)((uint64_t)(out[0] - e[0]) * 10000u / LAB_COUNT)
                    : 0;
                snprintf(line, sizeof(line), "lab6 %lu MHz %s: arithmetic %lu "
                         "us, with fetches %lu us for %lu: %lu.%lu ns a fetch "
                         "(%lu cycles)", (unsigned long)mhz, mem_name[m],
                         (unsigned long)e[0], (unsigned long)out[0],
                         (unsigned long)LAB_COUNT,
                         (unsigned long)r->fetch_ns10[c][m] / 10,
                         (unsigned long)r->fetch_ns10[c][m] % 10,
                         (unsigned long)(r->fetch_ns10[c][m] * mhz / 10000));
                log(line);
                continue;
            }
            if (lab_call(bp, HP_LAB_SEQ32, base[m], log2[m], LAB_COUNT, out))
            {
                r->fail_step = 5;
                break;
            }
            r->load_kbs[c][m] = lab_kbs(4, LAB_COUNT, out[0]);
            if (lab_call(bp, HP_LAB_STORE32, base[m], log2[m], LAB_COUNT, out))
            {
                r->fail_step = 5;
                break;
            }
            r->store_kbs[c][m] = lab_kbs(4, LAB_COUNT, out[0]);
            if (lab_call(bp, HP_LAB_COPY32, base[m], log2[m], LAB_COUNT, out))
            {
                r->fail_step = 5;
                break;
            }
            r->copy_kbs[c][m] = lab_kbs(4, LAB_COUNT, out[0]);
            snprintf(line, sizeof(line), "lab7 %lu MHz %s: load %lu KB/s, "
                     "store %lu KB/s, copy %lu KB/s (each way)",
                     (unsigned long)mhz, mem_name[m],
                     (unsigned long)r->load_kbs[c][m],
                     (unsigned long)r->store_kbs[c][m],
                     (unsigned long)r->copy_kbs[c][m]);
            log(line);
            /* the host's frame into the same memory: the framebuffer in
               SRAM, the bitstream FIFO in SDRAM */
            dst = m ? bp->sess_fifo : 0xE0000;
            {
                uint32_t sus = 0, alone = 0, with = 0;

                if (lab_contend(bp, base[m], log2[m], dst, &r->slow_pm[c][m],
                                &sus, &alone, &with))
                {
                    r->fail_step = 4;
                    break;
                }
                snprintf(line, sizeof(line), "lab7 %lu MHz %s contention: "
                         "loads %lu us alone, %lu us under the host's %lu "
                         "bytes (%lu us): %lu.%lu %% slower",
                         (unsigned long)mhz, mem_name[m],
                         (unsigned long)alone, (unsigned long)with,
                         (unsigned long)LAB_STREAM, (unsigned long)sus,
                         (unsigned long)r->slow_pm[c][m] / 10,
                         (unsigned long)r->slow_pm[c][m] % 10);
                log(line);
            }
        }
        if (r->fail_step)
            break;
        r->clocks = c + 1;
        bvp_close(bp, 0);
    }
    if (rc != HP_OK && !r->fail_step)
        r->fail_step = 9;
    snprintf(line, sizeof(line), "lab%d: %s (%lu clock%s)", row,
             r->fail_step ? "FAIL" : "PASS", (unsigned long)r->clocks,
             r->clocks == 1 ? "" : "s");
    log(line);
    bcm_video_off();
    bcm_video_release();
    free_all();
    return r->fail_step ? -10 - r->fail_step : 0;
}

/* ---- The brief probes (brief-map-peripherals.md section 11, brief-map-
   power.md section 9): the probe image (VPLAYER_PROBE_IMAGE, the plain
   one: the merged image has no probe ops) booted, the caller's steps,
   then the power cut and the stock wake path (every stage's recovery) */
int vplayer_probe(int (*pre)(void *ctx),
                  int (*fn)(struct bvp *b, uint32_t fast, void *ctx),
                  void *ctx)
{
    struct bvp *bp;
    int rc;

    if (thread_id || B)
        return -1;
    if (!load_image_from(&probe_image, false))
        return file_exists(VPLAYER_PROBE_IMAGE) ? VPLAYER_E_NOMEM : -2;
    bp = probe_bvp();
    if (!bp)
    {
        free_all();
        return VPLAYER_E_NOMEM;
    }
    if (bcm_video_take() < 0)
    {
        free_all();
        return -3;
    }
    check_fast_clock(false);
    rc = pre ? pre(ctx) : 0;
    if (rc == 0)
        rc = bcm_video_boot(img, img_bytes);
    if (rc == 0)
        rc = bvp_attach(bp, &port);
    if (rc == 0)
        rc = bvp_hello(bp);
    if (rc == HP_OK)
        rc = fn(bp, fast_flags, ctx);
    else
        rc = rc < 0 ? rc : -100 - rc;
    bcm_video_cut();
    bcm_video_release();
    free_all();
    return rc;
}

int vplayer_peek(int (*pre)(void *ctx), void *ctx)
{
    int rc;

    if (thread_id || B)
        return -1;
    if (bcm_video_take() < 0)
        return -3;
    rc = pre(ctx);
    if (bcm_video_hand_back() < 0)
    {
        bcm_video_cut();
        bcm_video_release();
    }
    return rc;
}

#endif /* !SIMULATOR */

/* ---- stage 9 (M4): the AAC decoder on the chip -------------------------

   Where the test's bytes go in the chip's SDRAM: bcmfw video.h
   VIDEO_AAC_IO (16 KB), as bcmfw/tests/video_test.py's t_aac */
#define AAC_IO_ASC      0x39C000
#define AAC_AU_MAX      2036        /* bcmfw/aac/aac.h AAC_MAX_AU */
/* dev42: each AU and its PCM in a slot of their own, rotating through the
   audio rings' window (video.h VIDEO_AUDIO_*, 256 KB: 32 slots of 8 KB,
   the AU at +0, 4 KB of PCM at +4 KB), so no AU reads what the last one
   left (session 35: the same two addresses every AU, AU 0 refused, AU 1
   wrong) */
#define AAC_SLOTS_AT    0x340000
#define AAC_SLOT        0x2000
#define AAC_SLOTS       32
#define AAC_SLOT_PCM    0x1000
#define AAC_MEMPROBE    0x39FFF0    /* video.h VIDEO_AAC_MEMPROBE */
#define AAC_LOG_MAX     4           /* lines per kind of failure */
#define AAC_FILE_MAX    (512 * 1024)

static uint32_t rd_le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t fnv_bytes(uint32_t h, const uint8_t *p, uint32_t n)
{
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

/* aac_image.c's fnv_words over the image file's little-endian words */
static uint32_t fnv_le_words(const uint8_t *p, uint32_t words)
{
    uint32_t h = 2166136261u;

    for (; words; words--, p += 4)
        h = (h ^ rd_le32(p)) * 16777619u;
    return h;
}

/* a whole file into a new pinned block; its handle, or <= 0 */
static int aac_load(const char *path, uint32_t *bytes)
{
    int fd = open(path, O_RDONLY), h;
    off_t n;

    if (fd < 0)
        return 0;
    n = ffilesize(fd);
    if (n < 12 || n > AAC_FILE_MAX)
    {
        close(fd);
        return 0;
    }
    h = core_alloc(n);
    if (h <= 0)
    {
        close(fd);
        return 0;
    }
    core_pin(h);
    if (read(fd, core_get_data(h), n) != n)
    {
        close(fd);
        core_unpin(h);
        core_free(h);
        return 0;
    }
    close(fd);
    *bytes = n;
    return h;
}

/* bytes (a multiple of 4) to the chip's SDRAM at off, from a word-aligned
   buffer, by the CPU */
static int aac_put(const struct bvp *b, uint32_t off, const void *src,
                   uint32_t bytes)
{
    return port.wr_block(b->fifo_host + off, src, bytes / 2) < 0 ? BVP_E_PORT : 0;
}

/* An uploaded AU read back over the port: 0, or 1 + the first wrong
   word's index (its words in *got, *want) */
static uint32_t aac_verify(const struct bvp *b, uint32_t off,
                           const uint32_t *src, uint32_t bytes,
                           uint32_t *got, uint32_t *want)
{
    uint32_t i, w;

    for (i = 0; i < bytes / 4; i++)
    {
        if (port.rd(b->fifo_host + off + 4 * i, &w) < 0 || w != src[i])
        {
            *got = w;
            *want = src[i];
            return i + 1;
        }
    }
    return 0;
}

/* HP_AAC_MEMPROBE: byte and halfword stores in SRAM and SDRAM, what the
   core read back, and the SDRAM bytes as the host reads them */
static void aac_memprobe(struct bvp *b, void (*log)(const char *line),
                         struct vplayer_aac_result *res)
{
    static const uint32_t want[5] = { HP_MEMPROBE_W0, HP_MEMPROBE_W1,
        HP_MEMPROBE_W2, HP_MEMPROBE_W3, HP_MEMPROBE_W4 };
    uint32_t a[2] = { 0, HP_AAC_MEMPROBE }, r[10], n = 0, k, host[4];
    char line[200];
    int rc = bvp_call(b, HP_OP_AAC, a, 2, r, 10, &n, 50000);

    if (rc != HP_OK || n < 5)
    {
        snprintf(line, sizeof(line), "stage 9: memprobe: status %d, %lu words",
                 rc, (unsigned long)n);
        log(line);
        return;
    }
    res->mem_sram = !memcmp(r, want, sizeof(want));
    if (n >= 10)
        res->mem_sdram = !memcmp(r + 5, want, sizeof(want));
    for (k = 0; k < 4; k++)
        if (port.rd(b->fifo_host + AAC_MEMPROBE + 4 * k, &host[k]) < 0)
            host[k] = 0xDEADDEAD;
    snprintf(line, sizeof(line), "stage 9: memprobe SRAM %s: %08lx %08lx "
             "%08lx %08lx, bytes %08lx", res->mem_sram ? "ok" : "WRONG",
             (unsigned long)r[0], (unsigned long)r[1], (unsigned long)r[2],
             (unsigned long)r[3], (unsigned long)r[4]);
    log(line);
    if (n >= 10)
    {
        snprintf(line, sizeof(line), "stage 9: memprobe SDRAM %s: %08lx "
                 "%08lx %08lx %08lx, bytes %08lx; host reads %08lx %08lx "
                 "%08lx %08lx", res->mem_sdram ? "ok" : "WRONG",
                 (unsigned long)r[5], (unsigned long)r[6],
                 (unsigned long)r[7], (unsigned long)r[8],
                 (unsigned long)r[9], (unsigned long)host[0],
                 (unsigned long)host[1], (unsigned long)host[2],
                 (unsigned long)host[3]);
        log(line);
    }
}

/* HP_AAC_MEMSTRESS (session 36c): a store and at once a load of it, at
   every width, and the decoder's halfword copy and clear, in SRAM (the
   control) and SDRAM; the per-pattern counts go to the log */
static void aac_memstress(struct bvp *b, void (*log)(const char *line),
                          struct vplayer_aac_result *res)
{
    uint32_t a[2] = { 0, HP_AAC_MEMSTRESS }, r[19], n = 0, k;
    uint32_t sum[2] = { 0, 0 };
    char line[256];     /* 14 counts of up to 10 digits and the legend */
    int rc = bvp_call(b, HP_OP_AAC, a, 2, r, 19, &n, 2000000);

    if (rc != HP_OK || n < 19)
    {
        snprintf(line, sizeof(line), "stage 9: memstress: status %d, %lu "
                 "words", rc, (unsigned long)n);
        log(line);
        return;
    }
    for (k = 0; k < 7; k++)
    {
        sum[0] += r[k];
        sum[1] += r[7 + k];
    }
    res->stress_sram = (int32_t)sum[0];
    res->stress_sdram = r[18] ? (int32_t)sum[1] : -1;
    snprintf(line, sizeof(line), "stage 9: memstress SRAM %lu %lu %lu %lu "
             "%lu %lu %lu; SDRAM %lu %lu %lu %lu %lu %lu %lu (w b h bw hw "
             "copy clear)",
             (unsigned long)r[0], (unsigned long)r[1], (unsigned long)r[2],
             (unsigned long)r[3], (unsigned long)r[4], (unsigned long)r[5],
             (unsigned long)r[6], (unsigned long)r[7], (unsigned long)r[8],
             (unsigned long)r[9], (unsigned long)r[10], (unsigned long)r[11],
             (unsigned long)r[12], (unsigned long)r[13]);
    log(line);
    if (r[14])
    {
        snprintf(line, sizeof(line), "stage 9: memstress SDRAM first wrong: "
                 "pattern %lu, index %lu, got %08lx, want %08lx",
                 (unsigned long)(r[14] - 1), (unsigned long)r[15],
                 (unsigned long)r[16], (unsigned long)r[17]);
        log(line);
    }
}

/* HP_AAC_STATE after a DECODE (session 36c's dump-and-diff): the parse's
   checkpoints (bcmfw/aac/aac.h AAC_T_*), checked for the second
   ic_stream's copy and, when log_it, logged 8 pairs a line as
   bcmfw/aac/test/aac_dev_sim.py prints them (keep the two in step) */
#define AAC_TRACE_MAX   40          /* aac.h AAC_TRACE_PAIRS */
#define AAC_TRACED_MAX  6           /* AUs with their checkpoints logged */

static const char *aac_tag_name(uint32_t tag)
{
    static const char *const names[] = { "?", "ele", "cpe", "ics1", "ics2",
        "info", "gain", "sect", "sf", "sfh", "flags", "spec", "spech", "err",
        "t14", "drop" };

    return tag < ARRAYLEN(names) ? names[tag] : "t?";
}

static void aac_trace(struct bvp *b, uint32_t k, bool log_it,
                      void (*log)(const char *line),
                      struct vplayer_aac_result *res)
{
    uint32_t pr[AAC_TRACE_MAX][2], np = 0, total = 0, first, n, i, o[31];
    uint32_t ics1 = 0, ics2 = 0;
    bool have1 = false, have2 = false;
    char line[200];
    int len;

    for (first = 0; first < AAC_TRACE_MAX; first += 15)
    {
        uint32_t a[3] = { 0, HP_AAC_STATE, first };

        if (bvp_call(b, HP_OP_AAC, a, 3, o, 31, &n, 50000) != HP_OK || n < 1)
            break;
        total = o[0];
        for (i = 1; i + 1 < n && np < AAC_TRACE_MAX; i += 2, np++)
        {
            pr[np][0] = o[i];
            pr[np][1] = o[i + 1];
        }
        if (first + 15 >= total)
            break;
    }
    for (i = 0; i < np; i++)
    {
        if (pr[i][0] == 3 && !have1)        /* AAC_T_ICS1 */
        {
            ics1 = pr[i][1];
            have1 = true;
        }
        else if (pr[i][0] == 4 && !have2)   /* AAC_T_ICS2 */
        {
            ics2 = pr[i][1];
            have2 = true;
        }
    }
    if (have1 && have2)
    {
        res->copy_checked++;
        if (ics1 != ics2 && ++res->copy_bad <= AAC_LOG_MAX)
        {
            snprintf(line, sizeof(line), "stage 9: AU %lu: ic_stream copy "
                     "WRONG (first %08lx, copy %08lx)", (unsigned long)k,
                     (unsigned long)ics1, (unsigned long)ics2);
            log(line);
        }
    }
    if (!log_it)
        return;
    res->traced++;
    for (first = 0; first < np || first == 0; first += 8)
    {
        len = snprintf(line, sizeof(line), "stage 9: trace AU %lu [%lu of "
                       "%lu]:", (unsigned long)k, (unsigned long)first,
                       (unsigned long)total);
        for (i = first; i < np && i < first + 8 && len > 0 &&
             len < (int)sizeof(line); i++)
            len += snprintf(line + len, sizeof(line) - len, " %s=%08lx",
                            aac_tag_name(pr[i][0]), (unsigned long)pr[i][1]);
        log(line);
    }
}

int vplayer_aac_test(const char *au_path, const char *ref_path, bool fast,
                     uint32_t aac_flags, void (*log)(const char *line),
                     struct vplayer_aac_result *res)
{
    uint32_t au_bytes = 0, ref_bytes = 0, asc_len, pos, k, n, out[4];
    uint32_t *stage;                    /* an AU, word-aligned */
    const uint8_t *au, *ref;
    struct bvp *bp = NULL;
    char line[200];
    int ha, hr, hs = 0, rc;

    memset(res, 0, sizeof(*res));
    res->first_bad = -1;
    res->tables = res->mem_sram = res->mem_sdram = -1;
    res->stress_sram = res->stress_sdram = -1;
    if (thread_id || B)
        return -1;                      /* a video holds the BCM */
    ha = aac_load(au_path, &au_bytes);
    hr = ha > 0 ? aac_load(ref_path, &ref_bytes) : 0;
    hs = hr > 0 ? core_alloc(AAC_AU_MAX + 8) : 0;
    if (ha <= 0 || hr <= 0 || hs <= 0)
    {
        if (hr > 0)
        {
            core_unpin(hr);
            core_free(hr);
        }
        if (ha > 0)
        {
            core_unpin(ha);
            core_free(ha);
        }
        return ha <= 0 || hr <= 0 ? -5 : -4;
    }
    core_pin(hs);
    stage = (uint32_t *)ALIGN_UP((uintptr_t)core_get_data(hs), 4);
    au = core_get_data(ha);
    ref = core_get_data(hr);
    /* the files: bcmfw/aac/test/mkdevclip.sh (.au: 'AACU', ASC length,
       ASC, then length + bytes per AU; .ref: 'AACR', AUs, then frames and
       the FNV-1a of that AU's PCM per AU, then the running FNV-1a) */
    asc_len = rd_le32(au + 4);
    res->aus_ref = ref_bytes >= 12 && !memcmp(ref, "AACR", 4) ?
                   rd_le32(ref + 4) : 0;
    rc = 0;
    if (memcmp(au, "AACU", 4) || asc_len < 2 || asc_len > 64 ||
        8 + asc_len > au_bytes || !res->aus_ref ||
        ref_bytes != 12 + 8 * res->aus_ref)
        rc = -6;
    if (rc == 0 && !load_image(false))
        rc = -2;
    if (rc == 0 && !(bp = probe_bvp()))
        rc = VPLAYER_E_NOMEM;
    if (rc == 0 && bcm_video_take() < 0)
        rc = -3;
    if (rc)
    {
        core_unpin(hs);
        core_free(hs);
        core_unpin(hr);
        core_free(hr);
        core_unpin(ha);
        core_free(ha);
        free_all();
        return rc;
    }
    bcm_video_set_dma(false);           /* the CPU's copy (DMA: D1) */
    if (fast)
        check_fast_clock(false);
    /* a fresh boot every run (dev42): the tables are parked by crt0 and
       placed by this boot's first OPEN, never left from an earlier use */
    bcm_video_off();
    rc = bcm_video_boot(img, img_bytes);
    if (rc == 0)
        rc = bvp_attach(bp, &port);
    if (rc == 0)
        rc = bvp_hello(bp);
    if (rc == HP_OK && !(bp->caps & HP_CAP_AAC))
        rc = BVP_E_PROTO;
    if (rc == HP_OK)
    {
        /* the bcs/bcc probe: registers only, before anything else */
        out[0] = 0;
        out[1] = HP_AAC_PROBE;
        rc = bvp_call(bp, HP_OP_AAC, out, 2, out, 3, &n, 50000);
        if (rc == HP_OK && n >= 2)
        {
            res->probe = out[0];
            res->probe_want = out[1];
            res->probe_ran = true;
        }
    }
    if (rc == HP_OK)
        rc = bvp_open(bp, 320, 240, fast ? fast_flags : 0);   /* SDRAM up */
    if (rc == HP_OK)
        aac_memprobe(bp, log, res);
    if (rc == HP_OK)
        aac_memstress(bp, log, res);
    if (rc == HP_OK)
    {
        memcpy(stage, au + 8, asc_len);
        rc = aac_put(bp, AAC_IO_ASC, stage, (asc_len + 3) & ~3u);
    }
    if (rc == HP_OK)
    {
        uint32_t a[5] = { bp->session, HP_AAC_OPEN, AAC_IO_ASC, asc_len,
                          aac_flags | VPLAYER_AAC_TRACE };
        uint32_t o[7], file_fnv = 0;

        memset(o, 0, sizeof(o));
        rc = bvp_call(bp, HP_OP_AAC, a, 5, o, 7, &n, 200000);
        res->rate = n >= 1 ? o[0] : 0;
        res->channels = n >= 2 ? o[1] : 0;
        res->open_code = n >= 3 ? (int)(int32_t)o[2] : 0;
        /* the tables: the parked copy the first OPEN took, and SDRAM's
           copy now, against the image file's own bytes */
        if (n >= 7 && o[4] + o[5] <= img_bytes && !(o[4] & 3))
        {
            file_fnv = fnv_le_words(img + o[4], o[5] / 4);
            res->tables = o[3] == file_fnv && o[6] == file_fnv;
        }
        snprintf(line, sizeof(line), "stage 9: AAC OPEN status %d, code %d: "
                 "%lu Hz, %lu ch; tables %s (parked %08lx, SDRAM %08lx, file "
                 "%08lx; %lu bytes from %05lx)", rc, res->open_code,
                 (unsigned long)res->rate, (unsigned long)res->channels,
                 res->tables < 0 ? "not reported" :
                 res->tables ? "same" : "DIFFER", (unsigned long)o[3],
                 (unsigned long)o[6], (unsigned long)file_fnv,
                 (unsigned long)o[5], (unsigned long)o[4]);
        log(line);
    }
    res->hash = 2166136261u;
    pos = 8 + asc_len;
    for (k = 0; rc == HP_OK && k < res->aus_ref && pos + 4 <= au_bytes; k++)
    {
        uint32_t len = rd_le32(au + pos), want_n = rd_le32(ref + 8 + 8 * k);
        uint32_t want_h = rd_le32(ref + 12 + 8 * k), h = 2166136261u, i;
        uint32_t slot = AAC_SLOTS_AT + (k % AAC_SLOTS) * AAC_SLOT, bad, g, wv;
        uint32_t a[5] = { bp->session, HP_AAC_DECODE, slot, len,
                          slot + AAC_SLOT_PCM };

        if (len == 0 || len > AAC_AU_MAX || pos + 4 + len > au_bytes)
        {
            rc = -6;
            break;
        }
        memcpy(stage, au + pos + 4, len);
        pos += 4 + len;
        rc = aac_put(bp, slot, stage, (len + 3) & ~3u);
        /* what the chip will read: the AU as uploaded? */
        if (rc == HP_OK &&
            (bad = aac_verify(bp, slot, stage, (len + 3) & ~3u, &g, &wv)))
        {
            if (++res->au_bad <= AAC_LOG_MAX)
            {
                snprintf(line, sizeof(line), "stage 9: AU %lu reads back "
                         "wrong at word %lu of %lu: %08lx, wrote %08lx",
                         (unsigned long)k, (unsigned long)(bad - 1),
                         (unsigned long)((len + 3) / 4), (unsigned long)g,
                         (unsigned long)wv);
                log(line);
            }
        }
        if (rc == HP_OK)
            rc = bvp_call(bp, HP_OP_AAC, a, 5, out, 4, &n, 500000);
        if (rc == HP_E_CORRUPT && n >= 4)
        {
            /* the decoder refused this AU (libfaad's error: -100 - code);
               the next decodes, so go on and count them */
            if (++res->errors <= AAC_LOG_MAX)
            {
                snprintf(line, sizeof(line), "stage 9: AU %lu (%lu bytes): "
                         "DECODE refused, code %ld (libfaad error %ld)",
                         (unsigned long)k, (unsigned long)len,
                         (long)(int32_t)out[3],
                         -100L - (long)(int32_t)out[3]);
                log(line);
            }
            if (res->first_bad < 0)
                res->first_bad = k;
            res->aus++;
            aac_trace(bp, k, res->traced < AAC_TRACED_MAX, log, res);
            rc = HP_OK;
            continue;
        }
        if (rc != HP_OK || n < 3)
        {
            snprintf(line, sizeof(line), "stage 9: AU %lu: DECODE status %d, "
                     "code %ld", (unsigned long)k, rc,
                     n >= 4 ? (long)(int32_t)out[3] : 0L);
            log(line);
            if (rc == HP_OK)
                rc = BVP_E_PROTO;
            break;
        }
        res->aus++;
        res->mhz = out[2];
        if (out[0])
        {
            res->us_total += out[1];
            res->us_n++;
            if (out[1] > res->us_max)
                res->us_max = out[1];
        }
        /* the PCM back word by word (the port has no read burst) */
        for (i = 0; i < out[0] && rc == HP_OK; i++)
        {
            uint32_t w;
            uint8_t by[4];

            if (port.rd(bp->fifo_host + slot + AAC_SLOT_PCM + 4 * i, &w) < 0)
            {
                rc = BVP_E_PORT;
                break;
            }
            by[0] = w;
            by[1] = w >> 8;
            by[2] = w >> 16;
            by[3] = w >> 24;
            h = fnv_bytes(h, by, 4);
            res->hash = fnv_bytes(res->hash, by, 4);
        }
        res->frames += out[0];
        if (rc == HP_OK && out[0] == want_n && h == want_h)
            res->matched++;
        else if (rc == HP_OK)
        {
            if (res->first_bad < 0)
                res->first_bad = k;
            if (res->aus - res->matched - res->errors <= AAC_LOG_MAX)
            {
                snprintf(line, sizeof(line), "stage 9: AU %lu differs: %lu "
                         "frames, hash %08lx; want %lu, %08lx",
                         (unsigned long)k, (unsigned long)out[0],
                         (unsigned long)h, (unsigned long)want_n,
                         (unsigned long)want_h);
                log(line);
            }
        }
        /* the checkpoints: every AU for the copy check; logged for the
           first three and the first wrong ones */
        if (rc == HP_OK)
            aac_trace(bp, k, res->traced < AAC_TRACED_MAX &&
                      (k < 3 || out[0] != want_n || h != want_h), log, res);
    }
    res->ref_hash =res->aus_ref ? rd_le32(ref + 8 + 8 * res->aus_ref) : 0;
    if (bp->session)
        bvp_close(bp, 0);
    bcm_video_off();
    bcm_video_release();
    core_unpin(hs);
    core_free(hs);
    core_unpin(hr);
    core_free(hr);
    core_unpin(ha);
    core_free(ha);
    free_all();
    res->rc = rc;
    return rc == HP_OK ? 0 : rc;
}

void vplayer_seek(unsigned long ms)
{
    if (thread_id)
        queue_post(&vq, VQ_SEEK, (intptr_t)ms);
}

void vplayer_stop(void)
{
    if (thread_id)
    {
        static bool slow_said;
        long t0 = current_tick;

        queue_post(&vq, VQ_STOP, 0);
        /* the thread ends the session (its release: vplayer_pump) and
           exits; its stack is ours.  Bounded through the thread's own
           waits (each port, DMA and owner wait is: the freeze audit,
           2026-10-26), so this join is not cut short (its stack and
           block are in use until it exits); past 3 s it is said once */
        while (!thread_over)
        {
            vplayer_pump();
            sleep(1);
            if (!slow_said && TIME_AFTER(current_tick, t0 + 3 * HZ))
            {
                slow_said = true;
                ui_log("video", "FAULT vplayer_stop wait=thread-exit ms=%ld",
                       (long)(current_tick - t0) * (1000 / HZ));
            }
        }
        thread_wait(thread_id);
        queue_delete(&vq);
        thread_id = 0;
    }
    free_all();
}

bool vplayer_info(struct vplayer_info *info)
{
    memset(info, 0, sizeof(*info));
    info->state = -1;
    info->open_why = open_why;
    info->open_err = open_err;
    if (!B)
        return false;
    info->refused_at = B->core.refused_at;
    info->refused_nal = B->core.refused_nal;
    info->stills = B->core.stats.stills;
    info->reloads = B->core.stats.reloads;
    info->state = B->core.state;
    info->fail_why = B->core.fail_why;
    info->restarts = B->core.stats.restarts;
    info->wakes = B->core.stats.wakes;
    info->recoveries = B->core.stats.recoveries;
    info->posted = B->core.eng.stats.posted;
    info->skipped = B->core.eng.stats.skipped;
    info->dropped_gops = B->core.eng.stats.dropped_gops;
    info->lead_us = (uint32_t)B->core.eng.lead_us;
    info->direct_reads = B->direct;
    info->ended = B->ended;
    info->clock_dropped = !!(B->core.open_drop & HP_OPEN_FAST_CLOCK);
    info->sd_n = B->core.bvp.sd_n;
    memcpy(info->sd, B->core.bvp.sd, sizeof(info->sd));
    memcpy(info->sd_pll, B->core.sd_pll, sizeof(info->sd_pll));
    info->sdc_done = B->core.sdc.done;
    info->sdc_words = B->core.sdc.words;
    info->sdc_bad = B->core.sdc.bad;
    info->sdc_port_err = B->core.sdc.port_err;
    info->sdc_first[0] = B->core.sdc.first_off;
    info->sdc_first[1] = B->core.sdc.first_got;
    info->sdc_first[2] = B->core.sdc.first_want;
    info->sdc_span = B->core.sdc.span;
    info->sdc_last_ok = B->core.sdc.last_ok;
    info->vclk_on = B->core.vclk_inited;
    info->errors = B->core.st_words[8];
    info->concealed = B->core.st_words[9];
    if (vpc_vclk_means(&B->core, &info->vclk_mhz, &info->vclk_lvl10,
                       &info->vclk_idle_pm, &info->vclk_ms))
    {
        info->vclk_changes = B->core.gov.band_changes;
        info->vclk_clock_changes = B->core.gov.clock_changes;
        info->vclk_lvl_changes = B->core.gov.level_changes;
        info->vclk_policy = B->core.gov.policy;
        info->vclk_fails = B->core.gov.fails;
        info->vclk_band = B->core.gov.band;
        memcpy(info->vclk_band_ms, B->core.gov.at_ms,
               sizeof(info->vclk_band_ms));
    }
    info->vclk_misses = B->core.vclk_misses;
    memcpy(info->pm, B->core.pm, sizeof(info->pm));
    info->pm_n = B->core.pm_n;
    info->pm_losses = B->core.pm_losses;
    info->pm_fast = B->core.pm_fast;
    info->dpb = B->core.dpb;
    bcm_video_port_stats(info->port, false);
    info->vfy_words = B->core.wire.words;
    info->vfy_bad = B->core.wire.bad;
    memcpy(info->vfy_first, B->core.wire.first, sizeof(info->vfy_first));
    info->lock_waits = B->core.lock_waits;
    info->stalls = B->core.stalls;
    info->stall_max_us = B->core.stall_max_us;
    memcpy(info->guard, B->core.guard, sizeof(info->guard));
    info->guard_n = B->core.guard_n;
    info->evt_raw = B->core.bvp.evt_raw;
    return true;
}
