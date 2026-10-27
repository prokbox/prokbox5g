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

/* Host test of the Chute rules
 * (apps/ipodui/games/chute/chute_core.c): the boat, the beat,
 * catches, misses and splashes, forgiveness, game over, one jumper a
 * step, the speeds, the record check and the best scores */
#include <stdio.h>
#include <string.h>
#include "chute_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define LAST (1u << (PC_STEPS - 1))

static int one_beat(struct pc_game *g)
{
    return pc_tick(g, pc_beat_ms(g) - g->beat_acc);
}

static void test_new_and_boat(void)
{
    struct pc_game g;

    pc_new(&g, 1, PC_SPEED_NORMAL);
    CHECK(pc_valid(&g));
    CHECK(g.boat == 1 && g.score == 0 && g.misses == 0);
    CHECK(!g.occ[0] && !g.occ[1] && !g.occ[2]);
    CHECK(pc_move(&g, -1) == PC_EV_MOVED && g.boat == 0);
    CHECK(pc_move(&g, -1) == 0 && g.boat == 0);
    CHECK(pc_move(&g, 1) && pc_move(&g, 1) && g.boat == 2);
    CHECK(pc_move(&g, 1) == 0 && g.boat == 2);
    pc_new(&g, 1, 7);
    CHECK(g.speed == PC_SPEED_NORMAL);
    pc_new(&g, 0, PC_SPEED_SLOW);               /* seed 0 still works */
    CHECK(pc_valid(&g));
}

static void test_beat(void)
{
    struct pc_game g;
    unsigned b;
    int ev;

    pc_new(&g, 99, PC_SPEED_NORMAL);
    b = pc_beat_ms(&g);
    CHECK(pc_tick(&g, b - 1) == 0);
    ev = pc_tick(&g, 1);
    CHECK(ev & PC_EV_BEAT);
    CHECK(ev & PC_EV_SPAWN);                    /* the first beat brings one */
    CHECK((g.occ[0] | g.occ[1] | g.occ[2]) == 1);
    /* a jumper steps down one a beat */
    ev = one_beat(&g);
    CHECK((g.occ[0] | g.occ[1] | g.occ[2]) & 2);
    /* the same seed, the same game */
    {
        struct pc_game a, c;
        int i;

        pc_new(&a, 1234, PC_SPEED_FAST);
        pc_new(&c, 1234, PC_SPEED_FAST);
        for (i = 0; i < 200; i++)
        {
            one_beat(&a);
            one_beat(&c);
        }
        CHECK(!memcmp(&a, &c, sizeof(a)));
    }
}

static void test_catch_and_miss(void)
{
    struct pc_game g;
    int ev;

    pc_new(&g, 5, PC_SPEED_NORMAL);
    g.beats = 10;                               /* past the sure spawns */
    g.occ[1] = LAST;
    g.boat = 1;
    ev = one_beat(&g);
    CHECK(ev & PC_EV_CATCH);
    CHECK(!(ev & PC_EV_MISS));
    CHECK(g.score == 1 && g.misses == 0 && g.splash == 0);
    CHECK(!(g.occ[1] & LAST));

    g.occ[0] = LAST;
    g.boat = 2;
    ev = one_beat(&g);
    CHECK(ev & PC_EV_MISS);
    CHECK(g.misses == 1 && g.splash == 1 && g.score == 1);
    ev = one_beat(&g);                          /* the splash lasts a beat */
    CHECK(g.splash == 0);

    /* a catch at 50 forgives a miss */
    g.score = PC_FORGIVE - 1;
    g.occ[2] = LAST;
    g.occ[0] = g.occ[1] = 0;
    ev = one_beat(&g);
    CHECK(ev & PC_EV_FORGIVE);
    CHECK(g.misses == 0 && g.score == PC_FORGIVE);
}

static void test_over(void)
{
    struct pc_game g;
    int i, ev = 0;

    pc_new(&g, 5, PC_SPEED_NORMAL);
    g.beats = 10;
    for (i = 0; i < PC_MISSES; i++)
    {
        memset(g.occ, 0, sizeof(g.occ));
        g.occ[0] = LAST;
        g.boat = 2;
        ev = one_beat(&g);
        CHECK(ev & PC_EV_MISS);
    }
    CHECK(ev & PC_EV_OVER);
    CHECK(g.phase == PC_PH_OVER && g.misses == PC_MISSES);
    CHECK(pc_valid(&g));
    CHECK(pc_tick(&g, 5000) == 0 && pc_move(&g, -1) == 0);
}

static void test_one_a_step(void)
{
    struct pc_game g;
    int i, lands = 0, catches = 0;

    /* a boat that always waits under the next one to land never misses,
     * and never two land on one beat */
    pc_new(&g, 777, PC_SPEED_FAST);
    for (i = 0; i < 5000 && g.phase == PC_PH_PLAY; i++)
    {
        int l, n = 0, ev;

        for (l = 0; l < PC_LANES; l++)
            if (g.occ[l] & LAST)
            {
                n++;
                while (g.boat != l)
                    pc_move(&g, l < g.boat ? -1 : 1);
            }
        CHECK(n <= 1);
        lands += n;
        ev = one_beat(&g);
        CHECK(!(ev & PC_EV_MISS));
        catches += (ev & PC_EV_CATCH) != 0;
        CHECK(pc_valid(&g));
        if (fails)
            break;
    }
    CHECK(lands == catches && catches > 1000);
    CHECK(g.score == (uint32_t)catches);
}

static void test_speeds(void)
{
    struct pc_game g;
    unsigned s, n, f;

    pc_new(&g, 1, PC_SPEED_SLOW);
    s = pc_beat_ms(&g);
    g.speed = PC_SPEED_NORMAL;
    n = pc_beat_ms(&g);
    g.speed = PC_SPEED_FAST;
    f = pc_beat_ms(&g);
    CHECK(s > n && n > f);
    g.score = 100;
    CHECK(pc_beat_ms(&g) < f);
    g.score = 100000;
    CHECK(pc_beat_ms(&g) >= PC_BEAT_MIN);
    g.speed = PC_SPEED_SLOW;
    g.score = 0;
    CHECK(pc_tick(&g, 100000) & PC_EV_BEAT);    /* a long gap is cut */
    CHECK(g.beats == PC_DT_MAX / s);
}

static void test_valid(void)
{
    struct pc_game g, t;

    pc_new(&g, 3, PC_SPEED_NORMAL);
    t = g; t.ver = 0;               CHECK(!pc_valid(&t));
    t = g; t.boat = 3;              CHECK(!pc_valid(&t));
    t = g; t.occ[0] = 1u << PC_STEPS; CHECK(!pc_valid(&t));
    t = g; t.occ[0] = 4; t.occ[2] = 4; CHECK(!pc_valid(&t));
    t = g; t.misses = PC_MISSES;    CHECK(!pc_valid(&t));  /* not over */
    t = g; t.speed = 3;             CHECK(!pc_valid(&t));
}

static void test_best(void)
{
    uint32_t b[3] = { 0 };

    CHECK(pc_best_insert(b, 3, 5) == 0);
    CHECK(pc_best_insert(b, 3, 9) == 0);
    CHECK(pc_best_insert(b, 3, 7) == 1 && b[2] == 5);
    CHECK(pc_best_insert(b, 3, 1) == -1);
}

int main(void)
{
    test_new_and_boat();
    test_beat();
    test_catch_and_miss();
    test_over();
    test_one_a_step();
    test_speeds();
    test_valid();
    test_best();
    printf("chute: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
