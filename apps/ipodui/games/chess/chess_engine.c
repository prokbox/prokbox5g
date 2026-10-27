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

/* The chess engine: see chess_engine.h. */
#include "chess_engine.h"

/* -- evaluation ------------------------------------------------------------ */
static int iabs(int v)
{
    return v < 0 ? -v : v;
}

void ce_build_pst(int32_t (*t)[64])
{
    static const int8_t p_mg[8] = { 0, 0, 2, 6, 12, 22, 36, 0 };
    static const int8_t p_ctr[8] = { 0, -8, 6, 14, 10, 4, 0, 0 };
    static const int8_t p_eg[8] = { 0, 0, 6, 14, 26, 46, 78, 0 };
    static const int8_t k_back[8] = { 12, 20, 10, 0, 0, 4, 24, 14 };
    int s;

    for (s = 0; s < 64; s++)
    {
        int f = CS_FILE(s), r = CS_RANK(s);
        int cd = (iabs(2 * f - 7) + iabs(2 * r - 7)) / 2 - 1;  /* 0..6 */
        bool mid = f == 3 || f == 4;
        int kmg = r == 0 ? k_back[f] : r == 1 ? -12 : -30 - 12 * (r - 2);

        t[CS_P - 1][s] = CS_S(85 + p_mg[r] + (mid ? p_ctr[r] : 0),
                              100 + p_eg[r]);
        t[CS_N - 1][s] = CS_S(330 - 8 * cd - (r == 0 ? 12 : 0), 290 - 6 * cd);
        t[CS_B - 1][s] = CS_S(350 - 4 * cd - (r == 0 ? 8 : 0), 300 - 4 * cd);
        t[CS_R - 1][s] = CS_S(480 + (r == 6 ? 16 : 0)
                              + (mid && r == 0 ? 8 : 0), 520 + (r == 6 ? 12 : 0));
        t[CS_Q - 1][s] = CS_S(1000 - 2 * cd, 940 - 5 * cd);
        t[CS_K - 1][s] = CS_S(kmg, 24 - 10 * cd);
    }
}

int ce_eval(const struct cs_pos *p)
{
    int ph = p->phase > 24 ? 24 : p->phase;
    int s = CS_MG(p->psq) * ph + CS_EG(p->psq) * (24 - ph);

    s = (s * 1365) >> 15;               /* / 24, without a divide */
    if (p->cnt[CS_B] >= 2)
        s += 30;
    if (p->cnt[CS_B | CS_BLACK] >= 2)
        s -= 30;
    return (p->side ? -s : s) + 8;      /* + the move */
}

/* -- the transposition table ---------------------------------------------- */
void ce_init(struct ce *e, struct ce_tt *tt, int tt_bits)
{
    uint32_t i, n = 1u << tt_bits;

    e->tt = tt;
    e->tt_mask = n - 1;
    for (i = 0; i < n; i++)
    {
        tt[i].check = 0;
        tt[i].move = 0;
        tt[i].score = 0;
        tt[i].depth = 0;
        tt[i].flag = CE_TT_NONE;
    }
    e->state = CE_IDLE;
    e->ply = -1;
}

static int to_tt(int v, int ply)
{
    return v > CE_MATE_MAX ? v + ply : v < -CE_MATE_MAX ? v - ply : v;
}

static int from_tt(int v, int ply)
{
    return v > CE_MATE_MAX ? v - ply : v < -CE_MATE_MAX ? v + ply : v;
}

static void tt_store(struct ce *e, int depth, int v, int flag, uint16_t m,
                     int ply)
{
    struct ce_tt *t = &e->tt[e->pos.key & e->tt_mask];
    uint16_t chk = (uint16_t)(e->pos.key >> 16);

    /* replace a shallower entry, or another position's */
    if (t->flag != CE_TT_NONE && t->check == chk && t->depth > depth)
        return;
    if (!m && t->check == chk)
        m = t->move;                    /* keep a known best move */
    t->check = chk;
    t->move = m;
    t->score = (int16_t)to_tt(v, ply);
    t->depth = (uint8_t)(depth < 0 ? 0 : depth);
    t->flag = (uint8_t)flag;
}

/* -- the search ------------------------------------------------------------ */
static int noise_of(const struct ce *e, uint16_t m)
{
    uint32_t h;

    if (e->noise <= 0)
        return 0;
    h = (m ^ e->seed) * 0x9e3779b1u;
    h ^= h >> 15;
    return (int)(h % (uint32_t)(2 * e->noise + 1)) - e->noise;
}

static void order(struct ce *e, struct ce_frame *f, int ply)
{
    const struct cs_pos *p = &e->pos;
    int i;

    for (i = 0; i < f->n; i++)
    {
        uint16_t m = e->mv[f->m0 + i];
        int s = 0;

        if (ply == 0 && m == e->best)
            s = 31000;                  /* the last iteration's best */
        else if (m == f->tt_move)
            s = 30000;
        else if (CS_IS_CAP(m))
        {
            int vic = CS_FLAGS(m) == CS_F_EP ? CS_P
                      : CS_TYPE(p->b[CS_TO88(CS_TO(m))]);

            s = 20000 + vic * 100 - CS_TYPE(p->b[CS_TO88(CS_FROM(m))])
                + (CS_IS_PROMO(m) ? CS_PROMO_TYPE(m) * 10 : 0);
        }
        else if (CS_IS_PROMO(m))
            s = 19000 + CS_PROMO_TYPE(m);
        else if (m == e->killer[ply][0])
            s = 18000;
        else if (m == e->killer[ply][1])
            s = 17999;
        e->sc[f->m0 + i] = (int16_t)s;
    }
}

/* the child position at ply is on the board: its value now (returns true,
 * *v), or a frame pushed for it (false) */
static bool enter(struct ce *e, int ply, int alpha, int beta, int depth,
                  int *v)
{
    struct cs_pos *p = &e->pos;
    struct ce_frame *f;
    uint16_t tt_move = 0;
    int m0, check, stand = 0;
    bool qs;

    if (ply > 0)
    {
        if (p->half >= 100 || cs_repeats(p) >= 1 || cs_dead(p))
        {
            *v = 0;
            return true;
        }
    }
    check = cs_in_check(p);
    if (check && depth >= 0 && ply < CE_MAXPLY - 12)
        depth++;                        /* look past a check */
    m0 = ply ? e->fr[ply - 1].m0 + e->fr[ply - 1].n : 0;
    if (ply >= CE_MAXPLY - 1 || m0 > CE_MSTACK - CS_MOVES_MAX)
    {
        *v = ce_eval(p);
        return true;
    }
    {
        const struct ce_tt *t = &e->tt[p->key & e->tt_mask];

        if (t->flag != CE_TT_NONE && t->check == (uint16_t)(p->key >> 16))
        {
            int s = from_tt(t->score, ply);

            tt_move = t->move;
            if (ply > 0 && depth > 0 && t->depth >= depth
                && (t->flag == CE_TT_EXACT
                    || (t->flag == CE_TT_LOWER && s >= beta)
                    || (t->flag == CE_TT_UPPER && s <= alpha)))
            {
                *v = s;
                return true;
            }
        }
    }
    qs = depth <= 0;
    if (qs)
    {
        stand = ce_eval(p);
        if (stand >= beta)
        {
            *v = stand;
            return true;
        }
        if (stand > alpha)
            alpha = stand;
    }
    f = &e->fr[ply];
    f->m0 = (uint16_t)m0;
    f->n = (uint16_t)cs_gen(p, &e->mv[m0], qs);
    f->i = 0;
    f->alpha = (int16_t)alpha;
    f->alpha0 = (int16_t)alpha;
    f->beta = (int16_t)beta;
    f->best = (int16_t)(qs ? stand : -CE_INF);
    f->best_move = 0;
    f->tt_move = tt_move;
    f->depth = (int8_t)depth;
    f->qs = qs;
    f->legal = 0;
    f->check = (uint8_t)check;
    order(e, f, ply);
    e->ply = (int8_t)ply;
    return false;
}

/* the frame at e->ply is done: its value goes to its parent */
static void finish(struct ce *e)
{
    int ply = e->ply;
    struct ce_frame *f = &e->fr[ply];
    int v = f->best;

    if (!f->qs)
    {
        if (!f->legal)
            v = f->check ? -CE_MATE + ply : 0;
        tt_store(e, f->depth, v, v <= f->alpha0 ? CE_TT_UPPER
                 : v >= f->beta ? CE_TT_LOWER : CE_TT_EXACT,
                 f->best_move, ply);
    }
    e->ply = (int8_t)(ply - 1);
    e->ret = (int16_t)v;
    e->have_ret = 1;
}

static void iteration(struct ce *e)
{
    int v;

    e->root_best = 0;
    e->root_score = -CE_INF;
    e->root_done = 0;
    e->have_ret = 0;
    enter(e, 0, -CE_INF, CE_INF, e->depth, &v);
    /* at the root a frame is always pushed (no cut-offs at ply 0, the
     * mstack is empty) */
}

void ce_start(struct ce *e, const struct cs_pos *p, int max_depth,
              int noise, uint32_t seed)
{
    int i;

    e->pos = *p;
    e->nodes = 0;
    e->seed = seed;
    e->noise = (int16_t)noise;
    e->max_depth = (int8_t)(max_depth < 1 ? 1 : max_depth > CE_MAXPLY - 14
                            ? CE_MAXPLY - 14 : max_depth);
    e->best = 0;
    e->score = 0;
    e->done_depth = 0;
    for (i = 0; i < CE_MAXPLY; i++)
        e->killer[i][0] = e->killer[i][1] = 0;
    e->depth = 1;
    e->state = CE_RUN;
    iteration(e);
}

int ce_run(struct ce *e, uint32_t nodes)
{
    while (e->state == CE_RUN && nodes)
    {
        struct ce_frame *f;
        uint16_t m;
        int v, k, bi;

        if (e->ply < 0)
        {
            /* the root came back: an iteration is complete */
            if (e->root_best)
            {
                e->best = e->root_best;
                e->score = e->root_score;
            }
            e->done_depth = e->depth;
            if (!e->best || e->depth >= e->max_depth
                || e->score > CE_MATE_MAX || e->score < -CE_MATE_MAX)
            {
                e->state = CE_DONE;
                break;
            }
            e->depth++;
            iteration(e);
            continue;
        }
        f = &e->fr[e->ply];
        if (e->have_ret)
        {
            e->have_ret = 0;
            m = e->mv[f->m0 + f->i - 1];
            cs_unmake(&e->pos, &e->un[e->ply]);
            v = -e->ret;
            if (e->ply == 0)
            {
                v += noise_of(e, m);
                e->root_done++;
            }
            if (v > f->best)
            {
                f->best = (int16_t)v;
                f->best_move = m;
                if (e->ply == 0)
                {
                    e->root_best = m;
                    e->root_score = (int16_t)v;
                }
                if (v > f->alpha)
                {
                    f->alpha = (int16_t)v;
                    if (v >= f->beta)
                    {
                        if (!CS_IS_CAP(m) && !CS_IS_PROMO(m)
                            && e->killer[e->ply][0] != m)
                        {
                            e->killer[e->ply][1] = e->killer[e->ply][0];
                            e->killer[e->ply][0] = m;
                        }
                        finish(e);
                        continue;
                    }
                }
            }
        }
        if (f->i >= f->n)
        {
            finish(e);
            continue;
        }
        /* the best-ordered of the rest */
        bi = f->i;
        for (k = f->i + 1; k < f->n; k++)
            if (e->sc[f->m0 + k] > e->sc[f->m0 + bi])
                bi = k;
        if (bi != f->i)
        {
            uint16_t tm = e->mv[f->m0 + bi];
            int16_t ts = e->sc[f->m0 + bi];

            e->mv[f->m0 + bi] = e->mv[f->m0 + f->i];
            e->sc[f->m0 + bi] = e->sc[f->m0 + f->i];
            e->mv[f->m0 + f->i] = tm;
            e->sc[f->m0 + f->i] = ts;
        }
        m = e->mv[f->m0 + f->i];
        f->i++;
        if (!cs_make_legal(&e->pos, m, &e->un[e->ply]))
            continue;
        f->legal++;
        e->nodes++;
        nodes--;
        if (enter(e, e->ply + 1, -f->beta, -f->alpha, f->depth - 1, &v))
        {
            e->ret = (int16_t)v;
            e->have_ret = 1;
        }
    }
    return e->state;
}

uint16_t ce_stop(struct ce *e)
{
    uint16_t m = e->best;

    /* an unfinished iteration searched the last one's best first
     * (order()): whatever it holds now is at least as good, one ply
     * deeper */
    if (e->state == CE_RUN && e->root_done && e->root_best)
        m = e->root_best;
    e->state = CE_IDLE;
    return m;
}
