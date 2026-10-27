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

/* vcheck_image_test.py's way into the first-use check: the PP's own
 * apps/video/vcheck.c and bcmvp.c, built for the Mac, over a host port
 * that Python drives into vc2sim running the merged image.  One verdict
 * a call; the band lines go to stdout. */
#include <stdio.h>
#include <string.h>
#include "vcheck.h"
#include "vcheck_stream.h"
#include "vclock_ladder.h"
#include "vclk.h"
#include "../../bcmfw/hostproto.h"

static struct bvp_port port;
static struct bvp bvp;
static uint16_t scratch[512] __attribute__((aligned(4)));
static uint16_t screen[VCHECK_PANEL_W * VCHECK_PANEL_H];
static int (*reboot_fn)(void);

static void log_line(void *ctx, const char *line)
{
    (void)ctx;
    printf("    %s\n", line);
    fflush(stdout);
}

static int reboot(void *ctx)
{
    (void)ctx;
    return reboot_fn ? reboot_fn() : -1;
}

/* attach and HELLO over the given port; 0, or the failing code */
int t_attach(int (*rd)(uint32_t, uint32_t *), int (*wr)(uint32_t, uint32_t),
             int (*wrb)(uint32_t, const uint16_t *, uint32_t),
             uint32_t (*now)(void), void (*yl)(void), int (*rb)(void))
{
    port.rd = rd;
    port.wr = wr;
    port.wr_block = wrb;
    port.now_us = now;
    port.yield = yl;
    reboot_fn = rb;
    if (bvp_attach(&bvp, &port))
        return -100;
    return bvp_hello(&bvp);
}

/* the check over the file's bytes: the verdict; *pics, *band, *why the
   first band's pictures matched, the band passed, the last band's why */
int t_run(const uint8_t *file, uint32_t bytes, int top_only, int *pics,
          int *band, int *why)
{
    static const uint8_t top[1] = { VCLK_TOP };
    struct vcheck_stream s;
    struct vcheck_io io;
    struct vcheck_result r;
    int v;

    if (vcheck_stream_parse(&s, file, bytes))
        return -200;
    memset(&io, 0, sizeof(io));
    io.port = &port;
    io.bvp = &bvp;
    io.reboot = reboot;
    io.log = log_line;
    io.screen = screen;
    io.pitch = VCHECK_PANEL_W;
    io.scratch = scratch;
    io.sums = vcheck_sums;
    v = vcheck_run(&io, &s, top_only ? top : vclock_ladder,
                   top_only ? 1 : VCLOCK_LADDER_N, &r);
    *pics = r.n ? (int)r.tried[0].pics : -1;
    *band = r.band;
    *why = r.n ? r.tried[r.n - 1].why : -1;
    return v;
}
