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

/* Bricks rules: see bricks_core.h */
#include <string.h>
#include "bricks_core.h"

#define FULL 0x3fff
/* our six layouts: one 14-bit mask a row (bit c = column c) */
static const uint16_t layouts[BK_LAYOUTS][BK_ROWS] = {
    { FULL, FULL, FULL, FULL, FULL, FULL, FULL, FULL },     /* the wall */
    { 0x0180, 0x03c0, 0x07e0, 0x0ff0, 0x1ff8, 0x3ffc, FULL, FULL },
    { 0x2aaa, 0x1555, 0x2aaa, 0x1555, 0x2aaa, 0x1555, 0x2aaa, 0x1555 },
    { FULL, 0x3333, 0x3333, 0x3333, 0x3333, 0x3333, 0x3333, FULL },
    { FULL, 0x2001, 0x23f1, 0x2211, 0x2211, 0x23f1, 0x2001, FULL },
    { 0x00c0, 0x01e0, 0x03f0, 0x07f8, 0x07f8, 0x03f0, 0x01e0, 0x00c0 },
};

/* the five paddle zones' directions, 1/256 (left to right); the middle
 * one keeps the ball's sideways sense */
static const int16_t zone_dir[5][2] = {
    { -181, -181 }, { -110, -231 }, { 48, -251 }, { 110, -231 },
    { 181, -181 },
};
static const int16_t base_speed[BK_SPEED__N] = { 130, 170, 215 };

int bk_points(int row)
{
    return row < 0 || row >= BK_ROWS ? 0 : (4 - row / 2) * 10;
}

int bk_speed(const struct bk_game *g)
{
    int lv = g->level - 1;
    int pct = 100 + 6 * (lv % BK_LAYOUTS) + 20 * (lv / BK_LAYOUTS)
              + 5 * (g->hits / BK_SPEEDUP_HITS);
    int s = base_speed[g->speed_set < BK_SPEED__N ? g->speed_set : 1] * pct
            / 100;

    return s > BK_SPEED_MAX ? BK_SPEED_MAX : s;
}

int bk_bricks_left(const struct bk_game *g)
{
    int r, c, n = 0;

    for (r = 0; r < BK_ROWS; r++)
        for (c = 0; c < BK_COLS; c++)
            n += g->brick[r][c] != 0;
    return n;
}

static void serve(struct bk_game *g)
{
    g->phase = BK_PH_SERVE;
    g->vx = g->vy = 0;
    g->bx = g->px + ((BK_PW - BK_BALL) / 2 << 8);
    g->by = (BK_PY - BK_BALL) << 8;
}

void bk_level(struct bk_game *g, int level)
{
    const uint16_t *l = layouts[(level - 1) % BK_LAYOUTS];
    int r, c;

    g->level = (uint8_t)level;
    g->hits = 0;
    for (r = 0; r < BK_ROWS; r++)
        for (c = 0; c < BK_COLS; c++)
            g->brick[r][c] = (uint8_t)((l[r] >> c) & 1 ? r / 2 + 1 : 0);
    g->px = ((BK_FW - BK_PW) / 2) << 8;
    serve(g);
}

void bk_new(struct bk_game *g, int speed_set, int lives)
{
    memset(g, 0, sizeof(*g));
    g->ver = BK_VER;
    g->speed_set = (uint8_t)(speed_set >= 0 && speed_set < BK_SPEED__N
                             ? speed_set : BK_SPEED_NORMAL);
    g->lives = (uint8_t)(lives < 1 ? 1 : lives > 9 ? 9 : lives);
    bk_level(g, 1);
}

bool bk_valid(const struct bk_game *g)
{
    int r, c;

    if (g->ver != BK_VER || g->phase > BK_PH_OVER || g->level < 1
        || g->lives > 9 || (g->lives == 0) != (g->phase == BK_PH_OVER)
        || g->speed_set >= BK_SPEED__N
        || g->px < 0 || g->px > (BK_FW - BK_PW) << 8
        || g->bx < 0 || g->bx > (BK_FW - BK_BALL) << 8
        || g->by < -(1 << 8) || g->by > BK_FH << 8
        || g->vx < -(BK_SPEED_MAX << 8) || g->vx > BK_SPEED_MAX << 8
        || g->vy < -(BK_SPEED_MAX << 8) || g->vy > BK_SPEED_MAX << 8)
        return false;
    for (r = 0; r < BK_ROWS; r++)
        for (c = 0; c < BK_COLS; c++)
            if (g->brick[r][c] > 6)
                return false;
    return true;
}

/* per level: a click's px at a crawl and at BK_PADDLE_VCAP clicks/s
 * and over (1/16 px). Half a turn is 12 clicks: Normal crosses the 260 px
 * of travel in 12 clicks at 20 clicks/s. */
static const uint16_t paddle_lo[BK_PADDLE_N] = { 8 * 16, 14 * 16, 20 * 16,
                                                  28 * 16, 40 * 16 };
static const uint16_t paddle_hi[BK_PADDLE_N] = { 18 * 16, 30 * 16, 42 * 16,
                                                  56 * 16, 80 * 16 };

int bk_click_px16(int level, unsigned vel)
{
    int lo, hi;

    if (level < 0)
        level = 0;
    if (level >= BK_PADDLE_N)
        level = BK_PADDLE_N - 1;
    if (vel > BK_PADDLE_VCAP)
        vel = BK_PADDLE_VCAP;
    lo = paddle_lo[level];
    hi = paddle_hi[level];
    return lo + (int)((hi - lo) * vel / BK_PADDLE_VCAP);
}

int bk_wheel_px(int level, int clicks, unsigned vel)
{
    int n = clicks < 0 ? -clicks : clicks;
    int px = (n * bk_click_px16(level, vel) + 8) >> 4;

    return clicks < 0 ? -px : px;
}

void bk_paddle(struct bk_game *g, int dx)
{
    int32_t p;

    if (g->phase == BK_PH_OVER)
        return;
    p = g->px + dx * 256;
    if (p < 0)
        p = 0;
    if (p > (BK_FW - BK_PW) << 8)
        p = (BK_FW - BK_PW) << 8;
    g->px = p;
    if (g->phase == BK_PH_SERVE)
        serve(g);
}

static void aim(struct bk_game *g, int zone, int sense)
{
    int s = bk_speed(g) * 256;
    int dx = zone_dir[zone][0], dy = zone_dir[zone][1];

    if (zone == 2 && sense < 0)
        dx = -dx;
    g->vx = s * dx / 256;
    g->vy = s * dy / 256;
}

int bk_launch(struct bk_game *g)
{
    if (g->phase != BK_PH_SERVE)
        return 0;
    g->phase = BK_PH_PLAY;
    aim(g, 3, 1);
    return BK_EV_LAUNCH;
}

/* every brick the ball's rect overlaps goes; true if any did */
static bool hit_bricks(struct bk_game *g)
{
    int x = g->bx >> 8, y = g->by >> 8, r, c, r0, r1, c0, c1;
    bool hit = false;

    if (y + BK_BALL <= BK_BY || y >= BK_BY + BK_ROWS * BK_BH)
        return false;
    r0 = (y - BK_BY) / BK_BH;
    r1 = (y + BK_BALL - 1 - BK_BY) / BK_BH;
    c0 = x / BK_BW;
    c1 = (x + BK_BALL - 1) / BK_BW;
    if (y < BK_BY)
        r0 = 0;
    if (r1 >= BK_ROWS)
        r1 = BK_ROWS - 1;
    if (c1 >= BK_COLS)
        c1 = BK_COLS - 1;
    for (r = r0; r <= r1; r++)
        for (c = c0; c <= c1; c++)
            if (g->brick[r][c])
            {
                g->brick[r][c] = 0;
                g->hits++;
                g->score += (uint32_t)bk_points(r);
                hit = true;
            }
    return hit;
}

int bk_tick(struct bk_game *g, unsigned dt)
{
    int32_t rx, ry;
    int ev = 0, n, i;

    if (g->phase != BK_PH_PLAY)
        return 0;
    if (dt > BK_DT_MAX)
        dt = BK_DT_MAX;
    rx = g->vx * (int32_t)dt / 1000;
    ry = g->vy * (int32_t)dt / 1000;
    n = (int)((rx < 0 ? -rx : rx) > (ry < 0 ? -ry : ry)
              ? (rx < 0 ? -rx : rx) : (ry < 0 ? -ry : ry))
        / (BK_SUB_PX * 256) + 1;
    for (i = n; i > 0; i--)
    {
        int32_t sx = rx / i, sy = ry / i, old_bottom;

        rx -= sx;
        ry -= sy;
        /* across */
        g->bx += sx;
        if (g->bx < 0 || g->bx > (BK_FW - BK_BALL) << 8)
        {
            g->bx = g->bx < 0 ? -g->bx
                              : 2 * ((BK_FW - BK_BALL) << 8) - g->bx;
            g->vx = -g->vx;
            rx = -rx;
            ev |= BK_EV_WALL;
        }
        else if (hit_bricks(g))
        {
            g->bx -= sx;
            g->vx = -g->vx;
            rx = -rx;
            ev |= BK_EV_BRICK;
        }
        /* up and down */
        old_bottom = (g->by >> 8) + BK_BALL;
        g->by += sy;
        if (g->by < 0)
        {
            g->by = -g->by;
            g->vy = -g->vy;
            ry = -ry;
            ev |= BK_EV_WALL;
        }
        else if (hit_bricks(g))
        {
            g->by -= sy;
            g->vy = -g->vy;
            ry = -ry;
            ev |= BK_EV_BRICK;
        }
        else if (g->vy > 0 && old_bottom <= BK_PY
                 && (g->by >> 8) + BK_BALL > BK_PY)
        {
            int cx = (g->bx >> 8) + BK_BALL / 2 - (g->px >> 8);

            if (cx >= -BK_BALL / 2 && cx < BK_PW + BK_BALL / 2)
            {
                int z = cx < 0 ? 0 : cx >= BK_PW ? 4 : cx * 5 / BK_PW;

                g->by = (BK_PY - BK_BALL) << 8;
                aim(g, z, g->vx);
                ev |= BK_EV_PADDLE;
                break;              /* the rest of this tick is lost */
            }
        }
        if ((g->by >> 8) >= BK_FH)
        {
            ev |= BK_EV_LOST;
            if (--g->lives == 0)
            {
                g->phase = BK_PH_OVER;
                g->vx = g->vy = 0;
                g->by = (BK_FH - BK_BALL) << 8;
                return ev | BK_EV_OVER;
            }
            serve(g);
            return ev;
        }
        if ((ev & BK_EV_BRICK) && !bk_bricks_left(g))
        {
            bk_level(g, g->level + 1);
            return ev | BK_EV_CLEARED;
        }
    }
    return ev;
}

int bk_best_insert(uint32_t *best, int n, uint32_t s)
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
