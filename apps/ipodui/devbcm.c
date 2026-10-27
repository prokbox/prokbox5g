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
 * Developer → Debug: the BCM2722 device test and the boot path toggle
 * (devbcm.h).  Debug-menu screens, like the rest of that menu: plain
 * English, developer mode only, never reachable from the shell's menus.
 * The full log is bcmtest.log in the system folder; these are summaries.
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
#include "config.h"
#include "action.h"
#include "gui/list.h"
#include "bcm-devtest.h"
#include "devbcm.h"
/* the results log needs these in every devtest build */
#include <stdio.h>
#include "file.h"
#include "kernel.h"
#include "rbpaths.h"
#include "version.h"
#include "timefuncs.h"
#ifdef HAVE_BCMFW_VIDEO
#include "core_alloc.h"
#include "audio.h"
#include "playlist.h"
#include "settings.h"
#ifdef HAVE_COP_CODEC
#include "codec_cop.h"
#endif
#include "video/vplayer.h"
#include "video/bcmvideo_image.h"
#include "video/vpcore.h"
#include "video/vtrace.h"
#include "backlight.h"
#include "pcmbuf.h"
#include "pcm_mixer.h"
#include "sound.h"
#ifdef HAVE_DAC_STANDBY
#include "dac_ext.h"
#endif
#include "../../bcmfw/hostproto.h"
#endif
/* the developer power log (Phase 9, p10-powerlog): stage 5b marks its
 * phases there when this build has it */
#if defined(__has_include)
#if __has_include("powerlog.h")
#include "powerlog.h"
#define DEVBCM_POWERLOG 1
#endif
#endif

/* Every stage's summary screen also goes to bcmtest-results.log: one
 * block per run, with the build and the time, appended and never rolled
 * at boot (session 36: six reboots rolled bcmtest.log's one previous
 * generation over the stage 7, 8a-1 and 9 results).  Past RESULTS_MAX the
 * file becomes bcmtest-results.1.log, so two files' worth stay. */
#define RESULTS_LOG     ROCKBOX_DIR "/bcmtest-results.log"
#define RESULTS_LOG1    ROCKBOX_DIR "/bcmtest-results.1.log"
#define RESULTS_MAX     (256 * 1024)

extern char simplelist_buffer[];   /* apps/gui/list.c */

static void results_put(int fd, const char *text)
{
    write(fd, text, strlen(text));
    write(fd, "\n", 1);
}

static void results_append(const struct simplelist_info *info)
{
    /* the summary's lines, as simplelist_addline() packed them: one after
     * another in simplelist_buffer, each with its NUL */
    const char *p = simplelist_buffer,
               *end = simplelist_buffer + SIMPLELIST_MAX_LINES *
                                         SIMPLELIST_MAX_LINELENGTH;
    int n = simplelist_get_line_count(), fd, k;
    char head[160];

    fd = open(RESULTS_LOG, O_RDONLY);
    if (fd >= 0)
    {
        off_t size = lseek(fd, 0, SEEK_END);

        close(fd);
        if (size > RESULTS_MAX)
        {
            remove(RESULTS_LOG1);
            rename(RESULTS_LOG, RESULTS_LOG1);
        }
    }
    fd = open(RESULTS_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0)
        return;
#if CONFIG_RTC
    {
        struct tm *tm = get_time();

        snprintf(head, sizeof(head), "=== %s: build %s, "
                 "%04d-%02d-%02d %02d:%02d:%02d, up %lu s ===",
                 info->title ? info->title : "?", rbversion,
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec,
                 (unsigned long)(current_tick / HZ));
    }
#else
    snprintf(head, sizeof(head), "=== %s: build %s, up %lu s ===",
             info->title ? info->title : "?", rbversion,
             (unsigned long)(current_tick / HZ));
#endif
    results_put(fd, head);
    for (k = 0; k < n && p < end; k++)
    {
        const char *z = memchr(p, 0, end - p);

        if (!z)
            break;
        results_put(fd, p);
        p = z + 1;
    }
    {
        /* whether bcmtest.log got this boot's lines (session 37: a
           boot's stage lines never reached it) */
        uint32_t ls[6];

        bcm_devtest_log_stats(ls);
        snprintf(head, sizeof(head), "(bcmtest.log this boot: %lu lines, "
                 "%lu bytes written, file %lu bytes; %lu opens failed, %lu "
                 "writes failed, last code %ld)",
                 (unsigned long)ls[0], (unsigned long)ls[4],
                 (unsigned long)ls[5], (unsigned long)ls[1],
                 (unsigned long)ls[2], (long)(int32_t)ls[3]);
        results_put(fd, head);
    }
    write(fd, "\n", 1);
    close(fd);
}

/* The bench session (devbench.c): its line hook sees every line a row
   logs and every line of its summary; in batch the rows' own confirm
   screens are skipped (the session's one confirm stands for them) and
   their summaries go to the log only.  Off outside the session. */
static bool bench_batch;
static void (*bench_hook)(const char *line);

void devbcm_bench_mode(bool batch, void (*hook)(const char *line))
{
    bench_batch = batch;
    bench_hook = hook;
}

static bool show(struct simplelist_info *info)
{
    results_append(info);
    if (bench_hook)
    {
        const char *p = simplelist_buffer;
        int k, n = simplelist_get_line_count();

        for (k = 0; k < n; k++)
        {
            bench_hook(p);
            p += strlen(p) + 1;
        }
    }
    if (bench_batch)
        return false;
    simplelist_addline("Log: bcmtest.log");
    return simplelist_show_list(info);
}

static int bits(uint32_t v)
{
    int n = 0;

    for (; v; v &= v - 1)
        n++;
    return n;
}

/* Stages 4 and 4b: counts and the clock only; the rest is in the log */
static void bench_lines(const struct bcmdt_result *r, unsigned stages)
{
    int s = (stages & BCMDT_STAGE(BCMDT_STAGE_4B)) ? BCMDT_STAGE_4B : 4;
    const char *name = s == 4 ? "Stage 4" : "Stage 4b";

    if (!(r->passed_mask & BCMDT_STAGE(s == 4 ? 1 : 0)))
    {
        simplelist_addline("%s: not run", name);
        return;
    }
    simplelist_addline("%s: %s", name,
                       (r->passed_mask & BCMDT_STAGE(s)) ? "PASS" : "FAIL");
    simplelist_addline("Checks: vector %d/%d, scalar %d/%d",
                       bits(r->b_vpass & r->b_vall), bits(r->b_vall),
                       bits(r->b_spass & r->b_sall), bits(r->b_sall));
    simplelist_addline("Probe checks: %d/%d, kernels %lu/%lu",
                       bits(r->b_ppass & r->b_pall), bits(r->b_pall),
                       (unsigned long)r->b_kernels_ok, (unsigned long)r->b_nk);
    simplelist_addline(s == 4 ? "Clock: %lu.%lu MHz (est.)"
                              : "Clock before switch: %lu.%lu MHz (est.)",
                       (unsigned long)(r->b_clock_khz / 1000),
                       (unsigned long)(r->b_clock_khz % 1000 / 100));
    if (s != 4)
    {
        /* the line above is table 1, run before the switch; table 2 runs
         * after it: near 150 only if the PLL took (dev8) */
        simplelist_addline("Clock after switch: %lu.%lu MHz (est.)",
                           (unsigned long)(r->b_clock2_khz / 1000),
                           (unsigned long)(r->b_clock2_khz % 1000 / 100));
        simplelist_addline("Clock at start: %lu.%lu MHz (measured)",
                           (unsigned long)(r->b_start_khz / 1000),
                           (unsigned long)(r->b_start_khz % 1000 / 100));
    }
    if (!(r->passed_mask & BCMDT_STAGE(s)))
        simplelist_addline("Last step: %08lx", (unsigned long)r->b_step);
}

static unsigned stages_passed;      /* the last run's passed_mask */

static bool devbcm_test_stages(unsigned stages)
{
    struct bcmdt_result r;
    struct simplelist_info info;
    int rc = bcm_devtest_run_stages(stages, &r);

    stages_passed = rc == -3 ? 0 : r.passed_mask;
    int last = (stages & (BCMDT_STAGE(3) | BCMDT_STAGE(BCMDT_STAGE_5))) ? 3
             : (stages & BCMDT_STAGE(2)) ? 2
             : (stages & BCMDT_STAGE(1)) ? 1 : 0;

    simplelist_info_init(&info, "BCM test", 0, NULL);
    simplelist_reset_lines();
    if (rc == -2)
        simplelist_addline("Not run: display busy");
    if (rc == -3)
    {
        simplelist_addline("Not run: %s", r.why);
        return show(&info);
    }
    /* a run with 4b/4c/5b/5i asks for stages out of order: "through
     * stage 1" read as a failure on a 4c pass (session 8) */
    if (rc >= 0 && !(stages & ~r.passed_mask) && !r.why[0])
        simplelist_addline("All stages passed");
    else
        simplelist_addline("Passed through stage %d", r.passed);
    if (r.why[0])
        simplelist_addline("Stopped: %s", r.why);
    simplelist_addline("Panel ID: %d (lev %08lx)", r.panel_id,
                       (unsigned long)r.gpio_lev);
    if (last >= 1)
    {
        simplelist_addline("Info: %08lx  from0: %lu", (unsigned long)r.info,
                           (unsigned long)r.from0);
        simplelist_addline("Self-check: %03lx/%03lx  faulted %04lx",
                           (unsigned long)r.selfcheck,
                           (unsigned long)r.selfcheck_all,
                           (unsigned long)r.p_faulted);
    }
    if (r.fault_pc || r.fault_sp)
        simplelist_addline("Fault pc %08lx sp %08lx", (unsigned long)r.fault_pc,
                           (unsigned long)r.fault_sp);
    simplelist_addline("Apple frame: %lu us", (unsigned long)r.apple_us);
    if (last >= 2)
    {
        simplelist_addline("First/wake: %lu/%lu us", (unsigned long)r.first_us,
                           (unsigned long)r.wake_us);
        simplelist_addline("Frame/rect: %lu/%lu us", (unsigned long)r.frame_us,
                           (unsigned long)r.rect_us);
    }
    if (last >= 3)
    {
        simplelist_addline("DMA frame/rect: %lu/%lu us", (unsigned long)r.dma_frame_us,
                           (unsigned long)r.dma_rect_us);
        simplelist_addline("BCM counter: %lu/%lu us", (unsigned long)r.dma_bcm_us,
                           (unsigned long)r.dma_bcm_rect_us);
        simplelist_addline("Forced: %lu us (BCM %lu)", (unsigned long)r.forced_us,
                           (unsigned long)r.forced_bcm_us);
        simplelist_addline("DMA done %lu, timeouts %lu", (unsigned long)r.dma_done,
                           (unsigned long)r.dma_tmo);
        simplelist_addline("ch1 %08lx %08lx %08lx", (unsigned long)r.dma_cs,
                           (unsigned long)r.dma_int, (unsigned long)r.dma_cb);
    }
    if (stages & (BCMDT_STAGE(4) | BCMDT_STAGE(BCMDT_STAGE_4B)))
        bench_lines(&r, stages);
    if (last >= 2)
        simplelist_addline("Pass flags: stage 2 %s, stage 3 %s",
                           (r.passed_mask & BCMDT_STAGE(2)) ? "written" : "cleared",
                           (r.passed_mask & BCMDT_STAGE(3)) ? "written" :
                           (last >= 3 || !(r.passed_mask & BCMDT_STAGE(2)))
                               ? "cleared" : "kept");
    if (stages & BCMDT_STAGE(BCMDT_STAGE_4B))
        simplelist_addline("Pass flag: stage 4b %s", (r.passed_mask &
                           BCMDT_STAGE(BCMDT_STAGE_4B)) && r.img4b ? "written"
                                                                   : "cleared");
    if (stages & BCMDT_STAGE(4))
        simplelist_addline("Pass flag: stage 4 %s", (r.passed_mask &
                           BCMDT_STAGE(4)) && r.img4 ? "written" : "cleared");
    if (stages & BCMDT_STAGE(BCMDT_STAGE_4C))
    {
        simplelist_addline("Stage 4c: %s",
                           (r.passed_mask & BCMDT_STAGE(BCMDT_STAGE_4C)) ? "PASS"
                                                                         : "FAIL");
        simplelist_addline("Frames crystal/PLL/again: %lu/%lu/%lu us",
                           (unsigned long)r.c_base_us, (unsigned long)r.c_fast_us,
                           (unsigned long)r.c_slow_us);
        simplelist_addline("Switch up/down: %lu/%lu us",
                           (unsigned long)r.c_up_us, (unsigned long)r.c_down_us);
        simplelist_addline("Pass flag: stage 4c %s", (r.passed_mask &
                           BCMDT_STAGE(BCMDT_STAGE_4C)) && r.img4c ? "written"
                                                                   : "cleared");
    }
    if (stages & BCMDT_STAGE(BCMDT_STAGE_5B))
    {
        simplelist_addline("Stage 5b: %s", (r.passed_mask &
                           BCMDT_STAGE(BCMDT_STAGE_5B)) ? "done" : "stopped");
        simplelist_addline("A: %lu frames, %lu late, PP %lu ms",
                           (unsigned long)r.m_frames[0], (unsigned long)r.m_late[0],
                           (unsigned long)(r.m_pp_us[0] / 1000));
        simplelist_addline("B (stub): %lu frames, %lu late, PP %lu ms",
                           (unsigned long)r.m_frames[1], (unsigned long)r.m_late[1],
                           (unsigned long)(r.m_pp_us[1] / 1000));
    }
    if (stages & BCMDT_STAGE(BCMDT_STAGE_5I))
    {
        simplelist_addline("Stage 5i: %s", (r.passed_mask &
                           BCMDT_STAGE(BCMDT_STAGE_5I)) ? "PASS" : "FAIL");
        simplelist_addline("No-op busy/idle: %lu/%lu us",
                           (unsigned long)r.i_nop_us[0],
                           (unsigned long)r.i_nop_us[1]);
        simplelist_addline("Rect max busy/idle: %lu/%lu us",
                           (unsigned long)r.i_rect_max_us[0],
                           (unsigned long)r.i_rect_max_us[1]);
        simplelist_addline("Ticks %lu, doorbells %lu, bad %lu",
                           (unsigned long)r.i_ticks, (unsigned long)r.i_bells,
                           (unsigned long)r.i_bad);
        simplelist_addline("Framebuffer words changed: %lu",
                           (unsigned long)r.i_fb_bad);
        simplelist_addline("Pass flag: stage 5i %s", (r.passed_mask &
                           BCMDT_STAGE(BCMDT_STAGE_5I)) && r.img5i ? "written"
                                                                   : "cleared");
    }
    if (stages & BCMDT_STAGE(BCMDT_STAGE_5S))
    {
        simplelist_addline("Stage 5s: %s (core sleep %s)", (r.passed_mask &
                           BCMDT_STAGE(BCMDT_STAGE_5S)) ? "PASS" : "FAIL",
                           r.s_slept ? "on" : "off");
        simplelist_addline("Wake light/deep: %lu/%lu ms",
                           (unsigned long)(r.s_wake_us[0] / 1000),
                           (unsigned long)(r.s_wake_us[1] / 1000));
        simplelist_addline("Reset/cold: %lu/%lu ms",
                           (unsigned long)(r.s_reset_us / 1000),
                           (unsigned long)(r.s_cold_us / 1000));
        simplelist_addline("Refused %lx, framebuffer changed %lu",
                           (unsigned long)r.s_refused,
                           (unsigned long)r.s_fb_bad);
        simplelist_addline("Upload CPU/E2: %lu/%lu us",
                           (unsigned long)r.s_up_us[0],
                           (unsigned long)r.s_up_us[1]);
        simplelist_addline("Read back wrong CPU/E2: %lu/%lu",
                           (unsigned long)r.s_up_bad[0],
                           (unsigned long)r.s_up_bad[1]);
    }
    if (stages & BCMDT_STAGE(BCMDT_STAGE_3D))
    {
        int sc;

        simplelist_addline("Stage 3d: %s (150 MHz %s)", (r.passed_mask &
                           BCMDT_STAGE(BCMDT_STAGE_3D)) ? "PASS" : "FAIL",
                           r.d_fast ? "run" : "not run");
        for (sc = 0; sc < BCMDT_3D_SCENES; sc++)
        {
            unsigned long f[2];
            int c;

            for (c = 0; c < 2; c++)
                f[c] = r.d_us[c][sc] ? (unsigned long)((uint64_t)r.d_frames[c][sc] *
                                                       10000000u / r.d_us[c][sc]) : 0;
            simplelist_addline("Scene %d, %lu tris: %lu.%lu / %lu.%lu fps", sc,
                               (unsigned long)r.d_tris[sc], f[0] / 10, f[0] % 10,
                               f[1] / 10, f[1] % 10);
        }
        simplelist_addline("(fps at 27 / 150 MHz, 30 the most)");
        if (r.d_te_us)
            simplelist_addline("Panel refresh (TE): %lu.%lu Hz",
                               (unsigned long)((10000000u + r.d_te_us / 2) /
                                               r.d_te_us / 10),
                               (unsigned long)((10000000u + r.d_te_us / 2) /
                                               r.d_te_us % 10));
        else
            simplelist_addline("Panel refresh (TE): not seen");
        if (r.d_sync && r.d_sync_us)
        {
            unsigned long f = (unsigned long)((uint64_t)r.d_sync_frames *
                                              10000000u / r.d_sync_us);

            simplelist_addline("Walk on TE: %lu.%lu fps, %lu late", f / 10, f % 10,
                               (unsigned long)r.d_sync_late);
        }
        simplelist_addline("Frames unlike the simulator: %lu",
                           (unsigned long)r.d_crc_bad);
    }
    if (stages & BCMDT_STAGE(BCMDT_STAGE_5))
    {
        simplelist_addline("Stage 5: %s",
                           (r.passed_mask & BCMDT_STAGE(BCMDT_STAGE_5)) ? "PASS"
                           : (r.passed_mask & BCMDT_STAGE(3)) ? "FAIL"
                                                              : "not run");
        simplelist_addline("Upload/full/list: %lu/%lu/%lu us",
                           (unsigned long)r.e_host_us, (unsigned long)r.e_full_us,
                           (unsigned long)r.e_rect_max_us);
        simplelist_addline("Rects %lu, wakes %lu (max %lu ms)",
                           (unsigned long)r.e_rects, (unsigned long)r.e_wakes,
                           (unsigned long)(r.e_wake_max_us / 1000));
        simplelist_addline("Stop seen %lu us, fallback %lu ms",
                           (unsigned long)r.e_stop_us,
                           (unsigned long)(r.e_fallback_us / 1000));
        simplelist_addline("Pass flag: stage 5 %s", (r.passed_mask &
                           BCMDT_STAGE(BCMDT_STAGE_5)) && r.img5 ? "written"
                                                                 : "cleared");
    }
    return show(&info);
}

/* The bench session's stage runs: the stages asked for, no screen of
   their own in batch; the stages that passed (BCMDT_STAGE bits) */
unsigned devbcm_bench_stages(unsigned stages)
{
    stages_passed = 0;
    devbcm_test_stages(stages);
    return stages_passed;
}

/* The rows that change the chip's clocks, voltage or pins (until the
 * power cut that ends them) confirm first.  Only SELECT on the last line
 * runs one (scroll down to it: a stray press cannot); MENU cancels.  (The
 * stage rows 0-5, 4b and 4c are the Bench session's steps now, through
 * devbcm_bench_stages; devmode menu re-layout, 2026-10-26.) */
static bool confirm_4b;
static int confirm_line;

static int confirm_cb(int action, struct gui_synclist *lists)
{
    /* no wraparound: one tick back from the top must not land on "Run" */
    lists->wraparound = false;
    if (action == ACTION_STD_OK)
    {
        if (gui_synclist_get_sel_pos(lists) != confirm_line)
            return ACTION_NONE;
        confirm_4b = true;
        return ACTION_STD_CANCEL;
    }
    return action;
}

/* dev9: stage 5b, the drift offload A/B (about 13 minutes), on a stage 5
 * flag; phases marked in the developer power log when it is on */
bool devbcm_test5b(void)
{
    struct simplelist_info info;
    bool ok;

    simplelist_info_init(&info, "BCM stage 5b", 0, NULL);
    simplelist_reset_lines();
    simplelist_addline("Optional measurement, about 13 min.");
    simplelist_addline("Power log on (5 s) or a USB meter;");
    simplelist_addline("music stopped; leave it alone.");
    simplelist_addline("Needs a stage 5 pass flag.");
    simplelist_addline("Risks: runbook, stage 5b.");
    simplelist_addline("MENU: cancel");
    confirm_line = 6;
    simplelist_addline("Run stage 5b now (SELECT here)");
    confirm_4b = false;
    info.action_callback = confirm_cb;
    simplelist_show_list(&info);
    if (!confirm_4b)
        return false;
#ifdef DEVBCM_POWERLOG
    bcm_devtest_set_mark(powerlog_mark);
#endif
    ok = devbcm_test_stages(BCMDT_STAGE(0) | BCMDT_STAGE(BCMDT_STAGE_5B));
    bcm_devtest_set_mark(NULL);
    return ok;
}

/* Stage 5i, idle sleep (about 3 minutes), on a stage 5 flag; the
 * screen's backlight goes off for a minute in the middle */
bool devbcm_test5i(void)
{
    struct simplelist_info info;
    const char *why = "";
    bool ok;

    simplelist_info_init(&info, "BCM stage 5i", 0, NULL);
    simplelist_reset_lines();
    if (!bcm_devtest_5i_allowed(&why))
    {
        simplelist_addline("Not available: %s", why);
        simplelist_addline("Run stages 0-5 first (5 must pass)");
        return show(&info);
    }
    simplelist_addline("Optional, about 3 min. The video");
    simplelist_addline("chip sleeps between commands; the");
    simplelist_addline("light goes off for a minute.");
    simplelist_addline("Leave it alone until it ends.");
    simplelist_addline("Risks: runbook, stage 5i.");
    simplelist_addline("MENU: cancel");
    confirm_line = 6;
    simplelist_addline("Run stage 5i now (SELECT here)");
    confirm_4b = false;
    info.action_callback = confirm_cb;
    simplelist_show_list(&info);
    if (!confirm_4b)
        return false;
#ifdef DEVBCM_POWERLOG
    bcm_devtest_set_mark(powerlog_mark);
#endif
    ok = devbcm_test_stages(BCMDT_STAGE(0) | BCMDT_STAGE(BCMDT_STAGE_5I));
    bcm_devtest_set_mark(NULL);
    return ok;
}

/* Stage 5s, the sleep spikes (about 3 minutes), on a stage 5 flag; the
 * screen goes dark and comes back several times */
bool devbcm_test5s(void)
{
    struct simplelist_info info;
    const char *why = "";
    bool ok, sleep;

    simplelist_info_init(&info, "BCM stage 5s", 0, NULL);
    simplelist_reset_lines();
    if (!bcm_devtest_5s_allowed(&why, &sleep))
    {
        simplelist_addline("Not available: %s", why);
        simplelist_addline("Run stages 0-5 first (5 must pass)");
        return show(&info);
    }
    simplelist_addline("Optional, about 3 min. The screen");
    simplelist_addline("goes dark and comes back; check the");
    simplelist_addline("picture each time it is held.");
    simplelist_addline("Chip sleep: %s", sleep ? "on (5i passed)" : "off");
    simplelist_addline("Risks: runbook, stage 5s.");
    simplelist_addline("MENU: cancel");
    confirm_line = 6;
    simplelist_addline("Run stage 5s now (SELECT here)");
    confirm_4b = false;
    info.action_callback = confirm_cb;
    simplelist_show_list(&info);
    if (!confirm_4b)
        return false;
#ifdef DEVBCM_POWERLOG
    bcm_devtest_set_mark(powerlog_mark);
#endif
    ok = devbcm_test_stages(BCMDT_STAGE(0) | BCMDT_STAGE(BCMDT_STAGE_5S));
    bcm_devtest_set_mark(NULL);
    return ok;
}

/* Stage 3d, the 3D render test (about 2 min, or 3 with the 150 MHz
 * runs), on a stage 5 flag; phases marked in the power log */
bool devbcm_test3d(void)
{
    struct simplelist_info info;
    const char *why = "";
    bool ok, fast;

    simplelist_info_init(&info, "BCM stage 3d", 0, NULL);
    simplelist_reset_lines();
    if (!bcm_devtest_3d_allowed(&why, &fast))
    {
        simplelist_addline("Not available: %s", why);
        simplelist_addline("Run stages 0-5 first (5 must pass)");
        return show(&info);
    }
    simplelist_addline("Optional, about %d min: five 3D", fast ? 2 : 1);
    simplelist_addline("scenes on the video chip, 10 s");
    simplelist_addline("each; just watch.");
    simplelist_addline("150 MHz runs: %s", fast ? "yes (4b, 4c passed)" : "no");
    simplelist_addline("Risks: runbook, stage 3d.");
    simplelist_addline("MENU: cancel");
    confirm_line = 6;
    simplelist_addline("Run stage 3d now (SELECT here)");
    confirm_4b = false;
    info.action_callback = confirm_cb;
    simplelist_show_list(&info);
    if (!confirm_4b)
        return false;
#ifdef DEVBCM_POWERLOG
    bcm_devtest_set_mark(powerlog_mark);
#endif
    ok = devbcm_test_stages(BCMDT_STAGE(0) | BCMDT_STAGE(BCMDT_STAGE_3D));
    bcm_devtest_set_mark(NULL);
    return ok;
}

#ifdef HAVE_BCMFW
bool devbcm_boot_status(void)
{
    struct simplelist_info info;
    char buf[64];
    const char *line;
    int k;

    simplelist_info_init(&info, "BCM boot path", 0, NULL);
    simplelist_reset_lines();
    for (k = 0; (line = bcm_boot_status(k, buf, sizeof(buf))); k++)
        simplelist_addline("%s", line);
    return show(&info);
}

static bool set_mode(enum bcmfw_mode mode)
{
    bcm_boot_set_mode(mode);
    return devbcm_boot_status();
}

bool devbcm_boot_off(void) { return set_mode(BCMFW_MODE_OFF); }
bool devbcm_boot_apple(void) { return set_mode(BCMFW_MODE_APPLE); }
bool devbcm_boot_cpu(void) { return set_mode(BCMFW_MODE_CPU); }
bool devbcm_boot_dma(void) { return set_mode(BCMFW_MODE_DMA); }

/* The status screen's first draw is the update that takes the fallback */
bool devbcm_boot_fallback(void)
{
    bcm_boot_test_fallback();
    return devbcm_boot_status();
}

#ifdef HAVE_BCMFW_L1
/* #62 level 1's failures, on purpose: the next boot's DMA rung or every
 * rung (a one-shot file), or the next backlight wake (in RAM) */
bool devbcm_inject_swap(void)
{
    bcm_boot_inject(BCMFW_INJECT_SWAP);
    return devbcm_boot_status();
}

bool devbcm_inject_swap_all(void)
{
    bcm_boot_inject(BCMFW_INJECT_SWAP_ALL);
    return devbcm_boot_status();
}

bool devbcm_inject_wake(void)
{
    bcm_boot_inject(BCMFW_INJECT_WAKE);
    return devbcm_boot_status();
}
#endif
#endif /* HAVE_BCMFW */

#ifdef HAVE_BCMFW_VIDEO
/* Stage 6 (p14-player): our own test clips through the real player (the
 * audio on Rockbox's playback path, the video following it on our
 * image), logged picture by picture; device-session-1.md, "Session 9".
 * Clip 1 is synthetic (a counter to photograph), clip 2 real footage
 * with pans and cuts (an openly licensed excerpt, bcmfw/video/
 * mkclip-real.sh) for pacing and sync. */
#define STAGE6_CLIP     ROCKBOX_DIR "/bcmtest/clip.mp4"
#define STAGE6_CLIP2    ROCKBOX_DIR "/bcmtest/clip2.mp4"
#define STAGE6_RECORDS  2048        /* about a minute at 30 pictures/s */
#define STAGE6_MAX_S    60

static struct vtrace s6_trace;
static long s6_t0;

static void s6_frame(const struct vpc_frame *f)
{
    vtrace_add(&s6_trace, f);
}

static void s6_line(void *ctx, const char *text)
{
    char buf[288];

    (void)ctx;
    snprintf(buf, sizeof(buf), "%6ld %s",
             (long)((USEC_TIMER - s6_t0) / 1000), text);
    bcm_devtest_log(buf);
    if (bench_hook)
        bench_hook(text);
}

static void s6_log(const char *text)
{
    s6_line(NULL, text);
}

/* One pass through the player: what the summary and the log need */
struct s6_pass
{
    struct vtrace_sum sum;
    struct vplayer_info vi;
    bool started, ended, have_vi;
};

static const char *const s6_steps[] =
{
    "up", "command 7 never done", "probe failed", "decoder context",
    "decoder frames",
};

static const char *s6_phase(uint32_t w)
{
    switch (w >> 24)    /* bcmfw/video/video.h VIDEO_PH_* */
    {
    case 0x01: return "opening";
    case 0x10: return "memory test, size";
    case 0x11: return "memory test, grid";
    case 0x12: return "memory test, vector load";
    case 0x13: return "memory test, vector store";
    case 0x20: return "packet taken";
    case 0x21: return "wrap copy";
    case 0x30: return "NAL unit";
    case 0x31: return "macroblock";
    case 0x32: return "loop filter";
    case 0x50: return "conversion";
    case 0x51: return "panel push";
    case 0x5F: return "picture shown";
    case 0x0F: return "between packets";
    default:   return "?";
    }
}

/* The frames' place from a BCM address */
static const char *s6_where(uint32_t a)
{
    return !a ? "unknown" : a >= 0xFE000000u ? "SDRAM" : "SRAM";
}

/* Everything one pass learned, to the log (the summary repeats the key
 * words, and logs them too) */
static void s6_detail(const struct s6_pass *p)
{
    const struct vplayer_info *vi = &p->vi;
    char line[200];
    uint32_t w = vi->sd[0], step = w >> 12 & 0xF;

    if (vi->clock_dropped && vi->sd_pll[0])
    {
        snprintf(line, sizeof(line), "stage 6 SDRAM at 150 MHz: %08lx, "
                 "+0x20 %08lx, probe read %08lx %08lx; again at 27 MHz",
                 (unsigned long)vi->sd_pll[0], (unsigned long)vi->sd_pll[1],
                 (unsigned long)vi->sd_pll[2], (unsigned long)vi->sd_pll[3]);
        s6_log(line);
    }
    if (!vi->sd_n || (w >> 16) != 0x5344)
        s6_log("stage 6 SDRAM: no report from the image");
    else
    {
        snprintf(line, sizeof(line), "stage 6 SDRAM: %s, attempt %lu, "
                 "bit 15 seen %lx, %s, %s; %08lx",
                 step < 5 ? s6_steps[step] : "?",
                 (unsigned long)(w >> 8 & 0xF), (unsigned long)(w >> 4 & 0xF),
                 w & 2 ? "150 MHz" : "27 MHz", w & 1 ? "timed" : "counted",
                 (unsigned long)w);
        s6_log(line);
    }
    if (vi->sd_n > 1)
    {
        snprintf(line, sizeof(line), "stage 6 SDRAM: +0x20 %08lx, probe "
                 "read %08lx %08lx (want 12345678 87654321)",
                 (unsigned long)vi->sd[1], (unsigned long)vi->sd[2],
                 (unsigned long)vi->sd[3]);
        s6_log(line);
    }
    if (vi->sd[4])
    {
        uint32_t m = vi->sd[4];

        snprintf(line, sizeof(line), "stage 6 memory test: %lu KB by "
                 "aliasing, %lu grid words wrong, vector load %s, vector "
                 "store %s; %08lx", (unsigned long)((m >> 16 & 0xFF) * 64),
                 (unsigned long)(m >> 8 & 0xFF),
                 m & 1 ? "right" : "WRONG", m & 2 ? "right" : "WRONG",
                 (unsigned long)m);
        s6_log(line);
    }
    if (vi->sdc_port_err)
    {
        snprintf(line, sizeof(line), "stage 6 readback (6s): host port "
                 "failed after +%06lx of %lu KB",
                 (unsigned long)vi->sdc_last_ok,
                 (unsigned long)(vi->sdc_span >> 10));
        s6_log(line);
    }
    else if (vi->sdc_done)
    {
        snprintf(line, sizeof(line), "stage 6 readback (6s): %lu of %lu "
                 "words wrong over %lu KB; first at +%06lx: %08lx, want "
                 "%08lx", (unsigned long)vi->sdc_bad,
                 (unsigned long)vi->sdc_words,
                 (unsigned long)(vi->sdc_span >> 10),
                 (unsigned long)vi->sdc_first[0],
                 (unsigned long)vi->sdc_first[1],
                 (unsigned long)vi->sdc_first[2]);
        s6_log(line);
    }
    snprintf(line, sizeof(line), "stage 6 frames: %s (%08lx)",
             s6_where(vi->dpb), (unsigned long)vi->dpb);
    s6_log(line);
    snprintf(line, sizeof(line), "stage 6 port: longest wait %lu us read "
             "address, %lu us read data, %lu us write; %lu timeouts",
             (unsigned long)vi->port[0], (unsigned long)vi->port[1],
             (unsigned long)vi->port[2], (unsigned long)vi->port[3]);
    s6_log(line);
    if (vi->port[3])
    {
        static const char *const kind[] =
            { "?", "read address", "read data", "write" };

        snprintf(line, sizeof(line), "stage 6 port: last timeout on %s "
                 "at %08lx; CONTROL %04lx, RD_ADDR %04lx",
                 kind[vi->port[4] < 4 ? vi->port[4] : 0],
                 (unsigned long)vi->port[5], (unsigned long)vi->port[6],
                 (unsigned long)vi->port[7]);
        s6_log(line);
    }
    if (vi->vfy_words)
    {
        snprintf(line, sizeof(line), "stage 6 packet readback: %lu of %lu "
                 "words wrong; first at %08lx: %08lx, want %08lx",
                 (unsigned long)vi->vfy_bad, (unsigned long)vi->vfy_words,
                 (unsigned long)vi->vfy_first[0],
                 (unsigned long)vi->vfy_first[1],
                 (unsigned long)vi->vfy_first[2]);
        s6_log(line);
    }
    snprintf(line, sizeof(line), "stage 6 stalls ridden out: %lu, longest "
             "%lu us; lockstep held %lu pushes",
             (unsigned long)vi->stalls, (unsigned long)vi->stall_max_us,
             (unsigned long)vi->lock_waits);
    s6_log(line);
    if (vi->guard_n == 48 && vi->guard[0] == 0x47554152)
    {
        /* bcmfw/video/video.h VIDEO_GUARD */
        static const char *const check[] =
            { "?", "NAL unit", "conversion", "panel push", "idle" };
        const uint32_t *g = vi->guard;

        if (g[1])
            snprintf(line, sizeof(line), "stage 6 guard: header hit by %s "
                     "%06lx, found by the %s check after %lu packets, %lu "
                     "pictures; 0x100 %08lx 0x108 %08lx 0x110 %08lx "
                     "0x128 %08lx", s6_phase(g[1] & 0x7FFFFFFF),
                     (unsigned long)(g[1] & 0xFFFFFF),
                     check[g[2] < 5 ? g[2] : 0], (unsigned long)g[3],
                     (unsigned long)g[13], (unsigned long)g[4],
                     (unsigned long)g[5], (unsigned long)g[6],
                     (unsigned long)g[7]);
        else
            snprintf(line, sizeof(line), "stage 6 guard: header intact "
                     "over %lu checks", (unsigned long)g[14]);
        s6_log(line);
        snprintf(line, sizeof(line), "stage 6 pictures: 1st Y %08lx U "
                 "%08lx V %08lx, 2nd Y %08lx U %08lx V %08lx; 1st "
                 "converted %08lx", (unsigned long)g[40],
                 (unsigned long)g[41], (unsigned long)g[42],
                 (unsigned long)g[43], (unsigned long)g[44],
                 (unsigned long)g[45], (unsigned long)g[46]);
        s6_log(line);
        snprintf(line, sizeof(line), "stage 6 guard: planes Y %08lx U "
                 "%08lx V %08lx, stride %lu; C kernels %05lx",
                 (unsigned long)g[8], (unsigned long)g[9],
                 (unsigned long)g[10], (unsigned long)g[11],
                 (unsigned long)g[12]);
        s6_log(line);
        if ((g[16] >> 16) == 0x5354)
        {
            /* video.h VIDEO_ST_*: bit n failed */
            static const char *const st[11] =
            {
                "transform", "transform DC", "intra 4x4", "intra 16x16",
                "intra chroma", "MC luma", "MC chroma", "loop filter luma",
                "loop filter chroma", "colour flat", "colour ramp",
            };
            uint32_t k, t = g[17] >> 24;
            int n = snprintf(line, sizeof(line), "stage 6 self-test: %lu "
                             "comparisons; wrong:",
                             (unsigned long)g[20]);

            for (k = 0; k < 11 && n < (int)sizeof(line) - 24; k++)
                if (g[16] & (1u << k))
                    n += snprintf(line + n, sizeof(line) - n, " %s", st[k]);
            if (!(g[16] & 0xFFFF))
                snprintf(line + n, sizeof(line) - n, " none");
            s6_log(line);
            if (g[16] & 0xFFFF)
            {
                snprintf(line, sizeof(line), "stage 6 self-test: first "
                         "wrong byte in %s at +%lu (row %lu, col %lu): "
                         "vector %02lx, C %02lx", t < 11 ? st[t] : "?",
                         (unsigned long)(g[17] & 0xFFFFFF),
                         (unsigned long)((g[17] & 0xFFFFFF) / 64),
                         (unsigned long)((g[17] & 0xFFFFFF) % 64),
                         (unsigned long)g[18], (unsigned long)g[19]);
                s6_log(line);
            }
            if (g[21])
            {
                int n = snprintf(line, sizeof(line), "stage 6 self-test: "
                                 "low SRAM changed by:");

                for (k = 0; k < 11 && n < (int)sizeof(line) - 24; k++)
                    if (g[21] & (1u << k))
                        n += snprintf(line + n, sizeof(line) - n, " %s",
                                      st[k]);
                snprintf(line + n, sizeof(line) - n, "; first at %03lx: "
                         "%08lx", (unsigned long)g[22], (unsigned long)g[23]);
            }
            else
                snprintf(line, sizeof(line), "stage 6 self-test: low "
                         "SRAM untouched");
            s6_log(line);
            /* each failing test's first wrong byte (dev16) */
            for (k = 0; k < 11; k++)
                if (g[24 + k])
                {
                    uint32_t o = g[24 + k] & 0xFFFF;

                    snprintf(line, sizeof(line), "stage 6 self-test: %s "
                             "first wrong at +%lu (row %lu, col %lu): "
                             "vector %02lx, C %02lx", st[k],
                             (unsigned long)o, (unsigned long)(o / 64),
                             (unsigned long)(o % 64),
                             (unsigned long)(g[24 + k] >> 16 & 0xFF),
                             (unsigned long)(g[24 + k] >> 24));
                    s6_log(line);
                }
            snprintf(line, sizeof(line), "stage 6 self-test: coefficients "
                     "at %08lx", (unsigned long)g[35]);
            s6_log(line);
            if ((g[47] >> 16) == 0x5343)
            {
                int n = snprintf(line, sizeof(line), "stage 6 self-test "
                                 "from a cleared VRF: wrong:");

                for (k = 0; k < 11 && n < (int)sizeof(line) - 24; k++)
                    if (g[47] & (1u << k))
                        n += snprintf(line + n, sizeof(line) - n, " %s",
                                      st[k]);
                if (!(g[47] & 0xFFFF))
                    snprintf(line + n, sizeof(line) - n, " none");
                s6_log(line);
            }
            if ((g[36] >> 16) == 0x414C)
            {
                snprintf(line, sizeof(line), "stage 6 scalar test: failing "
                         "cases %04lx; first case %lu computed %08lx",
                         (unsigned long)(g[36] & 0xFFFF),
                         (unsigned long)g[37], (unsigned long)g[38]);
                s6_log(line);
            }
        }
    }
    else
    {
        snprintf(line, sizeof(line), "stage 6 guard: not read (%lu words, "
                 "%08lx); last torn event index %08lx",
                 (unsigned long)vi->guard_n, (unsigned long)vi->guard[0],
                 (unsigned long)vi->evt_raw);
        s6_log(line);
    }
    if (vi->pm_losses)
    {
        snprintf(line, sizeof(line), "stage 6 lost %lu time(s), last on %s: "
                 "%s %06lx at %08lx after %lu packets; frames %08lx",
                 (unsigned long)vi->pm_losses,
                 vi->pm_fast ? "150 MHz" : "27 MHz", s6_phase(vi->pm[0]),
                 (unsigned long)(vi->pm[0] & 0xFFFFFF),
                 (unsigned long)vi->pm[2], (unsigned long)vi->pm[1],
                 (unsigned long)vi->pm[3]);
        s6_log(vi->pm_n ? line : "stage 6 lost: the port read nothing");
        if (vi->pm_n)
        {
            snprintf(line, sizeof(line), "stage 6 lost: fault pc %08lx sp "
                     "%08lx, info %08lx, state %08lx, status %08lx; %lu of "
                     "9 words read", (unsigned long)vi->pm[4],
                     (unsigned long)vi->pm[5], (unsigned long)vi->pm[6],
                     (unsigned long)vi->pm[7], (unsigned long)vi->pm[8],
                     (unsigned long)vi->pm_n);
            s6_log(line);
        }
    }
}

/* One clip through the player, its trace logged after "stage 6 clip n";
 * mode: the write modes (VPLAYER_PORT_*, dev12) */
static void s6_play(int n, const char *path, const char *what,
                    uint32_t mode, uint32_t kernels, struct vtrace_rec *rec,
                    struct s6_pass *p)
{
    struct vplayer_info vi;
    char line[96];
    int repeat, s;
    bool failed = false;

    memset(p, 0, sizeof(*p));
    vtrace_init(&s6_trace, rec, STAGE6_RECORDS);
    snprintf(line, sizeof(line), "stage 6 clip %d: %s", n, what);
    s6_log(line);
    vplayer_set_frame_hook(s6_frame);
    vplayer_set_sdram_check(true);
    vplayer_set_port_mode(mode);
    vplayer_set_kernels(kernels);
    vplayer_set_dump(kernels & HP_OPEN_DUMP ? n : 0);
    vplayer_bench_play(true);       /* stage 6 measures the player */
    p->started = vplayer_start(path);
    vplayer_bench_play(false);
    repeat = global_settings.repeat_mode;
    if (p->started)
    {
        global_settings.repeat_mode = REPEAT_OFF;
        playlist_create(NULL, NULL);
        playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false, true);
        playlist_start(0, 0, 0);
        for (s = 0; s < STAGE6_MAX_S * 10; s++)
        {
            vplayer_pump();     /* the player's release, if it gives up */
            sleep(HZ / 10);
            if (!vplayer_info(&vi))
                break;
            if (vi.ended || vi.state == VPC_FAILED)
            {
                p->ended = vi.ended;
                failed = vi.state == VPC_FAILED;
                break;
            }
            /* the audio ran out without the last picture */
            if (s > 20 && !(audio_status() & AUDIO_STATUS_PLAY))
                break;
        }
        if (vplayer_info(&p->vi))
        {
            p->have_vi = true;
            failed |= p->vi.state == VPC_FAILED;
            s6_detail(p);
        }
    }
    else
        s6_log("stage 6: the player refused the clip (ipodui.log)");
    vplayer_stop();
    audio_stop();
    global_settings.repeat_mode = repeat;
    vplayer_set_frame_hook(NULL);
    vplayer_set_sdram_check(false);
    vplayer_set_port_mode(0);
    vplayer_set_kernels(0);
    vplayer_set_dump(0);

    vtrace_summary(&s6_trace, p->ended, failed || !p->started, &p->sum);
    vtrace_lines(&s6_trace, &p->sum, s6_line, NULL);
}

/* A summary line: on the screen, and the same words in the log (owner,
 * session 10: nothing only on the screen) */
static void s6_say(const char *fmt, ...)
{
    char buf[96], line[120];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    simplelist_addline("%s", buf);
    snprintf(line, sizeof(line), "stage 6 summary: %s", buf);
    s6_log(line);
}

static void s6_result(const char *name, const struct s6_pass *p)
{
    const struct vtrace_sum *sum = &p->sum;
    const struct vplayer_info *vi = &p->vi;
    uint32_t step = vi->sd[0] >> 12 & 0xF;

    s6_say("%s %s: %lu shown, %lu skipped", name,
           sum->pass ? "PASS" : "FAIL", (unsigned long)sum->shown,
           (unsigned long)sum->skipped);
    s6_say(" A/V max %ld ms; hitches %lu", (long)(sum->drift_max_us / 1000),
           (unsigned long)sum->hitches);
    s6_say(" Chip %lu ms a frame, max %lu; %lu MHz",
           (unsigned long)(sum->work_mean_us / 1000),
           (unsigned long)(sum->work_max_us / 1000),
           (unsigned long)sum->mhz_hi);
    if (!p->have_vi)
        s6_say(" No report from the player");
    else
    {
        s6_say(" SDRAM %s; frames in %s",
               vi->sd_n ? (step < 5 ? s6_steps[step] : "?") : "no report",
               s6_where(vi->dpb));
        if (vi->sdc_done)
            s6_say(" Readback %lu/%lu bad over %lu KB%s",
                   (unsigned long)vi->sdc_bad, (unsigned long)vi->sdc_words,
                   (unsigned long)(vi->sdc_span >> 10),
                   vi->sdc_port_err ? ", port failed" : "");
        if (vi->sd[4])
            s6_say(" Memory %lu KB; vector ld %s st %s",
                   (unsigned long)((vi->sd[4] >> 16 & 0xFF) * 64),
                   vi->sd[4] & 1 ? "ok" : "bad",
                   vi->sd[4] & 2 ? "ok" : "bad");
        if (vi->pm_losses)
            s6_say(" Lost x%lu: %s", (unsigned long)vi->pm_losses,
                   vi->pm_n ? s6_phase(vi->pm[0]) : "port dead");
        if (vi->clock_dropped)
            s6_say(" Fell back to 27 MHz");
        s6_say(" Port waits %lu/%lu/%lu us; %lu timeouts",
               (unsigned long)vi->port[0], (unsigned long)vi->port[1],
               (unsigned long)vi->port[2], (unsigned long)vi->port[3]);
        if (vi->vfy_words)
            s6_say(" Packet readback %lu/%lu bad",
                   (unsigned long)vi->vfy_bad, (unsigned long)vi->vfy_words);
        if (vi->stalls)
            s6_say(" Stalls ridden out %lu, max %lu ms",
                   (unsigned long)vi->stalls,
                   (unsigned long)(vi->stall_max_us / 1000));
        if (vi->guard_n == 48 && vi->guard[0] == 0x47554152)
        {
            if ((vi->guard[16] >> 16) == 0x5354)
            {
                s6_say(" Self-test: %s %04lx",
                       vi->guard[16] & 0xFFFF ? "wrong" : "all right",
                       (unsigned long)(vi->guard[16] & 0xFFFF));
                if (vi->guard[21])
                    s6_say(" Low SRAM hit %04lx at %03lx",
                           (unsigned long)vi->guard[21],
                           (unsigned long)vi->guard[22]);
            }
            if (vi->guard[1])
                s6_say(" Guard hit: %s, %lu pictures",
                       s6_phase(vi->guard[1] & 0x7FFFFFFF),
                       (unsigned long)vi->guard[13]);
            else
                s6_say(" Guard: header intact");
            s6_say(" Planes %05lx %05lx %05lx",
                   (unsigned long)vi->guard[8], (unsigned long)vi->guard[9],
                   (unsigned long)vi->guard[10]);
        }
        else
            s6_say(" Guard not read");
    }
    if (!p->ended)
        s6_say(" Stopped before the end");
}

/* The passes (dev28, owner: measure smoothness on what ships): clip 1 on
 * the vectors with the self-test and the dumps, clip 1 again as the
 * player runs it, then clip 2 (real footage) the same.  The kernel A/B
 * passes of dev14-27 are gone: every kernel but the luma loop filter is
 * on the vectors and right on silicon (session 25). */
#define S6_PASSES 3
static const struct
{
    const char *name, *what;
    bool clip2;
    uint32_t mode, kernels;     /* VPLAYER_PORT_*, HP_OPEN_C_* */
} s6_passes[S6_PASSES] =
{
    { "1 Clip 1, checked", "clip 1, with the self-test and dumps", false, 0,
      HP_OPEN_SELFTEST | HP_OPEN_DUMP },
    { "2 Clip 1", "clip 1, as the player runs it", false, 0, 0 },
    { "3 Clip 2", "real footage, as the player runs it", true, 0, 0 },
};

bool devbcm_test6(void)
{
    struct simplelist_info info;
    struct s6_pass *pass;
    struct vtrace_rec *rec;
    const char *why = "";
    int h, hp, k;
    bool two, any1 = false, ok2 = true;

    simplelist_info_init(&info, "BCM stage 6", 0, NULL);
    simplelist_reset_lines();
    if (!bcm_devtest_6_allowed(&why))
    {
        simplelist_addline("Not available: %s", why);
        simplelist_addline("Run stages 0-5 first (5 must pass)");
        return show(&info);
    }
    if (!file_exists(STAGE6_CLIP) || !file_exists(VPLAYER_IMAGE))
    {
        simplelist_addline("Not available: no %s",
                           file_exists(STAGE6_CLIP) ? "video image" : "test clip");
        simplelist_addline("The install puts both in the system folder");
        return show(&info);
    }
    two = file_exists(STAGE6_CLIP2);
    simplelist_addline("Plays clip 1 twice, then clip 2.");
    simplelist_addline("Watch for stutter, especially");
    simplelist_addline("in the first second.");
    simplelist_addline("Everything goes in the log.");
    simplelist_addline("MENU: cancel");
    confirm_line = 5;
    simplelist_addline("Run stage 6 now (SELECT here)");
    confirm_4b = false;
    info.action_callback = confirm_cb;
    simplelist_show_list(&info);
    if (!confirm_4b)
        return false;

    /* the trace's records, and the player's blocks, before the audio's
       buffer (vplayer.h) */
    audio_stop();
    h = core_alloc(STAGE6_RECORDS * sizeof(struct vtrace_rec));
    if (h <= 0)
        return false;
    core_pin(h);
    hp = core_alloc(S6_PASSES * sizeof(struct s6_pass));
    if (hp <= 0)
    {
        core_free(h);
        return false;
    }
    core_pin(hp);
    rec = core_get_data(h);
    pass = core_get_data(hp);
    s6_t0 = USEC_TIMER;
    s6_log("=== BCM test, stage 6 (play a clip) ===");
    {
        /* the last run's dumps go, so a missing one means "not written
           this run" (owner); a header to match a pull to the install */
        static const char *const dumps[] =
        {
            "pic1.yuv", "pic2.yuv", "selftest1.bin", "selftest2.bin",
            "vrf1.bin", "vrf2.bin",
        };
        static int runs;
        char line[160];
        unsigned d;

        for (d = 0; d < ARRAYLEN(dumps); d++)
        {
            snprintf(line, sizeof(line), ROCKBOX_DIR "/bcmtest/%s", dumps[d]);
            remove(line);
        }
        snprintf(line, sizeof(line), "stage 6 run %d since boot: build %s, "
                 "video image (" BCMVIDEO_IMAGE_KIND ") %lu bytes fnv %08lx",
                 ++runs, rbversion, (unsigned long)BCMVIDEO_IMAGE_BYTES,
                 (unsigned long)BCMVIDEO_IMAGE_FNV);
        s6_log(line);
    }
    for (k = 0; k < S6_PASSES; k++)
    {
        memset(&pass[k], 0, sizeof(pass[k]));
        if (s6_passes[k].clip2 && !two)
            continue;
        s6_play(k + 1, s6_passes[k].clip2 ? STAGE6_CLIP2 : STAGE6_CLIP,
                s6_passes[k].what, s6_passes[k].mode, s6_passes[k].kernels,
                rec, &pass[k]);
        if (s6_passes[k].clip2)
            ok2 = pass[k].sum.pass;
        else
            any1 |= pass[k].sum.pass;
    }
    s6_log(any1 && ok2 ? "=== end: stage 6 played ==="
                       : "=== end: stage 6 failed ===");

    simplelist_info_init(&info, "BCM stage 6", 0, NULL);
    simplelist_reset_lines();
    s6_say("Stage 6 %s", any1 && ok2 ? "PASS" : "FAIL");
    for (k = 0; k < S6_PASSES; k++)
        if (!s6_passes[k].clip2 || two)
            s6_result(s6_passes[k].name, &pass[k]);
    core_free(hp);
    core_free(h);
    return show(&info);
}

/* The pace row (bench "vfeed", P19 vplayer health): a demanding clip
 * (stage 6's real footage, clip2.mp4) and a clip in the recommended
 * conversion profile (stage7.m4v, prokbox-convert.py's output) through
 * the player as a normal play runs it (no dumps, no C kernels).  Per
 * clip: the FEED p95 and max against its own frame time, skips, A/V and
 * the clock.  A clip passes when stage 6's summary passes (A/V within
 * VTRACE_DRIFT_US after VTRACE_SETTLE_MS, nothing skipped or dropped),
 * its FEED p95 is under VTRACE_PACE_PCT % of its frame and every record
 * ran at 150 MHz.  A missing clip is "Not available", and the screen
 * says first how many are missing (sitting 3 played one of two and only
 * the log said so). */
#define VFEED_CLIPS 2
static bool with_cpu_codec(bool (*stage)(void));
static void vfeed_clip(const char *path, const char *name,
                       struct vtrace_rec *rec, struct s6_pass *p)
{
    char line[160];
    uint32_t p95, fr;
    bool ok;

    if (!file_exists(path))
    {
        snprintf(line, sizeof(line), "vfeed %s: Not available (no clip)", name);
        s6_log(line);
        return;
    }
    s6_play(0, path, name, 0, 0, rec, p);
    p95 = vtrace_work_pct(&s6_trace, 95);
    fr = vtrace_frame_us(&s6_trace);
    ok = vtrace_pace_ok(&p->sum, p95, fr, 150);
    snprintf(line, sizeof(line), "vfeed %s: %s; FEED p95 %lu us, max %lu us, "
             "mean %lu us; frame %lu us (p95 %lu%%); %lu skipped, %lu GOPs "
             "dropped; A/V max %ld us; %lu-%lu MHz", name, ok ? "PASS" : "FAIL",
             (unsigned long)p95, (unsigned long)p->sum.work_max_us,
             (unsigned long)p->sum.work_mean_us, (unsigned long)fr,
             (unsigned long)(fr ? (uint64_t)p95 * 100 / fr : 0),
             (unsigned long)p->sum.skipped, (unsigned long)p->sum.dropped_gops,
             (long)p->sum.drift_max_us, (unsigned long)p->sum.mhz_lo,
             (unsigned long)p->sum.mhz_hi);
    s6_log(line);
    simplelist_addline("%s: %s, p95 %lu of %lu ms", name, ok ? "PASS" : "FAIL",
                       (unsigned long)(p95 / 1000), (unsigned long)(fr / 1000));
}

static bool devbcm_vfeed_run(void)
{
    static const char *const paths[VFEED_CLIPS] =
        { STAGE6_CLIP2, ROCKBOX_DIR "/bcmtest/stage7.m4v" };
    static const char *const names[VFEED_CLIPS] =
        { "demanding (clip2.mp4)", "converted (stage7.m4v)" };
    struct simplelist_info info;
    struct s6_pass *pass;
    struct vtrace_rec *rec;
    const char *why;
    int h, hp, k, missing;

    simplelist_info_init(&info, "Video pace", 0, NULL);
    simplelist_reset_lines();
    if (!bcm_devtest_6_allowed(&why) || !file_exists(VPLAYER_IMAGE))
    {
        s6_log("vfeed: Not available (stage 5 flag or video image missing)");
        return false;
    }
    audio_stop();
    h = core_alloc(STAGE6_RECORDS * sizeof(struct vtrace_rec));
    if (h <= 0)
        return false;
    core_pin(h);
    hp = core_alloc(sizeof(struct s6_pass));
    if (hp <= 0)
    {
        core_free(h);
        return false;
    }
    core_pin(hp);
    rec = core_get_data(h);
    pass = core_get_data(hp);
    s6_t0 = USEC_TIMER;
    s6_log("=== video pace (vfeed) ===");
    for (k = 0, missing = 0; k < VFEED_CLIPS; k++)
        missing += !file_exists(paths[k]);
    if (missing)
    {
        char line[96];

        snprintf(line, sizeof(line), "vfeed: %d of %d clips missing",
                 missing, VFEED_CLIPS);
        s6_log(line);
        simplelist_addline("%d of %d clips missing:", missing, VFEED_CLIPS);
        for (k = 0; k < VFEED_CLIPS; k++)
            if (!file_exists(paths[k]))
                simplelist_addline("  %s", names[k]);
    }
    for (k = 0; k < VFEED_CLIPS; k++)
        vfeed_clip(paths[k], names[k], rec, pass);
    s6_log("=== end: video pace ===");
    core_free(hp);
    core_free(h);
    return show(&info);
}

bool devbcm_vfeed(void) { return with_cpu_codec(devbcm_vfeed_run); }

/* Stage 7 (dev31): the Videos screen's operations on a real converted
 * file (bcmtest/stage7.m4v, prokbox-convert.py's output), scripted at the
 * player level as screens/video.c performs them: start, pause/resume,
 * the paused picture's redraw (the overlay's hide) and a scrub, PREV
 * (restart), the audio's own fast forward (a held NEXT), dark and wake
 * while paused, MENU and re-enter (resume), and the end of file.  Each
 * step's pictures, skips and drift are kept in memory and written to
 * bcmtest.log after the run, with the per-picture trace; what a script
 * can't judge (look, sound, a 10-minute run) stays on the owner's list
 * (device-session-1.md, "Session 28"). */
#define STAGE7_FILE     ROCKBOX_DIR "/bcmtest/stage7.m4v"
#define S7_STEPS        20

struct s7_step
{
    const char *what;
    uint32_t t_ms, el_start, el_end;    /* when, and the audio's elapsed */
    uint32_t rec0, rec1;                /* its V6 records [rec0, rec1) */
    uint32_t skipped, gops, late;       /* the engine's totals at its end */
    uint32_t underruns;                 /* the PCM ran dry during it (dev33) */
    uint32_t want_ms;                   /* a seek's target: the audio must
                                           be at or past it at the end; 0
                                           none */
    int state;                          /* vpc_state at its end */
};

static struct s7_step *s7;
static int s7_n;
static long s7_t0;

static uint32_t s7_elapsed(void)
{
    struct mp3entry *id3 = audio_current_track();

    return id3 ? id3->elapsed : 0;
}

static void s7_begin(const char *what)
{
    struct s7_step *st;

    if (s7_n >= S7_STEPS)
        return;
    st = &s7[s7_n];
    memset(st, 0, sizeof(*st));
    st->what = what;
    st->t_ms = (USEC_TIMER - s7_t0) / 1000;
    st->el_start = s7_elapsed();
    st->rec0 = s6_trace.n;
    st->underruns = pcmbuf_underruns();
}

static void s7_end(void)
{
    struct s7_step *st = &s7[s7_n];
    struct vplayer_info vi;

    if (s7_n >= S7_STEPS)
        return;
    st->el_end = s7_elapsed();
    st->rec1 = s6_trace.n;
    st->underruns = pcmbuf_underruns() - st->underruns;
    st->state = vplayer_info(&vi) ? vi.state : -1;
    st->skipped = s6_trace.last.skipped;
    st->gops = s6_trace.last.dropped_gops;
    st->late = s6_trace.last.late_posts;
    s7_n++;
}

/* waits ms in 100 ms steps; false if the player failed meanwhile */
static bool s7_wait(int ms)
{
    struct vplayer_info vi;

    for (; ms > 0; ms -= 100)
    {
        vplayer_pump();
        sleep(HZ / 10);
        if (vplayer_info(&vi) && vi.state == VPC_FAILED)
            return false;
    }
    return true;
}

/* Until the soundtrack really plays: the PCM channel on and elapsed
 * moved past where it started (session 36c: steps whose audio never ran
 * read as the video's failures).  Logged either way; false after
 * S7_AUDIO_MS. */
#define S7_AUDIO_MS 4000
static bool s7_audio_runs(unsigned long from)
{
    long t0 = current_tick;
    char line[120];

    while (TIME_BEFORE(current_tick, t0 + S7_AUDIO_MS * HZ / 1000))
    {
        struct mp3entry *id3 = audio_current_track();

        if (id3 && (audio_status() & AUDIO_STATUS_PLAY) &&
            mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) == CHANNEL_PLAYING &&
            id3->elapsed > from + 100)
        {
            snprintf(line, sizeof(line), "stage 7: audio runs %ld ms after "
                     "the start (elapsed %lu)",
                     (long)(current_tick - t0) * 1000 / HZ,
                     (unsigned long)id3->elapsed);
            s6_log(line);
            return true;
        }
        sleep(HZ / 50);
    }
    snprintf(line, sizeof(line), "stage 7: audio did not run in %d ms "
             "(status %d, PCM %d)", S7_AUDIO_MS, (int)audio_status(),
             (int)mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK));
    s6_log(line);
    return false;
}

static void s7_resume(void)
{
    struct mp3entry *id3 = audio_current_track();

    audio_resume();
    s7_audio_runs(id3 ? id3->elapsed : 0);
}

static bool s7_start(unsigned long elapsed)
{
    if (!vplayer_start(STAGE7_FILE))
        return false;
    playlist_create(NULL, NULL);
    playlist_insert_track(NULL, STAGE7_FILE, PLAYLIST_INSERT_LAST, false,
                          true);
    playlist_start(0, elapsed, 0);
    if (elapsed)
        vplayer_seek(elapsed);          /* as ui_video_play resumes */
    s7_audio_runs(elapsed);
    return true;
}

static void s7_seek(unsigned long ms, bool tell_player)
{
    audio_pre_ff_rewind();
    audio_ff_rewind(ms);
    if (tell_player)
        vplayer_seek(ms);               /* the shell's seeks announce it */
}

/* the steps' verdict: pictures moved, the player alive, and after each
 * step's first 15 pictures every one within VTRACE_DRIFT_US.  A session
 * that starts again inside the step (its shown count drops: sitting 4c,
 * the audio's elapsed jumped to 0 and back 1.6 s into the start and the
 * player sought again) leaves a record with no picture yet (shown 0,
 * PTS 0) whose "drift" is the audio clock itself (1764 ms): never a
 * picture's, so not counted, and the new session settles as a play's
 * start does (vtrace.h VTRACE_SETTLE_MS from its first picture) */
static int32_t s7_drift_max(const struct s7_step *st)
{
    int32_t m = 0;
    uint32_t k, settle_to = 0, prev = 0;
    bool settling = false;

    for (k = st->rec0; k < st->rec1 && k < s6_trace.cap; k++)
    {
        const struct vtrace_rec *r = &s6_trace.rec[k];
        int32_t d = r->drift_us;

        if (k > st->rec0 && r->shown < prev)
            settling = true;            /* a new session: no picture yet */
        prev = r->shown;
        if (!r->shown)
            continue;
        if (settling)
        {
            settling = false;
            settle_to = r->pts_ms + VTRACE_SETTLE_MS;
        }
        if (k < st->rec0 + 15 || r->pts_ms < settle_to)
            continue;
        if (d < 0)
            d = -d;
        if (d > m)
            m = d;
    }
    return m;
}

/* A seek's target inside the clip: want ahead, at most a quarter of the
 * clip, leaving S7_TAIL_MS to play; else as far back (sitting 4c: "scrub
 * +30 s" from 9 s of the 40 s stage7.m4v landed at 39 s, the clip ended
 * and the next four steps had nothing playing) */
#define S7_TAIL_MS  8000
static uint32_t s7_ahead(uint32_t from, uint32_t want, uint32_t len)
{
    uint32_t step = want < len / 4 ? want : len / 4;

    if (from + step + S7_TAIL_MS <= len)
        return from + step;
    return from >= step ? from - step : 0;
}

/* ---- Stage 8 (dev39): the BCM's audio pins ---------------------------
 *
 * 8a-0 listens only: our image samples GPIO 1-3 (Apple's audio pins,
 * docs/bcm2722-reference/audio.md) as inputs, once with the audio stopped
 * and once while the PP plays the test video's soundtrack, and counts
 * their edges.  A frame clock (2 x fs edges a second) or a bit clock on
 * them says the pins share the WM8758's audio bus.  Nothing is driven:
 * the tone (8a-1) waits for this result and the owner's go. */
#define S8_MASK     0xE         /* GPIO 1, 2, 3 */
#define S8_US       500000

static void s8_line(const char *what, int rc, const uint32_t r[8])
{
    char line[200];
    uint32_t k, ms = r[1] / 1000 ? r[1] / 1000 : 1;

    if (rc)
    {
        snprintf(line, sizeof(line), "stage 8a-0 %s: probe failed (%d)", what,
                 rc);
        s6_log(line);
        simplelist_addline("%s: probe failed (%d)", what, rc);
        return;
    }
    snprintf(line, sizeof(line), "stage 8a-0 %s: %lu samples in %lu us "
             "(%lu k/s); edges/s GPIO1 %lu, GPIO2 %lu, GPIO3 %lu; "
             "always high %lx, ever high %lx", what, (unsigned long)r[0],
             (unsigned long)r[1], (unsigned long)(r[0] / ms),
             (unsigned long)(r[2] * 1000 / ms),
             (unsigned long)(r[3] * 1000 / ms),
             (unsigned long)(r[4] * 1000 / ms), (unsigned long)r[6],
             (unsigned long)r[7]);
    s6_log(line);
    simplelist_addline("%s (%lu k samples/s):", what,
                       (unsigned long)(r[0] / ms));
    for (k = 0; k < 3; k++)
        simplelist_addline("  GPIO%lu: %lu edges/s%s", (unsigned long)(k + 1),
                           (unsigned long)(r[2 + k] * 1000 / ms),
                           r[2 + k] ? "" : (r[6] & (2u << k)) ? " (high)"
                                                             : " (low)");
}

/* 8a-0's verdict this boot, the only key to 8a-1: GPIO 1 floated in all
 * three states (8a-0b) AND the "playing" probe saw the codec's clocks on
 * GPIO 2 and 3 (session 36c: two of three runs saw none, the audio not
 * running, so their "PP playing" float result proved nothing) */
static bool s8_free_all;

/* The PP's audio really running: PCM out and the elapsed time moving, up
 * to ms (session 36c: with the COP decode switch on, "playing" was
 * sampled before any PCM).  True once it is. */
/* 8a-1: silence on the PP's own PCM for the tones, so the codec's bit
 * and frame clocks surely run (data-paths.md R9.6: the WM8758 makes them
 * only while its PLL is up, and clocks without PCM are unproven; after a
 * stop it parks both high, S34).  Half a KB of zeros (const, no RAM),
 * played over and over from the mixer's callback. */
static const uint32_t s8_zeros[128];

static void s8_silence_more(const void **start, size_t *size)
{
    *start = s8_zeros;
    *size = sizeof(s8_zeros);
}

static void s8_silence(bool on)
{
    static const struct mixer_play_cbs cbs = { s8_silence_more, NULL };

    if (on)
        mixer_channel_play_data(PCM_MIXER_CHAN_PLAYBACK, &cbs, s8_zeros,
                                sizeof(s8_zeros));
    else
        mixer_channel_stop(PCM_MIXER_CHAN_PLAYBACK);
}

static bool s8_wait_pcm(int ms)
{
    long end = current_tick + ms * HZ / 1000;
    unsigned long el0 = 0;
    bool seen = false;

    while (TIME_BEFORE(current_tick, end))
    {
        struct mp3entry *id3 = audio_current_track();

        if (mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK) == CHANNEL_PLAYING &&
            id3 &&
            (audio_status() & AUDIO_STATUS_PLAY))
        {
            if (!seen)
            {
                seen = true;
                el0 = id3->elapsed;
            }
            else if (id3->elapsed > el0 + 200)
                return true;
        }
        vplayer_pump();
        sleep(HZ / 20);
    }
    return false;
}

/* 8a-0b (dev40): does GPIO 1 float?  Its level with the BCM's pull off,
 * down and up (inputs; a weak pull drives nothing), with the PP playing,
 * with its transmitter stopped, and with the audio off.  A pin that follows
 * the pulls floats: driving it can't fight the PP.  One held at a level is
 * driven by something: stop (audio-spike.md section 8). */
/* bit st set: free in that state (0 playing / audio off, 1 TX stopped) */
static int s8_float(bool playing)
{
    static const char *const pulls[3] = { "off", "down", "up" };
    char line[200];
    int st, p, freed = 0;

    for (st = 0; st < (playing ? 2 : 1); st++)
    {
        const char *what = !playing ? "audio off" :
                           st ? "PP TX stopped" : "PP playing";
        bool follows = true;
        uint32_t lev[3] = { 0, 0, 0 };

        if (playing && st)
            bcm_devtest_iis_tx(false);
        for (p = 0; p < 3; p++)
        {
            uint32_t r[8];
            int rc = vplayer_probe_pins(0x2, 100000, p, r);

            if (rc)
            {
                follows = false;
                snprintf(line, sizeof(line), "stage 8a-0b %s, pull %s: probe "
                         "failed (%d)", what, pulls[p], rc);
                s6_log(line);
                continue;
            }
            /* 0 always low, 1 always high, 2 both */
            lev[p] = (r[6] & 2) ? 1 : (r[7] & 2) ? 2 : 0;
            snprintf(line, sizeof(line), "stage 8a-0b %s, pull %s: GPIO1 %s, "
                     "%lu edges", what, pulls[p],
                     lev[p] == 1 ? "high" : lev[p] == 2 ? "moving" : "low",
                     (unsigned long)r[2]);
            s6_log(line);
        }
        if (playing && st)
            bcm_devtest_iis_tx(true);
        follows = follows && lev[1] == 0 && lev[2] == 1;
        snprintf(line, sizeof(line), "stage 8a-0b %s: GPIO1 %s", what,
                 follows ? "follows the pulls: floats" :
                 "does not follow the pulls: driven or moving, do not drive");
        s6_log(line);
        freed |= follows << st;
    }
    return freed;
}

bool devbcm_test8(void)
{
    struct simplelist_info info;
    uint32_t quiet[8], playing[8];
    int rq, rp, repeat, fp, fq;
    bool pcm_ok, clocks;
    char line[160];

    simplelist_info_init(&info, "BCM stage 8", 0, NULL);
    simplelist_reset_lines();
    if (!file_exists(STAGE7_FILE) || !file_exists(VPLAYER_IMAGE))
    {
        simplelist_addline("Not available: no %s",
                           file_exists(VPLAYER_IMAGE) ? "test video"
                                                      : "video image");
        return show(&info);
    }
    simplelist_addline("8a-0: listens to the video");
    simplelist_addline("chip's audio pins (nothing");
    simplelist_addline("is driven), quiet, then while");
    simplelist_addline("a soundtrack plays (~5 s).");
    simplelist_addline("MENU: cancel");
    confirm_line = 5;
    simplelist_addline("Run stage 8a-0 now (SELECT here)");
    confirm_4b = false;
    info.action_callback = confirm_cb;
    simplelist_show_list(&info);
    if (!confirm_4b)
        return false;

    s6_t0 = USEC_TIMER;
    snprintf(line, sizeof(line), "=== BCM test, stage 8a-0 (the audio pins, "
             "listening): build %s ===", rbversion);
    s6_log(line);
    audio_stop();
    sleep(HZ / 2);
    rq = vplayer_probe_pins(S8_MASK, S8_US, 0, quiet);

    repeat = global_settings.repeat_mode;
    global_settings.repeat_mode = REPEAT_OFF;
    playlist_create(NULL, NULL);
    playlist_insert_track(NULL, STAGE7_FILE, PLAYLIST_INSERT_LAST, false,
                          true);
    playlist_start(0, 0, 0);
    pcm_ok = s8_wait_pcm(8000);         /* the PCM really running */
    snprintf(line, sizeof(line), "stage 8a-0: PCM %s before the playing "
             "probe", pcm_ok ? "running" : "NOT running after 8 s");
    s6_log(line);
    sleep(HZ / 2);
    rp = vplayer_probe_pins(S8_MASK, S8_US, 0, playing);
    fp = s8_float(true);                /* 8a-0b: playing, then TX stopped */
    audio_stop();
    fq = s8_float(false);               /* and with the audio off */
    global_settings.repeat_mode = repeat;

    simplelist_info_init(&info, "BCM stage 8a-0", 0, NULL);
    simplelist_reset_lines();
    s8_line("quiet", rq, quiet);
    s8_line("playing", rp, playing);
    /* the clocks: GPIO 2 and 3 moving while playing, still when quiet */
    clocks = rq == 0 && rp == 0 && pcm_ok &&
             playing[3] > quiet[3] + 100 && playing[4] > quiet[4] + 100;
    s8_free_all = clocks && (fq & 1) && fp == 3;
    snprintf(line, sizeof(line), "stage 8a-0: codec clocks seen while "
             "playing: %s; 8a-1 %s", clocks ? "yes" : "no",
             s8_free_all ? "unlocked" : "stays locked");
    s6_log(line);
    simplelist_addline("Clocks while playing: %s", clocks ? "yes" : "NO");
    simplelist_addline("8a-1: %s", s8_free_all ? "unlocked" : "locked");
    simplelist_addline("8a-0b, does GPIO1 float?");
    simplelist_addline("  audio off: %s", fq & 1 ? "floats" : "NOT free");
    simplelist_addline("  PP playing: %s", fp & 1 ? "floats" : "NOT free");
    simplelist_addline("  PP TX stopped: %s", fp & 2 ? "floats" : "NOT free");
    if (!pcm_ok)
    {
        /* no PCM, no codec clocks: the pins can't be judged (session
           36c: "not on its bus" was a soundtrack that never started) */
        s6_log("stage 8a-0: no PCM ran, so the pins can't be judged");
        simplelist_addline("No PCM ran: the pins");
        simplelist_addline("can't be judged. Run again.");
    }
    else if (!rq && !rp)
    {
        uint32_t k, any = 0;

        for (k = 0; k < 3; k++)
            any |= playing[2 + k] > quiet[2 + k] + 100;
        snprintf(line, sizeof(line), "stage 8a-0: %s", any ?
                 "the pins move with the PP's audio: they share its bus" :
                 "no activity with the PP's audio: not on its bus (or "
                 "faster than we sample)");
        s6_log(line);
        simplelist_addline("%s", any ? "Pins move with the audio:"
                                     : "No activity with the audio.");
        simplelist_addline("%s", any ? "they share its bus."
                                     : "(or too fast to sample)");
    }
    return show(&info);
}

/* 8b: a 440 Hz tone through the chip and back (vplayer_set_audio_loop),
 * over the test video's soundtrack while the video plays, at the chip's
 * full clock and then at 27 MHz.  The owner listens for a steady tone; the
 * player's log has the blocks, underruns and per-block costs. */
bool devbcm_test8b(void)
{
    static const struct { uint32_t t; const char *what; } run[] =
    {
        /* the CPU's copy (dev39a: session 32's DMA feed was corrupt) */
        { VPLAYER_TUNE_BOOST, "chip 150 MHz" },
        { VPLAYER_TUNE_BOOST | VPLAYER_TUNE_CHIP27, "chip 27 MHz" },
    };
    struct simplelist_info info;
    uint32_t keep = vplayer_get_tuning();
    int k, repeat;
    char line[160];

    simplelist_info_init(&info, "BCM stage 8b", 0, NULL);
    simplelist_reset_lines();
    if (!file_exists(STAGE7_FILE) || !file_exists(VPLAYER_IMAGE))
    {
        simplelist_addline("Not available: no %s",
                           file_exists(VPLAYER_IMAGE) ? "test video"
                                                      : "video image");
        return show(&info);
    }
    simplelist_addline("8b: a 440 Hz tone goes through");
    simplelist_addline("the video chip and back, over");
    simplelist_addline("the soundtrack, twice (~25 s).");
    simplelist_addline("Listen: is the tone steady?");
    simplelist_addline("MENU: cancel");
    confirm_line = 5;
    simplelist_addline("Run stage 8b now (SELECT here)");
    confirm_4b = false;
    info.action_callback = confirm_cb;
    simplelist_show_list(&info);
    if (!confirm_4b)
        return false;

    s6_t0 = USEC_TIMER;
    snprintf(line, sizeof(line), "=== BCM test, stage 8b (PCM through the "
             "chip and back): build %s ===", rbversion);
    s6_log(line);
    repeat = global_settings.repeat_mode;
    global_settings.repeat_mode = REPEAT_OFF;
    for (k = 0; k < (int)ARRAYLEN(run); k++)
    {
        audio_stop();
        vplayer_set_tuning(run[k].t);
        vplayer_set_audio_loop(0x8000);         /* half gain: the chip's */
        snprintf(line, sizeof(line), "stage 8b run %d: %s", k + 1,
                 run[k].what);
        s6_log(line);
        if (s7_start(30000))
            s7_wait(10000);
        vplayer_stop();                         /* its loop lines: the log */
        audio_stop();
        vplayer_set_audio_loop(0);
    }
    vplayer_set_tuning(keep);
    global_settings.repeat_mode = repeat;

    simplelist_info_init(&info, "BCM stage 8b", 0, NULL);
    simplelist_reset_lines();
    simplelist_addline("Done. Tell us:");
    simplelist_addline("1) at 150 MHz: steady tone?");
    simplelist_addline("2) at 27 MHz: steady tone?");
    simplelist_addline("3) any picture stutter?");
    simplelist_addline("The costs are in the log.");
    return show(&info);
}

/* The DMA lab (dev40): the PP's DMA into the chip's SDRAM by block size,
 * each block read back, with and without the word-0 rewrite (sessions
 * 32-33: every DMA block lost its first word) */
static void lab_line(const char *line)
{
    s6_log(line);
    simplelist_addline("%s", line + 9);     /* past "dma lab: " */
}

bool devbcm_dmalab(void)
{
    struct simplelist_info info;
    char line[120];
    int rc;

    simplelist_info_init(&info, "BCM DMA lab", 0, NULL);
    simplelist_reset_lines();
    if (!file_exists(VPLAYER_IMAGE))
    {
        simplelist_addline("Not available: no video image");
        return show(&info);
    }
    audio_stop();
    s6_t0 = USEC_TIMER;
    snprintf(line, sizeof(line), "=== BCM test, DMA lab: build %s ===",
             rbversion);
    s6_log(line);
    rc = vplayer_dma_lab(lab_line);
    if (rc)
    {
        snprintf(line, sizeof(line), "dma lab: failed (%d)", rc);
        lab_line(line);
    }
    return show(&info);
}

/* Bench row B13, the warm swap (warm-swap.md section 7): the display-only
 * image and the video image swapped 20 times with the panel kept lit,
 * then one swap without its go word and the cold restart after it.  On a
 * stage 5 flag (the display-only image is not in verified.txt). */
static void warm_line(const char *line)
{
    s6_log(line);
}

bool devbcm_warmswap(void)
{
    struct simplelist_info info;
    struct vplayer_warm_result r;
    const char *why = "";
    char line[120];
    int rc;

    simplelist_info_init(&info, "BCM warm swap", 0, NULL);
    simplelist_reset_lines();
    if (!file_exists(VPLAYER_IMAGE))
    {
        simplelist_addline("Not available: no video image");
        return show(&info);
    }
    if (!bcm_devtest_5i_allowed(&why))
    {
        simplelist_addline("Not available: %s", why);
        simplelist_addline("Run stages 0-5 first (5 must pass)");
        return show(&info);
    }
    if (!bench_batch)       /* the bench session asked already */
    {
        simplelist_addline("Optional, under 1 min: the video");
        simplelist_addline("chip changes program up to %d times", VPLAYER_WARM_ROUNDS);
        simplelist_addline("(stops at a failure), screen on.");
        simplelist_addline("No blink, blank or flicker allowed.");
        simplelist_addline("Then one dark restart (expected).");
        simplelist_addline("MENU: cancel");
        confirm_line = 6;
        simplelist_addline("Run the warm swap now (SELECT here)");
        confirm_4b = false;
        info.action_callback = confirm_cb;
        simplelist_show_list(&info);
        if (!confirm_4b)
            return false;
    }
    simplelist_info_init(&info, "BCM warm swap", 0, NULL);
    simplelist_reset_lines();
    audio_stop();
    s6_t0 = USEC_TIMER;
    snprintf(line, sizeof(line), "=== BCM test, warm swap (B13): build %s ===",
             rbversion);
    s6_log(line);
    rc = vplayer_warm_lab(bcmdt_jobs_display_image.data,
                          bcmdt_jobs_display_image.size, warm_line, &r);
    simplelist_addline("Warm swap: %s", rc == 0 && r.first_fail < 0 &&
                       r.ok == VPLAYER_WARM_ROUNDS ? "PASS" : "FAIL");
    if (rc)
        simplelist_addline("Could not start (%d)", rc);
    simplelist_addline("Swaps: %lu of %lu passed", (unsigned long)r.ok,
                       (unsigned long)r.rounds);
    if (r.first_fail >= 0)
        simplelist_addline("First failure: round %d, step %d",
                           r.first_fail, r.first_why);
    simplelist_addline("Park: %lu us max", (unsigned long)r.park_us_max);
    simplelist_addline("Upload: %lu us CPU, %lu us E2",
                       (unsigned long)r.up_us_max[0],
                       (unsigned long)r.up_us_max[1]);
    simplelist_addline("Go to ready: %lu us max",
                       (unsigned long)r.ready_us_max);
    simplelist_addline("Swap into video: %lu / %lu us",
                       (unsigned long)r.swap_us_max[0],
                       (unsigned long)r.swap_us_max[1]);
    simplelist_addline("Panel off after a swap: %lu",
                       (unsigned long)r.panel_off);
    if (r.first_fail >= 0 && r.first_fail < VPLAYER_WARM_ROUNDS)
        simplelist_addline("No go word: not run (a swap failed)");
    else
        simplelist_addline("No go word: %s; cold %lu ms",
                           r.no_go_ready ? "STARTED" : "nothing started",
                           (unsigned long)r.cold_us / 1000);
    return show(&info);
}

#ifdef HAVE_BCM_JOBS
#include "bcmjob_pp.h"
/* The job driver's device card (bench-p19.md): its self-test, a FILL and
 * a FENCE through the ring checked against the CPU path; the result is
 * in the log as "bcmjob selftest ok|failed rc=" */
bool devbcm_jobselftest(void)
{
    struct simplelist_info info;
    int r;

    simplelist_info_init(&info, "BCM jobs self-test", 0, NULL);
    simplelist_reset_lines();
    r = bcmjob_selftest();
    simplelist_addline("Job driver self-test: %s", r == 0 ? "PASS" :
                       r == 1 ? "not available" : "FAIL");
    if (r == 1)
        simplelist_addline("(jobs off: see the log for why)");
    else if (r < 0)
        simplelist_addline("Failed: %d", r);
    return show(&info);
}
#endif

/* Bench rows B5, B6, B7 (the player's image) and the probe rows (the
   probe image): one confirm screen each, on a stage 5 flag */
#define WIN_PUSHES_ALL 10           /* vplayer.c WIN_PUSHES, per pitch */
static bool lab_confirm_img(struct simplelist_info *info, char *title,
                            const char *const *lines, int n, bool probe)
{
    const char *why = "";
    int i;

    simplelist_info_init(info, title, 0, NULL);
    simplelist_reset_lines();
    if (!file_exists(probe ? VPLAYER_PROBE_IMAGE : VPLAYER_IMAGE))
    {
        simplelist_addline(probe ? "Not available: no probe image"
                                 : "Not available: no video image");
        show(info);
        return false;
    }
    if (!bcm_devtest_5i_allowed(&why))
    {
        simplelist_addline("Not available: %s", why);
        simplelist_addline("Run stages 0-5 first (5 must pass)");
        show(info);
        return false;
    }
    if (bench_batch)
        goto go;                    /* the bench session's confirm stands */
    for (i = 0; i < n; i++)
        simplelist_addline("%s", lines[i]);
    simplelist_addline("MENU: cancel");
    confirm_line = n + 1;
    simplelist_addline("Run it now (SELECT here)");
    confirm_4b = false;
    info->action_callback = confirm_cb;
    simplelist_show_list(info);
    if (!confirm_4b)
        return false;
go:
    simplelist_info_init(info, title, 0, NULL);
    simplelist_reset_lines();
    audio_stop();
    s6_t0 = USEC_TIMER;
    return true;
}

static bool lab_confirm(struct simplelist_info *info, char *title,
                        const char *const *lines, int n)
{
    return lab_confirm_img(info, title, lines, n, false);
}

bool devbcm_window(void)
{
    static const char *const lines[] = {
        "Optional, under 1 min: the video",
        "chip shows windows of pictures.",
        "At the end a white frame with a",
        "diagonal stays up 5 s: photograph",
        "it. It must be straight and whole." };
    struct simplelist_info info;
    struct vplayer_window_result r;
    static const int pitch[3] = { 320, 300, 400 };
    char line[100];
    int rc, k;

    if (!lab_confirm(&info, "BCM window push (B5)", lines, ARRAYLEN(lines)))
        return false;
    snprintf(line, sizeof(line), "=== BCM test, window push (B5): build %s ===",
             rbversion);
    s6_log(line);
    rc = vplayer_window_lab(bcmdt_jobs_display_image.data,
                            bcmdt_jobs_display_image.size, warm_line, &r);
    simplelist_addline("Window push: %s", rc == 0 && !r.dma_tmo &&
                       r.dma_done == 3 * WIN_PUSHES_ALL + 1 ? "PASS" : "FAIL");
    if (rc)
        simplelist_addline("Failed at step %d (%d)", r.fail_step, rc);
    if (r.fail_step == 5)       /* win_slot: ALLOC, write, COMMIT */
        simplelist_addline("The windows need COMMIT, which was "
                           "refused: nothing could be shown");
    for (k = 0; k < 3; k++)
        simplelist_addline("Pitch %d: %lu-%lu us", pitch[k],
                           (unsigned long)r.us_min[k],
                           (unsigned long)r.us_max[k]);
    simplelist_addline("By DMA: %lu, timed out: %lu",
                       (unsigned long)r.dma_done, (unsigned long)r.dma_tmo);
    return show(&info);
}

/* Bench row CR1 (open question #67): the port's writes read back, then
 * COMMIT, on the display-only image; read-only for the disk except the
 * device test's log */
bool devbcm_commit(void)
{
    static const char *const lines[] = {
        "Under 1 min, hands off: writes",
        "test patterns into the video",
        "chip, reads them back, and",
        "checks its picture slots." };
    static const char *const verdict[] = {
        "PASS", "FAIL: a region won't write", "FAIL: write path",
        "FAIL: chip-side sum", "FAIL: other", "Not run" };
    struct simplelist_info info;
    struct vplayer_commit_result r;
    char line[100];
    int rc;

    if (!lab_confirm(&info, "BCM COMMIT readback (CR1)", lines,
                     ARRAYLEN(lines)))
        return false;
    snprintf(line, sizeof(line), "=== BCM test, COMMIT readback (CR1): "
             "build %s ===", rbversion);
    s6_log(line);
    rc = vplayer_commit_lab(bcmdt_jobs_display_image.data,
                            bcmdt_jobs_display_image.size, warm_line, &r);
    simplelist_addline("COMMIT readback: %s",
                       verdict[r.verdict <= VPLAYER_CR_NOTRUN ? r.verdict
                                                              : VPLAYER_CR_OTHER]);
    if (rc)
        simplelist_addline("Could not start (%d)", rc);
    simplelist_addline("Control: %d wrong; pools %lu of %lu wrong",
                       r.control_bad, (unsigned long)r.regions_bad,
                       (unsigned long)r.regions);
    simplelist_addline("Slots: %lu of %lu pass", (unsigned long)r.pass,
                       (unsigned long)r.cases);
    simplelist_addline("Write %lu, sum %lu, other %lu",
                       (unsigned long)r.write_bad, (unsigned long)r.sum_bad,
                       (unsigned long)r.other);
    if (r.variants_ok)
        simplelist_addline("Other write forms land: %lx",
                           (unsigned long)r.variants_ok);
    return show(&info);
}

static bool mem_lab(int row)
{
    static const char *const lines6[] = {
        "Optional, about 1 min, hands",
        "off: times the video chip's",
        "reads at scattered addresses." };
    static const char *const lines7[] = {
        "Optional, about 2 min, hands",
        "off: times the video chip's",
        "memory, alone and while the",
        "iPod writes to it." };
    static const char *const mem[2] = { "SRAM", "SDRAM" };
    struct simplelist_info info;
    struct vplayer_mem_result r;
    char line[100];
    int rc;
    unsigned c, m;

    if (!lab_confirm(&info, row == 6 ? "BCM texel fetch (B6)" :
                     "BCM bandwidth (B7)", row == 6 ? lines6 : lines7,
                     row == 6 ? ARRAYLEN(lines6) : ARRAYLEN(lines7)))
        return false;
    snprintf(line, sizeof(line), "=== BCM test, %s (B%d): build %s ===",
             row == 6 ? "texel fetch" : "memory bandwidth", row, rbversion);
    s6_log(line);
    rc = vplayer_mem_lab(row, warm_line, &r);
    simplelist_addline("B%d: %s (%u clock%s)", row, rc ? "FAIL" : "PASS",
                       (unsigned)r.clocks, r.clocks == 1 ? "" : "s");
    if (rc)
        simplelist_addline("Failed at step %d (%d)", r.fail_step, rc);
    for (c = 0; c < r.clocks; c++)
        for (m = 0; m < 2; m++)
        {
            if (row == 6)
                simplelist_addline("%s %s: %lu.%lu ns a fetch",
                                   c ? "150 MHz" : "27 MHz", mem[m],
                                   (unsigned long)r.fetch_ns10[c][m] / 10,
                                   (unsigned long)r.fetch_ns10[c][m] % 10);
            else
                simplelist_addline("%s %s: %lu/%lu KB/s, %lu.%lu%% slower",
                                   c ? "150" : "27", mem[m],
                                   (unsigned long)r.load_kbs[c][m],
                                   (unsigned long)r.store_kbs[c][m],
                                   (unsigned long)r.slow_pm[c][m] / 10,
                                   (unsigned long)r.slow_pm[c][m] % 10);
        }
    return show(&info);
}

bool devbcm_fetch(void)
{
    return mem_lab(6);
}

bool devbcm_bandwidth(void)
{
    return mem_lab(7);
}

/* ---- Shared with devbcm_probes.c (the brief probes) ------------------- */
/* The probe rows (devbcm_probes.c): every one boots the probe image
   (vplayer_probe) */
bool devbcm_lab_open(struct simplelist_info *info, char *title,
                     const char *const *lines, int n)
{
    return lab_confirm_img(info, title, lines, n, true);
}

/* The same confirm for a row that needs no video image (the COP rows,
 * bench-p19.md C2/C3): lines, MENU cancels, SELECT on the last line runs;
 * the audio stops before it runs */
bool devbcm_confirm(struct simplelist_info *info, char *title,
                    const char *const *lines, int n)
{
    int i;

    if (bench_batch)
    {
        simplelist_info_init(info, title, 0, NULL);
        simplelist_reset_lines();
        audio_stop();
        s6_t0 = USEC_TIMER;
        return true;                /* the bench session's confirm stands */
    }
    simplelist_info_init(info, title, 0, NULL);
    simplelist_reset_lines();
    for (i = 0; i < n; i++)
        simplelist_addline("%s", lines[i]);
    simplelist_addline("MENU: cancel");
    confirm_line = n + 1;
    simplelist_addline("Run it now (SELECT here)");
    confirm_4b = false;
    info->action_callback = confirm_cb;
    simplelist_show_list(info);
    if (!confirm_4b)
        return false;
    simplelist_info_init(info, title, 0, NULL);
    simplelist_reset_lines();
    audio_stop();
    s6_t0 = USEC_TIMER;
    return true;
}

void devbcm_log(const char *line)
{
    s6_log(line);
}

bool devbcm_show(struct simplelist_info *info)
{
    return show(info);
}

/* The owner on cue: two lines of what to do, then SELECT on the last line
 * once it is done; MENU stops the stage (false) */
bool devbcm_cue(const char *title, const char *l1, const char *l2)
{
    struct simplelist_info info;

    simplelist_info_init(&info, (char *)title, 0, NULL);
    simplelist_reset_lines();
    simplelist_addline("%s", l1);
    simplelist_addline("%s", l2 ? l2 : "");
    simplelist_addline("MENU: end this check");
    confirm_line = 3;
    simplelist_addline("Done: go on (SELECT here)");
    confirm_4b = false;
    info.action_callback = confirm_cb;
    simplelist_show_list(&info);
    return confirm_4b;
}

/* The bench session's script screens (devbench.c): lines of text (already
 * wrapped to the screen), then MENU's line (menu NULL: none; MENU still
 * leaves) and the SELECT line, last; true for SELECT on that last line
 * (SELECT elsewhere does nothing), false for MENU */
bool devbcm_lines_go(const char *title, const char *const *lines, int n,
                     const char *menu, const char *go)
{
    struct simplelist_info info;
    int i;

    simplelist_info_init(&info, (char *)title, 0, NULL);
    simplelist_reset_lines();
    for (i = 0; i < n; i++)
        simplelist_addline("%s", lines[i]);
    if (menu)
        simplelist_addline("%s", menu);
    confirm_line = menu ? n + 1 : n;
    simplelist_addline("%s", go);
    confirm_4b = false;
    info.action_callback = confirm_cb;
    simplelist_show_list(&info);
    return confirm_4b;
}

void devbcm_pcm_silence(bool on)
{
    s8_silence(on);
}

/* ---- Stage 9 (M4): the AAC decoder on the chip ------------------------
 *
 * The image decodes a clip's access units (bcmtest/aac9.au, made with
 * bcmfw/aac/test/mkdevclip.sh) through HP_OP_AAC; each AU's PCM comes back
 * and its hash must equal the host build's (bcmtest/aac9.ref).  First at
 * the session's full clock (150 MHz when 4b/4c passed), then at 27 MHz.
 * Before either, the 32-bit bcs/bcc probe.  Nothing is played or driven:
 * the PCM only comes back to be hashed. */
#define STAGE9_AU       ROCKBOX_DIR "/bcmtest/aac9.au"
#define STAGE9_REF      ROCKBOX_DIR "/bcmtest/aac9.ref"

static void s9_run(const char *what, bool fast, uint32_t aac_flags,
                   bool *probe_ok, bool *probe_ran)
{
    struct vplayer_aac_result r;
    char line[200];
    int rc;

    rc = vplayer_aac_test(STAGE9_AU, STAGE9_REF, fast, aac_flags, s6_log, &r);
    if (r.probe_ran)
    {
        *probe_ran = true;
        *probe_ok = *probe_ok && r.probe == r.probe_want;
        snprintf(line, sizeof(line), "stage 9 %s: bcs/bcc probe %s (taken "
                 "%08lx, model %08lx)", what,
                 r.probe == r.probe_want ? "PASS" : "FAIL",
                 (unsigned long)r.probe, (unsigned long)r.probe_want);
        s6_log(line);
    }
    snprintf(line, sizeof(line), "stage 9 %s: %s (rc %d): %lu of %lu AUs "
             "matched, first wrong %ld; %lu Hz, %lu ch; chip %lu MHz, %lu us "
             "per AU (max %lu); PCM hash %08lx, host %08lx", what,
             !rc && r.matched == r.aus_ref && r.hash == r.ref_hash ?
             "PASS" : "FAIL", rc, (unsigned long)r.matched,
             (unsigned long)r.aus_ref, (long)r.first_bad,
             (unsigned long)r.rate, (unsigned long)r.channels,
             (unsigned long)r.mhz,
             (unsigned long)(r.us_n ? r.us_total / r.us_n : 0),
             (unsigned long)r.us_max, (unsigned long)r.hash,
             (unsigned long)r.ref_hash);
    s6_log(line);
    /* dev42: what session 35 couldn't tell apart */
    snprintf(line, sizeof(line), "stage 9 %s: refused AUs %lu, AUs read back "
             "wrong %lu, OPEN code %d, tables %s, memprobe SRAM %s, SDRAM %s",
             what, (unsigned long)r.errors, (unsigned long)r.au_bad,
             r.open_code, r.tables < 0 ? "?" : r.tables ? "same" : "DIFFER",
             r.mem_sram < 0 ? "?" : r.mem_sram ? "ok" : "WRONG",
             r.mem_sdram < 0 ? "?" : r.mem_sdram ? "ok" : "WRONG");
    s6_log(line);
    /* session 36c: the second ic_stream's copy (into SDRAM by halfwords,
       unless the byte-copy run), and the AUs whose checkpoints were
       logged for bcmfw/aac/test/aac_dev_sim.py --diff */
    snprintf(line, sizeof(line), "stage 9 %s: ic_stream copies wrong %lu of "
             "%lu; %lu AUs traced", what, (unsigned long)r.copy_bad,
             (unsigned long)r.copy_checked, (unsigned long)r.traced);
    s6_log(line);
    simplelist_addline("%s: %s", what,
                       !rc && r.matched == r.aus_ref && r.hash == r.ref_hash ?
                       "PASS" : "FAIL");
    if (rc)
        simplelist_addline("  error %d", rc);
    simplelist_addline("  %lu/%lu AUs matched", (unsigned long)r.matched,
                       (unsigned long)r.aus_ref);
    if (r.first_bad >= 0)
        simplelist_addline("  first wrong: AU %ld", (long)r.first_bad);
    simplelist_addline("  %lu us/AU at %lu MHz",
                       (unsigned long)(r.us_n ? r.us_total / r.us_n : 0),
                       (unsigned long)r.mhz);
    simplelist_addline("  (max %lu us)", (unsigned long)r.us_max);
    simplelist_addline("  refused %lu, upload bad %lu",
                       (unsigned long)r.errors, (unsigned long)r.au_bad);
    simplelist_addline("  tables %s, bytes SDRAM %s",
                       r.tables < 0 ? "?" : r.tables ? "same" : "DIFFER",
                       r.mem_sdram < 0 ? "?" : r.mem_sdram ? "ok" : "WRONG");
    simplelist_addline("  copies wrong %lu of %lu",
                       (unsigned long)r.copy_bad,
                       (unsigned long)r.copy_checked);
    simplelist_addline("  stress wrong: SRAM %ld, SDRAM %ld",
                       (long)r.stress_sram, (long)r.stress_sdram);
}

bool devbcm_test9(void)
{
    struct simplelist_info info;
    bool probe_ok = true, probe_ran = false;
    char line[160];

    simplelist_info_init(&info, "BCM stage 9", 0, NULL);
    simplelist_reset_lines();
    if (!file_exists(STAGE9_AU) || !file_exists(STAGE9_REF) ||
        !file_exists(VPLAYER_IMAGE))
    {
        simplelist_addline("Not available: no %s",
                           file_exists(VPLAYER_IMAGE) ? "test clip"
                                                      : "video image");
        simplelist_addline("Put aac9.au and aac9.ref in");
        simplelist_addline("the system folder's bcmtest");
        simplelist_addline("folder (mkdevclip.sh).");
        return show(&info);
    }
    simplelist_addline("9: the video chip decodes an");
    simplelist_addline("AAC clip, checked against the");
    simplelist_addline("computer's decode; full clock,");
    simplelist_addline("byte copies, 27 MHz (~20 s).");
    simplelist_addline("MENU: cancel");
    confirm_line = 5;
    simplelist_addline("Run stage 9 now (SELECT here)");
    confirm_4b = false;
    info.action_callback = confirm_cb;
    simplelist_show_list(&info);
    if (!confirm_4b)
        return false;

    audio_stop();
    s6_t0 = USEC_TIMER;
    snprintf(line, sizeof(line), "=== BCM test, stage 9 (AAC on the chip): "
             "build %s ===", rbversion);
    s6_log(line);
    simplelist_info_init(&info, "BCM stage 9", 0, NULL);
    simplelist_reset_lines();
    s9_run("full clock", true, 0, &probe_ok, &probe_ran);
    /* session 36c's A/B: the decoder's copies and clears a byte at a time */
    s9_run("byte copies", true, VPLAYER_AAC_BYTE_COPY, &probe_ok, &probe_ran);
    s9_run("27 MHz", false, 0, &probe_ok, &probe_ran);
    simplelist_addline("bcs/bcc probe: %s",
                       !probe_ran ? "did not run" : probe_ok ? "PASS" : "FAIL");
    s6_log("=== end: stage 9 ===");
    return show(&info);
}

/* 8a-1 (dev42): the BCM's own audio output, made safe after session 35's
 * full-scale screech.  LOCKED: it drives GPIO 1, so it runs only when this
 * boot's 8a-0b found the pin floating in all three states, and only on the
 * owner's OK for each tone.  The codec's volume is turned down for the whole
 * probe (and put back), the tone is capped at -40 dBFS in the image.
 * dev45: session 36c's block never became ready with GPIO 2 and 3 left
 * inputs, so the image now puts them on Apple's function 2 as the block's
 * clock inputs (the block is a clock slave, audio.md section 5), only
 * while it sees the codec's frame clock on GPIO 3; the codec is kept
 * awake for the probe (its standby stops the clocks).  Four sample
 * layouts, one labelled tone each: the owner says which one sounds
 * clean. */
static const char *const s8_layouts[4] =
{
    "1: 16-bit", "2: 16-bit, bytes swapped", "3: 32-bit, sample high",
    "4: 32-bit, sample low",
};

static bool s8_ask(const char *title, const char *l1, const char *l2)
{
    struct simplelist_info info;

    simplelist_info_init(&info, (char *)title, 0, NULL);
    simplelist_reset_lines();
    simplelist_addline("%s", l1);
    simplelist_addline("%s", l2);
    simplelist_addline("Volume is turned down for this.");
    simplelist_addline("MENU: stop here");
    confirm_line = 4;
    simplelist_addline("Play it (SELECT here)");
    confirm_4b = false;
    info.action_callback = confirm_cb;
    simplelist_show_list(&info);
    return confirm_4b;
}

/* After each tone: what the owner heard, picked on the device, so the
 * answer lands in bcmtest-results.log with the run (session 36 lost the
 * spoken one with the logs) */
static const char *const s8_heard[] =
{
    "A clean, quiet tone", "Noise or a screech", "Nothing", "Not sure",
};
static int choice_first, choice_got;

static int choice_cb(int action, struct gui_synclist *lists)
{
    lists->wraparound = false;
    if (action == ACTION_STD_OK)
    {
        int sel = gui_synclist_get_sel_pos(lists);

        if (sel < choice_first)
            return ACTION_NONE;
        choice_got = sel - choice_first;
        return ACTION_STD_CANCEL;
    }
    return action;
}

static int s8_verdict(int k)
{
    struct simplelist_info info;
    int j;

    simplelist_info_init(&info, "BCM stage 8a-1", 0, NULL);
    simplelist_reset_lines();
    simplelist_addline("Layout %s:", s8_layouts[k]);
    simplelist_addline("what did you hear? (SELECT)");
    choice_first = 2;
    for (j = 0; j < (int)ARRAYLEN(s8_heard); j++)
        simplelist_addline("%s", s8_heard[j]);
    choice_got = -1;
    info.action_callback = choice_cb;
    simplelist_show_list(&info);
    return choice_got;
}

/* The same pick for devbcm_probes.c (the TV rows): two lines of question,
 * then the options; the option picked (0 ...), or -1 for MENU */
int devbcm_choose(const char *title, const char *l1, const char *l2,
                  const char *const *opts, int n)
{
    struct simplelist_info info;
    int j;

    simplelist_info_init(&info, (char *)title, 0, NULL);
    simplelist_reset_lines();
    simplelist_addline("%s", l1);
    simplelist_addline("%s", l2 ? l2 : "");
    choice_first = 2;
    for (j = 0; j < n; j++)
        simplelist_addline("%s", opts[j]);
    choice_got = -1;
    info.action_callback = choice_cb;
    simplelist_show_list(&info);
    return choice_got;
}

/* The same pick after lines of text (already wrapped): the bench
 * session's questions */
int devbcm_lines_choose(const char *title, const char *const *lines, int n,
                        const char *const *opts, int nopts)
{
    struct simplelist_info info;
    int j;

    simplelist_info_init(&info, (char *)title, 0, NULL);
    simplelist_reset_lines();
    for (j = 0; j < n; j++)
        simplelist_addline("%s", lines[j]);
    choice_first = n;
    for (j = 0; j < nopts; j++)
        simplelist_addline("%s", opts[j]);
    choice_got = -1;
    info.action_callback = choice_cb;
    simplelist_show_list(&info);
    return choice_got;
}

bool devbcm_test8a1(void)
{
    struct simplelist_info info;
    uint32_t r[VPLAYER_TONE_OUT];
    char line[240];
    int k, rc, vol;
    bool played[4] = { false, false, false, false };
    bool clocks[4] = { false, false, false, false };
    int heard[4] = { -1, -1, -1, -1 };
#ifdef HAVE_DAC_STANDBY
    bool standby_was;
#endif

    simplelist_info_init(&info, "BCM stage 8a-1", 0, NULL);
    simplelist_reset_lines();
    if (!s8_free_all)
    {
        simplelist_addline("Locked.");
        simplelist_addline("Run stage 8a-0/0b first, in");
        simplelist_addline("this boot: GPIO1 must float");
        simplelist_addline("in all three states, and the");
        simplelist_addline("codec's clocks must show.");
        return show(&info);
    }
    if (!s8_ask("BCM stage 8a-1", "This drives the video chip's",
                "audio pin. Four quiet tones."))
        return false;

    audio_stop();
    /* the codec down for the probe, never saved (setvol() would) */
    vol = MAX(sound_min(SOUND_VOLUME), -50);
    sound_set_volume(vol);
#ifdef HAVE_DAC_STANDBY
    /* awake, so its clocks run for the block (not saved: a developer
       switch, put back below) */
    standby_was = dac_standby_allowed();
    dac_standby_allow(false);
#endif
    s8_silence(true);
    sleep(HZ / 10);         /* the codec's wake, 12 ms at worst */
    s6_t0 = USEC_TIMER;
    snprintf(line, sizeof(line), "=== BCM test, stage 8a-1 (quiet tones on "
             "the BCM's audio pins: clocks in on GPIO 2/3, data out on GPIO "
             "1, volume %d dB; the PP plays silence, PCM %d): build %s ===",
             vol, (int)mixer_channel_status(PCM_MIXER_CHAN_PLAYBACK),
             rbversion);
    s6_log(line);
    for (k = 0; k < 4; k++)
    {
        if (k && !s8_ask("BCM stage 8a-1", "Next tone, layout",
                         s8_layouts[k]))
            break;
        /* 440 Hz at 44.1 kHz, Apple's start bits */
        rc = vplayer_tone(1000, 34, k, 167393, 0, r);
        played[k] = rc == 0 && r[1] && r[2];
        clocks[k] = rc == 0 && r[7] != 0;
        if (played[k])
            heard[k] = s8_verdict(k);
        snprintf(line, sizeof(line), "stage 8a-1 layout %s: rc %d, control "
                 "before %08lx, after the rate %08lx, playing %08lx; ready "
                 "%lu, played %lu ms; fsel %08lx; edges in 3 ms GPIO2 %lu, "
                 "GPIO3 %lu; heard: %s", s8_layouts[k], rc,
                 (unsigned long)r[3], (unsigned long)r[0],
                 (unsigned long)r[4], (unsigned long)r[1],
                 (unsigned long)r[2], (unsigned long)r[5],
                 (unsigned long)r[6], (unsigned long)r[7],
                 heard[k] >= 0 ? s8_heard[heard[k]] : "(no answer)");
        s6_log(line);
        /* PB-5: Apple's two audio clock words, read only */
        snprintf(line, sizeof(line), "8a1: layout %d clocks 1c34/1c38 before "
                 "the rate %08lx/%08lx, playing %08lx/%08lx, after close "
                 "%08lx/%08lx", k + 1, (unsigned long)r[8],
                 (unsigned long)r[9], (unsigned long)r[10],
                 (unsigned long)r[11], (unsigned long)r[12],
                 (unsigned long)r[13]);
        s6_log(line);
    }
    s8_silence(false);
#ifdef HAVE_DAC_STANDBY
    dac_standby_allow(standby_was);
#endif
    sound_set_volume(global_status.volume);     /* the owner's again */

    simplelist_info_init(&info, "BCM stage 8a-1", 0, NULL);
    simplelist_reset_lines();
    for (k = 0; k < 4; k++)
        simplelist_addline("%s: %s", s8_layouts[k],
                           !played[k] ? (clocks[k] ? "not played"
                                                   : "not played, no clocks")
                           : heard[k] >= 0 ? s8_heard[heard[k]]
                           : "sent, no answer");
    return show(&info);
}

/* The player's full log on normal plays (dev41: off by default) */
bool devbcm_fullvlog(void)
{
    struct simplelist_info info;

    vplayer_set_full_log(!vplayer_get_full_log());
    simplelist_info_init(&info, "Video full log", 0, NULL);
    simplelist_reset_lines();
    simplelist_addline("Full video log on every play:");
    simplelist_addline("  %s (until reboot)", vplayer_get_full_log() ? "ON"
                                                                      : "off");
    simplelist_addline("Select the item again to toggle.");
    return show(&info);
}

bool devbcm_test7(void)
{
    struct simplelist_info info;
    struct vtrace_rec *rec;
    struct mp3entry *id3;
    unsigned long len = 0, el;
    int h, hs, k, repeat;
    bool ok = true, all = true;
    char line[256];  /* a step line runs past 200 (session 28 lost its tail) */

    simplelist_info_init(&info, "BCM stage 7", 0, NULL);
    simplelist_reset_lines();
    if (!file_exists(STAGE7_FILE) || !file_exists(VPLAYER_IMAGE))
    {
        simplelist_addline("Not available: no %s",
                           file_exists(VPLAYER_IMAGE) ? "test video"
                                                      : "video image");
        simplelist_addline("Put a converted video in the");
        simplelist_addline("system folder's bcmtest folder");
        simplelist_addline("as stage7.m4v");
        return show(&info);
    }
    simplelist_addline("Plays the test video and works");
    simplelist_addline("its controls (about 45 s).");
    simplelist_addline("Hands off until it ends.");
    simplelist_addline("Everything goes in the log.");
    simplelist_addline("MENU: cancel");
    confirm_line = 5;
    simplelist_addline("Run stage 7 now (SELECT here)");
    confirm_4b = false;
    info.action_callback = confirm_cb;
    simplelist_show_list(&info);
    if (!confirm_4b)
        return false;

    audio_stop();
    h = core_alloc(STAGE6_RECORDS * sizeof(struct vtrace_rec));
    if (h <= 0)
        return false;
    core_pin(h);
    hs = core_alloc(S7_STEPS * sizeof(struct s7_step));
    if (hs <= 0)
    {
        core_free(h);
        return false;
    }
    core_pin(hs);
    rec = core_get_data(h);
    s7 = core_get_data(hs);
    s7_n = 0;
    vtrace_init(&s6_trace, rec, STAGE6_RECORDS);
    s6_t0 = s7_t0 = USEC_TIMER;
    repeat = global_settings.repeat_mode;
    global_settings.repeat_mode = REPEAT_OFF;
    vplayer_set_frame_hook(s6_frame);
    vplayer_set_sdram_check(true);      /* the player's lines and trace */

    s7_begin("start");
    ok = s7_start(0) && s7_wait(4000);
    s7_end();
    id3 = audio_current_track();
    len = id3 ? id3->length : 0;
    if (ok)
    {
        s7_begin("pause 2 s, resume");
        audio_pause();
        ok = s7_wait(2000);
        s7_resume();
        ok = ok && s7_wait(3000);
        s7_end();
    }
    if (ok)
    {
        s7_begin("paused redraw, scrub ahead, resume");
        audio_pause();
        s7_wait(500);
        vplayer_seek(s7_elapsed());     /* the overlay's hide */
        s7_wait(700);
        el = s7_ahead(s7_elapsed(), 30000, len);
        if (s7_n < S7_STEPS)
            s7[s7_n].want_ms = el;
        s7_seek(el, true);
        s7_wait(1000);
        s7_resume();
        ok = s7_wait(3000);
        s7_end();
    }
    if (ok)
    {
        s7_begin("PREV: restart");
        s7_seek(0, true);
        ok = s7_wait(3000);
        s7_end();
    }
    if (ok)
    {
        s7_begin("audio's own fast forward");
        el = s7_ahead(s7_elapsed(), 20000, len);
        if (s7_n < S7_STEPS)
            s7[s7_n].want_ms = el;
        s7_seek(el, false);
        ok = s7_wait(3000);
        s7_end();
    }
    if (ok)
    {
        s7_begin("pause, dark 3 s, wake, resume");
        audio_pause();
        s7_wait(500);
        backlight_off();
        s7_wait(3000);
        backlight_on();
        s7_wait(1500);
        s7_resume();
        ok = s7_wait(3000);
        s7_end();
    }
    if (ok)
    {
        s7_begin("MENU, re-enter (resume)");
        el = s7_elapsed();
        vplayer_stop();
        audio_stop();
        sleep(HZ / 2);
        ok = s7_start(el) && s7_wait(4000);
        s7_end();
    }
    if (ok && len > 6000)
    {
        s7_begin("to the end");
        s7_seek(len - 5000, true);
        for (k = 0; k < 100; k++)
        {
            struct vplayer_info vi;

            sleep(HZ / 10);
            if (!vplayer_info(&vi) || vi.ended || vi.state == VPC_FAILED ||
                !(audio_status() & AUDIO_STATUS_PLAY))
                break;
        }
        s7_end();
    }
    /* dev39: the tuning A/B, 8 s from 30 s in under each (the player's
       port, thread and late lines close each one in bcmtest.log; "watch:
       CPU MHz" shows the PP's clock).  The thread's priority is fixed in
       its band since dev43, so no floor rows; the port's PP DMA is a
       dead end (docs/data-paths.md D1: nothing lands), so no DMA rows */
    {
        /* the PP's boost by the chip's clock, the CPU's copy throughout
           (the owner: "test multiple configurations"); the default
           first.  The two chip-27 rows answer DP-1: does the BCM's clock
           set the port's write rate at a fixed PP clock? */
#define BO VPLAYER_TUNE_BOOST
#define C27 VPLAYER_TUNE_CHIP27
#define E2 VPLAYER_TUNE_E2
        static const struct { uint32_t t; const char *what; } ab[] =
        {
            { 0,               "A/B: unboosted, CPU" },
            { BO,              "A/B: boosted, CPU" },
            { C27,             "A/B: unboosted, CPU, chip 27" },
            { BO | C27,        "A/B: boosted, CPU, chip 27" },
            /* the second engine (session 37's E2 lab): the CPU free while
               a block moves; whether it holds 30 fps unboosted */
            { E2,              "A/B: unboosted, engine 2" },
            { E2 | C27,        "A/B: unboosted, engine 2, chip 27" },
        };
#undef E2
#undef BO
#undef C27
        uint32_t keep = vplayer_get_tuning();

        /* measurements: a row that fails is logged and the next one
           starts afresh (session 33: one failing DMA row ended the
           stage); the earlier steps' verdict stands */
        for (k = 0; k < (int)ARRAYLEN(ab); k++)
        {
            vplayer_stop();
            audio_stop();
            vplayer_set_tuning(ab[k].t);
            s7_begin(ab[k].what);
            if (s7_start(30000))
                s7_wait(8000);
            s7_end();
        }
        vplayer_set_tuning(keep);
    }
    vplayer_stop();
    audio_stop();
    global_settings.repeat_mode = repeat;
    vplayer_set_frame_hook(NULL);
    vplayer_set_sdram_check(false);

    /* the log, now that nothing plays */
    snprintf(line, sizeof(line), "=== BCM test, stage 7 (a real video's "
             "controls): %s, %lu ms, build %s ===", STAGE7_FILE,
             (unsigned long)len, rbversion);
    s6_log(line);
    simplelist_info_init(&info, "BCM stage 7", 0, NULL);
    simplelist_reset_lines();
    for (k = 0; k < s7_n; k++)
    {
        const struct s7_step *st = &s7[k];
        uint32_t n = st->rec1 - st->rec0;
        int32_t dm = s7_drift_max(st);
        /* the audio never ran dry because of the video, except as the
           file ends */
        bool pass = n > 0 && st->state != VPC_FAILED &&
                    (n <= 15 || dm <= VTRACE_DRIFT_US) &&
                    (!st->want_ms || st->el_end >= st->want_ms) &&
                    (!st->underruns || !strncmp(st->what, "to the end", 10));

        if (strncmp(st->what, "A/B", 3))
            all &= pass;        /* the A/B rows are measurements (dev39) */
        snprintf(line, sizeof(line), "stage 7 step %d %s: %s at %lu ms; "
                 "elapsed %lu -> %lu; %lu pictures (V6 #%lu-#%lu), drift max "
                 "%ld ms after 15; totals skipped %lu, GOPs dropped %lu, late "
                 "%lu; state %d; PCM underruns %lu", k + 1,
                 pass ? "PASS" : "FAIL", st->what,
                 (unsigned long)st->t_ms, (unsigned long)st->el_start,
                 (unsigned long)st->el_end, (unsigned long)n,
                 (unsigned long)st->rec0, (unsigned long)st->rec1,
                 (long)(dm / 1000), (unsigned long)st->skipped,
                 (unsigned long)st->gops, (unsigned long)st->late, st->state,
                 (unsigned long)st->underruns);
        s6_log(line);
        s6_say("%d %s: %s, %lu pics, %ld ms", k + 1, pass ? "ok" : "FAIL",
               st->what, (unsigned long)n, (long)(dm / 1000));
    }
    if (!ok)
        s6_say("Stopped early: the player failed");
    s6_say("Stage 7 %s", all && ok && s7_n >= 7 ? "PASS" : "FAIL");
    {
        struct vtrace_sum sum;

        vtrace_summary(&s6_trace, true, !ok, &sum);
        vtrace_lines(&s6_trace, &sum, s6_line, NULL);
    }
    core_free(hs);
    core_free(h);
    s7 = NULL;
    return show(&info);
}

/* The stage with the codec on the main core: the COP switch applies at
 * the next codec load, and every stage stops the audio and starts its
 * own, so turning it off here is enough; it is on again after */
static bool with_cpu_codec(bool (*stage)(void))
{
#ifdef HAVE_COP_CODEC
    bool was = codec_cop_enabled();
    bool r;

    if (was)
    {
        codec_cop_set_enabled(false);
        bcm_devtest_log("stage: the COP decode switch is off for this run");
    }
    r = stage();
    if (was)
        codec_cop_set_enabled(true);
    return r;
#else
    return stage();
#endif
}

bool devbcm_cpu_test6(void)  { return with_cpu_codec(devbcm_test6); }
bool devbcm_cpu_test7(void)  { return with_cpu_codec(devbcm_test7); }
bool devbcm_cpu_test8(void)  { return with_cpu_codec(devbcm_test8); }
bool devbcm_cpu_test8b(void) { return with_cpu_codec(devbcm_test8b); }


/* ---- The second DMA engine lab (E2-0 ... E2-7) --------------------------
 *
 * docs/data-paths.md section 7.1 (hw-experiments): does the PP's second
 * DMA engine (0x6000_8000), driven as Apple's osos drives it, deliver
 * into the BCM's host port, and how much CPU does it free?  Step by step,
 * each only if the ones before behaved; the first piece that never ends,
 * leaves its event raised, or touches a guard word ends the lab, which
 * then cleans up.  Every target is filled with pattern T and guard words
 * G first, the source with S only after (S8's readback was of a target
 * that already held the data), and every word is read back.  The raw
 * values go to bcmtest.log and bcmtest-results.log line by line; the
 * screen gets the summary.  The BCM is given back through the stock wake
 * path at the end (bcm_video_release: Apple's vmcs, or the boot path's
 * image), as the other video stages do. */
#include "system.h"
#include "bcm-e2lab.h"

#define E2_NSIZES 4
/* largest first: the first piece, long enough to see a busy bit, tells
 * which signal ends a piece (the counting loop of E2-7 waits for it) */
static const uint32_t e2_sizes[E2_NSIZES] = { 65536, 32768, 4096, 256 };

struct e2_lab
{
    struct bvp b;
    uint32_t seed, seq;
    uint32_t done_how, done_int;    /* learnt: what ends a piece */
    uint32_t nirq_how, nirq_int;    /* ... without bit 30 (E2-7 (3)) */
    bool stop;                  /* a hang, a raised event or a guard */
    bool cut;                   /* a BCM guard: power-cycle first */
    bool image;                 /* our image runs and answered */
    int pass1, pass3, pass4;    /* -1 not run, 0 failed, 1 passed */
    uint32_t *src;              /* E2LAB_MAX_WORDS of S */
    uint32_t *dst;              /* G | T | G, also the BCM fills' staging */
    uint32_t *sdram;            /* the counting loop's SDRAM word, alone
                                   in its cache line */
};

static struct e2_lab *e2;
static const struct bvp_port e2_port =
{
    .rd = bcm_video_rd, .wr = bcm_video_wr, .wr_block = bcm_video_wr_block,
    .now_us = bcm_video_now_us, .yield = yield,
};

static void e2_raw(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    int fd;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    s6_log(line);
    fd = open(RESULTS_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0)
    {
        results_put(fd, line);
        close(fd);
    }
}

static uint32_t e2_new_seed(void)
{
    e2->seq++;
    e2->seed = e2_seed(USEC_TIMER ^ (e2->seq * 0x9E3779B9u) ^
                       (uint32_t)current_tick << 7);
    return e2->seed;
}

static const char *e2_verdict(const struct e2_tally *t)
{
    if (e2_tally_pass(t))
        return "all correct";
    if (t->ok == 0 && t->shifted == 0 && t->untouched == t->n)
        return "untouched";
    if (e2_tally_fixed_dst(t))
        return "word 0 only (dst fixed)";
    if (e2_tally_partial(t))
        return "partial";
    return "wrong";
}

static void e2_piece_log(const char *tag, const struct e2_piece *p,
                         const struct e2_tally *t, uint32_t ghits)
{
    e2_raw("e2 %s: %lu B rc %d in %lu us (how %lx at %lu/%lu/%lu/%lu us), "
           "MHz %ld, seed %03lx", tag, (unsigned long)p->bytes, p->rc,
           (unsigned long)p->us, (unsigned long)p->how,
           (unsigned long)p->t_how[0], (unsigned long)p->t_how[1],
           (unsigned long)p->t_how[2], (unsigned long)p->t_how[3],
           p->freq / 1000000, (unsigned long)e2->seed);
    e2_raw("e2 %s:  cmd %08lx -> %08lx, status pre %08lx %08lx post "
           "%08lx %08lx, seen %lu: %08lx@%lu %08lx@%lu", tag,
           (unsigned long)p->cmd_written, (unsigned long)p->cmd_post,
           (unsigned long)p->st_pre[0], (unsigned long)p->st_pre[1],
           (unsigned long)p->st_post[0], (unsigned long)p->st_post[1],
           (unsigned long)p->nsv, (unsigned long)p->sv[0],
           (unsigned long)p->sv_t[0], (unsigned long)p->sv[1],
           (unsigned long)p->sv_t[1]);
    e2_raw("e2 %s:  INT_STAT[28:26] %lx -> %lx, masked %lx, ready %lu us, "
           "count %lu, I2S free max %lu", tag,
           (unsigned long)(p->int_pre >> 26), (unsigned long)(p->int_post >> 26),
           (unsigned long)(p->en_masked >> 26), (unsigned long)p->ready_us,
           (unsigned long)p->count, (unsigned long)p->iis_max_free);
    if (t)
        e2_raw("e2 %s:  %s: ok %lu untouched %lu shifted %lu (first d %ld) "
               "guard-like %lu other %lu of %lu; first bad %ld, other %08lx "
               "at %ld; guard hits %lu", tag, e2_verdict(t),
               (unsigned long)t->ok, (unsigned long)t->untouched,
               (unsigned long)t->shifted, (long)t->first_d,
               (unsigned long)t->guardlike, (unsigned long)t->other,
               (unsigned long)t->n, (long)t->first_bad,
               (unsigned long)t->first_other, (long)t->first_other_i,
               (unsigned long)ghits);
}

/* After a piece: anything that ends the lab */
static bool e2_check(const char *tag, const struct e2_piece *p,
                     uint32_t ghits, bool bcm)
{
    if (p->rc == 1)
        e2_raw("e2 %s: STOP: the piece never ended (stopped)", tag);
    else if (p->rc == 2)
        e2_raw("e2 %s: STOP: its event stays raised (sources %lx left "
               "masked)", tag, (unsigned long)(p->en_masked >> 26));
    else if (p->rc < 0)
        e2_raw("e2 %s: STOP: %s", tag, p->rc == -1 ? "refused (target)"
                                                   : "port error");
    else if (ghits)
        e2_raw("e2 %s: STOP: %lu guard words changed", tag,
               (unsigned long)ghits);
    else
        return true;
    e2->stop = true;
    e2->cut = bcm && (ghits || p->rc == 1 || p->rc == -2);
    return false;
}

/* PP SDRAM target: G | T | G in dst, then S in src, one piece, every word
 * read back from SDRAM (the cache discarded, the uncached alias) */
static void e2_pp_run(const char *tag, struct e2_piece *p, uint32_t bytes,
                      struct e2_tally *t)
{
    uint32_t n = bytes / 4, seed = e2_new_seed(), i, ghits;
    uint32_t *g0 = e2->dst, *tgt = g0 + E2LAB_GUARD_WORDS, *g1 = tgt + n;
    const volatile uint32_t *u;
    uint32_t gb[E2LAB_GUARD_WORDS], ga[E2LAB_GUARD_WORDS];

    e2_fill(g0, E2LAB_GUARD_WORDS, E2_PAT_G, seed, 0);
    e2_fill(tgt, n, E2_PAT_T, seed, 0);
    e2_fill(g1, E2LAB_GUARD_WORDS, E2_PAT_G, seed, E2LAB_GUARD_WORDS);
    e2_fill(e2->src, n, E2_PAT_S, seed, 0);    /* after the target */
    p->src = e2->src;
    p->dst = tgt;
    p->port = false;
    p->lo = (uint32_t)(uintptr_t)tgt;
    p->hi = (uint32_t)(uintptr_t)(tgt + n);
    p->bytes = bytes;
    bcm_e2_piece(p);
    commit_discard_dcache();
    u = UNCACHED_ADDR(g0);
    e2_tally_init(t);
    for (i = 0; i < E2LAB_GUARD_WORDS; i++)
    {
        gb[i] = u[i];
        ga[i] = u[E2LAB_GUARD_WORDS + n + i];
    }
    for (i = 0; i < n; i++)
        e2_tally_add(t, u[E2LAB_GUARD_WORDS + i], seed, i, n);
    ghits = e2_guard_hits(gb, ga, seed);
    e2_piece_log(tag, p, t, ghits);
    e2_check(tag, p, ghits, false);
}

/* BCM target through the port: G | T | G written by the CPU (R1) and a
 * sample read back (R2), then S, one piece to bcm_addr, every word and
 * the guards read back.  < 0: the CPU's own fill or readback failed (the
 * step is void, not the engine's result). */
static int e2_bcm_run(const char *tag, struct e2_piece *p, uint32_t win,
                      uint32_t bytes, struct e2_tally *t)
{
    uint32_t n = bytes / 4, seed = e2_new_seed(), i, got, ghits, bad = 0;
    uint32_t base = win - 4 * E2LAB_GUARD_WORDS, all = n + 2 * E2LAB_GUARD_WORDS;
    uint32_t gb[E2LAB_GUARD_WORDS], ga[E2LAB_GUARD_WORDS];

    e2_fill(e2->dst, E2LAB_GUARD_WORDS, E2_PAT_G, seed, 0);
    e2_fill(e2->dst + E2LAB_GUARD_WORDS, n, E2_PAT_T, seed, 0);
    e2_fill(e2->dst + E2LAB_GUARD_WORDS + n, E2LAB_GUARD_WORDS, E2_PAT_G,
            seed, E2LAB_GUARD_WORDS);
    if (bcm_video_wr_block(base, (const uint16_t *)e2->dst, all * 2) ||
        bcm_video_flush())
    {
        e2_raw("e2 %s: the CPU's fill: port error", tag);
        return -1;
    }
    /* the fill on a sample: the guards' ends, T's first, last, every 64th */
    for (i = 0; i < all; i++)
    {
        if (!(i == 0 || i == all - 1 || i == E2LAB_GUARD_WORDS ||
              i == all - 1 - E2LAB_GUARD_WORDS ||
              (i > E2LAB_GUARD_WORDS && !((i - E2LAB_GUARD_WORDS) & 63))))
            continue;
        if (bcm_video_rd(base + 4 * i, &got) < 0)
        {
            e2_raw("e2 %s: the fill's readback: port error", tag);
            return -2;
        }
        bad += got != e2->dst[i];
    }
    if (bad)
    {
        e2_raw("e2 %s: the CPU's fill did not read back (%lu wrong): void",
               tag, (unsigned long)bad);
        return -3;
    }
    e2_fill(e2->src, n, E2_PAT_S, seed, 0);    /* after the target */
    p->src = e2->src;
    p->port = true;
    p->bcm_addr = win;
    p->lo = win;
    p->hi = win + E2LAB_WIN_SIZE;
    p->bytes = bytes;
    bcm_e2_piece(p);
    e2_tally_init(t);
    ghits = 0;
    if (p->rc != -2)
    {
        for (i = 0; i < all; i++)
        {
            if (bcm_video_rd(base + 4 * i, &got) < 0)
            {
                e2_raw("e2 %s: readback: port error at word %lu", tag,
                       (unsigned long)i);
                p->rc = -2;
                break;
            }
            if (i < E2LAB_GUARD_WORDS)
                gb[i] = got;
            else if (i >= E2LAB_GUARD_WORDS + n)
                ga[i - E2LAB_GUARD_WORDS - n] = got;
            else
                e2_tally_add(t, got, seed, i - E2LAB_GUARD_WORDS, n);
        }
        if (p->rc != -2)
            ghits = e2_guard_hits(gb, ga, seed);
    }
    e2_piece_log(tag, p, t, ghits);
    e2_check(tag, p, ghits, true);
    return 0;
}

static void e2_piece_apple(struct e2_piece *p)
{
    memset(p, 0, sizeof(*p));
    p->engine = E2LAB_ENG2;
    p->src_code = p->dst_code = 2;
    p->b27 = true;
    p->intr = true;
}

/* The signal that ended a piece, best first: STATUS bit 30, an interrupt
 * source, STATUS bit 31 falling, CMD bit 31 falling */
static void e2_learn(const struct e2_piece *p, uint32_t *how, uint32_t *bits)
{
    uint32_t h = p->how;

    if (*how || p->rc != 0 || !h)
        return;
    *how = (h & E2LAB_HOW_ST30) ? E2LAB_HOW_ST30 :
           (h & E2LAB_HOW_INT) ? E2LAB_HOW_INT :
           (h & E2LAB_HOW_ST31) ? E2LAB_HOW_ST31 : E2LAB_HOW_CMD31;
    *bits = p->int_post & ~p->int_pre;
}

/* E2-0: read only */
static bool e2_step0(void)
{
    uint32_t d[E2LAB_DUMP_WORDS], same = 0, zero = 0, ones = 0, k;
    static const struct { uint32_t at; int n; } part[4] =
        { { 0x60008000, 4 }, { 0x60009000, 64 }, { 0x6000A000, 4 },
          { 0x6000B000, 32 } };
    char line[120];
    int p, i = 0, len;

    e2_raw("e2 E2-0: reading the engines' registers");
    bcm_e2_dump(d);
    for (p = 0; p < 4; p++)
        for (k = 0; k < (uint32_t)part[p].n; k += 8)
        {
            uint32_t m;

            len = snprintf(line, sizeof(line), "e2 E2-0 %08lx:",
                           (unsigned long)(part[p].at + 4 * k));
            for (m = 0; m < 8 && k + m < (uint32_t)part[p].n; m++)
                len += snprintf(line + len, sizeof(line) - len, " %08lx",
                                (unsigned long)d[i++]);
            e2_raw("%s", line);
        }
    for (k = 0; k < 32; k++)
        same += d[4 + k] == d[4 + 32 + k];
    for (k = 0; k < 68; k++)
    {
        zero += d[k] == 0;
        ones += d[k] == 0xFFFFFFFFu;
    }
    e2_raw("e2 E2-0: 0x60009000-7F equals 0x60009080-FF in %lu of 32 words; "
           "engine 2's 68 words: %lu zero, %lu all-ones", (unsigned long)same,
           (unsigned long)zero, (unsigned long)ones);
    simplelist_addline("E2-0 master %08lx", (unsigned long)d[0]);
    simplelist_addline("E2-0 ch0 %08lx/%08lx", (unsigned long)d[4],
                       (unsigned long)d[5]);
    simplelist_addline("E2-0 alias 9000=9080: %lu/32", (unsigned long)same);
    if (zero == 68 || ones == 68)
    {
        e2_raw("e2 E2-0: STOP: engine 2 reads all %s: gated or absent",
               zero == 68 ? "zero" : "ones");
        simplelist_addline("E2-0: all %s: STOP", zero == 68 ? "0" : "1s");
        return false;
    }
    return true;
}

static void e2_line(const char *step, uint32_t bytes,
                    const struct e2_piece *p, const struct e2_tally *t)
{
    if (p->rc)
        simplelist_addline("%s %luK: rc %d", step,
                           (unsigned long)(bytes >> 10), p->rc);
    else
        simplelist_addline("%s %lu: %s %luus", step, (unsigned long)bytes,
                           e2_verdict(t), (unsigned long)p->us);
}

/* E2-1: engine 2, SDRAM to SDRAM, Apple's sequence */
static void e2_step1(void)
{
    struct e2_piece p;
    struct e2_tally t;
    int k, passed = 0;

    for (k = 0; k < E2_NSIZES && !e2->stop; k++)
    {
        e2_piece_apple(&p);
        e2_pp_run("E2-1", &p, e2_sizes[k], &t);
        e2_line("E2-1", e2_sizes[k], &p, &t);
        e2_learn(&p, &e2->done_how, &e2->done_int);
        passed += p.rc == 0 && e2_tally_pass(&t);
    }
    e2->pass1 = passed == E2_NSIZES;
    e2_raw("e2 E2-1: %d of %d sizes correct; end signal %lx", passed,
           E2_NSIZES, (unsigned long)e2->done_how);
}

/* E2-2: general channel 3, SDRAM to SDRAM, ours and Apple's FLAGS */
static void e2_step2(void)
{
    static const uint32_t flags[2] = { 0x04000000, 0x24000000 };
    struct e2_piece p;
    struct e2_tally t;
    int k;

    for (k = 0; k < 2 && !e2->stop; k++)
    {
        memset(&p, 0, sizeof(p));
        p.engine = E2LAB_CH3;
        p.flags = flags[k];
        p.incr = 0x20000000;        /* range 0 (incrementing), 32-bit */
        e2_pp_run(k ? "E2-2 Apple FLAGS" : "E2-2 our FLAGS", &p, 4096, &t);
        simplelist_addline("E2-2 %s: %s", k ? "Apple" : "ours",
                           p.rc ? "no end" : e2_verdict(&t));
    }
}

/* E2-3/E2-4/E2-7 (1): one size into a BCM window, repeated with a
 * write-ready wait before the start if it came partial.  1 passed. */
static int e2_bcm_size(const char *step, uint32_t win, uint32_t bytes)
{
    struct e2_piece p;
    struct e2_tally t;
    char tag[32];
    int rc;

    snprintf(tag, sizeof(tag), "%s %luK", step, (unsigned long)(bytes >> 10));
    e2_piece_apple(&p);
    rc = e2_bcm_run(tag, &p, win, bytes, &t);
    if (rc < 0)
    {
        simplelist_addline("%s: fill void", tag);
        e2->stop = true;
        return 0;
    }
    e2_line(step, bytes, &p, &t);
    if (e2->stop || p.rc || !e2_tally_partial(&t))
        return p.rc == 0 && e2_tally_pass(&t);
    snprintf(tag, sizeof(tag), "%s %luK ready", step,
             (unsigned long)(bytes >> 10));
    e2_piece_apple(&p);
    p.wait_ready = true;
    if (e2_bcm_run(tag, &p, win, bytes, &t) < 0)
    {
        e2->stop = true;
        return 0;
    }
    simplelist_addline("  +ready wait: %s", p.rc ? "no end" : e2_verdict(&t));
    return p.rc == 0 && e2_tally_pass(&t);
}

static int e2_bcm_sizes(const char *step, uint32_t win)
{
    int k, passed = 0;

    for (k = 0; k < E2_NSIZES && !e2->stop; k++)
        passed += e2_bcm_size(step, win, e2_sizes[k]);
    e2_raw("e2 %s: %d of %d sizes correct", step, passed, E2_NSIZES);
    return passed == E2_NSIZES;
}

/* The CPU's control block, pattern C, proves the readback path */
static void e2_control_block(const char *step, uint32_t at)
{
    uint32_t seed = e2_new_seed(), i, got, bad = 0;

    e2_fill(e2->dst, 64, E2_PAT_C, seed, 0);
    if (bcm_video_wr_block(at, (const uint16_t *)e2->dst, 128))
        bad = 64;
    for (i = 0; i < 64 && !bad; i++)
        if (bcm_video_rd(at + 4 * i, &got) < 0 || got != e2->dst[i])
            bad++;
    e2_raw("e2 %s: CPU control block at %08lx: %lu of 64 wrong", step,
           (unsigned long)at, (unsigned long)bad);
    simplelist_addline("%s CPU control: %s", step, bad ? "WRONG" : "ok");
}

/* E2-5: the [26:24] codes, then bit 27 clear, 4 KB each */
static void e2_step5(void)
{
    struct e2_piece p;
    struct e2_tally t;
    char tag[40];
    unsigned s, d;
    int arm, good = 0;

    for (arm = 0; arm < 10 && !e2->stop; arm++)
    {
        s = arm < 9 ? 2 + arm / 3 : 2;
        d = arm < 9 ? 2 + arm % 3 : 2;
        e2_piece_apple(&p);
        p.src_code = s;
        p.dst_code = d;
        p.b27 = arm < 9;
        snprintf(tag, sizeof(tag), "E2-5 src %u dst %u b27 %d", s, d, arm < 9);
        if (e2_bcm_run(tag, &p, E2LAB_WIN_IDLE, 4096, &t) < 0)
        {
            e2->stop = true;
            break;
        }
        if (p.rc == 0 && e2_tally_pass(&t))
        {
            good++;
            simplelist_addline("E2-5 %u/%u b27 %d: PASS", s, d, arm < 9);
        }
    }
    simplelist_addline("E2-5: %d of 10 arms correct", good);
}

/* E2-6: general channel 3 into the port at width 16 */
static void e2_step6(void)
{
    static const uint32_t incr[2] = { 0x10010000, 0x12010000 };
    struct e2_piece p;
    struct e2_tally t;
    char tag[32];
    int k;

    for (k = 0; k < 2 && !e2->stop; k++)
    {
        memset(&p, 0, sizeof(p));
        p.engine = E2LAB_CH3;
        p.flags = 0x24000000;
        p.incr = incr[k];
        snprintf(tag, sizeof(tag), "E2-6 INCR %08lx", (unsigned long)incr[k]);
        if (e2_bcm_run(tag, &p, E2LAB_WIN_IDLE, 4096, &t) < 0)
        {
            e2->stop = true;
            break;
        }
        simplelist_addline("E2-6 %08lx: %s", (unsigned long)incr[k],
                           p.rc ? "no end" : e2_verdict(&t));
    }
}

/* E2-7 (2)/(3): the CPU a 64 KB piece into the port leaves free.  Per
 * loop kind (IRAM only, IRAM + an SDRAM word): the count during the piece
 * against the same loop idle for as long; and the CPU's own copy of the
 * same bytes.  audio: the soundtrack plays (no bit 30, source 26 left to
 * the audio FIQ). */
static void e2_offload(bool audio)
{
    static const char *const kind[2] = { "IRAM", "IRAM+SDRAM" };
    const char *step = audio ? "E2-7(3)" : "E2-7(2)";
    uint32_t how = audio ? e2->nirq_how : e2->done_how;
    uint32_t bits = audio ? e2->nirq_int : e2->done_int;
    volatile uint32_t *word = UNCACHED_ADDR(e2->sdram);
    struct e2_piece p;
    struct e2_tally t;
    uint32_t idle, out[2], copy_us = 0, pm;
    char tag[40];
    int k, rc;

    if (!how)
    {
        e2_raw("e2 %s: skipped: no end signal learnt", step);
        simplelist_addline("%s: no end signal", step);
        return;
    }
    rc = bcm_e2_cpu_copy(E2LAB_WIN_IDLE, e2->src, E2LAB_PIECE_MAX, &copy_us);
    e2_raw("e2 %s: the CPU's copy of 64 KB: %lu us (rc %d), MHz %ld", step,
           (unsigned long)copy_us, rc, cpu_frequency / 1000000);
    for (k = 0; k < 2 && !e2->stop; k++)
    {
        e2_piece_apple(&p);
        p.intr = !audio;
        p.audio = audio;
        p.work = k ? E2LAB_WORK_SDRAM : E2LAB_WORK_IRAM;
        p.work_word = word;
        p.done_how = how;
        p.done_int = bits;
        snprintf(tag, sizeof(tag), "%s %s", step, kind[k]);
        if (e2_bcm_run(tag, &p, E2LAB_WIN_IDLE, E2LAB_PIECE_MAX, &t) < 0 ||
            e2->stop || p.rc)
        {
            e2->stop = true;
            break;
        }
        idle = bcm_e2_spin(k ? word : NULL, p.us, out);
        /* the loop's rate during the piece over its idle rate, per mille */
        pm = idle && p.us ? (uint32_t)(((uint64_t)p.count * out[0] * 1000) /
                                       ((uint64_t)idle * p.us)) : 0;
        e2_raw("e2 %s: piece %lu us, %lu loops; idle %lu loops in %lu us "
               "(I2S free max %lu); CPU kept %lu/1000; data %s", tag,
               (unsigned long)p.us, (unsigned long)p.count,
               (unsigned long)idle, (unsigned long)out[0],
               (unsigned long)out[1], (unsigned long)pm, e2_verdict(&t));
        simplelist_addline("%s %s: %lu/1000", step, kind[k],
                           (unsigned long)pm);
        if (audio)
            simplelist_addline("  I2S free max %lu/%lu of 16",
                               (unsigned long)p.iis_max_free,
                               (unsigned long)out[1]);
    }
    simplelist_addline("%s CPU copy %luus", step, (unsigned long)copy_us);
}

/* E2-7 (3)'s precondition: without bit 30, does a piece still raise a
 * source (26 would reach the audio FIQ)?  And what ends it then. */
static bool e2_noirq_check(void)
{
    struct e2_piece p;
    struct e2_tally t;

    e2_piece_apple(&p);
    p.intr = false;
    e2_pp_run("E2-7(3) pre, no bit 30", &p, 4096, &t);
    if (e2->stop)
        return false;
    e2_learn(&p, &e2->nirq_how, &e2->nirq_int);
    if ((p.int_post & ~p.int_pre) & (1u << 26))
    {
        e2_raw("e2 E2-7(3): skipped: source 26 rises without bit 30");
        simplelist_addline("E2-7(3): src 26 rises: skip");
        return false;
    }
    return true;
}

static void e2_step7(void)
{
    int rc;
    bool sd;

    /* (1) R4 through the image's OPEN, then the FIFO's window */
    rc = bvp_open(&e2->b, 320, 240, 0);
    sd = rc == HP_OK && E2LAB_WIN_SDRAM >= e2->b.sess_fifo + 64 &&
         E2LAB_WIN_SDRAM + E2LAB_WIN_SIZE + 64 <=
             e2->b.sess_fifo + e2->b.sess_fifo_size;
    e2_raw("e2 E2-7(1): OPEN rc %d, FIFO %08lx + %lx: %s", rc,
           (unsigned long)e2->b.sess_fifo, (unsigned long)e2->b.sess_fifo_size,
           sd ? "window inside" : "skipped");
    if (sd)
        simplelist_addline("E2-7(1) SDRAM: %s",
                           e2_bcm_sizes("E2-7(1)", E2LAB_WIN_SDRAM) ?
                           "PASS" : "fail");
    else
        simplelist_addline("E2-7(1): no SDRAM (%d)", rc);
    if (e2->stop)
        return;
    e2_offload(false);
    if (e2->stop || !file_exists(STAGE7_FILE))
    {
        if (!e2->stop)
            simplelist_addline("E2-7(3): no test video");
        return;
    }
    if (!e2_noirq_check())
        return;
    {
        int repeat = global_settings.repeat_mode;
        bool pcm;

        global_settings.repeat_mode = REPEAT_OFF;
        playlist_create(NULL, NULL);
        playlist_insert_track(NULL, STAGE7_FILE, PLAYLIST_INSERT_LAST, false,
                              true);
        playlist_start(0, 0, 0);
        pcm = s8_wait_pcm(8000);
        e2_raw("e2 E2-7(3): PCM %s", pcm ? "running" : "NOT running");
        if (pcm)
            e2_offload(true);
        else
            simplelist_addline("E2-7(3): no PCM");
        audio_stop();
        global_settings.repeat_mode = repeat;
    }
}

/* The probe image (the plain video image, whose free SRAM the E2 windows
   were placed in) from the disk, this build's only; its handle or 0 */
static int e2_image(uint8_t **img)
{
    int fd = open(VPLAYER_PROBE_IMAGE, O_RDONLY), h;

    if (fd < 0)
        return 0;
    if (ffilesize(fd) != BCMPROBE_IMAGE_BYTES ||
        (h = core_alloc(BCMPROBE_IMAGE_BYTES)) <= 0)
    {
        close(fd);
        return 0;
    }
    core_pin(h);
    *img = core_get_data(h);
    if (read(fd, *img, BCMPROBE_IMAGE_BYTES) != BCMPROBE_IMAGE_BYTES ||
        bcmdt_fnv(*img, BCMPROBE_IMAGE_BYTES) != BCMPROBE_IMAGE_FNV)
    {
        close(fd);
        core_unpin(h);
        core_free(h);
        return 0;
    }
    close(fd);
    return h;
}

static void e2_run(uint8_t *img)
{
    uint32_t final[6];
    int rc;

    bcm_e2_save();
    if (!e2_step0())
        goto out;
    e2_step1();
    if (!e2->stop)
        e2_step2();
    if (e2->stop)
        goto out;

    /* E2-3: held between bootstrap and start, as Apple holds it */
    if (bcm_video_take() < 0)
    {
        e2_raw("e2 E2-3: the BCM could not be taken");
        simplelist_addline("BCM not available");
        goto out;
    }
    bcm_video_set_dma(false);       /* the CPU's fills: no PP DMA */
    bcm_video_off();
    if (bcm_e2_hold() == 0)
    {
        e2_raw("e2 E2-3: BCM held after the bootstrap, MHz %ld",
               cpu_frequency / 1000000);
        e2->pass3 = e2_bcm_sizes("E2-3", E2LAB_WIN_HELD);
        if (!e2->cut)
            e2_control_block("E2-3", E2LAB_WIN_HELD);
    }
    else
        e2_raw("e2 E2-3: skipped: the bootstrap did not answer");
    bcm_e2_cut();
    if (e2->stop)
        goto release;
    sleep(HZ / 10);

    /* E2-4 ... E2-7: our image running, idle */
    rc = bcm_video_boot(img, BCMPROBE_IMAGE_BYTES);
    memset(&e2->b, 0, sizeof(e2->b));
    if (rc == 0)
        rc = bvp_attach(&e2->b, &e2_port);
    if (rc == 0)
        rc = bvp_hello(&e2->b);
    e2->image = rc == HP_OK;
    e2_raw("e2 E2-4: our image %s (%d)", e2->image ? "idle" : "NOT up", rc);
    if (!e2->image)
    {
        simplelist_addline("E2-4: image not up (%d)", rc);
        goto release;
    }
    e2->pass4 = e2_bcm_sizes("E2-4", E2LAB_WIN_IDLE);
    if (!e2->stop && !e2->cut)
        e2_control_block("E2-4", E2LAB_WIN_IDLE);
    if (!e2->stop && e2->pass3 != 1 && e2->pass4 != 1)
        e2_step5();
    if (!e2->stop)
        e2_step6();
    if (!e2->stop && (e2->pass3 == 1 || e2->pass4 == 1))
        e2_step7();

release:
    if (e2->cut || !e2->image)
        bcm_e2_cut();               /* a guard or a dead port: cold */
    else
        bcm_video_off();
    bcm_video_release();            /* the stock wake path: the panel back */
out:
    bcm_e2_restore(final);
    e2_raw("e2 end: engine 2 master %08lx, ch0 %08lx, ch1 %08lx; general "
           "master %08lx; E2-1 %d, E2-3 %d, E2-4 %d%s", (unsigned long)final[0],
           (unsigned long)final[1], (unsigned long)final[2],
           (unsigned long)final[3], e2->pass1, e2->pass3, e2->pass4,
           e2->stop ? "; stopped early" : "");
    e2_raw("e2 end: INT_STAT[28:26] %lx at the start, %lx now",
           (unsigned long)(final[4] >> 26), (unsigned long)(final[5] >> 26));
    if (final[5] & ~final[4])
        simplelist_addline("Event still raised: REBOOT");
    simplelist_addline("E2-3 %s, E2-4 %s%s",
                       e2->pass3 < 0 ? "-" : e2->pass3 ? "PASS" : "fail",
                       e2->pass4 < 0 ? "-" : e2->pass4 ? "PASS" : "fail",
                       e2->stop ? " (stopped)" : "");
}

static bool devbcm_e2lab(void)
{
    struct simplelist_info info;
    uint8_t *img = NULL;
    int h, hi;
    uintptr_t a;

    simplelist_info_init(&info, "BCM E2 lab", 0, NULL);
    simplelist_reset_lines();
    if (!file_exists(VPLAYER_PROBE_IMAGE))
    {
        simplelist_addline("Not available: no probe image");
        return show(&info);
    }
    simplelist_addline("Second DMA engine lab: PP");
    simplelist_addline("memory, then the chip held,");
    simplelist_addline("then idle (~30 s, screen dark).");
    simplelist_addline("MENU: cancel");
    confirm_line = 4;
    simplelist_addline("Run the E2 lab now (SELECT here)");
    confirm_4b = false;
    info.action_callback = confirm_cb;
    simplelist_show_list(&info);
    if (!confirm_4b)
        return false;

    simplelist_info_init(&info, "BCM E2 lab", 0, NULL);
    simplelist_reset_lines();
    audio_stop();                   /* before the memory: nothing plays */
    sleep(HZ / 2);
    hi = e2_image(&img);
    h = hi ? core_alloc(sizeof(struct e2_lab) + 2 * E2LAB_PIECE_MAX +
                        8 * E2LAB_GUARD_WORDS + 128) : 0;
    if (h <= 0)
    {
        if (hi)
        {
            core_unpin(hi);
            core_free(hi);
        }
        simplelist_addline("Not available: %s", hi ? "no memory"
                                                   : "image not this build's");
        return show(&info);
    }
    core_pin(h);                    /* the engines see physical addresses */
    e2 = core_get_data(h);
    memset(e2, 0, sizeof(*e2));
    e2->pass1 = e2->pass3 = e2->pass4 = -1;
    a = ALIGN_UP((uintptr_t)(e2 + 1), 32);
    e2->src = (uint32_t *)a;
    e2->dst = e2->src + E2LAB_MAX_WORDS;
    e2->sdram = (uint32_t *)ALIGN_UP((uintptr_t)(e2->dst + E2LAB_MAX_WORDS +
                                     2 * E2LAB_GUARD_WORDS), 32);

    s6_t0 = USEC_TIMER;
    e2_raw("=== BCM test, E2 lab (raw): build %s, up %lu s ===", rbversion,
           (unsigned long)(current_tick / HZ));
    e2_run(img);

    e2 = NULL;
    core_unpin(h);
    core_free(h);
    core_unpin(hi);
    core_free(hi);
    return show(&info);
}

bool devbcm_cpu_e2lab(void) { return with_cpu_codec(devbcm_e2lab); }

#endif /* HAVE_BCMFW_VIDEO */
