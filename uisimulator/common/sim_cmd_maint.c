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
 * Simulator script commands: the paused maintenance window (Phase 16-1)
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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "sim_cmd_maint.h"

/* apps/ipodui/maint.c and apps/playback.c, shell builds only. A weak
 * undefined reference does not link on macOS (Mach-O), so the stock
 * simulator gets null hooks and each command reports 'unavailable'. */

#ifdef HAVE_IPOD_UI
void sim_boot_stamp_unclean(void);
void sim_boot_stamp_nostamp(void);
int sim_boot_stamp_walk(int *walks, const char **why);
static void (*volatile const h_unclean)(void) = sim_boot_stamp_unclean;
static void (*volatile const h_nostamp)(void) = sim_boot_stamp_nostamp;
static int (*volatile const h_walk)(int *, const char **) = sim_boot_stamp_walk;
void sim_maint_pause_age(long seconds);
void sim_maint_try(long bytes);
void sim_audio_give_hold(long ms);
int sim_maint_title(char *buf, int len);
void sim_maint_hold(const char *job, long bytes);
void sim_maint_drop(const char *job, bool ok);
unsigned long sim_maint_audio_size(void);
static void (*volatile const h_hold)(const char *, long) = sim_maint_hold;
static void (*volatile const h_drop)(const char *, bool) = sim_maint_drop;
static unsigned long (*volatile const h_audio)(void) = sim_maint_audio_size;
static void (*volatile const h_pause_age)(long) = sim_maint_pause_age;
static void (*volatile const h_try)(long) = sim_maint_try;
static void (*volatile const h_give_hold)(long) = sim_audio_give_hold;
static int (*volatile const h_title)(char *, int) = sim_maint_title;
#else
static void (*volatile const h_unclean)(void) = NULL;
static void (*volatile const h_nostamp)(void) = NULL;
static int (*volatile const h_walk)(int *, const char **) = NULL;
static void (*volatile const h_hold)(const char *, long) = NULL;
static void (*volatile const h_drop)(const char *, bool) = NULL;
static unsigned long (*volatile const h_audio)(void) = NULL;
static void (*volatile const h_pause_age)(long) = NULL;
static void (*volatile const h_try)(long) = NULL;
static void (*volatile const h_give_hold)(long) = NULL;
static int (*volatile const h_title)(char *, int) = NULL;
#endif

static unsigned long audio_marked;

bool sim_cmd_maint(const char *cmd, char *argv[], int argc, int lineno)
{
    /* Phase 16-2 (#89): "boot_unclean" (or "boot_crash"): this session
     * ends as a crash or a power cut does: nothing more is written, so no
     * clean shutdown's boot stamp; a stamp the settled library already
     * wrote stays, as on the device (npfix H1). "boot_nostamp": test
     * setup, the next boot finds no stamp (removed now, none after).
     * "expect_boot_walk <0|1> [ms] [why]": waits up to ms (default 15000)
     * for this boot's decision and checks it (0: the walk was skipped and
     * no scan ran; 1: it walked), and the reason's name if given. */
    if (!strcmp(cmd, "boot_unclean") || !strcmp(cmd, "boot_crash"))
    {
        if (h_unclean)
            h_unclean();
        fprintf(stderr, "sim_script: %s%s\n", cmd,
                h_unclean ? "" : " unavailable");
        return true;
    }
    if (!strcmp(cmd, "boot_nostamp"))
    {
        if (h_nostamp)
            h_nostamp();
        fprintf(stderr, "sim_script: boot_nostamp%s\n",
                h_nostamp ? "" : " unavailable");
        return true;
    }
    if (!strcmp(cmd, "expect_walks_ge") && argc >= 2)
    {
        /* expect_walks_ge <n> [ms]: at least n scans ran this boot */
        int walks = 0;
        long ms = argc >= 3 ? atol(argv[2]) : 15000, t;
        const char *why = "-";

        for (t = 0; h_walk; t += 50)
        {
            h_walk(&walks, &why);
            if (walks >= atoi(argv[1]) || t >= ms)
                break;
            sleep(HZ / 20);
        }
        fprintf(stderr, walks >= atoi(argv[1]) ? "sim_script: expect_walks_ge "
                "ok walks=%d\n" : "sim_script: expect_walks_ge failed: "
                "walks=%d\n", walks);
        return true;
    }
    if (!strcmp(cmd, "expect_boot_walk") && argc >= 2)
    {
        int want = atoi(argv[1]), got = -1, walks = 0;
        long ms = argc >= 3 ? atol(argv[2]) : 15000, t;
        const char *why = "-";

        if (!h_walk)
        {
            fprintf(stderr, "sim_script: expect_boot_walk unavailable\n");
            return true;
        }
        for (t = 0; ; t += 50)
        {
            got = h_walk(&walks, &why);
            if (got >= 0 || t >= ms)
                break;
            sleep(HZ / 20);             /* the kernel's: other threads run */
        }
        if (got == want && (want || walks == 0)
            && (argc < 4 || !strcmp(why, argv[3])))
            fprintf(stderr, "sim_script: expect_boot_walk ok walk=%d why=%s "
                    "walks=%d\n", got, why, walks);
        else
            fprintf(stderr, "sim_script:%d: expect_boot_walk failed: walk=%d "
                    "why=%s walks=%d (want %s%s%s)\n", lineno, got, why, walks,
                    argv[1], argc >= 4 ? " " : "", argc >= 4 ? argv[3] : "");
        return true;
    }
    if (!strcmp(cmd, "maint_try") && argc >= 2)
    {
        if (h_try)
            h_try(atol(argv[1]));
        fprintf(stderr, "sim_script: maint_try %s\n", argv[1]);
        return true;
    }
    /* P16 gate fix: "maint_hold <job> <bytes>" takes the window for a
     * tenant (commit, chapters, dbq, lists) and holds the allocation;
     * "maint_drop <job> [ok]" frees it and ends that tenancy only.
     * "audio_mark" remembers the audio file buffer's size;
     * "expect_audio_back [ms]" waits (default 10000) until it is back
     * to within 64 KB of the mark (the regrow): "expect_audio_back ok"
     * / "... failed" */
    if (!strcmp(cmd, "maint_hold") && argc >= 3)
    {
        if (h_hold)
            h_hold(argv[1], atol(argv[2]));
        fprintf(stderr, "sim_script: maint_hold %s %s%s\n", argv[1], argv[2],
                h_hold ? "" : " unavailable");
        return true;
    }
    if (!strcmp(cmd, "maint_drop") && argc >= 2)
    {
        if (h_drop)
            h_drop(argv[1], argc < 3 || atoi(argv[2]) != 0);
        fprintf(stderr, "sim_script: maint_drop %s%s\n", argv[1],
                h_drop ? "" : " unavailable");
        return true;
    }
    if (!strcmp(cmd, "audio_mark"))
    {
        audio_marked = h_audio ? h_audio() : 0;
        fprintf(stderr, "sim_script: audio_mark %lu\n", audio_marked);
        return true;
    }
    if (!strcmp(cmd, "expect_audio_back"))
    {
        long ms = argc >= 2 ? atol(argv[1]) : 10000, t;
        unsigned long now = 0;

        for (t = 0; h_audio; t += 50)
        {
            now = h_audio();
            if (now + 65536 >= audio_marked || t >= ms)
                break;
            sleep(HZ / 20);
        }
        if (h_audio && now + 65536 >= audio_marked)
            fprintf(stderr, "sim_script: expect_audio_back ok audio=%lu "
                    "mark=%lu\n", now, audio_marked);
        else
            fprintf(stderr, "sim_script:%d: expect_audio_back failed: "
                    "audio=%lu mark=%lu\n", lineno, now, audio_marked);
        return true;
    }
    if (!strcmp(cmd, "give_hold") && argc >= 2)
    {
        if (h_give_hold)
            h_give_hold(atol(argv[1]));
        fprintf(stderr, "sim_script: give_hold %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "expect_title"))
    {
        char buf[160];

        if (!h_title)
            fprintf(stderr, "sim_script: expect_title unavailable\n");
        else if (h_title(buf, sizeof(buf)))
            fprintf(stderr, "sim_script: expect_title ok: %s\n", buf);
        else
            fprintf(stderr, "sim_script:%d: expect_title failed: %s\n",
                    lineno, buf);
        return true;
    }
    if (strcmp(cmd, "pause_age"))
        return false;
    if (argc < 2)
    {
        fprintf(stderr, "sim_script:%d: pause_age <seconds>\n", lineno);
        return true;
    }
    if (!h_pause_age)
    {
        fprintf(stderr, "sim_script: pause_age unavailable\n");
        return true;
    }
    h_pause_age(atol(argv[1]));
    fprintf(stderr, "sim_script: pause_age %ld\n", atol(argv[1]));
    return true;
}
