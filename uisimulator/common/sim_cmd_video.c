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
 * Video player sim script commands (see sim_cmd_video.h).
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "sim_cmd_video.h"

#ifdef HAVE_BCMFW_VIDEO
#include <fcntl.h>
#include "audio.h"
#include "thread.h"
#include "core_alloc.h"
#include "sim_video.h"
#include "video/vplayer.h"
#include "video/vpcore.h"
#include "video/vtrace.h"
#ifdef HAVE_IPOD_UI
#include "ipodui/screens/video.h"
#endif

static const char *const states[] = { "idle", "playing", "asleep", "done",
                                      "failed" };

/* stage 6's trace (apps/ipodui/devbcm.c on the device) in the sim:
   video_trace on (before video_start), video_trace expect pass|fail */
static struct vtrace trace;
static struct vtrace_rec trace_rec[2048];

static void trace_frame(const struct vpc_frame *f)
{
    vtrace_add(&trace, f);
}

static void trace_line(void *ctx, const char *text)
{
    (void)ctx;
    if (strncmp(text, "V6 ", 3) || getenv("SIM_V6"))
        fprintf(stderr, "sim_script: %s\n", text);
}

/* script paths can't hold spaces: "%20" stands for one, as in
   sim_script.c's other path commands */
static void unescape(char *p)
{
    char *w = p;

    while (*p)
    {
        if (p[0] == '%' && p[1] == '2' && p[2] == '0')
        {
            *w++ = ' ';
            p += 3;
        }
        else
            *w++ = *p++;
    }
    *w = '\0';
}

bool sim_cmd_video(const char *cmd, char *argv[], int argc, int lineno)
{
    if ((!strcmp(cmd, "video_start") || !strcmp(cmd, "video_screen")) &&
        argc >= 2)
        unescape(argv[1]);
    if (!strcmp(cmd, "video_trace") && argc >= 2)
    {
        if (!strcmp(argv[1], "on"))
        {
            vtrace_init(&trace, trace_rec, sizeof(trace_rec) / sizeof(trace_rec[0]));
            vplayer_set_frame_hook(trace_frame);
            fprintf(stderr, "sim_script: video_trace on\n");
        }
        else if (!strcmp(argv[1], "expect") && argc >= 3)
        {
            struct vplayer_info info;
            struct vtrace_sum sum;
            bool want = !strcmp(argv[2], "pass");

            vplayer_info(&info);
            vtrace_summary(&trace, info.ended, info.state == VPC_FAILED, &sum);
            vtrace_lines(&trace, &sum, trace_line, NULL);
            vplayer_set_frame_hook(NULL);
            fprintf(stderr, "sim_script:%d: video_trace expect %s: %s\n", lineno,
                    argv[2], sum.pass == want && sum.records > 0 ? "ok" : "failed");
        }
        return true;
    }
#ifdef HAVE_IPOD_UI
    if (!strcmp(cmd, "video_screen") && argc >= 2)
    {
        /* the shell's video screen on a sim path, as the Movies list
           reaches it: ui_video_play(path, tc_idx); tc_idx is the 0-based
           tagcache master index, -1 (default) = none */
        int tc = argc >= 3 ? atoi(argv[2]) : -1;

        fprintf(stderr, "sim_script:%d: video_screen %s tc=%d\n", lineno,
                argv[1], tc);
        ui_video_play(argv[1], tc);
        return true;
    }
    if (!strcmp(cmd, "setvideos"))
    {
        fprintf(stderr, "sim_script:%d: setvideos\n", lineno);
        ui_setvideos_push();
        return true;
    }
#endif
    if (!strcmp(cmd, "video_start") && argc >= 2)
    {
        /* video_start <path> [stage]: "stage" plays it as a bench step
           does (vplayer_bench_play), so its image is booted from the
           file and its stop leaves the chip on Apple's vmcs (sim_video.h
           sim_video_resident) */
        bool stage = argc >= 3 && !strcmp(argv[2], "stage");
        bool ok;

        if (stage)
            vplayer_bench_play(true);
        ok = vplayer_start(argv[1]);
        if (stage)
            vplayer_bench_play(false);
        fprintf(stderr, "sim_script:%d: video_start %s%s: %s\n", lineno,
                argv[1], stage ? " stage" : "", ok ? "started" : "refused");
        return true;
    }
    if (!strcmp(cmd, "video_seek") && argc >= 2)
    {
        /* the audio and the player to ms, as the shell's seeks do */
        unsigned long ms = strtoul(argv[1], NULL, 10);

        audio_pre_ff_rewind();
        audio_ff_rewind(ms);
        vplayer_seek(ms);
        fprintf(stderr, "sim_script:%d: video_seek %lu\n", lineno, ms);
        return true;
    }
    if (!strcmp(cmd, "audio_pause") && argc >= 2)
    {
        /* the audio the video follows, without the shell's keys */
        if (!strcmp(argv[1], "on"))
            audio_pause();
        else
            audio_resume();
        fprintf(stderr, "sim_script: audio_pause %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "wheel_touch"))
    {
        /* a finger set on the wheel without turning it */
        extern void sim_wheel_touch(void);
        extern bool sim_sleep_touch(void);  /* sim_cmd_sleep.c (P18) */
        if (!sim_sleep_touch())
            sim_wheel_touch();
        fprintf(stderr, "sim_script:%d: wheel_touch\n", lineno);
        return true;
    }
    if (!strcmp(cmd, "video_res"))
    {
        /* video_res: what a play could leak, against the first call's
           counts (ipodui-p19-play10): thread slots in use, core bytes
           free, host fds open */
        static int base_thr = -1, base_fds;
        static long base_core;
        struct thread_debug_info ti;
        int thr = 0, fds = 0;
        long core = (long)core_available();
        unsigned int i;

        for (i = 0; i < MAXTHREADS; i++)
            if (thread_get_debug_info(i, &ti) > 0)
                thr++;
        for (i = 0; i < 1024; i++)
            if (fcntl((int)i, F_GETFD) != -1)
                fds++;
        if (base_thr < 0)
        {
            base_thr = thr;
            base_fds = fds;
            base_core = core;
        }
        fprintf(stderr, "sim_script:%d: video_res threads %d core %ld fds %d"
                " drift threads %d core %ld fds %d\n", lineno, thr, core,
                fds, thr - base_thr, base_core - core, fds - base_fds);
        return true;
    }
    if (!strcmp(cmd, "video_stop"))
    {
        vplayer_stop();
        fprintf(stderr, "sim_script: video_stop\n");
        return true;
    }
    if (!strcmp(cmd, "video_shot") && argc >= 2)
    {
        if (!sim_video_shot(argv[1]))
            fprintf(stderr, "sim_script:%d: video_shot %s failed\n", lineno,
                    argv[1]);
        return true;
    }
    if (!strcmp(cmd, "expect_video") && argc >= 2)
    {
        struct vplayer_info info;
        uint32_t min = (uint32_t)atol(argv[1]);
        const char *st;
        bool ok;

        vplayer_info(&info);
        st = info.state >= 0 && info.state < 5 ? states[info.state] : "none";
        /* "done" also once the end was reached and the audio stopped */
        ok = sim_video_frames() >= min && !sim_video_errors() &&
             (argc < 3 || !strcmp(argv[2], st) ||
              (!strcmp(argv[2], "done") && info.ended));
        fprintf(stderr, "sim_script:%d: expect_video %s: %lu pictures, %lu "
                "errors, %s%s; %lu restarts, %lu wakes, lead %lu us, %lu "
                "direct reads\n", lineno, ok ? "ok" : "failed",
                (unsigned long)sim_video_frames(),
                (unsigned long)sim_video_errors(), st,
                info.ended ? " (ended)" : "",
                (unsigned long)info.restarts, (unsigned long)info.wakes,
                (unsigned long)info.lead_us, (unsigned long)info.direct_reads);
        return true;
    }
    return false;
}
#else
bool sim_cmd_video(const char *cmd, char *argv[], int argc, int lineno)
{
    (void)cmd;
    (void)argv;
    (void)argc;
    (void)lineno;
    return false;
}
#endif
