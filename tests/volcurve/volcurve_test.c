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
 * Host test for the shell's volume curve (apps/ipodui/volcurve.c,
 * docs/phase15-brief.md sections 2.3, 3.4, 7): monotonic, the end points,
 * the gap rule, no two steps on the same dB, the snap-down inverse and the
 * bar mapping. Prints the table (the hand-back's volume table).
 *
 *   cc -Iapps tests/volcurve/volcurve_test.c apps/ipodui/volcurve.c
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
#include <stdio.h>
#include <string.h>
#include "ipodui/volcurve.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; \
    printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv)
{
    int s, db;

    CHECK(VOL_STEPS == 48, "48 steps");
    CHECK(vol_step_to_db(0) == VOL_DB_MUTE, "step 0 is mute");
    CHECK(vol_step_to_db(1) == -60, "step 1 is -60 dB");
    CHECK(vol_step_to_db(17) == -24, "step 17 is -24 dB");
    CHECK(vol_step_to_db(VOL_STEPS - 1) == 6, "step 47 is +6 dB");
    CHECK(vol_step_to_db(-3) == VOL_DB_MUTE && vol_step_to_db(99) == 6,
          "out-of-range steps clamp");

    /* strictly increasing = monotonic and no shared value */
    for (s = 1; s < VOL_STEPS; s++)
        CHECK(vol_step_to_db(s) > vol_step_to_db(s - 1),
              "step %d (%d dB) not above step %d", s, vol_step_to_db(s), s - 1);

    /* 1 dB each over the top: steps 17..47 */
    for (s = 18; s < VOL_STEPS; s++)
        CHECK(vol_step_to_db(s) - vol_step_to_db(s - 1) == 1,
              "step %d: gap %d, want 1 dB", s,
              vol_step_to_db(s) - vol_step_to_db(s - 1));

    /* below -24: gaps 1..4 dB, never shrinking toward the bottom, 1 dB
     * just below -24, 16 gaps from -60 to -24 */
    int prev = 1, sum = 0;
    for (s = 17; s > 1; s--)
    {
        int gap = vol_step_to_db(s) - vol_step_to_db(s - 1);
        CHECK(gap >= 1 && gap <= 4, "step %d: gap %d out of 1..4", s, gap);
        CHECK(gap >= prev, "step %d: gap %d shrinks toward the bottom", s, gap);
        if (s == 17)
            CHECK(gap == 1, "gap just below -24 is %d, want 1", gap);
        prev = gap;
        sum += gap;
    }
    CHECK(sum == 36, "gaps from -60 to -24 sum to %d", sum);

    /* the inverse: exact on the table, snaps down (never louder) between */
    for (s = 0; s < VOL_STEPS; s++)
        CHECK(vol_db_to_step(vol_step_to_db(s)) == s, "round trip step %d", s);
    for (db = -100; db <= 10; db++)
    {
        s = vol_db_to_step(db);
        CHECK(s >= 0 && s < VOL_STEPS, "db %d -> step %d", db, s);
        CHECK(s == 0 || vol_step_to_db(s) <= db, "db %d snaps up to %d",
              db, vol_step_to_db(s));
        CHECK(s == VOL_STEPS - 1 || vol_step_to_db(s + 1) > db,
              "db %d snaps below the nearest step", db);
    }
    CHECK(vol_db_to_step(-25) == 16 && vol_db_to_step(-37) == 8 &&
          vol_db_to_step(-61) == 0, "snap examples");

    /* wheel moves: on the table one click is one step; off it the snap
     * down is the first click down, and an up click goes to the next step */
    CHECK(vol_step_add(-24, 1) == 18 && vol_step_add(-24, -1) == 16,
          "on-table clicks");
    CHECK(vol_step_add(-37, -1) == 8 && vol_step_add(-37, 1) == 9 &&
          vol_step_add(-37, -2) == 7, "off-table clicks (-37 dB)");
    CHECK(vol_step_add(-90, -1) == 0 && vol_step_add(-75, -1) == 0 &&
          vol_step_add(-75, 1) == 1, "clicks near mute");
    CHECK(vol_step_add(6, 5) == 47 && vol_step_add(-90, -5) == 0 &&
          vol_step_add(6, -47) == 0, "clicks clamp");
    CHECK(vol_step_add(-25, 0) == 16, "zero clicks");

    /* the bar: 0 .. 1024, linear in step */
    CHECK(vol_step_to_bar1024(0) == 0 && vol_step_to_bar1024(47) == 1024,
          "bar end points");
    for (s = 1; s < VOL_STEPS; s++)
    {
        int d = vol_step_to_bar1024(s) - vol_step_to_bar1024(s - 1);
        CHECK(d == 21 || d == 22, "bar step %d: %d, want even", s, d);
    }

    if (argc > 1 && !strcmp(argv[1], "-v"))
        for (s = 0; s < VOL_STEPS; s++)
            printf("step %2d  %4d dB  bar %4d\n", s, vol_step_to_db(s),
                   vol_step_to_bar1024(s));
    if (!fails)
        printf("ok   volcurve: 48 steps, mute..+6 dB, gap rule, snap down, "
               "even bar\n");
    return fails != 0;
}
