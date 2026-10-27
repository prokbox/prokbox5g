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
/* The books.dat runtime (apps/ipodui/ab_store.c, AB_HOST_TEST) against
 * real files: inserts, in-place updates, the storage-idle path, the
 * seed, eviction at the cap, books.tmp recovery, a bad header copy, a
 * torn sector, and a power cut at every 61st byte of an insert and of
 * an update (the store must read as the old or the new state, never
 * lose another book). */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "ab_host.h"
#include "ab_store.h"
#include "check.h"

char ab_host_dat[512], ab_host_tmp[512], ab_host_old[512];
long current_tick;
long ab_host_cut = -1;
long ab_host_meta_cut = -1;
int ab_host_fail_remove;
static struct ab_book_block block;
static int wakes, traces_idle, traces_evict, traces_seed;
static struct ab_rec seed_src[8];
static int seed_n;
static void (*idle_fn)(void);

void ab_host_trace(const char *fmt, ...)
{
    if (!strncmp(fmt, "ab store w=%s", 13))
    {
        va_list ap;
        va_start(ap, fmt);
        traces_idle += !strcmp(va_arg(ap, const char *), "idle");
        va_end(ap);
    }
    traces_evict += !strncmp(fmt, "ab store evict", 14);
    traces_seed += !strncmp(fmt, "ab seed", 7);
}

ssize_t ab_host_write(int fd, const void *p, size_t n)
{
    ssize_t r;

    if (ab_host_cut == 0)
        return -1;
    if (ab_host_cut > 0 && (long)n > ab_host_cut)
    {
        r = (write)(fd, p, (size_t)ab_host_cut);   /* torn */
        ab_host_cut = 0;
        (void)r;
        return -1;
    }
    r = (write)(fd, p, n);
    if (ab_host_cut > 0)
        ab_host_cut -= (long)n;
    return r;
}

static bool meta_cut(void)
{
    if (ab_host_meta_cut == 0)
        ab_host_cut = 0;                /* power is gone: writes too */
    else if (ab_host_meta_cut > 0)
        ab_host_meta_cut--;
    return ab_host_cut == 0;
}

int ab_host_rename(const char *a, const char *b)
{
    if (meta_cut())
        return -1;
    return (rename)(a, b);
}

int ab_host_remove(const char *a)
{
    if (meta_cut())
        return -1;
    if (ab_host_fail_remove > 0 && access(a, F_OK) == 0)
    {
        ab_host_fail_remove--;
        errno = EIO;
        return -1;
    }
    return (remove)(a);
}

void register_storage_idle_func(void (*fn)(void)) { idle_fn = fn; }
struct ab_book_block *ab_book_block(void) { return &block; }
_Bool ab_book_checkpoint(struct ab_rec *r, _Bool stopping)
{
    (void)r; (void)stopping;
    return 0;
}
static _Bool host_playing;
_Bool ab_book_playing(void) { return host_playing; }
void np_pb_ab_wake(void) { wakes++; }
static struct ab_rec seed_buf[256];
int lib_book_seed_begin(size_t bytes, void **buf)
{
    if (bytes > sizeof(seed_buf))
        return -1;
    *buf = seed_buf;
    return 1;
}
int lib_book_seed(struct ab_rec *out, int max)
{
    int i;
    for (i = 0; i < seed_n && i < max; i++)
        out[i] = seed_src[i];
    return i;
}
void lib_book_seed_end(int h) { (void)h; }

static ab_key_t key_of(int i)
{
    return 0x9e3779b97f4a7c15ull * (uint64_t)(i + 1);
}

static void rec_for(struct ab_rec *r, int i, uint32_t ms, bool finished)
{
    ab_rec_set(r, key_of(i), 0x1000 + i, i % 7, ms, ms * 4, ms + 1000 * i,
               finished ? ms + 1000 * i : 0, finished,
               block.store.clock++);
}

static void wipe(void)
{
    unlink(ab_host_dat);
    unlink(ab_host_tmp);
    memset(&block, 0, sizeof(block));
}

static int count_ok(int n, uint32_t ms)
{
    int i, ok = 0;
    struct ab_rec r;

    for (i = 0; i < n; i++)
        ok += ab_store_get(key_of(i), &r) && r.elapsed_ms == ms;
    return ok;
}

static void copy_file(const char *a, const char *b)
{
    char buf[4096];
    FILE *f = fopen(a, "rb"), *g = fopen(b, "wb");
    size_t n;

    while (f && g && (n = fread(buf, 1, sizeof(buf), f)) > 0)
        fwrite(buf, 1, n, g);
    if (f) fclose(f);
    if (g) fclose(g);
}

static void test_basic(void)
{
    struct ab_rec r, g;
    struct stat st;
    int i;

    wipe();
    CHECK(!ab_store_load());
    for (i = 0; i < 40; i++)
    {
        rec_for(&r, i, 5000, false);
        CHECK(ab_store_put(&r));
    }
    CHECK(ab_store_load());
    CHECK_EQ(block.store.count, 40);
    CHECK_EQ(block.store.sectors, 3);
    CHECK_EQ(count_ok(40, 5000), 40);
    CHECK(stat(ab_host_dat, &st) == 0);
    CHECK_EQ(st.st_size, ab_store_file_size(40));
    /* in place: same size, new value */
    rec_for(&r, 17, 7777, false);
    CHECK(ab_store_put(&r));
    CHECK(stat(ab_host_dat, &st) == 0);
    CHECK_EQ(st.st_size, ab_store_file_size(40));
    CHECK(ab_store_load());
    CHECK(ab_store_get(key_of(17), &g) && g.elapsed_ms == 7777);
    CHECK_EQ(ab_store_find(key_of(17)), AB_REC_STARTED);
    CHECK_EQ(ab_store_find(key_of(99)), AB_REC_NONE);
    /* finished: no spot (Rev 2) */
    rec_for(&r, 18, 1000, true);
    CHECK(ab_store_put(&r));
    CHECK_EQ(ab_store_find(key_of(18)), AB_REC_FINISHED);
}

static void test_idle(void)
{
    struct ab_rec r, g;

    /* storage thread: an existing book in place, traced idle */
    rec_for(&r, 3, 4242, false);
    ab_store_pending(&r);
    traces_idle = 0;
    ab_store_idle(false);
    CHECK_EQ(traces_idle, 1);
    CHECK(ab_store_get(key_of(3), &g) && g.elapsed_ms == 4242);
    /* a new book: never inserted on the storage thread */
    rec_for(&r, 60, 100, false);
    ab_store_pending(&r);
    wakes = 0;
    ab_store_idle(false);
    CHECK_EQ(wakes, 1);
    CHECK_EQ(ab_store_find(key_of(60)), AB_REC_NONE);
    ab_store_idle(true);                /* the UI pass inserts it */
    CHECK_EQ(ab_store_find(key_of(60)), AB_REC_STARTED);
    CHECK_EQ(block.store.count, 41);
}

/* H1: a stop's record (write-now) wins over an older track-change
 * checkpoint, whichever path writes it, and nothing older than the last
 * write of a book ever reaches the file */
static void test_stale(void)
{
    struct ab_rec old, newer, g;

    rec_for(&old, 7, 1111, false);      /* track change: older stamp */
    rec_for(&newer, 7, 2222, false);    /* the stop */
    ab_store_pending(&old);
    ab_store_pending_now(&newer);
    ab_store_pending(&old);             /* older than pending: dropped */
    ab_store_idle(true);                /* the UI pass */
    CHECK(ab_store_get(key_of(7), &g) && g.elapsed_ms == 2222);
    ab_store_pending(&old);             /* older than the last write */
    ab_store_idle(true);
    CHECK(ab_store_get(key_of(7), &g) && g.elapsed_ms == 2222);
    /* the flush (shutdown: idle callback on the UI thread) writes a
     * write-now that no UI pass saw */
    rec_for(&newer, 7, 3333, false);
    ab_store_pending_now(&newer);
    CHECK(idle_fn != NULL);
    idle_fn();
    CHECK(ab_store_get(key_of(7), &g) && g.elapsed_ms == 3333);
    ab_store_idle(false);               /* nothing pending: no write */
    CHECK(ab_store_get(key_of(7), &g) && g.elapsed_ms == 3333);
    rec_for(&newer, 7, 5000, false);    /* restore for the later tests */
    CHECK(ab_store_put(&newer));
}

/* a cut at every 61st byte of one insert: old or new, nothing else lost */
static void test_cut_insert(void)
{
    char base[600];
    long cut;
    int olds = 0, news = 0;

    snprintf(base, sizeof(base), "%s.base", ab_host_dat);
    copy_file(ab_host_dat, base);
    for (cut = 0; cut < ab_store_file_size(42) + 1024; cut += 61)
    {
        struct ab_rec r;

        copy_file(base, ab_host_dat);
        unlink(ab_host_tmp);
        CHECK(ab_store_load());
        rec_for(&r, 61, 900, false);
        ab_host_cut = cut;
        ab_store_put(&r);
        ab_host_cut = -1;
        CHECK(ab_store_load());         /* reboot */
        CHECK_EQ(count_ok(3, 5000), 3);
        if (ab_store_find(key_of(61)) == AB_REC_STARTED)
            news++;
        else
            olds++;
        CHECK(block.store.count == 41 || block.store.count == 42);
        CHECK_EQ(ab_store_find(key_of(60)), AB_REC_STARTED);
        CHECK_EQ(ab_store_find(key_of(17)), AB_REC_STARTED);
    }
    CHECK(olds > 0 && news > 0);
    copy_file(base, ab_host_dat);
    unlink(base);
    CHECK(ab_store_load());
}

/* #91: a cut after every rename/remove of an insert's safe replace, and
 * the replace's victim removal failing: old or new, never neither, no
 * books.tmp/books.old left after the reboot's load */
static void test_cut_replace(void)
{
    char base[600];
    int olds = 0, news = 0;

    snprintf(base, sizeof(base), "%s.base", ab_host_dat);
    copy_file(ab_host_dat, base);
    for (long n = 0; n <= 8; n++)
    {
        struct ab_rec r;

        copy_file(base, ab_host_dat);
        unlink(ab_host_tmp);
        unlink(ab_host_old);
        CHECK(ab_store_load());
        rec_for(&r, 61, 900, false);
        ab_host_meta_cut = n;
        ab_store_put(&r);
        ab_host_meta_cut = -1;
        ab_host_cut = -1;
        CHECK(ab_store_load());         /* reboot */
        CHECK(access(ab_host_dat, F_OK) == 0);
        CHECK(access(ab_host_tmp, F_OK) != 0);
        CHECK(access(ab_host_old, F_OK) != 0);
        CHECK_EQ(count_ok(3, 5000), 3);
        if (ab_store_find(key_of(61)) == AB_REC_STARTED)
            news++;
        else
            olds++;
        CHECK(block.store.count == 41 || block.store.count == 42);
        CHECK_EQ(ab_store_find(key_of(60)), AB_REC_STARTED);
    }
    CHECK(olds > 0 && news > 0);
    /* a stale books.old whose removal fails (EIO): the insert fails,
     * books.dat keeps the old state, no rename lands on a name */
    {
        struct ab_rec r;

        copy_file(base, ab_host_dat);
        copy_file(base, ab_host_old);
        CHECK(ab_store_load());         /* removes books.old */
        CHECK(access(ab_host_old, F_OK) != 0);
        copy_file(base, ab_host_old);
        ab_host_fail_remove = 1;
        rec_for(&r, 61, 900, false);
        ab_store_put(&r);
        ab_host_fail_remove = 0;
        CHECK(ab_store_load());
        CHECK(access(ab_host_tmp, F_OK) != 0);
        CHECK(block.store.count == 41 || block.store.count == 42);
        CHECK_EQ(ab_store_find(key_of(60)), AB_REC_STARTED);
    }
    copy_file(base, ab_host_dat);
    unlink(base);
    CHECK(ab_store_load());
}

/* a cut inside an in-place update: that record previous or unplayed,
 * every other record intact */
static void test_cut_update(void)
{
    long cut;

    for (cut = 0; cut <= 32; cut += 4)
    {
        struct ab_rec r, g;

        CHECK(ab_store_load());
        rec_for(&r, 5, 1234, false);
        ab_host_cut = cut;
        ab_store_put(&r);
        ab_host_cut = -1;
        CHECK(ab_store_load());
        CHECK(!ab_store_get(key_of(5), &g)
              || g.elapsed_ms == 1234 || g.elapsed_ms == 5000);
        CHECK(ab_store_get(key_of(4), &g) && g.elapsed_ms == 5000);
        CHECK(ab_store_get(key_of(6), &g) && g.elapsed_ms == 5000);
        rec_for(&r, 5, 5000, false);    /* restore */
        ab_store_put(&r);
    }
}

static void test_recovery(void)
{
    FILE *f;
    char z[512];

    /* crash between remove and rename: books.tmp is complete */
    CHECK(rename(ab_host_dat, ab_host_tmp) == 0);
    CHECK(ab_store_load());
    CHECK_EQ(block.store.count, 41);
    CHECK(access(ab_host_tmp, F_OK) != 0);
    /* sector 0 lost: the tail copy */
    memset(z, 0xa5, sizeof(z));
    f = fopen(ab_host_dat, "r+b");
    fwrite(z, 1, sizeof(z), f);
    fclose(f);
    CHECK(ab_store_load());
    CHECK_EQ(block.store.count, 41);
    CHECK_EQ(count_ok(3, 5000), 3);
    /* a torn record sector: at most its 16 records lost, the rest kept */
    f = fopen(ab_host_dat, "r+b");
    fseek(f, 512 + 100, SEEK_SET);
    fwrite(z, 1, 200, f);
    fclose(f);
    CHECK(ab_store_load());
    {
        int i, ok = 0;
        struct ab_rec g;
        for (i = 0; i < 40; i++)
            ok += ab_store_get(key_of(i), &g);
        CHECK(ok >= 40 - 16 - 1);
    }
}

static void test_seed(void)
{
    struct ab_rec g;

    wipe();
    CHECK(!ab_store_load());
    rec_for(&seed_src[0], 200, 300, false);
    rec_for(&seed_src[1], 201, 400, false);
    rec_for(&seed_src[2], 200, 999, false);  /* same book: one kept */
    seed_n = 3;
    traces_seed = 0;
    ab_store_seed();
    CHECK_EQ(traces_seed, 1);
    CHECK(ab_store_get(key_of(201), &g));
    CHECK(g.flags & AB_REC_F_SEEDED);
    CHECK_EQ(block.store.count, 2);
    ab_store_seed();                    /* once */
    CHECK_EQ(traces_seed, 1);
    CHECK(ab_store_load());
    ab_store_seed();                    /* persisted: still once */
    CHECK_EQ(traces_seed, 1);
}

static void test_evict(void)
{
    struct ab_rec r;
    int i;

    wipe();
    ab_store_load();
    /* fill to the cap with one batch through the seed path */
    seed_n = 0;
    for (i = 0; i < AB_STORE_MAX; i++)
    {
        rec_for(&r, i, 10, i == 100 || i == 900);
        if (i == 900)
            r.stamp = 0, ab_rec_seal(&r);   /* the oldest FINISHED */
        CHECK(ab_store_put(&r));
    }
    CHECK_EQ(block.store.count, AB_STORE_MAX);
    traces_evict = 0;
    rec_for(&r, AB_STORE_MAX + 5, 10, false);
    CHECK(ab_store_put(&r));
    CHECK_EQ(traces_evict, 1);
    CHECK_EQ(block.store.count, AB_STORE_MAX);
    CHECK_EQ(ab_store_find(key_of(900)), AB_REC_NONE);
    CHECK_EQ(ab_store_find(key_of(100)), AB_REC_FINISHED);
    CHECK_EQ(ab_store_find(key_of(AB_STORE_MAX + 5)), AB_REC_STARTED);
    /* next: the other FINISHED goes before any STARTED */
    rec_for(&r, AB_STORE_MAX + 6, 10, false);
    CHECK(ab_store_put(&r));
    CHECK_EQ(ab_store_find(key_of(100)), AB_REC_NONE);
    CHECK_EQ(ab_store_find(key_of(0)), AB_REC_STARTED);
}

/* ---- #67: the book to pin (Phase 8b §3.2) ------------------------------- */

static ab_key_t pin(enum ab_pin_src *src)
{
    ab_key_t k = ab_store_pin_key(NULL);

    if (src)
        *src = ab_store_pin_src();
    return k;
}

/* flips a byte of key's record in books.dat (its check then fails) */
static bool corrupt(ab_key_t key)
{
    FILE *f = fopen(ab_host_dat, "r+b");
    struct ab_rec r;
    long off;
    bool done = false;

    if (!f)
        return false;
    for (off = AB_STORE_SECTOR; !done && fseek(f, off, SEEK_SET) == 0
         && fread(&r, sizeof(r), 1, f) == 1; off += sizeof(r))
    {
        if (r.book_key != key)
            continue;
        r.elapsed_ms ^= 0x5a;
        done = fseek(f, off, SEEK_SET) == 0
               && fwrite(&r, sizeof(r), 1, f) == 1;
    }
    fclose(f);
    return done;
}

static void put_stamped(int i, uint32_t stamp, uint16_t extra_flags)
{
    struct ab_rec r;

    rec_for(&r, i, 100, false);
    r.stamp = stamp;
    r.flags |= extra_flags;
    ab_rec_seal(&r);
    CHECK(ab_store_put(&r));
}

static void test_pin(void)
{
    struct ab_rec r;
    enum ab_pin_src src;
    uint32_t g0, g1;
    int i;

    ab_store_idle(true);                /* earlier tests' pending: written */
    wipe();
    seed_n = 0;
    host_playing = false;
    CHECK(!ab_store_load());
    CHECK_EQ(pin(&src), 0);
    CHECK_EQ(src, AB_PIN_NONE);

    /* seeded only (a fresh migration, nothing played): no pin (O6f) */
    rec_for(&seed_src[0], 300, 100, false);
    rec_for(&seed_src[1], 301, 200, false);
    seed_n = 2;
    ab_store_seed();
    seed_n = 0;
    CHECK_EQ(block.store.count, 2);
    CHECK_EQ(pin(NULL), 0);
    CHECK(ab_store_load());             /* reboot */
    CHECK_EQ(pin(NULL), 0);

    /* pending only: the book's record is not written yet, it still wins */
    rec_for(&r, 302, 1000, false);
    ab_store_pending(&r);
    CHECK_EQ(ab_store_find(key_of(302)), AB_REC_NONE);
    CHECK_EQ(pin(&src), key_of(302));
    CHECK_EQ(src, AB_PIN_PENDING);
    ab_store_idle(true);                /* the UI pass inserts it */
    CHECK_EQ(ab_store_find(key_of(302)), AB_REC_STARTED);
    CHECK_EQ(pin(&src), key_of(302));
    CHECK_EQ(src, AB_PIN_STAMP);
    /* pending over an older written record of another book */
    rec_for(&r, 303, 1000, false);
    ab_store_pending(&r);
    CHECK_EQ(pin(&src), key_of(303));
    CHECK_EQ(src, AB_PIN_PENDING);
    ab_store_idle(true);
    CHECK_EQ(pin(&src), key_of(303));

    /* a seeded record stamped later than a played one loses to it, live
     * and after a reload */
    put_stamped(304, block.store.clock++, AB_REC_F_SEEDED);
    CHECK_EQ(pin(NULL), key_of(303));
    CHECK(ab_store_load());
    CHECK_EQ(pin(&src), key_of(303));
    CHECK_EQ(src, AB_PIN_STAMP);
    /* the real migration after a played book: the played one stays */
    wipe();
    CHECK(!ab_store_load());
    rec_for(&r, 310, 1000, true);       /* finished: still pinned */
    CHECK(ab_store_put(&r));
    rec_for(&seed_src[0], 311, 100, false);
    rec_for(&seed_src[1], 312, 200, false);
    seed_n = 2;
    ab_store_seed();                    /* stamps after 310's */
    seed_n = 0;
    CHECK_EQ(block.store.count, 3);
    CHECK_EQ(pin(NULL), key_of(310));
    CHECK(ab_store_load());
    CHECK_EQ(pin(NULL), key_of(310));

    /* load: the highest unseeded stamp across sectors (40 books = 3) */
    wipe();
    CHECK(!ab_store_load());
    for (i = 0; i < 40; i++)
    {
        rec_for(&r, i, 5000, false);
        CHECK(ab_store_put(&r));
    }
    rec_for(&r, 3, 6000, false);
    CHECK(ab_store_put(&r));
    rec_for(&r, 20, 6000, false);
    CHECK(ab_store_put(&r));
    put_stamped(21, block.store.clock++, AB_REC_F_SEEDED);
    CHECK_EQ(pin(NULL), key_of(20));
    CHECK(ab_store_load());
    CHECK_EQ(pin(NULL), key_of(20));
    /* a torn record is skipped: the next newest */
    CHECK(corrupt(key_of(20)));
    CHECK(ab_store_load());
    CHECK_EQ(pin(NULL), key_of(3));
    /* a write moves it: write-now, then the storage-idle pass */
    rec_for(&r, 7, 7000, false);
    CHECK(ab_store_put(&r));
    CHECK_EQ(pin(NULL), key_of(7));
    rec_for(&r, 9, 7000, false);
    ab_store_pending(&r);
    ab_store_idle(false);               /* storage thread, in place */
    CHECK_EQ(pin(&src), key_of(9));
    CHECK_EQ(src, AB_PIN_STAMP);        /* written: no longer pending */

    /* the stamp wraps: 5 is newer than 0xfffffff0 (stamp_before()) */
    wipe();
    CHECK(!ab_store_load());
    put_stamped(401, 5, 0);
    put_stamped(400, 0xfffffff0u, 0);
    CHECK_EQ(pin(NULL), key_of(401));
    CHECK(ab_store_load());
    CHECK_EQ(pin(NULL), key_of(401));

    /* the book playing or paused wins over pending and stamps */
    rec_for(&r, 402, 100, false);
    ab_store_pending(&r);
    block.map.book_key = key_of(500);
    host_playing = true;
    ab_store_pin_key(&g0);
    CHECK_EQ(pin(&src), key_of(500));
    CHECK_EQ(src, AB_PIN_PLAY);
    ab_store_pin_key(&g1);
    CHECK_EQ(g1, g0);                   /* same key: no move */
    host_playing = false;
    CHECK_EQ(pin(&src), key_of(402));
    CHECK_EQ(src, AB_PIN_PENDING);
    ab_store_pin_key(&g1);
    CHECK(g1 != g0);                    /* the key changed */
    block.map.book_key = 0;
    ab_store_idle(true);                /* drain (402's stamp comes from
                                           the wrapped clock: no check) */
    CHECK_EQ(ab_store_find(key_of(402)), AB_REC_STARTED);
}

int main(void)
{
    const char *t = getenv("TMPDIR");
    char dir[400];

    snprintf(dir, sizeof(dir), "%s/abstore.XXXXXX", t ? t : "/tmp");
    if (!mkdtemp(dir))
        return 1;
    snprintf(ab_host_dat, sizeof(ab_host_dat), "%s/books.dat", dir);
    snprintf(ab_host_tmp, sizeof(ab_host_tmp), "%s/books.tmp", dir);
    snprintf(ab_host_old, sizeof(ab_host_old), "%s/books.old", dir);
    test_basic();
    test_idle();
    test_stale();
    test_cut_insert();
    test_cut_update();
    test_cut_replace();
    test_recovery();
    test_seed();
    test_evict();
    test_pin();
    wipe();
    rmdir(dir);
    CHECK_DONE("test_abstore_rt");
}
