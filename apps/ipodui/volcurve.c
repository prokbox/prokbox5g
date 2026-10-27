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
 * iPod UI shell
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

/* The volume curve (docs/phase15-brief.md section 3.4). One wheel click is
 * one step; the bar fills linearly in steps, so it follows loudness rather
 * than raw dB (today's 1 dB clicks over -90..+6 left the bottom half of the
 * bar all but silent).
 *
 * The rule, in whole dB (SOUND_VOLUME has no decimals):
 *   step 0        mute (VOL_DB_MUTE)
 *   steps 17..47  -24 .. +6 dB, 1 dB each (31 steps: where listening is)
 *   steps 1..16   -60 .. -25 dB; the gaps below -24 grow toward the
 *                 bottom: 1 dB just below -24, at most 4 dB near -60
 *                 (16 gaps from -60 to -24: 4x1, 6x2, 4x3, 2x4 = 36 dB)
 * tests/volcurve/ checks monotonicity, the end points, the gap rule and
 * that no two steps share a value. */

#include "ipodui/volcurve.h"

static const signed char vol_db[VOL_STEPS] = {
    VOL_DB_MUTE,
    -60, -56, -52, -49, -46, -43, -40, -38,         /* steps  1..8  */
    -36, -34, -32, -30, -28, -27, -26, -25,         /* steps  9..16 */
    -24, -23, -22, -21, -20, -19, -18, -17,         /* steps 17..24 */
    -16, -15, -14, -13, -12, -11, -10,  -9,         /* steps 25..32 */
     -8,  -7,  -6,  -5,  -4,  -3,  -2,  -1,         /* steps 33..40 */
      0,   1,   2,   3,   4,   5,   6,              /* steps 41..47 */
};

_Static_assert(sizeof(vol_db) == VOL_STEPS, "48 volume steps");

int vol_step_to_db(int step)
{
    if (step <= 0)
        return VOL_DB_MUTE;
    if (step >= VOL_STEPS)
        step = VOL_STEPS - 1;
    return vol_db[step];
}

int vol_db_to_step(int db)
{
    int step = VOL_STEPS - 1;

    while (step > 0 && vol_db[step] > db)
        step--;
    return step;
}

int vol_step_to_bar1024(int step)
{
    if (step <= 0)
        return 0;
    if (step >= VOL_STEPS - 1)
        return 1024;
    return step * 1024 / (VOL_STEPS - 1);
}

int vol_step_add(int db, int clicks)
{
    int step = vol_db_to_step(db);

    /* off the table (a volume saved before the curve): the snap down is
     * itself the first click down, so -1 lands on the step below db */
    if (clicks < 0 && step > 0 && vol_db[step] != db)
        clicks++;
    step += clicks;
    if (step < 0)
        step = 0;
    if (step > VOL_STEPS - 1)
        step = VOL_STEPS - 1;
    return step;
}
