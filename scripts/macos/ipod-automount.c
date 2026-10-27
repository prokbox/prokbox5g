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
 * ipod-automount: mounts a ProkBox iPod's FAT32 volume (label IPOD) at
 * /private/tmp/ipod_mount with mount_msdos.
 *
 * macOS's own automount gives up on the iPod's 1 TB volume: its FAT driver
 * reads the whole allocation table (~250 MB) before mounting, and the
 * iPod's USB is too slow to finish inside the time limit. mount_msdos
 * doesn't read the table, so it mounts at once.
 *
 * Runs as root under launchd (dev.prokopto.ipod-automount.plist). It is a
 * compiled program rather than a script so that Full Disk Access can be
 * granted to it alone: macOS refuses background mounts without it.
 * Checks every 3 s; it only mounts, never writes, unmounts or repairs.
 *
 *   build:   cc -O2 -o ipod-automount ipod-automount.c
 *   pause:   touch /tmp/ipod-automount.pause   (resume: rm it)
 *   log:     /var/log/ipod-automount.log
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
#include <time.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>

#define MNT     "/private/tmp/ipod_mount"
#define PAUSE   "/tmp/ipod-automount.pause"

extern char **environ;

static void logmsg(const char *fmt, const char *arg)
{
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof(ts), "%F %T", localtime(&t));
    printf("%s ", ts);
    printf(fmt, arg);
    printf("\n");
    fflush(stdout);
}

/* /dev/<dev> is mounted somewhere, or something is mounted on MNT */
static int mounted(const char *dev, int *mnt_busy)
{
    struct statfs *m;
    int n = getmntinfo(&m, MNT_NOWAIT), hit = 0;
    char path[64];

    snprintf(path, sizeof(path), "/dev/%s", dev);
    *mnt_busy = 0;
    for (int i = 0; i < n; i++)
    {
        if (!strcmp(m[i].f_mntfromname, path))
            hit = 1;
        if (!strcmp(m[i].f_mntonname, MNT))
            *mnt_busy = 1;
    }
    return hit;
}

static int run(char *const argv[])
{
    pid_t pid;
    int st;

    if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ))
        return -1;
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* The disk partition named IPOD with a FAT32 type, e.g. "disk6s2" */
static int find_ipod(char *dev, size_t len)
{
    FILE *p = popen("/usr/sbin/diskutil list external physical", "r");
    char line[256];
    int found = 0;

    if (!p)
        return 0;
    while (!found && fgets(line, sizeof(line), p))
    {
        char *id;
        if (!strstr(line, "DOS_FAT_32") || !strstr(line, " IPOD "))
            continue;
        id = strrchr(line, ' ');
        if (id && sscanf(id + 1, "%31s", dev) == 1 && !strncmp(dev, "disk", 4)
            && strlen(dev) < len)
            found = 1;
    }
    pclose(p);
    return found;
}

int main(void)
{
    char dev[32];

    for (;;)
    {
        int busy;

        if (access(PAUSE, F_OK) != 0 && find_ipod(dev, sizeof(dev))
            && !mounted(dev, &busy))
        {
            char path[64];
            snprintf(path, sizeof(path), "/dev/%s", dev);
            mkdir(MNT, 0755);
            if (busy)
                logmsg("skip %s: " MNT " is in use", dev);
            else
            {
                char *argv[] = { "/sbin/mount_msdos", "-o", "noowners",
                                 path, MNT, NULL };
                if (run(argv) == 0)
                    logmsg("mounted %s at " MNT, path);
                else
                {
                    logmsg("mount of %s failed", path);
                    sleep(27);      /* don't retry a failing mount every 3 s */
                }
            }
        }
        sleep(3);
    }
}
