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
 * Host test: the paused maintenance window's policy (Phase 16-1)
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
#include "ipodui/maint_core.h"

#define HZ 100
#define P  MAINT_ST_PLAY
#define PP (MAINT_ST_PLAY | MAINT_ST_PAUSE)

/* The window predicate over (status, pause age, idle, fade, video) rows
 * (docs/phase16-brief.md 3.1: Q1), and the floor rule (Q11). */
static const struct row {
    int status; long age; int idle, fading, video, dark;
    enum maint_why want;
} rows[] = {
    { 0,  -1,      0, 0, 0, 1, MAINT_OPEN },        /* stopped: always */
    { 0,  -1,      0, 1, 1, 1, MAINT_OPEN },
    { P,  -1,      1, 0, 0, 1, MAINT_NO_PLAYING },  /* playing: never */
    { P,  999*HZ,  1, 0, 0, 1, MAINT_NO_PLAYING },
    { PP, -1,      1, 0, 0, 1, MAINT_NO_YOUNG },    /* age not known yet */
    { PP, 0,       1, 0, 0, 1, MAINT_NO_YOUNG },
    { PP, 179*HZ,  1, 0, 0, 1, MAINT_NO_YOUNG },
    { PP, 180*HZ,  1, 0, 0, 1, MAINT_OPEN },        /* 3 min, dark */
    { PP, 180*HZ,  0, 0, 0, 1, MAINT_NO_LIT },      /* lit or a key */
    { PP, 3600*HZ, 0, 0, 0, 1, MAINT_NO_LIT },
    { PP, 3600*HZ, 1, 1, 0, 1, MAINT_NO_FADE },     /* a fade */
    { PP, 3600*HZ, 1, 0, 1, 1, MAINT_NO_VIDEO },    /* video (BCM) */
    { PP, 3600*HZ, 1, 0, 0, 1, MAINT_OPEN },
    { PP, 3600*HZ, 1, 0, 0, 0, MAINT_NO_LIT },   /* charger: idle, lit */
    { 0,  -1,      1, 0, 0, 0, MAINT_OPEN },     /* stopped: any screen */
};

static const struct frow {
    size_t buf, bytes, floor; int want;
} frows[] = {
    { 15079816, 7600000,  MAINT_FLOOR_32MB, 0 },  /* 32 MB, 50k: waits */
    { 20977328, 7600000,  MAINT_FLOOR_32MB, 1 },  /* fresh 32 MB buffer */
    { 48634248, 7600000,  MAINT_FLOOR_64MB, 1 },  /* 64 MB, 50k: opens */
    { 46000000, 7600000,  MAINT_FLOOR_64MB, 0 },
    { 7500000,  0,        MAINT_FLOOR_32MB, 1 },  /* exactly the floor */
    { 7500000,  1,        MAINT_FLOOR_32MB, 0 },
    { 1000,     2000,     0,                0 },  /* no underflow */
};

/* P16 gate fix: a take that the free heap covers needs no shrink; the
 * memory an earlier take freed counts once (the 32 MB, 50k case: a
 * 7.6 MB commit after its own 7.6 MB take, buffer now 13.4 MB) */
static const struct trow {
    size_t buf, avail, bytes; int want;
} trows[] = {
    { 13400000, 7700000, 7600000, 1 },  /* taken already: free covers */
    { 13400000, 7600000, 7600000, 0 },  /* 64 B short: a second shrink */
    { 15079816, 0,       7600000, 0 },  /* 32 MB, 50k: never fits */
    { 48634248, 0,       7600000, 1 },  /* 64 MB, 50k: one shrink */
    { 7500000,  100000,  1000000, 0 },
    { 1000,     0,       (size_t)-1, 0 }, /* no overflow */
};

int main(void)
{
    unsigned i;
    int bad = 0;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
    {
        struct maint_in in = { rows[i].status, rows[i].age, rows[i].idle,
                               rows[i].dark, rows[i].fading, rows[i].video };
        enum maint_why got = maint_core_window(&in, 180 * HZ);

        if (got != rows[i].want)
        {
            printf("window row %u: got %d want %d\n", i, got, rows[i].want);
            bad++;
        }
    }
    for (i = 0; i < sizeof(frows) / sizeof(frows[0]); i++)
    {
        int got = maint_core_fits(frows[i].buf, frows[i].bytes,
                                  frows[i].floor);

        if (got != frows[i].want)
        {
            printf("floor row %u: got %d want %d\n", i, got, frows[i].want);
            bad++;
        }
    }
    for (i = 0; i < sizeof(trows) / sizeof(trows[0]); i++)
    {
        int got = maint_core_take_fits(trows[i].buf, trows[i].avail,
                                       trows[i].bytes, 128,
                                       MAINT_FLOOR_32MB);

        if (got != trows[i].want)
        {
            printf("take row %u: got %d want %d\n", i, got, trows[i].want);
            bad++;
        }
    }
    return bad != 0;
}
