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
 * H.264 decoder: luma motion vector prediction (8.4.1.3) and the P_Skip
 * motion vector (8.4.1.1), on the macroblock's neighbour cache.
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
#include "h264_int.h"

static inline int median3(int a, int b, int c)
{
    int lo = a < b ? a : b, hi = a < b ? b : a;
    return c < lo ? lo : c > hi ? hi : c;
}

static inline void copy_mv(int16_t *dst, const int16_t *src)
{
    dst[0] = src[0];
    dst[1] = src[1];
}

/* x, y, w: the partition's top-left and width in 4x4 blocks. */
void h264_mv_pred(const struct h264_mb *m, int x, int y, int w, int ref,
                  int shape, int16_t *mvp)
{
    int ia = C(x - 1, y), ib = C(x, y - 1), ic = C(x + w, y - 1);
    int ra = m->ref[ia], rb = m->ref[ib], rc = m->ref[ic];

    if (rc == REF_NA)            /* C not available: D instead (8.4.1.3.2) */
    {
        ic = C(x - 1, y - 1);
        rc = m->ref[ic];
    }
    if (shape == MVP_16x8)
    {
        if (y == 0 && rb == ref)
        {
            copy_mv(mvp, m->mv[ib]);
            return;
        }
        if (y != 0 && ra == ref)
        {
            copy_mv(mvp, m->mv[ia]);
            return;
        }
    }
    else if (shape == MVP_8x16)
    {
        if (x == 0 && ra == ref)
        {
            copy_mv(mvp, m->mv[ia]);
            return;
        }
        if (x != 0 && rc == ref)
        {
            copy_mv(mvp, m->mv[ic]);
            return;
        }
    }
    /* 8.4.1.3.1 */
    if (rb == REF_NA && rc == REF_NA && ra != REF_NA)
    {
        copy_mv(mvp, m->mv[ia]);
        return;
    }
    switch ((ra == ref) | (rb == ref) << 1 | (rc == ref) << 2)
    {
    case 1:
        copy_mv(mvp, m->mv[ia]);
        break;
    case 2:
        copy_mv(mvp, m->mv[ib]);
        break;
    case 4:
        copy_mv(mvp, m->mv[ic]);
        break;
    default:
        mvp[0] = median3(m->mv[ia][0], m->mv[ib][0], m->mv[ic][0]);
        mvp[1] = median3(m->mv[ia][1], m->mv[ib][1], m->mv[ic][1]);
        break;
    }
}

void h264_mv_pred_skip(const struct h264_mb *m, int16_t *mvp)
{
    int ia = C(-1, 0), ib = C(0, -1);
    if (m->ref[ia] == REF_NA || m->ref[ib] == REF_NA ||
        (m->ref[ia] == 0 && !m->mv[ia][0] && !m->mv[ia][1]) ||
        (m->ref[ib] == 0 && !m->mv[ib][0] && !m->mv[ib][1]))
    {
        mvp[0] = mvp[1] = 0;
        return;
    }
    h264_mv_pred(m, 0, 0, 4, 0, MVP_MEDIAN, mvp);
}
