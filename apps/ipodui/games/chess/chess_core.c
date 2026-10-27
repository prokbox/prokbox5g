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

/* Chess rules: see chess_core.h. A 0x88 board (a square is off the board
 * when s & 0x88), chosen over bitboards for the PP5022's ARM7TDMI: no
 * 64-bit registers, no count-leading-zeros, Thumb code; one byte per
 * square, ray scans with one AND a step (docs/design/games.md 5.6). */
#include "chess_core.h"

static const int8_t kn_d[8] = { 14, 18, 31, 33, -14, -18, -31, -33 };
static const int8_t kg_d[8] = { 1, 15, 16, 17, -1, -15, -16, -17 };
/* rook directions first (0..3), then the bishop's (4..7) */
static const int8_t sl_d[8] = { 1, 16, -1, -16, 15, 17, -15, -17 };

static const int32_t (*pst)[64];

void cs_set_pst(const int32_t (*t)[64])
{
    pst = t;
}

/* -- the hash: keys from a mixing function, not a 6 KB table ------------- */
static uint32_t zk(uint32_t i)
{
    /* an integer finaliser (two multiply-xorshift rounds) */
    i = i * 0x9e3779b9u + 0x632be5abu;
    i ^= i >> 16;
    i *= 0x7feb352du;
    i ^= i >> 15;
    i *= 0x846ca68bu;
    i ^= i >> 16;
    return i;
}
#define Z_PIECE(pc, s64)    zk((uint32_t)(pc) * 64 + (uint32_t)(s64))
#define Z_CASTLE(c)         zk(16 * 64 + (uint32_t)(c))
#define Z_EP(s88)           zk(17 * 64 + (uint32_t)((s88) & 7))
#define Z_SIDE              0x9b05688cu

static const uint8_t phase_of[8] = { 0, 0, 1, 1, 2, 4, 0, 0 };

static int32_t psq_of(int pc, int s88)
{
    int s = CS_TO64(s88);

    if (!pst)
        return 0;
    if (CS_COLOR(pc))
        return -pst[CS_TYPE(pc) - 1][s ^ 56];
    return pst[CS_TYPE(pc) - 1][s];
}

static void put(struct cs_pos *p, int s, int pc)
{
    p->b[s] = (uint8_t)pc;
    p->cnt[pc]++;
    p->phase = (uint8_t)(p->phase + phase_of[CS_TYPE(pc)]);
    p->key ^= Z_PIECE(pc, CS_TO64(s));
    p->psq += psq_of(pc, s);
}

static void take(struct cs_pos *p, int s)
{
    int pc = p->b[s];

    p->b[s] = CS_EMPTY;
    p->cnt[pc]--;
    p->phase = (uint8_t)(p->phase - phase_of[CS_TYPE(pc)]);
    p->key ^= Z_PIECE(pc, CS_TO64(s));
    p->psq -= psq_of(pc, s);
}

static uint32_t base_key(const struct cs_pos *p)
{
    uint32_t k = Z_CASTLE(p->castle);

    if (p->ep != CS_NOSQ)
        k ^= Z_EP(p->ep);
    if (p->side)
        k ^= Z_SIDE;
    return k;
}

static void clear(struct cs_pos *p, uint32_t *rep)
{
    int i;

    for (i = 0; i < 128; i++)
        p->b[i] = CS_EMPTY;
    for (i = 0; i < 16; i++)
        p->cnt[i] = 0;
    p->side = 0;
    p->castle = 0;
    p->ep = CS_NOSQ;
    p->phase = 0;
    p->king[0] = p->king[1] = 0;
    p->half = 0;
    p->ply = 0;
    p->rsv = 0;
    p->key = 0;
    p->psq = 0;
    p->rep = rep;
}

void cs_start(struct cs_pos *p, uint32_t *rep)
{
    static const uint8_t back[8] = { CS_R, CS_N, CS_B, CS_Q, CS_K, CS_B,
                                     CS_N, CS_R };
    int f;

    clear(p, rep);
    for (f = 0; f < 8; f++)
    {
        put(p, f, back[f]);
        put(p, 0x10 + f, CS_P);
        put(p, 0x60 + f, CS_P | CS_BLACK);
        put(p, 0x70 + f, back[f] | CS_BLACK);
    }
    p->king[0] = 0x04;
    p->king[1] = 0x74;
    p->castle = CS_WK | CS_WQ | CS_BK | CS_BQ;
    p->key ^= base_key(p);
}

int cs_set_fen(struct cs_pos *p, const char *fen, uint32_t *rep)
{
    static const char names[] = "pnbrqk";
    int r = 7, f = 0, n = 0;
    const char *s = fen;

    clear(p, rep);
    for (; *s && *s != ' '; s++)
    {
        int i;

        if (*s == '/')
        {
            r--;
            f = 0;
            continue;
        }
        if (*s >= '1' && *s <= '8')
        {
            f += *s - '0';
            continue;
        }
        for (i = 0; i < 6; i++)
            if (*s == names[i] || *s == names[i] - 32)
                break;
        if (i == 6 || r < 0 || f > 7)
            return -1;
        put(p, f + 16 * r, (i + 1) | (*s == names[i] ? CS_BLACK : 0));
        if (i == 5)
            p->king[*s == names[i]] = (uint8_t)(f + 16 * r);
        f++;
    }
    if (*s++ != ' ')
        return -1;
    p->side = *s == 'b';
    s += 2;
    for (; *s && *s != ' '; s++)
        p->castle |= *s == 'K' ? CS_WK : *s == 'Q' ? CS_WQ
                   : *s == 'k' ? CS_BK : *s == 'q' ? CS_BQ : 0;
    if (*s == ' ')
        s++;
    if (*s >= 'a' && *s <= 'h' && s[1] >= '1' && s[1] <= '8')
    {
        p->ep = (uint8_t)((*s - 'a') + 16 * (s[1] - '1'));
        s += 2;
    }
    else if (*s == '-')
        s++;
    while (*s == ' ')
        s++;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    p->half = (uint16_t)n;
    p->key ^= base_key(p);
    return 0;
}

bool cs_attacked(const struct cs_pos *p, int s, int by)
{
    const uint8_t *b = p->b;
    int i, t, c = by ? CS_BLACK : 0;

    /* pawns: a white pawn on s-15 or s-17 attacks s */
    t = by ? s + 15 : s - 15;
    if (!(t & 0x88) && b[t] == (CS_P | c))
        return true;
    t = by ? s + 17 : s - 17;
    if (!(t & 0x88) && b[t] == (CS_P | c))
        return true;
    for (i = 0; i < 8; i++)
    {
        t = s + kn_d[i];
        if (!(t & 0x88) && b[t] == (CS_N | c))
            return true;
        t = s + kg_d[i];
        if (!(t & 0x88) && b[t] == (CS_K | c))
            return true;
    }
    for (i = 0; i < 8; i++)
    {
        int d = sl_d[i];

        for (t = s + d; !(t & 0x88); t += d)
        {
            int pc = b[t];

            if (!pc)
                continue;
            if (CS_COLOR(pc) == by)
            {
                int ty = CS_TYPE(pc);

                if (ty == CS_Q || ty == (i < 4 ? CS_R : CS_B))
                    return true;
            }
            break;
        }
    }
    return false;
}

static int add_promos(uint16_t *out, int n, int f, int t, int cap, bool caps)
{
    int k;

    if (caps)                           /* quiescence: the queen only */
    {
        out[n++] = CS_MV(f, t, (cap ? CS_F_PROMO_CAP : CS_F_PROMO) + 3);
        return n;
    }
    for (k = 3; k >= 0; k--)
        out[n++] = CS_MV(f, t, (cap ? CS_F_PROMO_CAP : CS_F_PROMO) + k);
    return n;
}

int cs_gen(const struct cs_pos *p, uint16_t *out, bool caps)
{
    const uint8_t *b = p->b;
    int us = p->side, them = us ^ 1, n = 0, s;
    int up = us ? -16 : 16;

    for (s = 0; s < 128; s++)
    {
        int pc, ty, i, t, f64;

        if (s & 0x88)
        {
            s += 7;
            continue;
        }
        pc = b[s];
        if (!pc || CS_COLOR(pc) != us)
            continue;
        ty = CS_TYPE(pc);
        f64 = CS_TO64(s);
        if (ty == CS_P)
        {
            int last = us ? 0 : 7, rank = s >> 4;

            t = s + up;
            if (!b[t])
            {
                if ((t >> 4) == last)
                    n = add_promos(out, n, f64, CS_TO64(t), 0, caps);
                else if (!caps)
                {
                    out[n++] = CS_MV(f64, CS_TO64(t), CS_F_QUIET);
                    if (rank == (us ? 6 : 1) && !b[t + up])
                        out[n++] = CS_MV(f64, CS_TO64(t + up), CS_F_DOUBLE);
                }
            }
            for (i = -1; i <= 1; i += 2)
            {
                t = s + up + i;
                if (t & 0x88)
                    continue;
                if (b[t] && CS_COLOR(b[t]) == them)
                {
                    if ((t >> 4) == last)
                        n = add_promos(out, n, f64, CS_TO64(t), 1, caps);
                    else
                        out[n++] = CS_MV(f64, CS_TO64(t), CS_F_CAP);
                }
                else if (t == p->ep)
                    out[n++] = CS_MV(f64, CS_TO64(t), CS_F_EP);
            }
            continue;
        }
        if (ty == CS_N || ty == CS_K)
        {
            const int8_t *d = ty == CS_N ? kn_d : kg_d;

            for (i = 0; i < 8; i++)
            {
                t = s + d[i];
                if (t & 0x88)
                    continue;
                if (!b[t])
                {
                    if (!caps)
                        out[n++] = CS_MV(f64, CS_TO64(t), CS_F_QUIET);
                }
                else if (CS_COLOR(b[t]) == them)
                    out[n++] = CS_MV(f64, CS_TO64(t), CS_F_CAP);
            }
            if (ty == CS_K && !caps)
            {
                int r0 = us ? 0x70 : 0x00;

                if ((p->castle & (us ? CS_BK : CS_WK)) && s == r0 + 4
                    && !b[r0 + 5] && !b[r0 + 6]
                    && !cs_attacked(p, r0 + 4, them)
                    && !cs_attacked(p, r0 + 5, them)
                    && !cs_attacked(p, r0 + 6, them))
                    out[n++] = CS_MV(f64, CS_TO64(r0 + 6), CS_F_OO);
                if ((p->castle & (us ? CS_BQ : CS_WQ)) && s == r0 + 4
                    && !b[r0 + 3] && !b[r0 + 2] && !b[r0 + 1]
                    && !cs_attacked(p, r0 + 4, them)
                    && !cs_attacked(p, r0 + 3, them)
                    && !cs_attacked(p, r0 + 2, them))
                    out[n++] = CS_MV(f64, CS_TO64(r0 + 2), CS_F_OOO);
            }
            continue;
        }
        for (i = ty == CS_B ? 4 : 0; i < (ty == CS_R ? 4 : 8); i++)
        {
            int d = sl_d[i];

            for (t = s + d; !(t & 0x88); t += d)
            {
                if (!b[t])
                {
                    if (!caps)
                        out[n++] = CS_MV(f64, CS_TO64(t), CS_F_QUIET);
                    continue;
                }
                if (CS_COLOR(b[t]) == them)
                    out[n++] = CS_MV(f64, CS_TO64(t), CS_F_CAP);
                break;
            }
        }
    }
    return n;
}

/* the rights a move from or to s88 clears */
static uint8_t castle_keep(int s)
{
    switch (s)
    {
        case 0x00: return (uint8_t)~CS_WQ;
        case 0x07: return (uint8_t)~CS_WK;
        case 0x04: return (uint8_t)~(CS_WK | CS_WQ);
        case 0x70: return (uint8_t)~CS_BQ;
        case 0x77: return (uint8_t)~CS_BK;
        case 0x74: return (uint8_t)~(CS_BK | CS_BQ);
        default:   return 0xff;
    }
}

void cs_make(struct cs_pos *p, uint16_t m, struct cs_undo *u)
{
    int f = CS_TO88(CS_FROM(m)), t = CS_TO88(CS_TO(m)), fl = CS_FLAGS(m);
    int us = p->side, pc = p->b[f], cs;

    u->move = m;
    u->castle = p->castle;
    u->ep = p->ep;
    u->phase = p->phase;
    u->half = p->half;
    u->key = p->key;
    u->psq = p->psq;
    if (p->rep)
        p->rep[p->ply & CS_REP_MASK] = p->key;
    p->key ^= base_key(p);              /* side, castling, ep: out */
    cs = fl == CS_F_EP ? t + (us ? 16 : -16) : t;
    u->cap = p->b[cs];
    if (u->cap)
        take(p, cs);
    take(p, f);
    put(p, t, fl & CS_F_PROMO ? CS_PROMO_TYPE(m) | (us ? CS_BLACK : 0) : pc);
    if (fl == CS_F_OO)
    {
        int rk = p->b[t + 1];

        take(p, t + 1);
        put(p, t - 1, rk);
    }
    else if (fl == CS_F_OOO)
    {
        int rk = p->b[t - 2];

        take(p, t - 2);
        put(p, t + 1, rk);
    }
    if (CS_TYPE(pc) == CS_K)
        p->king[us] = (uint8_t)t;
    p->castle &= castle_keep(f) & castle_keep(t);
    p->half = CS_TYPE(pc) == CS_P || u->cap ? 0 : (uint16_t)(p->half + 1);
    p->ep = CS_NOSQ;
    if (fl == CS_F_DOUBLE)
    {
        /* an en passant square only when a pawn can take there: the
         * position (and its hash) is the same as without it otherwise */
        int them = (us ^ 1) ? CS_BLACK : 0;

        if ((!((t - 1) & 0x88) && p->b[t - 1] == (CS_P | them))
            || (!((t + 1) & 0x88) && p->b[t + 1] == (CS_P | them)))
            p->ep = (uint8_t)((f + t) / 2);
    }
    p->side ^= 1;
    p->ply++;
    p->key ^= base_key(p);              /* and back in */
}

void cs_unmake(struct cs_pos *p, const struct cs_undo *u)
{
    uint16_t m = u->move;
    int f = CS_TO88(CS_FROM(m)), t = CS_TO88(CS_TO(m)), fl = CS_FLAGS(m);
    int us = p->side ^ 1, pc = p->b[t];

    p->side = (uint8_t)us;
    p->ply--;
    if (fl & CS_F_PROMO)
    {
        p->cnt[pc]--;
        pc = CS_P | (us ? CS_BLACK : 0);
        p->cnt[pc]++;
    }
    p->b[t] = CS_EMPTY;
    p->b[f] = (uint8_t)pc;
    if (u->cap)
    {
        p->b[fl == CS_F_EP ? t + (us ? 16 : -16) : t] = u->cap;
        p->cnt[u->cap]++;
    }
    if (fl == CS_F_OO)
    {
        p->b[t + 1] = p->b[t - 1];
        p->b[t - 1] = CS_EMPTY;
    }
    else if (fl == CS_F_OOO)
    {
        p->b[t - 2] = p->b[t + 1];
        p->b[t + 1] = CS_EMPTY;
    }
    if (CS_TYPE(pc) == CS_K)
        p->king[us] = (uint8_t)f;
    p->castle = u->castle;
    p->ep = u->ep;
    p->phase = u->phase;
    p->half = u->half;
    p->key = u->key;
    p->psq = u->psq;
}

bool cs_make_legal(struct cs_pos *p, uint16_t m, struct cs_undo *u)
{
    cs_make(p, m, u);
    if (cs_attacked(p, p->king[p->side ^ 1], p->side))
    {
        cs_unmake(p, u);
        return false;
    }
    return true;
}

int cs_legal(struct cs_pos *p, uint16_t *out)
{
    struct cs_undo u;
    int n = cs_gen(p, out, false), i, k = 0;

    for (i = 0; i < n; i++)
        if (cs_make_legal(p, out[i], &u))
        {
            cs_unmake(p, &u);
            out[k++] = out[i];
        }
    return k;
}

bool cs_is_legal(struct cs_pos *p, uint16_t m)
{
    uint16_t mv[CS_MOVES_MAX];
    int n = cs_gen(p, mv, false), i;
    struct cs_undo u;

    for (i = 0; i < n; i++)
        if (mv[i] == m)
        {
            if (!cs_make_legal(p, m, &u))
                return false;
            cs_unmake(p, &u);
            return true;
        }
    return false;
}

int cs_repeats(const struct cs_pos *p)
{
    int i, n = 0;

    if (!p->rep)
        return 0;
    for (i = 4; i <= p->half && i <= p->ply && i < CS_REP_N; i += 2)
        if (p->rep[(p->ply - i) & CS_REP_MASK] == p->key)
            n++;
    return n;
}

bool cs_dead(const struct cs_pos *p)
{
    const uint8_t *c = p->cnt;
    int minors, s, col = -1;

    if (c[CS_P] || c[CS_P | CS_BLACK] || c[CS_R] || c[CS_R | CS_BLACK]
        || c[CS_Q] || c[CS_Q | CS_BLACK])
        return false;
    minors = c[CS_N] + c[CS_N | CS_BLACK] + c[CS_B] + c[CS_B | CS_BLACK];
    if (minors <= 1)
        return true;
    if (c[CS_N] || c[CS_N | CS_BLACK])
        return false;
    /* bishops only: dead when every one stands on one colour */
    for (s = 0; s < 128; s++)
    {
        int sc;

        if (s & 0x88)
        {
            s += 7;
            continue;
        }
        if (CS_TYPE(p->b[s]) != CS_B)
            continue;
        sc = ((s >> 4) + (s & 7)) & 1;
        if (col >= 0 && sc != col)
            return false;
        col = sc;
    }
    return true;
}

enum cs_end cs_end(const struct cs_pos *p, int n_legal)
{
    if (!n_legal)
        return cs_in_check(p) ? CS_MATE : CS_STALEMATE;
    if (cs_dead(p))
        return CS_DRAW_DEAD;
    if (p->half >= 100)
        return CS_DRAW_50;
    if (cs_repeats(p) >= 2)
        return CS_DRAW_REP;
    return CS_PLAYING;
}

uint16_t cs_take_back(uint16_t n, int human, int mode, bool thinking)
{
    uint16_t k = n;

    if (!n)
        return 0;
    if (mode == CS_VS_PLAYER || thinking)
        return n - 1;                   /* one ply */
    /* ply i was the person's when (i & 1) == human */
    if (((k - 1) & 1) != human)
        k--;                            /* the engine's reply */
    if (k && ((k - 1) & 1) == human)
        return k - 1;                   /* and the person's own */
    return n;                           /* none of theirs: keep all */
}
