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

/* The settings files' save and boot recovery (cfgsafe.h). No statics,
 * no allocation; one byte of stack buffer. Built in the firmware and the
 * simulator (settings.c) and, over the firmware's own file layer, in
 * tests/fatcrash/safetest (sites cfg-save, cfg-legacy*, cfg-empty*).
 * CFGSAFE_TRUST_ANY (that test's negative control only): every existing
 * file counts as whole, as the stock scheme assumed. */
#include <stdbool.h>
#include <stdio.h>
#include "config.h"
#include "file.h"
#include "safefile.h"
#include "cfgsafe.h"
/* the class accountant (as safefile.c): real where modclass.h has it */
#if defined(SIMULATOR) || defined(HAVE_IPOD_UI_DEVMODE)
#include "ipodui/modclass.h"
#else
#define modclass_save(begin) do { } while (0)
#endif

#define exists(p) file_exists(p)

bool cfgsafe_valid(const char *path)
{
#ifdef CFGSAFE_TRUST_ANY
    return exists(path);
#else
    char last = 0;
    off_t size;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return false;
    size = lseek(fd, 0, SEEK_END);
    if (size > 0 && size <= CFGSAFE_MAX
        && lseek(fd, size - 1, SEEK_SET) == size - 1
        && read(fd, &last, 1) != 1)
        last = 0;
    close(fd);
    return size > 0 && size <= CFGSAFE_MAX && last == '\n';
#endif
}

/* tmp is whole: it takes file's name, file keeps old's */
static int replace(const char *tmp, const char *file, const char *old)
{
    if (exists(file))
    {
        if (cfgsafe_valid(file))
        {
            if (!safefile_remove(old, file) || rename(file, old) < 0)
                return -1;
        }
        else if (!safefile_remove(file, old))
            return -1;          /* an empty file is nobody's fallback */
    }
    if (rename(tmp, file) < 0)
    {
        /* a failed rename may still have landed: undo onto a free name */
        if (!exists(file) && exists(old))
            rename(old, file);
        return -1;
    }
    return 0;
}

int cfgsafe_commit(const char *tmp, const char *file, const char *old)
{
    int rc;

    if (!cfgsafe_valid(tmp))
    {
        safefile_remove(tmp, file);
        return -1;
    }
    modclass_save(true);        /* the save protocol's own probes */
    rc = replace(tmp, file, old);
    modclass_save(false);
    return rc;
}

unsigned cfgsafe_recover(const char *file, const char *tmp,
                         const char *legacy, const char *old)
{
    unsigned r = 0;

    if (legacy && exists(legacy))
    {
        /* a cut inside a rename can leave .new as a second name on
         * file's chain: only that name goes */
        if (safefile_drop_alias(legacy, file) > 0)
            r |= CFGSAFE_R_DROPPED;
        else if (cfgsafe_valid(legacy) && replace(legacy, file, old) == 0)
            r |= CFGSAFE_R_LEGACY;
        else
        {
            safefile_remove(legacy, file);
            r |= CFGSAFE_R_DROPPED;
        }
    }
    if (tmp && exists(tmp))
    {
        safefile_remove(tmp, file);
        r |= CFGSAFE_R_DROPPED;
    }
    if (!cfgsafe_valid(file))
    {
        bool had = exists(file);

        if (cfgsafe_valid(old) && (!had || safefile_remove(file, old))
            && rename(old, file) == 0)
            r |= CFGSAFE_R_OLD;
        else if (had)
            r |= CFGSAFE_R_EMPTY;
    }
    return r;
}
