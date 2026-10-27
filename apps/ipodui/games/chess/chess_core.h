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

/* Chess rules (pure, host-tested in tests/chess): a 0x88 board, the full
 * legal-move generator (castling, en passant, promotion to any of four
 * pieces), make and unmake with an incremental hash and an incremental
 * tapered piece-square score, check, and the game's end: checkmate,
 * stalemate, threefold repetition, the 50-move rule and the positions in
 * which no mate is possible.
 * No SDK or Rockbox headers: the game (chess.c) draws it, the engine
 * (chess_engine.c) searches it. */
#ifndef CHESS_CORE_H
#define CHESS_CORE_H

#include <stdint.h>
#include <stdbool.h>

/* pieces: a type, plus CS_BLACK for black; 0 is an empty square */
enum { CS_EMPTY, CS_P, CS_N, CS_B, CS_R, CS_Q, CS_K };
#define CS_BLACK        8
#define CS_TYPE(p)      ((p) & 7)
#define CS_COLOR(p)     ((p) >> 3)          /* 0 white, 1 black */
#define CS_WHITE_SIDE   0
#define CS_BLACK_SIDE   1

/* squares: 0..63, a1 = 0, h1 = 7, a8 = 56 (file + 8 * rank); the board
 * itself is 0x88 (file + 16 * rank), off the board when s & 0x88 */
#define CS_SQ(f, r)     ((f) + 8 * (r))
#define CS_FILE(s)      ((s) & 7)
#define CS_RANK(s)      ((s) >> 3)
#define CS_TO88(s)      ((s) + ((s) & ~7))
#define CS_TO64(s)      (((s) + ((s) & 7)) >> 1)
#define CS_NOSQ         0x80                /* no en passant square */

/* a move in 16 bits: from (6) | to (6) << 6 | flags (4) << 12 */
enum {
    CS_F_QUIET = 0, CS_F_DOUBLE = 1, CS_F_OO = 2, CS_F_OOO = 3,
    CS_F_CAP = 4, CS_F_EP = 5,
    CS_F_PROMO = 8,             /* + 0..3: knight, bishop, rook, queen */
    CS_F_PROMO_CAP = 12,        /* the same, capturing */
};
#define CS_MV(f, t, fl) ((uint16_t)((f) | ((t) << 6) | ((fl) << 12)))
#define CS_FROM(m)      ((m) & 63)
#define CS_TO(m)        (((m) >> 6) & 63)
#define CS_FLAGS(m)     ((m) >> 12)
#define CS_IS_CAP(m)    (((m) >> 12) & 4)
#define CS_IS_PROMO(m)  (((m) >> 12) & 8)
#define CS_PROMO_TYPE(m) ((((m) >> 12) & 3) + CS_N)
#define CS_NOMOVE       0

/* castling rights */
#define CS_WK 1
#define CS_WQ 2
#define CS_BK 4
#define CS_BQ 8

/* the most moves any position has is 218: room for that */
#define CS_MOVES_MAX    256
/* the hash ring the repetition check reads: a power of two above the
 * 100 plies the 50-move rule allows plus the deepest search line */
#define CS_REP_N        256
#define CS_REP_MASK     (CS_REP_N - 1)

/* A packed score: the middlegame value in the high half, the endgame one
 * in the low half, so one add updates both (CS_S, CS_MG, CS_EG) */
#define CS_S(mg, eg)    ((int32_t)((uint32_t)(int32_t)(mg) << 16) + (int32_t)(eg))
#define CS_MG(s)        ((int16_t)(((s) + 0x8000) >> 16))
#define CS_EG(s)        ((int16_t)((s) & 0xffff))

struct cs_pos {
    uint8_t  b[128];            /* 0x88 */
    uint8_t  side;              /* to move: 0 white, 1 black */
    uint8_t  castle;            /* CS_WK | CS_WQ | CS_BK | CS_BQ */
    uint8_t  ep;                /* the en passant target (0x88) or CS_NOSQ */
    uint8_t  phase;             /* 0..24: knights, bishops 1, rooks 2, queens 4 */
    uint8_t  king[2];           /* 0x88 */
    uint16_t half;              /* plies since a capture or a pawn move */
    uint16_t ply;               /* plies since the start position */
    uint8_t  cnt[16];           /* pieces on the board, by piece code */
    uint16_t rsv;
    uint32_t key;               /* the position's hash */
    int32_t  psq;               /* packed score, white minus black */
    uint32_t *rep;              /* CS_REP_N keys by ply, or NULL */
};

struct cs_undo {
    uint16_t move;
    uint8_t  cap;               /* the piece taken, or 0 */
    uint8_t  castle, ep, phase;
    uint16_t half;
    uint32_t key;
    int32_t  psq;
};

/* the piece-square tables the score uses: [type - 1][square], white's
 * view, packed (CS_S); black reads the mirrored square, negated. NULL
 * (the default): the score stays 0 (perft, the rules tests). */
void cs_set_pst(const int32_t (*pst)[64]);

/* the start position; rep: the hash ring, or NULL */
void cs_start(struct cs_pos *p, uint32_t *rep);
/* a position from FEN (the tests); 0 or -1 */
int  cs_set_fen(struct cs_pos *p, const char *fen, uint32_t *rep);

/* is the 0x88 square s attacked by side `by` */
bool cs_attacked(const struct cs_pos *p, int s, int by);
static inline bool cs_in_check(const struct cs_pos *p)
{
    return cs_attacked(p, p->king[p->side], p->side ^ 1);
}

/* pseudo-legal moves into out (CS_MOVES_MAX); caps: only captures and
 * promotions (the engine's quiescence). Returns the count. */
int  cs_gen(const struct cs_pos *p, uint16_t *out, bool caps);
/* plays m (pseudo-legal) and fills u */
void cs_make(struct cs_pos *p, uint16_t m, struct cs_undo *u);
void cs_unmake(struct cs_pos *p, const struct cs_undo *u);
/* plays m if it leaves the mover's king safe; else undoes it: false */
bool cs_make_legal(struct cs_pos *p, uint16_t m, struct cs_undo *u);
/* the legal moves; returns the count */
int  cs_legal(struct cs_pos *p, uint16_t *out);
/* m in the legal moves of p (a saved game's move, the engine's TT move) */
bool cs_is_legal(struct cs_pos *p, uint16_t m);

/* times the current position occurred before (same side to move, within
 * the 50-move window); needs the hash ring */
int  cs_repeats(const struct cs_pos *p);
/* no sequence of legal moves can end in mate: K v K, K + one minor v K,
 * and kings with bishops all on one colour of square */
bool cs_dead(const struct cs_pos *p);

enum cs_end { CS_PLAYING, CS_MATE, CS_STALEMATE, CS_DRAW_REP, CS_DRAW_50,
              CS_DRAW_DEAD };
/* how the game stands with p->side to move, which has n_legal legal moves
 * (cs_legal()); needs the hash ring for the repetition rule. Checkmate
 * and stalemate first: a mate given by the hundredth ply still counts. */
enum cs_end cs_end(const struct cs_pos *p, int n_legal);

/* -- the game's turns (chess.c; pure, so the tests cover them) -- */
/* who sits across the board: the engine, or a second person passing the
 * device (owner, 2026-10-25); a game keeps its mode to the end */
enum cs_mode { CS_VS_CPU, CS_VS_PLAYER, CS_MODE__N };

/* does a person choose the move now (else the engine)? side: the side to
 * move; human: the side the person plays against the engine */
static inline bool cs_person_moves(int side, int human, int mode)
{
    return mode == CS_VS_PLAYER || side == human;
}

/* MENU's take-back: how many of the n plies played to keep. Against the
 * engine, back to the person's turn with one of their moves undone (the
 * engine's reply too); thinking: the engine is choosing a reply, which
 * is dropped with the move it answers. Two players: one ply. n when
 * there is nothing to take back. */
uint16_t cs_take_back(uint16_t n, int human, int mode, bool thinking);

#endif /* CHESS_CORE_H */
