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
 * Host test of the second core's job channel (Fit-2,
 * firmware/common/cop_job_core.c): the slot ring's order and states, and
 * the cache operations at each hand-off.
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
#include "cop_job_core.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)

static void test_order(void)
{
    struct copj_ring r;
    int s[COPJ_SLOTS], i;

    copj_ring_init(&r);
    CHECK(copj_take(&r) == -1);             /* nothing queued */
    CHECK(copj_pending(&r) == 0);

    /* A claimed but uncommitted slot is invisible to the worker */
    s[0] = copj_claim(&r);
    CHECK(s[0] == 0);
    CHECK(copj_take(&r) == -1);
    CHECK(copj_claim(&r) == -1);            /* one claim at a time */
    copj_commit(&r, s[0]);
    CHECK(copj_pending(&r) == 1);

    for (i = 1; i < COPJ_SLOTS; i++)
    {
        s[i] = copj_claim(&r);
        CHECK(s[i] == i);
        copj_commit(&r, s[i]);
    }
    CHECK(copj_claim(&r) == -1);            /* full */

    /* FIFO */
    for (i = 0; i < COPJ_SLOTS; i++)
        CHECK(copj_take(&r) == i);
    CHECK(copj_take(&r) == -1);

    /* Out-of-order finish is fine; the poster collects each */
    copj_finish(&r, 2);
    CHECK(copj_is_done(&r, 2));
    CHECK(!copj_is_done(&r, 0));
    CHECK(!copj_release(&r, 0));            /* not done: unchanged */
    copj_finish(&r, 0);
    CHECK(copj_release(&r, 0));
    CHECK(!copj_release(&r, 0));            /* twice: no */

    /* The ring wraps only past released slots, in order */
    s[0] = copj_claim(&r);
    CHECK(s[0] == 0);
    copj_commit(&r, s[0]);
    CHECK(copj_claim(&r) == -1);            /* slot 1 still running */
    copj_finish(&r, 1);
    CHECK(copj_release(&r, 1));
    CHECK(copj_claim(&r) == 1);
}

static void test_bad_input(void)
{
    struct copj_ring r;

    copj_ring_init(&r);
    copj_commit(&r, 3);                     /* never claimed */
    CHECK(copj_take(&r) == -1);
    copj_finish(&r, -1);
    copj_finish(&r, COPJ_SLOTS);
    CHECK(!copj_is_done(&r, -1));
    CHECK(!copj_is_done(&r, COPJ_SLOTS));
    copj_finish(&r, 0);                     /* not running */
    CHECK(!copj_is_done(&r, 0));
}

static void test_cache_rules(void)
{
    unsigned f;
    int p;

    /* Same core: never anything */
    for (f = 0; f < 16; f++)
        for (p = COPJ_POST; p <= COPJ_RETURN; p++)
            CHECK(copj_cache_ops(f, (enum copj_phase)p, false) == 0);

    /* A job on uncached data only (IRAM, the uncached alias) needs no
     * maintenance at either end */
    CHECK(copj_cache_ops(0, COPJ_POST, true) == 0);
    CHECK(copj_cache_ops(0, COPJ_START, true) == 0);
    CHECK(copj_cache_ops(0, COPJ_FINISH, true) == 0);
    CHECK(copj_cache_ops(0, COPJ_COLLECT, true) == 0);

    /* Input: poster writes back, worker writes back and drops */
    CHECK(copj_cache_ops(COPJ_IN, COPJ_POST, true) == COPJ_C_COMMIT);
    CHECK(copj_cache_ops(COPJ_IN, COPJ_START, true) == COPJ_C_COMMIT_DISCARD);
    CHECK(copj_cache_ops(COPJ_IN, COPJ_FINISH, true) == 0);
    CHECK(copj_cache_ops(COPJ_IN, COPJ_COLLECT, true) == 0);

    /* Output: the mirror image */
    CHECK(copj_cache_ops(COPJ_OUT, COPJ_POST, true) == 0);
    CHECK(copj_cache_ops(COPJ_OUT, COPJ_START, true) == 0);
    CHECK(copj_cache_ops(COPJ_OUT, COPJ_FINISH, true) == COPJ_C_COMMIT);
    CHECK(copj_cache_ops(COPJ_OUT, COPJ_COLLECT, true)
          == COPJ_C_COMMIT_DISCARD);

    /* Code: written back by the loader, instructions dropped too */
    CHECK(copj_cache_ops(COPJ_CODE, COPJ_POST, true) == COPJ_C_COMMIT);
    CHECK(copj_cache_ops(COPJ_CODE, COPJ_START, true)
          == COPJ_C_COMMIT_DISCARD_I);
    CHECK(copj_cache_ops(COPJ_CODE | COPJ_IN, COPJ_START, true)
          == COPJ_C_COMMIT_DISCARD_I);

    /* Calls: always write back before; drop after only if written */
    CHECK(copj_cache_ops(0, COPJ_CALL, true) == COPJ_C_COMMIT);
    CHECK(copj_cache_ops(0, COPJ_RETURN, true) == 0);
    CHECK(copj_cache_ops(COPJ_CALL_WROTE, COPJ_RETURN, true)
          == COPJ_C_COMMIT_DISCARD);

    /* Never a bare discard: every drop writes back first (a discard alone
     * would lose the core's own dirty lines, its stack among them) */
    for (f = 0; f < 16; f++)
        for (p = COPJ_POST; p <= COPJ_RETURN; p++)
        {
            unsigned ops = copj_cache_ops(f, (enum copj_phase)p, true);
            CHECK((ops & ~(COPJ_C_COMMIT | COPJ_C_COMMIT_DISCARD
                           | COPJ_C_COMMIT_DISCARD_I)) == 0);
        }
}

int main(void)
{
    test_order();
    test_bad_input();
    test_cache_rules();
    if (fails)
        fprintf(stderr, "%d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
