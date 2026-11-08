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

/* p19-vcpwr: the video chip's awake-time power rows (s1, l5, vs, ck, sd)
 * and the developer build's watch over S1 and L5 (devbench_vcpwr.h says
 * what each is; docs/bcm2722-reference/power-apple-vs-ours.md is the
 * design).  No row writes the merged image; every row restores what it
 * found, and a chip that stops is the heartbeat watchdog's and the cut's
 * (the boot ladder brings it up again). */

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "devbench_sim.h"               /* DEVBENCH_SIM */

#if defined(HAVE_BCMFW_RESIDENT) && defined(HAVE_BCMFW_DEVTEST) && \
    !defined(SIMULATOR)
#define VCP_DEVICE
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "rbpaths.h"
#include "bcm-devtest.h"
#include "devbcm.h"
#include "devbench_vcpwr.h"
#include "ipodui/uilog.h"
#ifdef HAVE_BCM_JOBS
#include "bcmjob_pp.h"
#include "bcmjob.h"
#endif

/* ---- shared: one FENCE through the job ring ---------------------------- */

/* 1 answered, 0 lost (queued, never completed), -1 jobs not up */
static int vcp_fence(void)
{
#ifdef HAVE_BCM_JOBS
    uint32_t seq;

    if (!bcmjob_have(J_OP_FENCE) || bcmjob_fence(&seq) != BCMJOB_OK)
        return -1;
    return bcmjob_wait(seq, J_T_FENCE_BASE) == BCMJOB_OK;
#else
    return -1;
#endif
}

static uint32_t vcp_chip_failures(void)
{
#ifdef HAVE_BCM_JOBS
    return bcmjob_chip_failures();
#else
    return 0;
#endif
}

static int vcp_selftest(void)
{
#ifdef HAVE_BCM_JOBS
    return bcmjob_selftest();
#else
    return 1;
#endif
}

/* The strikes file (developer builds: S1's failing boots).  One line,
 * "s1 <n>"; no file: 0 */
#define VCP_STRIKES ROCKBOX_DIR "/bcmvcpwr.txt"

static int vcp_strikes(void)
{
    char b[16] = "";
    int fd = open(VCP_STRIKES, O_RDONLY), n = 0;

    if (fd >= 0)
    {
        if (read(fd, b, sizeof(b) - 1) > 3 && !strncmp(b, "s1 ", 3))
            n = b[3] - '0';
        close(fd);
    }
    return n >= 0 && n <= 9 ? n : 0;
}

static void vcp_set_strikes(int n)
{
    char b[8];
    int fd;

    if (n <= 0)
    {
        remove(VCP_STRIKES);
        return;
    }
    fd = open(VCP_STRIKES, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        write(fd, b, snprintf(b, sizeof(b), "s1 %d\n", n > 9 ? 9 : n));
        close(fd);
    }
}

/* the saved switches (bcmres.cfg, the daily build's): S1 and L5 */
static void vcp_save_idle(bool on)
{
    bool idle, res = bcm_boot_resident_setting(&idle);

    if (idle != on)
        bcm_boot_resident_set(res, on);
}

static void vcp_save_level5(bool on)
{
    if (bcm_boot_level5_setting() != on)
        bcm_boot_level5_set(on);
}

/* ---- the watch ----------------------------------------------------------
 * UI thread, every main-loop pass; a second's work is a handful of port
 * reads under the LCD driver's lock.  Off (nothing read) unless S1 or L5
 * runs in RAM. */
static struct
{
    uint32_t gen;               /* the image the base belongs to */
    long next;                  /* the next S1 sample */
    long base_tick;
    uint32_t base[4];           /* ticks, bells, sleeps, bad */
    bool have_base;
    int read_fails;
    bool s1_struck;             /* this boot's strike is counted */
    /* L5 after a swap or a wake: the self-test, then the 2 s heartbeat */
    uint32_t l5_gen;
    long l5_until;
    uint32_t l5_fail0;
    bool l5_checking, l5_logged;
} vw;

static void vcp_s1_fail(const char *why)
{
    int n;

    bcm_boot_res_latch(true, false, why);
    if (vw.s1_struck)
        return;
    vw.s1_struck = true;
    n = vcp_strikes() + 1;
    vcp_set_strikes(n);
    if (n >= VCP_S1_STRIKES)
    {
        vcp_save_idle(false);
        devbcm_log("chip idle: saved off (third failing start)");
    }
}

static void vcp_l5_fail(const char *why, bool cut)
{
    vcp_save_level5(false);         /* L5: saved off at once */
    bcm_boot_res_latch(false, true, why);
    if (cut)
        bcm_boot_res_cut("level 5 failed");
}

void vcpwr_service(void)
{
    bool idle, level5;
    uint32_t gen, c[4], reg;
    int st;

    bcm_boot_res_now(&idle, &level5);
    if (!idle && !level5)
        return;
    st = bcm_boot_res_state(&gen);
    if (st == 0)
    {
        vw.have_base = false;       /* dark, a session: start again after */
        vw.read_fails = 0;
        return;
    }
    if (level5 && st > 0 && gen != vw.l5_gen)
    {
        /* L5 at a swap or a wake: the level as read back after the
           firmware's 10 ms settle, the self-test, then the heartbeat */
        int t;

        vw.l5_gen = gen;
        if (bcm_boot_res_read(c, &reg) < 0 || (reg & 0xF) != 5)
        {
            vcp_l5_fail("readback", false);
            return;
        }
        t = vcp_selftest();
        if (vcp_l5_why(5, t, true))
        {
            vcp_l5_fail("selftest", true);
            return;
        }
        vw.l5_checking = true;
        vw.l5_until = current_tick + VCP_HB_MS * HZ / 1000;
        vw.l5_fail0 = vcp_chip_failures();
    }
    if (vw.l5_checking && (st < 0 || vcp_chip_failures() != vw.l5_fail0))
    {
        vw.l5_checking = false;
        vcp_l5_fail("heartbeat", true);
        return;
    }
    if (vw.l5_checking && TIME_AFTER(current_tick, vw.l5_until))
    {
        vw.l5_checking = false;
        if (vcp_fence() == 0)
        {
            vcp_l5_fail("heartbeat", true);
            return;
        }
        if (!vw.l5_logged)
        {
            vw.l5_logged = true;
            ui_log("video", "chip level: 5 held (read back, self-test, "
                   "2 s heartbeat)");
        }
    }
    if (!idle || TIME_BEFORE(current_tick, vw.next))
        return;
    vw.next = current_tick + HZ;
    if (st < 0 || bcm_boot_res_read(c, &reg) < 0)
    {
        if (++vw.read_fails >= VCP_READ_FAILS)
            vcp_s1_fail("read");
        return;
    }
    vw.read_fails = 0;
    if (!vw.have_base || gen != vw.gen)
    {
        vw.gen = gen;
        vw.have_base = true;
    }
    else
    {
        uint32_t ms = (uint32_t)(current_tick - vw.base_tick) * (1000 / HZ);
        const char *why = vcp_s1_why(c[0] - vw.base[0], c[2] - vw.base[2],
                                     c[3] - vw.base[3], ms);

        /* a UI pass that came late is a long window, not a fault: the
           rule holds over any length */
        if (why)
        {
            vcp_s1_fail(why);
            return;
        }
    }
    memcpy(vw.base, c, sizeof(vw.base));
    vw.base_tick = current_tick;
}
#endif /* the watch */

#if defined(VCP_DEVICE) && defined(HAVE_BCMFW_VIDEO)
#include "button.h"
#include "lcd.h"
#include "backlight.h"
#include "core_alloc.h"
#include "audio.h"
#include "playlist.h"
#include "settings.h"
#include "video/vplayer.h"
#include "video/vpcore.h"
#include "video/vtrace.h"
#include "video/vclk.h"
#include "video/bcmvideo_image.h"       /* the flags name this image */
#include "devbench_vclk.h"              /* vs's own band check (vbv) */
#ifdef HAVE_IPOD_UI_DEVMODE
#include "ipodui/powerlog.h"
#endif

#define CLIP1   ROCKBOX_DIR "/bcmtest/clip.mp4"
#define CLIP2   ROCKBOX_DIR "/bcmtest/clip2.mp4"
#define RECORDS 1024

static void vcp_mark(const char *tag)
{
#ifdef HAVE_IPOD_UI_DEVMODE
    powerlog_mark(tag);
#else
    (void)tag;
#endif
}

/* the phase screen: drawn once a phase (a still screen) */
static void vcp_draw(const char *title, const char *what, int k, int n,
                     int secs)
{
    char line[40];

    lcd_clear_display();
    lcd_puts(0, 0, title);
    snprintf(line, sizeof(line), "Phase %d of %d, %d s:", k + 1, n, secs);
    lcd_puts(0, 2, line);
    lcd_puts(0, 3, what);
    lcd_puts(0, 5, "Leave it alone until the");
    lcd_puts(0, 6, "row ends.");
    lcd_puts(0, 8, "MENU: ends this row early.");
    lcd_update();
}

/* hold for secs, a FENCE every period ms (0: none), the backlight on;
 * false on MENU.  *fences answered, *lost queued and never answered;
 * *stopped: the image was marked stopped meanwhile */
static bool vcp_hold(int secs, int period, uint32_t *fences, uint32_t *lost,
                     bool *stopped)
{
    long end = current_tick + secs * HZ;
    uint32_t gen;

    while (TIME_BEFORE(current_tick, end))
    {
        int btn = button_get_w_tmo(period ? period * HZ / 1000 + 1 : HZ);

        backlight_on();
        if (btn == BUTTON_MENU)
            return false;
        if (bcm_boot_res_state(&gen) < 0)
            *stopped = true;
        if (period)
        {
            int f = vcp_fence();

            if (f > 0)
                (*fences)++;
            else if (f == 0)
                (*lost)++;
        }
    }
    return true;
}

static bool vcp_live(const char *tag)
{
    uint32_t c[4], reg;
    char line[96];

    if (bcm_boot_resident_active() && bcm_boot_res_read(c, &reg) == 0)
        return true;
    snprintf(line, sizeof(line), "%s: Not available: the resident video "
             "image isn't the display driver", tag);
    devbcm_log(line);
    return false;
}

/* ---- s1: idle sleep in the lit shell ------------------------------------ */

int devbench_vcpwr_s1(void)
{
    bool idle0, lvl0, pass = true, done = true, stopped = false;
    uint32_t c0[4], c1[4], reg, fences, lost;
    char line[128];
    int k, b_ok = 0;

    if (!vcp_live("s1"))
        return BENCH_SKIP;
    bcm_boot_res_now(&idle0, &lvl0);
    for (k = 0; k < VCP_PHASES && done; k++)
    {
        bool b = vcp_phase_b(k);
        long t0;
        uint32_t ms;
        const char *why;

        snprintf(line, sizeof(line), "s1 %c%d", b ? 'B' : 'A', k / 2 + 1);
        vcp_mark(line);
        vcp_draw("Chip idle sleep", b ? "B: chip sleeps when idle"
                                      : "A: chip polls (today)", k,
                 VCP_PHASES, VCP_PHASE_S);
        if (bcm_boot_res_live(b, 0, NULL) < 0 ||
            bcm_boot_res_read(c0, &reg) < 0)
        {
            pass = false;
            devbcm_log("s1: FAIL: the image no longer answers");
            break;
        }
        t0 = current_tick;
        fences = lost = 0;
        done = vcp_hold(VCP_PHASE_S, 500, &fences, &lost, &stopped);
        ms = (uint32_t)(current_tick - t0) * (1000 / HZ);
        if (bcm_boot_res_read(c1, &reg) < 0)
        {
            pass = false;
            devbcm_log("s1: FAIL: the image no longer answers");
            break;
        }
        why = vcp_s1_why(c1[0] - c0[0], c1[2] - c0[2], c1[3] - c0[3], ms);
        snprintf(line, sizeof(line), "s1 %c%d ms=%lu ticks=%lu bells=%lu "
                 "sleeps=%lu bad=%lu fences=%lu lost=%lu%s%s", b ? 'B' : 'A',
                 k / 2 + 1, (unsigned long)ms,
                 (unsigned long)(c1[0] - c0[0]),
                 (unsigned long)(c1[1] - c0[1]),
                 (unsigned long)(c1[2] - c0[2]),
                 (unsigned long)(c1[3] - c0[3]), (unsigned long)fences,
                 (unsigned long)lost, b && why ? " why=" : "",
                 b && why ? why : "");
        devbcm_log(line);
        if (b)
        {
            if (!why && !lost && !stopped)
                b_ok++;
            else
                pass = false;
        }
    }
    vcp_mark("s1");
    if (!done && pass)
    {
        bcm_boot_res_live(idle0, 0, NULL);
        devbcm_log("s1: Not run (MENU; the gate back as found)");
        return BENCH_SKIP;
    }
    pass = pass && b_ok == 2;
    if (pass)
    {
        /* the switch the daily build honours: on from the next start,
           and now (the watch keeps an eye on it) */
        vcp_save_idle(true);
        vcp_set_strikes(0);
        bcm_boot_res_live(1, 0, NULL);
    }
    else
    {
        vcp_save_idle(false);
        bcm_boot_res_latch(true, false, "s1 row");
    }
    snprintf(line, sizeof(line), "s1: %s (B phases woken on the tick, "
             "slept, nothing bad, no FENCE lost: %d of 2; idle sleep saved "
             "%s)", pass ? "PASS" : "FAIL", b_ok, pass ? "on" : "off");
    devbcm_log(line);
    return pass ? BENCH_PASS : BENCH_FAIL;
}

/* ---- l5: level 5 at the crystal ----------------------------------------- */

#define VCP_L5_GO "Run the core level test? For half of its 80 s the " \
    "video chip's core runs at level 5, lower than today's 11.\n\n" \
    "Risk: the screen may garble or freeze; holding MENU and SELECT " \
    "restarts the iPod and undoes it.\n\nYes, run it: starts it. Not " \
    "now (or MENU): skips this row; the session goes on."

int devbench_vcpwr_l5(void)
{
    static const char *const go[2] = { "Yes, run it", "Not now" };
    bool idle0, lvl0, pass = true, done = true, stopped = false;
    uint32_t c[4], reg, fences, lost, fail0;
    int k, b_ok = 0, level0;
    char line[128];

    if (!vcp_live("l5"))
        return BENCH_SKIP;
    if (devbench_ask("Chip core level 5", VCP_L5_GO, go, 2) != 0)
    {
        devbcm_log("l5: Not run (no go)");
        return BENCH_SKIP;
    }
    bcm_boot_res_read(c, &reg);
    level0 = (reg & 0xF) >= 5 ? (int)(reg & 0xF) : 11;
    bcm_boot_res_now(&idle0, &lvl0);
    bcm_boot_res_live(0, 0, NULL);      /* polling: the level's own step */
    for (k = 0; k < VCP_PHASES && done; k++)
    {
        bool b = vcp_phase_b(k);
        int rb = -1, st = 99;
        uint32_t t0;
        const char *why = NULL;

        snprintf(line, sizeof(line), "l5 %c%d", b ? 'B' : 'A', k / 2 + 1);
        vcp_mark(line);
        vcp_draw("Chip core level 5", b ? "B: core at level 5"
                                        : "A: core at level 11 (today)", k,
                 VCP_PHASES, VCP_PHASE_S);
        fail0 = vcp_chip_failures();
        t0 = USEC_TIMER;
        /* the write, Apple's 10 ms settle (in the firmware), the read */
        if (bcm_boot_res_live(-1, b ? 5 : level0, &rb) == -1)
        {
            pass = false;
            devbcm_log("l5: FAIL: the image no longer answers");
            break;
        }
        if (b)
            st = vcp_selftest();
        fences = lost = 0;
        done = vcp_hold(VCP_PHASE_S, 500, &fences, &lost, &stopped);
        if (b)
            why = vcp_l5_why(rb, st, !stopped && !lost &&
                             vcp_chip_failures() == fail0);
        snprintf(line, sizeof(line), "l5 %c%d level %d (write to read %lu "
                 "us) self-test %d fences=%lu lost=%lu stopped=%d%s%s",
                 b ? 'B' : 'A', k / 2 + 1, rb,
                 (unsigned long)(USEC_TIMER - t0) > 999999 ? 999999 :
                 (unsigned long)(USEC_TIMER - t0), st, (unsigned long)fences,
                 (unsigned long)lost, stopped, why ? " why=" : "",
                 why ? why : "");
        devbcm_log(line);
        if (b && !why)
            b_ok++;
        else if (b || rb != level0)
            pass = false;
        if (stopped)
            break;
    }
    vcp_mark("l5");
    if (!done && pass)
    {
        bcm_boot_res_live(idle0, lvl0 ? 5 : level0, NULL);
        devbcm_log("l5: Not run (MENU; the level back as found)");
        return BENCH_SKIP;
    }
    pass = pass && b_ok == 2;
    if (pass)
    {
        vcp_save_level5(true);
        bcm_boot_res_live(idle0, 5, NULL);  /* on now; the watch checks */
    }
    else
    {
        vcp_l5_fail("l5 row", stopped);
        bcm_boot_res_live(idle0, 0, NULL);
    }
    snprintf(line, sizeof(line), "l5: %s (level 5 read back after the "
             "10 ms settle, self-test and heartbeat held: %d of 2; found "
             "%d; level 5 saved %s)", pass ? "PASS" : "FAIL", b_ok, level0,
             pass ? "on" : "off");
    devbcm_log(line);
    return pass ? BENCH_PASS : BENCH_FAIL;
}

/* ---- a play (vs, sd): the bench's own loop, as devbench_vclk.c's ------- */

static struct vtrace trace;
static int trace_h;

static void vcp_on_frame(const struct vpc_frame *f)
{
    vtrace_add(&trace, f);
}

static bool vcp_raced(int top)
{
    struct vplayer_info vi;
    int k;

    for (k = 0; k < 10; k++)
    {
        sleep(HZ / 10);
        vplayer_pump();
        if (vplayer_info(&vi) && (int)vi.vclk_band == top)
            return true;
    }
    return false;
}

/* path for secs under the governor (top-of-band), a seek at 5 s and a
 * pause at 10 s when steps; gate: the idle gate after OPEN (-1: as the
 * session found it, cleared).  The counters' deltas into p */
static void vcp_play(const char *path, int secs, bool steps, int gate,
                     struct vcp_vs_phase *p, struct vtrace_sum *sum)
{
    struct vplayer_info vi;
    int s, repeat = global_settings.repeat_mode, top = 0, k;
    uint32_t c0[4], c1[4];
    long t0 = 0;
    bool started, counted = false;

    memset(p, 0, sizeof(*p));
    vtrace_init(&trace, core_get_data(trace_h), RECORDS);
    vplayer_set_frame_hook(vcp_on_frame);
    vplayer_vclk_policy(VCLK_POLICY_TOP);
    started = vplayer_start(path);
    p->failed = !started;
    p->raced = !steps;
    if (started)
    {
        for (k = 0; k < 12; k++)
            if (vplayer_vclk_mask() >> k & 1)
                top = k;
        /* the B phases' gate, after OPEN; A reads only */
        if (bcm_video_idle_gate(gate > 0 ? 1 : -1, c0) == 0)
        {
            counted = true;
            t0 = current_tick;
        }
        global_settings.repeat_mode = REPEAT_ONE;
        playlist_create(NULL, NULL);
        playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false, true);
        playlist_start(0, 0, 0);
        for (s = 0; s < secs * 10; s++)
        {
            vplayer_pump();
            sleep(HZ / 10);
            backlight_on();
            if (!vplayer_info(&vi) || vi.state == VPC_FAILED)
            {
                p->failed = true;
                break;
            }
            if (steps && s == 50)
            {
                audio_pre_ff_rewind();
                audio_ff_rewind(1000);
                vplayer_seek(1000);
                p->raced = vcp_raced(top);
            }
            if (steps && s == 100)
            {
                audio_pause();
                sleep(HZ);
                audio_resume();
            }
        }
        if (counted && bcm_video_idle_gate(0, c1) == 0)
        {
            /* cleared at each phase's end (and by the take at the next) */
            p->ms = (uint32_t)(current_tick - t0) * (1000 / HZ);
            p->ticks = c1[0] - c0[0];
            p->bells = c1[1] - c0[1];
            p->sleeps = c1[2] - c0[2];
            p->bad = c1[3] - c0[3];
        }
        if (vplayer_info(&vi))
        {
            p->moves = vi.vclk_changes;
            p->gov_ms = vi.vclk_ms;
            p->idle_pm = vi.vclk_idle_pm;
        }
    }
    vplayer_stop();
    audio_stop();
    global_settings.repeat_mode = repeat;
    vplayer_set_frame_hook(NULL);
    vtrace_summary(&trace, false, p->failed, sum);
    p->skipped = sum->skipped;
    p->dropped = sum->dropped_gops;
    p->late = sum->late_posts;
    p->av_us = sum->drift_max_us;
}

static bool vcp_play_begin(const char *tag)
{
    char line[96];

    if (!file_exists(CLIP1) || !file_exists(VPLAYER_IMAGE))
    {
        snprintf(line, sizeof(line), "%s: Not available: no %s", tag,
                 file_exists(CLIP1) ? "video image" : "test clip");
        devbcm_log(line);
        return false;
    }
    trace_h = core_alloc(RECORDS * sizeof(struct vtrace_rec));
    if (trace_h <= 0)
    {
        trace_h = 0;
        snprintf(line, sizeof(line), "%s: Not available: no memory", tag);
        devbcm_log(line);
        return false;
    }
    core_pin(trace_h);
    return true;
}

static void vcp_play_end(void)
{
    if (trace_h > 0)
        core_free(trace_h);
    trace_h = 0;
}

/* ---- vs: idle sleep inside a video session ------------------------------ */

/* The bands the band check (vbv) verified for the embedded image
   (bcmpassvc.txt, as vplayer.c check_vclk reads it) */
static uint32_t vcp_bands_verified(void)
{
    char text[400];
    int fd = open(VPLAYER_VCLK_FLAGS, O_RDONLY), n = 0;

    if (fd >= 0)
    {
        n = read(fd, text, sizeof(text) - 1);
        close(fd);
    }
    text[n > 0 ? n : 0] = '\0';
    return vclk_flags_mask(text, BCMVIDEO_IMAGE_BYTES, BCMVIDEO_IMAGE_FNV);
}

/* Sitting 4gh-b5: vs read idle=0 in every phase.  The idle share is the
   chip clock governor's (vpc_vclk_means), and the governor starts only
   with a band between the crystal and 150 MHz verified for this image
   (vplayer.c be_vclk); the session's image was new and vbv held, so none
   was.  vs runs the band check itself first when the flags don't name
   one.  true: the governor can run */
static bool vcp_vs_bands(void)
{
    const uint32_t outer = 1u | 1u << VCLK_TOP;
    uint32_t m = vcp_bands_verified();
    char line[120];
    int rc;

    if (m & ~outer)
    {
        snprintf(line, sizeof(line), "vs: governor bands %03lx verified for "
                 "this image (checked: bcmpassvc.txt)", (unsigned long)m);
        devbcm_log(line);
        return true;
    }
    devbcm_log("vs: no governor band verified for this image: the band "
               "check (vbv) runs first");
    rc = devbench_vclk_verify();
    m = vcp_bands_verified();
    snprintf(line, sizeof(line), "vs: band check %s, bands %03lx",
             rc == BENCH_PASS ? "passed" : rc == BENCH_SKIP ? "not run"
             : "failed", (unsigned long)m);
    devbcm_log(line);
    return (m & ~outer) != 0;
}

int devbench_vcpwr_vs(void)
{
    static const char *const clips[2] = { CLIP1, CLIP2 };
    static const char *const names[2] = { "clip1", "clip2" };
    struct vcp_vs_phase ph[VCP_PHASES];
    struct vtrace_sum sum;
    char line[200];
    int c, k, ran = 0, ok = 0;
    bool any_counted = false;

    if (!vcp_vs_bands())
    {
        devbcm_log("vs: Not available: no governor band holds on this unit "
                   "(the idle share needs the governor)");
        return BENCH_SKIP;
    }
    if (!vcp_play_begin("vs"))
        return BENCH_SKIP;
    for (c = 0; c < 2; c++)
    {
        const char *why = NULL;
        bool clip_ok = true;

        if (!file_exists(clips[c]))
            continue;
        for (k = 0; k < VCP_PHASES; k++)
        {
            bool b = vcp_phase_b(k);

            snprintf(line, sizeof(line), "vs %s %c%d", names[c],
                     b ? 'B' : 'A', k / 2 + 1);
            vcp_mark(line);
            vcp_play(clips[c], VCP_VS_S, true, b, &ph[k], &sum);
            any_counted |= ph[k].ms > 0;
            snprintf(line, sizeof(line), "vs %s %c%d mhz=%lu idle=%lu "
                     "ticks=%lu ms=%lu sleeps=%lu bad=%lu bells=%lu "
                     "skipped=%lu dropped=%lu late=%lu av_us=%ld moves=%lu "
                     "top=%s", names[c], b ? 'B' : 'A', k / 2 + 1,
                     (unsigned long)sum.mhz_hi,
                     (unsigned long)ph[k].idle_pm,
                     (unsigned long)ph[k].ticks, (unsigned long)ph[k].ms,
                     (unsigned long)ph[k].sleeps, (unsigned long)ph[k].bad,
                     (unsigned long)ph[k].bells,
                     (unsigned long)ph[k].skipped,
                     (unsigned long)ph[k].dropped,
                     (unsigned long)ph[k].late, (long)ph[k].av_us,
                     (unsigned long)ph[k].moves, ph[k].raced ? "yes" : "no");
            devbcm_log(line);
            if (b && !why)
                why = vcp_vs_why(&ph[k], &ph[k - 1]);
        }
        if (!any_counted)
        {
            vcp_play_end();
            devbcm_log("vs: Not available: the session is not on the "
                       "resident image (no idle counters)");
            return BENCH_SKIP;
        }
        clip_ok = !why;
        snprintf(line, sizeof(line), "vs %s: %s%s%s (idle B %lu/%lu vs A "
                 "%lu/%lu permille%s)", names[c], clip_ok ? "PASS" : "FAIL",
                 why ? " " : "", why ? why : "",
                 (unsigned long)ph[1].idle_pm, (unsigned long)ph[3].idle_pm,
                 (unsigned long)ph[0].idle_pm, (unsigned long)ph[2].idle_pm,
                 vcp_vs_effect(&ph[1], &ph[0]) &&
                 vcp_vs_effect(&ph[3], &ph[2])
                 ? "; gated: B slept, A did not" : "; no effect");
        devbcm_log(line);
        ran++;
        ok += clip_ok;
    }
    vcp_play_end();
    vcp_mark("vs");
    snprintf(line, sizeof(line), "vs: %s: %d of %d clips (the gate cleared "
             "at every phase's end)", ran && ok == ran ? "PASS" : "FAIL", ok,
             ran);
    devbcm_log(line);
    return ran && ok == ran ? BENCH_PASS : BENCH_FAIL;
}

/* ---- ck: the block clocks ----------------------------------------------- */

static const char *vcp_src_name(uint32_t off, uint32_t w)
{
    static const char *const n[8] = { "none", "B", "C", "D", "s4", "xtal",
                                      "s6", "A" };

    if (off == 0x28 && vcp_clk_src(w) == 0)
        return "xtal";
    return n[vcp_clk_src(w)];
}

/* one step: words at offs[] to pw 0, read back, the self-test, a hold,
 * then each word back as found.  true: passed */
static bool vcp_ck_step(int n, const uint32_t *offs, uint32_t base,
                        bool *menu)
{
    uint32_t found[2], v, rd, fences = 0, lost = 0;
    bool ok = true, stopped = false;
    char line[128];
    int j, st;

    for (j = 0; j < n; j++)
    {
        if (bcm_boot_res_rw(VCP_CLK_BASE + offs[j], &found[j], 0) < 0)
            return false;
        v = 0xA5A50000u;                /* pw 0: the block's clock off */
        rd = 0xFFFFFFFFu;
        if (bcm_boot_res_rw(VCP_CLK_BASE + offs[j], &v, 1) < 0 ||
            bcm_boot_res_rw(VCP_CLK_BASE + offs[j], &rd, 0) < 0 ||
            (rd & 0xFFFF) != 0)
            ok = false;
        snprintf(line, sizeof(line), "ck +%02lX found %04lx wrote 0 read "
                 "%04lx", (unsigned long)offs[j],
                 (unsigned long)(found[j] & 0xFFFF),
                 (unsigned long)(rd & 0xFFFF));
        devbcm_log(line);
    }
    st = vcp_selftest();
    *menu = !vcp_hold(VCP_CK_HOLD_S, 100, &fences, &lost, &stopped);
    for (j = 0; j < n; j++)
    {
        v = 0xA5A50000u | (found[j] & 0xFFFF);
        rd = 0xFFFFFFFFu;
        if (bcm_boot_res_rw(VCP_CLK_BASE + offs[j], &v, 1) < 0 ||
            bcm_boot_res_rw(VCP_CLK_BASE + offs[j], &rd, 0) < 0 ||
            (rd & 0xFFFF) != (found[j] & 0xFFFF))
        {
            ok = false;
            snprintf(line, sizeof(line), "ck +%02lX: FAIL: not back (%04lx "
                     "read %04lx)", (unsigned long)offs[j],
                     (unsigned long)(found[j] & 0xFFFF),
                     (unsigned long)(rd & 0xFFFF));
            devbcm_log(line);
        }
    }
    ok = ok && !stopped && vcp_ck_step_ok(0, 0, st, fences, base, lost);
    snprintf(line, sizeof(line), "ck step +%02lX%s selftest %d fences %lu "
             "of base %lu lost %lu stopped %d: %s", (unsigned long)offs[0],
             n > 1 ? "/+38" : "", st, (unsigned long)fences,
             (unsigned long)base, (unsigned long)lost, stopped,
             ok ? "ok" : "fail");
    devbcm_log(line);
    return ok;
}

int devbench_vcpwr_ck(void)
{
    static const uint32_t s2[2] = { 0x34, 0x38 }, s3[1] = { 0x3C };
    uint32_t off, w = 0, base = 0, lost = 0;
    char line[160];
    int n = 0, off_n = 0;
    bool reads = true, ok, menu = false, stopped = false;

    if (!vcp_live("ck"))
        return BENCH_SKIP;
    vcp_mark("ck census");
    vcp_draw("Chip block clocks", "Census, then two steps", 0, 4,
             3 * VCP_CK_HOLD_S);
    /* step 1, read only: PLL dividers +00..+0C, controls +14..+20, the
       clock words, the regulator */
    for (off = 0; off <= 0x20; off += 4)
    {
        if (off == 0x10)
            continue;
        if (bcm_boot_res_rw(VCP_CLK_BASE + off, &w, 0) < 0)
            reads = false;
        n += snprintf(line + n, sizeof(line) - n, " %02lX=%04lx",
                      (unsigned long)off, (unsigned long)(w & 0xFFFF));
    }
    devbcm_log("ck found PLLs:");
    devbcm_log(line);
    for (off = VCP_CLK_FIRST; off <= VCP_CLK_LAST; off += 4)
    {
        if (bcm_boot_res_rw(VCP_CLK_BASE + off, &w, 0) < 0)
            reads = false;
        snprintf(line, sizeof(line), "ck found +%02lX = %04lx (%s / %lu)%s",
                 (unsigned long)off, (unsigned long)(w & 0xFFFF),
                 vcp_src_name(off, w), (unsigned long)vcp_clk_div(w),
                 vcp_clk_off(off, w) ? " off" : "");
        devbcm_log(line);
        off_n += vcp_clk_off(off, w);
    }
    if (bcm_boot_res_rw(0x10001400u, &w, 0) < 0)
        reads = false;
    snprintf(line, sizeof(line), "ck found regulator %04lx (level %lu); "
             "%d clock words off", (unsigned long)(w & 0xFFFF),
             (unsigned long)(w & 0xF), off_n);
    devbcm_log(line);
    if (!reads)
    {
        devbcm_log("ck: FAIL: a census read did not complete");
        return BENCH_FAIL;
    }
    /* the baseline: the same hold with nothing written */
    vcp_mark("ck base");
    vcp_draw("Chip block clocks", "Baseline, nothing written", 1, 4,
             VCP_CK_HOLD_S);
    if (!vcp_hold(VCP_CK_HOLD_S, 100, &base, &lost, &stopped))
    {
        devbcm_log("ck: Not run (MENU)");
        return BENCH_SKIP;
    }
    snprintf(line, sizeof(line), "ck base fences %lu lost %lu",
             (unsigned long)base, (unsigned long)lost);
    devbcm_log(line);
    vcp_mark("ck 34/38");
    vcp_draw("Chip block clocks", "Audio clocks off", 2, 4, VCP_CK_HOLD_S);
    ok = vcp_ck_step(2, s2, base, &menu);
    if (ok && !menu)
    {
        /* id 6, the USB-side gate (bit 15; reset meaning unknown): last,
           and only after ids 2 and 3 passed */
        vcp_mark("ck 3C");
        vcp_draw("Chip block clocks", "USB-side clock off", 3, 4,
                 VCP_CK_HOLD_S);
        ok = vcp_ck_step(1, s3, base, &menu);
    }
    vcp_mark("ck");
    if (menu && ok)
    {
        devbcm_log("ck: Not run (MENU; every word back as found)");
        return BENCH_SKIP;
    }
    snprintf(line, sizeof(line), "ck: %s", ok ? "PASS" : "FAIL");
    devbcm_log(line);
    return ok ? BENCH_PASS : BENCH_FAIL;
}

/* ---- sd: Apple's SDRAM idle after a video ------------------------------- */

#define VCP_CACHE_CTL   0x10000C00u     /* bits 30 and 16: Apple clears */
#define VCP_CACHE_ON    0xC0000000u     /* bit 31 enable, 30 a flush asked */
#define VCP_CACHE_SD    0x00010000u     /* bit 16: set by every image's
                                           SDRAM bring-up (Apple's @147ca,
                                           ours sdram_up.h): not "on" */

/* the pattern through the port: written (write) or checked; the wrong
 * words */
static uint32_t vcp_sd_pattern(uint32_t seed, bool write)
{
    uint32_t i, v, wrong = 0;

    for (i = 0; i < VCP_SD_WORDS; i++)
    {
        uint32_t a = VCP_SD_PATTERN + 4 * i;

        v = vcp_sd_word(seed, i);
        if (write)
        {
            if (bcm_boot_res_rw(a, &v, 1) < 0)
                return VCP_SD_WORDS;
        }
        else if (bcm_boot_res_rw(a, &v, 0) < 0 || v != vcp_sd_word(seed, i))
            wrong++;
    }
    return wrong;
}

int devbench_vcpwr_sd(void)
{
    struct vcp_vs_phase p;
    struct vtrace_sum sum;
    uint32_t seed = USEC_TIMER | 1, w, polls, f1 = 0, f10 = 0, lost = 0;
    uint32_t wrong, cache;
    bool cleared = false, s1 = false, s10 = false, menu, back;
    char line[128];
    const char *why;

    if (!vcp_live("sd") || !vcp_play_begin("sd"))
        return BENCH_SKIP;
    /* a short play: the session's OPEN brings SDRAM up, and after its
       CLOSE it is left refreshing (memory-contract.md section 3) */
    vcp_mark("sd play");
    vcp_play(CLIP1, VCP_SD_PLAY_S, false, -1, &p, &sum);
    if (p.failed || !vcp_live("sd"))
    {
        vcp_play_end();
        devbcm_log("sd: Not available: the play did not hand the display "
                   "back to the resident image");
        return BENCH_SKIP;
    }
    /* sitting 4gh (quirk #58): this guard refused bit 16 as "the cache
       is on", but every image sets bit 16 once SDRAM is up, so it
       skipped every run.  The cache is on only with bit 31 (ours never
       sets it) or a flush in flight (bit 30); bit 16 is cleared here as
       Apple's idle (@14892) clears it, and the restore's cut resets it */
    if (bcm_boot_res_rw(VCP_CACHE_CTL, &cache, 0) < 0 ||
        (cache & VCP_CACHE_ON))
    {
        vcp_play_end();
        snprintf(line, sizeof(line), "sd: Not available: the chip's cache "
                 "word reads %08lx (enabled or a flush in flight)",
                 (unsigned long)cache);
        devbcm_log(line);
        return BENCH_SKIP;
    }
    snprintf(line, sizeof(line), "sd cache word %08lx", (unsigned long)cache);
    devbcm_log(line);
    vcp_sd_pattern(seed, true);
    wrong = vcp_sd_pattern(seed, false);
    if (wrong)
    {
        vcp_play_end();
        snprintf(line, sizeof(line), "sd: Not available: SDRAM is not up "
                 "(%lu of %d words wrong before the idle)",
                 (unsigned long)wrong, VCP_SD_WORDS);
        devbcm_log(line);
        return BENCH_SKIP;
    }
    /* Apple's idle (@14892): bits 30 and 16 cleared, command 0, bit 15
       polled, bounded */
    vcp_mark("sd idle");
    vcp_draw("Chip memory idle", "Memory idle, then restart", 0, 2, 11);
    if (cache & VCP_CACHE_SD)
    {
        cache &= ~(VCP_CACHE_SD | 0x40000000u);
        bcm_boot_res_rw(VCP_CACHE_CTL, &cache, 1);
    }
    w = 0;
    bcm_boot_res_rw(VCP_SD_CMD, &w, 1);
    for (polls = 0; polls < VCP_SD_POLLS; polls++)
        if (bcm_boot_res_rw(VCP_SD_CMD, &w, 0) == 0 && !(w & VCP_SD_BUSY))
        {
            cleared = true;
            break;
        }
    snprintf(line, sizeof(line), "sd idle polls=%lu bit15=%s",
             (unsigned long)polls, cleared ? "clear" : "stuck");
    devbcm_log(line);
    menu = false;
    if (cleared)
    {
        bool stopped = false;

        /* the core and the port still answer with SDRAM idle */
        menu = !vcp_hold(1, 100, &f1, &lost, &stopped);
        s1 = !stopped && !lost && f1;
        snprintf(line, sizeof(line), "sd hold=1 s fences=%lu lost=%lu "
                 "stopped=%d", (unsigned long)f1, (unsigned long)lost,
                 stopped);
        devbcm_log(line);
        if (!menu)
        {
            menu = !vcp_hold(10, 100, &f10, &lost, &stopped);
            s10 = !stopped && !lost && f10;
            snprintf(line, sizeof(line), "sd hold=10 s fences=%lu lost=%lu "
                     "stopped=%d", (unsigned long)f10, (unsigned long)lost,
                     stopped);
            devbcm_log(line);
        }
    }
    /* the restore: the image believes SDRAM is up, so the cut, and the
       next session's OPEN brings it up on a fresh start (an in-place
       re-bring-up needs an image op: the next batch) */
    bcm_boot_res_cut("sd restore");
    lcd_update();
    sleep(2 * HZ);
    vcp_mark("sd back");
    vcp_draw("Chip memory idle", "A short play after it", 1, 2,
             VCP_SD_PLAY_S);
    vcp_play(CLIP1, VCP_SD_PLAY_S, false, -1, &p, &sum);
    back = !p.failed && sum.shown > 0 && !p.skipped && !p.dropped;
    vcp_play_end();
    snprintf(line, sizeof(line), "sd back bringup=%s shown=%lu "
             "pattern=n/a (cut; kept or lost needs the image op)",
             back ? "ok" : "fail", (unsigned long)sum.shown);
    devbcm_log(line);
    vcp_mark("sd");
    if (menu && cleared)
    {
        devbcm_log("sd: Not run (MENU; restored by the cut)");
        return BENCH_SKIP;
    }
    why = vcp_sd_why(cleared, s1, s10, back, 0);
    snprintf(line, sizeof(line), "sd: %s%s%s", why ? "FAIL " : "PASS",
             why ? "why=" : "", why ? why : "");
    devbcm_log(line);
    return why ? BENCH_FAIL : BENCH_PASS;
}
#endif /* the rows */

#ifdef DEVBENCH_SIM
/* ---- the simulator: the rows are silicon questions, so the sim never
 * models a pass.  Each says "Not run (sim)"; its fault switch
 * (bench_sim set fail_<id>) fails it, to drive the runner's paths. */
#include "file.h"
#include "devbench.h"
#include "devbench_vcpwr.h"
#include "devbench_sim_calls.h"

static int vcp_sim_row(const char *id)
{
    char path[64], line[64];

    snprintf(path, sizeof(path), DBS_FAIL "%s", id);
    if (file_exists(path))
    {
        snprintf(line, sizeof(line), "%s: FAIL (fault switch, sim)", id);
        devbcm_log(line);
        return BENCH_FAIL;
    }
    snprintf(line, sizeof(line), "%s: Not run (sim)", id);
    devbcm_log(line);
    return BENCH_SKIP;
}

int devbench_vcpwr_s1(void) { return vcp_sim_row("s1"); }
int devbench_vcpwr_l5(void) { return vcp_sim_row("l5"); }
int devbench_vcpwr_vs(void) { return vcp_sim_row("vs"); }
int devbench_vcpwr_ck(void) { return vcp_sim_row("ck"); }
int devbench_vcpwr_sd(void) { return vcp_sim_row("sd"); }
#endif
