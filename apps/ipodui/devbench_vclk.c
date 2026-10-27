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

/* The bench session's clock rows (devbench_vclk.h): the device side.
 * Called from devbench.c's step table on the UI thread; each returns an
 * enum bench_res.  Lines go through devbcm_log() (bcmtest.log and the
 * step's hook, so the results file keeps them).  Both rows put the player
 * back to the governor over the verified bands. */
#include <stdio.h>
#include <string.h>
#include "config.h"

#if defined(HAVE_BCMFW_VIDEO) && !defined(SIMULATOR)
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "rbpaths.h"
#include "core_alloc.h"
#include "audio.h"
#include "playlist.h"
#include "settings.h"
#include "bcm-devtest.h"
#include "devbcm.h"
#include "devbench.h"
#include "devbench_vclk.h"
#include "video/vplayer.h"
#include "video/bcmvideo_image.h"
#include "video/vpcore.h"
#include "video/vtrace.h"

#define CLIP1   ROCKBOX_DIR "/bcmtest/clip.mp4"
#define CLIP2   ROCKBOX_DIR "/bcmtest/clip2.mp4"
#define RECORDS 1024

/* ---- a play ------------------------------------------------------------- */

static struct vtrace trace;
static int trace_h;

static void on_frame(const struct vpc_frame *f)
{
    vtrace_add(&trace, f);
}

/* steps during a governor phase: a seek at 5 s, a pause at 10 s for 1 s */
struct play
{
    bool started, ended, failed, raced_seek, raced_resume, have_vi;
    struct vplayer_info vi;
    struct vtrace_sum sum;
};

static void seek_to(unsigned long ms)
{
    audio_pre_ff_rewind();
    audio_ff_rewind(ms);
    vplayer_seek(ms);
}

/* whether the chip is at the top band within a second (it raced), or was
 * there already */
static bool raced(int top, uint32_t before)
{
    struct vplayer_info vi;
    int k;

    if ((int)before == top)
        return true;
    for (k = 0; k < 10; k++)
    {
        sleep(HZ / 10);
        vplayer_pump();
        if (vplayer_info(&vi) && (int)vi.vclk_band == top)
            return true;
    }
    return false;
}

/* path for seconds (looped by a seek to 0 at its end), at a band held
 * (band >= 0) or under the governor (-1, with the steps) */
static void play(const char *path, int band, int seconds, bool steps,
                 struct play *p)
{
    struct vplayer_info vi;
    int s, repeat = global_settings.repeat_mode;
    int top = 0, k;

    memset(p, 0, sizeof(*p));
    for (k = 0; k < 12; k++)
        if (vplayer_vclk_mask() >> k & 1)
            top = k;
    vtrace_init(&trace, core_get_data(trace_h), RECORDS);
    vplayer_set_frame_hook(on_frame);
    vplayer_vclk_force(band);
    p->started = vplayer_start(path);
    if (p->started)
    {
        for (k = 0; k < 12; k++)
            if (vplayer_vclk_mask() >> k & 1)
                top = k;
        global_settings.repeat_mode = REPEAT_ONE;
        playlist_create(NULL, NULL);
        playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST, false, true);
        playlist_start(0, 0, 0);
        for (s = 0; s < seconds * 10; s++)
        {
            vplayer_pump();
            sleep(HZ / 10);
            if (!vplayer_info(&vi) || vi.state == VPC_FAILED)
            {
                p->failed = true;
                break;
            }
            if (steps && s == 50)
            {
                seek_to(1000);
                p->raced_seek = raced(top, vi.vclk_band);
            }
            if (steps && s == 100)
            {
                uint32_t before = vi.vclk_band;

                audio_pause();
                sleep(HZ);
                audio_resume();
                p->raced_resume = raced(top, before);
            }
        }
        p->have_vi = vplayer_info(&p->vi);
        p->ended = p->have_vi && p->vi.ended;
    }
    vplayer_stop();
    audio_stop();
    global_settings.repeat_mode = repeat;
    vplayer_set_frame_hook(NULL);
    vplayer_vclk_force(-1);
    vtrace_summary(&trace, p->ended, p->failed || !p->started, &p->sum);
}

static bool begin(const char *tag)
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
    if (trace_h > 0)
        core_pin(trace_h);
    else
    {
        snprintf(line, sizeof(line), "%s: Not available: no memory", tag);
        devbcm_log(line);
        return false;
    }
    return true;
}

static void end(void)
{
    if (trace_h > 0)
        core_free(trace_h);
    trace_h = 0;
}

static uint32_t est(const struct vplayer_info *vi)
{
    static const uint8_t mhz[12] =
        { 27, 46, 59, 74, 87, 102, 116, 123, 130, 140, 147, 150 };
    static const uint8_t lvl[12] =
        { 11, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

    return bvc_est_ma100(vi->vclk_band_ms, mhz, lvl, 12);
}

/* ---- vbv: the bands, one by one ----------------------------------------- */

int devbench_vclk_verify(void)
{
    char line[120], flags[400];
    struct play p;
    int b, n = 0, ok = 0, fd;
    bool cap = false;

    if (!begin("vbv"))
        return BENCH_SKIP;
    flags[0] = '\0';
    for (b = 1; b < VCLK_BANDS; b++)
    {
        bool pass;

        play(CLIP1, b, BVC_VERIFY_S, false, &p);
        if (p.have_vi && p.vi.vclk_on)
            cap = true;
        if (!cap)
            break;              /* the image has no bands: say so once */
        pass = p.have_vi && !p.failed &&
               bvc_verify_ok(p.vi.vclk_band, (uint32_t)b, p.vi.vclk_fails,
                             p.sum.shown, p.vi.errors, p.vi.concealed,
                             p.vi.recoveries);
        snprintf(line, sizeof(line), "vbv %u MHz level %u: %s (ran %u MHz, "
                 "fallbacks %lu, shown %lu, errors %lu, concealed %lu, "
                 "misses %lu)", vclk_band_mhz[b], vclk_band_level[b],
                 pass ? "PASS" : "FAIL",
                 p.have_vi ? vclk_band_mhz[p.vi.vclk_band % 12] : 0,
                 (unsigned long)p.vi.vclk_fails, (unsigned long)p.sum.shown,
                 (unsigned long)p.vi.errors, (unsigned long)p.vi.concealed,
                 (unsigned long)p.vi.vclk_misses);
        devbcm_log(line);
        n++;
        if (pass && b != VCLK_TOP)
        {
            /* 150 MHz is stages 4b and 4c's flag */
            size_t k = strlen(flags);

            vclk_flag_line(flags + k, sizeof(flags) - k, b,
                           BCMVIDEO_IMAGE_BYTES, BCMVIDEO_IMAGE_FNV);
            ok++;
        }
        else if (pass)
            ok++;
    }
    end();
    if (!cap)
    {
        devbcm_log("vbv: Not available: the video image has no clock bands "
                   "(HP_CAP_VCLK: the next image batch)");
        return BENCH_SKIP;
    }
    fd = open(VPLAYER_VCLK_FLAGS, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0)
    {
        write(fd, flags, strlen(flags));
        close(fd);
    }
    snprintf(line, sizeof(line), "vbv: %s: %d of %d bands hold; flags %s",
             ok == n ? "PASS" : "FAIL", ok, n,
             fd >= 0 ? "written" : "NOT written");
    devbcm_log(line);
    return ok == n ? BENCH_PASS : BENCH_FAIL;
}

/* ---- vgov: the governor against fixed 150 MHz --------------------------- */

static void gov_line(const char *clip, const char *what, const struct play *p)
{
    char line[160];

    snprintf(line, sizeof(line), "vgov %s %s: shown %lu skipped %lu GOPs "
             "dropped %lu A/V max %ld us hitches %lu; mhz=%lu lvl=%lu.%lu "
             "idle=%lu bands=%lu clocks=%lu est=%lu.%02lu mA at the bands' "
             "tops [estimate]", clip, what, (unsigned long)p->sum.shown,
             (unsigned long)p->sum.skipped,
             (unsigned long)p->sum.dropped_gops, (long)p->sum.drift_max_us,
             (unsigned long)p->sum.hitches, (unsigned long)p->vi.vclk_mhz,
             (unsigned long)p->vi.vclk_lvl10 / 10,
             (unsigned long)p->vi.vclk_lvl10 % 10,
             (unsigned long)p->vi.vclk_idle_pm,
             (unsigned long)p->vi.vclk_changes,
             (unsigned long)p->vi.vclk_clock_changes,
             (unsigned long)est(&p->vi) / 100,
             (unsigned long)est(&p->vi) % 100);
    devbcm_log(line);
}

int devbench_vclk_gov(void)
{
    static const char *const clips[2] = { CLIP1, CLIP2 };
    static const char *const names[2] = { "clip1", "clip2" };
    char line[160];
    struct play a, b, q;
    int c, ran = 0, ok = 0;

    if (!begin("vgov"))
        return BENCH_SKIP;
    if ((vplayer_vclk_mask() & ~0x801u) == 0)
        devbcm_log("vgov: only the crystal and 150 MHz verified so far: "
                   "the governor has nothing between (run vbv first)");
    for (c = 0; c < 2; c++)
    {
        bool pass;

        if (!file_exists(clips[c]))
            continue;
        /* fixed 150, then the governor with each in-band policy: the
           band's top (the default) and proportional (the A/B) */
        play(clips[c], VCLK_TOP, BVC_GOV_S, true, &a);
        vplayer_vclk_policy(VCLK_POLICY_TOP);
        play(clips[c], -1, BVC_GOV_S, true, &b);
        vplayer_vclk_policy(VCLK_POLICY_PROP);
        play(clips[c], -1, BVC_GOV_S, true, &q);
        vplayer_vclk_policy(VCLK_POLICY_TOP);
        if (!b.have_vi || !b.vi.vclk_on)
        {
            end();
            devbcm_log("vgov: Not available: the video image has no clock "
                       "bands (HP_CAP_VCLK: the next image batch)");
            return BENCH_SKIP;
        }
        gov_line(names[c], "150", &a);
        gov_line(names[c], "top", &b);
        gov_line(names[c], "prop", &q);
        /* the proportional policy's verdict: informational (the A/B) */
        snprintf(line, sizeof(line), "vgov %s prop: %s (misses %lu, A/V max "
                 "%ld us, %lu bands, %lu clocks; informational)", names[c],
                 !q.failed && bvc_gov_ok(q.sum.skipped + q.sum.dropped_gops,
                                         a.sum.skipped + a.sum.dropped_gops,
                                         q.sum.drift_max_us,
                                         a.sum.drift_max_us, q.sum.hitches,
                                         a.sum.hitches, q.vi.vclk_changes,
                                         q.vi.vclk_ms, q.raced_seek &&
                                         q.raced_resume) ? "pass" : "fail",
                 (unsigned long)(q.sum.skipped + q.sum.dropped_gops),
                 (long)q.sum.drift_max_us, (unsigned long)q.vi.vclk_changes,
                 (unsigned long)q.vi.vclk_clock_changes);
        devbcm_log(line);
        pass = !b.failed &&
               bvc_gov_ok(b.sum.skipped + b.sum.dropped_gops,
                          a.sum.skipped + a.sum.dropped_gops,
                          b.sum.drift_max_us, a.sum.drift_max_us,
                          b.sum.hitches, a.sum.hitches,
                          b.vi.vclk_changes, b.vi.vclk_ms,
                          b.raced_seek && b.raced_resume);
        snprintf(line, sizeof(line), "vgov %s: %s (misses %lu vs %lu, A/V "
                 "max %ld vs %ld us, hitches %lu vs %lu at 150, %lu changes "
                 "in %lu s, top after the seek %s and the resume %s)",
                 names[c], pass ? "PASS" : "FAIL",
                 (unsigned long)(b.sum.skipped + b.sum.dropped_gops),
                 (unsigned long)(a.sum.skipped + a.sum.dropped_gops),
                 (long)b.sum.drift_max_us, (long)a.sum.drift_max_us,
                 (unsigned long)b.sum.hitches,
                 (unsigned long)a.sum.hitches,
                 (unsigned long)b.vi.vclk_changes,
                 (unsigned long)b.vi.vclk_ms / 1000,
                 b.raced_seek ? "yes" : "no", b.raced_resume ? "yes" : "no");
        devbcm_log(line);
        ran++;
        ok += pass;
    }
    end();
    snprintf(line, sizeof(line), "vgov: %s: %d of %d clips",
             ran && ok == ran ? "PASS" : "FAIL", ok, ran);
    devbcm_log(line);
    return ran && ok == ran ? BENCH_PASS : BENCH_FAIL;
}

#endif /* HAVE_BCMFW_VIDEO && !SIMULATOR */
