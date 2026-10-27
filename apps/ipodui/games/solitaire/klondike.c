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

/* Klondike rules (klondike.h) */
#include <string.h>
#include "klondike.h"

static uint32_t xs(uint32_t *st)
{
    uint32_t x = *st ? *st : 0x9e3779b9u;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *st = x;
    return x;
}

void kl_deal(struct kl_state *s, uint32_t seed, int draw)
{
    uint8_t deck[52];
    uint32_t r = seed;
    int i, p, k = 0;

    memset(s, 0, sizeof(*s));
    s->ver = KL_VER;
    s->draw = draw == 1 ? 1 : 3;
    s->seed = seed;
    for (i = 0; i < 52; i++)
        deck[i] = (uint8_t)i;
    for (i = 51; i > 0; i--)
    {
        int j = (int)(xs(&r) % (uint32_t)(i + 1));
        uint8_t t = deck[i];

        deck[i] = deck[j];
        deck[j] = t;
    }
    /* pile i gets i + 1 cards, the last face up */
    for (p = 0; p < 7; p++)
        for (i = 0; i <= p; i++)
        {
            uint8_t c = deck[k++];

            s->card[KL_TAB + p][i] = i == p ? (uint8_t)(c | KL_UP) : c;
            s->len[KL_TAB + p]++;
        }
    while (k < 52)
        s->card[KL_STOCK][s->len[KL_STOCK]++] = deck[k++];
}

bool kl_valid(const struct kl_state *s)
{
    uint8_t seen[52];
    int p, i, total = 0;

    if (s->ver != KL_VER || (s->draw != 1 && s->draw != 3)
        || s->n_undo > KL_UNDO_MAX || s->undo_at >= KL_UNDO_MAX)
        return false;
    memset(seen, 0, sizeof(seen));
    for (p = 0; p < KL_PILES; p++)
    {
        int max = p >= KL_FOUND && p < KL_TAB ? 13
                : p >= KL_TAB ? 19 : KL_PILE_MAX;

        if (s->len[p] > max)
            return false;
        for (i = 0; i < s->len[p]; i++)
        {
            int c = KL_IDX(s->card[p][i]);

            if (c >= 52 || seen[c]++)
                return false;
            total++;
        }
    }
    return total == 52;
}

int kl_top(const struct kl_state *s, int p)
{
    return s->len[p] ? s->card[p][s->len[p] - 1] : -1;
}

int kl_first_up(const struct kl_state *s, int p)
{
    int i;

    for (i = 0; i < s->len[p]; i++)
        if (s->card[p][i] & KL_UP)
            return i;
    return s->len[p];
}

static void push_undo(struct kl_state *s, int from, int to, int n, int fl)
{
    struct kl_move *m = &s->undo[s->undo_at];

    m->from = (uint8_t)from;
    m->to = (uint8_t)to;
    m->n = (uint8_t)n;
    m->flags = (uint8_t)fl;
    s->undo_at = (uint8_t)((s->undo_at + 1) % KL_UNDO_MAX);
    if (s->n_undo < KL_UNDO_MAX)
        s->n_undo++;
}

int kl_draw(struct kl_state *s)
{
    int n, i;

    if (s->len[KL_STOCK] == 0)
    {
        if (s->len[KL_WASTE] == 0)
            return -1;
        /* the waste turned over as the new stock, face down */
        n = s->len[KL_WASTE];
        for (i = 0; i < n; i++)
            s->card[KL_STOCK][i] =
                (uint8_t)KL_IDX(s->card[KL_WASTE][n - 1 - i]);
        s->len[KL_STOCK] = (uint8_t)n;
        s->len[KL_WASTE] = 0;
        s->passes++;
        push_undo(s, KL_WASTE, KL_STOCK, n, KL_MV_RECYCLE);
        s->moves++;
        return 0;
    }
    n = s->len[KL_STOCK] < s->draw ? s->len[KL_STOCK] : s->draw;
    for (i = 0; i < n; i++)
    {
        uint8_t c = s->card[KL_STOCK][--s->len[KL_STOCK]];

        s->card[KL_WASTE][s->len[KL_WASTE]++] = (uint8_t)(c | KL_UP);
    }
    push_undo(s, KL_STOCK, KL_WASTE, n, KL_MV_DRAW);
    s->moves++;
    return 0;
}

static bool is_found(int p) { return p >= KL_FOUND && p < KL_TAB; }
static bool is_tab(int p)   { return p >= KL_TAB && p < KL_PILES; }

bool kl_can(const struct kl_state *s, int from, int n, int to)
{
    int c, t;

    if (from == to || from <= KL_STOCK || from >= KL_PILES
        || to < KL_FOUND || to >= KL_PILES || n < 1 || n > s->len[from])
        return false;
    if (n > 1 && !is_tab(from))
        return false;
    c = s->card[from][s->len[from] - n];
    if (!(c & KL_UP))
        return false;
    t = kl_top(s, to);
    if (is_found(to))
    {
        if (n != 1)
            return false;
        if (t < 0)
            return KL_RANK(c) == 1;
        return KL_SUIT(c) == KL_SUIT(t) && KL_RANK(c) == KL_RANK(t) + 1;
    }
    if (t < 0)
        return KL_RANK(c) == 13;        /* only a king on an empty pile */
    return (t & KL_UP) && KL_RED(c) != KL_RED(t)
           && KL_RANK(c) + 1 == KL_RANK(t);
}

int kl_move(struct kl_state *s, int from, int n, int to)
{
    int i, fl = 0;

    if (!kl_can(s, from, n, to))
        return -1;
    for (i = 0; i < n; i++)
        s->card[to][s->len[to] + i] = s->card[from][s->len[from] - n + i];
    s->len[to] = (uint8_t)(s->len[to] + n);
    s->len[from] = (uint8_t)(s->len[from] - n);
    if (is_tab(from) && s->len[from]
        && !(s->card[from][s->len[from] - 1] & KL_UP))
    {
        s->card[from][s->len[from] - 1] |= KL_UP;
        fl = KL_MV_FLIP;
    }
    push_undo(s, from, to, n, fl);
    s->moves++;
    s->won = kl_won(s);
    return 0;
}

int kl_found_for(const struct kl_state *s, int from)
{
    int f;

    for (f = KL_FOUND; f < KL_TAB; f++)
        if (kl_can(s, from, 1, f))
            return f;
    return -1;
}

int kl_send_home(struct kl_state *s, int from)
{
    int f;

    if (from != KL_WASTE && !is_tab(from))
        return -1;
    f = kl_found_for(s, from);
    if (f < 0 || kl_move(s, from, 1, f) != 0)
        return -1;
    return f;
}

bool kl_undo(struct kl_state *s)
{
    struct kl_move *m;
    int i, n;

    if (!s->n_undo)
        return false;
    s->undo_at = (uint8_t)((s->undo_at + KL_UNDO_MAX - 1) % KL_UNDO_MAX);
    s->n_undo--;
    m = &s->undo[s->undo_at];
    n = m->n;
    if (m->flags & KL_MV_DRAW)
    {
        for (i = 0; i < n; i++)
        {
            uint8_t c = s->card[KL_WASTE][--s->len[KL_WASTE]];

            s->card[KL_STOCK][s->len[KL_STOCK]++] = (uint8_t)KL_IDX(c);
        }
    }
    else if (m->flags & KL_MV_RECYCLE)
    {
        for (i = 0; i < n; i++)
            s->card[KL_WASTE][i] =
                (uint8_t)(s->card[KL_STOCK][n - 1 - i] | KL_UP);
        s->len[KL_WASTE] = (uint8_t)n;
        s->len[KL_STOCK] = 0;
        if (s->passes)
            s->passes--;
    }
    else
    {
        if (m->flags & KL_MV_FLIP)
            s->card[m->from][s->len[m->from] - 1] &= (uint8_t)~KL_UP;
        for (i = 0; i < n; i++)
            s->card[m->from][s->len[m->from] + i] =
                s->card[m->to][s->len[m->to] - n + i];
        s->len[m->from] = (uint8_t)(s->len[m->from] + n);
        s->len[m->to] = (uint8_t)(s->len[m->to] - n);
    }
    s->moves++;                         /* an undo counts, as classic */
    s->won = 0;
    return true;
}

bool kl_won(const struct kl_state *s)
{
    int f;

    for (f = KL_FOUND; f < KL_TAB; f++)
        if (s->len[f] != 13)
            return false;
    return true;
}

bool kl_solvable_now(const struct kl_state *s)
{
    int p;

    if (s->len[KL_STOCK] || s->len[KL_WASTE])
        return false;
    for (p = KL_TAB; p < KL_PILES; p++)
        if (kl_first_up(s, p) != 0)
            return false;
    return !kl_won(s);
}

int kl_finish_step(struct kl_state *s)
{
    int p, best = -1, best_rank = 99;

    for (p = KL_WASTE; p < KL_PILES; p++)
    {
        int t, f;

        if (is_found(p) || (t = kl_top(s, p)) < 0)
            continue;
        f = kl_found_for(s, p);
        if (f >= 0 && KL_RANK(t) < best_rank)
        {
            best = p;
            best_rank = KL_RANK(t);
        }
    }
    if (best >= 0)
        kl_move(s, best, 1, kl_found_for(s, best));
    return best;
}
