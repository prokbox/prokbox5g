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
 * Revision 2 sim script command "record"; see sim_cmd_record.h.
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
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "thread.h"
#include "screendump.h"
#include "sim_cmd_record.h"

extern const char *sim_shot_dir;        /* sim_script.c; an OS path */
int sim_get_os_path(char *buffer, const char *path, size_t bufsize);

#define RECORD_TMP        "/.sim_script_record.bmp"
#define RECORD_NAME_MAX   64
#define RECORD_FPS_MIN    1
#define RECORD_FPS_MAX    30

/* Continues the frame sequence across more than one "record" of the same
 * name (a video built from several bursts); a new name starts over. */
static char last_name[RECORD_NAME_MAX];
static int next_frame;

static bool dump_frame(const char *dir, int n)
{
    char ospath[PATH_MAX];
    char dest[PATH_MAX + 32];       /* dir (<= PATH_MAX) + "/frameNNNNN.bmp" */

    snprintf(dest, sizeof(dest), "%s/frame%05d.bmp", dir, n);
    if (!screen_dump_to(RECORD_TMP)
        || sim_get_os_path(ospath, RECORD_TMP, sizeof(ospath)) < 0
        || rename(ospath, dest) != 0)
        return false;
    return true;
}

static void sleep_ms(long ms)
{
    if (ms > 0)
        sleep((ms * HZ + 999) / 1000);
}

static void record(long ms, long fps, const char *name, int lineno)
{
    char dir[PATH_MAX];
    long i, n, prev_ms = 0, frames = 0;

    if (ms <= 0 || !name[0])
    {
        fprintf(stderr, "sim_script:%d: record <ms> <fps> <name>\n", lineno);
        return;
    }
    if (fps < RECORD_FPS_MIN)
        fps = RECORD_FPS_MIN;
    if (fps > RECORD_FPS_MAX)
        fps = RECORD_FPS_MAX;

    snprintf(dir, sizeof(dir), "%s/%.*s_frames", sim_shot_dir,
             RECORD_NAME_MAX - 8, name);
    if (strcmp(name, last_name))
    {
        mkdir(dir, 0755);           /* ok if it already exists */
        snprintf(last_name, sizeof(last_name), "%s", name);
        next_frame = 0;
    }

    /* n frames span [0, ms); target_ms(i) = (i+1)*1000/fps rounds down,
     * so sleeping each frame's delta from the previous one's target
     * (not a fixed 1000/fps every time) keeps a non-dividing fps (24,
     * e.g.) from drifting over a long recording. */
    n = ms * fps / 1000;
    for (i = 0; i < n; i++)
    {
        long target_ms;

        if (!dump_frame(dir, next_frame))
        {
            fprintf(stderr, "sim_script:%d: record: failed to write "
                    "frame %d\n", lineno, next_frame);
            return;
        }
        next_frame++;
        frames++;
        target_ms = (i + 1) * 1000 / fps;
        sleep_ms(target_ms - prev_ms);
        prev_ms = target_ms;
    }
    fprintf(stderr, "sim_script: record %s %ld frames (%ld ms at %ld fps)\n",
            name, frames, ms, fps);
}

/* "record_start"/"record_stop": the same frame dump on a thread of its
 * own, so the script keeps pressing keys and turning the wheel while it
 * runs (a navigation clip: pushes, pops and a wheel glide as they play
 * out). Frame i is due at start + i/fps on the tick; a frame the host was
 * too late for repeats the last dump (a hard link) instead of shifting
 * every later frame, so the clip keeps the real timing. */
#define BG_TMP            "/.sim_script_record_bg.bmp"
static long bg_stack[(DEFAULT_STACK_SIZE * 2) / sizeof(long)];
static unsigned int bg_thread = 0;
static volatile bool bg_stop;
static char bg_dir[PATH_MAX];
static long bg_fps;
static long bg_frames, bg_repeats;
static bool bg_failed;

static bool bg_dump(int n)
{
    char ospath[PATH_MAX];
    char dest[PATH_MAX + 32];

    snprintf(dest, sizeof(dest), "%s/frame%05d.bmp", bg_dir, n);
    return screen_dump_to(BG_TMP)
        && sim_get_os_path(ospath, BG_TMP, sizeof(ospath)) >= 0
        && rename(ospath, dest) == 0;
}

static bool bg_repeat(int n)
{
    char src[PATH_MAX + 32], dest[PATH_MAX + 32];

    snprintf(src, sizeof(src), "%s/frame%05d.bmp", bg_dir, n - 1);
    snprintf(dest, sizeof(dest), "%s/frame%05d.bmp", bg_dir, n);
    return link(src, dest) == 0;
}

static void bg_record_thread(void)
{
    long start = current_tick;
    long i = 0;

    while (!bg_stop)
    {
        long due;

        if (!bg_dump(next_frame))
        {
            bg_failed = true;
            break;
        }
        next_frame++;
        bg_frames++;
        i++;
        /* slots the host overslept: repeat the frame, keep the clock */
        while (!bg_stop && TIME_AFTER(current_tick,
                                      start + (i + 1) * HZ / bg_fps))
        {
            if (!bg_repeat(next_frame))
            {
                bg_failed = true;
                break;
            }
            next_frame++;
            bg_repeats++;
            i++;
        }
        if (bg_failed)
            break;
        due = start + i * HZ / bg_fps;
        if (TIME_BEFORE(current_tick, due))
            sleep(due - current_tick);
    }
}

static void record_start(long fps, const char *name, int lineno)
{
    if (bg_thread)
    {
        fprintf(stderr, "sim_script:%d: record_start: already recording\n",
                lineno);
        return;
    }
    if (!name[0] || fps < RECORD_FPS_MIN)
    {
        fprintf(stderr, "sim_script:%d: record_start <fps> <name>\n",
                lineno);
        return;
    }
    if (fps > RECORD_FPS_MAX)
        fps = RECORD_FPS_MAX;
    /* ticks per frame must be whole for steady pacing (HZ 100: 10, 20,
     * 25 fps); others still work, with a +-1 tick jitter */
    bg_fps = fps;
    snprintf(bg_dir, sizeof(bg_dir), "%s/%.*s_frames", sim_shot_dir,
             RECORD_NAME_MAX - 8, name);
    if (strcmp(name, last_name))
    {
        mkdir(bg_dir, 0755);
        snprintf(last_name, sizeof(last_name), "%s", name);
        next_frame = 0;
    }
    bg_stop = false;
    bg_failed = false;
    bg_frames = bg_repeats = 0;
    bg_thread = create_thread(bg_record_thread, bg_stack, sizeof(bg_stack),
                              0, "sim record" IF_PRIO(, PRIORITY_REALTIME_1)
                              IF_COP(, CPU));
    if (!bg_thread)
        fprintf(stderr, "sim_script:%d: record_start: no thread\n", lineno);
}

static void record_stop(int lineno)
{
    if (!bg_thread)
    {
        fprintf(stderr, "sim_script:%d: record_stop: not recording\n",
                lineno);
        return;
    }
    bg_stop = true;
    thread_wait(bg_thread);
    bg_thread = 0;
    if (bg_failed)
        fprintf(stderr, "sim_script:%d: record: failed to write frame %d\n",
                lineno, next_frame);
    fprintf(stderr, "sim_script: record %s %ld frames (%ld repeated, "
            "%ld fps, background)\n", last_name, bg_frames + bg_repeats,
            bg_repeats, bg_fps);
}

bool sim_cmd_record(const char *cmd, char *argv[], int argc, int lineno)
{
    if (!strcmp(cmd, "record") && argc >= 4)
    {
        record(atol(argv[1]), atol(argv[2]), argv[3], lineno);
        return true;
    }
    if (!strcmp(cmd, "record_start") && argc >= 3)
    {
        record_start(atol(argv[1]), argv[2], lineno);
        return true;
    }
    if (!strcmp(cmd, "record_stop"))
    {
        record_stop(lineno);
        return true;
    }
    return false;
}
