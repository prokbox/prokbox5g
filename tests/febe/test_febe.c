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
 *  Host test of the front end / back end boundary (P19 S1):
 * apps/ipodui/fe_be_core.c, plain memory, real threads.
 *
 * Copyright (C) 2026 prokopto-dev
 * Copyright (C) 2026 prokopto-dev
 *
 * Portions derived from Rockbox (rockbox.org).
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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fe_be_core.h"
#include "cop_job_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- the slot's states and single writers ---------------------------- */
static void test_slot(void)
{
    static struct febe_slot s;      /* static: word aligned, zeroed */
    uint32_t a[3] = { 7, 8, 9 }, out[3] = { 0 }, ret = 0;
    bool bad;

    CHECK(!febe_slot_done(&s));
    CHECK(!febe_slot_take(&s, &bad));               /* nothing filled */
    CHECK(febe_slot_fill(&s, 1, 42, 0, a, 3));
    CHECK(!febe_slot_fill(&s, 2, 42, 0, a, 3));     /* busy: refused */
    CHECK(!febe_slot_collect(&s, out, 3, &ret));    /* not done */
    CHECK(febe_slot_take(&s, &bad) && !bad);
    CHECK(!febe_slot_take(&s, &bad));               /* taken once */
    s.arg[0] = 70;                                   /* the BE's output */
    febe_slot_finish(&s, 5);
    CHECK(febe_slot_done(&s));
    CHECK(febe_slot_collect(&s, out, 3, &ret));
    CHECK(out[0] == 70 && out[1] == 8 && out[2] == 9 && ret == 5);
    CHECK(s.state == FEBE_SLOT_FREE);
    /* too many arguments: refused, nothing changes */
    CHECK(!febe_slot_fill(&s, 3, 1, 0, a, FEBE_ARG_WORDS + 1));
    CHECK(s.state == FEBE_SLOT_FREE);
    /* a torn record (canary) is still answered, flagged */
    CHECK(febe_slot_fill(&s, 4, 9, 0, a, 3));
    s.arg[2] ^= 1;
    CHECK(febe_slot_take(&s, &bad) && bad);
    febe_slot_finish(&s, FEBE_RET_BADSUM);
    CHECK(febe_slot_collect(&s, out, 0, &ret) && ret == FEBE_RET_BADSUM);
    /* finish out of order does nothing */
    febe_slot_finish(&s, 1);
    CHECK(s.state == FEBE_SLOT_FREE);
}

/* ---- the cache operations: the cop_job table, turned around ---------- */
static void test_ops(void)
{
    CHECK(febe_ops_post(0, false) == 0);            /* one core: nothing */
    CHECK(febe_ops_reply(FEBE_F_WROTE, false) == 0);
    CHECK(febe_ops_post(0, true) == COPJ_C_COMMIT);
    CHECK(febe_ops_post(FEBE_F_IN_BULK, true) == COPJ_C_COMMIT);
    CHECK(febe_ops_reply(0, true) == 0);
    CHECK(febe_ops_reply(FEBE_F_WROTE, true) == COPJ_C_COMMIT_DISCARD);
}

/* ---- generations ------------------------------------------------------ */
static void test_gen(void)
{
    static struct febe_gen g;
    uint32_t seen = 0;

    CHECK(!febe_gen_check(&g, &seen));
    CHECK(febe_gen_reclaimable(&g, 0));
    febe_gen_publish(&g);                           /* gen 1 */
    CHECK(!febe_gen_reclaimable(&g, 1));            /* not seen yet */
    CHECK(febe_gen_check(&g, &seen) && seen == 1);
    CHECK(!febe_gen_check(&g, &seen));              /* once per gen */
    febe_gen_ack(&g, seen);
    CHECK(febe_gen_reclaimable(&g, 1));
    /* wrap */
    g.pub = 0xffffffffu; g.ack = 0xfffffffeu;
    CHECK(!febe_gen_reclaimable(&g, 0xffffffffu));
    febe_gen_publish(&g);                           /* 0 */
    CHECK(febe_gen_check(&g, &seen) && seen == 0);
    febe_gen_ack(&g, seen);
    CHECK(febe_gen_reclaimable(&g, 0xffffffffu));
    CHECK(febe_gen_reclaimable(&g, 0));
}

/* ---- the seqlock: a torn read is never accepted (two real threads) ---- */
#define NW 12
static struct { uint32_t seq; uint32_t w[NW]; } rec;
static volatile int stop_writer;

static void *writer(void *arg)
{
    uint32_t v = 0;
    (void)arg;
    while (!stop_writer)
    {
        int i;
        v++;
        febe_seq_write_begin(&rec.seq);
        for (i = 0; i < NW; i++)
            FEBE_ST(rec.w[i], v);       /* every word equal when whole */
        febe_seq_write_end(&rec.seq);
    }
    return NULL;
}

static void test_seqlock(void)
{
    pthread_t t;
    long reads = 0, torn = 0, retried = 0, gaveup = 0;
    uint32_t last = 0;
    int k;

    pthread_create(&t, NULL, writer, NULL);
    for (k = 0; k < 400000; k++)
    {
        uint32_t d[NW];
        int i, tries = febe_seq_copy(&rec.seq, rec.w, d, NW, 64);
        if (tries < 0) { gaveup++; continue; }
        reads++;
        if (tries > 1) retried++;
        for (i = 1; i < NW; i++)
            if (d[i] != d[0]) { torn++; break; }
        if (d[0] < last) torn++;        /* never back in time */
        last = d[0];
    }
    stop_writer = 1;
    pthread_join(t, NULL);
    CHECK(torn == 0);
    CHECK(reads > 0);
    printf("     seqlock: %ld reads, %ld retried, %ld gave up, %ld torn\n",
           reads, retried, gaveup, torn);
}

static void test_sum(void)
{
    uint32_t a[2] = { 0, 0 }, b[2] = { 0, 1 };
    CHECK(febe_sum(a, 2) != 0);
    CHECK(febe_sum(a, 2) != febe_sum(b, 2));
    CHECK(febe_sum(NULL, 0) != 0);
}

int main(void)
{
    test_slot();
    test_ops();
    test_gen();
    test_sum();
    test_seqlock();
    return fails ? 1 : 0;
}
