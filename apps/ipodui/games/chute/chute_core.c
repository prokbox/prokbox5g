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

/* Chute rules: see chute_core.h */
#include <string.h>
#include "chute_core.h"

static const uint16_t beat_base[PC_SPEED__N] = { 760, 600, 460 };

static uint32_t rnd(struct pc_game *g)
{
    uint32_t x = g->seed;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->seed = x;
    return x;
}

unsigned pc_beat_ms(const struct pc_game *g)
{
    unsigned b = beat_base[g->speed < PC_SPEED__N ? g->speed : 1];
    unsigned s = g->score > 200 ? 200 : g->score;

    /* 2 % quicker every 10 points, to a floor */
    b = b * (100 - s / 5) / 100;
    return b < PC_BEAT_MIN ? PC_BEAT_MIN : b;
}

void pc_new(struct pc_game *g, uint32_t seed, int speed)
{
    memset(g, 0, sizeof(*g));
    g->ver = PC_VER;
    g->phase = PC_PH_PLAY;
    g->speed = (uint8_t)(speed >= 0 && speed < PC_SPEED__N
                         ? speed : PC_SPEED_NORMAL);
    g->boat = 1;
    g->seed = seed ? seed : 0x2545f491u;
}

bool pc_valid(const struct pc_game *g)
{
    int l;
    uint8_t any = 0;

    if (g->ver != PC_VER || g->phase > PC_PH_OVER
        || g->speed >= PC_SPEED__N || g->boat >= PC_LANES
        || g->misses > PC_MISSES || g->splash > PC_LANES || !g->seed
        || (g->misses == PC_MISSES) != (g->phase == PC_PH_OVER))
        return false;
    for (l = 0; l < PC_LANES; l++)
    {
        if (g->occ[l] >> PC_STEPS)
            return false;
        if (g->occ[l] & any)
            return false;           /* two on one step */
        any |= g->occ[l];
    }
    return true;
}

int pc_move(struct pc_game *g, int dir)
{
    int b = g->boat + (dir < 0 ? -1 : 1);

    if (g->phase != PC_PH_PLAY || b < 0 || b >= PC_LANES)
        return 0;
    g->boat = (uint8_t)b;
    return PC_EV_MOVED;
}

static int beat(struct pc_game *g)
{
    int l, ev = PC_EV_BEAT;
    uint8_t top = 0;

    g->beats++;
    g->splash = 0;
    for (l = 0; l < PC_LANES; l++)
    {
        if (g->occ[l] & (1u << (PC_STEPS - 1)))
        {
            /* it reaches the water this beat */
            if (g->boat == l)
            {
                g->score++;
                ev |= PC_EV_CATCH;
                if (g->score % PC_FORGIVE == 0 && g->misses)
                {
                    g->misses--;
                    ev |= PC_EV_FORGIVE;
                }
            }
            else
            {
                g->misses++;
                g->splash = (uint8_t)(l + 1);
                ev |= PC_EV_MISS;
            }
        }
        g->occ[l] = (uint8_t)((g->occ[l] << 1) & ((1u << PC_STEPS) - 1));
        top |= g->occ[l] & 1;
    }
    if (g->misses >= PC_MISSES)
    {
        g->misses = PC_MISSES;
        g->phase = PC_PH_OVER;
        return ev | PC_EV_OVER;
    }
    /* a new jumper on the top step (only when it is free, so there is
     * one jumper a step at most), more often as the score grows; the
     * first two beats always bring one */
    if (!top)
    {
        unsigned p = 40 + (g->score > 120 ? 120 : g->score) / 3;

        if (g->beats < 2 || rnd(g) % 100 < p)
        {
            g->occ[rnd(g) % PC_LANES] |= 1;
            ev |= PC_EV_SPAWN;
        }
    }
    return ev;
}

int pc_tick(struct pc_game *g, unsigned dt)
{
    int ev = 0;
    unsigned b;

    if (g->phase != PC_PH_PLAY)
        return 0;
    if (dt > PC_DT_MAX)
        dt = PC_DT_MAX;
    g->beat_acc = (uint16_t)(g->beat_acc + dt);
    b = pc_beat_ms(g);
    while (g->beat_acc >= b && g->phase == PC_PH_PLAY)
    {
        g->beat_acc = (uint16_t)(g->beat_acc - b);
        ev |= beat(g);
        b = pc_beat_ms(g);
    }
    return ev;
}

int pc_best_insert(uint32_t *best, int n, uint32_t s)
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
