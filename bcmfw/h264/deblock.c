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
 * H.264 decoder: the deblocking filter (8.7) for progressive frames, run
 * over the whole picture once all its macroblocks are decoded.  Edges
 * whose boundary strengths are all 0 cost only the bS derivation.
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

#if H264_VPU_DBK
#include "vpu_kernels.h"
#define REF(name) name##_c
#else
#define REF(name) name
#endif

/* Table 8-16: alpha' and beta' by indexA / indexB */
static const uint8_t alpha_tab[52] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    4, 4, 5, 6, 7, 8, 9, 10, 12, 13, 15, 17, 20, 22, 25, 28,
    32, 36, 40, 45, 50, 56, 63, 71, 80, 90, 101, 113, 127, 144, 162, 182,
    203, 226, 255, 255
};
static const uint8_t beta_tab[52] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16,
    17, 17, 18, 18
};
/* Table 8-17: tC0' by indexA and bS = 1, 2, 3 */
static const int8_t tc0_tab[52][3] = {
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 1},
    {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 1, 1}, {0, 1, 1}, {1, 1, 1},
    {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {1, 1, 2}, {1, 1, 2}, {1, 1, 2},
    {1, 1, 2}, {1, 2, 3}, {1, 2, 3}, {2, 2, 3}, {2, 2, 4}, {2, 3, 4},
    {2, 3, 4}, {3, 3, 5}, {3, 4, 6}, {3, 4, 6}, {4, 5, 7}, {4, 5, 8},
    {4, 6, 9}, {5, 7, 10}, {6, 8, 11}, {6, 8, 13}, {7, 10, 14},
    {8, 11, 16}, {9, 12, 18}, {10, 13, 20}, {11, 15, 23}, {13, 17, 25}
};

#if !H264_VPU_DBK || H264_VPU_REF
void REF(h264_dbk_luma)(uint8_t *pix, int xs, int ys, int alpha, int beta,
                        const uint8_t *bs, const int8_t *tc0)
{
    int k, l;
    for (k = 0; k < 4; k++)
    {
        uint8_t *line = pix + 4 * k * ys;
        int s = bs[k];
        if (!s)
            continue;
        for (l = 0; l < 4; l++, line += ys)
        {
            int p0 = line[-xs], p1 = line[-2 * xs], p2 = line[-3 * xs];
            int q0 = line[0], q1 = line[xs], q2 = line[2 * xs];
            int ap, aq;
            if (iabs(p0 - q0) >= alpha || iabs(p1 - p0) >= beta ||
                iabs(q1 - q0) >= beta)
                continue;
            ap = iabs(p2 - p0);
            aq = iabs(q2 - q0);
            if (s < 4)
            {
                int c0 = tc0[k], tc = c0 + (ap < beta) + (aq < beta);
                int delta = CLIP3(-tc, tc, ((q0 - p0) * 4 + (p1 - q1) + 4) >> 3);
                line[-xs] = clip255(p0 + delta);
                line[0] = clip255(q0 - delta);
                if (ap < beta)
                    line[-2 * xs] = p1 + CLIP3(-c0, c0,
                        (p2 + ((p0 + q0 + 1) >> 1) - 2 * p1) >> 1);
                if (aq < beta)
                    line[xs] = q1 + CLIP3(-c0, c0,
                        (q2 + ((p0 + q0 + 1) >> 1) - 2 * q1) >> 1);
            }
            else
            {
                int strong = iabs(p0 - q0) < ((alpha >> 2) + 2);
                if (ap < beta && strong)
                {
                    int p3 = line[-4 * xs];
                    line[-xs] = (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3;
                    line[-2 * xs] = (p2 + p1 + p0 + q0 + 2) >> 2;
                    line[-3 * xs] = (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3;
                }
                else
                    line[-xs] = (2 * p1 + p0 + q1 + 2) >> 2;
                if (aq < beta && strong)
                {
                    int q3 = line[3 * xs];
                    line[0] = (p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3;
                    line[xs] = (p0 + q0 + q1 + q2 + 2) >> 2;
                    line[2 * xs] = (2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3;
                }
                else
                    line[0] = (2 * q1 + q0 + p1 + 2) >> 2;
            }
        }
    }
}

void REF(h264_dbk_chroma)(uint8_t *pix, int xs, int ys, int alpha, int beta,
                          const uint8_t *bs, const int8_t *tc0)
{
    int l;
    for (l = 0; l < 8; l++, pix += ys)
    {
        int s = bs[l >> 1];
        int p0, p1, q0, q1;
        if (!s)
            continue;
        p0 = pix[-xs];
        p1 = pix[-2 * xs];
        q0 = pix[0];
        q1 = pix[xs];
        if (iabs(p0 - q0) >= alpha || iabs(p1 - p0) >= beta ||
            iabs(q1 - q0) >= beta)
            continue;
        if (s < 4)
        {
            int tc = tc0[l >> 1] + 1;
            int delta = CLIP3(-tc, tc, ((q0 - p0) * 4 + (p1 - q1) + 4) >> 3);
            pix[-xs] = clip255(p0 + delta);
            pix[0] = clip255(q0 - delta);
        }
        else
        {
            pix[-xs] = (2 * p1 + p0 + q1 + 2) >> 2;
            pix[0] = (2 * q1 + q0 + p1 + 2) >> 2;
        }
    }
}

#endif /* C filters */

#if H264_VPU_DBK
/* The vector filters (gen_vpu.py) take the lines to filter as a lane
 * mask and tC0 per lane; lines with bS = 4 and bS < 4 go to the strong
 * and the normal kernel (an edge never mixes them in this decoder, but
 * the signature allows it). */
void h264_dbk_luma(uint8_t *pix, int xs, int ys, int alpha, int beta,
                   const uint8_t *bs, const int8_t *tc0)
{
#if H264_VPU_RUNTIME
    if (H264_USE_C(H264_OFF_DBK) || H264_BAD(H264_BAD_DBK | H264_BAD_DBK_LUMA))
    {
        h264_dbk_luma_c(pix, xs, ys, alpha, beta, bs, tc0);
        return;
    }
#endif
    uint32_t tcv[4];
    int k, normal = 0, strong = 0, stride = xs == 1 ? ys : xs, b0 = bs[0];
    if (b0 && bs[1] == b0 && bs[2] == b0 && bs[3] == b0)    /* uniform */
    {
        if (b0 == 4)
            (xs == 1 ? h264_vdbk_luma_v_strong : h264_vdbk_luma_h_strong)
                (pix, stride, alpha, beta, 0xffff, tcv);
        else
        {
            tcv[0] = tcv[1] = tcv[2] = tcv[3] = (uint8_t)tc0[0] * 0x01010101u;
            (xs == 1 ? h264_vdbk_luma_v_normal : h264_vdbk_luma_h_normal)
                (pix, stride, alpha, beta, 0xffff, tcv);
        }
        return;
    }
    for (k = 0; k < 4; k++)
    {
        if (bs[k] == 4)
            strong |= 0xf << (4 * k);
        else if (bs[k])
            normal |= 0xf << (4 * k);
        tcv[k] = (uint8_t)tc0[k] * 0x01010101u;
    }
    if (normal)
        (xs == 1 ? h264_vdbk_luma_v_normal : h264_vdbk_luma_h_normal)
            (pix, stride, alpha, beta, normal, tcv);
    if (strong)
        (xs == 1 ? h264_vdbk_luma_v_strong : h264_vdbk_luma_h_strong)
            (pix, stride, alpha, beta, strong, tcv);
}

void h264_dbk_chroma(uint8_t *pix, int xs, int ys, int alpha, int beta,
                     const uint8_t *bs, const int8_t *tc0)
{
#if H264_VPU_RUNTIME
    if (H264_USE_C(H264_OFF_DBK) || H264_BAD(H264_BAD_DBK))
    {
        h264_dbk_chroma_c(pix, xs, ys, alpha, beta, bs, tc0);
        return;
    }
#endif
    uint32_t tcv[4];
    int k, normal = 0, strong = 0, stride = xs == 1 ? ys : xs, b0 = bs[0];
    if (b0 && bs[1] == b0 && bs[2] == b0 && bs[3] == b0)    /* uniform */
    {
        if (b0 == 4)
            (xs == 1 ? h264_vdbk_chroma_v_strong : h264_vdbk_chroma_h_strong)
                (pix, stride, alpha, beta, 0x00ff, tcv);
        else
        {
            tcv[0] = tcv[1] = (uint8_t)tc0[0] * 0x01010101u;
            (xs == 1 ? h264_vdbk_chroma_v_normal : h264_vdbk_chroma_h_normal)
                (pix, stride, alpha, beta, 0x00ff, tcv);
        }
        return;
    }
    for (k = 0; k < 4; k++)
    {
        if (bs[k] == 4)
            strong |= 3 << (2 * k);
        else if (bs[k])
            normal |= 3 << (2 * k);
    }
    tcv[0] = (uint8_t)tc0[0] * 0x0101u | (uint8_t)tc0[1] * 0x01010000u;
    tcv[1] = (uint8_t)tc0[2] * 0x0101u | (uint8_t)tc0[3] * 0x01010000u;
    tcv[2] = tcv[3] = 0;
    if (normal)
        (xs == 1 ? h264_vdbk_chroma_v_normal : h264_vdbk_chroma_h_normal)
            (pix, stride, alpha, beta, normal, tcv);
    if (strong)
        (xs == 1 ? h264_vdbk_chroma_v_strong : h264_vdbk_chroma_h_strong)
            (pix, stride, alpha, beta, strong, tcv);
}
#endif

/* A nibble's four bits as four bytes, 0xff where set (segment k of an
 * edge is byte k of its bS word) */
#define NIB(m) ((m) & 1 ? 0xffu : 0) | ((m) & 2 ? 0xff00u : 0) | \
               ((m) & 4 ? 0xff0000u : 0) | ((m) & 8 ? 0xff000000u : 0)
static const uint32_t nib_bytes[16] = {
    NIB(0), NIB(1), NIB(2), NIB(3), NIB(4), NIB(5), NIB(6), NIB(7),
    NIB(8), NIB(9), NIB(10), NIB(11), NIB(12), NIB(13), NIB(14), NIB(15)
};

#if H264_VPU_DBK
/* A nibble of segments as the vector kernels' lane masks: luma 4 lanes a
 * segment, chroma 2 */
#define L4(m) (((m) & 1 ? 0xfu : 0) | ((m) & 2 ? 0xf0u : 0) | \
               ((m) & 4 ? 0xf00u : 0) | ((m) & 8 ? 0xf000u : 0))
#define L2(m) (((m) & 1 ? 3u : 0) | ((m) & 2 ? 0xcu : 0) | \
               ((m) & 4 ? 0x30u : 0) | ((m) & 8 ? 0xc0u : 0))
static const uint16_t lane4[16] = {
    L4(0), L4(1), L4(2), L4(3), L4(4), L4(5), L4(6), L4(7),
    L4(8), L4(9), L4(10), L4(11), L4(12), L4(13), L4(14), L4(15)
};
static const uint8_t lane2[16] = {
    L2(0), L2(1), L2(2), L2(3), L2(4), L2(5), L2(6), L2(7),
    L2(8), L2(9), L2(10), L2(11), L2(12), L2(13), L2(14), L2(15)
};
#endif

/* 8.7.2.1: boundary strength of the four 4-sample segments of one edge
 * between two inter macroblocks (or inside one), as one word (segment k
 * in byte k).  p, q: the macroblocks' info and MVs; bq0: the first q
 * block (raster), dq: the q block step along the edge (4 for a vertical
 * edge, 1 for a horizontal one), dp: the offset from a q block to its p
 * block (in p's raster).  e: the edge index, 0 for the macroblock edge.
 * The segments with coefficients on either side (bS 2) come from the two
 * nzmasks at once, a nibble and one table load; only an edge of a
 * macroblock with more than one motion vector compares per segment. */
static uint32_t strength_inter(const struct h264_mbinfo *p,
                               const int16_t (*pmv)[2], const struct h264_mbinfo *q,
                               const int16_t (*qmv)[2], int bq0, int dq, int dp, int e)
{
    uint32_t m = (uint32_t)(q->nzmask >> bq0) | (uint32_t)(p->nzmask >> (bq0 + dp));
    uint32_t nz, w;
    int k;

    /* the four segments' "coefficients on a side" bits as a nibble */
    if (dq == 4)
    {
        m &= 0x1111;
        m = (m | m >> 3 | m >> 6 | m >> 9) & 0xf;
    }
    else
        m &= 0xf;
    nz = nib_bytes[m];
    /* one MV and reference on both sides: one comparison for the edge */
    if ((q->flags & MBF_UNIFORM) && (e || (p->flags & MBF_UNIFORM)))
    {
        if (e || (p->refpic[0] == q->refpic[0] &&
                  iabs(pmv[0][0] - qmv[0][0]) < 4 &&
                  iabs(pmv[0][1] - qmv[0][1]) < 4))
            return nz & 0x02020202u;
        return (nz & 0x02020202u) | (~nz & 0x01010101u);
    }
    w = nz & 0x02020202u;
    for (k = 0; k < 4; k++)
    {
        int bq = bq0 + k * dq, bp = bq + dp;
        int ip, iq;

        if (m >> k & 1)
            continue;           /* bS 2 */
        ip = ((bp >> 3) << 1) | ((bp & 3) >> 1);
        iq = ((bq >> 3) << 1) | ((bq & 3) >> 1);
        if (p->refpic[ip] != q->refpic[iq] ||
            iabs(pmv[bp][0] - qmv[bq][0]) >= 4 ||
            iabs(pmv[bp][1] - qmv[bq][1]) >= 4)
            w |= 1u << (8 * k);
    }
    return w;
}

/* alpha, beta (Table 8-16) and the tC0 row (Table 8-17) for an average
 * QP and the q macroblock's offsets, through a one-entry cache per use
 * (d->dbk[slot]): the QP rarely changes from one macroblock to the next */
static inline const struct h264_dbk_par *dbk_par(struct h264_dec *d, int slot,
                                                 int qpav,
                                                 const struct h264_mbinfo *q,
                                                 int koff)
{
    struct h264_dbk_par *o = &d->dbk[slot];
    int key = (qpav + 1) | koff;         /* koff: the offsets, see filter_mb */
    if (o->key != key)
    {
        int ia = CLIP3(0, 51, qpav + q->alpha_off);
        int ib = CLIP3(0, 51, qpav + q->beta_off);
        o->key = key;
        o->beta = beta_tab[ib];
        o->alpha = o->beta ? alpha_tab[ia] : 0;
        o->tc = tc0_tab[ia];
        o->tcw[0] = 0;
        o->tcw[1] = (uint8_t)o->tc[0] * 0x01010101u;
        o->tcw[2] = (uint8_t)o->tc[1] * 0x01010101u;
        o->tcw[3] = (uint8_t)o->tc[2] * 0x01010101u;
    }
    return o;
}

/* tC0 of the four segments (0 where bS is 0 or 4); bw: the four bS as a
 * word, so a uniform edge takes one lookup */
static inline void tc0_of(int8_t *tc0, const uint8_t *b, uint32_t bw,
                          const int8_t *tct)
{
    int k;
    if (bw == (b[0] * 0x01010101u))
    {
        int t = b[0] < 4 ? tct[b[0] - 1] : 0;
        tc0[0] = tc0[1] = tc0[2] = tc0[3] = t;
        return;
    }
    for (k = 0; k < 4; k++)
        tc0[k] = b[k] && b[k] < 4 ? tct[b[k] - 1] : 0;
}

/* One edge of a macroblock: the luma edge at y (its first sample), and
 * with dire bit 1 clear (edges 0 and 2) the chroma edges at u and at v
 * (u + d->uv).  dire bit 0, dir: 0 vertical (filter across x), 1
 * horizontal; bw: the four bS as bytes, not all 0; lc: the luma and
 * chroma thresholds.  Out of line, so filter_mb's loop keeps its state in
 * registers; six arguments, all in registers. */
static H264_NOINLINE void filter_edge(uint8_t *y, uint8_t *u,
                                      const struct h264_dec *d, int dire,
                                      uint32_t bw,
                                      const struct h264_dbk_par *const *lc)
{
    const struct h264_dbk_par *l = lc[0], *c = lc[1];
    const uint8_t *b = (const uint8_t *)&bw;
    int W = d->ls, CW = d->cs, dir = dire & 1, chroma = !(dire & 2);
    uint8_t *v = u + d->uv;
    int8_t tc0[4];

#if H264_VPU_DBK
    if (!H264_USE_C(H264_OFF_DBK) && !H264_BAD(H264_BAD_DBK | H264_BAD_DBK_LUMA))
    {
        uint32_t tcv[4];
        int s0 = b[0];

        /* an edge of one bS (most of them): straight to the vector
         * kernels, tC0 broadcast from the table, no per-segment glue (the
         * wrapper's uniform case, without tc0_of) */
        if (bw == s0 * 0x01010101u)
        {
            if (l->alpha)
            {
                if (s0 == 4)
                    (dir ? h264_vdbk_luma_h_strong : h264_vdbk_luma_v_strong)
                        (y, W, l->alpha, l->beta, 0xffff, tcv);
                else
                {
                    tcv[0] = tcv[1] = tcv[2] = tcv[3] = l->tcw[s0];
                    (dir ? h264_vdbk_luma_h_normal : h264_vdbk_luma_v_normal)
                        (y, W, l->alpha, l->beta, 0xffff, tcv);
                }
            }
            if (!chroma || !c->alpha)
                return;
            if (s0 == 4)
            {
                (dir ? h264_vdbk_chroma_h_strong : h264_vdbk_chroma_v_strong)
                    (u, CW, c->alpha, c->beta, 0x00ff, tcv);
                (dir ? h264_vdbk_chroma_h_strong : h264_vdbk_chroma_v_strong)
                    (v, CW, c->alpha, c->beta, 0x00ff, tcv);
            }
            else
            {
                tcv[0] = tcv[1] = c->tcw[s0];
                tcv[2] = tcv[3] = 0;
                (dir ? h264_vdbk_chroma_h_normal : h264_vdbk_chroma_v_normal)
                    (u, CW, c->alpha, c->beta, 0x00ff, tcv);
                (dir ? h264_vdbk_chroma_h_normal : h264_vdbk_chroma_v_normal)
                    (v, CW, c->alpha, c->beta, 0x00ff, tcv);
            }
            return;
        }
        /* mixed bS (an inter edge: 0, 1 or 2 only, never 4): the normal
         * kernel on the lanes with bS > 0, tC0 per segment straight from
         * the table (the wrapper's loops unrolled) */
        if (!(bw & 0x04040404u))
        {
            uint32_t n = (bw | bw >> 1) & 0x01010101u;

            n = (n | n >> 7 | n >> 14 | n >> 21) & 0xf;
            if (l->alpha)
            {
                const uint32_t *t = l->tcw;

                tcv[0] = t[b[0]];
                tcv[1] = t[b[1]];
                tcv[2] = t[b[2]];
                tcv[3] = t[b[3]];
                (dir ? h264_vdbk_luma_h_normal : h264_vdbk_luma_v_normal)
                    (y, W, l->alpha, l->beta, lane4[n], tcv);
            }
            if (!chroma || !c->alpha)
                return;
            {
                const uint32_t *t = c->tcw;

                tcv[0] = (t[b[0]] & 0xffffu) | (t[b[1]] & 0xffff0000u);
                tcv[1] = (t[b[2]] & 0xffffu) | (t[b[3]] & 0xffff0000u);
                tcv[2] = tcv[3] = 0;
                (dir ? h264_vdbk_chroma_h_normal : h264_vdbk_chroma_v_normal)
                    (u, CW, c->alpha, c->beta, lane2[n], tcv);
                (dir ? h264_vdbk_chroma_h_normal : h264_vdbk_chroma_v_normal)
                    (v, CW, c->alpha, c->beta, lane2[n], tcv);
            }
            return;
        }
    }
#endif
    if (l->alpha)
    {
        tc0_of(tc0, b, bw, l->tc);
        h264_dbk_luma(y, dir ? W : 1, dir ? 1 : W, l->alpha, l->beta, b, tc0);
    }
    /* chroma: the edges at 0 and 4 chroma samples (luma 0, 8) */
    if (!chroma || !c->alpha)
        return;
    tc0_of(tc0, b, bw, c->tc);
    h264_dbk_chroma(u, dir ? CW : 1, dir ? 1 : CW, c->alpha, c->beta, b, tc0);
    h264_dbk_chroma(v, dir ? CW : 1, dir ? 1 : CW, c->alpha, c->beta, b, tc0);
}

/* bS 2 of an internal edge e of an inter macroblock with one MV: where
 * either side has coefficients (nz: its nzmask) */
static inline uint32_t bs_inner_nz(unsigned nz, int dir, int e)
{
    uint32_t m;
    if (dir)
        m = (nz >> (4 * e) | nz >> (4 * e - 4)) & 0xf;
    else
    {
        m = (nz >> e | nz >> (e - 1)) & 0x1111;
        m = (m | m >> 3 | m >> 6 | m >> 9) & 0xf;
    }
    return nib_bytes[m] & 0x02020202u;
}

static H264_NOINLINE void filter_mb(struct h264_dec *d, int mx, int my)
{
    const int W = d->ls, CW = d->cs, addr = my * d->mb_w + mx;  /* strides */
    const struct h264_mbinfo *q = &d->mbi[addr];
    const int16_t (*qmv)[2] = (const int16_t (*)[2])d->mvs[addr];
    uint32_t bsw[2][4];          /* four bS per edge, as bytes */
    int dir, e, any = 0;
    int left = mx > 0 && (q->dbk_idc != 2 || d->mbi[addr - 1].slice == q->slice);
    int top = my > 0 &&
              (q->dbk_idc != 2 || d->mbi[addr - d->mb_w].slice == q->slice);

    for (dir = 0; dir < 2; dir++)
    {
        /* the macroblock edge */
        if (!(dir ? top : left))
            bsw[dir][0] = 0;
        else
        {
            int pa = dir ? addr - d->mb_w : addr - 1;
            const struct h264_mbinfo *p = &d->mbi[pa];
            if ((p->flags | q->flags) & MBF_INTRA)
            {
                bsw[dir][0] = 0x04040404;
                any = 1;
            }
            else if ((p->flags & q->flags & MBF_UNIFORM) && !(p->nzmask | q->nzmask))
            {
                /* one MV a side and no coefficients (P_Skip next to
                 * P_Skip, mostly): bS 1 or 0 for the whole edge, without
                 * strength_inter's call */
                const int16_t *pv = d->mvs[pa][0];
                any |= bsw[dir][0] = p->refpic[0] != q->refpic[0] ||
                                     iabs(pv[0] - qmv[0][0]) >= 4 ||
                                     iabs(pv[1] - qmv[0][1]) >= 4
                                   ? 0x01010101u : 0;
            }
            else
                any |= bsw[dir][0] = strength_inter(p, (const int16_t (*)[2])d->mvs[pa],
                                                    q, qmv, 0, dir ? 1 : 4,
                                                    dir ? 12 : 3, 0);
        }
        /* the internal edges */
        if (q->flags & MBF_INTRA)
        {
            bsw[dir][1] = bsw[dir][2] = bsw[dir][3] = 0x03030303;
            any = 1;
        }
        else if (!q->nzmask && (q->flags & MBF_UNIFORM))
            bsw[dir][1] = bsw[dir][2] = bsw[dir][3] = 0;    /* one MV, no
                                                               coefficients */
        else if (q->flags & MBF_UNIFORM)        /* one MV: coefficients only */
            for (e = 1; e < 4; e++)
                any |= bsw[dir][e] = bs_inner_nz(q->nzmask, dir, e);
        else
            for (e = 1; e < 4; e++)
                any |= bsw[dir][e] = (e & 1) && (q->flags & MBF_UNI8)
                    ? bs_inner_nz(q->nzmask, dir, e)    /* inside an 8x8 */
                    : strength_inter(q, qmv, q, qmv, dir ? e * 4 : e,
                                     dir ? 1 : 4, dir ? -4 : -1, e);
    }
    if (!any)
        return;

    {
        /* internal edges: p = q, so one set of thresholds per plane; a
         * macroblock edge's are the same when p's QPs are q's */
        const struct h264_dbk_par *in[2], *mb[2];
        uint8_t *y0 = d->cur->y + (my * 16) * W + mx * 16;
        uint8_t *u0 = d->cur->u + (my * 8) * CW + mx * 8;
        int koff = (uint8_t)q->alpha_off << 8 | (uint8_t)q->beta_off << 16;

        d->uv = d->cur->v - d->cur->u;
        in[0] = dbk_par(d, 0, q->qp, q, koff);
        in[1] = dbk_par(d, 1, q->qpc, q, koff);
        for (dir = 0; dir < 2; dir++)
        {
            int ystep = dir ? 4 * W : 4, cstep = dir ? 2 * CW : 2;
            uint8_t *y = y0, *u = u0;
            const uint32_t *bp = bsw[dir];
            if (bp[0])
            {
                const struct h264_mbinfo *p =
                    &d->mbi[dir ? addr - d->mb_w : addr - 1];
                mb[0] = p->qp == q->qp ? in[0]
                      : dbk_par(d, 2, (p->qp + q->qp + 1) >> 1, q, koff);
                mb[1] = p->qpc == q->qpc ? in[1]
                      : dbk_par(d, 3, (p->qpc + q->qpc + 1) >> 1, q, koff);
                filter_edge(y, u, d, dir, bp[0], mb);
            }
            for (e = 1; e < 4; e++)
            {
                y += ystep;
                u += cstep;
                if (bp[e])
                    filter_edge(y, u, d, dir | (e & 1) << 1, bp[e], in);
            }
        }
    }
}

#ifdef H264_DEBUG
int h264_debug_no_deblock;
#endif

void h264_deblock_picture(struct h264_dec *d)
{
    int mx, my;
#ifdef H264_DEBUG
    if (h264_debug_no_deblock)
        return;
#endif
    for (my = 0; my < d->mb_h; my++)
        for (mx = 0, H264_PROG(0x32, my); mx < d->mb_w; mx++)
            if (d->mbi[my * d->mb_w + mx].dbk_idc != 1)
                filter_mb(d, mx, my);
}
