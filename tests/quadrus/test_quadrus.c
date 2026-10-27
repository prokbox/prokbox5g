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

/* Host test of the Quadrus rules (apps/ipodui/games/quadrus/quadrus_core.c):
 * rotation and wall kicks, the bag, gravity and the lock delay, line
 * clears, levels, the score, game over, the record check and the best
 * scores table; the hold slot, the soft drop and the double tap (owner's
 * controls, 2026-10-24) */
#include <stdio.h>
#include <string.h>
#include "quadrus_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* the piece by hand, in an empty well */
static void place(struct qd_game *g, int p, int r, int x, int y)
{
    g->cur = (uint8_t)p;
    g->rot = (uint8_t)r;
    g->px = (int8_t)x;
    g->py = (int8_t)y;
    g->landed = !qd_fits(g, p, r, x, y + 1);
    g->lock_acc = g->fall_acc = 0;
    g->lock_resets = 0;
}

static void fresh(struct qd_game *g)
{
    qd_new(g, 42, 1);
    memset(g->cell, 0, sizeof(g->cell));
}

/* bottom row r filled except columns in the mask gap */
static void fill_row(struct qd_game *g, int r, unsigned gap)
{
    int c;

    for (c = 0; c < QD_COLS; c++)
        g->cell[r][c] = (gap >> c) & 1 ? 0 : 3;
}

static bool same_cells(int8_t a[4][2], int8_t b[4][2])
{
    return !memcmp(a, b, 8);
}

static void test_rotation(void)
{
    int8_t a[4][2], b[4][2];
    int p;

    for (p = 0; p < QD_PIECES; p++)
    {
        qd_cells(p, 0, a);
        qd_cells(p, 4, b);
        CHECK(same_cells(a, b));            /* four turns: back home */
    }
    /* T: up, then a quarter turn clockwise points right */
    qd_cells(QD_T, 1, a);
    CHECK(a[0][0] == 2 && a[0][1] == 1);
    CHECK(a[1][0] == 1 && a[1][1] == 0);
    CHECK(a[2][0] == 1 && a[2][1] == 1);
    CHECK(a[3][0] == 1 && a[3][1] == 2);
    /* the long piece stands in column 2 of its box */
    qd_cells(QD_I, 1, a);
    for (p = 0; p < 4; p++)
        CHECK(a[p][0] == 2);
}

static void test_kicks(void)
{
    struct qd_game g;

    /* free space: an ordinary turn in place */
    fresh(&g);
    place(&g, QD_T, 0, 4, 8);
    CHECK(qd_rotate(&g, 1) == QD_EV_MOVED);
    CHECK(g.rot == 1 && g.px == 4 && g.py == 8);
    CHECK(qd_rotate(&g, -1) == QD_EV_MOVED && g.rot == 0);

    /* the long piece standing against the left wall (box at -2, its
     * column at 0): turning flat needs a kick of two columns right */
    fresh(&g);
    place(&g, QD_I, 1, -2, 8);
    CHECK(qd_fits(&g, QD_I, 1, -2, 8));
    CHECK(qd_rotate(&g, 1) == QD_EV_MOVED);
    CHECK(g.rot == 2 && g.px == 0);

    /* ... against the right wall: kicked left */
    fresh(&g);
    place(&g, QD_I, 1, QD_COLS - 3, 8);     /* its column at QD_COLS-1 */
    CHECK(qd_rotate(&g, 1) == QD_EV_MOVED);
    CHECK(g.rot == 2 && g.px + 3 <= QD_COLS - 1 && g.px == QD_COLS - 4);

    /* T pointing right, flush with the left wall: turning puts a cell at
     * column -1, so it kicks one right */
    fresh(&g);
    place(&g, QD_T, 1, -1, 8);
    CHECK(qd_fits(&g, QD_T, 1, -1, 8));
    CHECK(qd_rotate(&g, 1) == QD_EV_MOVED);
    CHECK(g.rot == 2 && g.px == 0);

    /* on the floor: a turn that would go through it kicks up a row */
    fresh(&g);
    place(&g, QD_T, 0, 4, QD_H - 2);        /* flat: rows QD_H-2, QD_H-1 */
    CHECK(qd_fits(&g, QD_T, 0, 4, QD_H - 2));
    CHECK(qd_rotate(&g, 1) == QD_EV_MOVED);
    CHECK(g.rot == 1 && g.py == QD_H - 3 && g.px == 4);
    /* the long piece flat on the floor stands up: two rows up */
    fresh(&g);
    place(&g, QD_I, 0, 4, QD_H - 2);        /* its row QD_H-1 */
    CHECK(qd_rotate(&g, 1) == QD_EV_MOVED);
    CHECK(g.rot == 1 && g.py == QD_H - 4);

    /* boxed in: no kick fits, nothing changes */
    fresh(&g);
    place(&g, QD_I, 0, 4, 10);
    memset(g.cell[9], 1, QD_COLS);
    memset(g.cell[10], 0, QD_COLS);
    memset(g.cell[11], 1, QD_COLS);
    g.cell[10][0] = g.cell[10][1] = g.cell[10][2] = g.cell[10][3] = 1;
    g.cell[10][8] = g.cell[10][9] = g.cell[10][10] = g.cell[10][11] = 1;
    place(&g, QD_I, 0, 4, 9);               /* its cells on row 10 */
    CHECK(qd_fits(&g, QD_I, 0, 4, 9));
    CHECK(qd_rotate(&g, 1) == 0);
    CHECK(g.rot == 0 && g.px == 4 && g.py == 9);

    /* the square never turns */
    fresh(&g);
    place(&g, QD_O, 0, 5, 5);
    CHECK(qd_rotate(&g, 1) == 0 && g.rot == 0);
}

static void test_bag(void)
{
    struct qd_game a, b;
    int round, i, seen[QD_PIECES], diff = 0;

    qd_new(&a, 1234, 1);
    qd_new(&b, 1234, 1);
    /* every seven pieces in a row hold all seven (the preview is the
     * first of each bag, so count from nxt) */
    for (round = 0; round < 20; round++)
    {
        memset(seen, 0, sizeof(seen));
        for (i = 0; i < QD_PIECES; i++)
        {
            seen[a.cur]++;
            /* the same seed, the same run */
            CHECK(a.cur == b.cur && a.nxt == b.nxt);
            memset(a.cell, 0, sizeof(a.cell));
            memset(b.cell, 0, sizeof(b.cell));
            qd_hard_drop(&a);
            qd_hard_drop(&b);
        }
        for (i = 0; i < QD_PIECES; i++)
            CHECK(seen[i] == 1);
    }
    qd_new(&a, 1234, 1);
    qd_new(&b, 98765, 1);
    for (i = 0; i < 14; i++)
    {
        diff += a.nxt != b.nxt;
        memset(a.cell, 0, sizeof(a.cell));
        memset(b.cell, 0, sizeof(b.cell));
        qd_hard_drop(&a);
        qd_hard_drop(&b);
    }
    CHECK(diff > 0);
    qd_new(&a, 0, 1);                       /* seed 0 still shuffles */
    CHECK(qd_valid(&a));
}

static void test_gravity_and_lock(void)
{
    struct qd_game g;
    int y, ev;

    fresh(&g);
    place(&g, QD_O, 0, 5, 4);
    y = g.py;
    CHECK(qd_tick(&g, 500) == 0 && qd_tick(&g, 499) == 0);
    CHECK(g.py == y);                       /* level 1: 1 row/s */
    CHECK(qd_tick(&g, 1) == QD_EV_FELL && g.py == y + 1);
    CHECK(qd_fall_ms(1) == 1000 && qd_fall_ms(20) == 50);
    CHECK(qd_fall_ms(10) < qd_fall_ms(9));
    /* a big step is cut to QD_DT_MAX: level 20 falls 20 rows a second */
    place(&g, QD_O, 0, 5, 0);
    g.level = 20;
    ev = qd_tick(&g, 400);
    CHECK(ev == QD_EV_FELL && g.py == 8);
    ev = qd_tick(&g, 10000);                /* to the floor, and resting */
    CHECK(ev == QD_EV_FELL && g.py == QD_H - 2 && g.landed);
    g.level = 1;

    /* resting on the floor: the lock delay */
    place(&g, QD_O, 0, 5, QD_H - 2);
    CHECK(g.landed);
    CHECK(qd_tick(&g, QD_LOCK_MS - 1) == 0);
    /* a move restarts it */
    CHECK(qd_move(&g, -1) == QD_EV_MOVED && g.lock_acc == 0);
    CHECK(qd_tick(&g, QD_LOCK_MS - 1) == 0);
    ev = qd_tick(&g, 1);
    CHECK(ev & QD_EV_LOCK);
    CHECK(ev & QD_EV_SPAWN);
    CHECK(g.cell[QD_H - 1][4] == QD_O + 1 && g.cell[QD_H - 2][5] == QD_O + 1);
    /* moves into a wall or a block do nothing */
    place(&g, QD_O, 0, 0, 5);
    CHECK(qd_move(&g, -1) == 0 && g.px == 0);
    g.cell[5][2] = 1;
    CHECK(qd_move(&g, 1) == 0 && g.px == 0);

    /* the lock resets run out: it locks however much it moves */
    fresh(&g);
    place(&g, QD_O, 0, 4, QD_H - 2);
    for (y = 0; y < QD_LOCK_RESETS; y++)
    {
        qd_tick(&g, QD_LOCK_MS - 10);
        qd_move(&g, (y & 1) ? 1 : -1);
    }
    qd_tick(&g, QD_LOCK_MS - 10);
    qd_move(&g, 1);                         /* no reset left */
    CHECK(qd_tick(&g, 10) & QD_EV_LOCK);

    /* sliding off a ledge: it falls again */
    fresh(&g);
    g.cell[10][4] = g.cell[10][5] = 1;
    place(&g, QD_O, 0, 4, 8);
    CHECK(g.landed);
    CHECK(qd_move(&g, 1) == QD_EV_MOVED && g.landed);  /* still on 5 */
    CHECK(qd_move(&g, 1) == QD_EV_MOVED && !g.landed);
}

static void test_clears(void)
{
    struct qd_game g;
    int ev, r;

    /* one row: a flat long piece into a four-cell gap */
    fresh(&g);
    fill_row(&g, QD_H - 1, 0x0f0);          /* gap at columns 4..7 */
    g.cell[QD_H - 2][0] = 5;                /* a block above, to see fall */
    place(&g, QD_I, 0, 4, 2);
    ev = qd_hard_drop(&g);
    CHECK(ev & QD_EV_LOCK);
    CHECK(ev & QD_EV_CLEAR);
    CHECK(!(ev & QD_EV_SPAWN));
    CHECK(g.phase == QD_PH_CLEAR && g.clear_rows == 1u << (QD_H - 1));
    CHECK(qd_move(&g, 1) == 0);     /* nothing moves while it flashes */
    CHECK(qd_tick(&g, QD_CLEAR_MS - 1) == 0);
    ev = qd_tick(&g, 1);
    CHECK(ev & QD_EV_COLLAPSE);
    CHECK(ev & QD_EV_SPAWN);
    CHECK(g.phase == QD_PH_FALL && g.lines == 1 && g.last_clear == 1);
    CHECK(g.cell[QD_H - 1][0] == 5);        /* the block came down */
    for (r = 1; r < QD_COLS; r++)
        CHECK(g.cell[QD_H - 1][r] == 0);
    CHECK(g.score == 2u * (QD_H - 2 - 2) + 100);

    /* four rows at once, at level 3, a gap row between kept rows */
    qd_new(&g, 7, 3);
    memset(g.cell, 0, sizeof(g.cell));
    for (r = QD_H - 5; r < QD_H; r++)
        fill_row(&g, r, 1u << 11);
    g.cell[QD_H - 5][11] = 0;
    g.cell[QD_H - 5][3] = 0;                /* row QD_H-5 is not full */
    place(&g, QD_I, 1, 9, 2);               /* standing, its column 11 */
    g.score = 0;
    ev = qd_hard_drop(&g);
    CHECK(ev & QD_EV_CLEAR);
    CHECK(g.clear_rows == (0xfu << (QD_H - 4)));
    qd_tick(&g, QD_CLEAR_MS);
    CHECK(g.lines == 4 && g.last_clear == 4);
    CHECK(g.score == 2u * (uint32_t)(QD_H - 4 - 2) + 800u * 3u);
    CHECK(g.cell[QD_H - 1][3] == 0 && g.cell[QD_H - 1][0] == 3);
    CHECK(g.cell[QD_H - 2][0] == 0);
}

static void test_levels_and_score(void)
{
    struct qd_game g;
    int i, ev = 0;

    CHECK(qd_points(1, 1) == 100 && qd_points(2, 1) == 250);
    CHECK(qd_points(3, 2) == 900 && qd_points(4, 5) == 4000);
    CHECK(qd_points(0, 9) == 0 && qd_points(5, 1) == 0);

    /* ten single rows: level 1 -> 2 */
    qd_new(&g, 99, 1);
    for (i = 0; i < 10; i++)
    {
        memset(g.cell, 0, sizeof(g.cell));
        fill_row(&g, QD_H - 1, 0x0f0);
        place(&g, QD_I, 0, 4, 2);
        qd_hard_drop(&g);
        ev = qd_tick(&g, QD_CLEAR_MS);
        CHECK(ev & QD_EV_COLLAPSE);
        CHECK(g.level == (i < 9 ? 1 : 2));
    }
    CHECK(ev & QD_EV_LEVEL);
    CHECK(g.lines == 10);
    /* a start level counts from itself */
    qd_new(&g, 99, 7);
    CHECK(g.level == 7 && g.start_level == 7);
    qd_new(&g, 99, 30);
    CHECK(g.level == QD_START_MAX);
    qd_new(&g, 99, 0);
    CHECK(g.level == 1);
    /* the level stops at the top */
    qd_new(&g, 99, 10);
    g.lines = 500;
    memset(g.cell, 0, sizeof(g.cell));
    fill_row(&g, QD_H - 1, 0x0f0);
    place(&g, QD_I, 0, 4, 2);
    qd_hard_drop(&g);
    qd_tick(&g, QD_CLEAR_MS);
    CHECK(g.level == QD_LEVEL_MAX);
}

static void test_game_over(void)
{
    struct qd_game g;
    int r, ev;

    /* the spawn spot is taken: the next piece can't come in */
    fresh(&g);
    for (r = QD_HIDDEN; r < QD_H; r++)
        fill_row(&g, r, (r & 1) ? 0x001 : 0x800);   /* no full row */
    place(&g, QD_O, 0, 5, 0);               /* in the hidden rows */
    ev = qd_hard_drop(&g);
    CHECK(ev & QD_EV_OVER);
    CHECK(g.phase == QD_PH_OVER);
    CHECK(qd_move(&g, 1) == 0 && qd_rotate(&g, 1) == 0);
    CHECK(qd_hard_drop(&g) == 0 && qd_tick(&g, 1000) == 0);
    CHECK(qd_valid(&g));

    /* a lock with room left, but the next spawn collides */
    fresh(&g);
    for (r = QD_HIDDEN + 2; r < QD_H; r++)
        fill_row(&g, r, (r & 1) ? 0x001 : 0x800);
    g.nxt = QD_T;
    place(&g, QD_O, 0, 0, QD_HIDDEN);       /* rows 2,3 at columns 0,1 */
    ev = qd_hard_drop(&g);
    CHECK(ev & QD_EV_LOCK);
    CHECK(!(ev & QD_EV_OVER));
    g.cell[QD_HIDDEN][5] = 1;               /* the T's spawn cell */
    g.nxt = QD_T;
    place(&g, QD_O, 0, 8, QD_HIDDEN);
    ev = qd_hard_drop(&g);
    CHECK(ev & QD_EV_SPAWN);
    CHECK(ev & QD_EV_OVER);
}

static void test_valid(void)
{
    struct qd_game g, t;

    qd_new(&g, 5, 1);
    CHECK(qd_valid(&g));
    t = g; t.ver = 9;               CHECK(!qd_valid(&t));
    t = g; t.cur = 7;               CHECK(!qd_valid(&t));
    t = g; t.level = 0;             CHECK(!qd_valid(&t));
    t = g; t.cell[5][5] = 8;        CHECK(!qd_valid(&t));
    t = g; t.phase = QD_PH_CLEAR;   CHECK(!qd_valid(&t));  /* no rows */
    t = g; t.bag_n = 9;             CHECK(!qd_valid(&t));
    t = g; t.cell[t.py + 1][t.px + 1] = 1;
    t.cur = QD_O;                   CHECK(!qd_valid(&t));  /* overlaps */
    t = g; t.seed = 0;              CHECK(!qd_valid(&t));
}

static void test_best(void)
{
    uint32_t b[5] = { 0 };

    CHECK(qd_best_insert(b, 5, 0) == -1);
    CHECK(qd_best_insert(b, 5, 300) == 0);
    CHECK(qd_best_insert(b, 5, 500) == 0);
    CHECK(qd_best_insert(b, 5, 400) == 1);
    CHECK(b[0] == 500 && b[1] == 400 && b[2] == 300 && b[3] == 0);
    CHECK(qd_best_insert(b, 5, 100) == 3);
    CHECK(qd_best_insert(b, 5, 50) == 4);
    CHECK(qd_best_insert(b, 5, 10) == -1);  /* full, and lower than all */
    CHECK(qd_best_insert(b, 5, 450) == 1 && b[4] == 100);
}

static void test_hold(void)
{
    struct qd_game g;
    int ev, first, second, third;

    fresh(&g);
    CHECK(g.hold == 0 && !(g.flags & QD_F_HELD));
    first = g.cur;
    second = g.nxt;
    /* an empty slot: the piece goes in, the next one comes */
    ev = qd_hold(&g);
    CHECK((ev & QD_EV_HOLD) && (ev & QD_EV_SPAWN));
    CHECK(g.hold == first + 1 && g.cur == second);
    CHECK(g.rot == 0 && g.py <= QD_HIDDEN && (g.flags & QD_F_HELD));
    third = g.nxt;
    /* once per piece */
    CHECK(qd_hold(&g) == 0 && g.hold == first + 1 && g.cur == second);
    /* it locks: the next piece may hold, and swaps with the slot */
    qd_hard_drop(&g);
    CHECK(!(g.flags & QD_F_HELD) && g.cur == third);
    ev = qd_hold(&g);
    CHECK((ev & QD_EV_HOLD) && g.cur == first && g.hold == third + 1);
    CHECK(g.nxt != 0xff && g.rot == 0);
    CHECK(g.px == (QD_COLS - qd_box(first)) / 2);
    CHECK(qd_hold(&g) == 0);                /* still once per piece */
    CHECK(qd_valid(&g));
    /* not while rows flash, nor after the end */
    g.phase = QD_PH_CLEAR;
    g.flags = 0;
    CHECK(qd_hold(&g) == 0);
    g.phase = QD_PH_OVER;
    CHECK(qd_hold(&g) == 0);
    /* a slot value out of range fails the record check */
    fresh(&g);
    g.hold = QD_PIECES + 1;
    CHECK(!qd_valid(&g));
    g.hold = QD_PIECES;                     /* the last piece: fine */
    CHECK(qd_valid(&g));
    /* the record didn't grow: an old one reads as "nothing held" */
    CHECK(sizeof(struct qd_game) == 284);
}

static void test_soft_drop(void)
{
    struct qd_game g;
    int y;

    fresh(&g);
    place(&g, QD_O, 0, 5, 4);
    CHECK(qd_step_ms(&g) == 1000);
    qd_soft(&g, true);
    CHECK(qd_step_ms(&g) == 100);           /* ten times gravity */
    y = g.py;
    g.score = 0;
    CHECK(qd_tick(&g, 99) == 0 && g.py == y);
    CHECK(qd_tick(&g, 1) == QD_EV_FELL && g.py == y + 1);
    CHECK(g.score == QD_SOFT_PTS);
    qd_tick(&g, 300);
    CHECK(g.py == y + 4 && g.score == 4u * QD_SOFT_PTS);
    /* off: gravity again, and no more points */
    qd_soft(&g, false);
    CHECK(qd_step_ms(&g) == 1000);
    qd_tick(&g, 999);
    CHECK(g.py == y + 4 && g.score == 4u * QD_SOFT_PTS);
    /* fast levels: never past a row a frame */
    g.level = 10;
    qd_soft(&g, true);
    CHECK(qd_step_ms(&g) == QD_SOFT_MIN_MS);
    g.level = 20;
    CHECK(qd_step_ms(&g) == QD_SOFT_MIN_MS);
    /* turning it on mid-step brings no burst of rows */
    fresh(&g);
    place(&g, QD_O, 0, 5, 4);
    y = g.py;
    qd_tick(&g, 900);
    qd_soft(&g, true);
    qd_tick(&g, 33);
    CHECK(g.py == y);
    /* on the floor: the usual lock delay, and it stays on for the next */
    place(&g, QD_O, 0, 5, QD_H - 2);
    CHECK(qd_tick(&g, QD_LOCK_MS - 1) == 0);
    CHECK(qd_tick(&g, 1) & QD_EV_LOCK);
    CHECK(g.flags & QD_F_SOFT);
}

static void test_taps(void)
{
    struct qd_game g;
    int ev, x, y, dy, gy;
    uint32_t s;

    /* a tap turns at once */
    fresh(&g);
    place(&g, QD_T, 0, 4, 6);
    CHECK(!qd_tap_pending(&g));
    CHECK(qd_tap(&g) == QD_EV_MOVED && g.rot == 1);
    CHECK(qd_tap_pending(&g));
    /* a double tap: that turn taken back, then the hard drop */
    x = g.px;
    gy = 0;
    {
        struct qd_game t = g;

        t.rot = 0;
        gy = qd_ghost_y(&t);                /* where the unturned T lands */
    }
    dy = gy - g.py;
    s = g.score;
    ev = qd_double_tap(&g);
    CHECK((ev & QD_EV_LOCK) && (ev & QD_EV_SPAWN));
    CHECK(g.score == s + QD_HARD_PTS * (uint32_t)dy);
    /* the flat T, pointing up, at the floor: row QD_H-1 holds three */
    CHECK(g.cell[QD_H - 1][x] && g.cell[QD_H - 1][x + 1]
          && g.cell[QD_H - 1][x + 2] && g.cell[QD_H - 2][x + 1]
          && !g.cell[QD_H - 2][x] && !g.cell[QD_H - 2][x + 2]);
    CHECK(!qd_tap_pending(&g));             /* the new piece: fresh */

    /* a turn that kicked: the undo goes back through the kick */
    fresh(&g);
    place(&g, QD_T, 1, 0, 6);               /* pointing right, at the wall */
    {
        int8_t cl[4][2];
        int k, minx = 9;

        qd_cells(QD_T, 1, cl);
        for (k = 0; k < 4; k++)
            minx = cl[k][0] < minx ? cl[k][0] : minx;
        g.px = (int8_t)-minx;               /* flush with the left wall */
    }
    x = g.px;
    y = g.py;
    CHECK(qd_tap(&g) == QD_EV_MOVED && g.rot == 2);
    CHECK(g.px != x || g.py != y);          /* it had to kick */
    qd_tick(&g, 1000);                      /* gravity: a row */
    CHECK(qd_untap(&g) == QD_EV_MOVED);
    CHECK(g.rot == 1 && g.px == x && g.py == y + 1);
    CHECK(!qd_tap_pending(&g));
    CHECK(qd_untap(&g) == 0);               /* nothing left to take back */

    /* the square doesn't turn: a double tap still drops it */
    fresh(&g);
    place(&g, QD_O, 0, 5, 4);
    CHECK(qd_tap(&g) == 0 && qd_tap_pending(&g));
    ev = qd_double_tap(&g);
    CHECK(ev & QD_EV_LOCK);
    CHECK(g.cell[QD_H - 1][5] == QD_O + 1 && g.cell[QD_H - 2][6] == QD_O + 1);

    /* a lock between the taps: the new piece has no tap to take back */
    fresh(&g);
    place(&g, QD_T, 0, 4, QD_H - 2);
    qd_tap(&g);
    qd_tick(&g, QD_LOCK_MS);
    CHECK(!qd_tap_pending(&g));
    /* a hold forgets the tap too */
    place(&g, QD_T, 0, 4, 6);
    qd_tap(&g);
    qd_hold(&g);
    CHECK(!qd_tap_pending(&g) && qd_untap(&g) == 0);

    /* a double tap clears rows: the flash, then the collapse */
    fresh(&g);
    fill_row(&g, QD_H - 1, 0x0f0);
    place(&g, QD_I, 0, 4, 2);               /* flat, over the gap */
    CHECK(qd_tap(&g) == QD_EV_MOVED && g.rot == 1);
    ev = qd_double_tap(&g);                 /* flat again: the gap fits */
    CHECK((ev & QD_EV_LOCK) && (ev & QD_EV_CLEAR));
    ev = qd_tick(&g, QD_CLEAR_MS);
    CHECK((ev & QD_EV_COLLAPSE) && g.lines == 1);

    /* the record check holds the undo in range */
    fresh(&g);
    g.undo_dx = 3;
    CHECK(!qd_valid(&g));
    g.undo_dx = 0;
    g.undo_dy = 1;
    CHECK(!qd_valid(&g));
}

int main(void)
{
    test_rotation();
    test_kicks();
    test_bag();
    test_gravity_and_lock();
    test_clears();
    test_levels_and_score();
    test_game_over();
    test_valid();
    test_best();
    test_hold();
    test_soft_drop();
    test_taps();
    printf("quadrus: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
