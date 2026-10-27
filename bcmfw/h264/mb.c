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
 * H.264 decoder: slice data and the macroblock layer (7.3.4, 7.3.5) for
 * I and P slices with CAVLC, and the reconstruction of each macroblock
 * (prediction plus residual) straight into the picture.
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

/* macroblock kinds (mb_type, slice-type independent) */
enum {
    MB_P16x16, MB_P16x8, MB_P8x16, MB_P8x8, MB_P8x8REF0,
    MB_I4x4, MB_I16, MB_PCM
};

/* H264_AV_* of each Intra_4x4 block (raster r) for the neighbour
 * macroblocks available to intra prediction (ia: MBA_* bits).  Above-right
 * (6.4.11.4): inside the macroblock where already decoded, from the one
 * above for the top row (from the one above-right for r = 3), never for
 * r = 5, 7, 11, 13, 15. */
#define AV_L(ia, r)  (((r) & 3) || ((ia) & MBA_A) ? H264_AV_LEFT : 0)
#define AV_T(ia, r)  ((r) > 3 || ((ia) & MBA_B) ? H264_AV_TOP : 0)
#define AV_TL(ia, r) (((r) & 3) && (r) > 3 ? H264_AV_TOPLEFT : \
                      ((r) & 3) ? ((ia) & MBA_B ? H264_AV_TOPLEFT : 0) : \
                      (r) > 3 ? ((ia) & MBA_A ? H264_AV_TOPLEFT : 0) : \
                      ((ia) & MBA_D ? H264_AV_TOPLEFT : 0))
#define AV_TR(ia, r) ((r) == 3 ? ((ia) & MBA_C ? H264_AV_TOPRIGHT : 0) : \
                      (r) < 3 ? ((ia) & MBA_B ? H264_AV_TOPRIGHT : 0) : \
                      (r) == 5 || (r) == 7 || (r) == 11 || (r) == 13 || \
                      (r) == 15 ? 0 : H264_AV_TOPRIGHT)
#define AV(ia, r) (AV_L(ia, r) | AV_T(ia, r) | AV_TL(ia, r) | AV_TR(ia, r))
#define AV_ROW(ia) { AV(ia, 0), AV(ia, 1), AV(ia, 2), AV(ia, 3), AV(ia, 4), \
    AV(ia, 5), AV(ia, 6), AV(ia, 7), AV(ia, 8), AV(ia, 9), AV(ia, 10), \
    AV(ia, 11), AV(ia, 12), AV(ia, 13), AV(ia, 14), AV(ia, 15) }
static const uint8_t blk_avail[16][16] = {
    AV_ROW(0), AV_ROW(1), AV_ROW(2), AV_ROW(3), AV_ROW(4), AV_ROW(5),
    AV_ROW(6), AV_ROW(7), AV_ROW(8), AV_ROW(9), AV_ROW(10), AV_ROW(11),
    AV_ROW(12), AV_ROW(13), AV_ROW(14), AV_ROW(15)
};

#ifdef H264_DEBUG
void (*h264_debug_trace)(const struct h264_dec *, const struct h264_mb *, int);
#endif

static inline int more_data(const struct h264_dec *d, const struct h264_bits *b)
{
    return bs_tell(b) < d->rbsp_bits;
}

/* ---- Neighbours ----------------------------------------------------------- */

static H264_NOINLINE void load_neighbours(struct h264_dec *d, struct h264_mb *m)
{
    const struct h264_mbinfo *mi = d->mbi;
    int W = d->mb_w, addr = m->addr, s = d->slice_num, k, p;
    int cip = d->pps_cur->constrained_intra_pred;
    int a = addr - 1, b = addr - W, c = addr - W + 1, dd = addr - W - 1;

    /* in locals: the cache's byte stores could alias m's words */
    int av = 0, iav;
    const int16_t (*mvs)[16][2] = d->mvs;

    if (m->x > 0 && mi[a].slice == s)
        av |= MBA_A;
    if (m->y > 0 && mi[b].slice == s)
        av |= MBA_B;
    if (m->y > 0 && m->x < W - 1 && mi[c].slice == s)
        av |= MBA_C;
    if (m->y > 0 && m->x > 0 && mi[dd].slice == s)
        av |= MBA_D;
    iav = av;
    if (cip)
    {
        if ((av & MBA_A) && !(mi[a].flags & MBF_INTRA))
            iav &= ~MBA_A;
        if ((av & MBA_B) && !(mi[b].flags & MBF_INTRA))
            iav &= ~MBA_B;
        if ((av & MBA_C) && !(mi[c].flags & MBF_INTRA))
            iav &= ~MBA_C;
        if ((av & MBA_D) && !(mi[dd].flags & MBF_INTRA))
            iav &= ~MBA_D;
    }
    m->avail = av;
    m->iavail = iav;

    /* the current macroblock: nothing decoded yet.  Its references are
     * REF_NA, x = 4 included (above-right of the right column: never
     * decoded, so mv prediction takes D there); each row's two words
     * also cover the next row's x = -1, written below.  Its MVs are
     * not cleared: prediction reads an MV only where the reference is
     * available (C falls back to D, and D, A and B inside the
     * macroblock are decoded first), every inter macroblock writes all
     * sixteen, and store_mb stores zeros for intra ones. */
    for (k = 0; k < 4; k++)
    {
        h264_w4 *w = (h264_w4 *)&m->ref[C(0, k)];
        w[0] = w[1] = (uint8_t)REF_NA * 0x01010101u;
    }

    for (k = 0; k < 4; k++)
    {
        int l = C(-1, k);
        if (av & MBA_A)
        {
            m->nz[l] = mi[a].nz[k * 4 + 3];
            m->ref[l] = mi[a].ref[(k >> 1) * 2 + 1];
            *(h264_mvw *)m->mv[l] = *(const h264_mvw *)mvs[a][k * 4 + 3];
        }
        else
        {
            m->nz[l] = NZ_NA;
            m->ref[l] = REF_NA;
            *(h264_mvw *)m->mv[l] = 0;
        }
    }
    /* the row above, a word at a time */
    k = C(0, -1);
    if (av & MBA_B)
    {
        const h264_mvw *mv = (const h264_mvw *)mvs[b][12];
        uint32_t r2 = (uint8_t)mi[b].ref[2], r3 = (uint8_t)mi[b].ref[3];
        *(h264_w4 *)&m->nz[k] = *(const h264_w4 *)&mi[b].nz[12];
        *(h264_w4 *)&m->ref[k] = r2 * 0x0101u | r3 * 0x01010000u;
        *(h264_mvw *)m->mv[k] = mv[0];
        *(h264_mvw *)m->mv[k + 1] = mv[1];
        *(h264_mvw *)m->mv[k + 2] = mv[2];
        *(h264_mvw *)m->mv[k + 3] = mv[3];
    }
    else
    {
        *(h264_w4 *)&m->nz[k] = 0xffffffffu;             /* NZ_NA */
        *(h264_w4 *)&m->ref[k] = (uint8_t)REF_NA * 0x01010101u;
        *(h264_mvw *)m->mv[k] = 0;
        *(h264_mvw *)m->mv[k + 1] = 0;
        *(h264_mvw *)m->mv[k + 2] = 0;
        *(h264_mvw *)m->mv[k + 3] = 0;
    }
    k = C(4, -1);
    if (av & MBA_C)
    {
        m->ref[k] = mi[c].ref[2];
        *(h264_mvw *)m->mv[k] = *(const h264_mvw *)mvs[c][12];
    }
    else
    {
        m->ref[k] = REF_NA;
        *(h264_mvw *)m->mv[k] = 0;
    }
    k = C(-1, -1);
    if (av & MBA_D)
    {
        m->ref[k] = mi[dd].ref[3];
        *(h264_mvw *)m->mv[k] = *(const h264_mvw *)mvs[dd][15];
    }
    else
    {
        m->ref[k] = REF_NA;
        *(h264_mvw *)m->mv[k] = 0;
    }

    for (p = 0; p < 2; p++)
        for (k = 0; k < 2; k++)
        {
            m->nzc[p][C(-1, k)] = (av & MBA_A)
                                ? mi[a].nz[16 + p * 4 + k * 2 + 1] : NZ_NA;
            m->nzc[p][C(k, -1)] = (av & MBA_B)
                                ? mi[b].nz[16 + p * 4 + 2 + k] : NZ_NA;
        }
}

/* Add the residual of the coded blocks in mask (bit k: m->coef[k], luma
 * raster 0-15, then Cb and Cr 16-23) to the planes at py, pu and pv:
 * only the set bits are visited, each at its d->blk_off. */
static H264_NOINLINE void add_residual(const struct h264_dec *d, struct h264_mb *m,
                                       uint8_t *py, uint8_t *pu, uint8_t *pv,
                                       uint32_t mask)
{
    while (mask)
    {
        int k = 31 - __builtin_clz(mask & -mask);
        int stride = k < 16 ? d->ls : d->cs;
        uint8_t *p = (k < 16 ? py : k < 20 ? pu : pv) + d->blk_off[k];
        mask &= mask - 1;
        if (m->dconly & (1u << k))
            h264_idct4_dc_add(p, stride, m->coef[k]);
        else
            h264_idct4_add(p, stride, m->coef[k]);
    }
}

/* ---- Intra ------------------------------------------------------------------ */

static H264_NOINLINE int intra4x4_pred_modes(const struct h264_dec *d,
                                             struct h264_bits *bp, struct h264_mb *m)
{
    struct h264_bits b = *bp;    /* in registers: see the bit reader */
    const struct h264_mbinfo *mi = d->mbi;
    int k, ia = m->iavail;

    /* the neighbours' modes (-1: not available for intra prediction),
     * loaded here rather than in load_neighbours: only Intra_4x4
     * macroblocks read them */
    if (ia & MBA_A)
    {
        const int8_t *s = mi[m->addr - 1].ipm;
        m->ipm[C(-1, 0)] = s[3];
        m->ipm[C(-1, 1)] = s[7];
        m->ipm[C(-1, 2)] = s[11];
        m->ipm[C(-1, 3)] = s[15];
    }
    else
        m->ipm[C(-1, 0)] = m->ipm[C(-1, 1)] = m->ipm[C(-1, 2)] =
            m->ipm[C(-1, 3)] = -1;
    *(h264_w4 *)&m->ipm[C(0, -1)] = (ia & MBA_B)
        ? *(const h264_w4 *)&mi[m->addr - d->mb_w].ipm[12] : 0xffffffffu;
    for (k = 0; k < 16; k++)
    {
        int r = h264_blk_raster[k], c = C(r & 3, r >> 2);
        int a = m->ipm[c - 1], bb = m->ipm[c - 8];
        int pred = (a < 0 || bb < 0) ? 2 : a < bb ? a : bb;
        int mode;
        uint32_t w;
        if (b.n < 4)
            bs_fill(&b);
        w = b.cache;
        if ((int32_t)w < 0)      /* prev_intra4x4_pred_mode_flag */
        {
            mode = pred;
            bs_skip(&b, 1);
        }
        else                     /* rem_intra4x4_pred_mode */
        {
            mode = (w >> 28) & 7;
            mode += mode >= pred;
            bs_skip(&b, 4);
        }
        m->ipm[c] = mode;
    }
    *bp = b;
    return 0;
}

static H264_NOINLINE int recon_intra4x4(struct h264_dec *d, struct h264_mb *m, uint8_t *y0,
                          int stride)
{
    const uint8_t *av = blk_avail[m->iavail & 15];
    int k;
    for (k = 0; k < 16; k++)
    {
        int r = h264_blk_raster[k];
        uint8_t *p = y0 + (r >> 2) * 4 * stride + (r & 3) * 4;
        if (h264_pred4x4(p, stride, m->ipm[C(r & 3, r >> 2)], av[r]) < 0)
            return -1;
        if (m->coded & (1u << r))
        {
            if (m->dconly & (1u << r))
                h264_idct4_dc_add(p, stride, m->coef[r]);
            else
                h264_idct4_add(p, stride, m->coef[r]);
        }
    }
    (void)d;
    return 0;
}

static int mb_avail_bits(int ia)
{
    int av = 0;
    if (ia & MBA_A)
        av |= H264_AV_LEFT;
    if (ia & MBA_B)
        av |= H264_AV_TOP;
    if (ia & MBA_D)
        av |= H264_AV_TOPLEFT;
    return av;
}

/* ---- Inter -------------------------------------------------------------------- */

static int read_ref(struct h264_bits *b, int nref)
{
    uint32_t v;
    if (nref <= 1)
        return 0;
    if (nref == 2)
        return !bs_get1(b);      /* te(v) with range 1 */
    v = bs_ue(b);
    return v < (uint32_t)nref ? (int)v : -1;
}

static void fill_part(struct h264_mb *m, int x, int y, int w, int h, int ref,
                      const int16_t *mv)
{
    h264_mvw v = *(const h264_mvw *)mv;
    int i, j;
    for (j = y; j < y + h; j++)
    {
        int c = C(x, j);
        for (i = 0; i < w; i++)
        {
            m->ref[c + i] = ref;
            *(h264_mvw *)m->mv[c + i] = v;
        }
    }
}

static int read_mvd(struct h264_bits *b, const int16_t *mvp, int16_t *mv)
{
    int32_t dx = bs_se(b), dy = bs_se(b);
    if (dx < -32768 || dx > 32767 || dy < -32768 || dy > 32767)
        return -1;
    dx += mvp[0];
    dy += mvp[1];
    if (dx < -32768 || dx > 32767 || dy < -32768 || dy > 32767)
        return -1;
    mv[0] = dx;
    mv[1] = dy;
    return 0;
}

/* sub_mb_type (Table 7-18): partition width and height in 4x4 blocks */
static const uint8_t sub_w[4] = { 2, 2, 1, 1 }, sub_h[4] = { 2, 1, 2, 1 };

static H264_NOINLINE int inter_mb(struct h264_dec *d, struct h264_bits *b, struct h264_mb *m)
{
    int nref = d->nref, refs[4], k, j;
    int16_t mvp[2], mv[2] __attribute__((aligned(4)));   /* word copies */

    if (m->mb_type <= MB_P8x16)
    {
        int np = m->mb_type == MB_P16x16 ? 1 : 2;
        m->flags |= MBF_UNI8;
        int pw = m->mb_type == MB_P8x16 ? 2 : 4;
        int ph = m->mb_type == MB_P16x8 ? 2 : 4;
        int shape = m->mb_type == MB_P16x8 ? MVP_16x8
                  : m->mb_type == MB_P8x16 ? MVP_8x16 : MVP_MEDIAN;
        for (k = 0; k < np; k++)
            if ((refs[k] = read_ref(b, nref)) < 0)
                return -1;
        for (k = 0; k < np; k++)
        {
            int x = m->mb_type == MB_P8x16 ? k * 2 : 0;
            int y = m->mb_type == MB_P16x8 ? k * 2 : 0;
            h264_mv_pred(m, x, y, pw, refs[k], shape, mvp);
            if (read_mvd(b, mvp, mv) < 0)
                return -1;
            if (np == 1)
            {
                /* one partition: no prediction inside the macroblock
                 * reads the cache again, so its references a row a word
                 * and the one MV, in C(0, 0), as store_mb and the MC take
                 * it (MBF_UNIFORM) */
                uint32_t rw = (uint8_t)refs[0] * 0x01010101u;
                *(h264_w4 *)&m->ref[C(0, 0)] = rw;
                *(h264_w4 *)&m->ref[C(0, 1)] = rw;
                *(h264_w4 *)&m->ref[C(0, 2)] = rw;
                *(h264_w4 *)&m->ref[C(0, 3)] = rw;
                *(h264_mvw *)m->mv[C(0, 0)] = *(const h264_mvw *)mv;
            }
            else
                fill_part(m, x, y, pw, ph, refs[k], mv);
        }
        for (k = 0; k < np; k++)
        {
            int x = m->mb_type == MB_P8x16 ? k * 2 : 0;
            int y = m->mb_type == MB_P16x8 ? k * 2 : 0;
            if (!d->reflist[refs[k]])
                return -1;
            h264_inter_pred(d, d->reflist[refs[k]], x, y, pw, ph,
                            m->mv[C(x, y)]);
        }
        return 0;
    }
    else
    {
        uint32_t sub[4];
        for (k = 0; k < 4; k++)
            if ((sub[k] = bs_ue(b)) > 3)
                return -1;
        if (!(sub[0] | sub[1] | sub[2] | sub[3]))
            m->flags |= MBF_UNI8;
        for (k = 0; k < 4; k++)
        {
            refs[k] = m->mb_type == MB_P8x8 ? read_ref(b, nref) : 0;
            if (refs[k] < 0)
                return -1;
        }
        for (k = 0; k < 4; k++)
        {
            int x0 = (k & 1) * 2, y0 = (k >> 1) * 2;
            int w = sub_w[sub[k]], h = sub_h[sub[k]];
            for (j = 0; j < 4 / (w * h); j++)
            {
                int x = x0 + (w == 1 ? (j & 1) : 0);
                int y = y0 + (h == 1 ? (w == 1 ? j >> 1 : j) : 0);
                h264_mv_pred(m, x, y, w, refs[k], MVP_MEDIAN, mvp);
                if (read_mvd(b, mvp, mv) < 0)
                    return -1;
                fill_part(m, x, y, w, h, refs[k], mv);
            }
        }
        for (k = 0; k < 4; k++)
        {
            int x0 = (k & 1) * 2, y0 = (k >> 1) * 2;
            int w = sub_w[sub[k]], h = sub_h[sub[k]];
            if (!d->reflist[refs[k]])
                return -1;
            if (w == 2 && h == 2)
                h264_inter_pred(d, d->reflist[refs[k]], x0, y0, 2, 2,
                                m->mv[C(x0, y0)]);
            else
                for (j = 0; j < 4 / (w * h); j++)
                {
                    int x = x0 + (w == 1 ? (j & 1) : 0);
                    int y = y0 + (h == 1 ? (w == 1 ? j >> 1 : j) : 0);
                    h264_inter_pred(d, d->reflist[refs[k]], x, y, w, h,
                                    m->mv[C(x, y)]);
                }
        }
        return 0;
    }
}

/* ---- Store ------------------------------------------------------------------ */

static H264_NOINLINE void store_mb(struct h264_dec *d, struct h264_mb *m)
{
    struct h264_mbinfo *mi = &d->mbi[m->addr];
    h264_mvw *mvs = (h264_mvw *)d->mvs[m->addr];
    int k, qp, qpi;

    mi->slice = d->slice_num;
    mi->flags = m->flags;
    if ((m->flags & MBF_SKIP) ||
        (!(m->flags & MBF_INTRA) && m->mb_type == MB_P16x16))
        mi->flags |= MBF_UNIFORM;
    qp = (m->flags & MBF_PCM) ? 0 : m->qp;
    mi->qp = qp;
    qpi = CLIP3(0, 51, qp + d->pps_cur->chroma_qp_offset);
    mi->qpc = h264_chroma_qp[qpi];
    mi->dbk_idc = d->sh.dbk_idc;
    mi->alpha_off = d->sh.alpha_off;
    mi->beta_off = d->sh.beta_off;
    if (m->flags & MBF_SKIP)
    {
        /* P_Skip: no coefficients, reference 0, one MV (in C(0, 0) only:
         * decode_skip fills nothing else) */
        h264_mvw v = *(const h264_mvw *)m->mv[C(0, 0)];
        uint32_t id = d->reflist[0]->id;
        h264_w4 *w = (h264_w4 *)mi->nz;
        w[0] = w[1] = w[2] = w[3] = w[4] = w[5] = 0;
        w = (h264_w4 *)mi->ipm;
        w[0] = w[1] = w[2] = w[3] = 0x02020202u;
        *(h264_w4 *)mi->ref = 0;
        *(h264_w4 *)mi->refpic = id * 0x01010101u;
        mi->nzmask = 0;
        for (k = 0; k < 16; k++)
            mvs[k] = v;
        return;
    }
    {
        /* the luma TotalCoeffs a row a word.  The blocks with any are
         * m->coded's luma bits (h264_residual sets them exactly where
         * TotalCoeff > 0, but for an Intra_16x16's DC: the loop filter
         * reads nzmask only between inter macroblocks). */
        h264_w4 *dn = (h264_w4 *)mi->nz, *di = (h264_w4 *)mi->ipm;
        for (k = 0; k < 4; k++)
            dn[k] = *(const h264_w4 *)&m->nz[C(0, k)];
        mi->nzmask = m->coded;
        if (m->flags & MBF_INTRA)
        {
            /* no MVs: prediction reads an intra neighbour's as zero (the
             * cache's are stale, see load_neighbours) */
            for (k = 0; k < 16; k++)
                mvs[k] = 0;
            if (m->flags & MBF_I4x4)
                for (k = 0; k < 4; k++)
                    di[k] = *(const h264_w4 *)&m->ipm[C(0, k)];
            else
                di[0] = di[1] = di[2] = di[3] = 0x02020202u;
        }
        else
        {
            di[0] = di[1] = di[2] = di[3] = 0x02020202u;
            if (mi->flags & MBF_UNIFORM)     /* every MV equals the first */
            {
                h264_mvw v = *(const h264_mvw *)m->mv[C(0, 0)];
                for (k = 0; k < 16; k++)
                    mvs[k] = v;
            }
            else
                for (k = 0; k < 16; k++)
                    mvs[k] = *(const h264_mvw *)m->mv[C(k & 3, k >> 2)];
        }
    }
    for (k = 0; k < 4; k++)
    {
        mi->nz[16 + k] = m->nzc[0][C(k & 1, k >> 1)];
        mi->nz[20 + k] = m->nzc[1][C(k & 1, k >> 1)];
    }
    if (m->flags & MBF_INTRA)
    {
        *(h264_w4 *)mi->ref = 0xffffffffu;           /* -1 */
        *(h264_w4 *)mi->refpic = 0xffffffffu;        /* none */
        return;
    }
    for (k = 0; k < 4; k++)
    {
        int r = m->ref[C((k & 1) * 2, (k >> 1) * 2)];
        mi->ref[k] = r;
        mi->refpic[k] = d->reflist[r]->id;
    }
}

/* ---- Macroblocks ------------------------------------------------------------- */

/* P_Skip's neighbours: only what its MV prediction reads (A, B, C and D's
 * references and MVs; 8.4.1.1).  The next macroblock loads its own from
 * d->mbi and d->mvs, so the rest of the caches may stay stale. */
static H264_NOINLINE void load_neighbours_skip(struct h264_dec *d, struct h264_mb *m)
{
    const struct h264_mbinfo *mi = d->mbi;
    const int16_t (*mvs)[16][2] = d->mvs;
    int W = d->mb_w, addr = m->addr, s = d->slice_num, x = m->x;
    int a = addr - 1, b = addr - W, c = addr - W + 1, dd = addr - W - 1;
    h264_mvw *mv = (h264_mvw *)m->mv;

    if (x > 0 && mi[a].slice == s)
    {
        m->ref[C(-1, 0)] = mi[a].ref[1];
        mv[C(-1, 0)] = *(const h264_mvw *)mvs[a][3];
    }
    else
    {
        m->ref[C(-1, 0)] = REF_NA;
        mv[C(-1, 0)] = 0;
    }
    if (m->y > 0 && mi[b].slice == s)
    {
        m->ref[C(0, -1)] = mi[b].ref[2];
        mv[C(0, -1)] = *(const h264_mvw *)mvs[b][12];
    }
    else
    {
        m->ref[C(0, -1)] = REF_NA;
        mv[C(0, -1)] = 0;
    }
    if (m->y > 0 && x < W - 1 && mi[c].slice == s)
    {
        m->ref[C(4, -1)] = mi[c].ref[2];
        mv[C(4, -1)] = *(const h264_mvw *)mvs[c][12];
    }
    else
    {
        m->ref[C(4, -1)] = REF_NA;
        mv[C(4, -1)] = 0;
    }
    if (m->y > 0 && x > 0 && mi[dd].slice == s)
    {
        m->ref[C(-1, -1)] = mi[dd].ref[3];
        mv[C(-1, -1)] = *(const h264_mvw *)mvs[dd][15];
    }
    else
    {
        m->ref[C(-1, -1)] = REF_NA;
        mv[C(-1, -1)] = 0;
    }
}

static void mb_setup(struct h264_dec *d, struct h264_mb *m, int addr, int skip)
{
    /* the next macroblock in a row without a divide (vc2sim counts
     * div.ss as one instruction; its cost on the core is unmeasured) */
    if (addr == m->addr + 1 && m->x + 1 < d->mb_w)
        m->x++;
    else
    {
        m->x = addr % d->mb_w;
        m->y = addr / d->mb_w;
    }
    m->addr = addr;
    if (skip)
        load_neighbours_skip(d, m);
    else
        load_neighbours(d, m);
}

static H264_NOINLINE void decode_skip(struct h264_dec *d, struct h264_mb *m, int addr,
                        int qp)
{
    int16_t mv[2] __attribute__((aligned(4)));

    mb_setup(d, m, addr, 1);
    m->flags = MBF_SKIP;
    m->qp = qp;
    h264_mv_pred_skip(m, mv);
    /* store_mb takes the rest (see there); the next macroblock reads its
     * neighbours from d->mbi and d->mvs, not from these caches */
    *(h264_mvw *)m->mv[C(0, 0)] = *(const h264_mvw *)mv;
    h264_inter_pred(d, d->reflist[0], 0, 0, 4, 4, mv);
    store_mb(d, m);
    d->stats.skipped++;
}

static H264_NOINLINE int decode_pcm(struct h264_dec *d, struct h264_bits *b,
                      struct h264_mb *m)
{
    int w = d->ls, i, j, k;                  /* the planes' stride */
    uint8_t *y = d->cur->y + m->y * 16 * w + m->x * 16;
    uint8_t *u = d->cur->u + m->y * 8 * (w / 2) + m->x * 8;
    uint8_t *v = d->cur->v + m->y * 8 * (w / 2) + m->x * 8;

    bs_get(b, (8 - (bs_tell(b) & 7)) & 7);       /* pcm_alignment_zero_bit */
    for (j = 0; j < 16; j++)
        for (i = 0; i < 16; i++)
            y[j * w + i] = bs_get(b, 8);
    for (j = 0; j < 8; j++)
        for (i = 0; i < 8; i++)
            u[j * (w / 2) + i] = bs_get(b, 8);
    for (j = 0; j < 8; j++)
        for (i = 0; i < 8; i++)
            v[j * (w / 2) + i] = bs_get(b, 8);
    for (k = 0; k < 16; k++)
        m->nz[C(k & 3, k >> 2)] = 16;
    for (k = 0; k < 4; k++)
        m->nzc[0][C(k & 1, k >> 1)] = m->nzc[1][C(k & 1, k >> 1)] = 16;
    return bs_overrun(b) ? -1 : 0;
}

static H264_NOINLINE int decode_mb(struct h264_dec *d, struct h264_bits *b,
                     struct h264_mb *m, int addr, int *qp)
{
    int w = d->ls, cw = d->cs;               /* the planes' strides */
    uint8_t *py, *pu, *pv;
    uint32_t t;
    int i16_mode = 0, r;

    mb_setup(d, m, addr, 0);
    py = d->cur->y + m->y * 16 * w + m->x * 16;
    pu = d->cur->u + m->y * 8 * cw + m->x * 8;
    pv = d->cur->v + m->y * 8 * cw + m->x * 8;

    t = bs_ue(b);
    if (d->sh.type == 0)
    {
        if (t < 5)
        {
            m->mb_type = MB_P16x16 + t;
            m->flags = 0;
            goto inter;
        }
        t -= 5;
    }
    if (t > 25)
        return -1;
    m->flags = MBF_INTRA;
    if (t == 25)
    {
        m->mb_type = MB_PCM;
        m->flags |= MBF_PCM;
        m->qp = *qp;
        m->cbp = 0;
        r = decode_pcm(d, b, m);
        if (r < 0)
            return r;
        store_mb(d, m);
        return 0;
    }
    if (t == 0)
    {
        m->mb_type = MB_I4x4;
        m->flags |= MBF_I4x4;
        intra4x4_pred_modes(d, b, m);
    }
    else
    {
        m->mb_type = MB_I16;
        m->flags |= MBF_I16;
        i16_mode = (t - 1) & 3;
        m->cbp = (((t - 1) >> 2) % 3) << 4 | (t >= 13 ? 15 : 0);
    }
    m->chroma_mode = bs_ue(b);
    if (m->chroma_mode > 3)
        return -1;
    goto cbp;

inter:
    r = inter_mb(d, b, m);
    if (r < 0)
        return r;

cbp:
    if (m->mb_type != MB_I16)
    {
        t = bs_ue(b);
        if (t > 47)
            return -1;
        m->cbp = (m->flags & MBF_INTRA) ? h264_cbp_intra[t] : h264_cbp_inter[t];
    }
    if (m->cbp || m->mb_type == MB_I16)
    {
        int32_t dq = bs_se(b);
        if (dq < -26 || dq > 25)
            return -1;
        dq += *qp;               /* -26 .. 76: one wrap at most */
        *qp = dq < 0 ? dq + 52 : dq >= 52 ? dq - 52 : dq;
    }
    m->qp = *qp;
    m->qpc = h264_chroma_qp[CLIP3(0, 51, m->qp + d->pps_cur->chroma_qp_offset)];
    r = h264_residual(b, m, m->mb_type == MB_I16);
    if (r < 0)
        return r;

#ifdef H264_DEBUG
    if (h264_debug_trace)
        h264_debug_trace(d, m, i16_mode);
#endif
    /* reconstruction */
    if (m->mb_type == MB_I4x4)
    {
        if (recon_intra4x4(d, m, py, w) < 0)
            return -1;
    }
    else if (m->mb_type == MB_I16)
    {
        if (h264_pred16x16(py, w, i16_mode, mb_avail_bits(m->iavail)) < 0)
            return -1;
        add_residual(d, m, py, pu, pv, m->coded & 0xffff);
    }
    if (m->flags & MBF_INTRA)
    {
        int av = mb_avail_bits(m->iavail);
        if (h264_pred_chroma8x8(pu, cw, m->chroma_mode, av) < 0 ||
            h264_pred_chroma8x8(pv, cw, m->chroma_mode, av) < 0)
            return -1;
        add_residual(d, m, py, pu, pv, m->coded & 0xffff0000u);
    }
    else                         /* inter: luma and chroma at once */
        add_residual(d, m, py, pu, pv, m->coded);
    store_mb(d, m);
    return 0;
}

int h264_decode_slice_data(struct h264_dec *d, struct h264_bits *b)
{
    struct h264_mb *m = &d->mb;
    int addr = d->sh.first_mb, qp = d->sh.qp, skip_run = -1, r;

    for (;;)
    {
        if (addr >= d->mbs || d->mbi[addr].slice != 0xffff)
            return H264_ERR(4, H264_E_BITSTREAM);
        if (d->sh.type == 0)
        {
            if (skip_run < 0)
            {
                uint32_t v = bs_ue(b);
                if (v > (uint32_t)(d->mbs - addr) || b->err)
                    return H264_ERR(4, H264_E_BITSTREAM);
                skip_run = v;
            }
            if (skip_run > 0)
            {
                if (!d->reflist[0])
                    return H264_ERR(4, H264_E_STATE);
                H264_PROG(0x31, addr | 0x10000);
                decode_skip(d, m, addr, qp);
                d->mbs_done++;
                d->stats.mbs++;
                addr++;
                if (--skip_run == 0 && !more_data(d, b))
                    break;
                continue;
            }
            skip_run = -1;
        }
        H264_PROG(0x31, addr);
        r = decode_mb(d, b, m, addr, &qp);
        if (r < 0 || b->err)
        {
            memset(m->coef, 0, sizeof(m->coef));
            return H264_ERR(4, H264_E_BITSTREAM);
        }
        d->mbs_done++;
        d->stats.mbs++;
        addr++;
        if (!more_data(d, b))
            break;
    }
    return bs_overrun(b) ? H264_ERR(4, H264_E_BITSTREAM) : H264_OK;
}
