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
/* Round-trip test of the books.dat format (ab_store.h, ab_store_core.c):
 * record seal/check, a torn record, states, the finished rule at stop,
 * the double header, sector maths, the sector-first-key index lookup
 * (one sector read per lookup) and the eviction order. The file is built
 * in RAM exactly as B writes it. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ab_store.h"
#include "check.h"

static void test_record(void)
{
    struct ab_rec r, z;

    ab_rec_set(&r, 0x1122334455667788ull, 0xdeadbeef, 3, 61000, 123456,
               3661000, 36000000, false, 7);
    CHECK(ab_rec_ok(&r));
    CHECK_EQ(ab_rec_state(&r), AB_REC_STARTED);
    CHECK_EQ(r.file_idx, 3);
    CHECK_EQ(r.elapsed_ms, 61000);
    CHECK_EQ(r.stamp, 7);
    /* a torn record (any byte) fails its check: reads as no record */
    {
        int i, caught = 0;
        for (i = 0; i < (int)sizeof(r); i++)
        {
            struct ab_rec t = r;
            ((uint8_t *)&t)[i] ^= 0x10;
            caught += !ab_rec_ok(&t);
        }
        CHECK_EQ(caught, (int)sizeof(r));
    }
    /* a zeroed slot is never a record */
    memset(&z, 0, sizeof(z));
    CHECK(!ab_rec_ok(&z));
    CHECK_EQ(ab_rec_state(&z), AB_REC_NONE);
    z.book_key = 5;                     /* a key but no check */
    CHECK(!ab_rec_ok(&z));
    /* finished within 30 s of the end at stop: no position kept (Rev 2) */
    ab_rec_set(&r, 9, 0x1234, 2, 10000, 999, 35975000, 36000000, true, 8);
    CHECK_EQ(ab_rec_state(&r), AB_REC_FINISHED);
    CHECK_EQ(r.elapsed_ms, 0);
    CHECK_EQ(r.book_ms, 0);
    /* 30.001 s before the end: still started */
    ab_rec_set(&r, 9, 0x1234, 2, 10000, 999, 35969999, 36000000, true, 8);
    CHECK_EQ(ab_rec_state(&r), AB_REC_STARTED);
    /* while playing (not stopping) the rule does not apply */
    ab_rec_set(&r, 9, 0x1234, 2, 10000, 999, 35999000, 36000000, false, 8);
    CHECK_EQ(ab_rec_state(&r), AB_REC_STARTED);
    /* the book's queue ended: the caller passes book_ms = len */
    ab_rec_set(&r, 9, 0, 0, 0, 0, 36000000, 36000000, true, 9);
    CHECK_EQ(ab_rec_state(&r), AB_REC_FINISHED);
}

static void test_header(void)
{
    struct ab_store_hdr a, b;

    memset(&a, 0, sizeof(a));
    a.seq = 4;
    a.count = 17;
    a.sectors = ab_store_sectors(17);
    ab_store_hdr_seal(&a);
    CHECK(ab_store_hdr_ok(&a));
    CHECK_EQ(a.sectors, 2);
    b = a;
    b.seq = 5;
    ab_store_hdr_seal(&b);
    CHECK(ab_store_hdr_pick(&a, &b) == &b);
    CHECK(ab_store_hdr_pick(&b, &a) == &b);
    b.count = 99;                       /* torn copy: the other wins */
    CHECK(!ab_store_hdr_ok(&b));
    CHECK(ab_store_hdr_pick(&a, &b) == &a);
    a.magic = 0;
    CHECK(ab_store_hdr_pick(&a, &b) == NULL);
    /* wrap-around of seq */
    a = b;
    a.count = 1; a.sectors = 1; a.seq = 0xffffffffu; ab_store_hdr_seal(&a);
    b = a; b.seq = 0; ab_store_hdr_seal(&b);
    CHECK(ab_store_hdr_pick(&a, &b) == &b);
}

static void test_sectors(void)
{
    CHECK_EQ(ab_store_sectors(0), 0);
    CHECK_EQ(ab_store_sectors(16), 1);
    CHECK_EQ(ab_store_sectors(17), 2);
    CHECK_EQ(ab_store_rec_sector(0), 1);
    CHECK_EQ(ab_store_rec_sector(15), 1);
    CHECK_EQ(ab_store_rec_sector(16), 2);
    CHECK_EQ(ab_store_file_size(0), 1024);
    CHECK_EQ(ab_store_file_size(AB_STORE_MAX), (128 + 2) * 512);
    CHECK_EQ(sizeof(struct ab_rec) * AB_STORE_PER_SECT, AB_STORE_SECTOR);
}

/* A full file in RAM (2,048 records): the index finds every key with one
 * sector read and rules out absent keys; insert order is sorted. */
static void test_index(void)
{
    enum { N = AB_STORE_MAX };
    struct ab_rec *recs = calloc(N, sizeof(*recs));
    struct ab_store_index ix;
    int i, s, found = 0, absent_ok = 1;

    for (i = 0; i < N; i++)             /* keys 10, 20, ... */
        ab_rec_set(&recs[i], (ab_key_t)(i + 1) * 10, i, 0, i, 0, i, 0,
                   false, (uint32_t)i);
    memset(&ix, 0, sizeof(ix));
    for (s = 0; s < ab_store_sectors(N); s++)
        ab_store_index_add(&ix, s, recs + s * AB_STORE_PER_SECT,
                           AB_STORE_PER_SECT);
    CHECK_EQ(ix.sectors, 128);
    CHECK_EQ(ix.count, N);
    CHECK_EQ(ix.clock, N);              /* max stamp + 1 */
    for (i = 0; i < N; i++)
    {
        ab_key_t k = (ab_key_t)(i + 1) * 10;
        s = ab_store_sector_for(&ix, k);
        /* one sector: the key is in it */
        found += ab_store_rec_find(recs + s * AB_STORE_PER_SECT,
                                   AB_STORE_PER_SECT, k) >= 0;
        /* k + 5 is absent: its sector says so */
        s = ab_store_sector_for(&ix, k + 5);
        absent_ok &= ab_store_rec_find(recs + s * AB_STORE_PER_SECT,
                                       AB_STORE_PER_SECT, k + 5) < 0;
    }
    CHECK_EQ(found, N);
    CHECK(absent_ok);
    CHECK_EQ(ab_store_sector_for(&ix, 1), 0);   /* below all keys */
    /* insertion point (a new book goes through books.tmp in this order) */
    CHECK_EQ(ab_store_rec_find(recs, N, 15), -(1) - 1);
    CHECK_EQ(ab_store_rec_find(recs, N, 5), -0 - 1);
    CHECK_EQ(ab_store_rec_find(recs, N, 99999), -N - 1);
    memset(&ix, 0, sizeof(ix));
    CHECK_EQ(ab_store_sector_for(&ix, 10), -1);
    free(recs);
}

/* Rev 1 item 11: past the cap evict the oldest FINISHED, then the oldest
 * STARTED; a record whose check failed goes first (it is lost anyway) */
static void test_evict(void)
{
    struct ab_rec r[6];

    ab_rec_set(&r[0], 1, 0, 0, 0, 0, 0, 0, false, 50);     /* started */
    ab_rec_set(&r[1], 2, 0, 0, 0, 0, 100, 100, true, 40);  /* finished */
    ab_rec_set(&r[2], 3, 0, 0, 0, 0, 0, 0, false, 10);     /* started */
    ab_rec_set(&r[3], 4, 0, 0, 0, 0, 100, 100, true, 30);  /* finished */
    ab_rec_set(&r[4], 5, 0, 0, 0, 0, 0, 0, false, 20);     /* started */
    ab_rec_set(&r[5], 6, 0, 0, 0, 0, 0, 0, false, 60);
    CHECK_EQ(ab_store_evict_pick(r, 6), 3);     /* oldest finished */
    r[3] = r[0];
    r[1] = r[0];                                /* no finished left */
    CHECK_EQ(ab_store_evict_pick(r, 6), 2);     /* oldest started */
    r[5].elapsed_ms ^= 1;                       /* torn */
    CHECK_EQ(ab_store_evict_pick(r, 6), 5);
    CHECK_EQ(ab_store_evict_pick(r, 0), -1);
}

int main(void)
{
    test_record();
    test_header();
    test_sectors();
    test_index();
    test_evict();
    CHECK_DONE("test_abstore");
}
