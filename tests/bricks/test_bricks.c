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

/* Host test of the Bricks rules (apps/ipodui/games/bricks/bricks_core.c):
 * the layouts, the paddle and the serve, wall, brick (side and face) and
 * paddle bounces with their five angles, speed-ups, lost balls, the next
 * level, game over, the record check and the best scores */
#include <stdio.h>
#include <string.h>
#include "bricks_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* a ball in flight by hand, in a field with no bricks */
static void fly(struct bk_game *g, int x, int y, int vx, int vy)
{
    memset(g->brick, 0, sizeof(g->brick));
    g->brick[BK_ROWS - 1][BK_COLS - 1] = 1;     /* not cleared */
    g->phase = BK_PH_PLAY;
    g->bx = x << 8;
    g->by = y << 8;
    g->vx = vx * 256;
    g->vy = vy * 256;
}

static void test_layouts(void)
{
    struct bk_game g;
    int l;

    bk_new(&g, BK_SPEED_NORMAL, 3);
    CHECK(bk_valid(&g));
    CHECK(g.level == 1 && g.lives == 3 && g.phase == BK_PH_SERVE);
    CHECK(bk_bricks_left(&g) == BK_ROWS * BK_COLS);
    CHECK(g.brick[0][0] == 1 && g.brick[7][13] == 4);   /* four colours */
    for (l = 1; l <= BK_LAYOUTS; l++)
    {
        bk_level(&g, l);
        CHECK(bk_bricks_left(&g) > 0);
        CHECK(bk_valid(&g));
    }
    bk_level(&g, 2);
    l = bk_bricks_left(&g);
    bk_level(&g, 2 + BK_LAYOUTS);               /* they come round */
    CHECK(bk_bricks_left(&g) == l);
    bk_new(&g, 9, 0);                           /* out of range: clamped */
    CHECK(g.speed_set == BK_SPEED_NORMAL && g.lives == 1);
}

static void test_paddle_and_serve(void)
{
    struct bk_game g;

    bk_new(&g, BK_SPEED_NORMAL, 3);
    CHECK(bk_pad_x(&g) == (BK_FW - BK_PW) / 2);
    /* the ball rides on the paddle while serving */
    bk_paddle(&g, -30);
    CHECK(bk_pad_x(&g) == (BK_FW - BK_PW) / 2 - 30);
    CHECK(bk_ball_x(&g) == bk_pad_x(&g) + (BK_PW - BK_BALL) / 2);
    CHECK(bk_ball_y(&g) == BK_PY - BK_BALL);
    bk_paddle(&g, -1000);
    CHECK(bk_pad_x(&g) == 0);
    bk_paddle(&g, 1000);
    CHECK(bk_pad_x(&g) == BK_FW - BK_PW);
    CHECK(bk_tick(&g, 50) == 0);                /* nothing moves */
    CHECK(bk_launch(&g) == BK_EV_LAUNCH);
    CHECK(g.phase == BK_PH_PLAY && g.vx > 0 && g.vy < 0);
    CHECK(bk_launch(&g) == 0);
    /* the paddle no longer carries it */
    bk_paddle(&g, -20);
    CHECK(bk_ball_x(&g) != bk_pad_x(&g) + (BK_PW - BK_BALL) / 2);
}

static void test_walls(void)
{
    struct bk_game g;
    int ev;

    bk_new(&g, BK_SPEED_NORMAL, 3);
    fly(&g, 2, 120, -100, -10);
    ev = bk_tick(&g, 50);                       /* 5 px left: bounces */
    CHECK(ev & BK_EV_WALL);
    CHECK(g.vx > 0 && bk_ball_x(&g) == 3);
    fly(&g, BK_FW - BK_BALL - 1, 120, 100, -10);
    ev = bk_tick(&g, 50);
    CHECK(ev & BK_EV_WALL && g.vx < 0);
    CHECK(bk_ball_x(&g) == BK_FW - BK_BALL - 4);
    fly(&g, 100, 2, 0, -100);
    ev = bk_tick(&g, 50);
    CHECK(ev & BK_EV_WALL && g.vy > 0 && bk_ball_y(&g) == 3);
}

static void test_bricks(void)
{
    struct bk_game g;
    int ev, y0 = BK_BY + BK_ROWS * BK_BH;

    /* from below: the bottom row's brick goes, the ball turns down */
    bk_new(&g, BK_SPEED_NORMAL, 3);
    g.phase = BK_PH_PLAY;
    g.bx = (5 * BK_BW + 8) << 8;
    g.by = (y0 + 2) << 8;
    g.vx = 0;
    g.vy = -100 * 256;
    ev = bk_tick(&g, 50);
    CHECK(ev & BK_EV_BRICK);
    CHECK(g.brick[BK_ROWS - 1][5] == 0 && g.brick[BK_ROWS - 2][5] != 0);
    CHECK(g.vy > 0 && g.vx == 0);
    CHECK(g.score == (uint32_t)bk_points(BK_ROWS - 1));
    CHECK(g.hits == 1);
    CHECK(bk_points(0) == 40 && bk_points(7) == 10);

    /* from the side: a lone brick, the ball comes in level from the
     * left: it turns back and keeps its height */
    fly(&g, 3 * BK_BW - BK_BALL - 1, BK_BY + 2, 100, 0);
    g.brick[0][3] = 2;
    ev = bk_tick(&g, 50);
    CHECK(ev & BK_EV_BRICK && g.brick[0][3] == 0);
    CHECK(g.vx < 0 && g.vy == 0 && bk_ball_y(&g) == BK_BY + 2);

    /* straddling two bricks from below: both go, one bounce */
    fly(&g, 4 * BK_BW - 3, y0 + 1, 0, -100);
    g.brick[BK_ROWS - 1][3] = g.brick[BK_ROWS - 1][4] = 3;
    g.hits = 0;
    ev = bk_tick(&g, 50);
    CHECK(ev & BK_EV_BRICK);
    CHECK(!g.brick[BK_ROWS - 1][3] && !g.brick[BK_ROWS - 1][4]);
    CHECK(g.hits == 2 && g.vy > 0);
}

static void test_paddle_angles(void)
{
    struct bk_game g;
    int z, lastvx = -100000;

    /* the five zones, left to right: steeper in the middle, the sideways
     * part growing toward the ends */
    for (z = 0; z < 5; z++)
    {
        int px = 100, cx = px + z * BK_PW / 5 + BK_PW / 10;

        bk_new(&g, BK_SPEED_NORMAL, 3);
        fly(&g, cx - BK_BALL / 2, BK_PY - BK_BALL - 3, 1, 100);
        g.px = px << 8;
        CHECK(bk_tick(&g, 50) & BK_EV_PADDLE);
        CHECK(g.vy < 0 && bk_ball_y(&g) == BK_PY - BK_BALL);
        CHECK(g.vx > lastvx);
        lastvx = g.vx;
    }
    /* the middle keeps the incoming sense */
    bk_new(&g, BK_SPEED_NORMAL, 3);
    fly(&g, 100 + BK_PW / 2 - BK_BALL / 2, BK_PY - BK_BALL - 3, -1, 100);
    g.px = 100 << 8;
    CHECK(bk_tick(&g, 50) & BK_EV_PADDLE);
    CHECK(g.vx < 0);
    /* past the paddle's end: missed */
    bk_new(&g, BK_SPEED_NORMAL, 3);
    fly(&g, 10, BK_PY - BK_BALL - 3, 0, 100);
    g.px = 100 << 8;
    CHECK(!(bk_tick(&g, 50) & BK_EV_PADDLE));
}

static void test_speed(void)
{
    struct bk_game g;
    int s1, s2;

    bk_new(&g, BK_SPEED_SLOW, 3);
    s1 = bk_speed(&g);
    bk_new(&g, BK_SPEED_FAST, 3);
    CHECK(bk_speed(&g) > s1);
    s1 = bk_speed(&g);
    g.hits = BK_SPEEDUP_HITS;
    s2 = bk_speed(&g);
    CHECK(s2 > s1);
    bk_level(&g, 2);                            /* hits reset, level up */
    CHECK(g.hits == 0 && bk_speed(&g) > s1);
    bk_level(&g, 1 + BK_LAYOUTS);               /* round two: faster */
    CHECK(bk_speed(&g) > s2);
    g.level = 200;
    CHECK(bk_speed(&g) == BK_SPEED_MAX);
    /* a launch goes at the current speed */
    bk_new(&g, BK_SPEED_NORMAL, 3);
    bk_launch(&g);
    s1 = (int)((int64_t)g.vx * g.vx / 65536 + (int64_t)g.vy * g.vy / 65536);
    s2 = bk_speed(&g);
    CHECK(s1 > s2 * s2 * 95 / 100 && s1 < s2 * s2 * 105 / 100);
}

static void test_lost_and_over(void)
{
    struct bk_game g;
    int ev;

    bk_new(&g, BK_SPEED_NORMAL, 2);
    fly(&g, 10, BK_FH - 3, 0, 100);
    ev = bk_tick(&g, 50);
    CHECK(ev & BK_EV_LOST);
    CHECK(!(ev & BK_EV_OVER));
    CHECK(g.lives == 1 && g.phase == BK_PH_SERVE);
    CHECK(bk_ball_y(&g) == BK_PY - BK_BALL);
    fly(&g, 10, BK_FH - 3, 0, 100);
    ev = bk_tick(&g, 50);
    CHECK(ev & BK_EV_OVER);
    CHECK(g.lives == 0 && g.phase == BK_PH_OVER);
    CHECK(bk_valid(&g));
    CHECK(bk_tick(&g, 50) == 0 && bk_launch(&g) == 0);
}

static void test_cleared(void)
{
    struct bk_game g;
    int ev;

    bk_new(&g, BK_SPEED_NORMAL, 3);
    fly(&g, 13 * BK_BW + 8, BK_BY + BK_ROWS * BK_BH + 2, 0, -100);
    g.score = 0;
    ev = bk_tick(&g, 50);                       /* the last brick */
    CHECK(ev & BK_EV_BRICK && ev & BK_EV_CLEARED);
    CHECK(g.level == 2 && g.phase == BK_PH_SERVE && g.lives == 3);
    CHECK(bk_bricks_left(&g) > 0 && g.score == 10);
}

static void test_valid(void)
{
    struct bk_game g, t;

    bk_new(&g, BK_SPEED_NORMAL, 3);
    t = g; t.ver = 2;               CHECK(!bk_valid(&t));
    t = g; t.brick[0][0] = 7;       CHECK(!bk_valid(&t));
    t = g; t.px = -256;            CHECK(!bk_valid(&t));
    t = g; t.level = 0;             CHECK(!bk_valid(&t));
    t = g; t.lives = 0;             CHECK(!bk_valid(&t));  /* not over */
    t = g; t.vx = 1000 << 8;        CHECK(!bk_valid(&t));
}

static void test_long_run(void)
{
    struct bk_game g;
    int i, ev;

    /* a paddle that follows the ball never loses it, and the ball never
     * leaves the field */
    bk_new(&g, BK_SPEED_FAST, 3);
    bk_launch(&g);
    for (i = 0; i < 20000 && g.phase != BK_PH_OVER; i++)
    {
        int want = bk_ball_x(&g) + BK_BALL / 2 - BK_PW / 2 - (i % 7) * 3;

        bk_paddle(&g, want - bk_pad_x(&g));
        ev = bk_tick(&g, 33);
        CHECK(!(ev & BK_EV_LOST));
        CHECK(bk_valid(&g));
        if (g.phase == BK_PH_SERVE)
            bk_launch(&g);
        if (fails)
            break;
    }
    CHECK(g.level > 1);                         /* it cleared a level */
}

/* Paddle Speed: clicks to pixels */
static void test_wheel_map(void)
{
    struct bk_game g;
    int lv, v, n;
    unsigned vs[] = { 0, 5, 20, 40, 400 };

    CHECK(BK_PADDLE_DEF == 1);
    for (lv = 0; lv < BK_PADDLE_N; lv++)
    {
        int prev_px = 0;

        for (v = 0; v < 5; v++)
        {
            int last = 0;

            for (n = 0; n <= 40; n++)
            {
                int p = bk_wheel_px(lv, n, vs[v]);

                CHECK(p >= last);                       /* monotonic */
                CHECK(bk_wheel_px(lv, -n, vs[v]) == -p); /* symmetric */
                last = p;
            }
            CHECK(bk_wheel_px(lv, 1, vs[v]) >= prev_px); /* in velocity */
            prev_px = bk_wheel_px(lv, 1, vs[v]);
        }
        if (lv)                                         /* in the level */
            for (v = 0; v < 5; v++)
                CHECK(bk_wheel_px(lv, 3, vs[v])
                      > bk_wheel_px(lv - 1, 3, vs[v]));
        /* a fast spin caps (no growth past the cap velocity) */
        CHECK(bk_wheel_px(lv, 5, 40) == bk_wheel_px(lv, 5, 4000));
        CHECK(bk_wheel_px(lv, 0, 30) == 0);
    }
    CHECK(bk_wheel_px(99, 1, 0) == bk_wheel_px(BK_PADDLE_N - 1, 1, 0));
    /* clicks to cross the 260 px of travel, printed per level */
    for (lv = 0; lv < BK_PADDLE_N; lv++)
        for (v = 1; v < 4; v++)
        {
            n = 0;
            bk_new(&g, BK_SPEED_NORMAL, 3);
            bk_paddle(&g, -1000);
            while (bk_pad_x(&g) < BK_FW - BK_PW && n < 500)
            {
                bk_paddle(&g, bk_wheel_px(lv, 1, vs[v]));
                n++;
            }
            printf("  level %d at %2u clicks/s: %2d clicks to cross\n", lv,
                   vs[v], n);
            CHECK(n >= 1);
        }
    /* Normal: about half a turn (12 clicks) at a brisk 20 clicks/s */
    bk_new(&g, BK_SPEED_NORMAL, 3);
    bk_paddle(&g, -1000);
    for (n = 0; n < 12; n++)
        bk_paddle(&g, bk_wheel_px(1, 1, 20));
    CHECK(bk_pad_x(&g) >= 250);
    /* clamped at both edges, no overshoot: the paddle stays in range */
    for (n = 0; n < 200; n++)
    {
        bk_paddle(&g, bk_wheel_px(4, n & 1 ? 7 : -7, 40));
        CHECK(bk_pad_x(&g) >= 0 && bk_pad_x(&g) <= BK_FW - BK_PW);
    }
    bk_paddle(&g, bk_wheel_px(4, 50, 40));
    CHECK(bk_pad_x(&g) == BK_FW - BK_PW);
    bk_paddle(&g, bk_wheel_px(4, -50, 40));
    CHECK(bk_pad_x(&g) == 0);
}

int main(void)
{
    test_layouts();
    test_paddle_and_serve();
    test_walls();
    test_bricks();
    test_paddle_angles();
    test_speed();
    test_lost_and_over();
    test_cleared();
    test_valid();
    test_long_run();
    test_wheel_map();
    printf("bricks: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
