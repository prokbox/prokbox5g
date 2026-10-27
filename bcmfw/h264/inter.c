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
 * H.264 decoder: inter prediction of one partition (8.4.2.2), luma and
 * both chroma planes, into the picture being decoded.  Reference sample
 * positions outside the picture are clamped to its edge (8-228, 8-229):
 * blocks that reach outside go through a small edge-extended copy, so
 * the kernels never read outside a plane.
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

/* Copy a w x h window at (x, y) of a W x H plane, clamping coordinates:
 * per row, columns left of the plane repeat its first sample, columns
 * [l, r) are read in place, the rest repeat its last sample. */
#define EDGE_LS 32      /* the luma window's stride (21 wide) */
#define EDGE_CS 16      /* the chroma window's (9 wide) */

static void edge_copy(uint8_t *dst, int ds, const uint8_t *plane, int ps, int W,
                      int H, int x, int y, int w, int h)
{
    int i, j, l = CLIP3(0, w, -x), r = CLIP3(l, w, W - x);
    for (j = 0; j < h; j++, dst += ds)
    {
        const uint8_t *row = plane + CLIP3(0, H - 1, y + j) * ps;
        uint8_t first = row[0], last = row[W - 1];
        for (i = 0; i < l; i++)
            dst[i] = first;
        for (; i < r; i++)
            dst[i] = row[x + i];
        for (; i < w; i++)
            dst[i] = last;
    }
}

void h264_inter_pred(struct h264_dec *d, const struct h264_frame *ref,
                     int bx, int by, int bw, int bh, const int16_t *mv)
{
    /* the edge-emulation window at a stride of 32 (luma, 21 rows) or 16
     * (chroma): a multiple of 16, so the vector kernels' vldp/vldq pairs
     * keep one 16-byte line phase from row to row, the only form Apple's
     * code uses and silicon has run (strides of 21 and 9 until 2026-10-25:
     * unknown on silicon, alignment-audit.md; + 32: vector kernels read
     * 32-byte rows) */
    uint8_t *tmp = d->edge;             /* 21 * EDGE_LS + 32 */
    const struct h264_mb *m = &d->mb;
    int W = d->mb_w * 16, H = d->mb_h * 16, CW = W / 2, CH = H / 2;
    int LS = d->ls, CS = d->cs;         /* the planes' strides */
    int w = bw * 4, h = bh * 4, cw = w / 2, ch = h / 2;
    int px = m->x * 16 + bx * 4, py = m->y * 16 + by * 4;
    int x = px + (mv[0] >> 2), y = py + (mv[1] >> 2);
    int fx = mv[0] & 3, fy = mv[1] & 3, p, mx, my;
    uint8_t *dst = d->cur->y + py * LS + px;

    /* luma: the 6-tap needs x-2 .. x+w+2 when fx != 0 and y-2 .. y+h+2
     * when fy != 0; whole-sample directions need no margin */
    mx = fx ? 2 : 0;
    my = fy ? 2 : 0;
    if (x - mx >= 0 && y - my >= 0 && x + w + mx + (mx >> 1) <= W &&
        y + h + my + (my >> 1) <= H)
        h264_mc_luma(dst, LS, ref->y + y * LS + x, LS, w, h, fx, fy);
    else
    {
        /* only the samples the filter reads: the margins of a direction
         * with a fractional phase (the kernels' other taps are unused) */
        edge_copy(tmp + (2 - my) * EDGE_LS + 2 - mx, EDGE_LS, ref->y, LS, W, H,
                  x - mx, y - my, w + mx + mx + (mx >> 1), h + my + my + (my >> 1));
        h264_mc_luma(dst, LS, tmp + 2 * EDGE_LS + 2, EDGE_LS, w, h, fx, fy);
    }

    /* chroma (4:2:0): eighth-sample vectors */
    px >>= 1;
    py >>= 1;
    x = px + (mv[0] >> 3);
    y = py + (mv[1] >> 3);
    fx = mv[0] & 7;
    fy = mv[1] & 7;
    for (p = 0; p < 2; p++)
    {
        const uint8_t *plane = p ? ref->v : ref->u;
        uint8_t *cd = (p ? d->cur->v : d->cur->u) + py * CS + px;
        if (x >= 0 && y >= 0 && x + cw + (fx != 0) <= CW &&
            y + ch + (fy != 0) <= CH)
            h264_mc_chroma(cd, CS, plane + y * CS + x, CS, cw, ch, fx, fy);
        else
        {
            edge_copy(tmp, EDGE_CS, plane, CS, CW, CH, x, y, cw + (fx != 0),
                      ch + (fy != 0));
            h264_mc_chroma(cd, CS, tmp, EDGE_CS, cw, ch, fx, fy);
        }
    }
}
