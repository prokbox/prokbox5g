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
#include "libfile.h"         /* libfile_sum_*: a 0.8.0 record's crc */
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
    CHECK_EQ(LIB_SHUF_REC_SIZE, 92);
    CHECK_EQ(offsetof(struct lib_shuffle_rec, count), 76);  /* v1's crc */
    CHECK_EQ(offsetof(struct lib_shuffle_rec, crc), 88);
    CHECK_EQ(offsetof(struct lib_shuffle_rec, it), 32);
    CHECK_EQ(sizeof(struct lib_shuf_stamp), 16);
    CHECK_EQ(sizeof(struct lib_shuf_count), 12);
    CHECK_EQ(LIB_SHUF_VERSION, 2);
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
    back.version = LIB_SHUF_VERSION + 1;
    lib_shuf_rec_seal(&back);   /* seal rewrites the version: forge it */
    back.version = LIB_SHUF_VERSION + 1;
    back.crc = lib_shuf_rec_sum(&back);
    CHECK_EQ(lib_shuf_rec_check(&back, sizeof(back), &stamp0, 4),
             LIB_SHUF_BAD);
    back = r;
    back.count.total ^= 4;                              /* count covered */
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

/* ---- 0.8.1: the counter ("n of M" over a library source) ---------------
 * A model of lib_shuffle.c's queue: refills of LIB_SHUF_WINDOW entries
 * from the Songs iterator, non-music entries skipped (every 7th entry),
 * the history trimmed to LIB_SHUF_HISTORY. For every queued entry the
 * model also notes, independently of the count, its expected position:
 * the music songs of its lap so far (a lap = one iterator lap, or the
 * run after a restart / a shelf commit). */
#define Q_MAX 4096
struct qmodel {
    struct lib_shuffle_rec r;
    uint32_t want[Q_MAX];       /* expected lap position of each entry */
    uint32_t n;                 /* entries queued (after trims) */
    uint32_t lap_songs;         /* songs of the current lap so far */
    uint32_t lap_seen;          /* the iterator lap the model is in */
    bool lap_new;               /* model: the next entry starts a lap */
};

static bool qm_music(uint32_t e)
{
    return e % 7 != 3;
}

static void qm_refill(struct qmodel *q, uint32_t k)
{
    while (k-- > 0 && q->n < Q_MAX)
    {
        uint32_t lap = lib_shuf_rec_lap(&q->r), e;

        do
            e = shuf_songs_next(&q->r.it.s, true);
        while (e != SHUF_NONE && !qm_music(e));
        lib_shuf_count_note_lap(&q->r, lap);    /* as next_entry() does */
        CHECK(e != SHUF_NONE);
        if (q->r.it.s.lap != q->lap_seen)
        {
            q->lap_seen = q->r.it.s.lap;
            q->lap_new = true;
        }
        if (q->lap_new)
        {
            q->lap_songs = 0;
            q->lap_new = false;
        }
        q->want[q->n++] = ++q->lap_songs;
        lib_shuf_count_queued(&q->r);
    }
}

static void qm_trim(struct qmodel *q, uint32_t played)
{
    /* refill_finish(): keep LIB_SHUF_HISTORY played entries */
    if (played > LIB_SHUF_HISTORY)
    {
        uint32_t del = played - LIB_SHUF_HISTORY;
        memmove(q->want, q->want + del, (q->n - del) * sizeof(q->want[0]));
        q->n -= del;
    }
}

static void qm_start(struct qmodel *q, uint32_t n, uint32_t first)
{
    memset(q, 0, sizeof(*q));
    q->r.source = LIB_SHUF_SRC_SONGS;
    q->r.mode = NP_SHUFFLE_SONGS;
    q->r.stamp = stamp0;
    shuf_songs_init(&q->r.it.s, n, 0xc0ffee, first);
    q->lap_new = true;
}

/* Every queued entry's position, from the end as Now Playing asks */
static int qm_check_all(const struct qmodel *q)
{
    uint32_t i;
    int bad = 0;

    for (i = 0; i < q->n; i++)
    {
        uint32_t got = lib_shuf_count_pos(&q->r, q->n - 1 - i);
        if (got != q->want[i] && bad++ < 3)
            fprintf(stderr, "  entry %u of %u: pos %u, want %u\n", i, q->n,
                    got, q->want[i]);
    }
    return bad;
}

/* Refills across many windows (and laps: 70 entries, 60 music), with
 * history trims; previous inside the window is just an earlier entry */
static void test_count_refills(void)
{
    struct qmodel q;
    uint32_t played, w;

    qm_start(&q, 70, SHUF_NONE);
    qm_refill(&q, LIB_SHUF_WINDOW);
    CHECK_EQ(lib_shuf_count_pos(&q.r, LIB_SHUF_WINDOW - 1), 1); /* "1 of" */
    CHECK_EQ(lib_shuf_count_pos(&q.r, 0), LIB_SHUF_WINDOW);
    CHECK_EQ(qm_check_all(&q), 0);
    played = 1;
    for (w = 0; w < 12; w++)
    {
        /* walk to LIB_SHUF_LOW left, then the refill and its trim */
        played = q.n - LIB_SHUF_LOW;
        qm_refill(&q, LIB_SHUF_WINDOW);
        qm_trim(&q, played);
        CHECK_EQ(qm_check_all(&q), 0);
    }
    /* the 60-song laps restart at 1 (Repeat): a window holds both laps */
    CHECK(q.r.it.s.lap >= 8);
    {
        uint32_t i, ones = 0, max = 0;
        for (i = 0; i < q.n; i++)
        {
            ones += lib_shuf_count_pos(&q.r, q.n - 1 - i) == 1;
            if (lib_shuf_count_pos(&q.r, q.n - 1 - i) > max)
                max = lib_shuf_count_pos(&q.r, q.n - 1 - i);
        }
        CHECK(ones >= 2);
        CHECK_EQ(max, 60);      /* a lap is every music entry, once */
    }
    /* the queue past the count: unknown, never a wrong number */
    CHECK_EQ(lib_shuf_count_pos(&q.r, q.r.count.total), 0);

    /* a large library: no lap in the window, 1..N in queue order */
    qm_start(&q, 50000, 777);
    qm_refill(&q, LIB_SHUF_WINDOW);
    CHECK_EQ(q.want[0], 1);
    for (w = 0; w < 5; w++)
    {
        played = q.n - LIB_SHUF_LOW;
        qm_refill(&q, LIB_SHUF_WINDOW);
        qm_trim(&q, played);
    }
    CHECK_EQ(qm_check_all(&q), 0);
    CHECK_EQ(lib_shuf_count_pos(&q.r, 0), 6 * LIB_SHUF_WINDOW);
}

/* A restart (database changed): the next entry is a lap's first */
static void test_count_restart(void)
{
    struct qmodel q;
    struct lib_shuf_stamp now = { 1240, 50100, 3, 0 };

    qm_start(&q, 50000, SHUF_NONE);
    qm_refill(&q, 2 * LIB_SHUF_WINDOW);
    lib_shuf_rec_restart(&q.r, 50100, &now);
    CHECK(q.r.flags & LIB_SHUF_F_LAP);
    q.lap_new = true;
    q.lap_seen = q.r.it.s.lap;
    qm_refill(&q, LIB_SHUF_WINDOW);
    CHECK(!(q.r.flags & LIB_SHUF_F_LAP));
    CHECK_EQ(qm_check_all(&q), 0);
    CHECK_EQ(lib_shuf_count_pos(&q.r, LIB_SHUF_WINDOW - 1), 1);
    CHECK_EQ(lib_shuf_count_pos(&q.r, LIB_SHUF_WINDOW), 100); /* old lap */
    /* the restart's flag is in the record: it survives a reboot */
    lib_shuf_rec_restart(&q.r, 50100, &now);
    lib_shuf_rec_seal(&q.r);
    CHECK_EQ(lib_shuf_rec_check(&q.r, sizeof(q.r), &now, q.r.queued),
             LIB_SHUF_RESUME);
    CHECK(q.r.flags & LIB_SHUF_F_LAP);
}

/* A shelf commit: what is queued after the current song goes, and the
 * new order starts at the current song ("1 of M"); the history keeps
 * its numbers */
static void test_count_commit(void)
{
    struct qmodel q;
    uint32_t cur = 30, gone, i;

    qm_start(&q, 50000, SHUF_NONE);
    qm_refill(&q, LIB_SHUF_WINDOW);
    gone = q.n - 1 - cur;
    lib_shuf_count_unqueue(&q.r, gone);
    q.n -= gone;
    lib_shuf_count_lap_from(&q.r, 1);
    q.want[cur] = 1;
    q.lap_songs = 1;
    shuf_songs_init(&q.r.it.s, 50000, 0xabc, SHUF_NONE);
    q.lap_seen = 0;
    CHECK_EQ(lib_shuf_count_pos(&q.r, 0), 1);
    for (i = 0; i < cur; i++)
        CHECK_EQ(lib_shuf_count_pos(&q.r, q.n - 1 - i), i + 1);
    qm_refill(&q, LIB_SHUF_WINDOW);
    CHECK_EQ(qm_check_all(&q), 0);
    CHECK_EQ(lib_shuf_count_pos(&q.r, 0), 1 + LIB_SHUF_WINDOW);
    /* removing more than was counted clamps */
    lib_shuf_count_unqueue(&q.r, 1000000);
    CHECK_EQ(q.r.count.total, 0);
    CHECK_EQ(lib_shuf_count_pos(&q.r, 0), 0);
}

/* Resume: the count goes through the file with the iterator; a power cut
 * after the control file and before the record leaves extra entries */
static void test_count_resume(void)
{
    struct qmodel q, cont;
    struct lib_shuffle_rec back;
    uint32_t extra = 20;

    qm_start(&q, 70, SHUF_NONE);
    qm_refill(&q, 3 * LIB_SHUF_WINDOW);
    q.r.queued = q.n;
    lib_shuf_rec_seal(&q.r);
    CHECK_EQ(file_round_trip(&q.r, &back, sizeof(q.r)), sizeof(q.r));
    CHECK_EQ(lib_shuf_rec_check(&back, sizeof(back), &stamp0, q.n),
             LIB_SHUF_RESUME);
    cont = q;
    cont.r = back;
    CHECK_EQ(qm_check_all(&cont), 0);
    qm_refill(&cont, LIB_SHUF_WINDOW);
    qm_refill(&q, LIB_SHUF_WINDOW);
    CHECK_EQ(qm_check_all(&cont), 0);
    CHECK(!memcmp(&cont.r.count, &q.r.count, sizeof(q.r.count)));

    /* power cut: 'extra' entries queued after the record was written */
    cont = q;
    cont.r = back;
    qm_refill(&q, extra);           /* what really got queued */
    cont.n = q.n;
    memcpy(cont.want, q.want, sizeof(q.want));
    lib_shuf_count_extra(&cont.r, extra);
    CHECK_EQ(lib_shuf_count_pos(&cont.r, extra), lib_shuf_count_pos(&back,
                                                                     0));
    CHECK_EQ(cont.r.count.total, back.count.total + extra);
}

/* A version 1 record (80 B, from 0.8.0) upgrades in place; the caller
 * restarts its lap and counts what it resumed */
static void test_upgrade(void)
{
    struct lib_shuffle_rec r, v1;
    struct libfile_sum_state s;
    uint32_t crc;

    make_songs(&r, 40);
    /* the 0.8.0 layout: the same 76 bytes, then their crc */
    memcpy(&v1, &r, LIB_SHUF_V1_SIZE - 4);
    v1.version = 1;
    libfile_sum_init(&s);
    libfile_sum_add(&s, &v1, LIB_SHUF_V1_SIZE - 4);
    crc = libfile_sum_end(&s);
    memcpy((char *)&v1 + LIB_SHUF_V1_SIZE - 4, &crc, 4);
    CHECK_EQ(lib_shuf_rec_check(&v1, LIB_SHUF_V1_SIZE, &stamp0, 41),
             LIB_SHUF_BAD);
    CHECK(lib_shuf_rec_upgrade(&v1, LIB_SHUF_V1_SIZE));
    CHECK_EQ(v1.version, LIB_SHUF_VERSION);
    CHECK_EQ(v1.count.total, 0);
    CHECK_EQ(v1.count.lap_at, 0);
    CHECK_EQ(v1.count.prev_at, 0);
    CHECK(!memcmp(&v1.it, &r.it, sizeof(r.it)));
    CHECK_EQ(v1.queued, r.queued);
    CHECK_EQ(lib_shuf_rec_check(&v1, sizeof(v1), &stamp0, 41),
             LIB_SHUF_RESUME);
    /* not a v1 record: untouched */
    memcpy(&v1, &r, sizeof(r));
    CHECK(!lib_shuf_rec_upgrade(&v1, sizeof(v1)));      /* already v2 */
    CHECK(!memcmp(&v1, &r, sizeof(r)));
    memcpy(&v1, &r, LIB_SHUF_V1_SIZE - 4);
    v1.version = 1;
    crc ^= 1;                                           /* torn */
    memcpy((char *)&v1 + LIB_SHUF_V1_SIZE - 4, &crc, 4);
    CHECK(!lib_shuf_rec_upgrade(&v1, LIB_SHUF_V1_SIZE));
    CHECK(!lib_shuf_rec_upgrade(&v1, LIB_SHUF_V1_SIZE - 1));
}

int main(void)
{
    test_layout();
    test_count_refills();
    test_count_restart();
    test_count_commit();
    test_count_resume();
    test_upgrade();
    test_round_trip();
    test_bad();
    test_stamp();
    test_restart();
    test_book_flag();
    CHECK_DONE("test_shuffle_rec");
}
