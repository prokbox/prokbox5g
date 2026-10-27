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
#include "config.h"
#include "system.h"
#include "kernel.h"
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

bool sim_cmd_record(const char *cmd, char *argv[], int argc, int lineno)
{
    if (!strcmp(cmd, "record") && argc >= 4)
    {
        record(atol(argv[1]), atol(argv[2]), argv[3], lineno);
        return true;
    }
    return false;
}
