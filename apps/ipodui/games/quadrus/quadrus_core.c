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

/* Quadrus rules: see quadrus_core.h. Our own rotation (a plain quarter
 * turn in the piece's box) and our own kicks: try in place, one column
 * either way, one row up (a turn on the floor), then the same up a row;
 * the long piece also tries two columns either way, and two rows up. */
#include <string.h>
#include "quadrus_core.h"

/* rotation 0 of every piece, { col, row } in its box */
static const int8_t shape0[QD_PIECES][4][2] = {
    [QD_I] = { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } },
    [QD_O] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } },
    [QD_T] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },
    [QD_S] = { { 1, 0 }, { 2, 0 }, { 0, 1 }, { 1, 1 } },
    [QD_Z] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 2, 1 } },
    [QD_J] = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },
    [QD_L] = { { 2, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },
};

static const int8_t kicks[][2] = {
    { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { -1, -1 }, { 1, -1 },
    { -2, 0 }, { 2, 0 }, { 0, -2 }, /* the long piece only */
};
#define KICKS_N     6
#define KICKS_I_N   9

static const uint16_t fall_tab[QD_LEVEL_MAX + 1] = {
    1000, 1000, 820, 672, 551, 452, 371, 304, 249, 204, 168,
    137, 113, 92, 76, 62, 55, 52, 50, 50, 50,
};
static const uint16_t points_tab[5] = { 0, 100, 250, 450, 800 };

int qd_box(int p)
{
    return p == QD_I ? 4 : p == QD_O ? 2 : 3;
}

void qd_cells(int p, int r, int8_t out[4][2])
{
    int i, k, n = qd_box(p);

    for (i = 0; i < 4; i++)
    {
        int x = shape0[p][i][0], y = shape0[p][i][1];

        for (k = 0; k < (r & 3); k++)
        {
            int t = x;              /* a quarter turn clockwise */

            x = n - 1 - y;
            y = t;
        }
        out[i][0] = (int8_t)x;
        out[i][1] = (int8_t)y;
    }
}

bool qd_fits(const struct qd_game *g, int p, int r, int x, int y)
{
    int8_t cl[4][2];
    int i;

    qd_cells(p, r, cl);
    for (i = 0; i < 4; i++)
    {
        int cx = x + cl[i][0], cy = y + cl[i][1];

        if (cx < 0 || cx >= QD_COLS || cy < 0 || cy >= QD_H)
            return false;
        if (g->cell[cy][cx])
            return false;
    }
    return true;
}

/* -- the bag -------------------------------------------------------------- */
static uint32_t rnd(struct qd_game *g)
{
    uint32_t x = g->seed;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->seed = x;
    return x;
}

static int bag_draw(struct qd_game *g)
{
    if (g->bag_n == 0)
    {
        int i;

        for (i = 0; i < QD_PIECES; i++)
            g->bag[i] = (uint8_t)i;
        for (i = QD_PIECES - 1; i > 0; i--)
        {
            int j = (int)(rnd(g) % (uint32_t)(i + 1));
            uint8_t t = g->bag[i];

            g->bag[i] = g->bag[j];
            g->bag[j] = t;
        }
        g->bag_n = QD_PIECES;
    }
    return g->bag[--g->bag_n];
}

/* -- the piece's life ----------------------------------------------------- */
static bool fits_now(const struct qd_game *g, int dx, int dy)
{
    return qd_fits(g, g->cur, g->rot, g->px + dx, g->py + dy);
}

/* piece p comes in at the top (a spawn, or out of the hold slot) */
static int enter(struct qd_game *g, int p)
{
    g->cur = (uint8_t)p;
    g->rot = 0;
    g->px = (int8_t)((QD_COLS - qd_box(g->cur)) / 2);
    g->py = (int8_t)(g->cur == QD_I ? QD_HIDDEN - 1 : QD_HIDDEN);
    g->fall_acc = g->lock_acc = 0;
    g->lock_resets = 0;
    if (!fits_now(g, 0, 0))
    {
        g->phase = QD_PH_OVER;      /* no room for it: the well is full */
        return QD_EV_SPAWN | QD_EV_OVER;
    }
    g->landed = !fits_now(g, 0, 1);
    return QD_EV_SPAWN;
}

static int spawn(struct qd_game *g)
{
    int p = g->nxt;

    g->nxt = (uint8_t)bag_draw(g);
    return enter(g, p);
}

/* the piece is gone (locked, or into the hold slot): its tap with it */
static void forget_tap(struct qd_game *g)
{
    g->flags &= (uint8_t)~(QD_F_TAP | QD_F_UNDO | QD_F_ROT_MASK);
    g->undo_dx = g->undo_dy = 0;
}

static int lock(struct qd_game *g)
{
    int8_t cl[4][2];
    int i, r, c, ev = QD_EV_LOCK;
    bool all_hidden = true;
    uint32_t full = 0;

    qd_cells(g->cur, g->rot, cl);
    for (i = 0; i < 4; i++)
    {
        int cx = g->px + cl[i][0], cy = g->py + cl[i][1];

        g->cell[cy][cx] = (uint8_t)(g->cur + 1);
        if (cy >= QD_HIDDEN)
            all_hidden = false;
    }
    g->landed = 0;
    forget_tap(g);
    g->flags &= (uint8_t)~QD_F_HELD;    /* the next piece may hold again */
    if (all_hidden)
    {
        g->phase = QD_PH_OVER;      /* locked above the well */
        return ev | QD_EV_OVER;
    }
    for (r = 0; r < QD_H; r++)
    {
        for (c = 0; c < QD_COLS && g->cell[r][c]; c++)
            ;
        if (c == QD_COLS)
            full |= 1u << r;
    }
    if (full)
    {
        g->phase = QD_PH_CLEAR;
        g->clear_rows = full;
        g->clear_acc = 0;
        return ev | QD_EV_CLEAR;
    }
    return ev | spawn(g);
}

static int collapse(struct qd_game *g)
{
    int r, w = QD_H - 1, n = 0, ev = QD_EV_COLLAPSE, lv;

    for (r = QD_H - 1; r >= 0; r--)
    {
        if (g->clear_rows & (1u << r))
        {
            n++;
            continue;
        }
        if (w != r)
            memcpy(g->cell[w], g->cell[r], QD_COLS);
        w--;
    }
    for (; w >= 0; w--)
        memset(g->cell[w], 0, QD_COLS);
    g->clear_rows = 0;
    g->last_clear = (uint8_t)n;
    g->score += qd_points(n, g->level);
    g->lines = (uint16_t)(g->lines + n);
    lv = g->start_level + g->lines / QD_LINES_PER_LEVEL;
    if (lv > QD_LEVEL_MAX)
        lv = QD_LEVEL_MAX;
    if (lv > g->level)
    {
        g->level = (uint8_t)lv;
        ev |= QD_EV_LEVEL;
    }
    g->phase = QD_PH_FALL;
    return ev | spawn(g);
}

/* a move or a turn while resting restarts the lock delay (a few times) */
static void after_move(struct qd_game *g)
{
    if (g->landed && g->lock_resets < QD_LOCK_RESETS)
    {
        g->lock_acc = 0;
        g->lock_resets++;
    }
    g->landed = !fits_now(g, 0, 1);
}

/* -- the API -------------------------------------------------------------- */
void qd_new(struct qd_game *g, uint32_t seed, int start_level)
{
    memset(g, 0, sizeof(*g));
    if (start_level < 1)
        start_level = 1;
    if (start_level > QD_START_MAX)
        start_level = QD_START_MAX;
    g->ver = QD_VER;
    g->phase = QD_PH_FALL;
    g->level = g->start_level = (uint8_t)start_level;
    g->seed = seed ? seed : 0x9e3779b9u;
    g->nxt = (uint8_t)bag_draw(g);
    spawn(g);
}

bool qd_valid(const struct qd_game *g)
{
    int r, c;

    if (g->ver != QD_VER || g->phase > QD_PH_OVER || g->level < 1
        || g->level > QD_LEVEL_MAX || g->start_level < 1
        || g->start_level > QD_START_MAX || g->level < g->start_level
        || g->cur >= QD_PIECES || g->nxt >= QD_PIECES || g->rot > 3
        || g->bag_n > QD_PIECES || !g->seed || g->hold > QD_PIECES
        || g->undo_dx < -2 || g->undo_dx > 2 || g->undo_dy < -2
        || g->undo_dy > 0
        || (g->clear_rows >> QD_H) != 0
        || (g->phase == QD_PH_CLEAR) != (g->clear_rows != 0))
        return false;
    for (r = 0; r < g->bag_n; r++)
        if (g->bag[r] >= QD_PIECES)
            return false;
    for (r = 0; r < QD_H; r++)
        for (c = 0; c < QD_COLS; c++)
            if (g->cell[r][c] > QD_PIECES)
                return false;
    if (g->phase == QD_PH_FALL && !fits_now(g, 0, 0))
        return false;
    return true;
}

int qd_move(struct qd_game *g, int dx)
{
    if (g->phase != QD_PH_FALL || (dx != -1 && dx != 1)
        || !fits_now(g, dx, 0))
        return 0;
    g->px = (int8_t)(g->px + dx);
    after_move(g);
    return QD_EV_MOVED;
}

/* a quarter turn with kicks: the kick used, or -1 (no room, or the
 * square) */
static int turn(struct qd_game *g, int dir)
{
    int nr, i, n;

    if (g->phase != QD_PH_FALL || g->cur == QD_O)
        return -1;
    nr = (g->rot + (dir < 0 ? 3 : 1)) & 3;
    n = g->cur == QD_I ? KICKS_I_N : KICKS_N;
    for (i = 0; i < n; i++)
    {
        int x = g->px + kicks[i][0], y = g->py + kicks[i][1];

        if (qd_fits(g, g->cur, nr, x, y))
        {
            g->rot = (uint8_t)nr;
            g->px = (int8_t)x;
            g->py = (int8_t)y;
            after_move(g);
            return i;
        }
    }
    return -1;
}

int qd_rotate(struct qd_game *g, int dir)
{
    return turn(g, dir) >= 0 ? QD_EV_MOVED : 0;
}

int qd_tap(struct qd_game *g)
{
    int rot = g->rot, k;

    if (g->phase != QD_PH_FALL)
        return 0;
    forget_tap(g);
    g->flags |= QD_F_TAP;
    k = turn(g, 1);
    if (k < 0)
        return 0;                       /* nothing turned: nothing to undo */
    g->flags |= (uint8_t)(QD_F_UNDO | (rot << QD_F_ROT_SHIFT));
    g->undo_dx = kicks[k][0];
    g->undo_dy = kicks[k][1];
    return QD_EV_MOVED;
}

int qd_untap(struct qd_game *g)
{
    int rot = (g->flags & QD_F_ROT_MASK) >> QD_F_ROT_SHIFT, ev = 0;
    int x = g->px - g->undo_dx, y = g->py - g->undo_dy;

    if (g->phase == QD_PH_FALL && (g->flags & QD_F_UNDO))
    {
        /* back where the turn started, as far as gravity and the wheel
         * have moved it since; failing that, the old turn in place */
        if (!qd_fits(g, g->cur, rot, x, y))
        {
            x = g->px;
            y = g->py;
        }
        if (qd_fits(g, g->cur, rot, x, y))
        {
            g->rot = (uint8_t)rot;
            g->px = (int8_t)x;
            g->py = (int8_t)y;
            after_move(g);
            ev = QD_EV_MOVED;
        }
    }
    forget_tap(g);
    return ev;
}

int qd_double_tap(struct qd_game *g)
{
    if (g->phase != QD_PH_FALL)
        return 0;
    return qd_untap(g) | qd_hard_drop(g);
}

bool qd_tap_pending(const struct qd_game *g)
{
    return g->phase == QD_PH_FALL && (g->flags & QD_F_TAP);
}

void qd_soft(struct qd_game *g, bool on)
{
    if (on)
        g->flags |= QD_F_SOFT;
    else
        g->flags &= (uint8_t)~QD_F_SOFT;
    if (g->fall_acc >= qd_step_ms(g))
        g->fall_acc = 0;                /* the faster step: no burst */
}

int qd_hold(struct qd_game *g)
{
    int ev, out;

    if (g->phase != QD_PH_FALL || (g->flags & QD_F_HELD))
        return 0;
    out = g->cur;
    forget_tap(g);
    if (!g->hold)
        ev = spawn(g);
    else
        ev = enter(g, g->hold - 1);
    g->hold = (uint8_t)(out + 1);
    g->flags |= QD_F_HELD;
    if (ev & QD_EV_OVER)
        g->flags &= (uint8_t)~QD_F_HELD;
    return ev | QD_EV_HOLD;
}

int qd_hard_drop(struct qd_game *g)
{
    int gy;

    if (g->phase != QD_PH_FALL)
        return 0;
    gy = qd_ghost_y(g);
    g->score += QD_HARD_PTS * (uint32_t)(gy - g->py);
    g->py = (int8_t)gy;
    return QD_EV_MOVED | lock(g);
}

int qd_tick(struct qd_game *g, unsigned dt)
{
    int ev = 0;

    if (dt > QD_DT_MAX)
        dt = QD_DT_MAX;
    switch (g->phase)
    {
        case QD_PH_CLEAR:
            g->clear_acc = (uint16_t)(g->clear_acc + dt);
            if (g->clear_acc >= QD_CLEAR_MS)
                ev = collapse(g);
            return ev;
        case QD_PH_FALL:
            g->landed = !fits_now(g, 0, 1);
            if (!g->landed)
            {
                unsigned step = qd_step_ms(g);

                g->fall_acc = (uint16_t)(g->fall_acc + dt);
                while (g->fall_acc >= step)
                {
                    g->fall_acc = (uint16_t)(g->fall_acc - step);
                    if (!fits_now(g, 0, 1))
                        break;
                    g->py++;
                    if (g->flags & QD_F_SOFT)
                        g->score += QD_SOFT_PTS;
                    ev |= QD_EV_FELL;
                }
                g->landed = !fits_now(g, 0, 1);
                if (g->landed)
                    g->fall_acc = g->lock_acc = 0;
                return ev;
            }
            g->lock_acc = (uint16_t)(g->lock_acc + dt);
            if (g->lock_acc >= QD_LOCK_MS)
                ev |= lock(g);
            return ev;
        default:
            return 0;
    }
}

int qd_ghost_y(const struct qd_game *g)
{
    int y = g->py;

    while (qd_fits(g, g->cur, g->rot, g->px, y + 1))
        y++;
    return y;
}

unsigned qd_fall_ms(int level)
{
    if (level < 1)
        level = 1;
    if (level > QD_LEVEL_MAX)
        level = QD_LEVEL_MAX;
    return fall_tab[level];
}

unsigned qd_step_ms(const struct qd_game *g)
{
    unsigned f = qd_fall_ms(g->level), s;

    if (!(g->flags & QD_F_SOFT))
        return f;
    s = f / QD_SOFT_DIV;
    if (s < QD_SOFT_MIN_MS)
        s = QD_SOFT_MIN_MS;
    return s < f ? s : f;
}

uint32_t qd_points(int rows, int level)
{
    if (rows < 0 || rows > 4)
        return 0;
    return (uint32_t)points_tab[rows] * (uint32_t)level;
}

int qd_best_insert(uint32_t *best, int n, uint32_t s)
{
    int i, k;

    if (s == 0)
        return -1;
    for (i = 0; i < n && best[i] >= s; i++)
        ;
    if (i == n)
        return -1;
    for (k = n - 1; k > i; k--)
        best[k] = best[k - 1];
    best[i] = s;
    return i;
}
