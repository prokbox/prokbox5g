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

/* Chute rules (pure, host-tested in tests/chute): a handheld
 * LCD game in spirit. Jumpers leave a plane and come down three lanes
 * in fixed steps, all on one beat; the player rows a boat between the
 * lanes to catch each one as it reaches the water. A catch scores a
 * point; a miss is a splash, and the third miss ends the game (every
 * 50 points forgives one). The beat quickens with the score. At most one
 * jumper is on each step, so two never land on the same beat. No SDK
 * or Rockbox headers: the game (chute.c) draws it. */
#ifndef CHUTE_CORE_H
#define CHUTE_CORE_H

#include <stdint.h>
#include <stdbool.h>

#define PC_LANES        3
#define PC_STEPS        6           /* positions down a lane; the last
                                       is just above the water */
#define PC_MISSES       3
#define PC_FORGIVE      50          /* points that forgive a miss */
#define PC_BEAT_MIN     240         /* ms: the fastest beat */
#define PC_DT_MAX       1000
#define PC_VER          1

enum { PC_SPEED_SLOW, PC_SPEED_NORMAL, PC_SPEED_FAST, PC_SPEED__N };
enum { PC_PH_PLAY, PC_PH_OVER };

#define PC_EV_BEAT      0x01        /* the jumpers stepped */
#define PC_EV_SPAWN     0x02        /* one left the plane */
#define PC_EV_CATCH     0x04
#define PC_EV_MISS      0x08
#define PC_EV_OVER      0x10
#define PC_EV_MOVED     0x20        /* the boat moved */
#define PC_EV_FORGIVE   0x40        /* a miss was forgiven */

struct pc_game {
    uint8_t  ver, phase, speed, boat;
    uint8_t  misses, splash;        /* splash: 1 + lane, 0 none */
    uint8_t  occ[PC_LANES];         /* bit p: a jumper on step p */
    uint8_t  pad;
    uint16_t beat_acc;
    uint32_t score;
    uint32_t seed;
    uint32_t beats;
};

void pc_new(struct pc_game *g, uint32_t seed, int speed);
bool pc_valid(const struct pc_game *g);
/* the boat a lane left (-1) or right (+1) */
int  pc_move(struct pc_game *g, int dir);
int  pc_tick(struct pc_game *g, unsigned dt_ms);
/* the beat now, in ms */
unsigned pc_beat_ms(const struct pc_game *g);
int  pc_best_insert(uint32_t *best, int n, uint32_t s);

#endif /* CHUTE_CORE_H */
