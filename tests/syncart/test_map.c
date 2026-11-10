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

/* Host test of apps/ipodui/syncart.c (#225 S3): the synced-cover map over
 * fixtures from our own test-only writer (fixwrite.c), on real files.
 *
 *   test_map DIR            the checks (DIR: scratch, emptied first)
 *   test_map --perf DIR N   time a build over N songs (10 an album)
 *
 * Checks: the round trip (every representative found at its group's
 * offset in the 200 px file, the 100 px image never chosen); case-blind
 * paths; paths outside Music or in the link folder never looked up; a
 * cover past its image file's end dropped; no cover database: NONE and
 * the map removed; a song database that is not the covers' (match rate
 * < 50 %): MISMATCH, nothing found; the fallback's kept database tried
 * second; SAME until an input or the library commit changes, then STALE
 * and a rebuilt map with the new offsets; nothing ever written under
 * iPod_Control. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ftw.h>
#include "fixwrite.h"
#include "syncart.h"

int safefile_replace(const char *tmp, const char *dest, const char *prev)
{
    remove(prev);
    rename(dest, prev);
    if (rename(tmp, dest) != 0)
        return -1;
    remove(prev);
    return 0;
}

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

static char root[512];
static uint8_t blk_scratch[16384];

static void sh(const char *fmt, const char *a)
{
    char cmd[1200];

    snprintf(cmd, sizeof(cmd), fmt, a, a, a);
    if (system(cmd) != 0)
        exit(2);
}

static FILE *open_in(const char *rel)
{
    char p[1024];
    FILE *f;

    snprintf(p, sizeof(p), "%s%s", root, rel);
    f = fopen(p, "wb");
    if (!f)
    {
        perror(p);
        exit(2);
    }
    return f;
}

/* the library: n tracks, 10 an album, every 'nth' album without a cover
 * (0: all have one); dbids from base; covers stored once per album */
struct lib {
    int n, albums;
    char (*path)[64];
    int *group;                 /* -1 none */
    uint64_t *dbid;
};

static void lib_make(struct lib *l, int n, int nth)
{
    l->n = n;
    l->albums = (n + 9) / 10;
    l->path = calloc(n, sizeof(*l->path));
    l->group = calloc(n, sizeof(int));
    l->dbid = calloc(n, sizeof(uint64_t));
    for (int i = 0; i < n; i++)
    {
        int a = i / 10;

        snprintf(l->path[i], sizeof(l->path[i]),
                 "/iPod_Control/Music/F%02d/T%06d.mp3", (i * 7) % 50, i);
        l->group[i] = nth && a % nth == nth - 1 ? -1 : a;
    }
}

/* writes the cover database (groups in order: group g at slot g of
 * both files) and a song database with dbid base + i (or, with
 * songs_base != base, a song database whose ids differ) */
static void write_dbs(const struct lib *l, uint64_t base, uint64_t songs_base,
                      const char *songs_rel, long cut_large)
{
    static const struct fx_fmt fmts[2] = { { 1028, 100, 100 },
                                           { 1029, 200, 200 } };
    struct fx_art aw;
    struct fx_db dw;
    FILE *f;
    uint8_t *img = malloc(200 * 200 * 2);
    int ng = 0;

    for (int i = 0; i < l->n; i++)
        if (l->group[i] >= ng)
            ng = l->group[i] + 1;
    f = open_in("/iPod_Control/Artwork/ArtworkDB");
    fx_art_begin(&aw, f, fmts, 2, 100);
    for (int i = 0; i < l->n; i++)
    {
        uint32_t off[2];

        if (l->group[i] < 0)
            continue;
        off[0] = l->group[i] * 20000u;
        off[1] = l->group[i] * 80000u;
        fx_art_add(&aw, base + i, 1234, off, NULL);
    }
    fx_art_end(&aw);
    fclose(f);
    for (int k = 0; k < 2; k++)
    {
        f = open_in(k ? "/iPod_Control/Artwork/F1029_1.ithmb"
                      : "/iPod_Control/Artwork/F1028_1.ithmb");
        for (int g = 0; g < ng; g++)
        {
            unsigned s = k ? 200 : 100;

            fx_pattern(img, g, s, s);
            fwrite(img, 1, s * s * 2, f);
        }
        fclose(f);
    }
    if (cut_large >= 0)
    {
        char p[1024];

        snprintf(p, sizeof(p), "%s/iPod_Control/Artwork/F1029_1.ithmb",
                 root);
        if (truncate(p, cut_large) != 0)
            exit(2);
    }
    f = open_in(songs_rel);
    fx_db_begin(&dw, f, 0x184, 0);
    for (int i = 0; i < l->n; i++)
    {
        struct fx_song s = { l->path[i], songs_base + i, 0, 0 };

        fx_db_add(&dw, &s);
    }
    fx_db_end(&dw);
    fclose(f);
    free(img);
}

struct reps {
    const struct lib *l;
    int lower;                  /* hand the paths over lower-cased */
    int extra;                  /* + 2 albums outside Music / in PKB */
};

/* album a's art key (the map's key; any non-zero 64-bit value) */
#define KEY(a) (0x5a5a000000000000ull + (uint64_t)(a) * 0x9e3779b97f4a7c15ull)

static bool path_of(void *ctx, int i, char *buf, size_t len, uint64_t *key)
{
    struct reps *r = ctx;

    *key = KEY(i);
    if (i >= r->l->albums)
    {
        snprintf(buf, len, i == r->l->albums ? "/Music/A/B/x.mp3"
                                             : "/iPod_Control/Music/PKB/"
                                               "P0000001.MP3");
        return true;
    }
    snprintf(buf, len, "%s", r->l->path[i * 10]);
    if (r->lower)
        for (char *p = buf; *p; p++)
            if (*p >= 'A' && *p <= 'Z')
                *p += 'a' - 'A';
    return true;
}

static int build(const struct lib *l, uint32_t commit, int lower,
                 struct syncart_hdr *st)
{
    struct reps r = { l, lower, 1 };
    int n = l->albums + 2;
    size_t need = syncart_need(n);
    void *blk = malloc(need);
    int rc = syncart_build(commit, n, path_of, &r, blk, need, NULL, st);

    free(blk);
    return rc;
}

static int check(uint32_t commit)
{
    return syncart_check(commit, blk_scratch, sizeof(blk_scratch));
}

static int small_n;                    /* found_all(): 100 px images */
static int found_all(const struct lib *l, bool want_found, int *wrong)
{
    int found = 0;

    *wrong = 0;
    small_n = 0;
    for (int a = 0; a < l->albums; a++)
    {
        struct syncart_ent e;
        char file[600], want[600];
        int g = l->group[a * 10];

        if (!syncart_lookup(KEY(a), &e, file, sizeof(file)))
            continue;
        found++;
        if (g >= 0 && e.offset == g * 20000u && e.size == 20000
            && e.slot_w == 100 && e.slot_h == 100
            && strstr(file, "/F1028_1.ithmb"))
        {
            small_n++;                  /* the 100 px one: counted apart */
            continue;
        }
        snprintf(want, sizeof(want), "%s/iPod_Control/Artwork/F1029_1.ithmb",
                 root);
        if (!want_found || g < 0 || e.offset != g * 80000u
            || e.size != 80000 || e.slot_w != 200 || e.slot_h != 200
            || strcmp(file, want))
            (*wrong)++;
    }
    return found;
}

/* every entry under iPod_Control with its size and mtime, as one string
 * (nftw's order is the directory order: stable between two calls) */
static char *snap_out;
static size_t snap_len;
static int snap_one(const char *path, const struct stat *sb, int type,
                    struct FTW *ftw)
{
    size_t n = strlen(snap_out);

    (void)type; (void)ftw;
    snprintf(snap_out + n, snap_len - n, "%s %lld %ld.%09ld\n", path,
             (long long)sb->st_size, (long)sb->st_mtimespec.tv_sec,
             (long)sb->st_mtimespec.tv_nsec);
    return 0;
}
static void snapshot(char *out, size_t len)
{
    char p[600];

    snprintf(p, sizeof(p), "%s/iPod_Control", root);
    out[0] = '\0';
    snap_out = out;
    snap_len = len;
    nftw(p, snap_one, 8, FTW_PHYS);
}

static int perf(const char *dir, int n)
{
    struct lib l;
    struct syncart_hdr st;
    struct timespec a, b;
    int rc;

    snprintf(root, sizeof(root), "%s", dir);
    sh("rm -rf '%s' && mkdir -p '%s/iPod_Control/iTunes' "
       "'%s/iPod_Control/Artwork'", root);
    sh("mkdir -p '%s/.rockbox'", root);
    syncart_root = root;
    lib_make(&l, n, 0);
    write_dbs(&l, 1000, 1000, "/iPod_Control/iTunes/iTunesDB", -1);
    clock_gettime(CLOCK_MONOTONIC, &a);
    rc = build(&l, 1, 0, &st);
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("perf songs=%d albums=%d rc=%d n=%u images=%u ms=%.1f "
           "block=%zu map=%zu\n", n, l.albums, rc, st.n, st.images,
           (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6,
           syncart_need(l.albums + 2),
           (size_t)SYNCART_HDR_BYTES + st.n * sizeof(struct syncart_ent));
    clock_gettime(CLOCK_MONOTONIC, &a);
    {
        int wrong, f = found_all(&l, true, &wrong);

        clock_gettime(CLOCK_MONOTONIC, &b);
        printf("perf lookups=%d found=%d wrong=%d us_each=%.1f\n", l.albums,
               f, wrong, ((b.tv_sec - a.tv_sec) * 1e6
                          + (b.tv_nsec - a.tv_nsec) / 1e3) / l.albums);
    }
    return rc == SYNCART_BUILT ? 0 : 1;
}

/* Padded slots and wide strides (the reader's rule from a real
 * database, artdb_img_geom()): album 0, a 48 x 55 picture with pad_h 7 in
 * a 55 x 55 slot stored with a 56-pixel stride (6,160 B); album 1, a
 * 150 x 200 picture with pad_h 50 in a 200 x 200 slot. Through the map
 * and syncart_row_rgb(), each comes back as the slot's own pixels (the
 * fixture's pattern at the slot size), never the stride's extra column. */
static void padded(void)
{
    static const struct fx_fmt fmt = { 1029, 200, 200 };
    static const struct { unsigned pw, ph, pad_h, sw, sh, stride; } im[2] = {
        { 48, 55, 7, 55, 55, 56 }, { 150, 200, 50, 200, 200, 200 },
    };
    struct lib l;
    struct fx_art aw;
    struct fx_db dw;
    struct syncart_hdr st;
    FILE *f;
    uint32_t off = 0, offs[2];
    uint8_t *pat = malloc(200 * 200 * 2), *row = malloc(240 * 2);
    uint8_t *rgb = malloc(240 * 4), *slot;
    int a, rc;

    sh("rm -rf '%s/iPod_Control/Artwork' && mkdir -p "
       "'%s/iPod_Control/Artwork' && true '%s'", root);
    lib_make(&l, 20, 0);                /* 2 albums */
    f = open_in("/iPod_Control/Artwork/F1029_1.ithmb");
    for (a = 0; a < 2; a++)
    {
        offs[a] = off;
        fx_pattern(pat, a, im[a].sw, im[a].sh);
        for (unsigned y = 0; y < im[a].sh; y++)
        {
            memset(row, 0xa5, im[a].stride * 2);    /* the stride's tail */
            memcpy(row, pat + y * im[a].sw * 2, im[a].sw * 2);
            fwrite(row, 1, im[a].stride * 2, f);
        }
        off += im[a].stride * im[a].sh * 2;
    }
    fclose(f);
    f = open_in("/iPod_Control/Artwork/ArtworkDB");
    fx_art_begin(&aw, f, &fmt, 1, 100);
    for (int i = 0; i < l.n; i++)
    {
        struct fx_img_opt o;

        a = i / 10;
        memset(&o, 0, sizeof(o));
        o.size = im[a].stride * im[a].sh * 2;
        o.w = im[a].pw;
        o.h = im[a].ph;
        o.pad_h = im[a].pad_h;
        fx_art_add(&aw, 7000 + i, 1234, &offs[a], &o);
    }
    fx_art_end(&aw);
    fclose(f);
    f = open_in("/iPod_Control/iTunes/iTunesDB");
    fx_db_begin(&dw, f, 0x184, 0);
    for (int i = 0; i < l.n; i++)
    {
        struct fx_song s = { l.path[i], 7000 + i, 0, 0 };

        fx_db_add(&dw, &s);
    }
    fx_db_end(&dw);
    fclose(f);
    rc = build(&l, 9, 0, &st);
    CHECK(rc == SYNCART_BUILT && st.n == 2, "padded build rc=%d n=%u", rc,
          st.n);
    for (a = 0; a < 2; a++)
    {
        struct syncart_ent e;
        char file[600];
        int fd, bad = 0;
        unsigned stride;

        if (!syncart_lookup(KEY(a), &e, file, sizeof(file)))
        {
            CHECK(0, "padded album %d not found", a);
            continue;
        }
        stride = e.size / (2u * e.slot_h);
        CHECK(e.slot_w == im[a].sw && e.slot_h == im[a].sh
              && stride == im[a].stride && e.offset == offs[a]
              && e.pad_h == (int)im[a].pad_h,
              "padded album %d: slot %ux%u stride %u off %u pad_h %d", a,
              e.slot_w, e.slot_h, stride, e.offset, e.pad_h);
        /* decode the way the art builder does: rows of the stride, the
         * slot's pixels of each */
        slot = malloc(e.size);
        fd = open(file, O_RDONLY);
        if (fd < 0 || pread(fd, slot, e.size, e.offset) != (ssize_t)e.size)
            bad = -1;
        if (fd >= 0)
            close(fd);
        fx_pattern(pat, a, im[a].sw, im[a].sh);
        for (unsigned y = 0; !bad && y < e.slot_h; y++)
        {
            syncart_row_rgb(slot + y * stride * 2, e.slot_w, rgb);
            for (unsigned x = 0; x < e.slot_w; x++)
            {
                const uint8_t *q = pat + (y * im[a].sw + x) * 2;
                unsigned v = q[0] | q[1] << 8;
                unsigned r = v >> 11, g = (v >> 5) & 63, b = v & 31;

                if (rgb[4 * x] != ((b << 3) | (b >> 2))
                    || rgb[4 * x + 1] != ((g << 2) | (g >> 4))
                    || rgb[4 * x + 2] != ((r << 3) | (r >> 2))
                    || rgb[4 * x + 3] != 0xff)
                    bad++;
            }
        }
        CHECK(!bad, "padded album %d: %d pixels differ", a, bad);
        free(slot);
    }
    free(pat);
    free(row);
    free(rgb);
}

int main(int argc, char **argv)
{
    static char before[1 << 16], after[1 << 16];
    struct syncart_hdr st;
    struct lib l, l2;
    int rc, wrong, f;
    char p[1024];
    struct stat sb;

    if (argc == 4 && !strcmp(argv[1], "--perf"))
        return perf(argv[2], atoi(argv[3]));
    if (argc != 2)
        return 2;
    snprintf(root, sizeof(root), "%s", argv[1]);
    sh("rm -rf '%s' && mkdir -p '%s/iPod_Control/iTunes' "
       "'%s/iPod_Control/Artwork'", root);
    snprintf(p, sizeof(p), "%s/.rockbox/afb", root);
    sh("mkdir -p '%s'", p);
    syncart_root = root;

    /* no cover database: none */
    CHECK(check(1) == SYNCART_NONE, "no databases: none");

    /* round trip: 600 tracks, 60 albums, every 4th without a cover */
    lib_make(&l, 600, 4);
    write_dbs(&l, 5000, 5000, "/iPod_Control/iTunes/iTunesDB", -1);
    snapshot(before, sizeof(before));
    CHECK(check(1) == SYNCART_STALE_DBS, "no map yet: stale (databases)");
    rc = build(&l, 1, 0, &st);
    CHECK(rc == SYNCART_BUILT, "build rc=%d", rc);
    CHECK(st.status == SYNCART_ST_OK && st.n == 45 && st.albums == 60
          && st.images == 2 * 450 && st.matched == st.images
          && st.db_which == 0,
          "header status=%u n=%u albums=%u images=%u matched=%u which=%u",
          st.status, st.n, st.albums, st.images, st.matched, st.db_which);
    f = found_all(&l, true, &wrong);
    CHECK(f == 45 && wrong == 0, "lookups found=%d wrong=%d", f, wrong);
    {
        struct syncart_ent e;

        CHECK(syncart_lookup(KEY(0), &e, NULL, 0) && e.offset == 0,
              "album 0 not found");
        CHECK(!syncart_lookup(KEY(l.albums + 7), &e, NULL, 0)
              && !syncart_lookup(0, &e, NULL, 0), "an unknown key found");
        /* the albums whose representative is outside Music or in our
         * link folder: never in the map */
        CHECK(!syncart_lookup(KEY(l.albums), &e, NULL, 0)
              && !syncart_lookup(KEY(l.albums + 1), &e, NULL, 0),
              "outside Music found");
        CHECK(!syncart_synced("/iPod_Control/MusicX/a.mp3")
              && syncart_synced("/IPOD_CONTROL/music/F01/A.MP3")
              && !syncart_synced("/ipod_control/music/pkb/x.mp3"),
              "synced path rule");
    }
    CHECK(check(1) == SYNCART_SAME, "same inputs: same");
    CHECK(check(2) == SYNCART_STALE, "library commit changed: stale");

    /* representatives handed over lower-cased: the same map */
    rc = build(&l, 1, 1, &st);
    f = found_all(&l, true, &wrong);
    CHECK(rc == SYNCART_BUILT && f == 45 && wrong == 0,
          "lower-cased build rc=%d found=%d wrong=%d", rc, f, wrong);

    /* a cover database that changed (a re-sync: groups shifted) */
    lib_make(&l2, 600, 3);
    write_dbs(&l2, 5000, 5000, "/iPod_Control/iTunes/iTunesDB", -1);
    CHECK(check(1) == SYNCART_STALE_DBS, "new cover database: stale");
    rc = build(&l2, 1, 0, &st);
    f = found_all(&l2, true, &wrong);
    CHECK(rc == SYNCART_BUILT && f == 40 && wrong == 0,
          "rebuilt rc=%d found=%d wrong=%d", rc, f, wrong);
    CHECK(check(1) == SYNCART_SAME, "rebuilt: same");

    /* covers past their image file's end: dropped (bounds, §5.4) */
    write_dbs(&l, 5000, 5000, "/iPod_Control/iTunes/iTunesDB",
              10 * 80000 + 100);
    rc = build(&l, 1, 0, &st);
    f = found_all(&l, true, &wrong);
    /* 200 px: groups 0..9 fit, except 3 and 7 (no cover): 8; the 37
     * others fall back to their 100 px image (that file is whole) */
    CHECK(rc == SYNCART_BUILT && f == 45 && wrong == 0 && small_n == 37,
          "cut image file rc=%d found=%d wrong=%d small=%d", rc, f, wrong,
          small_n);

    /* a song database that is not the covers' (another sync): mismatch */
    write_dbs(&l, 5000, 900000, "/iPod_Control/iTunes/iTunesDB", -1);
    rc = build(&l, 1, 0, &st);
    f = found_all(&l, true, &wrong);
    CHECK(rc == SYNCART_BUILT && st.status == SYNCART_ST_MISMATCH && f == 0
          && st.n == 0, "mismatch rc=%d status=%u found=%d matched=%u/%u",
          rc, st.status, f, st.matched, st.images);
    CHECK(check(1) == SYNCART_SAME, "a mismatch map holds until a change");

    /* ... and the kept one (the developer fallback's .orig) matches */
    write_dbs(&l, 5000, 5000, "/.rockbox/afb/iTunesDB.orig", -1);
    write_dbs(&l, 5000, 900000, "/iPod_Control/iTunes/iTunesDB", -1);
    rc = build(&l, 1, 0, &st);
    f = found_all(&l, true, &wrong);
    CHECK(rc == SYNCART_BUILT && st.status == SYNCART_ST_OK
          && st.db_which == 1 && f == 45 && wrong == 0,
          "kept database rc=%d status=%u which=%u found=%d wrong=%d", rc,
          st.status, st.db_which, f, wrong);
    CHECK(check(1) == SYNCART_SAME, "kept database: same");

    /* nothing written under iPod_Control by any of it */
    snprintf(p, sizeof(p), "%s/.rockbox/afb/iTunesDB.orig", root);
    remove(p);
    write_dbs(&l, 5000, 5000, "/iPod_Control/iTunes/iTunesDB", -1);
    snapshot(before, sizeof(before));
    rc = build(&l, 1, 0, &st);
    check(1);
    found_all(&l, true, &wrong);
    snapshot(after, sizeof(after));
    CHECK(rc == SYNCART_BUILT && !strcmp(before, after) && before[0],
          "iPod_Control changed (or listing empty)");

    /* the cover database gone: none, and the map removed */
    snprintf(p, sizeof(p), "%s/iPod_Control/Artwork/ArtworkDB", root);
    remove(p);
    CHECK(check(1) == SYNCART_NONE, "cover database gone: none");
    snprintf(p, sizeof(p), "%s/.rockbox/artcache/syncart.map", root);
    CHECK(stat(p, &sb) != 0, "map not removed");
    {
        struct syncart_ent e;

        CHECK(!syncart_lookup(KEY(0), &e, NULL, 0),
              "found with no map");
    }

    /* a damaged map header: stale */
    write_dbs(&l, 5000, 5000, "/iPod_Control/iTunes/iTunesDB", -1);
    build(&l, 1, 0, &st);
    {
        int fd = open(p, O_RDWR);
        char c = 'x';

        pwrite(fd, &c, 1, 40);
        close(fd);
    }
    syncart_forget();
    CHECK(check(1) == SYNCART_STALE_DBS, "damaged map header not stale");
    {
        struct syncart_ent e;

        CHECK(!syncart_lookup(KEY(0), &e, NULL, 0),
              "found through a damaged map");
    }

    /* a cover database the reader refuses (its magic damaged): kept as
     * BAD with no entries, so the next passes do not stream it again */
    write_dbs(&l, 5000, 5000, "/iPod_Control/iTunes/iTunesDB", -1);
    snprintf(p, sizeof(p), "%s/iPod_Control/Artwork/ArtworkDB", root);
    {
        int fd = open(p, O_RDWR);

        pwrite(fd, "xxxx", 4, 0);
        close(fd);
    }
    CHECK(check(1) == SYNCART_STALE_DBS, "damaged database: stale");
    rc = build(&l, 1, 0, &st);
    f = found_all(&l, true, &wrong);
    CHECK(rc == SYNCART_BUILT && st.status == SYNCART_ST_BAD && st.n == 0
          && f == 0, "bad database rc=%d status=%u n=%u found=%d", rc,
          st.status, st.n, f);
    CHECK(check(1) == SYNCART_SAME, "a bad database is remembered");
    CHECK(check(2) == SYNCART_STALE, "... until the library changes");

    padded();
    printf("syncart map: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
