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

/* The chess engine (pure, host-tested in tests/chess): alpha-beta with
 * iterative deepening, a small transposition table, move ordering (the
 * table's move, captures by most valuable victim and least valuable
 * attacker, two killers a ply), a check extension and a captures-only
 * quiescence, over a tapered evaluation (our own piece-square tables,
 * middlegame and endgame blended by the material left).
 *
 * The search is resumable: its stack is explicit (a frame a ply, in the
 * caller's memory), so the game runs it a few milliseconds a frame and
 * the UI thread goes back to the scheduler in between
 * (docs/design/games.md 5.6). No SDK or Rockbox headers. */
#ifndef CHESS_ENGINE_H
#define CHESS_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "chess_core.h"

#define CE_MAXPLY       36          /* search depth incl. quiescence */
#define CE_MSTACK       1024        /* moves on the search's stack */
#define CE_INF          32000
#define CE_MATE         30000
#define CE_MATE_MAX     (CE_MATE - 200)     /* above: a mate score */

struct ce_tt {                      /* 8 bytes */
    uint16_t check;                 /* the key's top half */
    uint16_t move;
    int16_t  score;
    uint8_t  depth;
    uint8_t  flag;                  /* CE_TT_* */
};
enum { CE_TT_NONE, CE_TT_EXACT, CE_TT_LOWER, CE_TT_UPPER };

struct ce_frame {
    int16_t  alpha, beta, best, alpha0;
    uint16_t best_move, tt_move;
    uint16_t m0, n, i;              /* its moves: mv[m0 .. m0+n), next i */
    int8_t   depth;                 /* plies left; <= 0: quiescence */
    uint8_t  qs, legal, check;
};

/* the engine's memory, from the game's working block */
struct ce {
    struct cs_pos pos;              /* the search's copy of the game */
    struct ce_tt *tt;               /* 1 << tt_bits entries */
    uint32_t tt_mask;
    uint32_t nodes;
    uint32_t seed;                  /* the level's randomness */
    int16_t  noise;                 /* +- cp at the root (the Easy level) */
    int16_t  score;                 /* the last full iteration's score */
    int16_t  ret;                   /* a child's value, coming back */
    int8_t   ply;                   /* the frame running; -1: none */
    int8_t   depth, max_depth;      /* the iteration, the level's cap */
    int8_t   done_depth;            /* the last full iteration */
    uint8_t  state;                 /* CE_* below */
    uint8_t  have_ret;
    uint16_t best;                  /* the last full iteration's move */
    uint16_t root_best;             /* this iteration's, so far */
    int16_t  root_score;
    uint16_t root_done;             /* root moves searched this iteration */
    uint16_t killer[CE_MAXPLY][2];
    uint16_t mv[CE_MSTACK];
    int16_t  sc[CE_MSTACK];
    struct ce_frame fr[CE_MAXPLY];
    struct cs_undo un[CE_MAXPLY];
};
enum { CE_IDLE, CE_RUN, CE_DONE };

/* our own tapered piece-square tables into pst (6 x 64, packed CS_S);
 * hand them to cs_set_pst() */
void ce_build_pst(int32_t (*pst)[64]);
/* the evaluation, from the side to move's view */
int  ce_eval(const struct cs_pos *p);

/* tt: 1 << tt_bits entries (cleared here) */
void ce_init(struct ce *e, struct ce_tt *tt, int tt_bits);
/* start a search of p (copied, with its hash ring), to max_depth plies;
 * noise > 0 adds a fixed per-move offset of up to +-noise centipawns at
 * the root, from seed (a weaker, varied level) */
void ce_start(struct ce *e, const struct cs_pos *p, int max_depth,
              int noise, uint32_t seed);
/* search up to `nodes` more nodes; CE_RUN (more to do) or CE_DONE */
int  ce_run(struct ce *e, uint32_t nodes);
/* the move to play now: the last full iteration's, or this one's best
 * so far when it has searched its first root move (the previous best,
 * searched first); CS_NOMOVE only when no legal move exists. Ends the
 * search. */
uint16_t ce_stop(struct ce *e);

#endif /* CHESS_ENGINE_H */
