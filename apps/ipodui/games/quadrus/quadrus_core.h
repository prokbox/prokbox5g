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

/* Quadrus rules (pure, host-tested in tests/quadrus): a 12 x 18 well, the
 * seven four-square pieces from a shuffled bag of seven, rotation with
 * our own wall kicks, gravity by elapsed time, a lock delay, line clears
 * (a short flash, then the collapse), levels, the score and game over;
 * the hold slot, the soft drop, and the tap whose turn a double tap takes
 * back before its hard drop (owner, 2026-10-24).
 * No SDK or Rockbox headers: the game (quadrus.c) draws it. */
#ifndef QUADRUS_CORE_H
#define QUADRUS_CORE_H

#include <stdint.h>
#include <stdbool.h>

#define QD_COLS         12
#define QD_ROWS         18          /* visible */
#define QD_HIDDEN       2           /* above the well: room to spawn */
#define QD_H            (QD_ROWS + QD_HIDDEN)
#define QD_VER          1

#define QD_LEVEL_MAX    20
#define QD_START_MAX    10
#define QD_LINES_PER_LEVEL 10
#define QD_LOCK_MS      500         /* resting before the piece locks */
#define QD_LOCK_RESETS  12          /* moves that restart it, per piece */
#define QD_CLEAR_MS     240         /* the full rows flash this long */
#define QD_DT_MAX       1000        /* one tick never advances more */
#define QD_SOFT_DIV     10          /* soft drop: gravity's step / 10 */
#define QD_SOFT_MIN_MS  33          /* ...but never past a row a frame */
#define QD_SOFT_PTS     1           /* points a soft-dropped row */
#define QD_HARD_PTS     2           /* points a hard-dropped row */

enum { QD_I, QD_O, QD_T, QD_S, QD_Z, QD_J, QD_L, QD_PIECES };
enum { QD_PH_FALL, QD_PH_CLEAR, QD_PH_OVER };

/* what a call did (bit mask) */
#define QD_EV_MOVED     0x01        /* the piece moved or turned */
#define QD_EV_FELL      0x02        /* gravity moved it down */
#define QD_EV_LOCK      0x04        /* it locked into the well */
#define QD_EV_CLEAR     0x08        /* full rows began to flash */
#define QD_EV_COLLAPSE  0x10        /* the flashing rows went */
#define QD_EV_SPAWN     0x20        /* a new piece came in */
#define QD_EV_OVER      0x40        /* the game ended */
#define QD_EV_LEVEL     0x80        /* the level went up */
#define QD_EV_HOLD      0x100       /* the piece went to the hold slot */

/* qd_game.flags */
#define QD_F_HELD       0x01        /* this piece came from (or went to) the
                                       hold slot: no second hold until it
                                       locks */
#define QD_F_SOFT       0x02        /* the soft drop is on */
#define QD_F_TAP        0x04        /* a tap turned (or tried) this piece */
#define QD_F_UNDO       0x08        /* ...and undo_* takes that turn back */
#define QD_F_ROT_SHIFT  4           /* bits 4-5: the rotation before it */
#define QD_F_ROT_MASK   0x30

/* The whole game: also the state record, consistent after every call.
 * cell[r][c]: 0 empty, else 1 + the piece that left it. hold: 0 empty,
 * else 1 + the piece in the slot. hold, flags and undo_* sit in what was
 * padding (284 bytes, every member naturally aligned), so a record saved
 * before them reads as "nothing held". */
struct qd_game {
    uint8_t  ver, phase, level, start_level;
    uint8_t  cur, rot, nxt, lock_resets;
    int8_t   px, py;                /* the piece's box, in cells */
    uint8_t  landed, last_clear;
    uint8_t  bag[QD_PIECES], bag_n;
    uint8_t  hold;
    uint8_t  flags;                 /* QD_F_* */
    uint16_t lines;
    uint16_t fall_acc, lock_acc, clear_acc;
    int8_t   undo_dx, undo_dy;      /* the tap's kick, to take back */
    uint32_t clear_rows;            /* bit r: row r is flashing */
    uint32_t score;
    uint32_t seed;
    uint8_t  cell[QD_H][QD_COLS];
};

/* the cells of piece p in rotation r: out[i] = { col, row } in its box */
void qd_cells(int p, int r, int8_t out[4][2]);
/* the box's size (I 4, O 2, the others 3) */
int  qd_box(int p);

/* a new game from a seed, at start level 1..QD_START_MAX */
void qd_new(struct qd_game *g, uint32_t seed, int start_level);
/* the record passes every range check (a resumed game) */
bool qd_valid(const struct qd_game *g);
/* does piece p in rotation r fit with its box at (x, y)? */
bool qd_fits(const struct qd_game *g, int p, int r, int x, int y);

/* the player: one column left (-1) or right (+1); a quarter turn
 * clockwise (+1) or anticlockwise (-1), with wall kicks; the hard drop */
int  qd_move(struct qd_game *g, int dx);
int  qd_rotate(struct qd_game *g, int dir);
int  qd_hard_drop(struct qd_game *g);
/* SELECT (owner, 2026-10-24). qd_tap(): a press, a quarter turn clockwise
 * at once, remembered. qd_untap(): take that turn back (the press became a
 * hold, or the exit chord cancelled it). qd_double_tap(): a second press
 * soon after: take the first one's turn back, then the hard drop.
 * qd_tap_pending(): this piece has a turn a double tap may take back. */
int  qd_tap(struct qd_game *g);
int  qd_untap(struct qd_game *g);
int  qd_double_tap(struct qd_game *g);
bool qd_tap_pending(const struct qd_game *g);
/* the soft drop on or off: gravity's step / QD_SOFT_DIV (never under
 * QD_SOFT_MIN_MS), QD_SOFT_PTS a row it brings down */
void qd_soft(struct qd_game *g, bool on);
/* the hold slot: an empty slot takes the piece and the next one comes in;
 * a full one swaps. Once per piece (until it locks). */
int  qd_hold(struct qd_game *g);
/* time: gravity, the lock delay and the clear flash */
int  qd_tick(struct qd_game *g, unsigned dt_ms);

/* where the piece would land (its box's row) */
int  qd_ghost_y(const struct qd_game *g);
/* gravity's step at a level, in ms (level 1: 1000 ms; level 20: 50) */
unsigned qd_fall_ms(int level);
/* the step now: gravity's, or the soft drop's while it is on */
unsigned qd_step_ms(const struct qd_game *g);
/* points for n rows cleared at once at a level */
uint32_t qd_points(int rows, int level);

/* the best scores, highest first: inserts s, returns its place (0..n-1)
 * or -1 if it didn't make the table */
int  qd_best_insert(uint32_t *best, int n, uint32_t s);

#endif /* QUADRUS_CORE_H */
