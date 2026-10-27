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

/* GPL-2.0-or-later. Real-file harness for actual extracted artcache.c book
 * persistence functions, not a duplicate implementation. The Python runner
 * extracts exactly the reviewed functions and append block at compile time.
 * Scheduling, decoding, playback concurrency and device FAT remain simulator
 * and hardware tests; this harness checks publication and byte preservation. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "artfmt.h"
#include "ab_book.h"

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define ALIGN_UP(a,b) (((a) + (b) - 1) & ~((b) - 1))
#define UPDATE_TRACE(...) ((void)0)
#define BOOKS_BATCH 8
#define BIDX_HDR ((off_t)sizeof(struct ab_idx_hdr))
#define BIDX_PAGE ((int)(ARTIDX_PAGE_BYTES / sizeof(struct ab_idx_rec)))
static char F_BIDX[1024], F_BIDX_TMP[1024], F_BIDX_PREV[1024], F_BPAK[1024];
#define AB_IDX_PATH F_BIDX
#define AB_IDX_PREV F_BIDX_PREV
static bool books_owed, fail_open, fail_read, fail_write, fail_flush;
static int audio_status(void) { return 0; }
static bool art_hold(void) { return audio_status() != 0; }
static bool io_ok(void) { return true; }
/* the paused window (apps/ipodui/maint.h, #88): a book owed is noted */
enum maint_job { MAINT_COMMIT, MAINT_CHAPTERS, MAINT_DBQ, MAINT_LISTS };
static void maint_want(enum maint_job j) { (void)j; }
static int h_open(const char *p, int f, ...)
{
    if (fail_open) { errno = EIO; return -1; }
    return open(p, f, 0666);
}
static ssize_t h_read(int fd, void *p, size_t n)
{
    if (fail_read) { errno = EIO; return -1; }
    return read(fd, p, n);
}
static ssize_t h_write(int fd, const void *p, size_t n)
{
    if (fail_write) { errno = ENOSPC; return -1; }
    return write(fd, p, n);
}
static int h_flush(int fd)
{
    if (fail_flush) { errno = EIO; return -1; }
    return fsync(fd);
}
#define open h_open
#define read h_read
#define write h_write
#define fsync h_flush
struct bwork {
    uint32_t page[ARTIDX_PAGE_BYTES / 4];
    struct ab_idx_hdr bhdr;
    bool bhdr_ok, books_defer, failed, deferred;
    const char *bpath;
    int n_bpend;
    struct ab_idx_rec bpend[BOOKS_BATCH];
    struct artidx_reader rd;
    struct artalb_hdr thdr;
    struct artalb_rec trec[2];
    struct artcache_rec delta[8];
    int n_delta;
    const char *tpath;
    char path[1024];
    struct { int chapter_count; } id3;
    struct chap_skip { uint64_t key; uint32_t max_mtime; int tries; } chskip[8];
    int n_chskip;
};
#define FAIL(w) ((w)->failed = true)    /* artcache.c: + the log line */
#define DEFER(w) ((w)->failed = (w)->deferred = true)
#define ui_log(tag, ...) ((void)(tag))
#define ART_PACE() ((void)0)            /* artcache.c: a yield point */
#include "books_actual.inc"

/* Compile the actual run loops with controlled producers. No firmware
 * scheduling claims: these verify error/status propagation at their boundary. */
#define TCHUNK 2
#define DELTA_MAX 8
#define CHAP_DEFERRED (-2)
#define CHAP_CAP (-3)
#define BK_UNBUILDABLE (-4)
#define CHAP_IO (-5)
#define CHAP_LATER (-6)
#define CHAP_SKIP_MAX 8
#define CHAP_TRIES 3
#define EV_RETRY 8
#define ARTCACHE_BUILD_PHASE_BOOKS 1
#define ARTCACHE_BUILD_PHASE_CHAPTERS 2
static bool chapters_owed, chap_later, chap_stale, cap_fits = true, lookup_error;
static int current_tick, table_result, chapter_result, book_result;
static void build_phase(int phase, int count) { (void)phase; (void)count; }
static struct { int phase_done, phase_total; } status;
static void build_advance(void) { status.phase_done++; }
static bool playing(void) { return false; }
static int wait_ready(bool wait) { (void)wait; return 0; }
static bool test_artchap_fits(int a, int b, int c)
{ (void)a; (void)b; (void)c; return cap_fits; }
#define artchap_fits test_artchap_fits
static bool chapter_lookup(struct artidx_reader *r, uint64_t key,
                            struct artcache_rec *out, bool *io_err)
{
    (void)r; *io_err = lookup_error;
    memset(out, 0, sizeof(*out)); out->key = key; out->mtime = 1;
    return !lookup_error;
}
#define artidx_lookup chapter_lookup
static int test_artalb_read(const char *path, const struct artalb_hdr *h,
                            uint32_t first, int n, struct artalb_rec *out)
{
    (void)path; (void)h; (void)n;
    if (table_result < 0) return -1;
    if (first) return 0;
    memset(out, 0, sizeof(*out));
    out->key = 99; out->count = 1; out->max_mtime = 1;
    out->flags = LIBFILE_ALB_BOOK;
    return 1;
}
#define artalb_read test_artalb_read
static bool commit(struct bwork *w, bool final)
{ (void)final; w->n_delta = 0; return true; }
static bool commit_ex(struct bwork *w, bool a, bool b)
{ (void)b; return commit(w, a); }
static int chapter_book(struct bwork *w, const struct artalb_rec *t,
                         const struct artcache_rec *b)
{ (void)w; (void)t; (void)b; return chapter_result; }
static bool known_skip;
static bool bk_skipped(struct bwork *w, const struct artalb_rec *t)
{ (void)w; (void)t; return known_skip; }
static void bk_skip(struct bwork *w, const struct artalb_rec *t)
{ (void)w; (void)t; }
static int book_build(struct bwork *w, const struct artalb_rec *t,
                       const struct ab_idx_rec *old)
{
    (void)old;
    if (book_result == 1)
    {
        int rc = test_append(w, t->key);
        fail_flush = true; /* fail final index flush, not the blob append */
        return rc;
    }
    return book_result;
}
#include "books_loops.inc"

/* Actual metadata-to-fallback gate, with parser failures controlled. */
#define AB_MAX_CHAPTERS 1024
#define AB_CH_POOL 24576
static bool metadata_ok = true;
static int metadata_count, chapter_read_result;
static bool get_metadata(void *id3, int fd, const char *path)
{
    (void)fd; (void)path;
    *(int *)id3 = metadata_count;
    return metadata_ok;
}
#define metadata_read_chapters_buf test_metadata_read_chapters_buf
static int metadata_read_chapters_buf(int fd, void *id3,
                                      struct audiobook_chapters *out,
                                      void *scratch, size_t n)
{
    (void)fd; (void)id3; (void)scratch; (void)n;
    out->chapter_count = chapter_read_result > 0 ? chapter_read_result : 0;
    return chapter_read_result;
}
#include "books_metadata.inc"

static void test_failure_status(struct bwork *w)
{
    w->rd.fd = 1;
    w->rd.hdr.count = 1;
    table_result = -1; w->failed = false;
    assert(run_chapters(w) == EV_RETRY && w->failed);
    w->failed = false;
    assert(run_books(w) == EV_RETRY && w->failed);
    /* one book's chapter art failing: the pass is incomplete (the others
     * go on, no EV_RETRY of its own) for CHAP_TRIES passes, then left */
    table_result = 0; chapter_result = -1; w->failed = false;
    w->n_chskip = 0;
    assert(run_chapters(w) == 0 && w->failed);
    w->failed = false;
    assert(run_chapters(w) == 0 && w->failed);
    w->failed = false;
    assert(run_chapters(w) == 0 && !w->failed && w->chskip[0].tries == 3);
    assert(run_chapters(w) == 0 && !w->failed && w->chskip[0].tries == 3);
    w->n_chskip = 0;
    chapter_result = 0; lookup_error = true; w->failed = false;
    assert(run_chapters(w) == EV_RETRY && w->failed);
    /* a full index is a design limit: the rest keep the cover */
    lookup_error = false; cap_fits = false; w->failed = false;
    assert(run_chapters(w) == 0 && !w->failed);
    cap_fits = true; book_result = -1; w->failed = false;
    assert(run_books(w) == EV_RETRY && w->failed);
    /* an unbuildable book has no art to build: done, not pending, both
     * when found and when remembered (owner session 2026-10-19) */
    book_result = BK_UNBUILDABLE; w->failed = false;
    assert(run_books(w) == 0 && !w->failed);
    known_skip = true; book_result = -1; w->failed = false;
    assert(run_books(w) == 0 && !w->failed);
    known_skip = false;
    book_result = 1; w->failed = false;
    assert(run_books(w) == EV_RETRY && w->failed);
    fail_flush = false;
    snprintf(w->path, sizeof(w->path), "%s", F_BPAK);
    metadata_ok = false;
    assert(!test_metadata_gate(w));
    metadata_ok = true; metadata_count = 3; chapter_read_result = -1;
    assert(!test_metadata_gate(w));
    chapter_read_result = 0;
    assert(!test_metadata_gate(w));
    chapter_read_result = 3;
    assert(test_metadata_gate(w));
    metadata_count = 0;
    assert(test_metadata_gate(w)); /* verified no chapters permits fallback */
    fail_open = true;
    assert(!test_metadata_gate(w));
    fail_open = false;
}

static void prefix_unchanged(void)
{
    unsigned char b[256];
    int fd = open(F_BPAK, O_RDONLY), i;
    assert(fd >= 0 && read(fd, b, sizeof(b)) == sizeof(b));
    close(fd);
    for (i = 0; i < 256; i++) assert(b[i] == 0x77);
}
static void fixture(struct bwork *w)
{
    unsigned char b[256];
    struct ab_idx_rec rec = { .book_key = 41, .file_count = 1, .size = 32 };
    struct ab_idx_hdr h = { .count = 1, .pak_size = sizeof(b), .generation = 1 };
    uint32_t wi = 8;
    int fd;
    memset(b, 0x77, sizeof(b));
    fd = open(F_BPAK, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    assert(fd >= 0 && write(fd, b, sizeof(b)) == sizeof(b)); close(fd);
    bk_hdr_seal(&h, bk_sum_add(0, &rec, sizeof(rec), &wi));
    fd = open(F_BIDX, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    assert(fd >= 0 && write(fd, &h, sizeof(h)) == sizeof(h));
    assert(write(fd, &rec, sizeof(rec)) == sizeof(rec)); close(fd);
    books_load(w);
    assert(w->bhdr_ok && !w->books_defer);
}
int main(int argc, char **argv)
{
    struct bwork w;
    struct ab_idx_rec r;
    struct ab_idx_hdr checked;
    uint32_t gen;
    bool io_err;
    int fd;
    assert(argc == 2);
    snprintf(F_BIDX, sizeof(F_BIDX), "%s/books.idx", argv[1]);
    snprintf(F_BIDX_TMP, sizeof(F_BIDX_TMP), "%s/books.idx.tmp", argv[1]);
    snprintf(F_BIDX_PREV, sizeof(F_BIDX_PREV), "%s/books.idx.prev", argv[1]);
    snprintf(F_BPAK, sizeof(F_BPAK), "%s/books.pak", argv[1]);
    fixture(&w);
    test_failure_status(&w);
    fixture(&w);
    assert(test_append(&w, 42) == 0 && w.bpend[0].offset == 256);
    assert(books_flush(&w)); prefix_unchanged();
    fd = open(F_BIDX, O_RDONLY);
    assert(books_lookup(&w, fd, 41, &r, &io_err) && !io_err && r.offset == 0);
    fail_read = true;
    assert(!books_lookup(&w, fd, 41, &r, &io_err) && io_err);
    fail_read = false;
    close(fd);
    assert(books_check(&w, F_BIDX_PREV, &checked) == ARTIDX_OPENED);
    assert(checked.generation == 1);
    assert(test_append(&w, 43) == 0);
    artfmt_test_fail(ARTFMT_TEST_RENAME, 6, EIO); /* promote + rollback fail */
    assert(!books_flush(&w)); artfmt_test_reset(); prefix_unchanged();
    assert(w.failed);
    assert(w.bhdr_ok && !strcmp(w.bpath, F_BIDX_PREV));
    assert(ab_pak_lookup(42, &r, &gen) && idx_gen_is(gen));
    fd = open(F_BIDX, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    assert(fd >= 0 && write(fd, "bad", 3) == 3); close(fd);
    books_load(&w);
    assert(w.bhdr_ok && !strcmp(w.bpath, F_BIDX_PREV));
    assert(ab_pak_lookup(42, &r, &gen));
    assert(test_append(&w, 44) == 0 && w.bpend[0].offset == 320);
    assert(books_flush(&w)); prefix_unchanged();
    assert(books_check(&w, F_BIDX_PREV, &checked) == ARTIDX_OPENED);
    fail_read = true; books_load(&w); fail_read = false;
    assert(w.books_defer); prefix_unchanged();
    fail_open = true; books_load(&w); fail_open = false;
    assert(w.books_defer); prefix_unchanged();
    books_load(&w);
    assert(test_append(&w, 45) == 0);
    fail_flush = true; assert(!books_flush(&w)); fail_flush = false;
    prefix_unchanged(); books_load(&w);
    assert(test_append(&w, 46) == 0);
    fail_write = true; assert(!books_flush(&w)); fail_write = false;
    prefix_unchanged(); books_load(&w);
    assert(remove(F_BIDX) == 0 && remove(F_BIDX_PREV) == 0);
    books_load(&w); assert(!w.bhdr_ok && !w.books_defer);
    assert(test_append(&w, 47) == 0 && w.bpend[0].offset >= 352);
    assert(books_flush(&w)); prefix_unchanged();
    books_compact(&w); prefix_unchanged();
    fd = open(F_BPAK, O_RDONLY);
    {
        off_t end = bk_size(fd);
        close(fd);
        fail_flush = true;
        assert(test_append(&w, 48) == -1); /* orphaned append, no index delta */
        fail_flush = false;
        assert(!w.n_bpend);
        prefix_unchanged();
        assert(test_append(&w, 49) == 0);
        assert(w.bpend[0].offset == (uint32_t)end + 32);
        assert(books_flush(&w));
        prefix_unchanged();
    }
    puts("ok   actual-C books fallback, publication faults, append preservation");
    return 0;
}
