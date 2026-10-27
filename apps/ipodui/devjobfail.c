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

#include "config.h"
#include "ipodui/devjobfail.h"

#ifdef HAVE_BCMJOB_INJECT
#include "jobs.h"
#include "ipodui/uilog.h"
#ifdef HAVE_BCMFW_DEVTEST
#include "ipodui/devbcm.h"
#endif

/* The ops each kind covers: every op a visualiser style draws a frame
 * with, or DECODE */
static const uint8_t viz_ops[] = { J_OP_SPECTRUM, J_OP_BARS, J_OP_POLYLINE,
                                   J_OP_SEGMENTS, J_OP_PALROW };
static const uint8_t dec_ops[] = { J_OP_DECODE };

int devjf_arm(int kind, unsigned n)
{
    const uint8_t *ops = kind == DEVJF_VIZ ? viz_ops : dec_ops;
    unsigned cnt = kind == DEVJF_VIZ ? sizeof(viz_ops) : sizeof(dec_ops);
    unsigned i;
    int r = 0;

    for (i = 0; i < cnt; i++)
        if (bcmjob_dev_inject_fail(ops[i], n) < 0)
            r = -1;
    ui_log("bcmjob", "jobfail %s n=%u rc=%d", kind == DEVJF_VIZ ? "viz" : "decode",
           n, r);
    return r;
}

static bool step(int kind, unsigned *n, const char *title)
{
    *n = *n >= 2 ? 0 : *n + 1;
    devjf_arm(kind, *n);
#ifdef HAVE_BCMFW_DEVTEST
    devbcm_cue(title, *n ? (*n == 1 ? "next job fails once" : "next 2 jobs fail")
                         : "off",
               "expires after its jobs");
#else
    (void)title;
#endif
    return false;
}

bool devjf_row_viz(void)
{
    static unsigned n;

    return step(DEVJF_VIZ, &n, "Fail visualiser frames");
}

bool devjf_row_decode(void)
{
    static unsigned n;

    return step(DEVJF_DECODE, &n, "Fail decodes");
}
#endif /* HAVE_BCMJOB_INJECT */
