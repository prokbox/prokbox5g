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

/* The visualizer's line ops, PP side (bcmjob_pp.h; #126, proposed, sim
 * only): POLYLINE, SEGMENTS and PALROW packed as bcmfw/jobs.h lays them
 * out and queued with bcmjob_submit().  Built for the device
 * (HAVE_BCM_JOBS) and the simulator (HAVE_BCM_JOBS_MODEL) alike, and by
 * tests/bcmjob. */
#include <stdbool.h>
#include <stdint.h>
#include "bcmjob_pp.h"
#include "bcmjob.h"

/* ---- Scope, Radial, Spectrogram on the BCM (bcmjob_pp.h; #126) ---------- */
bool bcmjob_have_polyline(void)
{
    return bcmjob_have(J_OP_POLYLINE);
}

bool bcmjob_have_segments(void)
{
    return bcmjob_have(J_OP_SEGMENTS);
}

bool bcmjob_have_palrow(void)
{
    return bcmjob_have(J_OP_PALROW);
}

/* n bytes from v, four to a word (the low first), at a: the words used */
static uint32_t pack8(uint32_t *a, const uint8_t *v, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < (n + 3) / 4; i++)
        a[i] = 0;
    for (i = 0; i < n; i++)
        a[i / 4] |= (uint32_t)v[i] << (8 * (i & 3));
    return (n + 3) / 4;
}

int bcmjob_viz_polyline(const struct bcmjob_poly *p)
{
    uint32_t a[J_POLY_ARGS];

    a[0] = p->surf;
    a[1] = p->x | (uint32_t)p->y << 16;
    a[2] = p->w | (uint32_t)p->h << 16;
    a[3] = p->src;
    a[4] = p->off | (uint32_t)p->step << 16;
    a[5] = p->n | (uint32_t)p->flags << 16;
    a[6] = p->gain | (uint32_t)p->half << 16;
    a[7] = p->colour | (uint32_t)p->width << 16;
    return bcmjob_submit(J_OP_POLYLINE, 0, a, J_POLY_ARGS);
}

int bcmjob_viz_segments(const struct bcmjob_segs *s)
{
    uint32_t a[J_SEGS_ARGS(J_SEG_N_MAX)], k = 8;

    if (!s->n || s->n > J_SEG_N_MAX)
        return BCMJOB_FAILED;
    a[0] = s->surf;
    a[1] = s->x | (uint32_t)s->y << 16;
    a[2] = s->w | (uint32_t)s->h << 16;
    a[3] = (uint16_t)s->cx | (uint32_t)(uint16_t)s->cy << 16;
    a[4] = s->n | (uint32_t)s->a0 << 8 | (uint32_t)s->astep << 16 |
           (uint32_t)(s->width & 0xF) << 24 | (uint32_t)(s->flags & 0xF) << 28;
    a[5] = s->r0 | (uint32_t)s->lmax << 8 | (uint32_t)s->rring << 16 |
           (uint32_t)(s->capd & 0xF) << 24 | (uint32_t)(s->ringd & 0xF) << 28;
    a[6] = s->c0 | (uint32_t)s->c1 << 16;
    a[7] = s->cap | (uint32_t)s->ring << 16;
    k += pack8(a + k, s->len, s->n);
    k += pack8(a + k, s->peak, s->n);
    return bcmjob_submit(J_OP_SEGMENTS, 0, a, k);
}

int bcmjob_viz_palrow(const struct bcmjob_palrow *p)
{
    uint32_t a[J_PAL_ARGS(J_PAL_N_MAX)];

    if (!p->nb || p->nb > J_PAL_N_MAX)
        return BCMJOB_FAILED;
    a[0] = p->surf;
    a[1] = p->x | (uint32_t)p->y << 16;
    a[2] = p->w | (uint32_t)p->h << 16;
    a[3] = p->lut;
    a[4] = p->nb;
    return bcmjob_submit(J_OP_PALROW, 0, a, 5 + pack8(a + 5, p->lv, p->nb));
}
