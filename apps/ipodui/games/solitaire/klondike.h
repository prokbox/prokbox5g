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

/* Klondike rules (pure, host-tested in tests/solitaire): the deal, the
 * stock and waste (draw one or three, unlimited passes), the four
 * foundations, the seven tableau piles, moves, undo and the win.
 * No SDK or Rockbox headers: the game (solitaire.c) draws it. */
#ifndef SOLITAIRE_KLONDIKE_H
#define SOLITAIRE_KLONDIKE_H

#include <stdint.h>
#include <stdbool.h>

/* A card: bits 0-5 its index (suit * 13 + rank - 1; rank 1 = ace,
 * 13 = king), bit 7 face up. Suits: 0 spades, 1 hearts, 2 diamonds,
 * 3 clubs (hearts and diamonds red). */
#define KL_UP       0x80
#define KL_IDX(c)   ((c) & 0x3f)
#define KL_RANK(c)  (KL_IDX(c) % 13 + 1)
#define KL_SUIT(c)  (KL_IDX(c) / 13)
#define KL_RED(c)   (KL_SUIT(c) == 1 || KL_SUIT(c) == 2)

/* Piles */
enum { KL_STOCK = 0, KL_WASTE = 1, KL_FOUND = 2,    /* 2..5 */
       KL_TAB = 6,                                  /* 6..12 */
       KL_PILES = 13 };
#define KL_PILE_MAX 24      /* stock/waste 24; tableau 6 down + 13 up */

struct kl_move {
    uint8_t from, to, n, flags;
};
#define KL_MV_FLIP      0x01    /* the card it exposed was turned up */
#define KL_MV_DRAW      0x02    /* stock -> waste, n cards */
#define KL_MV_RECYCLE   0x04    /* waste -> stock, n cards */

#define KL_UNDO_MAX 48
#define KL_VER      1

/* The whole game: also the state record (kept consistent after every
 * call). 540 bytes. */
struct kl_state {
    uint8_t  ver;               /* KL_VER */
    uint8_t  draw;              /* 1 or 3: this deal's */
    uint8_t  won, n_undo;
    uint16_t moves, passes;
    uint32_t seed;
    uint32_t elapsed_s;
    uint8_t  len[KL_PILES];
    uint8_t  pad[3];
    uint8_t  card[KL_PILES][KL_PILE_MAX];
    struct kl_move undo[KL_UNDO_MAX];   /* a ring of the latest moves */
    uint8_t  undo_at;           /* next slot in the ring */
    uint8_t  pad2[3];
};

/* A shuffled deal from seed (xorshift32), draw 1 or 3 */
void kl_deal(struct kl_state *s, uint32_t seed, int draw);
/* Is it a sane record (after a load): version, 52 distinct cards,
 * lengths in range? */
bool kl_valid(const struct kl_state *s);

static inline int kl_len(const struct kl_state *s, int p)
{ return s->len[p]; }
/* the top card of pile p, or -1 */
int  kl_top(const struct kl_state *s, int p);
/* the first face-up card's index in pile p (len if none) */
int  kl_first_up(const struct kl_state *s, int p);

/* Stock -> waste (draw cards), or the waste back to the stock when the
 * stock is empty. 0 done, -1 both empty. */
int  kl_draw(struct kl_state *s);
/* Can the n top cards of from go onto to? (from: waste, a foundation or
 * a tableau pile; n > 1 only from a tableau pile, a face-up run) */
bool kl_can(const struct kl_state *s, int from, int n, int to);
/* Moves them (kl_can() first), turning up a tableau card it exposes.
 * 0, or -1 (illegal: nothing changed). */
int  kl_move(struct kl_state *s, int from, int n, int to);
/* The foundation the top card of from can go to, or -1 */
int  kl_found_for(const struct kl_state *s, int from);
/* Send the top card of from (the waste or a tableau pile) to its
 * foundation, as one undoable move. The foundation, or -1 (nothing
 * changed: not legal, or from is the stock or a foundation). */
int  kl_send_home(struct kl_state *s, int from);
/* Takes back the last move; false if none is kept */
bool kl_undo(struct kl_state *s);
/* Every card on a foundation */
bool kl_won(const struct kl_state *s);
/* Every card face up and the stock and waste empty: the rest is
 * mechanical (the auto-finish) */
bool kl_solvable_now(const struct kl_state *s);
/* One auto-finish step: the lowest card that can go to a foundation.
 * The pile moved from, or -1. */
int  kl_finish_step(struct kl_state *s);

#endif /* SOLITAIRE_KLONDIKE_H */
