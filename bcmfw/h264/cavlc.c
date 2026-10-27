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
 * H.264 decoder: the residual of a macroblock (7.3.5.3) with CAVLC
 * (7.3.5.3.2, 9.2), table driven, dequantised straight into the blocks.
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

/* total_zeros' rows (h264_tzd_row): luma, chroma DC */
#define TZ_LUMA    h264_tzd_row
#define TZ_CDC     (h264_tzd_row + 15)
static const struct h264_vlc *const ct_tab[9] = {
    &h264_vlc_coeff_token_cdc,                       /* nC = -1 */
    &h264_vlc_coeff_token0, &h264_vlc_coeff_token0,
    &h264_vlc_coeff_token1, &h264_vlc_coeff_token1,
    &h264_vlc_coeff_token2, &h264_vlc_coeff_token2,
    &h264_vlc_coeff_token2, &h264_vlc_coeff_token2
};

/* Where each scan position's level goes and what it is multiplied by,
 * as one word: mul << 4 | raster index (see dq_table).  The DC levels are
 * transformed before scaling, so they are written unscaled: luma DC in
 * zig-zag order, chroma DC in scan order. */
static const int32_t tab_luma_dc[16] = {
    0x10, 0x11, 0x14, 0x18, 0x15, 0x12, 0x13, 0x16,
    0x19, 0x1c, 0x1d, 0x1a, 0x17, 0x1b, 0x1e, 0x1f
};
static const int32_t tab_chroma_dc[4] = { 0x10, 0x11, 0x12, 0x13 };

/* the flat-matrix dequantisation (8.5.12.1) by scan position:
 * v(qp % 6, zigzag[p]) << (qp / 6), built once per QP and stream (the
 * values depend on nothing else) */
static H264_NOINLINE void dq_table(int32_t *tab, int qp)
{
    const uint8_t *dq = h264_dequant4[h264_qp_mod6[qp]];
    int qd = h264_qp_div6[qp], p;
    for (p = 0; p < 16; p++)
    {
        int z = h264_zigzag4x4[p];
        tab[p] = (dq[z] << qd) << 4 | z;
    }
}

static inline const int32_t *dq_for(struct h264_mb *m, int qp)
{
    uint32_t bit = 1u << (qp & 31);
    if (!(m->dq_built[qp >> 5] & bit))
    {
        dq_table(m->dq[qp], qp);
        m->dq_built[qp >> 5] |= bit;
    }
    return m->dq[qp];
}

/* The reader's hot state, cache c and count n, lives in the caller's
 * locals (registers); pos, buf and size stay in *bp and are touched only
 * by a refill.  Same rules as bs_fill in h264_int.h.  FILL(k) makes sure
 * of k <= 25 bits: the fewer a symbol can need, the less often a refill
 * runs, and the more whole bytes each one takes.  A level needs 22: a
 * prefix of up to 15 zeros is seen whole, and its suffix is at most 6
 * bits (prefix 15 refills for its 12-bit suffix). */
#define FILL(k) \
    do { \
        if (n < (k)) \
        { \
            uint32_t pos_ = bp->pos, w_; \
            int k_ = (32 - n) >> 3; \
            if (pos_ + 4 <= bp->size) \
            { \
                const uint8_t *p_ = bp->buf + pos_; \
                w_ = (uint32_t)p_[0] << 24 | (uint32_t)p_[1] << 16 | \
                     (uint32_t)p_[2] << 8 | p_[3]; \
            } \
            else \
                w_ = bs_word_tail(bp->buf, pos_, bp->size); \
            c |= w_ >> n; \
            bp->pos = pos_ + k_; \
            n += 8 * k_; \
        } \
    } while (0)
#define SKIP(k) do { c <<= (k); n -= (k); } while (0)

/* one code of table t from c (at least 16 valid bits): length << 8 |
 * value, see h264_vlc */
static inline uint32_t vlc(uint32_t c, const struct h264_vlc *t)
{
    int lz = __builtin_clz(c | 1);
    uint32_t r;
    if (lz > t->lzmax)
        lz = t->lzmax;
    r = t->row[lz];
    return t->e[(r >> 5) + (c >> (r & 31))];
}

/* nC from the left and upper blocks' TotalCoeff (9.2.1) */
static inline int pred_nc(const int8_t *nz, int x, int y)
{
    int na = nz[C(x - 1, y)], nb = nz[C(x, y - 1)];
    if (na >= 0 && nb >= 0)
        return (na + nb + 1) >> 1;
    if (na >= 0)
        return na;
    if (nb >= 0)
        return nb;
    return 0;
}

/* One residual block (9.2) of up to max coefficients, for nC = nc (-1:
 * chroma DC).  The level at scan position p goes, times tab[p] >> 4, to
 * coef[tab[p] & 15]; the other coefficients are left alone (zero).
 * Products beyond 16 bits wrap; only a non-conforming stream has them.
 * tzr: total_zeros' rows for this kind of block (TZ_LUMA, TZ_CDC).  Returns TotalCoeff, or -1 on an error; a block of
 * one coefficient is DC only when coef[0] is set (a level is never 0).
 * Out of line: its own registers hold the reader's cache and count (see
 * FILL; they go back to *bp at the end), so neither it nor the caller's
 * loops spill (inlined five times, they did, and cost 5 KB). */
static H264_NOINLINE
int block(struct h264_bits *bp, int nc, int max,
          int16_t *coef, const int32_t *tab, const uint32_t *tzr)
{
    int level[16];
    int tc, t1, v, i, sl, zeros, p, code, len, thr;
    uint32_t w, c = bp->cache;
    int n = bp->n;

    /* coeff_token (9.2.1); the trailing ones' signs come from the same
     * fill (at most 16 + 3 bits) */
    FILL(19);                    /* coeff_token and the signs */
    if (nc < 8)
    {
        uint32_t e = vlc(c, ct_tab[nc + 1]);
        SKIP(e >> 8);
        v = e & 0xff;
    }
    else
    {
        v = c >> 26;             /* 6-bit fixed length: xxxx = tc-1, yy = t1 */
        SKIP(6);
        v = v == 3 ? 0 : ((v >> 2) + 1) << 2 | (v & 3);
        if ((v & 3) > v >> 2)
            return -1;
    }
    tc = v >> 2;                 /* unused words: 63 */
    t1 = v & 3;
    if (tc == 0)
    {
        bp->cache = c;
        bp->n = n;
        return 0;
    }
    if (tc > max)
        return -1;

    /* levels (9.2.2): trailing ones, -1 for a set bit */
    for (i = 0; i < t1; i++)
    {
        level[i] = (int32_t)c >> 31 | 1;
        c <<= 1;
    }
    n -= t1;
    if (i < tc)
    {
        /* the first other level: suffixLength 0 or 1, and + 2 when there
         * are fewer than three trailing ones */
        int lz;
        sl = tc > 10 && t1 < 3;
        /* refill only when the code may not be whole in the cache: a
         * leading 1 among the n valid bits and lz + 7 >= its length (the
         * bits below n are zeros or the stream's own, so a refill leaves
         * the leading zeros as they are when lz < n) */
        w = c;
        lz = __builtin_clz(w | 1);
        if (n < lz + 7)
        {
            FILL(22);
            w = c;
            lz = __builtin_clz(w | 1);
        }
        if (lz < 14 || (lz == 14 && sl))
        {
            /* the top lz + 1 + sl bits are 1 << sl | level_suffix */
            len = lz + 1 + sl;
            code = (lz << sl) + (int)(w >> (32 - len)) - (1 << sl);
        }
        else if (lz == 14)       /* suffixLength 0: a 4-bit suffix */
        {
            len = 19;
            code = 14 + (int)((w >> 13) & 15);
        }
        else if (lz == 15)       /* level_prefix 15: a 12-bit suffix */
        {
            SKIP(16);
            FILL(12);
            len = 12;
            code = (15 << sl) + (int)(c >> 20) + (sl ? 0 : 15);
        }
        else                     /* level_prefix > 15: not Baseline */
            return -1;
        SKIP(len);
        if (t1 < 3)
            code += 2;
        v = (code + 2) >> 1;
        level[i] = (code & 1) ? -v : v;
        /* suffixLength goes up once |level| > 3 << (suffixLength - 1),
         * i.e. levelCode >= 3 << suffixLength (9.2.2.1), up to 6 */
        if (!sl)
            sl = 1;
        thr = 3 << sl;
        if (code >= thr)
        {
            sl++;
            thr <<= 1;
        }
        for (i++; i < tc; i++)
        {
            int m;               /* the leading 1: bit m, lz = 31 - m */
            w = c;
            m = 31 - __builtin_clz(w | 1);
            if (n < 38 - m)      /* lz + 7: see the first level */
            {
                FILL(22);
                w = c;
                m = 31 - __builtin_clz(w | 1);
            }
            if (m > 16)
            {
                /* w >> (m - sl) = 1 << sl | level_suffix; prefix lz */
                len = 32 - m + sl;
                code = (int)((uint32_t)(30 - m) << sl) + (int)(w >> (m - sl));
            }
            else if (m == 16)
            {
                SKIP(16);
                FILL(12);
                len = 12;
                code = (15 << sl) + (int)(c >> 20);
            }
            else
                return -1;
            SKIP(len);
            v = (code + 2) >> 1;
            level[i] = (code & 1) ? -v : v;
            if (code >= thr)
            {
                sl++;
                thr = sl < 6 ? thr << 1 : 0x7fffffff;
            }
        }
    }

    /* total_zeros and run_before (9.2.3); level[0] is the highest
     * frequency coefficient */
    if (tc < max)
    {
        uint32_t e, r = tzr[tc - 1];
        FILL(9);
        e = h264_tzd[(r >> 5) + (c >> (r & 31))];
        SKIP(e >> 8);
        zeros = e & 0xff;
        if (zeros > max - tc)
            return -1;
    }
    else
        zeros = 0;
    p = tc + zeros - 1;
    for (i = 0; zeros > 0 && i < tc - 1; i++)
    {
        int run;
        v = tab[p];
        coef[v & 15] = level[i] * (v >> 4);
        FILL(11);
        w = c;
        if (zeros <= 6)
        {
            int e = h264_rb3[zeros - 1][w >> 29];
            run = e & 15;
            len = e >> 4;
        }
        else if (w >= 0x20000000u)
        {
            run = 7 - (int)(w >> 29);
            len = 3;
        }
        else
        {
            int lz = __builtin_clz(w | 1);
            run = lz + 4;        /* 0001 = 7 ... 00000000001 = 14 */
            len = lz + 1;
        }
        if (run > zeros)         /* also every word longer than 11 bits */
            return -1;
        SKIP(len);
        zeros -= run;
        p -= run + 1;
    }
    for (; i < tc; i++, p--)     /* the rest are consecutive */
    {
        v = tab[p];
        coef[v & 15] = level[i] * (v >> 4);
    }
    bp->cache = c;
    bp->n = n;
    return tc;
}

int h264_residual(struct h264_bits *bp, struct h264_mb *m, int i16)
{
    const int32_t *tab;
    int k, t, i, cbp = m->cbp;
    uint32_t coded = 0, dconly = 0;  /* m->coded, m->dconly, in registers */

    m->coded = 0;
    m->dconly = 0;
    if (!(cbp | i16))            /* nothing coded: every TotalCoeff is 0 */
    {
        for (k = 0; k < 4; k++)
            *(h264_w4 *)&m->nz[C(0, k)] = 0;
        for (k = 0; k < 2; k++)
        {
            int8_t *nz = m->nzc[k];
            nz[C(0, 0)] = nz[C(1, 0)] = nz[C(0, 1)] = nz[C(1, 1)] = 0;
        }
        return 0;
    }
    tab = dq_for(m, m->qp) + i16;    /* AC blocks start at scan position 1 */
    if (i16)
    {
        int16_t dc[16] __attribute__((aligned(4)));
        int mask;
        memset(dc, 0, sizeof(dc));
        t = block(bp, pred_nc(m->nz, 0, 0), 16, dc, tab_luma_dc,
                  TZ_LUMA);
        if (t < 0)
            return -1;
        mask = t ? h264_luma_dc_dequant(m->coef[0], dc, m->qp) : 0;
        coded = mask;
        dconly = mask;
    }
    for (k = 0; k < 4; k++)
    {
        /* an 8x8 at a time: its first 4x4's raster index and cache entry
         * (C(x, y) of raster r = 4y + x is r + 4y + 12) */
        int r0 = (k & 1) * 2 + (k & 2) * 4, b4;
        int8_t *nz0 = &m->nz[r0 + (r0 & ~3) + C(0, 0)];
        if (!(cbp & (1 << k)))
        {
            /* the whole 8x8 is empty */
            nz0[0] = nz0[1] = nz0[8] = nz0[9] = 0;
            continue;
        }
        for (b4 = 0; b4 < 4; b4++)
        {
            int r = r0 + (b4 & 1) + (b4 & 2) * 2, na, nb, nc;
            int8_t *nz = nz0 + (b4 & 1) + (b4 & 2) * 4;
            na = nz[-1];
            nb = nz[-8];         /* pred_nc's rule (9.2.1) */
            nc = (na | nb) >= 0 ? (na + nb + 1) >> 1 : na >= 0 ? na : nb >= 0 ? nb : 0;
            t = block(bp, nc, 16 - i16, m->coef[r], tab, TZ_LUMA);
            if (t < 0)
                return -1;
            nz[0] = t;
            if (!t)
                continue;
            coded |= 1u << r;
            if (i16)
                dconly &= ~(1u << r);
            else if (t == 1 && m->coef[r][0])    /* one coefficient, the DC */
                dconly |= 1u << r;
        }
    }
    if (cbp >> 4)
    {
        /* chroma DC, Cb then Cr */
        for (k = 0; k < 2; k++)
        {
            int16_t dc[4] __attribute__((aligned(4))) = { 0, 0, 0, 0 };
            t = block(bp, -1, 4, dc, tab_chroma_dc, TZ_CDC);
            if (t < 0)
                return -1;
            if (t)
            {
                int mask = h264_chroma_dc_dequant(m->coef[16 + k * 4], dc,
                                                  m->qpc);
                coded |= (uint32_t)mask << (16 + k * 4);
                dconly |= (uint32_t)mask << (16 + k * 4);
            }
        }
    }
    if (cbp >> 4 == 2)
    {
        tab = dq_for(m, m->qpc) + 1;
        for (k = 0; k < 2; k++)
            for (i = 0; i < 4; i++)
            {
                int x = i & 1, y = i >> 1, blk = 16 + k * 4 + i;
                t = block(bp, pred_nc(m->nzc[k], x, y), 15, m->coef[blk],
                          tab, TZ_LUMA);
                if (t < 0)
                    return -1;
                m->nzc[k][C(x, y)] = t;
                if (!t)
                    continue;
                coded |= 1u << blk;
                dconly &= ~(1u << blk);
            }
    }
    else
        for (k = 0; k < 2; k++)
        {
            int8_t *nz = m->nzc[k];
            nz[C(0, 0)] = nz[C(1, 0)] = nz[C(0, 1)] = nz[C(1, 1)] = 0;
        }
    m->coded = coded;
    m->dconly = dconly;
    return bs_overrun(bp) ? -1 : 0;
}
