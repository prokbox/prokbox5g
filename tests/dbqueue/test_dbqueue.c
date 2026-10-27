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

/* Host tests of the queue-by-index core, apps/ipodui/dbqueue_core.c
 * (#109, docs/design/queue-by-index.md section 7):
 *  - header and block seal/check: torn tail, wrong file, wrong seq;
 *  - T field packing;
 *  - the mode table (stamp fields, RAM loaded, directory holds, mode);
 *  - H1: every failure mode returns 0 and a sentinel no FAT name matches;
 *  - H2: the rebase covers every slot, history included;
 *  - H4: a reserve with keep carries the words over;
 *  - sort order of database words against A: words (sort_compare_fn);
 *  - a cut after every write of a new queue, a mode change and a
 *    materialization (the tests/fatcrash idea, modelled in memory);
 *  - M5: a recovered previous control file finds its intact payload. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dbqueue_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* the firmware's crc_32() (MSB first, poly 0x04C11DB7), bitwise */
uint32_t crc_32(const void *src, uint32_t len, uint32_t crc)
{
    const unsigned char *p = src;
    while (len--)
    {
        int b;
        crc ^= (uint32_t)*p++ << 24;
        for (b = 0; b < 8; b++)
            crc = crc & 0x80000000u ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

/* -- playlist.c's word layout and comparator (copied, playlist.c) ---- */
#define PLAYLIST_SEEK_MASK              0x0FFFFFFF
#define PLAYLIST_INSERT_TYPE_MASK       0xC0000000
#define PLAYLIST_INSERT_TYPE_PREPEND    0x40000000
#define PLAYLIST_INSERT_TYPE_INSERT     0x80000000
#define PLAYLIST_INSERT_TYPE_APPEND     0xC0000000

/* unsigned long is 32 bits on the device: uint32_t here */
static int sort_compare_fn(const void* p1, const void* p2)
{
    uint32_t* e1 = (uint32_t*) p1;
    uint32_t* e2 = (uint32_t*) p2;
    uint32_t flags1 = *e1 & PLAYLIST_INSERT_TYPE_MASK;
    uint32_t flags2 = *e2 & PLAYLIST_INSERT_TYPE_MASK;

    if (flags1 == flags2)
        return (*e1 & PLAYLIST_SEEK_MASK) - (*e2 & PLAYLIST_SEEK_MASK);
    else if (flags1 == PLAYLIST_INSERT_TYPE_PREPEND ||
        flags2 == PLAYLIST_INSERT_TYPE_APPEND)
        return -1;
    else if (flags1 == PLAYLIST_INSERT_TYPE_APPEND ||
        flags2 == PLAYLIST_INSERT_TYPE_PREPEND)
        return 1;
    else if (flags1 && flags2)
        return (*e1 & PLAYLIST_SEEK_MASK) - (*e2 & PLAYLIST_SEEK_MASK);
    else
        return *e1 - *e2;
}

static void test_sort_order(void)
{
    uint32_t w[7];
    int i;
    /* shuffled: db slots 3,0,2,1 and A: entries at seeks 900, 40 */
    w[0] = PLAYLIST_INSERT_TYPE_INSERT | DBQ_FLAG | 3;
    w[1] = PLAYLIST_INSERT_TYPE_INSERT | 900;
    w[2] = PLAYLIST_INSERT_TYPE_INSERT | DBQ_FLAG | 0;
    w[3] = PLAYLIST_INSERT_TYPE_INSERT | DBQ_FLAG | 2;
    w[4] = PLAYLIST_INSERT_TYPE_INSERT | 40;
    w[5] = PLAYLIST_INSERT_TYPE_INSERT | DBQ_FLAG | 1;
    w[6] = PLAYLIST_INSERT_TYPE_INSERT | (DBQ_FLAG - 1);    /* P4's limit */
    qsort(w, 7, sizeof(w[0]), sort_compare_fn);
    CHECK((w[0] & PLAYLIST_SEEK_MASK) == 40);
    CHECK((w[1] & PLAYLIST_SEEK_MASK) == 900);
    CHECK((w[2] & PLAYLIST_SEEK_MASK) == DBQ_FLAG - 1);
    for (i = 0; i < 4; i++)     /* database words last, in slot order */
        CHECK((w[3 + i] & PLAYLIST_SEEK_MASK) == (DBQ_FLAG | (unsigned)i));
}

static void test_tag(void)
{
    uint32_t t = dbq_tag(1, 0x123456);
    CHECK(t == 0x01123456u);
    CHECK(dbq_tag_seq(t) == 0x123456 && dbq_tag_file(t) == 1);
    CHECK(dbq_tag(0, 0x1FFFFFF) == 0x00FFFFFFu);
    CHECK(dbq_seq_next(DBQ_SEQ_MASK) == 1 && dbq_seq_next(0) == 1);
    CHECK(dbq_seq_next(5) == 6);
}

static void stamp(struct dbq_stamp *s, int32_t id, int32_t n, const char *p)
{
    memset(s, 0, sizeof(*s));
    s->commitid = id;
    s->entries = n;
    strncpy(s->db_path, p, sizeof(s->db_path) - 1);
}

static void test_seal(void)
{
    uint32_t vals[16];
    struct dbq_block b;
    struct dbq_hdr h;
    int i;

    for (i = 0; i < 16; i++)
        vals[i] = i * 7;
    dbq_block_seal(&b, 9, 16, DBQ_MODE_IDX, vals);
    CHECK(dbq_block_check(&b, vals, 9, 16));
    CHECK(!dbq_block_check(&b, vals, 8, 16));       /* wrong seq */
    CHECK(!dbq_block_check(&b, vals, 9, 15));       /* wrong count */
    vals[15] = 0;                                   /* torn tail */
    CHECK(!dbq_block_check(&b, vals, 9, 16));

    memset(&h, 0, sizeof(h));
    h.nblocks = 1;
    h.next_seq = 10;
    h.e[0].seq = 9;
    h.e[0].count = 16;
    h.e[0].file = 1;
    stamp(&h.e[0].st, 5, 100, "/.rockbox/database_gen5");
    dbq_hdr_seal(&h);
    CHECK(dbq_hdr_check(&h));
    CHECK(dbq_hdr_find(&h, dbq_tag(1, 9), 0, 16) == &h.e[0]);
    /* the T's file may be stale after a mode change: the row decides */
    CHECK(dbq_hdr_find(&h, dbq_tag(0, 9), 0, 16) == &h.e[0]);
    CHECK(!dbq_hdr_find(&h, dbq_tag(1, 8), 0, 16));    /* wrong seq */
    CHECK(!dbq_hdr_find(&h, dbq_tag(1, 9), 0, 15));    /* wrong count */
    CHECK(!dbq_hdr_find(&h, dbq_tag(1, 9), 1, 16));    /* not from 0 */
    ((char *)&h)[sizeof(h) - 8] ^= 1;                  /* torn */
    CHECK(!dbq_hdr_check(&h));
    CHECK(!dbq_hdr_find(&h, dbq_tag(1, 9), 0, 16));
}

static void test_modes(void)
{
    struct dbq_stamp blk, act, other;

    stamp(&blk, 5, 100, "/.rockbox/database_gen5");
    act = blk;
    CHECK(dbq_res_mode(DBQ_MODE_IDX, 3, &blk, &act, true) == DBQ_RES_LIVE);
    CHECK(dbq_res_mode(DBQ_MODE_IDX, 3, &blk, &act, false) == DBQ_RES_LIVE);
    CHECK(dbq_res_mode(DBQ_MODE_IDX, 100, &blk, &act, true) == DBQ_RES_DISK);
    CHECK(dbq_res_mode(DBQ_MODE_IDX, 3, &blk, NULL, true) == DBQ_RES_DISK);
    CHECK(dbq_res_mode(DBQ_MODE_IDX, 3, &blk, NULL, false) == DBQ_RES_LOST);
    /* every stamp field counts (L6) */
    other = blk; other.commitid = 6;
    CHECK(dbq_res_mode(DBQ_MODE_IDX, 3, &blk, &other, true) == DBQ_RES_DISK);
    other = blk; other.entries = 101;
    CHECK(dbq_res_mode(DBQ_MODE_IDX, 3, &blk, &other, false) == DBQ_RES_LOST);
    other = blk; strcpy(other.db_path, "/.rockbox/database_gen6");
    CHECK(dbq_res_mode(DBQ_MODE_IDX, 3, &blk, &other, true) == DBQ_RES_DISK);
    other = blk; other.entries = -1;               /* RAM not loaded */
    CHECK(dbq_res_mode(DBQ_MODE_IDX, 3, &blk, &other, true) == DBQ_RES_DISK);
    CHECK(dbq_res_mode(DBQ_MODE_SEEK, 4096, &blk, &act, true) == DBQ_RES_SEEK);
    CHECK(dbq_res_mode(DBQ_MODE_SEEK, 4096, &blk, &act, false) == DBQ_RES_LOST);
    CHECK(dbq_res_mode(DBQ_MODE_PATH, 0, &blk, NULL, false) == DBQ_RES_PATH);
    CHECK(dbq_res_mode(DBQ_MODE_PATH, DBQ_LOST, &blk, NULL, true) == DBQ_RES_LOST);
    CHECK(dbq_res_mode(7, 1, &blk, &act, true) == DBQ_RES_LOST);
}

/* H1: every failure mode is 0 with a sentinel */
static void test_h1_sentinel(void)
{
    char buf[261];
    int why;
    int32_t hdr[2];
    const char name[] = "/Music/a.mp3";

    for (why = DBQ_WHY_DELETED; why <= DBQ_WHY_BUSY; why++)
    {
        strcpy(buf, "/Music/stale.mp3");     /* a half-done read */
        CHECK(dbq_resolve_finish(why, 1234, buf, sizeof(buf)) == 0);
        CHECK(!strcmp(buf, "/<dbq>/1234"));
        CHECK(strchr(buf, '<') && strchr(buf, '>'));   /* no FAT name */
        CHECK(dbq_is_sentinel(buf));
    }
    buf[0] = '\0';                          /* "ok" with nothing read */
    CHECK(dbq_resolve_finish(DBQ_OK, 7, buf, sizeof(buf)) == 0);
    CHECK(dbq_is_sentinel(buf));
    strcpy(buf, name);
    CHECK(dbq_resolve_finish(DBQ_OK, 7, buf, sizeof(buf)) == 0);
    CHECK(!strcmp(buf, name) && !dbq_is_sentinel(buf));
    /* a short buffer still gets a (cut) sentinel, never a name */
    CHECK(dbq_resolve_finish(DBQ_WHY_IO, 99999, buf, 6) == 0);
    CHECK(!strncmp(buf, "/<dbq", 5));

    /* the name checks that turn a bad read into DBQ_WHY_IO */
    hdr[0] = sizeof(name);
    hdr[1] = 42;
    CHECK(dbq_check_name(hdr, 42, name, sizeof(name), 261) == DBQ_OK);
    CHECK(dbq_check_name(hdr, DBQ_LOST, name, sizeof(name), 261) == DBQ_OK);
    CHECK(dbq_check_name(hdr, 41, name, sizeof(name), 261) == DBQ_WHY_IO);
    CHECK(dbq_check_name(hdr, 42, name, 5, 261) == DBQ_WHY_IO);  /* short */
    hdr[0] = 400;
    CHECK(dbq_check_name(hdr, 42, name, sizeof(name), 261) == DBQ_WHY_IO);
    hdr[0] = 4;                                     /* no NUL */
    CHECK(dbq_check_name(hdr, 42, name, sizeof(name), 261) == DBQ_WHY_IO);
    hdr[0] = 0;
    CHECK(dbq_check_name(hdr, 42, name, sizeof(name), 261) == DBQ_WHY_IO);
}

/* -- H2: rebase over every slot ------------------------------------- */
#define NE 64
struct snap {
    uint32_t fp[NE];
    int32_t seek[NE];
    bool del[NE];
    int n;
};
struct two { struct snap *s[2]; int yields; };

static int fp_cb(void *c, int which, uint32_t idx, uint32_t *fp, int32_t *seek)
{
    struct snap *s = ((struct two *)c)->s[which];
    if ((int)idx >= s->n)
        return DBQ_FP_RANGE;
    if (s->del[idx])
        return DBQ_FP_DELETED;
    *fp = s->fp[idx];
    *seek = s->seek[idx];
    return DBQ_FP_OK;
}

static void yield_cb(void *c) { ((struct two *)c)->yields++; }

static void test_h2_rebase(void)
{
    static struct snap old, nw;
    struct two t = { { &old, &nw }, 0 };
    struct dbq_fp_ops ops = { fp_cb, yield_cb, &t };
    uint32_t vals[40], before[40];
    int i;

    old.n = nw.n = NE;
    for (i = 0; i < NE; i++)
    {
        old.fp[i] = nw.fp[i] = 1000 + i;
        old.seek[i] = 5000 + 64 * i;
        nw.seek[i] = 9000 + 64 * i;
    }
    /* a queue of 40 slots; playback is at slot 30: 0..29 are history */
    for (i = 0; i < 40; i++)
        vals[i] = before[i] = (i * 13) % NE;
    old.del[vals[3]] = true;            /* deleted before the commit */
    nw.del[vals[5]] = true;             /* deleted by this commit */
    CHECK(dbq_rebase(vals, 40, &ops) == DBQ_REBASE_STAMP);
    CHECK(vals[3] == DBQ_LOST);
    CHECK(vals[5] == before[5]);        /* resolves to the sentinel later */
    for (i = 0; i < 40; i++)
        if (i != 3)
            CHECK(vals[i] == before[i]);

    /* a renumbering commit: one history slot's entry changed. Every slot
     * (history and future) becomes its old filename seek */
    for (i = 0; i < 40; i++)
        vals[i] = before[i];
    old.del[vals[3]] = false;
    nw.fp[vals[2]] ^= 0x55;             /* slot 2 is history */
    CHECK(dbq_rebase(vals, 40, &ops) == DBQ_REBASE_SEEK);
    for (i = 0; i < 40; i++)
        CHECK(vals[i] == (uint32_t)(5000 + 64 * before[i]));

    /* deleted in the old snapshot, or an old LOST: LOST after SEEK too */
    for (i = 0; i < 40; i++)
        vals[i] = before[i];
    vals[0] = DBQ_LOST;
    old.del[vals[39]] = true;
    CHECK(dbq_rebase(vals, 40, &ops) == DBQ_REBASE_SEEK);
    CHECK(vals[0] == DBQ_LOST && vals[39] == DBQ_LOST);
    CHECK(vals[29] == (uint32_t)(5000 + 64 * before[29]));
    old.del[before[39]] = false;
    nw.fp[before[2]] ^= 0x55;

    /* M-D: a rebuild that shrank the library: an index past the new
     * snapshot's entries is a renumbering (SEEK), never a deletion */
    for (i = 0; i < 40; i++)
        vals[i] = before[i];
    nw.n = 30;
    CHECK(dbq_rebase(vals, 40, &ops) == DBQ_REBASE_SEEK);
    for (i = 0; i < 40; i++)
        CHECK(vals[i] == (uint32_t)(5000 + 64 * before[i]));
    nw.n = NE;

    /* M-A: the in-place pieces, a chunk at a time, agree with the whole */
    {
        uint32_t a[40];
        bool mism = false;
        for (i = 0; i < 40; i++)
            a[i] = before[i];
        nw.fp[before[33]] ^= 1;
        for (i = 0; i < 40 && !mism; i += 8)
            mism = dbq_rebase_check(a + i, 8, &ops);
        CHECK(mism);
        for (i = 0; i < 40; i += 8)
            dbq_rebase_seek(a + i, 8, &ops);
        for (i = 0; i < 40; i++)
            CHECK(a[i] == (uint32_t)(5000 + 64 * before[i]));
        nw.fp[before[33]] ^= 1;
        /* while converting: slots below the split resolve as SEEK */
        CHECK(dbq_slot_mode(DBQ_MODE_IDX, 16, 15) == DBQ_MODE_SEEK);
        CHECK(dbq_slot_mode(DBQ_MODE_IDX, 16, 16) == DBQ_MODE_IDX);
        CHECK(dbq_slot_mode(DBQ_MODE_PATH, 16, 3) == DBQ_MODE_PATH);
        CHECK(dbq_slot_mode(DBQ_MODE_IDX, 0, 0) == DBQ_MODE_IDX);
    }

    /* 50k-like: yields while it goes */
    {
        uint32_t *big = malloc(3000 * sizeof(uint32_t));
        t.yields = 0;
        for (i = 0; i < 3000; i++)
            big[i] = i % NE;
        CHECK(dbq_rebase(big, 3000, &ops) == DBQ_REBASE_STAMP);
        CHECK(t.yields >= 3000 / 256 - 1);
        free(big);
    }
}

/* -- H4: reserve with keep ------------------------------------------ */
static void test_h4_reserve(void)
{
    unsigned long src[8], dst[16];
    int i;

    CHECK(dbq_reserve_target(2000, 2000, 1500, 0, false) == 0);
    CHECK(dbq_reserve_target(2000, 2000, 50000, 0, false) == 50000);
    CHECK(dbq_reserve_target(2000, 2000, 50000, 1999, true) == 50000);
    CHECK(dbq_reserve_target(50000, 2000, 10, 0, false) == 2000);  /* shrink */
    /* keep never shrinks: an insert into a queue with headroom (review
     * floor run: a 49,001-row T insert after a 49,257 reserve) */
    CHECK(dbq_reserve_target(50000, 2000, 3000, 4000, true) == 0);
    CHECK(dbq_reserve_target(49257, 2000, 49001, 0, true) == 0);
    CHECK(dbq_reserve_target(2000, 2000, 1500, 1800, true) == 0);
    CHECK(dbq_reserve_target(2000, 2000, 3000, 4000, true) == -1);
    CHECK(dbq_reserve_target(50000, 2000, 50000, 4000, true) == 0);
    for (i = 0; i < 8; i++)
        src[i] = 0x80000000ul | DBQ_FLAG | i;
    memset(dst, 0, sizeof(dst));
    CHECK(dbq_reserve_copy(dst, src, 8, 16, true) == 8);
    CHECK(!memcmp(dst, src, sizeof(src)) && dst[8] == 0);
    memset(dst, 0, sizeof(dst));
    CHECK(dbq_reserve_copy(dst, src, 8, 16, false) == 0 && dst[0] == 0);
}

/* M-C: only "nothing to resume" and an unconfirmed database block keep
 * a resume from playing; a torn path queue still resumes */
static void test_resume_codes(void)
{
    int rc;
    CHECK(dbq_resume_refused(-1) && dbq_resume_refused(-19));
    for (rc = -18; rc <= 0; rc++)
        if (rc != -1)
            CHECK(!dbq_resume_refused(rc));
    CHECK(!dbq_resume_refused(-15));
}

static void test_perm(void)
{
    uint32_t key[7] = { 50, DBQ_LOST, 10, 30, 10, 0, 99 };
    uint32_t perm[7];
    int i;

    dbq_sort_perm(perm, key, 7);
    for (i = 1; i < 7; i++)
        CHECK(key[perm[i - 1]] <= key[perm[i]]);
    CHECK(perm[6] == 1 && perm[1] == 2 && perm[2] == 4);
}

/* -- the cut model (tests/fatcrash's idea, in memory) ---------------
 * Files: the control file's T line, dbq.hdr as safefile keeps it (dest,
 * prev: a cut replace recovers to one of them whole), two payload files.
 * Every write of design 5.2, a mode change and a materialization is cut
 * after each step; the boot either resumes a queue whose every value is
 * that queue's, or resumes nothing. */
#define QN 6
struct pay { struct dbq_block b; uint32_t v[QN]; int valid_words; };
struct disk {
    bool has_t; uint32_t t_tag; uint32_t t_count;
    struct dbq_hdr hdr;
    struct pay pay[2];
    bool path_new;              /* dbqpath.dat holds the new names */
};

static const struct dbq_entry *boot(const struct disk *d, uint32_t *out)
{
    const struct dbq_entry *e;
    const struct pay *p;
    uint32_t vals[QN];

    if (!d->has_t)
        return NULL;
    e = dbq_hdr_find(&d->hdr, d->t_tag, 0, d->t_count);
    if (!e)
        return NULL;            /* -19: no resume (S1) */
    p = &d->pay[e->file];
    memset(vals, 0xEE, sizeof(vals));
    memcpy(vals, p->v, p->valid_words * sizeof(uint32_t));
    if (!dbq_block_check(&p->b, vals, e->seq, e->count))
        return NULL;
    if (p->b.mode != e->mode)
        return NULL;
    memcpy(out, vals, sizeof(vals));
    return e;
}

static void write_pay(struct pay *p, uint32_t seq, uint32_t mode,
                      const uint32_t *v, int words)
{
    memcpy(p->v, v, sizeof(p->v));
    dbq_block_seal(&p->b, seq, QN, mode, v);
    p->valid_words = words;     /* < QN: cut inside the write */
}

static void hdr_set(struct dbq_hdr *h, uint32_t seq, unsigned file,
                    uint8_t mode, const struct dbq_entry *prev)
{
    struct dbq_hdr n;
    memset(&n, 0, sizeof(n));
    n.nblocks = 1;
    n.next_seq = dbq_seq_next(seq);
    n.cur_file = file;
    n.e[0].seq = seq;
    n.e[0].file = file;
    n.e[0].mode = mode;
    n.e[0].count = QN;
    if (prev && prev->mode == DBQ_MODE_IDX && prev->file != file)
    {
        n.e[1] = *prev;
        n.nblocks = 2;
    }
    dbq_hdr_seal(&n);
    *h = n;
}

static bool same(const uint32_t *a, const uint32_t *b)
{
    return !memcmp(a, b, QN * sizeof(uint32_t));
}

static void test_cuts(void)
{
    static const uint32_t q1[QN] = { 1, 2, 3, 4, 5, 6 };
    static const uint32_t q2[QN] = { 60, 50, 40, 30, 20, 10 };
    static const uint32_t q2s[QN] = { 600, 500, 400, 300, 200, 100 };
    struct disk base, d;
    uint32_t got[QN];
    const struct dbq_entry *e;
    int step;

    /* Q1 (seq 1) in dbq0.dat, committed */
    memset(&base, 0, sizeof(base));
    write_pay(&base.pay[0], 1, DBQ_MODE_IDX, q1, QN);
    hdr_set(&base.hdr, 1, 0, DBQ_MODE_IDX, NULL);
    base.has_t = true; base.t_tag = dbq_tag(0, 1); base.t_count = QN;
    e = boot(&base, got);
    CHECK(e && same(got, q1));

    /* a new queue Q2 (seq 2): 5.2 steps 1-4, cut after each */
    for (step = 0; step <= 6; step++)
    {
        d = base;
        if (step >= 1) d.has_t = false;                  /* 1: P only */
        if (step >= 2) write_pay(&d.pay[1], 2, DBQ_MODE_IDX, q2,
                                 step == 2 ? 3 : QN);    /* 2: torn, 3: whole */
        if (step >= 4) hdr_set(&d.hdr, 2, 1, DBQ_MODE_IDX, &base.hdr.e[0]);
        if (step >= 5) { d.has_t = true; d.t_tag = dbq_tag(1, 2);
                         d.t_count = QN; }               /* 4: T + fsync */
        e = boot(&d, got);
        if (step == 0)
            CHECK(e && same(got, q1));
        else if (step < 5)
            CHECK(!e);          /* no T: empty queue, no resume */
        else
            CHECK(e && same(got, q2));
    }

    /* M5: the control swap is cut and recovery puts the previous control
     * file back (its T names Q1), with dbq.hdr new or old */
    d = base;
    write_pay(&d.pay[1], 2, DBQ_MODE_IDX, q2, QN);
    hdr_set(&d.hdr, 2, 1, DBQ_MODE_IDX, &base.hdr.e[0]);
    d.has_t = true; d.t_tag = dbq_tag(0, 1); d.t_count = QN;
    e = boot(&d, got);
    CHECK(e && e->seq == 1 && same(got, q1));   /* dbq0.dat was not touched */
    d.hdr = base.hdr;
    e = boot(&d, got);
    CHECK(e && e->seq == 1 && same(got, q1));
    /* a third queue overwrote dbq0.dat: Q1's T no longer resumes */
    write_pay(&d.pay[0], 3, DBQ_MODE_IDX, q1, QN);
    hdr_set(&d.hdr, 3, 0, DBQ_MODE_IDX, NULL);
    CHECK(!boot(&d, got));

    /* a mode change (IDX -> SEEK) of Q2: the other file, then dbq.hdr */
    memset(&base, 0, sizeof(base));
    write_pay(&base.pay[1], 2, DBQ_MODE_IDX, q2, QN);
    hdr_set(&base.hdr, 2, 1, DBQ_MODE_IDX, NULL);
    base.has_t = true; base.t_tag = dbq_tag(1, 2); base.t_count = QN;
    for (step = 0; step <= 3; step++)
    {
        d = base;
        if (step >= 1) write_pay(&d.pay[0], 2, DBQ_MODE_SEEK, q2s,
                                 step == 1 ? 2 : QN);
        if (step >= 3) hdr_set(&d.hdr, 2, 0, DBQ_MODE_SEEK, NULL);
        e = boot(&d, got);
        CHECK(e != NULL);
        if (step < 3)
            CHECK(e->mode == DBQ_MODE_IDX && same(got, q2));
        else
            CHECK(e->mode == DBQ_MODE_SEEK && same(got, q2s));
    }

    /* a materialization (SEEK -> PATH): dbqpath.dat, the other file, the
     * header. Before the header lands the block is still SEEK (valid in
     * the retained generation, which the prepare gate keeps) */
    base = d;
    for (step = 0; step <= 3; step++)
    {
        static const uint32_t offs[QN] = { 0, 20, 40, 60, 80, 100 };
        d = base;
        if (step >= 1) d.path_new = true;
        if (step >= 2) write_pay(&d.pay[1], 2, DBQ_MODE_PATH, offs, QN);
        if (step >= 3) hdr_set(&d.hdr, 2, 1, DBQ_MODE_PATH, NULL);
        e = boot(&d, got);
        CHECK(e != NULL);
        if (step < 3)
            CHECK(e->mode == DBQ_MODE_SEEK && same(got, q2s));
        else
            CHECK(e->mode == DBQ_MODE_PATH && same(got, offs) && d.path_new);
    }
}

int main(void)
{
    test_tag();
    test_seal();
    test_modes();
    test_sort_order();
    test_h1_sentinel();
    test_h2_rebase();
    test_h4_reserve();
    test_perm();
    test_resume_codes();
    test_cuts();
    if (fails)
    {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    return 0;
}
