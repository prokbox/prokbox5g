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

/* Host test of the Chess rules, engine and wheel pad (#132):
 * apps/ipodui/games/chess/chess_{core,engine,pad}.c.
 *  - perft: legal move counts on six standard positions (start, the
 *    castling/en passant/promotion test positions) against the published
 *    values;
 *  - the rules' edge cases: castling through or out of check, a pinned en
 *    passant capture, the four promotions, checkmate, stalemate,
 *    threefold repetition, the 50-move rule, positions without mate;
 *  - the hash: make/unmake restore it, and it matches a fresh FEN;
 *  - the engine: a legal move from every test position, a mate in one,
 *    a free queen taken, the same move however its search is sliced;
 *  - the pad: taps, holds, slides, presses;
 *  - the turns: two players (no engine, alternate sides, a take-back of
 *    one ply, the mate names the winning colour) and the take-back
 *    against the engine. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "chess_core.h"
#include "chess_engine.h"
#include "chess_pad.h"

static int checks, fails;
#define CHECK(x) do { checks++; if (!(x)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static uint32_t rep[CS_REP_N];

static uint64_t perft(struct cs_pos *p, int depth)
{
    uint16_t mv[CS_MOVES_MAX];
    struct cs_undo u;
    uint64_t n = 0;
    int i, k = cs_gen(p, mv, false);

    for (i = 0; i < k; i++)
    {
        if (!cs_make_legal(p, mv[i], &u))
            continue;
        n += depth > 1 ? perft(p, depth - 1) : 1;
        cs_unmake(p, &u);
    }
    return n;
}

static const struct {
    const char *fen;
    int depth;
    uint64_t want[5];
} perfts[] = {
    { "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5,
      { 20, 400, 8902, 197281, 4865609 } },
    { "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
      4, { 48, 2039, 97862, 4085603 } },
    { "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5,
      { 14, 191, 2812, 43238, 674624 } },
    { "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4,
      { 6, 264, 9467, 422333 } },
    { "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3,
      { 44, 1486, 62379 } },
    { "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
      3, { 46, 2079, 89890 } },
};

static void test_perft(void)
{
    struct cs_pos p;
    unsigned i;
    int d;

    for (i = 0; i < sizeof(perfts) / sizeof(perfts[0]); i++)
    {
        CHECK(cs_set_fen(&p, perfts[i].fen, NULL) == 0);
        for (d = 1; d <= perfts[i].depth; d++)
        {
            uint64_t n = perft(&p, d);

            if (n != perfts[i].want[d - 1])
                printf("perft %u depth %d: %llu, want %llu\n", i, d,
                       (unsigned long long)n,
                       (unsigned long long)perfts[i].want[d - 1]);
            CHECK(n == perfts[i].want[d - 1]);
        }
    }
    /* cs_start() is the same position as its FEN */
    {
        struct cs_pos a, b;

        cs_start(&a, NULL);
        cs_set_fen(&b, perfts[0].fen, NULL);
        CHECK(a.key == b.key);
        CHECK(!memcmp(a.b, b.b, sizeof(a.b)));
        CHECK(a.castle == b.castle && a.phase == 24 && b.phase == 24);
    }
}

static uint16_t mv_of(const char *s)
{
    int f = CS_SQ(s[0] - 'a', s[1] - '1'), t = CS_SQ(s[2] - 'a', s[3] - '1');
    return (uint16_t)(f | t << 6);
}

/* the legal move from..to (+ promotion letter), or 0 */
static uint16_t find(struct cs_pos *p, const char *s)
{
    uint16_t mv[CS_MOVES_MAX], want = mv_of(s);
    int n = cs_legal(p, mv), i;

    for (i = 0; i < n; i++)
    {
        if ((mv[i] & 0xfff) != want)
            continue;
        if (!CS_IS_PROMO(mv[i]))
            return mv[i];
        if (s[4] && "nbrq"[CS_PROMO_TYPE(mv[i]) - CS_N] == s[4])
            return mv[i];
    }
    return 0;
}

static bool play(struct cs_pos *p, const char *s)
{
    struct cs_undo u;
    uint16_t m = find(p, s);

    if (!m)
    {
        printf("no legal move %s\n", s);
        return false;
    }
    cs_make(p, m, &u);
    return true;
}

static int legal_n(struct cs_pos *p)
{
    uint16_t mv[CS_MOVES_MAX];
    return cs_legal(p, mv);
}

static void test_rules(void)
{
    struct cs_pos p;
    uint16_t mv[CS_MOVES_MAX];
    int n, i, k;

    /* castling: not out of check, not through an attacked square, not
     * into one; both sides when free */
    cs_set_fen(&p, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", NULL);
    CHECK(find(&p, "e1g1") && find(&p, "e1c1"));
    cs_set_fen(&p, "r3k2r/8/8/8/8/8/8/R3K1rR w KQkq - 0 1", NULL);
    CHECK(!find(&p, "e1g1"));           /* the king is in check */
    cs_set_fen(&p, "r3k2r/8/8/8/8/8/5r2/R3K2R w KQkq - 0 1", NULL);
    CHECK(!find(&p, "e1g1"));           /* f1 attacked */
    CHECK(find(&p, "e1c1"));
    cs_set_fen(&p, "r3k2r/8/8/8/8/8/3r4/R3K2R w KQkq - 0 1", NULL);
    CHECK(!find(&p, "e1c1") && find(&p, "e1g1"));   /* d1 attacked */
    cs_set_fen(&p, "r3k2r/8/8/8/8/8/1r6/R3K2R w KQkq - 0 1", NULL);
    CHECK(find(&p, "e1c1"));            /* b1 attacked: allowed */
    cs_set_fen(&p, "r3k2r/8/8/8/8/8/8/RN2K2R w KQkq - 0 1", NULL);
    CHECK(!find(&p, "e1c1"));           /* b1 occupied */
    /* a rook taken on its square takes the right with it */
    cs_set_fen(&p, "r3k2r/8/8/8/8/8/6b1/R3K2R b KQkq - 0 1", NULL);
    CHECK(play(&p, "g2h1"));
    CHECK(!(p.castle & CS_WK) && (p.castle & CS_WQ));
    /* a rook that moves loses its side's right; the king both */
    cs_set_fen(&p, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", NULL);
    CHECK(play(&p, "a1a2") && p.castle == (CS_WK | CS_BK | CS_BQ));
    CHECK(play(&p, "e8e7") && p.castle == CS_WK);

    /* en passant: allowed, removes the pawn; not when it exposes the king */
    cs_set_fen(&p, "4k3/8/8/8/3p4/8/4P3/4K3 w - - 0 1", NULL);
    CHECK(play(&p, "e2e4"));
    CHECK(p.ep == 0x24);
    CHECK(play(&p, "d4e3"));
    CHECK(p.b[0x34] == CS_EMPTY && p.b[0x24] == (CS_P | CS_BLACK));
    CHECK(p.cnt[CS_P] == 0);
    cs_set_fen(&p, "8/8/8/K2pP2q/8/8/8/7k w - d6 0 1", NULL);
    CHECK(!find(&p, "e5d6"));           /* the rank would open */
    /* no capturing pawn: no en passant square (the hash stays the same) */
    cs_set_fen(&p, "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1", NULL);
    CHECK(play(&p, "e2e4") && p.ep == CS_NOSQ);

    /* promotion: four choices, each one a different piece */
    cs_set_fen(&p, "8/P6k/8/8/8/8/8/K7 w - - 0 1", NULL);
    n = cs_legal(&p, mv);
    for (i = k = 0; i < n; i++)
        k += CS_IS_PROMO(mv[i]) != 0;
    CHECK(k == 4);
    CHECK(play(&p, "a7a8n") && p.b[0x70] == CS_N && p.cnt[CS_P] == 0);
    cs_set_fen(&p, "1r5k/P7/8/8/8/8/8/K7 w - - 0 1", NULL);
    CHECK(play(&p, "a7b8q") && p.b[0x71] == CS_Q && p.cnt[CS_R | CS_BLACK] == 0);

    /* the end of the game */
    cs_start(&p, rep);                  /* the fool's mate */
    CHECK(play(&p, "f2f3") && play(&p, "e7e5") && play(&p, "g2g4"));
    CHECK(cs_end(&p, legal_n(&p)) == CS_PLAYING);
    CHECK(play(&p, "d8h4"));
    CHECK(cs_in_check(&p) && cs_end(&p, legal_n(&p)) == CS_MATE);
    cs_set_fen(&p, "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", NULL);
    CHECK(legal_n(&p) == 0 && cs_end(&p, legal_n(&p)) == CS_STALEMATE);
    cs_set_fen(&p, "7k/8/6K1/8/8/8/8/R7 w - - 99 80", NULL);
    CHECK(cs_end(&p, legal_n(&p)) == CS_PLAYING);
    CHECK(play(&p, "a1b1") && cs_end(&p, legal_n(&p)) == CS_DRAW_50);
    cs_set_fen(&p, "6k1/8/6K1/8/8/8/8/R7 w - - 99 80", NULL);
    CHECK(play(&p, "a1a8") && cs_end(&p, legal_n(&p)) == CS_MATE);   /* mate wins */
    cs_start(&p, rep);                  /* knights out and back, twice */
    for (i = 0; i < 2; i++)
    {
        CHECK(cs_end(&p, legal_n(&p)) == CS_PLAYING);
        CHECK(play(&p, "g1f3") && play(&p, "g8f6"));
        CHECK(play(&p, "f3g1") && play(&p, "f6g8"));
    }
    CHECK(cs_repeats(&p) == 2 && cs_end(&p, legal_n(&p)) == CS_DRAW_REP);
    /* the dead positions */
    cs_set_fen(&p, "8/8/4k3/8/8/3K4/8/8 w - - 0 1", NULL);
    CHECK(cs_end(&p, legal_n(&p)) == CS_DRAW_DEAD);
    cs_set_fen(&p, "8/8/4k3/8/8/3KB3/8/8 w - - 0 1", NULL);
    CHECK(cs_end(&p, legal_n(&p)) == CS_DRAW_DEAD);
    cs_set_fen(&p, "8/8/4k3/8/8/3KN3/8/8 w - - 0 1", NULL);
    CHECK(cs_end(&p, legal_n(&p)) == CS_DRAW_DEAD);
    cs_set_fen(&p, "8/8/4kb2/8/8/3KB3/8/8 w - - 0 1", NULL);  /* e3, f6 */
    CHECK(cs_end(&p, legal_n(&p)) == CS_DRAW_DEAD);  /* both on dark squares */
    cs_set_fen(&p, "8/8/4k1b1/8/8/3KB3/8/8 w - - 0 1", NULL); /* e3, g6 */
    CHECK(cs_end(&p, legal_n(&p)) == CS_PLAYING);    /* opposite colours: a mate exists */
    cs_set_fen(&p, "8/8/4k3/8/8/2NKN3/8/8 w - - 0 1", NULL);
    CHECK(cs_end(&p, legal_n(&p)) == CS_PLAYING);
    cs_set_fen(&p, "8/8/4k3/8/8/3K4/7P/8 w - - 0 1", NULL);
    CHECK(cs_end(&p, legal_n(&p)) == CS_PLAYING);
}

/* make/unmake restore everything; the hash of a line equals a fresh one */
static void test_hash(void)
{
    struct cs_pos p, q, s;
    struct cs_undo u;
    uint16_t mv[CS_MOVES_MAX];
    int32_t pst[6][64];
    int n, i;

    ce_build_pst(pst);
    cs_set_pst((const int32_t (*)[64])pst);
    cs_set_fen(&p, perfts[1].fen, NULL);
    s = p;
    n = cs_gen(&p, mv, false);
    for (i = 0; i < n; i++)
    {
        cs_make(&p, mv[i], &u);
        cs_unmake(&p, &u);
        CHECK(!memcmp(&p, &s, sizeof(p)));
    }
    cs_start(&p, NULL);
    CHECK(play(&p, "e2e4") && play(&p, "c7c5") && play(&p, "g1f3"));
    CHECK(play(&p, "d7d6") && play(&p, "e1e2"));
    cs_set_fen(&q, "rnbqkbnr/pp2pppp/3p4/2p5/4P3/5N2/PPPPKPPP/RNBQ1B1R b kq - 1 3",
               NULL);
    CHECK(p.key == q.key && p.psq == q.psq && p.phase == q.phase);
    /* the score is symmetric: the start position's score is 0 */
    cs_start(&p, NULL);
    CHECK(p.psq == 0);
    CHECK(ce_eval(&p) == 8);            /* the side to move's tempo */
    cs_set_pst(NULL);
}

static struct ce_tt tt[1 << 12];
static struct ce eng;

static uint16_t think(const struct cs_pos *p, int depth, uint32_t chunk)
{
    ce_init(&eng, tt, 12);
    ce_start(&eng, p, depth, 0, 1);
    while (ce_run(&eng, chunk) == CE_RUN)
        ;
    return ce_stop(&eng);
}

static void test_engine(void)
{
    struct cs_pos p;
    int32_t pst[6][64];
    unsigned i;
    uint16_t m, m2;

    ce_build_pst(pst);
    cs_set_pst((const int32_t (*)[64])pst);
    for (i = 0; i < sizeof(perfts) / sizeof(perfts[0]); i++)
    {
        cs_set_fen(&p, perfts[i].fen, rep);
        m = think(&p, 4, 100000);
        CHECK(m && cs_is_legal(&p, m));
        /* the search sliced into single nodes: the same move */
        m2 = think(&p, 4, 1);
        CHECK(m2 == m);
    }
    /* a mate in one, found at once */
    cs_set_fen(&p, "6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1", rep);
    m = think(&p, 3, 100000);
    CHECK(m == find(&p, "a1a8"));
    CHECK(eng.score > CE_MATE_MAX);
    /* and the defence: black, mated in one, stops it */
    cs_set_fen(&p, "6k1/5ppp/8/8/8/8/5PPP/R5K1 b - - 0 1", rep);
    m = think(&p, 3, 100000);
    {
        struct cs_undo u, u2;
        uint16_t r;

        CHECK(m && cs_is_legal(&p, m));
        cs_make(&p, m, &u);
        r = find(&p, "a1a8");
        if (r)
        {
            cs_make(&p, r, &u2);
            CHECK(cs_end(&p, legal_n(&p)) != CS_MATE);
            cs_unmake(&p, &u2);
        }
        cs_unmake(&p, &u);
    }
    /* a free queen is taken */
    cs_set_fen(&p, "4k3/8/8/3q4/4P3/8/8/4K3 w - - 0 1", rep);
    CHECK(think(&p, 2, 100000) == find(&p, "e4d5"));
    /* no move to make: none */
    cs_set_fen(&p, "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1", rep);
    CHECK(think(&p, 3, 100000) == CS_NOMOVE);
    /* stopped early, it still plays a legal move */
    cs_start(&p, rep);
    ce_init(&eng, tt, 12);
    ce_start(&eng, &p, 6, 0, 1);
    ce_run(&eng, 50);
    m = ce_stop(&eng);
    CHECK(m && cs_is_legal(&p, m));
    /* the noisy level plays legal moves */
    for (i = 0; i < 8; i++)
    {
        ce_init(&eng, tt, 12);
        ce_start(&eng, &p, 2, 70, i * 7919u);
        while (ce_run(&eng, 1000) == CE_RUN)
            ;
        m = ce_stop(&eng);
        CHECK(m && cs_is_legal(&p, m));
    }
    cs_set_pst(NULL);
}

static void test_pad(void)
{
    struct cp_pad p;

    CHECK(cp_region(0) == CP_UP && cp_region(95) == CP_UP);
    CHECK(cp_region(11) == CP_UP && cp_region(12) == CP_RIGHT);
    CHECK(cp_region(24) == CP_RIGHT && cp_region(48) == CP_DOWN);
    CHECK(cp_region(72) == CP_LEFT && cp_region(84) == CP_UP);
    cp_reset(&p);
    /* a tap on the right: one step at the lift */
    CHECK(cp_feed(&p, 24, false, 1000) == CP_NONE);
    CHECK(cp_active(&p));
    CHECK(cp_feed(&p, 25, false, 1100) == CP_NONE);
    CHECK(cp_feed(&p, -1, false, 1150) == CP_RIGHT);
    CHECK(!cp_active(&p));
    /* too long for a tap, released before the hold: nothing */
    CHECK(cp_feed(&p, 48, false, 2000) == CP_NONE);
    CHECK(cp_feed(&p, -1, false, 2420) == CP_NONE);
    /* a hold at the left: a step at 450 ms, then every 160 ms */
    CHECK(cp_feed(&p, 72, false, 3000) == CP_NONE);
    CHECK(cp_feed(&p, 72, false, 3449) == CP_NONE);
    CHECK(cp_feed(&p, 73, false, 3450) == CP_LEFT);
    CHECK(cp_feed(&p, 73, false, 3600) == CP_NONE);
    CHECK(cp_feed(&p, 73, false, 3610) == CP_LEFT);
    CHECK(cp_feed(&p, 73, false, 3770) == CP_LEFT);
    CHECK(cp_feed(&p, -1, false, 3800) == CP_NONE);  /* no tap at the end */
    /* a slide (the wheel turning) never steps */
    CHECK(cp_feed(&p, 94, false, 4000) == CP_NONE);
    CHECK(cp_feed(&p, 4, false, 4050) == CP_NONE);  /* 6 across the top */
    CHECK(cp_feed(&p, 4, false, 4600) == CP_NONE);
    CHECK(cp_feed(&p, -1, false, 4650) == CP_NONE);
    /* a drift within the slop, across the wrap, still taps */
    CHECK(cp_feed(&p, 94, false, 5000) == CP_NONE);
    CHECK(cp_feed(&p, 1, false, 5050) == CP_NONE);
    CHECK(cp_feed(&p, -1, false, 5100) == CP_UP);
    /* a press of the button under the finger: not a tap */
    CHECK(cp_feed(&p, 0, false, 6000) == CP_NONE);
    CHECK(cp_feed(&p, 0, true, 6040) == CP_NONE);
    CHECK(cp_feed(&p, -1, false, 6100) == CP_NONE);
    /* ... nor a hold */
    CHECK(cp_feed(&p, 48, true, 7000) == CP_NONE);
    CHECK(cp_feed(&p, 48, false, 7500) == CP_NONE);
    CHECK(cp_feed(&p, -1, false, 7600) == CP_NONE);
    /* a press seen only at the lift voids it too */
    CHECK(cp_feed(&p, 24, false, 8000) == CP_NONE);
    CHECK(cp_feed(&p, -1, true, 8100) == CP_NONE);
}

/* a game as chess.c runs it: the moves played, replayed for a take-back */
struct tp_game {
    struct cs_pos p;
    uint16_t mv[64];
    uint16_t n;
    int human, mode, engine_starts;
};

/* the turn's start: chess.c starts the engine when no person moves */
static void tp_turn(struct tp_game *g)
{
    if (!cs_person_moves(g->p.side, g->human, g->mode))
        g->engine_starts++;
}

/* the person's move from square f to square t: SELECT, SELECT */
static bool tp_play(struct tp_game *g, int f, int t)
{
    uint16_t mv[CS_MOVES_MAX];
    struct cs_undo u;
    int i, k = cs_legal(&g->p, mv);

    if (!cs_person_moves(g->p.side, g->human, g->mode))
        return false;
    for (i = 0; i < k; i++)
        if (CS_FROM(mv[i]) == f && CS_TO(mv[i]) == t)
        {
            cs_make(&g->p, mv[i], &u);
            g->mv[g->n++] = mv[i];
            tp_turn(g);
            return true;
        }
    return false;
}

/* MENU tap: back to the kept plies, replayed from the start */
static void tp_back(struct tp_game *g)
{
    struct cs_undo u;
    uint16_t keep = cs_take_back(g->n, g->human, g->mode, false), i;

    cs_start(&g->p, rep);
    for (i = 0; i < keep; i++)
        CHECK(cs_make_legal(&g->p, g->mv[i], &u));
    g->n = keep;
    tp_turn(g);
}

#define SQN(f, r) CS_SQ((f) - 'a', (r) - 1)

static void test_turns(void)
{
    struct tp_game g;
    uint16_t mv[CS_MOVES_MAX];
    int k;

    /* two players: every turn is a person's, White then Black */
    memset(&g, 0, sizeof(g));
    g.mode = CS_VS_PLAYER;
    cs_start(&g.p, rep);
    tp_turn(&g);
    CHECK(g.p.side == CS_WHITE_SIDE);
    CHECK(tp_play(&g, SQN('e', 2), SQN('e', 4)));
    CHECK(g.p.side == CS_BLACK_SIDE);
    CHECK(!tp_play(&g, SQN('d', 2), SQN('d', 4)));  /* not White's turn */
    CHECK(tp_play(&g, SQN('e', 7), SQN('e', 5)));
    CHECK(g.p.side == CS_WHITE_SIDE);
    CHECK(tp_play(&g, SQN('g', 1), SQN('f', 3)));
    CHECK(g.n == 3 && g.p.side == CS_BLACK_SIDE);
    /* a take-back: one ply, Black's... */
    tp_back(&g);
    CHECK(g.n == 2 && g.p.side == CS_WHITE_SIDE);
    CHECK(g.p.b[CS_TO88(SQN('g', 1))] == CS_N);
    /* ...then White's own, and to the start */
    tp_back(&g);
    CHECK(g.n == 1 && g.p.side == CS_BLACK_SIDE);
    tp_back(&g);
    CHECK(g.n == 0 && g.p.side == CS_WHITE_SIDE);
    tp_back(&g);                        /* nothing left: stays */
    CHECK(g.n == 0);
    /* the shortest mate: f3 e5 g4 Qh4, Black's win */
    CHECK(tp_play(&g, SQN('f', 2), SQN('f', 3)));
    CHECK(tp_play(&g, SQN('e', 7), SQN('e', 5)));
    CHECK(tp_play(&g, SQN('g', 2), SQN('g', 4)));
    CHECK(tp_play(&g, SQN('d', 8), SQN('h', 4)));
    k = cs_legal(&g.p, mv);
    CHECK(cs_end(&g.p, k) == CS_MATE);
    CHECK(g.p.side == CS_WHITE_SIDE);   /* mated: the panel says Black wins */
    CHECK(g.engine_starts == 0);        /* the engine never ran */

    /* against the engine: a take-back undoes the reply and your move */
    CHECK(cs_take_back(4, 0, CS_VS_CPU, false) == 2);
    CHECK(cs_take_back(3, 0, CS_VS_CPU, false) == 2);   /* game over on yours */
    CHECK(cs_take_back(1, 0, CS_VS_CPU, false) == 0);
    CHECK(cs_take_back(1, 1, CS_VS_CPU, false) == 1);   /* only its move */
    CHECK(cs_take_back(3, 1, CS_VS_CPU, false) == 1);
    CHECK(cs_take_back(2, 1, CS_VS_CPU, false) == 1);
    CHECK(cs_take_back(3, 0, CS_VS_CPU, true) == 2);    /* stop thinking */
    CHECK(cs_take_back(0, 0, CS_VS_CPU, false) == 0);
    CHECK(cs_take_back(5, 1, CS_VS_PLAYER, false) == 4);
    /* and it starts thinking on its turns only */
    memset(&g, 0, sizeof(g));
    g.mode = CS_VS_CPU;
    g.human = CS_BLACK_SIDE;
    cs_start(&g.p, rep);
    tp_turn(&g);
    CHECK(g.engine_starts == 1);
    CHECK(!tp_play(&g, SQN('e', 2), SQN('e', 4)));  /* the engine's turn */
}

int main(void)
{
    test_perft();
    test_rules();
    test_hash();
    test_engine();
    test_pad();
    test_turns();
    printf("chess: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
