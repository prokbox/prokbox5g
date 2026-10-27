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

/* Host test of the album art cache formats (apps/ipodui/artfmt.c, compiled
 * with -DARTFMT_HOST over real files): CRC convention, headers, the index
 * writer/reader (merge, delete, GC, multi-page lookups), torn and corrupt
 * files, the album table (fixture from tests/artcache/keys.txt-style tags),
 * slot geometry and bundle names. Usage: test_artfmt <tmpdir> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "artfmt.h"

static int fails;
static char dir[512];
static uint32_t page[ARTIDX_PAGE_BYTES / 4];

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", \
    __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char *p(const char *name)
{
    static char buf[4][600];
    static int i;

    i = (i + 1) & 3;
    snprintf(buf[i], sizeof(buf[i]), "%s/%s", dir, name);
    return buf[i];
}

static uint64_t rnd_state = 0x9e3779b97f4a7c15ULL;
static uint64_t rnd(void)
{
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 7;
    rnd_state ^= rnd_state << 17;
    return rnd_state;
}

static int cmp_key(const void *a, const void *b)
{
    uint64_t x = ((const struct artcache_rec *)a)->key;
    uint64_t y = ((const struct artcache_rec *)b)->key;
    return x < y ? -1 : x > y;
}

static struct artcache_rec mkrec(uint64_t key, uint32_t slot)
{
    struct artcache_rec r;

    memset(&r, 0, sizeof(r));
    r.key = key;
    r.slot = slot;
    r.state = ARTREC_OK;
    r.mtime = (uint32_t)key;
    return r;
}

static void test_crc(void)
{
    /* the zlib/PNG CRC-32 check value */
    uint32_t c = artfmt_crc_end(artfmt_crc(ARTFMT_CRC_INIT, "123456789", 9));
    CHECK(c == 0xCBF43926u, "crc32 check value %08x", c);
    /* running == one shot */
    c = artfmt_crc(ARTFMT_CRC_INIT, "1234", 4);
    c = artfmt_crc_end(artfmt_crc(c, "56789", 5));
    CHECK(c == 0xCBF43926u, "running crc %08x", c);
}

static void test_headers(void)
{
    struct artidx_hdr h;
    struct artpak_hdr ph;
    char name[17];

    CHECK(sizeof(struct artcache_rec) == 32, "rec size %zu", sizeof(struct artcache_rec));
    CHECK(sizeof(struct artidx_hdr) == 32, "idx hdr size");
    CHECK(sizeof(struct artpak_hdr) == 20, "pak hdr size %zu", sizeof(struct artpak_hdr));
    CHECK(sizeof(struct artpak_tag) == 16, "pak tag size");
    CHECK(sizeof(struct libfile_hdr) == 544, "lib hdr size");
    CHECK(sizeof(struct libfile_album) == 24, "lib album size");
    CHECK(sizeof(struct artalb_rec) == 24, "alb rec size");
    CHECK(sizeof(struct artbundle_img) == 12, "bundle img size");
    CHECK(sizeof(struct artbundle_hdr) <= 512, "bundle hdr size");

    memset(&h, 0, sizeof(h));
    h.generation = 7;
    h.count = 3;
    artidx_hdr_seal(&h);
    CHECK(artidx_hdr_ok(&h), "sealed idx hdr");
    h.count = 4;
    CHECK(!artidx_hdr_ok(&h), "idx hdr crc catches a change");

    artpak_hdr_init(&ph, 40, 40);
    CHECK(artpak_hdr_ok(&ph, 40, 40), "pak hdr");
    CHECK(!artpak_hdr_ok(&ph, 48, 48), "pak hdr other size");
    CHECK(ph.slot_bytes == 3584, "40x40 slot %u", ph.slot_bytes);
    CHECK(artpak_slot_bytes(48, 48) == 5120, "48x48 slot");
    CHECK(artpak_slot_offset(3584, 2) == 512 + 7168, "slot offset");
    CHECK(artpak_slot_offset(3584, 5) % 512 == 0, "slot aligned");

    artbundle_name(0x1a2b3c4d5e6f7788ULL, name);
    CHECK(!strcmp(name, "1A/1A2B3C4D.ART"), "bundle name %s", name);
    CHECK(artcache_sizes[0].w == 55 && artcache_sizes[0].h == 55, "thumb 55x55");
    CHECK(artpak_slot_bytes(55, 55) == 6144, "55x55 slot");
    CHECK(artcache_sizes[ARTCACHE_PANEL].w == 300, "panel (H1: 300x300)");
}

static bool drop_odd(void *ctx, const struct artcache_rec *r, uint32_t i)
{
    (void)i;
    (*(int *)ctx)++;
    return !(r->slot & 1);
}

#define N 1000      /* 8 pages of 128 records */
static struct artcache_rec recs[N];

static void test_index(void)
{
    struct artidx_reader rd, rd2;
    struct artidx_hdr t;
    struct artcache_rec r, add[4];
    bool io;
    int i, n, visited = 0;

    for (i = 0; i < N; i++)
        recs[i] = mkrec(rnd() | 1, i);
    qsort(recs, N, sizeof(recs[0]), cmp_key);

    memset(&t, 0, sizeof(t));
    t.generation = 1;
    t.n_slots = N;
    artidx_init(&rd);
    /* empty old index + N adds */
    n = artidx_write_merged(p("index.tmp"), NULL, recs, N, NULL, NULL, &t, page);
    CHECK(n == N, "write %d", n);
    CHECK(rename(p("index.tmp"), p("index.dat")) == 0, "rename");
    CHECK(artidx_open(&rd, p("index.dat")) == ARTIDX_OPENED, "open");
    CHECK(rd.hdr.count == N && rd.n_pages == 8 && rd.hdr.n_slots == N,
          "hdr count %u pages %d", rd.hdr.count, rd.n_pages);
    for (i = 0; i < N; i++)
    {
        CHECK(artidx_lookup(&rd, recs[i].key, &r, &io) && !io
              && r.slot == (uint32_t)recs[i].slot, "lookup %d", i);
    }
    CHECK(!artidx_lookup(&rd, recs[0].key - 1, &r, &io) && !io, "absent low");
    CHECK(!artidx_lookup(&rd, recs[N - 1].key + 1, &r, &io), "absent high");
    CHECK(!artidx_lookup(&rd, recs[500].key + 1, &r, &io), "absent mid");
    CHECK(artidx_get(&rd, 999, &r) && r.key == recs[999].key, "get last");

    /* merge: replace one, delete one, add one new, GC odd slots */
    add[0] = mkrec(recs[10].key, 5000);                 /* replace (even) */
    add[1] = mkrec(recs[20].key, 0); add[1].state = 0;  /* delete */
    add[2] = mkrec(recs[30].key + 1, 6001);             /* new, odd slot */
    qsort(add, 3, sizeof(add[0]), cmp_key);
    t.generation = 2;
    artidx_init(&rd2);
    n = artidx_write_merged(p("index.tmp"), &rd, add, 3, drop_odd, &visited,
                            &t, page);
    {
        int want = 2;               /* the replacement and the new record */
        for (i = 0; i < N; i++)
            if (i != 10 && i != 20 && !(recs[i].slot & 1))
                want++;
        CHECK(n == want, "merged count %d, want %d", n, want);
    }
    CHECK(visited == N - 2, "keep() calls %d", visited);
    CHECK(rename(p("index.tmp"), p("index.dat")) == 0, "rename 2");
    /* the old reader still reads the old file (POSIX keeps it) */
    CHECK(artidx_lookup(&rd, recs[21].key, &r, NULL), "old reader");
    artidx_close(&rd);
    CHECK(artidx_open(&rd2, p("index.dat")) == ARTIDX_OPENED, "reopen");
    CHECK(rd2.hdr.generation == 2, "generation");
    CHECK(artidx_lookup(&rd2, recs[10].key, &r, NULL) && r.slot == 5000, "replaced");
    CHECK(!artidx_lookup(&rd2, recs[20].key, &r, NULL), "deleted");
    CHECK(artidx_lookup(&rd2, recs[30].key + 1, &r, NULL) && r.slot == 6001,
          "added survives GC");
    for (i = 0; i < N; i++)
        if (i != 10 && i != 20)
            CHECK(artidx_lookup(&rd2, recs[i].key, &r, NULL) == !(recs[i].slot & 1),
                  "gc %d", i);
    artidx_close(&rd2);
}

static void put_file(const char *path, const void *b, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && write(fd, b, n) == (ssize_t)n, "write %s", path);
    close(fd);
}

static size_t get_file(const char *path, void *b, size_t cap)
{
    int fd = open(path, O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, b, cap) : -1;
    close(fd);
    return n < 0 ? 0 : (size_t)n;
}

static void test_torn(void)
{
    static uint8_t img[512 + N * 32];
    struct artidx_reader rd;
    size_t n = get_file(p("index.dat"), img, sizeof(img)), cut;
    struct artidx_hdr t;
    int fd;

    artidx_init(&rd);
    CHECK(n > 512, "index size %zu", n);
    CHECK(artidx_open(&rd, p("missing.dat")) == ARTIDX_MISSING, "missing");
    /* cut anywhere: never accepted */
    for (cut = 0; cut < n; cut += (cut < 600 ? 7 : 997))
    {
        put_file(p("torn.dat"), img, cut);
        CHECK(artidx_open(&rd, p("torn.dat")) == ARTIDX_CORRUPT, "cut at %zu", cut);
    }
    /* one flipped bit in a record, in the header */
    img[512 + 77] ^= 4;
    put_file(p("torn.dat"), img, n);
    CHECK(artidx_open(&rd, p("torn.dat")) == ARTIDX_CORRUPT, "record bit");
    img[512 + 77] ^= 4;
    img[9] ^= 1;
    put_file(p("torn.dat"), img, n);
    CHECK(artidx_open(&rd, p("torn.dat")) == ARTIDX_CORRUPT, "header bit");
    img[9] ^= 1;
    put_file(p("torn.dat"), img, n);
    CHECK(artidx_open(&rd, p("torn.dat")) == ARTIDX_OPENED, "restored");
    artidx_close(&rd);
    /* the writer's placeholder header (power cut before the final header
     * write): records complete, header CRC all ones */
    memcpy(&t, img, sizeof(t));
    memset(&t.hdr_crc, 0xff, 4);
    memcpy(img, &t, sizeof(t));
    put_file(p("torn.dat"), img, n);
    CHECK(artidx_open(&rd, p("torn.dat")) == ARTIDX_CORRUPT, "placeholder hdr");
    /* records out of order with a matching CRC (a bad writer) */
    {
        struct artcache_rec a[2];
        struct artidx_hdr h;

        a[0] = mkrec(9, 0);
        a[1] = mkrec(5, 1);
        memset(&h, 0, sizeof(h));
        h.count = 2;
        h.rec_crc = artfmt_crc_end(artfmt_crc(ARTFMT_CRC_INIT, a, sizeof(a)));
        artidx_hdr_seal(&h);
        memset(img, 0, 512);
        memcpy(img, &h, sizeof(h));
        memcpy(img + 512, a, sizeof(a));
        put_file(p("torn.dat"), img, 512 + sizeof(a));
        CHECK(artidx_open(&rd, p("torn.dat")) == ARTIDX_CORRUPT, "order");
    }
    /* write into a directory that does not exist: error, no crash */
    memset(&t, 0, sizeof(t));
    CHECK(artidx_write_merged(p("nodir/index.tmp"), NULL, NULL, 0, NULL, NULL,
                              &t, page) < 0, "write error");
    /* an empty index is valid */
    CHECK(artidx_write_merged(p("empty.dat"), NULL, NULL, 0, NULL, NULL,
                              &t, page) == 0, "empty write");
    CHECK(artidx_open(&rd, p("empty.dat")) == ARTIDX_OPENED && rd.hdr.count == 0,
          "empty open");
    {
        struct artcache_rec r;
        CHECK(!artidx_lookup(&rd, 1, &r, NULL), "empty lookup");
    }
    artidx_close(&rd);
    fd = open(p("torn.dat"), O_RDONLY);
    close(fd);
}

/* A precompute file as library.c writes it (libfile.h): the header, one
 * filler section, then the album section (folder order; keys may repeat
 * or be 0). */
#define T_ENTRIES 1234
static const struct artalb_stamp stamp = { 42, T_ENTRIES, 3 };

static void write_table(const char *path, const struct libfile_album *r,
                        int n, int32_t commit, bool bad_sum)
{
    static uint8_t buf[LIBFILE_HDR_SIZE + 64 + 300 * 24];
    struct libfile_hdr *h = (struct libfile_hdr *)buf;
    struct libfile_sum_state st;
    uint32_t off = LIBFILE_HDR_SIZE;

    memset(buf, 0, sizeof(buf));
    h->magic = LIBFILE_MAGIC;
    h->version = LIBFILE_VERSION;
    h->sortkey_version = 3;
    h->commitid = commit;
    h->entries = T_ENTRIES;
    h->nsec = 2;
    /* section 0: 16 int32 rows of another category */
    h->sec[0] = (struct libfile_sec){ 1, LIBFILE_ROWS, 4, 16, off, 0 };
    memset(buf + off, 0x5a, 64);
    libfile_sum_init(&st);
    libfile_sum_add(&st, buf + off, 64);
    h->sec[0].sum = libfile_sum_end(&st);
    off += 64;
    h->sec[1] = (struct libfile_sec){ LIBFILE_CAT_ALBUMTAB, 0,
                                      sizeof(*r), n, off, 0 };
    memcpy(buf + off, r, n * sizeof(*r));
    libfile_sum_init(&st);
    libfile_sum_add(&st, buf + off, n * sizeof(*r));
    h->sec[1].sum = libfile_sum_end(&st) ^ (bad_sum ? 1 : 0);
    off += n * sizeof(*r);
    libfile_sum_init(&st);
    libfile_sum_add(&st, h, LIBFILE_HDR_SIZE);
    h->sum = libfile_sum_end(&st);
    put_file(path, buf, off);
}

static void test_table(void)
{
    static const char *const tags[][3] = {
        { "Signalfire", NULL, "OK Compass" }, { NULL, "Harbour Lights", "Mannequin" },
        { "Various Artists", "x", "Now 100" }, { NULL, NULL, "Mystery" },
        { "Björvik", NULL, "Homestead" },
    };
    static struct libfile_album r[300];
    struct artalb_rec out[64];
    struct artalb_hdr h, h2;
    struct artalb_stamp other = stamp;
    int i, n = 5;

    memset(r, 0, sizeof(r));
    for (i = 0; i < n; i++)
    {
        uint64_t k = artcache_key(tags[i][0], tags[i][1], tags[i][2], NULL);
        r[i].key_lo = (uint32_t)k;
        r[i].key_hi = (uint32_t)(k >> 32);
        r[i].rep = i * 10;
        r[i].count = 3;
        r[i].mtime = 1000 + i;
    }
    write_table(p("lib.dat"), r, n, 42, false);
    CHECK(artalb_check(p("lib.dat"), &stamp, &h, page) == ARTIDX_OPENED,
          "table ok");
    CHECK(h.n_albums == 5 && h.lib.commitid == 42
          && h.offset == LIBFILE_HDR_SIZE + 64, "table hdr");
    CHECK(artalb_read(p("lib.dat"), &h, 1, 64, out) == 4
          && out[0].key == artcache_key(NULL, "Harbour Lights", "Mannequin", NULL)
          && out[0].rep_idx == 10 && out[0].max_mtime == 1001
          && out[3].rep_idx == r[4].rep, "table read");
    CHECK(artalb_read(p("lib.dat"), &h, 5, 64, out) == 0, "table end");
    /* another database's file: not current */
    other.commitid = 41;
    CHECK(artalb_check(p("lib.dat"), &other, &h2, page) == ARTIDX_MISSING,
          "stamp commit");
    other = stamp;
    other.entries++;
    CHECK(artalb_check(p("lib.dat"), &other, &h2, page) == ARTIDX_MISSING,
          "stamp entries");
    other = stamp;
    other.sortkey_version++;
    CHECK(artalb_check(p("lib.dat"), &other, &h2, page) == ARTIDX_MISSING,
          "stamp sortkey");
    /* replaced while the builder walks it: detected */
    r[0].mtime++;
    write_table(p("lib.dat"), r, n, 42, false);
    CHECK(artalb_read(p("lib.dat"), &h, 0, 64, out) < 0, "table replaced");
    CHECK(artalb_check(p("lib.dat"), &stamp, &h2, page) == ARTIDX_OPENED,
          "table 2");
    /* bad section sum, bad header sum, rep out of range, cut, missing */
    write_table(p("lib.dat"), r, n, 42, true);
    CHECK(artalb_check(p("lib.dat"), &stamp, &h, page) == ARTIDX_CORRUPT,
          "section sum");
    write_table(p("lib.dat"), r, n, 42, false);
    {
        static uint8_t img[LIBFILE_HDR_SIZE + 64 + 300 * 24];
        size_t sz = get_file(p("lib.dat"), img, sizeof(img));

        img[offsetof(struct libfile_hdr, sec) + 3] ^= 1;
        put_file(p("lib.dat"), img, sz);
        CHECK(artalb_check(p("lib.dat"), &stamp, &h, page) == ARTIDX_CORRUPT,
              "header sum");
    }
    r[2].rep = T_ENTRIES;
    write_table(p("lib.dat"), r, n, 42, false);
    CHECK(artalb_check(p("lib.dat"), &stamp, &h, page) == ARTIDX_CORRUPT,
          "rep range");
    r[2].rep = 20;
    /* key 0 and repeated keys are the library's to make: accepted */
    r[1].key_lo = r[1].key_hi = 0;
    r[3] = r[2];
    write_table(p("lib.dat"), r, n, 42, false);
    CHECK(artalb_check(p("lib.dat"), &stamp, &h, page) == ARTIDX_OPENED,
          "zero and repeated keys");
    CHECK(artalb_check(p("nope.dat"), &stamp, &h, page) == ARTIDX_MISSING,
          "missing");
    /* > one 4 KB page of records, then cut */
    for (i = 0; i < 300; i++)
    {
        r[i].key_lo = 1000 + 3 * i;
        r[i].key_hi = i;
        r[i].rep = i;
        r[i].mtime = i;
    }
    write_table(p("lib.dat"), r, 300, 42, false);
    CHECK(artalb_check(p("lib.dat"), &stamp, &h, page) == ARTIDX_OPENED,
          "300");
    CHECK(artalb_read(p("lib.dat"), &h, 250, 64, out) == 50
          && out[49].key == ((uint64_t)299 << 32 | (1000 + 3 * 299))
          && out[49].rep_idx == 299, "300 read");
    {
        static uint8_t img[LIBFILE_HDR_SIZE + 64 + 300 * 24];
        size_t sz = get_file(p("lib.dat"), img, sizeof(img));
        put_file(p("lib.dat"), img, sz - 5);
        CHECK(artalb_check(p("lib.dat"), &stamp, &h, page) == ARTIDX_CORRUPT,
              "cut");
    }
}

/* GC at 50,000 albums (review LOW 3: the GC used to hold every table key
 * in the decode buffer, ~8,270 albums on the target, and skipped GC above
 * that): an index of 50,000 records plus 2,000 albums since removed, a
 * folder-order table of the 50,000 (with repeats and keyless rows),
 * marked through a 3,000-key buffer (17 chunks) and with one chunk. */
#define G_ALBUMS  50000
#define G_GONE    2000
#define G_ENTRIES 1000000

static uint64_t *gc_buf;
static int gc_calls;
static uint64_t *gc_keys(void *ctx)
{
    (void)ctx;
    gc_calls++;
    return gc_buf;
}

static bool keep_live(void *ctx, const struct artcache_rec *r, uint32_t i)
{
    const uint32_t *live = ctx;

    (void)r;
    return (live[i / 32] >> (i % 32)) & 1;
}

static void write_big_table(const char *path, const struct libfile_album *r,
                            int n)
{
    size_t size = LIBFILE_HDR_SIZE + (size_t)n * sizeof(*r);
    uint8_t *buf = calloc(1, size);
    struct libfile_hdr *h = (struct libfile_hdr *)buf;
    struct libfile_sum_state st;

    h->magic = LIBFILE_MAGIC;
    h->version = LIBFILE_VERSION;
    h->sortkey_version = 3;
    h->commitid = 77;
    h->entries = G_ENTRIES;
    h->nsec = 1;
    h->sec[0] = (struct libfile_sec){ LIBFILE_CAT_ALBUMTAB, 0, sizeof(*r), n,
                                      LIBFILE_HDR_SIZE, 0 };
    memcpy(buf + LIBFILE_HDR_SIZE, r, (size_t)n * sizeof(*r));
    libfile_sum_init(&st);
    libfile_sum_add(&st, buf + LIBFILE_HDR_SIZE, (size_t)n * sizeof(*r));
    h->sec[0].sum = libfile_sum_end(&st);
    libfile_sum_init(&st);
    libfile_sum_add(&st, h, LIBFILE_HDR_SIZE);
    h->sum = libfile_sum_end(&st);
    put_file(path, buf, size);
    free(buf);
}

static void test_gc_50k(void)
{
    static const struct artalb_stamp gstamp = { 77, G_ENTRIES, 3 };
    const int n_idx = G_ALBUMS + G_GONE, n_tab = G_ALBUMS + 500;
    struct artcache_rec *all = calloc(n_idx, sizeof(*all));
    struct libfile_album *tab = calloc(n_tab, sizeof(*tab));
    uint32_t *live = calloc(ARTIDX_MAX_RECS / 32, sizeof(uint32_t));
    struct artalb_rec tbuf[64];
    struct artidx_reader *rd = calloc(1, sizeof(*rd));
    struct artidx_hdr t;
    struct artalb_hdr th;
    struct artcache_rec r;
    unsigned caps[2] = { 3000, G_ALBUMS + 600 };
    int i, n, c, marked, bad;

    gc_buf = malloc(sizeof(uint64_t) * caps[1]);
    /* the index: G_ALBUMS + G_GONE distinct keys; the last G_GONE (in
     * generation order, so spread over the key range) are gone */
    for (i = 0; i < n_idx; i++)
    {
        all[i] = mkrec((rnd() | 1), i);
        all[i].mtime = i < G_ALBUMS;    /* 1: still in the table */
    }
    qsort(all, n_idx, sizeof(*all), cmp_key);
    for (i = 1; i < n_idx; i++)
        CHECK(all[i].key != all[i - 1].key, "distinct keys");
    memset(&t, 0, sizeof(t));
    t.n_slots = n_idx;
    artidx_init(rd);
    n = artidx_write_merged(p("gc.dat"), NULL, all, n_idx, NULL, NULL, &t,
                            page);
    CHECK(n == n_idx, "gc index %d", n);
    CHECK(artidx_open(rd, p("gc.dat")) == ARTIDX_OPENED, "gc open");
    /* the table: the kept albums in a shuffled ("folder") order, 400
     * repeats and 100 keyless rows */
    n = 0;
    for (i = 0; i < n_idx; i++)
        if (all[i].mtime)
        {
            uint64_t k = all[i].key;

            tab[n].key_lo = (uint32_t)k;
            tab[n].key_hi = (uint32_t)(k >> 32);
            tab[n].rep = n;
            n++;
        }
    CHECK(n == G_ALBUMS, "kept %d", n);
    for (; n < G_ALBUMS + 400; n++)
        tab[n] = tab[rnd() % G_ALBUMS];
    for (; n < n_tab; n++)
        memset(&tab[n], 0, sizeof(tab[n]));
    for (i = n_tab - 1; i > 0; i--)
    {
        int j = rnd() % (i + 1);
        struct libfile_album x = tab[i];

        tab[i] = tab[j];
        tab[j] = x;
    }
    for (i = 0; i < n_tab; i++)
        tab[i].rep = i;
    write_big_table(p("gclib.dat"), tab, n_tab);
    CHECK(artalb_check(p("gclib.dat"), &gstamp, &th, page) == ARTIDX_OPENED,
          "gc table");
    CHECK(th.n_albums == (uint32_t)n_tab, "gc table rows %u", th.n_albums);

    for (c = 0; c < 2; c++)
    {
        gc_calls = 0;
        CHECK(artidx_gc_mark(rd, p("gclib.dat"), &th, tbuf, 64, gc_keys,
                             NULL, caps[c], live), "gc mark cap %u", caps[c]);
        marked = bad = 0;
        for (i = 0; i < n_idx; i++)
        {
            int b = (live[i / 32] >> (i % 32)) & 1;

            marked += b;
            bad += b != (int)all[i].mtime;
        }
        CHECK(marked == G_ALBUMS && bad == 0,
              "cap %u: %d marked, %d wrong", caps[c], marked, bad);
        CHECK(c == 1 || gc_calls > 17, "re-fetched after reads (%d)",
              gc_calls);
    }
    /* the final commit's merge with it: only the removed albums go */
    t.generation = 2;
    n = artidx_write_merged(p("gc.tmp"), rd, NULL, 0, keep_live, live, &t,
                            page);
    CHECK(n == G_ALBUMS, "gc merged %d", n);
    artidx_close(rd);
    CHECK(artidx_open(rd, p("gc.tmp")) == ARTIDX_OPENED, "gc reopen");
    bad = 0;
    for (i = 0; i < n_idx; i += 7)
        bad += artidx_lookup(rd, all[i].key, &r, NULL) != (bool)all[i].mtime;
    CHECK(bad == 0, "gc lookups %d wrong", bad);
    artidx_close(rd);
    /* a table replaced meanwhile: false (keep everything) */
    tab[0].rep ^= 1;
    write_big_table(p("gclib.dat"), tab, n_tab);
    CHECK(artidx_open(rd, p("gc.dat")) == ARTIDX_OPENED, "gc open 2");
    CHECK(!artidx_gc_mark(rd, p("gclib.dat"), &th, tbuf, 64, gc_keys, NULL,
                          caps[0], live), "gc replaced table");
    artidx_close(rd);
    free(gc_buf);
    free(rd);
    free(live);
    free(tab);
    free(all);
}

/* H1: panels.pak's header and the panel set section (LIBFILE_CAT_PANELSET)
 * of the precompute file */
static void write_panel_table(const char *path, const struct libfile_album *r,
                              int n, const struct libfile_album *ps, int np,
                              int32_t commit, bool bad_sum)
{
    static uint8_t buf[LIBFILE_HDR_SIZE + 64 * 24 + 40 * 24];
    struct libfile_hdr *h = (struct libfile_hdr *)buf;
    struct libfile_sum_state st;
    uint32_t off = LIBFILE_HDR_SIZE;

    memset(buf, 0, sizeof(buf));
    h->magic = LIBFILE_MAGIC;
    h->version = LIBFILE_VERSION;
    h->sortkey_version = 3;
    h->commitid = commit;
    h->entries = T_ENTRIES;
    h->nsec = 2;
    h->sec[0] = (struct libfile_sec){ LIBFILE_CAT_ALBUMTAB, 0,
                                      sizeof(*r), n, off, 0 };
    memcpy(buf + off, r, n * sizeof(*r));
    libfile_sum_init(&st);
    libfile_sum_add(&st, buf + off, n * sizeof(*r));
    h->sec[0].sum = libfile_sum_end(&st);
    off += n * sizeof(*r);
    h->sec[1] = (struct libfile_sec){ LIBFILE_CAT_PANELSET, 0,
                                      sizeof(*ps), np, off, 0 };
    memcpy(buf + off, ps, np * sizeof(*ps));
    libfile_sum_init(&st);
    libfile_sum_add(&st, buf + off, np * sizeof(*ps));
    h->sec[1].sum = libfile_sum_end(&st) ^ (bad_sum ? 1 : 0);
    off += np * sizeof(*ps);
    libfile_sum_init(&st);
    libfile_sum_add(&st, h, LIBFILE_HDR_SIZE);
    h->sum = libfile_sum_end(&st);
    put_file(path, buf, off);
}

static void test_panels(void)
{
    static struct libfile_album r[64], ps[40];
    struct artalb_rec out[LIBFILE_PANELS_MAX];
    struct artalb_hdr h;
    struct artpanel_hdr ph, bad;
    int i;

    /* header */
    CHECK(sizeof(struct artpanel_hdr) == 792
          && sizeof(struct artpanel_ent) == 24, "panel hdr size");
    CHECK(artcache_sizes[ARTCACHE_PANEL].w == 300
          && artcache_sizes[ARTCACHE_PANEL].h == 300, "panel size");
    CHECK(artpak_slot_bytes(300, 300) == 180224
          && artpanel_slot_offset(180224, 2) == 1024 + 2 * 180224,
          "panel slot geometry");
    artpanel_hdr_init(&ph, 300, 300);
    CHECK(artpanel_hdr_ok(&ph, 300, 300) && ph.n_slots == 0, "empty hdr");
    CHECK(!artpanel_hdr_ok(&ph, 240, 240), "other size accepted");
    ph.n_slots = 2;
    ph.ent[5] = (struct artpanel_ent){ 0x1234, 7, 8, 9, ARTREC_OK, 1, {0} };
    ph.ent[6] = (struct artpanel_ent){ 0x5678, 7, 0, 0, ARTREC_NOART, 0,
                                       {0} };
    artpanel_hdr_seal(&ph);
    CHECK(artpanel_hdr_ok(&ph, 300, 300), "sealed hdr");
    bad = ph;
    bad.ent[5].mtime ^= 1;
    CHECK(!artpanel_hdr_ok(&bad, 300, 300), "crc");
    bad = ph;
    bad.ent[5].slot = 2;            /* past n_slots */
    artpanel_hdr_seal(&bad);
    CHECK(!artpanel_hdr_ok(&bad, 300, 300), "slot past the file");
    bad = ph;
    bad.ent[6].slot = 9;            /* NOART: its slot is meaningless */
    artpanel_hdr_seal(&bad);
    CHECK(artpanel_hdr_ok(&bad, 300, 300), "NOART slot checked");
    bad = ph;
    bad.n_slots = ARTPANEL_SLOTS + 1;
    artpanel_hdr_seal(&bad);
    CHECK(!artpanel_hdr_ok(&bad, 300, 300), "too many slots");

    /* #92 (review M1): grouped is the slot order alone; the version
     * stays 1, the only one an 8b build reads */
    CHECK(ph.version == ARTPANEL_VERSION && ARTPANEL_VERSION == 1,
          "the header stays v1");
    bad = ph;
    bad.version = 2;                /* a #92 development file */
    artpanel_hdr_seal(&bad);
    CHECK(artpanel_hdr_ok(&bad, 300, 300), "v2 (dev builds) read as v1");
    bad.version = 3;
    artpanel_hdr_seal(&bad);
    CHECK(!artpanel_hdr_ok(&bad, 300, 300), "v3 refused");
    {
        struct artpanel_hdr g;
        uint8_t plan[ARTPANEL_SLOTS];
        int ent[3];

        artpanel_hdr_init(&g, 300, 300);
        /* interleaved: music in slots 0, 2, 4; books in 1, 3 */
        for (i = 0; i < 5; i++)
        {
            g.ent[i] = (struct artpanel_ent){ 0x100 + i, 1, 2, 3, ARTREC_OK,
                                              (uint8_t)i, {0} };
            ARTPANEL_ENT_SET(&g.ent[i]) = i & 1;
        }
        g.ent[5] = (struct artpanel_ent){ 0x200, 1, 0, 0, ARTREC_NOART, 0,
                                          {0} };
        g.n_slots = 5;
        CHECK(!artpanel_grouped(&g), "interleaved is not grouped");
        CHECK(artpanel_regroup_plan(&g, plan) == 5, "plan slots");
        CHECK(plan[0] == 0 && plan[2] == 1 && plan[4] == 2 && plan[1] == 3
              && plan[3] == 4 && plan[5] == 0xff, "plan order");
        ent[0] = 0; ent[1] = 2; ent[2] = 4;
        CHECK(!artpanel_run(&g, ent, 3), "interleaved run");
        for (i = 0; i < 5; i++)
            g.ent[i].slot = plan[i];
        CHECK(artpanel_grouped(&g) && artpanel_run(&g, ent, 3),
              "regrouped: grouped, one run");
        g.ent[2].slot = 7;
        CHECK(!artpanel_grouped(&g), "a gap in a set is not grouped");
    }

    /* the panel set */
    for (i = 0; i < 64; i++)
        r[i] = (struct libfile_album){ 100 + i, 0, i * 3, 2, 500 + i, 0 };
    for (i = 0; i < 40; i++)
        ps[i] = r[(i * 7) % 64];
    write_table(p("nopanel.dat"), r, 64, 42, false);
    CHECK(artalb_check(p("nopanel.dat"), &stamp, &h, page) == ARTIDX_OPENED
          && artalb_panels(p("nopanel.dat"), &h, out, LIBFILE_PANELS_MAX)
             == 0, "a file without a panel set");
    write_panel_table(p("panel.dat"), r, 64, ps, 32, 42, false);
    CHECK(artalb_check(p("panel.dat"), &stamp, &h, page) == ARTIDX_OPENED,
          "panel table check");
    CHECK(artalb_panels(p("panel.dat"), &h, out, LIBFILE_PANELS_MAX) == 32,
          "panel set read");
    for (i = 0; i < 32; i++)
        CHECK(out[i].key == ps[i].key_lo && out[i].rep_idx == ps[i].rep
              && out[i].max_mtime == ps[i].mtime, "panel %d", i);
    CHECK(artalb_panels(p("panel.dat"), &h, out, 8) < 0, "over max");
    write_panel_table(p("panel.dat"), r, 64, ps, 32, 42, true);
    CHECK(artalb_check(p("panel.dat"), &stamp, &h, page) == ARTIDX_OPENED
          && artalb_panels(p("panel.dat"), &h, out, LIBFILE_PANELS_MAX) < 0,
          "panel sum not checked");
    ps[3].rep = T_ENTRIES;          /* out of range */
    write_panel_table(p("panel.dat"), r, 64, ps, 32, 42, false);
    CHECK(artalb_check(p("panel.dat"), &stamp, &h, page) == ARTIDX_OPENED
          && artalb_panels(p("panel.dat"), &h, out, LIBFILE_PANELS_MAX) < 0,
          "panel rep not checked");
    ps[3].rep = 1;
    write_panel_table(p("panel.dat"), r, 64, ps, 32, 42, false);
    CHECK(artalb_check(p("panel.dat"), &stamp, &h, page) == ARTIDX_OPENED,
          "panel table check 2");
    write_panel_table(p("panel.dat"), r, 64, ps, 31, 42, false);
    CHECK(artalb_panels(p("panel.dat"), &h, out, LIBFILE_PANELS_MAX) < 0,
          "replaced file read");
}

/* H1 end review: artpanel_read() (artcache_panel_get()) on torn slots */
static void test_panel_read(void)
{
    enum { W = 300, H = 300, NPIX = W * H * 2 };
    static unsigned char pix[NPIX], got[NPIX];
    static unsigned char slot[180224];
    uint32_t sb = artpak_slot_bytes(W, H);
    struct artpanel_hdr ph;
    struct artpanel_ent a = { 0xA11CEULL, 1, 2, 3, ARTREC_OK, 0, {0} };
    struct artpanel_ent b = { 0xB0BULL, 1, 2, 3, ARTREC_OK, 1, {0} };
    struct artpak_tag tag;
    volatile int seq = 7;
    const char *f = p("panels.pak");
    int fd, i;

    CHECK(sb == sizeof(slot), "panel slot size");
    for (i = 0; i < NPIX; i++)
        pix[i] = (unsigned char)(i * 31 + 7);
    artpanel_hdr_init(&ph, W, H);
    ph.n_slots = 2;
    ph.ent[0] = a;
    ph.ent[1] = b;
    artpanel_hdr_seal(&ph);
    fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    memset(slot, 0, sizeof(slot));
    memcpy(slot, &ph, sizeof(ph));
    CHECK(write(fd, slot, ARTPANEL_HDR_BYTES) == ARTPANEL_HDR_BYTES, "hdr");
    /* slot 0: album a, complete */
    memset(slot, 0, sizeof(slot));
    memcpy(slot, pix, NPIX);
    tag.key = a.key;
    tag.pix_crc = artfmt_crc_end(artfmt_crc(ARTFMT_CRC_INIT, pix, NPIX));
    tag.magic = ARTPAK_TAG_MAGIC;
    memcpy(slot + NPIX, &tag, sizeof(tag));
    CHECK(write(fd, slot, sb) == (ssize_t)sb, "slot 0");
    /* slot 1: album b's entry, but an append cut half way (power loss) */
    CHECK(write(fd, slot, sb / 2) == (ssize_t)(sb / 2), "slot 1");
    close(fd);

    CHECK(artpanel_read(f, sb, &a, got, NPIX, &seq, 7)
          && !memcmp(got, pix, NPIX), "complete slot");
    CHECK(!artpanel_read(f, sb, &a, got, NPIX, &seq, 6),
          "a rewrite during the read (seq moved) accepted");
    CHECK(!artpanel_read(f, sb, &b, got, NPIX, &seq, 7),
          "cut append accepted");
    b.slot = 0;                     /* a's slot, b's entry: another album */
    CHECK(!artpanel_read(f, sb, &b, got, NPIX, &seq, 7),
          "another album's slot accepted");
    /* slot 0 torn by a rewrite for the same album (pixels half new, tag
     * old): only the sequence tells, as in the builder's protocol */
    fd = open(f, O_WRONLY);
    memset(slot, 0x55, NPIX / 2);
    CHECK(pwrite(fd, slot, NPIX / 2, ARTPANEL_HDR_BYTES) == NPIX / 2,
          "tear");
    close(fd);
    seq++;                          /* panel_save() before the slot write */
    CHECK(!artpanel_read(f, sb, &a, got, NPIX, &seq, 7),
          "same-album torn slot accepted");
    CHECK(!artpanel_read(p("nonexistent.pak"), sb, &a, got, NPIX, &seq, 8),
          "missing file");
}

/* np-sharp: npcover.pak's header (most recent first, two slots), its
 * corruption checks, and a slot rewritten for another album as the
 * builder does it (old tag cleared first): the reader's tag check */
static void test_npcover(void)
{
    enum { W = 300, H = 300, NPIX = W * H * 2 };
    static unsigned char pix[NPIX], got[NPIX];
    static unsigned char slot[180224];
    uint32_t sb = artpak_slot_bytes(W, H);
    struct artpanel_hdr h, bad;
    struct artpanel_ent e;
    struct artpak_tag tag;
    volatile int seq = 3;
    const char *f = p("npcover.pak");
    uint32_t s;
    int fd, i;

    artnpc_init(&h, W, H);
    CHECK(artnpc_ok(&h, W, H) && h.n_slots == 0, "npc empty");
    CHECK(!artpanel_hdr_ok(&h, W, H), "npc read as panels.pak");
    CHECK(!artnpc_ok(&h, 240, 240), "npc other size");
    CHECK(artnpc_find(&h, 0xA) < 0, "npc find in empty");
    /* A, B, C rendered in turn: C, B kept, A's slot reused by C */
    memset(&e, 0, sizeof(e));
    e.state = ARTREC_OK;
    e.key = 0xA;
    e.slot = (uint8_t)(s = artnpc_slot(&h));
    CHECK(s == 0, "npc first slot %u", (unsigned)s);
    artnpc_put(&h, &e);
    e.key = 0xB;
    e.slot = (uint8_t)(s = artnpc_slot(&h));
    CHECK(s == 1, "npc second slot %u", (unsigned)s);
    artnpc_put(&h, &e);
    CHECK(artnpc_ok(&h, W, H) && h.n_slots == 2, "npc two");
    CHECK(artnpc_find(&h, 0xB) == 0 && artnpc_find(&h, 0xA) == 1,
          "npc MRU order");
    e.key = 0xC;
    e.slot = (uint8_t)(s = artnpc_slot(&h));
    CHECK(s == 0, "npc evicts the oldest's slot %u", (unsigned)s);
    artnpc_put(&h, &e);
    CHECK(artnpc_ok(&h, W, H) && h.n_slots == 2
          && artnpc_find(&h, 0xC) == 0 && artnpc_find(&h, 0xB) == 1
          && artnpc_find(&h, 0xA) < 0, "npc MRU eviction");
    /* the same key again: one entry, at the front */
    e.key = 0xB;
    e.slot = (uint8_t)(s = artnpc_slot(&h));
    CHECK(s == 1, "npc oldest is B %u", (unsigned)s);
    artnpc_put(&h, &e);
    CHECK(artnpc_find(&h, 0xB) == 0 && artnpc_find(&h, 0xC) == 1
          && h.ent[2].state == 0, "npc same key once");
    /* corruption */
    bad = h;
    bad.ent[0].key ^= 1;
    CHECK(!artnpc_ok(&bad, W, H), "npc crc");
    bad = h;
    bad.n_slots = 3;
    artpanel_hdr_seal(&bad);
    CHECK(!artnpc_ok(&bad, W, H), "npc too many slots");
    bad = h;
    bad.ent[1].slot = bad.ent[0].slot;
    artpanel_hdr_seal(&bad);
    CHECK(!artnpc_ok(&bad, W, H), "npc two entries one slot");
    bad = h;
    bad.ent[2] = bad.ent[0];
    bad.ent[2].key = 0xD;
    artpanel_hdr_seal(&bad);
    CHECK(!artnpc_ok(&bad, W, H), "npc entry past the slots");
    bad = h;
    bad.ent[0].slot = 2;
    artpanel_hdr_seal(&bad);
    CHECK(!artnpc_ok(&bad, W, H), "npc slot past the file");
    bad = h;
    bad.magic = ARTPANEL_MAGIC;
    artpanel_hdr_seal(&bad);
    CHECK(!artnpc_ok(&bad, W, H), "panels.pak read as npc");

    /* on disk: slot 1 holds B; rewritten in place for D (its old tag
     * zeroed first, then half the pixels): B's entry and D's both refuse
     * the slot until D's tag lands */
    for (i = 0; i < NPIX; i++)
        pix[i] = (unsigned char)(i * 13 + 5);
    fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    memset(slot, 0, sizeof(slot));
    memcpy(slot, &h, sizeof(h));
    CHECK(write(fd, slot, ARTPANEL_HDR_BYTES) == ARTPANEL_HDR_BYTES,
          "npc hdr");
    for (i = 0; i < 2; i++)
    {
        memset(slot, 0, sizeof(slot));
        memcpy(slot, pix, NPIX);
        tag.key = h.ent[0].slot == i ? h.ent[0].key : h.ent[1].key;
        tag.pix_crc = 0;
        tag.magic = ARTPAK_TAG_MAGIC;
        memcpy(slot + NPIX, &tag, sizeof(tag));
        CHECK(write(fd, slot, sb) == (ssize_t)sb, "npc slot %d", i);
    }
    close(fd);
    CHECK(artpanel_read(f, sb, &h.ent[0], got, NPIX, &seq, 3)
          && !memcmp(got, pix, NPIX), "npc slot read");
    e = h.ent[0];
    e.key = 0xC;                    /* another album's entry, B's slot */
    CHECK(!artpanel_read(f, sb, &e, got, NPIX, &seq, 3),
          "npc tag mismatch accepted");
    fd = open(f, O_WRONLY);
    memset(&tag, 0, sizeof(tag));
    CHECK(pwrite(fd, &tag, sizeof(tag),
                 artpanel_slot_offset(sb, h.ent[0].slot) + NPIX)
          == (ssize_t)sizeof(tag), "npc untag");
    memset(slot, 0x55, NPIX / 2);
    CHECK(pwrite(fd, slot, NPIX / 2, artpanel_slot_offset(sb, h.ent[0].slot))
          == NPIX / 2, "npc half");
    close(fd);
    CHECK(!artpanel_read(f, sb, &h.ent[0], got, NPIX, &seq, 3),
          "npc old entry over a rewrite accepted");
    e = h.ent[0];
    e.key = 0xD;
    CHECK(!artpanel_read(f, sb, &e, got, NPIX, &seq, 3),
          "npc new entry before its tag accepted");
}

/* Phase 5 (D10): the Now Playing fit and bundle round trip, with the
 * fixture shapes 16:9, 2:3 and 4:1 (tests/fixtures/np/) */
static void test_bundle(void)
{
    static const struct { unsigned sw, sh, w, h, x, y; } fits[] = {
        { 1920, 1080, 142,  80,  0, 31 },   /* 16:9 */
        {  600,  900,  95, 142, 23,  0 },   /* 2:3: centred horizontally */
        { 1200,  300, 142,  36,  0, 53 },   /* 4:1 (35.5 rounds up) */
        {  500,  500, 142, 142,  0,  0 },
        {   40,   30, 142, 107,  0, 17 },   /* upscaled (106.5 up) */
        { 5000,    1, 142,   1,  0, 70 },   /* never 0 */
        {    0,    0, 142, 142,  0,  0 },   /* unknown: the box */
    };
    static uint16_t pix[142 * 142], back[142 * 142];
    struct artcache_dim box = artcache_sizes[ARTCACHE_NOWPLAYING];
    char hdr[ARTBUNDLE_HDR_BYTES];
    uint16_t w, h;
    unsigned i;
    int fd;

    CHECK(box.w == 142 && box.h == 142, "box %ux%u", box.w, box.h);
    CHECK(artcache_size_fits(ARTCACHE_NOWPLAYING)
          && !artcache_size_fits(ARTCACHE_THUMB)
          && !artcache_size_fits(ARTCACHE_PANEL), "fit sizes");
    for (i = 0; i < sizeof(fits) / sizeof(fits[0]); i++)
    {
        struct artcache_fit f;

        artcache_fit_rect(fits[i].sw, fits[i].sh, box.w, box.h, &f);
        CHECK(f.w == fits[i].w && f.h == fits[i].h && f.x == fits[i].x
              && f.y == fits[i].y, "fit %ux%u: %ux%u+%u+%u", fits[i].sw,
              fits[i].sh, f.w, f.h, f.x, f.y);
    }

    for (i = 0; i < 3; i++)
    {
        unsigned bw = fits[i].w, bh = fits[i].h, j;
        uint64_t key = 0x1234567800000000ull + i;

        for (j = 0; j < bw * bh; j++)
            pix[j] = (uint16_t)(j * 31 + i);
        CHECK(artbundle_write(p("b.ART"), key, ARTCACHE_NOWPLAYING, bw, bh,
                              pix, hdr), "write %u", i);
        memset(back, 0, sizeof(back));
        CHECK(artbundle_read(p("b.ART"), key, ARTCACHE_NOWPLAYING, box.w,
                             box.h, back, sizeof(back), &w, &h) == 1
              && w == bw && h == bh
              && !memcmp(back, pix, (size_t)bw * bh * 2),
              "round trip %u: %ux%u", i, w, h);
        /* another key (a name collision), another size: not there */
        CHECK(artbundle_read(p("b.ART"), key + 1, ARTCACHE_NOWPLAYING,
                             box.w, box.h, back, sizeof(back), &w, &h) == 0,
              "other key");
        CHECK(artbundle_read(p("b.ART"), key, ARTCACHE_COVERFLOW, box.w,
                             box.h, back, sizeof(back), &w, &h) == 0,
              "other size");
        /* too big for the reader's box or buffer */
        CHECK(artbundle_read(p("b.ART"), key, ARTCACHE_NOWPLAYING, 55, 55,
                             back, sizeof(back), &w, &h) == -1, "box");
        CHECK(artbundle_read(p("b.ART"), key, ARTCACHE_NOWPLAYING, box.w,
                             box.h, back, 100, &w, &h) == -1, "buffer");
    }
    /* a write cut after the pixels: the header is still zero */
    fd = open(p("b.ART"), O_WRONLY | O_TRUNC);
    memset(hdr, 0, sizeof(hdr));
    CHECK(fd >= 0 && write(fd, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr)
          && write(fd, pix, 1000) == 1000, "torn");
    close(fd);
    CHECK(artbundle_read(p("b.ART"), 0x1234567800000000ull,
                         ARTCACHE_NOWPLAYING, box.w, box.h, back,
                         sizeof(back), &w, &h) == 0, "torn read");
    CHECK(artbundle_read(p("none.ART"), 1, ARTCACHE_NOWPLAYING, box.w,
                         box.h, back, sizeof(back), &w, &h) == 0, "missing");

    /* QW2 (Phase 11 §3.9): a composed bundle (version 2, format 1, the
     * layout hash in 'reserved') and a fitted one both come back from
     * ONE read of header + pixels; the old reader skips the composed */
    {
        static uint32_t one[(512 + 138 * 171 * 2) / 4 + 1];
        static uint16_t comp[138 * 171];
        struct artbundle_img im, got;
        const uint16_t *px = (const uint16_t *)((char *)one + 512);
        uint64_t key = 0x2234567800000000ull;
        unsigned j;

        for (j = 0; j < 138 * 171; j++)
            comp[j] = (uint16_t)(j * 7 + 3);
        memset(&im, 0, sizeof(im));
        im.size_id = ARTCACHE_NOWPLAYING;
        im.format = ARTBUNDLE_FMT_NPCOMPOSED;
        im.w = 138;
        im.h = 171;
        im.reserved = 0xbeef;
        CHECK(artbundle_write_img(p("c.ART"), key, ARTBUNDLE_VERSION_COMPOSED,
                                  &im, comp, hdr), "write composed");
        CHECK(artbundle_read_one(p("c.ART"), key, ARTCACHE_NOWPLAYING, one,
                                 512 + sizeof(comp), &got) == 1
              && got.format == ARTBUNDLE_FMT_NPCOMPOSED && got.w == 138
              && got.h == 171 && got.reserved == 0xbeef
              && !memcmp(px, comp, sizeof(comp)), "read_one composed");
        CHECK(artbundle_read(p("c.ART"), key, ARTCACHE_NOWPLAYING, box.w,
                             box.h, back, sizeof(back), &w, &h) == 0,
              "old reader skips composed");
        CHECK(artbundle_read_one(p("c.ART"), key, ARTCACHE_NOWPLAYING, one,
                                 512 + sizeof(comp) - 2, &got) == -1,
              "read_one short buffer");
        CHECK(artbundle_read_one(p("c.ART"), key + 1, ARTCACHE_NOWPLAYING,
                                 one, sizeof(one), &got) == 0,
              "read_one other key");
        /* a fitted (version 1) bundle through the one-read path */
        CHECK(artbundle_write(p("f.ART"), key, ARTCACHE_NOWPLAYING, 142, 80,
                              pix, hdr), "write fitted");
        CHECK(artbundle_read_one(p("f.ART"), key, ARTCACHE_NOWPLAYING, one,
                                 sizeof(one), &got) == 1 && got.format == 0
              && got.w == 142 && got.h == 80
              && !memcmp(px, pix, 142 * 80 * 2), "read_one fitted");
        CHECK(artbundle_read_one(p("none.ART"), key, ARTCACHE_NOWPLAYING, one,
                                 sizeof(one), &got) == 0, "read_one missing");
    }
}

/* Phase 5, chapter art (docs/design/chapter-art.md): record packing, the
 * plan for shared images, the cap, a torn index and a torn bundle, and the
 * GC's second pass. */
struct chimg { uint32_t pos; int st; };

static int chimg_fn(void *ctx, int i, uint32_t *pos, uint8_t *type)
{
    const struct chimg *c = ctx;

    *pos = c[i].pos;
    *type = 1;                  /* AA_TYPE_JPG */
    return c[i].st;
}

static int find_rec(const struct artcache_rec *r, int n, uint64_t key)
{
    int i;

    for (i = 0; i < n; i++)
        if (r[i].key == key)
            return i;
    return -1;
}

static void test_chapters(void)
{
    const uint64_t book = 0xb00cb00c12345678ull, book2 = 0x2b002b0087654321ull;
    const uint64_t gone = 0x60e060e011112222ull;
    /* 0-2 share pos 100; 3 none (does not end the group); 4 shares too;
     * 5, 6 own images; 7-8 a PNG / failed group; 9 none */
    static const struct chimg img[10] = {
        { 100, 1 }, { 100, 1 }, { 100, 1 }, { 0, 0 }, { 100, 1 },
        { 500, 1 }, { 900, 1 }, { 1300, -1 }, { 1300, -1 }, { 0, 0 },
    };
    struct artcache_rec out[16], r, all[64];
    struct artidx_reader *rd = calloc(1, sizeof(*rd));
    struct artidx_hdr t;
    uint32_t live[64 / 32 + 1];
    uint64_t kbuf[16];
    int n, i, k, n_all = 0, n_chap = 0;
    const uint64_t k0 = artcache_key_chapter(book, 0);

    /* packing */
    artrec_chapter_init(&r, artcache_key_chapter(book, 3), k0, book, 77, 1);
    CHECK(artrec_is_chapter(&r) && r.state == ARTREC_OK
          && artrec_bundle_key(&r) == k0 && artrec_book_key(&r) == book
          && r.mtime == 77 && !artrec_owns_bundle(&r), "chapter packing");
    artrec_chapter_init(&r, k0, k0, book, 77, 1);
    CHECK(artrec_owns_bundle(&r), "owner");
    r = mkrec(book, 5);
    CHECK(!artrec_is_chapter(&r) && artrec_bundle_key(&r) == book
          && artrec_owns_bundle(&r), "album record owns its bundle");

    /* the plan: shared images point at one bundle */
    n = artchap_plan(book, 77, 10, chimg_fn, (void *)img, out, 16);
    CHECK(n == 6, "plan records %d", n);
    for (i = 0; i < n; i++)
        CHECK(artrec_book_key(&out[i]) == book && out[i].mtime == 77,
              "plan book %d", i);
    for (i = 0; i < 10; i++)
    {
        uint64_t ck = artcache_key_chapter(book, i);
        int j = find_rec(out, n, ck);
        uint64_t want = i <= 4 ? k0 : ck;

        if (img[i].st <= 0)
            CHECK(j < 0, "chapter %d: no record", i);
        else
            CHECK(j >= 0 && artrec_bundle_key(&out[j]) == want,
                  "chapter %d bundle", i);
    }
    k = 0;
    for (i = 0; i < n; i++)
        k += artrec_owns_bundle(&out[i]);
    CHECK(k == 3, "3 bundles decoded for 6 records (%d)", k);
    CHECK(artchap_plan(book, 77, 10, chimg_fn, (void *)img, out, 2) == 2,
          "room");

    /* non-adjacent copies at one position (the builder's size + CRC
     * dedupe of ID3 CHAP APIC copies): one bundle per position */
    {
        static const struct chimg img2[5] = {
            { 100, 1 }, { 500, 1 }, { 100, 1 }, { 0, 0 }, { 500, 1 },
        };
        static const int owner[5] = { 0, 1, 0, -1, 1 };

        n = artchap_plan(book, 77, 5, chimg_fn, (void *)img2, out, 16);
        CHECK(n == 4, "non-adjacent plan records %d", n);
        for (i = 0; i < 5; i++)
        {
            int j = find_rec(out, n, artcache_key_chapter(book, i));

            if (owner[i] < 0)
                CHECK(j < 0, "non-adjacent %d: no record", i);
            else
                CHECK(j >= 0 && artrec_bundle_key(&out[j])
                                == artcache_key_chapter(book, owner[i]),
                      "non-adjacent %d bundle", i);
        }
        CHECK(artchap_owner(chimg_fn, (void *)img2, 4, 500) == 1,
              "owner of 4");
    }

    /* sort + unique (a state 0 delete sorts after an add of its key) */
    n = artchap_plan(book, 77, 10, chimg_fn, (void *)img, out, 16);
    out[n] = out[0];
    out[n].state = 0;
    out[n + 1] = mkrec(book, 9);
    n = artrec_sort_unique(out, n + 2);
    CHECK(n == 7, "unique %d", n);
    for (i = 1; i < n; i++)
        CHECK(out[i - 1].key < out[i].key, "sorted %d", i);
    CHECK(out[find_rec(out, n, k0)].state == 0, "the delete wins");

    /* the cap */
    CHECK(artchap_fits(0, 0, ARTIDX_MAX_RECS), "empty cap");
    CHECK(!artchap_fits(0, 0, ARTIDX_MAX_RECS + 1), "over cap");
    CHECK(artchap_fits(60000, 5000, 536) && !artchap_fits(60000, 5000, 537),
          "cap minus albums");
    CHECK(!artchap_fits(65000, 1000, 0), "album records past the cap");
    CHECK(!artchap_fits(0, ARTIDX_MAX_RECS + 5, 1), "huge table");

    /* an index: book (chapters done, mtime 77) with its 6 chapter
     * records; book2 (done, mtime 9) with 2 current chapter records and a
     * stale one (mtime 8); a dropped book with one; 20 albums */
    n = artchap_plan(book, 77, 10, chimg_fn, (void *)img, out, 16);
    for (i = 0; i < n; i++)
        all[n_all++] = out[i];
    n_chap = n;
    r = mkrec(book, 1);
    r.mtime = 77;
    r.flags = ARTREC_F_CHAPTERS;
    all[n_all++] = r;
    r = mkrec(book2, 2);
    r.mtime = 9;
    r.flags = ARTREC_F_CHAPTERS;
    all[n_all++] = r;
    for (i = 0; i < 3; i++)
    {
        uint64_t ck = artcache_key_chapter(book2, i);

        artrec_chapter_init(&all[n_all++], ck, ck, book2, i < 2 ? 9 : 8, 1);
    }
    r = mkrec(gone, 3);
    r.flags = ARTREC_F_CHAPTERS;
    all[n_all++] = r;
    artrec_chapter_init(&all[n_all], artcache_key_chapter(gone, 0),
                        artcache_key_chapter(gone, 0), gone, r.mtime, 1);
    n_all++;
    for (i = 0; i < 20; i++)
        all[n_all++] = mkrec(rnd() | 1, 10 + i);
    qsort(all, n_all, sizeof(all[0]), cmp_key);
    memset(&t, 0, sizeof(t));
    t.n_slots = 40;
    artidx_init(rd);
    CHECK(artidx_write_merged(p("ch.dat"), NULL, all, n_all, NULL, NULL, &t,
                              page) == n_all, "chapter index");
    CHECK(artidx_open(rd, p("ch.dat")) == ARTIDX_OPENED, "chapter open");
    /* pass 1 (as artidx_gc_mark() leaves it): the albums and both books
     * live, 'gone' not, no chapter record */
    memset(live, 0, sizeof(live));
    for (i = 0; i < n_all; i++)
        if (!artrec_is_chapter(&all[i]) && all[i].key != gone)
            live[i / 32] |= 1u << (i % 32);
    for (k = 0; k < 2; k++)
    {
        uint32_t lv[sizeof(live) / 4];
        int bad = 0, kept = 0;

        memcpy(lv, live, sizeof(lv));
        gc_buf = kbuf;
        CHECK(artidx_gc_mark_chapters(rd, gc_keys, NULL, k ? 1 : 16, lv),
              "gc chapters %d", k);
        for (i = 0; i < n_all; i++)
        {
            int b = (lv[i / 32] >> (i % 32)) & 1;
            bool want;

            if (!artrec_is_chapter(&all[i]))
            {
                bad += b != (int)((live[i / 32] >> (i % 32)) & 1);
                continue;
            }
            kept += b;
            if (k)
                want = true;        /* cap 1 < 2 books: keep them all */
            else
                want = artrec_book_key(&all[i]) == book
                       || (artrec_book_key(&all[i]) == book2
                           && all[i].mtime == 9);
            bad += b != want;
        }
        CHECK(bad == 0, "gc chapters cap %d: %d wrong", k ? 1 : 16, bad);
        CHECK(kept == (k ? n_chap + 4 : n_chap + 2), "kept %d", kept);
        if (!k)
        {
            /* the merge with it: the stale one, the dropped book and its
             * chapter go */
            t.generation = 2;
            CHECK(artidx_write_merged(p("ch.tmp"), rd, NULL, 0, keep_live,
                                      lv, &t, page) == n_all - 3,
                  "gc merge");
        }
    }
    artidx_close(rd);
    CHECK(artidx_open(rd, p("ch.tmp")) == ARTIDX_OPENED, "merged open");
    CHECK(artidx_lookup(rd, artcache_key_chapter(book, 4), &r, NULL)
          && artrec_bundle_key(&r) == k0, "sharer survives");
    CHECK(!artidx_lookup(rd, artcache_key_chapter(book2, 2), &r, NULL),
          "stale dropped");
    CHECK(!artidx_lookup(rd, artcache_key_chapter(gone, 0), &r, NULL),
          "dropped book's chapter dropped");
    artidx_close(rd);

    /* torn: an index cut inside a chapter record is refused whole (the
     * builder rebuilds; the reader shows covers meanwhile) */
    {
        static uint8_t buf[ARTIDX_HDR_BYTES + 64 * 32];
        size_t len = get_file(p("ch.dat"), buf, sizeof(buf));

        CHECK(len == ARTIDX_HDR_BYTES + (size_t)n_all * 32, "len %zu", len);
        put_file(p("cht.dat"), buf, len - 20);
        CHECK(artidx_open(rd, p("cht.dat")) == ARTIDX_CORRUPT, "torn index");
        artidx_close(rd);
        /* one flipped bit of a chapter record's bundle key */
        buf[ARTIDX_HDR_BYTES + find_rec(all, n_all, k0) * 32 + 8] ^= 1;
        put_file(p("cht.dat"), buf, len);
        CHECK(artidx_open(rd, p("cht.dat")) == ARTIDX_CORRUPT, "bad crc");
        artidx_close(rd);
    }
    /* a torn bundle: the owner's file cut after its header was zeroed
     * reads as absent (PENDING: Now Playing shows the cover) */
    {
        static uint16_t pix[142 * 142];
        char hdr[ARTBUNDLE_HDR_BYTES];
        uint16_t w, h;
        int fd;

        CHECK(artbundle_write(p("ch.ART"), k0, ARTCACHE_NOWPLAYING, 142, 80,
                              pix, hdr), "chapter bundle");
        CHECK(artbundle_read(p("ch.ART"), k0, ARTCACHE_NOWPLAYING, 142, 142,
                             pix, sizeof(pix), &w, &h) == 1 && w == 142
              && h == 80, "chapter bundle read");
        CHECK(artbundle_read(p("ch.ART"), artcache_key_chapter(book, 1),
                             ARTCACHE_NOWPLAYING, 142, 142, pix, sizeof(pix),
                             &w, &h) == 0, "a sharer's own key is not it");
        fd = open(p("ch.ART"), O_WRONLY | O_TRUNC);
        memset(hdr, 0, sizeof(hdr));
        CHECK(fd >= 0 && write(fd, hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr),
              "cut");
        close(fd);
        CHECK(artbundle_read(p("ch.ART"), k0, ARTCACHE_NOWPLAYING, 142, 142,
                             pix, sizeof(pix), &w, &h) == 0, "torn bundle");
    }
    free(rd);
}

static void check_bundle_pixels(const char *path, uint16_t expected)
{
    uint16_t out[16], w = 0, h = 0;
    struct artbundle_img im;
    uint32_t one[(ARTBUNDLE_HDR_BYTES + sizeof(out)) / 4];
    int i;
    CHECK(artbundle_read(path, 42, ARTCACHE_NOWPLAYING, 4, 4,
                         out, sizeof(out), &w, &h) == 1 && w == 4 && h == 4,
          "recoverable bundle dimensions");
    for (i = 0; i < 16; i++)
        CHECK(out[i] == expected, "old pixels survive failure (%d)", i);
    CHECK(artbundle_read_one(path, 42, ARTCACHE_NOWPLAYING, one,
                             sizeof(one), &im) == 1 && im.w == 4 && im.h == 4,
          "single-read fallback dimensions");
    for (i = 0; i < 16; i++)
        CHECK(((uint16_t *)((char *)one + ARTBUNDLE_HDR_BYTES))[i] == expected,
              "single-read old pixels survive failure (%d)", i);
}

static void test_persistence(void)
{
    uint16_t pix[16];
    uint32_t hdr[ARTBUNDLE_HDR_BYTES / 4];
    struct artidx_reader rd;
    const enum artfmt_test_op ops[] = {
        ARTFMT_TEST_WRITE, ARTFMT_TEST_FLUSH, ARTFMT_TEST_READ,
        ARTFMT_TEST_CLOSE, ARTFMT_TEST_RENAME, ARTFMT_TEST_SHORT_WRITE
    };
    char path[600], tmp[610], prev[610];
    int i, fd;
    snprintf(path, sizeof(path), "%s/durable.ART", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(prev, sizeof(prev), "%s.prev", path);
    for (i = 0; i < 16; i++)
        pix[i] = 11;
    CHECK(artbundle_write(path, 42, ARTCACHE_NOWPLAYING, 4, 4, pix, hdr),
          "initial durable bundle");
    for (i = 0; i < 16; i++)
        pix[i] = 22;
    CHECK(artbundle_write(path, 42, ARTCACHE_NOWPLAYING, 4, 4, pix, hdr),
          "publish replacement bundle");
    check_bundle_pixels(path, 22);
    check_bundle_pixels(prev, 11);
    for (i = 0; i < (int)(sizeof(ops) / sizeof(ops[0])); i++)
    {
        artfmt_test_fail(ops[i], 1, EIO);
        CHECK(!artbundle_write(path, 42, ARTCACHE_NOWPLAYING, 4, 4, pix, hdr),
              "fault %d must not report success", ops[i]);
        artfmt_test_reset();
        check_bundle_pixels(path, 22);
    }
    /* Temp open failure, validation open failure, primary read failure. */
    for (i = 0; i < 3; i++)
    {
        artfmt_test_fail(ARTFMT_TEST_OPEN, 1ull << i, EACCES);
        CHECK(!artbundle_write(path, 42, ARTCACHE_NOWPLAYING, 4, 4, pix, hdr),
              "failed open %d defers publication", i);
        artfmt_test_reset();
        check_bundle_pixels(path, 22);
    }
    artfmt_test_fail(ARTFMT_TEST_READ, 4, EIO);
    CHECK(!artbundle_write(path, 42, ARTCACHE_NOWPLAYING, 4, 4, pix, hdr),
          "primary I/O failure must not rotate previous");
    artfmt_test_reset();
    /* the rename fault in the loop above dropped the old prev (removed
     * before the first rename since 2026-10-19); if one is there it must
     * still be the old pixels */
    if (access(prev, F_OK) == 0)
        check_bundle_pixels(prev, 11);

    /* Failed promotion restores the old primary. Failed rollback leaves a
     * recoverable .prev, which both public reader APIs must serve. */
    artfmt_test_fail(ARTFMT_TEST_RENAME, 2, EIO);
    CHECK(!artbundle_write(path, 42, ARTCACHE_NOWPLAYING, 4, 4, pix, hdr),
          "failed promotion is not success");
    artfmt_test_reset();
    check_bundle_pixels(path, 22);
    artfmt_test_fail(ARTFMT_TEST_RENAME, 6, EIO);
    CHECK(!artbundle_write(path, 42, ARTCACHE_NOWPLAYING, 4, 4, pix, hdr),
          "failed rollback is not success");
    artfmt_test_reset();
    CHECK(access(path, F_OK) != 0 && access(tmp, F_OK) == 0
          && access(prev, F_OK) == 0, "cut publication retains tmp and prev");
    check_bundle_pixels(path, 22);
    /* Simulate a torn primary after reboot; a new replacement must not
     * rotate this damaged file over the retained valid previous bundle. */
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    CHECK(fd >= 0 && write(fd, "cut", 3) == 3, "torn-primary fixture");
    close(fd);
    check_bundle_pixels(path, 22);
    CHECK(artbundle_write(path, 42, ARTCACHE_NOWPLAYING, 4, 4, pix, hdr),
          "repair torn primary without damaging previous");
    check_bundle_pixels(prev, 22);

    /* Missing primary must never rename an empty/missing file over prev. */
    CHECK(remove(path) == 0, "simulate missing primary");
    artfmt_test_fail(ARTFMT_TEST_RENAME, 2, EIO);
    CHECK(!artbundle_write(path, 42, ARTCACHE_NOWPLAYING, 4, 4, pix, hdr),
          "failed first promotion retains existing previous");
    artfmt_test_reset();
    check_bundle_pixels(path, 22);
    CHECK(artfile_publish(path, tmp, prev) == ARTFILE_PUBLISHED,
          "recover missing primary from validated temp");
    check_bundle_pixels(prev, 22);
    /* Exercise each generic result directly with the valid temp still left
     * by failed bundle publication. */
    artfmt_test_fail(ARTFMT_TEST_RENAME, 1, EIO);
    CHECK(!artbundle_write(path, 42, ARTCACHE_NOWPLAYING, 4, 4, pix, hdr),
          "prepare validated temp");
    artfmt_test_reset();
    /* An open error other than "not found" on the primary is a deferral:
     * nothing renamed, tmp kept (never "missing", never a rename on a
     * guess). Owner session 2026-10-19. */
    artfmt_test_fail(ARTFMT_TEST_PROBE, 1, EIO);
    CHECK(artfile_publish(path, tmp, prev) == ARTFILE_RETAINED,
          "probe error defers publication");
    artfmt_test_reset();
    CHECK(access(tmp, F_OK) == 0, "probe error keeps the validated temp");
    check_bundle_pixels(path, 22);
    artfmt_test_fail(ARTFMT_TEST_RENAME, 1, EIO);
    CHECK(artfile_publish(path, tmp, prev) == ARTFILE_RETAINED,
          "rotation failure retains primary");
    artfmt_test_reset();
    artfmt_test_fail(ARTFMT_TEST_RENAME, 2, EIO);
    CHECK(artfile_publish(path, tmp, prev) == ARTFILE_RETAINED,
          "promotion failure rolled back primary");
    artfmt_test_reset();
    artfmt_test_fail(ARTFMT_TEST_RENAME, 6, EIO);
    CHECK(artfile_publish(path, tmp, prev) == ARTFILE_RECOVER_PREV,
          "failed rollback explicitly names previous recovery");
    artfmt_test_reset();
    check_bundle_pixels(path, 22);

    artidx_init(&rd);
    artfmt_test_fail(ARTFMT_TEST_OPEN, 1, EACCES);
    CHECK(artidx_open(&rd, p("durable-index")) == ARTIDX_IO_ERROR,
          "permission failure is not missing");
    artfmt_test_reset();
    CHECK(artidx_open(&rd, p("durable-index")) == ARTIDX_MISSING,
          "ENOENT is missing");
    fd = open(p("durable-index"), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    CHECK(fd >= 0 && write(fd, "cut", 3) == 3, "truncated-index fixture");
    close(fd);
    CHECK(artidx_open(&rd, p("durable-index")) == ARTIDX_CORRUPT,
          "verified EOF truncation is corrupt");
    artfmt_test_fail(ARTFMT_TEST_READ, 1, EIO);
    CHECK(artidx_open(&rd, p("durable-index")) == ARTIDX_IO_ERROR,
          "read error is not corrupt");
    artfmt_test_reset();
    artfmt_test_fail(ARTFMT_TEST_SHORT_READ, 1, 0);
    artfmt_test_fail(ARTFMT_TEST_READ, 2, EIO);
    CHECK(artidx_open(&rd, p("durable-index")) == ARTIDX_IO_ERROR,
          "error after short positive read is I/O, not corruption");
    artfmt_test_reset();
    artidx_close(&rd);
}

/* #90: the source signature compare (artrec_sig_*) */
static void test_signature(void)
{
    struct artcache_rec old, now;

    memset(&old, 0, sizeof(old));
    old.key = 0x1234;
    old.state = ARTREC_OK;
    old.src_kind = ARTSRC_EMBEDDED;
    old.src_hash = 0xabc;
    old.src_size = 50000;
    old.src_crc = 0xdeadbeef;
    old.mtime = 100;
    now = old;
    now.mtime = 200;                    /* tags changed: not compared */
    CHECK(artrec_sig_valid(&old), "valid");
    CHECK(!artrec_sig_missing(&old), "not missing");
    CHECK(artrec_sig_same(&old, &now), "same source");
    now.src_crc ^= 1;
    CHECK(!artrec_sig_same(&old, &now), "crc differs");
    now = old;
    now.src_size++;
    CHECK(!artrec_sig_same(&old, &now), "size differs");
    now = old;
    now.src_hash++;
    CHECK(!artrec_sig_same(&old, &now), "path differs");
    now = old;
    now.src_kind = ARTSRC_FILE;
    CHECK(!artrec_sig_same(&old, &now), "kind differs");
    /* no valid signature: trusted, filled lazily, never "same" */
    old.src_crc = 0;
    now = old;
    CHECK(!artrec_sig_valid(&old) && artrec_sig_missing(&old), "crc 0");
    CHECK(!artrec_sig_same(&old, &now), "crc 0 never same");
    old.src_crc = 7;
    old.src_size = 0;
    CHECK(artrec_sig_missing(&old), "size 0");
    old.src_size = 9;
    /* states and records that never carry one */
    old.state = ARTREC_NOART;
    CHECK(!artrec_sig_valid(&old) && !artrec_sig_missing(&old), "noart");
    old.state = ARTREC_FAILED;
    CHECK(!artrec_sig_missing(&old), "failed");
    old.state = ARTREC_OK;
    old.flags = ARTREC_F_CHAPTER;       /* src fields = its book's key */
    CHECK(!artrec_sig_valid(&old) && !artrec_sig_missing(&old), "chapter");
    old.flags = 0;
    old.src_kind = ARTSRC_NONE;
    CHECK(!artrec_sig_valid(&old) && !artrec_sig_missing(&old), "none");
}

/* #90 review: the signature styles and the unsignable mark */
static void test_signature_styles(void)
{
    struct artcache_rec old, now;

    memset(&old, 0, sizeof(old));
    old.state = ARTREC_OK;
    old.src_kind = ARTSRC_FILE;
    old.src_type = 3;                       /* AA_TYPE_JPG: old style */
    old.src_size = 5000;
    old.src_crc = 0x1234;
    old.src_hash = 7;
    now = old;
    now.src_type = 0;
    CHECK(artrec_sig_same(&old, &now), "old style compares old style");
    now.src_type = ARTREC_T_SIG2;
    CHECK(!artrec_sig_same(&old, &now), "styles never compare equal");
    old.src_type |= ARTREC_T_SIG2;
    CHECK(artrec_sig_same(&old, &now) && artrec_src_type(&old) == 3 /* AA_TYPE_JPG */,
          "sig2 compares sig2, type masked");
    old.src_crc = 0;
    CHECK(artrec_sig_missing(&old), "missing");
    old.src_type |= ARTREC_T_UNSIGNED;
    CHECK(!artrec_sig_missing(&old) && artrec_src_type(&old) == 3 /* AA_TYPE_JPG */,
          "unsigned: tried, not owed");
}

/* #100: panelpool.idx and the shown set's pick */
static int shown_count(const struct artpool_idx *x, int set, uint64_t lo,
                       uint64_t hi)
{
    int k, n = 0;

    for (k = 0; k < ARTPANEL_SLOTS; k++)
    {
        const struct artpanel_ent *e = &x->shown.h.ent[k];

        if (e->state == ARTREC_OK && ARTPANEL_ENT_SET(e) == set
            && e->key >= lo && e->key <= hi)
            n++;
    }
    return n;
}

static void test_pool(void)
{
    static struct artpool_idx x, keep;
    struct artpanel_hdr ph;
    uint64_t r1[ARTPOOL_RAND];
    int k, n, n1, j, overlap;

    CHECK(sizeof(struct artpool_idx) <= 5 * 1024, "pool idx size %u",
          (unsigned)sizeof(struct artpool_idx));
    artpool_init(&x, 300, 300);
    CHECK(artpool_ok(&x.pool) && artshown_ok(&x.shown.h, 300, 300),
          "empty idx");
    CHECK(artpool_free_slot(&x.pool) == 0, "empty: slot 0");
    x.pool.n_slots = 3;
    x.pool.ent[0] = (struct artpanel_ent){ 900, 0, 0, 0, ARTREC_OK, 0, {0} };
    x.pool.ent[1] = (struct artpanel_ent){ 901, 0, 0, 0, ARTREC_OK, 2, {0} };
    artpool_seal(&x);
    CHECK(artpool_ok(&x.pool) && artpool_free_slot(&x.pool) == 1,
          "a free slot inside");
    x.pool.ent[1].slot = 3;
    CHECK(!artpool_ok(&x.pool), "slot past n_slots accepted");
    x.pool.ent[1].slot = 2;
    x.pool.crc ^= 1;
    CHECK(!artpool_ok(&x.pool), "bad crc accepted");

    /* panels.pak: music 1..20, books 101..106; the pool: music 201..260,
     * books 301..310, and key 5 again (panels.pak's copy wins) */
    artpanel_hdr_init(&ph, 300, 300);
    for (k = 0; k < 26; k++)
    {
        ph.ent[k].key = k < 20 ? 1 + k : 101 + (k - 20);
        ph.ent[k].state = ARTREC_OK;
        ph.ent[k].slot = k;
        ARTPANEL_ENT_SET(&ph.ent[k]) = k < 20 ? 0 : 1;
    }
    ph.n_slots = 26;
    artpool_init(&x, 300, 300);
    for (k = 0; k < 71; k++)
    {
        struct artpanel_ent *e = &x.pool.ent[k];

        e->key = k < 60 ? 201 + k : k < 70 ? 301 + (k - 60) : 5;
        e->state = ARTREC_OK;
        e->slot = k;
        ARTPANEL_ENT_SET(e) = (k >= 60 && k < 70) ? 1 : 0;
    }
    x.pool.n_slots = 71;
    for (k = 0; k < 15; k++)
        x.pool.pinned[k] = 1 + k;           /* played, newest */
    for (k = 0; k < 6; k++)
        x.pool.pinned[15 + k] = 101 + k;
    x.pool.n_pinned = 21;
    artpool_seal(&x);
    CHECK(artpool_ok(&x.pool), "pool ok");

    n = artpool_pick(&x, &ph, 1, false);
    CHECK(n == 32 && artshown_ok(&x.shown.h, 300, 300), "first pick %d", n);
    CHECK(shown_count(&x, 0, 1, 14) == 14 && shown_count(&x, 0, 15, 15) == 0,
          "music: 14 pinned, 10 places left to random");
    CHECK(shown_count(&x, 0, 0, ~0ull) == 24, "music 32 - 8 books");
    CHECK(shown_count(&x, 1, 0, ~0ull) == 8, "8 books");
    CHECK(shown_count(&x, 1, 301, 310) == ARTPOOL_BOOK_RAND,
          "books: 3 random (Q10)");
    CHECK(shown_count(&x, 1, 101, 105) == 5 && shown_count(&x, 1, 106, 106)
          == 0, "books: the first 5 pinned");
    for (k = 0; k < 14; k++)
        CHECK(x.shown.h.ent[k].key == (uint64_t)(1 + k), "pinned order %d", k);
    for (k = 0, j = 0; k < ARTPANEL_SLOTS; k++)
        if (x.shown.h.ent[k].key == 5)
            j++;
    CHECK(j == 1, "a key in both files shown once");
    for (k = 0; k < ARTPANEL_SLOTS; k++)
    {
        const struct artpanel_ent *e = &x.shown.h.ent[k];

        if (e->key >= 201)
            CHECK(artshown_pool(e) && (uint64_t)(e->slot - ARTPOOL_SLOT0)
                  == (e->key >= 301 ? 60 + e->key - 301 : e->key - 201),
                  "pool slot encoded");
        else
            CHECK(!artshown_pool(e) && e->slot < 26, "pak slot kept");
    }
    n1 = x.pool.n_rand;
    CHECK(n1 == ARTPOOL_MUSIC_RAND + ARTPOOL_BOOK_RAND, "random picks %d", n1);
    memcpy(r1, x.pool.rand, sizeof(r1));

    /* a refresh keeps the random picks */
    keep = x;
    artpool_pick(&x, &ph, 99, false);
    CHECK(x.pool.n_rand == n1 && !memcmp(x.pool.rand, r1, n1 * 8),
          "refresh keeps the picks");

    /* a rotation: fresh random picks, none of the previous */
    x = keep;
    n = artpool_pick(&x, &ph, 12345, true);
    CHECK(n == 32 && x.pool.n_rand == n1, "rotation count");
    for (k = 0, overlap = 0; k < x.pool.n_rand; k++)
        for (j = 0; j < n1; j++)
            if (x.pool.rand[k] == r1[j])
                overlap++;
    CHECK(overlap == 0, "rotation repeats %d previous picks", overlap);
    CHECK(shown_count(&x, 0, 1, 14) == 14 && shown_count(&x, 1, 301, 310) == 3,
          "rotation keeps the pinned and the shares");
    /* two seeds: two different sets (variety) */
    keep = x;
    artpool_pick(&x, &ph, 777, true);
    for (k = 0, overlap = 0; k < x.pool.n_rand; k++)
        for (j = 0; j < keep.pool.n_rand; j++)
            if (x.pool.rand[k] == keep.pool.rand[j])
                overlap++;
    CHECK(overlap == 0, "second rotation repeats %d", overlap);
    /* no panels.pak (NULL) and no pinned list: the pool alone */
    x.pool.n_pinned = 0;
    n = artpool_pick(&x, NULL, 5, true);
    CHECK(n == 32 && shown_count(&x, 1, 0, ~0ull) == 8
          && shown_count(&x, 1, 301, 310) == 8, "pool only %d", n);
}

int main(int argc, char **argv)
{
    snprintf(dir, sizeof(dir), "%s", argc > 1 ? argv[1] : "/tmp");
    test_crc();
    test_headers();
    test_index();
    test_torn();
    test_panel_read();
    test_table();
    test_gc_50k();
    test_panels();
    test_bundle();
    test_chapters();
    test_persistence();
    test_signature();
    test_signature_styles();
    test_pool();
    test_npcover();
    if (fails)
    {
        printf("%d failure(s)\n", fails);
        return 1;
    }
    return 0;
}
