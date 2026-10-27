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

/* Bricks rules (pure, host-tested in tests/bricks): a 308 x 200 field,
 * 8 rows of 14 bricks, a paddle the wheel moves, one ball. The ball moves
 * in short sub-steps, one axis at a time, so it bounces off the side of
 * a brick it hits from the side and off the face it hits from above or
 * below; the paddle sends it off at one of five angles by where it lands.
 * Six layouts, then they come round again faster. No SDK or Rockbox
 * headers: the game (bricks.c) draws it. All lengths are field pixels;
 * positions are kept in 1/256 px. */
#ifndef BRICKS_CORE_H
#define BRICKS_CORE_H

#include <stdint.h>
#include <stdbool.h>

#define BK_FW           308         /* the field */
#define BK_FH           200
#define BK_ROWS         8
#define BK_COLS         14
#define BK_BW           22          /* a brick */
#define BK_BH           10
#define BK_BY           18          /* the top row's top */
#define BK_PW           48          /* the paddle */
#define BK_PH           8
#define BK_PY           184         /* its top */
#define BK_BALL         6
#define BK_LAYOUTS      6
#define BK_VER          1
#define BK_SPEEDUP_HITS 12          /* faster every this many bricks */
#define BK_SPEED_MAX    420         /* px/s, a ceiling */
#define BK_DT_MAX       100         /* one tick never advances more */
#define BK_SUB_PX       2           /* the longest sub-step */

enum { BK_SPEED_SLOW, BK_SPEED_NORMAL, BK_SPEED_FAST, BK_SPEED__N };
/* the paddle's wheel mapping (Paddle Speed setting): levels Slow ..
 * Fastest */
#define BK_PADDLE_N     5
#define BK_PADDLE_DEF   1           /* Normal */
#define BK_PADDLE_VCAP  40          /* clicks/s where the full reach is used */
enum { BK_PH_SERVE, BK_PH_PLAY, BK_PH_OVER };

/* what a call did (bit mask) */
#define BK_EV_WALL      0x01
#define BK_EV_PADDLE    0x02
#define BK_EV_BRICK     0x04
#define BK_EV_LOST      0x08        /* a ball fell past the paddle */
#define BK_EV_CLEARED   0x10        /* the last brick went: next level */
#define BK_EV_OVER      0x20        /* the last ball went */
#define BK_EV_LAUNCH    0x40

/* The whole game: also the state record, consistent after every call.
 * brick[r][c]: 0 gone, else its colour 1..6 */
struct bk_game {
    uint8_t  ver, phase, level, lives;
    uint8_t  speed_set, pad;
    uint16_t hits;                  /* bricks broken this level */
    int32_t  bx, by;                /* the ball's top-left, 1/256 px */
    int32_t  vx, vy;                /* 1/256 px a second */
    int32_t  px;                    /* the paddle's left, 1/256 px */
    uint32_t score;
    uint8_t  brick[BK_ROWS][BK_COLS];
};

/* a new game: speed BK_SPEED_*, lives 1..9 */
void bk_new(struct bk_game *g, int speed_set, int lives);
bool bk_valid(const struct bk_game *g);
/* the layout of a level (1..), into g->brick; the ball back on serve */
void bk_level(struct bk_game *g, int level);
int  bk_bricks_left(const struct bk_game *g);

/* move the paddle dx pixels (clamped to the field); while serving the
 * ball rides on it */
void bk_paddle(struct bk_game *g, int dx);
/* Raw wheel clicks (signed, one per 15 degrees) turned into pixels:
 * each click moves base .. reach px of the level, base when slow and
 * reach from BK_PADDLE_VCAP clicks/s up, linearly between (clicks/s =
 * the wheel velocity). Odd in the clicks, never overshoots a click's
 * worth, monotonic in clicks, velocity and level. */
int  bk_wheel_px(int level, int clicks, unsigned vel);
/* the px one click moves at the speed (1/16 px) */
int  bk_click_px16(int level, unsigned vel);
/* SELECT while serving: the ball goes up and to the right */
int  bk_launch(struct bk_game *g);
/* time: the ball moves and bounces */
int  bk_tick(struct bk_game *g, unsigned dt_ms);

/* the ball's speed now, px/s (setting, level, bricks broken) */
int  bk_speed(const struct bk_game *g);
/* points for breaking a brick in row r (higher rows are worth more) */
int  bk_points(int row);
/* integer pixel positions */
static inline int bk_ball_x(const struct bk_game *g) { return g->bx >> 8; }
static inline int bk_ball_y(const struct bk_game *g) { return g->by >> 8; }
static inline int bk_pad_x(const struct bk_game *g) { return g->px >> 8; }

/* the best scores, highest first (as Quadrus' table) */
int  bk_best_insert(uint32_t *best, int n, uint32_t s);

#endif /* BRICKS_CORE_H */
