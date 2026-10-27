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

/* Host test of the Klondike rules (apps/ipodui/games/solitaire/klondike.c) */
#include <stdio.h>
#include <string.h>
#include "klondike.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* card index: suit 0 spades, 1 hearts, 2 diamonds, 3 clubs; rank 1..13 */
#define C(suit, rank) ((uint8_t)((suit) * 13 + (rank) - 1))
#define U(suit, rank) ((uint8_t)(C(suit, rank) | KL_UP))

static void put(struct kl_state *s, int p, uint8_t c)
{
    s->card[p][s->len[p]++] = c;
}

/* An empty valid-version state: tests then place cards by hand */
static void blank(struct kl_state *s, int draw)
{
    memset(s, 0, sizeof(*s));
    s->ver = KL_VER;
    s->draw = (uint8_t)draw;
}

static void test_deal(void)
{
    struct kl_state a, b;
    uint8_t seen[52];
    int p, i, total = 0;

    kl_deal(&a, 12345, 3);
    CHECK(kl_valid(&a));
    CHECK(a.draw == 3 && a.ver == KL_VER && a.moves == 0);
    memset(seen, 0, sizeof(seen));
    for (p = 0; p < KL_PILES; p++)
        for (i = 0; i < a.len[p]; i++)
        {
            CHECK(!seen[KL_IDX(a.card[p][i])]);
            seen[KL_IDX(a.card[p][i])] = 1;
            total++;
        }
    CHECK(total == 52);
    for (p = 0; p < 7; p++)
    {
        CHECK(a.len[KL_TAB + p] == p + 1);
        CHECK(kl_first_up(&a, KL_TAB + p) == p);
        CHECK(kl_top(&a, KL_TAB + p) & KL_UP);
    }
    CHECK(a.len[KL_STOCK] == 24);
    CHECK(a.len[KL_WASTE] == 0);
    for (i = 0; i < 24; i++)
        CHECK(!(a.card[KL_STOCK][i] & KL_UP));
    for (p = KL_FOUND; p < KL_TAB; p++)
        CHECK(a.len[p] == 0);
    kl_deal(&b, 12345, 3);
    CHECK(!memcmp(&a, &b, sizeof(a)));
    kl_deal(&b, 12346, 3);
    CHECK(memcmp(a.card, b.card, sizeof(a.card)) != 0);
    kl_deal(&b, 0, 1);              /* seed 0 still shuffles */
    CHECK(kl_valid(&b) && b.draw == 1);
    kl_deal(&b, 7, 5);              /* anything but 1 is 3 */
    CHECK(b.draw == 3);
}

static void test_draw_cycle(int draw)
{
    struct kl_state s;
    uint8_t stock0[24];
    int steps = 0, n;

    kl_deal(&s, 99, draw);
    memcpy(stock0, s.card[KL_STOCK], 24);
    while (s.len[KL_STOCK])
    {
        int before = s.len[KL_STOCK];
        int want = before < draw ? before : draw;
        int top = s.card[KL_STOCK][before - 1];

        CHECK(kl_draw(&s) == 0);
        CHECK(s.len[KL_STOCK] == before - want);
        CHECK(kl_top(&s, KL_WASTE) == ((s.card[KL_STOCK][before - want] |
                                         KL_UP)));
        /* the stock's top card is the first one turned */
        CHECK(s.card[KL_WASTE][s.len[KL_WASTE] - want] == (top | KL_UP));
        steps++;
    }
    CHECK(steps == (draw == 1 ? 24 : 8));
    CHECK(s.len[KL_WASTE] == 24 && s.passes == 0);
    /* recycle: the waste goes back face down in the original order */
    CHECK(kl_draw(&s) == 0);
    CHECK(s.len[KL_WASTE] == 0 && s.len[KL_STOCK] == 24 && s.passes == 1);
    CHECK(!memcmp(s.card[KL_STOCK], stock0, 24));
    CHECK(kl_valid(&s));
    /* unlimited passes */
    for (n = 0; n < 5 * 25; n++)
        CHECK(kl_draw(&s) == 0);
    CHECK(s.passes >= 5);
    CHECK(kl_valid(&s));

    /* draw 3 with fewer than 3 left */
    if (draw == 3)
    {
        struct kl_state t;

        blank(&t, 3);
        put(&t, KL_STOCK, C(0, 5));
        put(&t, KL_STOCK, C(0, 6));
        CHECK(kl_draw(&t) == 0);
        CHECK(t.len[KL_WASTE] == 2 && t.len[KL_STOCK] == 0);
        CHECK(kl_top(&t, KL_WASTE) == U(0, 5));
    }
    /* both empty */
    {
        struct kl_state t;

        blank(&t, draw);
        CHECK(kl_draw(&t) == -1);
        CHECK(t.n_undo == 0 && t.moves == 0);
    }
}

static void test_moves(void)
{
    struct kl_state s;

    blank(&s, 1);
    put(&s, KL_TAB + 0, U(0, 8));       /* 8 of spades (black) */
    put(&s, KL_TAB + 1, U(1, 7));       /* 7 of hearts (red) */
    put(&s, KL_TAB + 2, U(3, 7));       /* 7 of clubs (black) */
    put(&s, KL_TAB + 3, C(2, 2));       /* face down */
    put(&s, KL_TAB + 3, U(0, 13));      /* king of spades */
    put(&s, KL_WASTE, U(1, 1));         /* ace of hearts */
    put(&s, KL_TAB + 4, U(0, 6));       /* 6 of spades */
    put(&s, KL_TAB + 5, C(3, 3));       /* face down only */
    /* (a hand-built partial deal: not kl_valid, 52 cards are not placed) */

    /* alternating colours, one rank down */
    CHECK(kl_can(&s, KL_TAB + 1, 1, KL_TAB + 0));
    CHECK(!kl_can(&s, KL_TAB + 2, 1, KL_TAB + 0));    /* same colour */
    CHECK(!kl_can(&s, KL_TAB + 0, 1, KL_TAB + 1));    /* rank up */
    CHECK(!kl_can(&s, KL_TAB + 4, 1, KL_TAB + 0));   /* two ranks */
    /* king to empty, nothing else */
    CHECK(kl_can(&s, KL_TAB + 3, 1, KL_TAB + 6));
    CHECK(!kl_can(&s, KL_TAB + 0, 1, KL_TAB + 6));
    /* face down refused */
    CHECK(!kl_can(&s, KL_TAB + 5, 1, KL_TAB + 6));
    CHECK(!kl_can(&s, KL_TAB + 3, 2, KL_TAB + 6));
    /* nothing onto the stock or waste, nothing from the stock */
    CHECK(!kl_can(&s, KL_TAB + 1, 1, KL_WASTE));
    CHECK(!kl_can(&s, KL_TAB + 1, 1, KL_STOCK));
    CHECK(!kl_can(&s, KL_STOCK, 1, KL_TAB + 0));
    CHECK(!kl_can(&s, KL_TAB + 1, 1, KL_TAB + 1));
    CHECK(!kl_can(&s, KL_TAB + 1, 2, KL_TAB + 0));    /* n > len */
    CHECK(!kl_can(&s, KL_TAB + 1, 0, KL_TAB + 0));
    /* ace to a foundation; the found_for helper */
    CHECK(kl_found_for(&s, KL_WASTE) == KL_FOUND);
    CHECK(kl_found_for(&s, KL_TAB + 0) == -1);
    CHECK(kl_move(&s, KL_TAB + 0, 1, KL_FOUND) == -1);   /* illegal */
    CHECK(kl_move(&s, KL_WASTE, 1, KL_FOUND + 2) == 0);
    CHECK(s.len[KL_FOUND + 2] == 1 && s.len[KL_WASTE] == 0);
    CHECK(s.moves == 1);
    /* same suit ascending only */
    {
        struct kl_state t = s;
        t.len[KL_TAB + 6] = 0;
        put(&t, KL_TAB + 6, U(1, 2));   /* 2 of hearts: not placed yet */
        CHECK(kl_can(&t, KL_TAB + 6, 1, KL_FOUND + 2));
        t.card[KL_TAB + 6][0] = U(2, 2);    /* 2 of diamonds */
        CHECK(!kl_can(&t, KL_TAB + 6, 1, KL_FOUND + 2));
        t.card[KL_TAB + 6][0] = U(1, 3);    /* 3 of hearts */
        CHECK(!kl_can(&t, KL_TAB + 6, 1, KL_FOUND + 2));
        CHECK(!kl_can(&t, KL_TAB + 6, 1, KL_FOUND + 1));   /* only aces */
    }

    /* a run: 8s, 7h, 6d moves as one onto a red 9 only from a tableau */
    CHECK(kl_move(&s, KL_TAB + 1, 1, KL_TAB + 0) == 0);
    CHECK(kl_move(&s, KL_TAB + 4, 1, KL_TAB + 0) == 0);
    CHECK(s.len[KL_TAB + 0] == 3);
    /* the run of 3 onto an empty pile: not a king */
    CHECK(!kl_can(&s, KL_TAB + 0, 3, KL_TAB + 1));
    /* the king with its face-down card under it: auto-flip */
    CHECK(kl_move(&s, KL_TAB + 3, 1, KL_TAB + 1) == 0);
    CHECK(s.card[KL_TAB + 3][0] == U(2, 2));
    CHECK(s.undo[(s.undo_at + KL_UNDO_MAX - 1) % KL_UNDO_MAX].flags
          == KL_MV_FLIP);
    /* now a run of more than one from a non-tableau pile: refused */
    {
        struct kl_state t;
        blank(&t, 1);
        put(&t, KL_WASTE, U(1, 5));
        put(&t, KL_WASTE, U(0, 4));
        put(&t, KL_TAB, U(3, 6));
        CHECK(!kl_can(&t, KL_WASTE, 2, KL_TAB));
        CHECK(!kl_can(&t, KL_WASTE, 1, KL_TAB));      /* 4 on 6 */
    }
    /* the run onto a queen: Q(red) K? build a red 9 */
    {
        struct kl_state t = s;
        int p = KL_TAB + 5;
        t.len[p] = 0;
        t.len[KL_STOCK] = 0;
        put(&t, p, U(2, 9));            /* 9 of diamonds */
        CHECK(kl_can(&t, KL_TAB + 0, 3, p));
        CHECK(!kl_can(&t, KL_TAB + 0, 2, p));
        CHECK(kl_move(&t, KL_TAB + 0, 3, p) == 0);
        CHECK(t.len[p] == 4 && t.len[KL_TAB] == 0);
        CHECK(t.card[p][3] == U(0, 6));
    }
}

static bool same_piles(const struct kl_state *a, const struct kl_state *b)
{
    int p;

    if (memcmp(a->len, b->len, sizeof(a->len)) || a->passes != b->passes)
        return false;
    for (p = 0; p < KL_PILES; p++)
        if (memcmp(a->card[p], b->card[p], a->len[p]))
            return false;
    return true;
}

static void test_undo(void)
{
    struct kl_state s, snap[16];
    int k = 0, i, p, q, done = 0, flipped = 0;
    uint32_t seed;

    /* a deal with a tableau move that exposes a face-down card */
    for (seed = 1; seed < 500 && !done; seed++)
    {
        kl_deal(&s, seed, 3);
        for (p = KL_TAB + 1; p < KL_PILES && !done; p++)
            for (q = KL_TAB; q < KL_PILES && !done; q++)
                if (kl_can(&s, p, 1, q))
                    done = 1;
    }
    CHECK(done);
    /* every kind: draws, a recycle, a move with a flip */
    while (s.len[KL_STOCK])
    {
        snap[k++] = s;
        CHECK(kl_draw(&s) == 0);
    }
    snap[k++] = s;
    CHECK(kl_draw(&s) == 0);            /* the recycle */
    CHECK(s.passes == 1);
    snap[k++] = s;
    done = 0;
    for (p = KL_TAB + 1; p < KL_PILES && !done; p++)
        for (q = KL_TAB; q < KL_PILES && !done; q++)
            if (kl_can(&s, p, 1, q))
            {
                CHECK(kl_move(&s, p, 1, q) == 0);
                flipped = s.undo[(s.undo_at + KL_UNDO_MAX - 1)
                                 % KL_UNDO_MAX].flags == KL_MV_FLIP;
                done = 1;
            }
    CHECK(done && flipped);
    while (k > 0)
    {
        uint16_t m = s.moves;

        CHECK(kl_undo(&s));
        CHECK(s.moves == m + 1);
        CHECK(same_piles(&s, &snap[--k]));
    }
    CHECK(!kl_undo(&s));
    CHECK(kl_valid(&s));

    /* the ring keeps the latest KL_UNDO_MAX moves */
    kl_deal(&s, 1, 1);
    for (i = 0; i < KL_UNDO_MAX + 10; i++)
        kl_draw(&s);
    CHECK(s.n_undo == KL_UNDO_MAX);
    for (i = 0; i < KL_UNDO_MAX; i++)
        CHECK(kl_undo(&s));
    CHECK(!kl_undo(&s));
    CHECK(kl_valid(&s));
    /* 58 draws, 48 undone: 10 draws stand (24 cards a pass + recycle) */
    {
        struct kl_state t;
        kl_deal(&t, 1, 1);
        for (i = 0; i < 10; i++)
            kl_draw(&t);
        CHECK(!memcmp(s.len, t.len, sizeof(s.len)));
        for (p = 0; p < KL_PILES; p++)
            CHECK(!memcmp(s.card[p], t.card[p], s.len[p]));
    }
}

static void test_win(void)
{
    struct kl_state s;
    int f, r, steps = 0;

    blank(&s, 3);
    /* foundations up to 10, J/Q/K of each suit spread in the tableau,
     * all face up: one auto-finish away */
    for (f = 0; f < 4; f++)
        for (r = 1; r <= 10; r++)
            put(&s, KL_FOUND + f, U(f, r));
    put(&s, KL_TAB + 0, U(0, 13));
    put(&s, KL_TAB + 0, U(1, 12));
    put(&s, KL_TAB + 0, U(3, 11));
    put(&s, KL_TAB + 1, U(1, 13));
    put(&s, KL_TAB + 1, U(3, 12));
    put(&s, KL_TAB + 1, U(2, 11));
    put(&s, KL_TAB + 2, U(3, 13));
    put(&s, KL_TAB + 2, U(2, 12));
    put(&s, KL_TAB + 2, U(0, 11));
    put(&s, KL_TAB + 3, U(2, 13));
    put(&s, KL_TAB + 3, U(0, 12));
    put(&s, KL_TAB + 3, U(1, 11));
    CHECK(kl_valid(&s));
    CHECK(!kl_won(&s));
    CHECK(kl_solvable_now(&s));
    while (!kl_won(&s))
    {
        int p = kl_finish_step(&s);

        CHECK(p >= KL_TAB);
        if (p < 0 || ++steps > 52)
            break;
    }
    CHECK(kl_won(&s) && s.won);
    CHECK(steps == 12);
    CHECK(!kl_solvable_now(&s));
    CHECK(kl_finish_step(&s) == -1);
    CHECK(kl_valid(&s));

    /* not solvable_now with a face-down card or a non-empty stock */
    blank(&s, 3);
    put(&s, KL_TAB, C(0, 1));
    put(&s, KL_TAB, U(0, 2));
    CHECK(!kl_solvable_now(&s));
    blank(&s, 3);
    put(&s, KL_TAB, U(0, 2));
    put(&s, KL_STOCK, C(0, 1));
    CHECK(!kl_solvable_now(&s));
}

static void test_valid(void)
{
    struct kl_state s, t;

    kl_deal(&s, 77, 1);
    CHECK(kl_valid(&s));
    t = s; t.ver = 9;                       CHECK(!kl_valid(&t));
    t = s; t.draw = 2;                      CHECK(!kl_valid(&t));
    t = s; t.n_undo = KL_UNDO_MAX + 1;      CHECK(!kl_valid(&t));
    t = s; t.undo_at = KL_UNDO_MAX;         CHECK(!kl_valid(&t));
    t = s; t.card[KL_TAB][0] = t.card[KL_TAB + 1][0];
    CHECK(!kl_valid(&t));                   /* a duplicate */
    t = s; t.len[KL_STOCK]--;               CHECK(!kl_valid(&t)); /* 51 */
    t = s; t.len[KL_FOUND] = 14;            CHECK(!kl_valid(&t));
    t = s; t.len[KL_TAB] = 20;              CHECK(!kl_valid(&t));
    t = s; t.card[KL_STOCK][0] = 60;        CHECK(!kl_valid(&t));
}

static void test_send_home(void)
{
    struct kl_state s;
    int f;

    /* an ace from the waste onto an empty foundation */
    blank(&s, 1);
    put(&s, KL_WASTE, U(1, 1));
    f = kl_send_home(&s, KL_WASTE);
    CHECK(f == KL_FOUND);
    CHECK(s.len[KL_WASTE] == 0 && s.len[f] == 1 && s.moves == 1);
    /* the 2 of the same suit follows from a tableau pile, which flips */
    put(&s, KL_TAB + 2, C(3, 9));
    put(&s, KL_TAB + 2, U(1, 2));
    f = kl_send_home(&s, KL_TAB + 2);
    CHECK(f >= KL_FOUND && f < KL_TAB && s.len[f] == 2);
    CHECK(s.card[KL_TAB + 2][0] == U(3, 9));
    /* undo takes it back: the card home on the tableau, face down again */
    CHECK(kl_undo(&s));
    CHECK(s.len[KL_TAB + 2] == 2 && s.card[KL_TAB + 2][0] == C(3, 9));
    CHECK(s.card[KL_TAB + 2][1] == U(1, 2) && s.len[f] == 1);
    /* illegal: a 3 of that suit now, a lone 5, a non-ace on empty */
    blank(&s, 1);
    put(&s, KL_FOUND, U(1, 1));
    put(&s, KL_TAB, U(1, 3));
    put(&s, KL_TAB + 1, U(2, 5));
    put(&s, KL_WASTE, U(0, 2));
    CHECK(kl_send_home(&s, KL_TAB) == -1);
    CHECK(kl_send_home(&s, KL_TAB + 1) == -1);
    CHECK(kl_send_home(&s, KL_WASTE) == -1);
    CHECK(kl_send_home(&s, KL_TAB + 3) == -1);      /* empty pile */
    CHECK(s.moves == 0 && s.n_undo == 0);
    /* not from the stock or a foundation */
    put(&s, KL_STOCK, C(0, 1));
    CHECK(kl_send_home(&s, KL_STOCK) == -1);
    CHECK(kl_send_home(&s, KL_FOUND) == -1);
    /* a face-down top card never goes */
    blank(&s, 1);
    put(&s, KL_TAB, C(0, 1));
    CHECK(kl_send_home(&s, KL_TAB) == -1);
}

int main(void)
{
    test_send_home();
    test_deal();
    test_draw_cycle(1);
    test_draw_cycle(3);
    test_moves();
    test_undo();
    test_win();
    test_valid();
    printf("klondike: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
