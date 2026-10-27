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

/* Host test: the library shuffle resume record (apps/ipodui/
 * lib_shuffle_rec.c, lib_shuffle.h; Phase 5 contracts, Revision 1 item 6):
 * write, read back through a real file, CRC, every stamp mismatch ->
 * restart with a new lap that continues deterministically. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "lib_shuffle.h"
#include "check.h"

static const struct lib_shuf_stamp stamp0 = { 1234, 50000, 3, 0xabcdef01u };

static void make_songs(struct lib_shuffle_rec *r, uint32_t steps)
{
    uint32_t i;

    memset(r, 0, sizeof(*r));
    r->stamp = stamp0;
    r->stamp.albums_sum = 0;
    r->source = LIB_SHUF_SRC_SONGS;
    r->mode = NP_SHUFFLE_SONGS;
    r->repeat = 1;
    shuf_songs_init(&r->it.s, 50000, 0x5eed, 777);
    for (i = 0; i < steps; i++)
        shuf_songs_next(&r->it.s, true);
    r->queued = 1 + steps;
    lib_shuf_rec_seal(r);
}

static uint32_t one(void *ctx, uint32_t album)
{
    (void)ctx;
    return 1 + album % 13;
}

static void make_albums(struct lib_shuffle_rec *r, uint32_t steps)
{
    uint32_t i, a, t;

    memset(r, 0, sizeof(*r));
    r->stamp = stamp0;
    r->source = LIB_SHUF_SRC_ALBUMS;
    r->mode = NP_SHUFFLE_ALBUMS;
    shuf_albums_init(&r->it.a, 4000, 0x1234);
    for (i = 0; i < steps; i++)
        shuf_albums_next(&r->it.a, one, NULL, true, &a, &t);
    r->queued = steps;
    lib_shuf_rec_seal(r);
}

/* write the record to a file and read it back, as lib_shuffle.c does */
static size_t file_round_trip(const struct lib_shuffle_rec *in,
                              struct lib_shuffle_rec *out, size_t write_len)
{
    char path[] = "/tmp/npshufXXXXXX";
    int fd = mkstemp(path);
    FILE *f;
    size_t n;

    CHECK(fd >= 0);
    f = fdopen(fd, "w+b");
    fwrite(in, 1, write_len, f);
    fflush(f);
    rewind(f);
    memset(out, 0xa5, sizeof(*out));
    n = fread(out, 1, sizeof(*out), f);
    fclose(f);
    remove(path);
    return n;
}

static void test_layout(void)
{
    CHECK_EQ(sizeof(struct lib_shuffle_rec), LIB_SHUF_REC_SIZE);
    CHECK_EQ(offsetof(struct lib_shuffle_rec, crc), 76);
    CHECK_EQ(offsetof(struct lib_shuffle_rec, it), 32);
    CHECK_EQ(sizeof(struct lib_shuf_stamp), 16);
}

static void test_round_trip(void)
{
    struct lib_shuffle_rec r, back, cont;
    size_t n;
    uint32_t i;

    make_songs(&r, 120);
    n = file_round_trip(&r, &back, sizeof(r));
    CHECK_EQ(n, sizeof(r));
    CHECK(!memcmp(&r, &back, sizeof(r)));
    CHECK_EQ(lib_shuf_rec_check(&back, n, &(struct lib_shuf_stamp){
                 1234, 50000, 3, 0x99 }, 121), LIB_SHUF_RESUME);
    /* resume continues the same order: the next 50 match a run that never
     * stopped */
    make_songs(&cont, 120);
    for (i = 0; i < 50; i++)
        CHECK_EQ(shuf_songs_next(&back.it.s, true),
                 shuf_songs_next(&cont.it.s, true));

    make_albums(&r, 300);
    n = file_round_trip(&r, &back, sizeof(r));
    CHECK_EQ(lib_shuf_rec_check(&back, n, &stamp0, 300), LIB_SHUF_RESUME);
    make_albums(&cont, 300);
    for (i = 0; i < 50; i++)
    {
        uint32_t a1, t1, a2, t2;
        CHECK(shuf_albums_next(&back.it.a, one, NULL, true, &a1, &t1));
        CHECK(shuf_albums_next(&cont.it.a, one, NULL, true, &a2, &t2));
        CHECK_EQ(a1, a2);
        CHECK_EQ(t1, t2);
    }
}

static void test_bad(void)
{
    struct lib_shuffle_rec r, back;
    size_t n;

    make_songs(&r, 3);
    n = file_round_trip(&r, &back, sizeof(r) - 4);      /* torn write */
    CHECK_EQ(lib_shuf_rec_check(&back, n, &stamp0, 4), LIB_SHUF_BAD);
    CHECK_EQ(lib_shuf_rec_check(&r, 0, &stamp0, 4), LIB_SHUF_BAD);
    back = r;
    back.it.s.pos ^= 1;                                 /* bit flip */
    CHECK_EQ(lib_shuf_rec_check(&back, sizeof(back), &stamp0, 4),
             LIB_SHUF_BAD);
    back = r;
    back.magic++;
    CHECK_EQ(lib_shuf_rec_check(&back, sizeof(back), &stamp0, 4),
             LIB_SHUF_BAD);
    back = r;
    back.version = 2;
    lib_shuf_rec_seal(&back);   /* seal rewrites the version: forge it */
    back.version = 2;
    back.crc = lib_shuf_rec_sum(&back);
    CHECK_EQ(lib_shuf_rec_check(&back, sizeof(back), &stamp0, 4),
             LIB_SHUF_BAD);
    back = r;
    back.source = 9;
    back.crc = lib_shuf_rec_sum(&back);
    CHECK_EQ(lib_shuf_rec_check(&back, sizeof(back), &stamp0, 4),
             LIB_SHUF_BAD);
}

static void test_stamp(void)
{
    struct lib_shuffle_rec s, a;
    struct lib_shuf_stamp now;

    make_songs(&s, 10);
    make_albums(&a, 10);
    /* Songs ignores the album checksum */
    now = stamp0; now.albums_sum = 1;
    CHECK_EQ(lib_shuf_rec_check(&s, sizeof(s), &now, 11), LIB_SHUF_RESUME);
    CHECK_EQ(lib_shuf_rec_check(&a, sizeof(a), &now, 10), LIB_SHUF_RESTART);
    now = stamp0; now.commitid++;                       /* a commit */
    CHECK_EQ(lib_shuf_rec_check(&s, sizeof(s), &now, 11), LIB_SHUF_RESTART);
    CHECK_EQ(lib_shuf_rec_check(&a, sizeof(a), &now, 10), LIB_SHUF_RESTART);
    now = stamp0; now.entries--;
    CHECK_EQ(lib_shuf_rec_check(&s, sizeof(s), &now, 11), LIB_SHUF_RESTART);
    now = stamp0; now.sortkey_version++;
    CHECK_EQ(lib_shuf_rec_check(&a, sizeof(a), &now, 10), LIB_SHUF_RESTART);
    /* the control file and the record disagree (power cut between them) */
    CHECK_EQ(lib_shuf_rec_check(&s, sizeof(s), &stamp0, 61), LIB_SHUF_RESTART);
    /* an ordinary queue's record only carries the shelf setting */
    memset(&s, 0, sizeof(s));
    s.mode = NP_SHUFFLE_ALBUMS;
    lib_shuf_rec_seal(&s);
    now = stamp0; now.commitid = 99;
    CHECK_EQ(lib_shuf_rec_check(&s, sizeof(s), &now, 5), LIB_SHUF_RESUME);
}

static void test_restart(void)
{
    struct lib_shuffle_rec r, again;
    struct lib_shuf_stamp now = { 1240, 50100, 3, 0 };
    uint32_t first[20], i;

    make_songs(&r, 70);
    lib_shuf_rec_restart(&r, 50100, &now);
    lib_shuf_rec_seal(&r);
    CHECK_EQ(lib_shuf_rec_check(&r, sizeof(r), &now, r.queued),
             LIB_SHUF_RESUME);
    CHECK_EQ(r.it.s.p.n, 50100);
    CHECK_EQ(r.it.s.pos, 0);
    CHECK_EQ(r.it.s.first, SHUF_NONE);
    CHECK_EQ(r.it.s.seed, shuf_lap_seed(0x5eed, 1));   /* lap 0 -> 1 */
    for (i = 0; i < 20; i++)
        first[i] = shuf_songs_next(&r.it.s, true);
    /* deterministic: the same restart gives the same order */
    make_songs(&again, 70);
    lib_shuf_rec_restart(&again, 50100, &now);
    for (i = 0; i < 20; i++)
        CHECK_EQ(shuf_songs_next(&again.it.s, true), first[i]);

    make_albums(&r, 5);
    now.albums_sum = 0x77;
    lib_shuf_rec_restart(&r, 3990, &now);
    CHECK_EQ(r.it.a.p.n, 3990);
    CHECK_EQ(r.stamp.albums_sum, 0x77);
    CHECK_EQ(r.it.a.seed, shuf_lap_seed(0x1234, 1));
}

/* P3: the "book stops at its end" flag in the former pad byte survives a
 * file round trip and the seal, is covered by the CRC, and a record from
 * before it (pad 0) reads as no flag. Nothing else moved. */
static void test_book_flag(void)
{
    struct lib_shuffle_rec r, back;

    CHECK_EQ(offsetof(struct lib_shuffle_rec, flags), 27);
    memset(&r, 0, sizeof(r));
    r.source = LIB_SHUF_SRC_NONE;
    r.mode = NP_SHUFFLE_OFF;
    r.queued = 1;
    r.flags = LIB_SHUF_F_BOOK;
    lib_shuf_rec_seal(&r);
    CHECK_EQ(r.flags, LIB_SHUF_F_BOOK);         /* the seal keeps it */
    CHECK_EQ(file_round_trip(&r, &back, sizeof(r)), sizeof(r));
    CHECK(memcmp(&r, &back, sizeof(r)) == 0);
    CHECK_EQ(back.flags & LIB_SHUF_F_BOOK, LIB_SHUF_F_BOOK);
    CHECK_EQ(lib_shuf_rec_check(&back, sizeof(back), &stamp0, 1),
             LIB_SHUF_RESUME);
    back.flags = 0;                             /* covered by the CRC */
    CHECK_EQ(lib_shuf_rec_check(&back, sizeof(back), &stamp0, 1),
             LIB_SHUF_BAD);
    r.flags = 0;                                /* an older record */
    lib_shuf_rec_seal(&r);
    CHECK_EQ(file_round_trip(&r, &back, sizeof(r)), sizeof(r));
    CHECK_EQ(back.flags, 0);
    CHECK_EQ(lib_shuf_rec_check(&back, sizeof(back), &stamp0, 1),
             LIB_SHUF_RESUME);
    /* a library source keeps its flag byte too (the runtime clears it) */
    make_songs(&r, 3);
    r.flags = LIB_SHUF_F_BOOK;
    lib_shuf_rec_seal(&r);
    CHECK_EQ(file_round_trip(&r, &back, sizeof(r)), sizeof(r));
    CHECK_EQ(back.flags, LIB_SHUF_F_BOOK);
    CHECK_EQ(lib_shuf_rec_check(&back, sizeof(back), &stamp0, 4),
             LIB_SHUF_RESUME);
}

int main(void)
{
    test_layout();
    test_round_trip();
    test_bad();
    test_stamp();
    test_restart();
    test_book_flag();
    CHECK_DONE("test_shuffle_rec");
}
