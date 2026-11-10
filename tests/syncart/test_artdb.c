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

/* Host tests for apps/ipodui/artdb_core.c (#225 S3), the reader of the
 * stock firmware's cover database and song database.
 *
 *   test_artdb DIR            the tests (DIR: scratch for fixtures)
 *   test_artdb --survey DIR   aggregates of a real Artwork folder (and
 *                             DIR/../iTunes/iTunesDB if present): counts,
 *                             header lengths, image shapes. Prints no id,
 *                             name, path or pixel; for local use only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "artdb_core.h"
#include "fixwrite.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++;                  \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__);       \
    printf("\n"); } } while (0)

/* -- a file in memory as an afb_in --------------------------------------- */
struct buf {
    uint8_t *b;
    uint32_t n;
};

static int buf_read(void *ctx, uint32_t off, void *p, uint32_t n)
{
    struct buf *m = ctx;

    if (off > m->n || n > m->n - off)
        return -1;
    memcpy(p, m->b + off, n);
    return 0;
}

static struct buf slurp(const char *path)
{
    struct buf m = { NULL, 0 };
    FILE *f = fopen(path, "rb");
    long n;

    if (!f)
        return m;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    m.b = malloc(n ? n : 1);
    m.n = fread(m.b, 1, n, f);
    fclose(f);
    return m;
}

/* -- the spec the fixture is written from -------------------------------- */
#define NSONG 7
static const struct fx_fmt fmts[2] = { { 1028, 100, 100 }, { 1029, 200, 200 } };
static const char *const paths[NSONG] = {
    "/iPod_Control/Music/F00/ABCD.mp3",
    "/iPod_Control/Music/F23/WXYZ.m4a",
    "/iPod_Control/Music/F49/Caf\xc3\xa9 \xf0\x9f\x8e\xb5.mp3", /* é, U+1F3B5 */
    "/iPod_Control/Music/F07/QQQQ.mp3",
    NULL,                               /* a song with no location */
    "/iPod_Control/Music/F01/NOAR.mp3", /* no cover */
    "/iPod_Control/Music/F02/LAST.mp3",
};
static const int group[NSONG] = { 0, 0, 1, 2, -1, -1, 1 };

struct seen_img {
    int n;
    struct artdb_img im[64];
};

static int img_cb(void *ctx, const struct artdb_img *im)
{
    struct seen_img *s = ctx;

    if (s->n < 64)
        s->im[s->n] = *im;
    s->n++;
    return 0;
}

struct seen_song {
    int n;
    struct { uint64_t dbid; uint32_t art_id; uint16_t cnt; uint8_t has;
             char path[300]; } s[64];
};

static int song_cb(void *ctx, const struct artdb_song *so)
{
    struct seen_song *s = ctx;

    if (s->n < 64)
    {
        s->s[s->n].dbid = so->dbid;
        s->s[s->n].art_id = so->art_id;
        s->s[s->n].cnt = so->art_count;
        s->s[s->n].has = so->has_art;
        snprintf(s->s[s->n].path, sizeof(s->s[0].path), "%s", so->path);
    }
    s->n++;
    return 0;
}

static int stop_cb(void *ctx, const struct artdb_img *im)
{
    (void)im;
    return ++*(int *)ctx == 2 ? ARTDB_E_STOP : 0;
}

/* write the fixture; ids[i] = mhii id of song i (0 none) */
static int fx_label;                    /* fixtures get mhii labels */

static void write_fixture(const char *dir, uint32_t mhit_hl, int utf8,
                          const struct fx_img_opt *opt_for_song3,
                          uint32_t *ids)
{
    char p[512];
    FILE *f;
    struct fx_art aw;
    struct fx_db dw;

    snprintf(p, sizeof(p), "%s/ArtworkDB", dir);
    f = fopen(p, "wb");
    fx_art_begin(&aw, f, fmts, 2, 100);
    aw.label = fx_label;
    for (int i = 0; i < NSONG; i++)
    {
        uint32_t off[2];

        ids[i] = 0;
        if (group[i] < 0)
            continue;
        off[0] = group[i] * 20000;
        off[1] = group[i] * 80000;
        ids[i] = fx_art_add(&aw, 0xa000000000000000ull + i, 1000 + i, off,
                            i == 3 ? opt_for_song3 : NULL);
    }
    fx_art_end(&aw);
    fclose(f);
    snprintf(p, sizeof(p), "%s/iTunesDB", dir);
    f = fopen(p, "wb");
    fx_db_begin(&dw, f, mhit_hl, utf8);
    for (int i = 0; i < NSONG; i++)
    {
        struct fx_song s = { paths[i], 0xa000000000000000ull + i, ids[i],
                             ids[i] ? 1000 + i : 0 };

        fx_db_add(&dw, &s);
    }
    fx_db_end(&dw);
    fclose(f);
}

static int read_art(struct buf *m, struct seen_img *s, uint32_t *bad)
{
    struct afb_in in = { m, buf_read };

    memset(s, 0, sizeof(*s));
    return artdb_read(&in, m->n, img_cb, s, bad);
}

static int read_songs(struct buf *m, struct seen_song *s)
{
    struct afb_in in = { m, buf_read };

    memset(s, 0, sizeof(*s));
    return artdb_songs(&in, m->n, song_cb, s);
}

/* 1. write -> read back -> compare with the spec */
static void t_roundtrip(const char *dir)
{
    uint32_t ids[NSONG], bad = 99;
    struct seen_img si;
    struct seen_song ss;
    char p[512];
    struct buf a, d;
    int k = 0, rc;

    write_fixture(dir, 0x184, 0, NULL, ids);
    snprintf(p, sizeof(p), "%s/ArtworkDB", dir);
    a = slurp(p);
    snprintf(p, sizeof(p), "%s/iTunesDB", dir);
    d = slurp(p);
    rc = read_art(&a, &si, &bad);
    CHECK(rc == 10, "images %d, want 10 (5 songs x 2 formats)", rc);
    CHECK(bad == 0, "bad %u", bad);
    for (int i = 0; i < NSONG; i++)
    {
        if (group[i] < 0)
            continue;
        for (int f = 0; f < 2; f++, k++)
        {
            const struct artdb_img *im = &si.im[k];
            char want[24];

            snprintf(want, sizeof(want), "F%u_1.ithmb", fmts[f].id);
            CHECK(im->dbid == 0xa000000000000000ull + i, "dbid %d", i);
            CHECK(im->img_id == ids[i], "img_id %d", i);
            CHECK(im->fmt == fmts[f].id, "fmt %d/%d", i, f);
            CHECK(im->w == fmts[f].w && im->h == fmts[f].h, "w/h %d/%d", i, f);
            CHECK(im->size == (uint32_t)fmts[f].w * fmts[f].h * 2, "size");
            CHECK(im->offset == (uint32_t)group[i] * (f ? 80000 : 20000),
                  "offset %d/%d: %u", i, f, im->offset);
            CHECK(im->pad_v == 0 && im->pad_h == 0, "pad");
            CHECK(im->src_size == 1000u + i, "src_size %d", i);
            CHECK(!strcmp(im->file, want), "name %d/%d: %s", i, f, im->file);
        }
    }
    rc = read_songs(&d, &ss);
    CHECK(rc == NSONG - 1, "songs %d, want %d (one has no location)", rc,
          NSONG - 1);
    for (int i = 0, j = 0; i < NSONG && j < ss.n; i++)
    {
        if (!paths[i])
            continue;
        CHECK(ss.s[j].dbid == 0xa000000000000000ull + i, "song dbid %d", i);
        CHECK(!strcmp(ss.s[j].path, paths[i]), "path %d: %s", i,
              ss.s[j].path);
        CHECK(ss.s[j].art_id == ids[i], "art_id %d: %u", i, ss.s[j].art_id);
        CHECK(ss.s[j].has == (ids[i] ? 1 : 2), "has_art %d", i);
        CHECK(ss.s[j].cnt == (ids[i] ? 1 : 0), "art count %d", i);
        j++;
    }
    /* the callback's stop ends the walk with its value */
    {
        struct afb_in in = { &a, buf_read };
        int cnt = 0;

        CHECK(artdb_read(&in, a.n, stop_cb, &cnt, NULL) == ARTDB_E_STOP,
              "stop");
    }
    /* every cut of the file is an error, never a crash or a count */
    for (uint32_t n = 0; n < a.n; n++)
    {
        struct buf c = { a.b, n };
        uint32_t b2;

        if (read_art(&c, &si, &b2) >= 0)
        {
            CHECK(0, "cut at %u read as whole", n);
            break;
        }
    }
    for (uint32_t n = 0; n < d.n; n++)
    {
        struct buf c = { d.b, n };

        if (read_songs(&c, &ss) >= 0)
        {
            CHECK(0, "song db cut at %u read as whole", n);
            break;
        }
    }
    /* a chunk whose length runs past its parent: the first mhii's */
    {
        uint32_t at = 0x84 + 0x60 + 0x5c;   /* mhfd, mhsd, mhli */
        uint32_t tl = a.b[at + 8] | a.b[at + 9] << 8;

        CHECK(!memcmp(a.b + at, "mhii", 4), "fixture layout");
        a.b[at + 8] = (tl + 0x1000) & 0xff;
        a.b[at + 9] = (tl + 0x1000) >> 8;
        CHECK(read_art(&a, &si, &bad) == ARTDB_E_NEST, "nest");
        a.b[at + 8] = tl & 0xff;
        a.b[at + 9] = tl >> 8;
        /* an mhni length past its container */
        at += 0x98 + 0x18;
        CHECK(!memcmp(a.b + at, "mhni", 4), "fixture layout mhni");
        a.b[at + 8] += 4;
        CHECK(read_art(&a, &si, &bad) == ARTDB_E_NEST, "mhni nest");
        a.b[at + 8] -= 4;
        /* a wrong tag */
        a.b[at] = 'X';
        CHECK(read_art(&a, &si, &bad) == ARTDB_E_TAG, "tag");
        a.b[at] = 'm';
        CHECK(read_art(&a, &si, &bad) == 10, "restored");
        /* not a cover database at all */
        a.b[0] = 'X';
        CHECK(read_art(&a, &si, &bad) == ARTDB_E_MAGIC, "magic");
        a.b[0] = 'm';
    }
    /* bytes past the database's own length: not a database we trust */
    {
        struct buf t = { realloc(a.b, a.n + 4), a.n + 4 };

        memset(t.b + a.n, 0, 4);
        CHECK(read_art(&t, &si, &bad) == ARTDB_E_LEN, "trailing bytes");
        a.b = t.b;
    }
    free(a.b);
    free(d.b);
}

/* 1b. an image record with a non-thumbnail child (a label string, as
 * photo databases carry): skipped, the thumbnails still read */
static void t_label(const char *dir)
{
    uint32_t ids[NSONG], bad = 9;
    struct seen_img si;
    struct buf a;
    char p[512];

    fx_label = 1;
    write_fixture(dir, 0x184, 0, NULL, ids);
    fx_label = 0;
    snprintf(p, sizeof(p), "%s/ArtworkDB", dir);
    a = slurp(p);
    CHECK(read_art(&a, &si, &bad) == 10 && bad == 0, "labelled records");
    CHECK(si.im[0].w == 100 && !strcmp(si.im[0].file, "F1028_1.ithmb"),
          "labelled: first image");
    free(a.b);
}

/* 2. one bad image is skipped and counted, the rest still read */
static void t_bad_images(const char *dir)
{
    /* ok: accepted. The padded shapes are the ones a real database holds
     * (#225 survey: the mhni's w/h are the content, size is the stored
     * slot, whose rows may be wider: 55 x 55 in a 56-pixel stride; a
     * 48-wide cover with pad_h 7 in the same slot) */
    static const struct { struct fx_img_opt o; bool ok; const char *what; }
    cases[] = {
        { { 12345, 0, 0, NULL, 0, 0 }, false, "size not rows of the slot" },
        { { 691200, 720, 480, NULL, 0, 0 }, false, "720x480 TV-out size" },
        { { 0, 0, 0, "F1029_1.ithmb", 0, 0 }, false, "no leading colon" },
        { { 0, 0, 0, ":Thumbs:F1029_1.ithmb", 0, 0 }, false,
          "a folder in the name" },
        { { 0, 0, 0, ":../F1029_1.ithmb", 0, 0 }, false, "a separator" },
        { { 0, 0, 0, ":F1029_1.ithmb/x", 0, 0 }, false, "a slash after" },
        { { 0, 0, 0, ":F10a9_1.ithmb", 0, 0 }, false, "not digits" },
        { { 0, 0, 0, ":F1029_1.jpg", 0, 0 }, false, "another extension" },
        { { 0, 0, 0, ":F1029_.ithmb", 0, 0 }, false, "no file number" },
        { { 0, 0, 0, ":F1234567_1.ithmb", 0, 0 }, false, "too many digits" },
        { { 80000, 200, 200, NULL, -1, 0 }, false, "negative padding" },
        { { 80000, 200, 200, NULL, 0, 10 }, false, "slot wider than stride" },
        { { 6161, 55, 55, NULL, 0, 0 }, false, "size not whole rows" },
        { { 100000, 200, 200, NULL, 0, 50 }, false, "slot over 240" },
        { { 6160, 55, 55, NULL, 0, 0 }, true, "55x55 in a 56 stride" },
        { { 6160, 48, 55, NULL, 0, 7 }, true, "48 wide, pad_h 7" },
        { { 80000, 150, 200, NULL, 0, 50 }, true, "150 wide in 200 slot" },
        { { 80000, 200, 120, NULL, 80, 0 }, true, "120 high in 200 slot" },
        { { 2, 1, 1, NULL, 0, 0 }, true, "1x1 is fine (control)" },
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
    {
        struct fx_img_opt o[2];
        uint32_t ids[NSONG], bad = 0;
        struct seen_img si;
        struct buf a;
        char p[512];
        int rc;
        bool control = cases[c].ok;

        memset(o, 0, sizeof(o));
        o[1] = cases[c].o;              /* song 3's 200 px image */
        write_fixture(dir, 0x184, 0, o, ids);
        snprintf(p, sizeof(p), "%s/ArtworkDB", dir);
        a = slurp(p);
        rc = read_art(&a, &si, &bad);
        CHECK(rc == (control ? 10 : 9) && bad == (control ? 0u : 1u),
              "%s: images %d bad %u", cases[c].what, rc, bad);
        if (control && cases[c].o.size)
        {
            /* the reader fills the slot: the overridden image is the
             * one whose size matches (sizes differ from the defaults) */
            const struct fx_img_opt *q = &cases[c].o;
            int k, hit = 0;

            for (k = 0; k < si.n && k < 64; k++)
            {
                const struct artdb_img *im = &si.im[k];

                if (im->size != q->size || im->w != q->w || im->h != q->h)
                    continue;
                hit++;
                CHECK(im->slot_w == q->w + q->pad_h
                      && im->slot_h == q->h + q->pad_v
                      && im->stride == q->size / 2 / (q->h + q->pad_v),
                      "%s: slot %ux%u stride %u", cases[c].what,
                      im->slot_w, im->slot_h, im->stride);
            }
            CHECK(hit == 1, "%s: overridden image seen %d times",
                  cases[c].what, hit);
        }
        free(a.b);
    }
}

/* 3. the song walk: UTF-8 locations, and no +352 in a short header */
static void t_songs(const char *dir)
{
    uint32_t ids[NSONG];
    struct seen_song ss;
    struct buf d;
    char p[512];

    write_fixture(dir, 0x184, 1, NULL, ids);
    snprintf(p, sizeof(p), "%s/iTunesDB", dir);
    d = slurp(p);
    CHECK(read_songs(&d, &ss) == NSONG - 1, "utf8 songs");
    CHECK(!strcmp(ss.s[2].path, paths[2]), "utf8 path: %s", ss.s[2].path);
    free(d.b);
    write_fixture(dir, 0xf4, 0, NULL, ids);
    d = slurp(p);
    CHECK(read_songs(&d, &ss) == NSONG - 1, "short-header songs");
    for (int j = 0; j < ss.n; j++)
        CHECK(ss.s[j].art_id == 0, "short header: +352 read (%d)", j);
    CHECK(ss.s[0].has == 1 && ss.s[0].cnt == 1, "short header keeps has/count");
    free(d.b);
    /* a header that holds +352 but is shorter than 0x184: not trusted */
    write_fixture(dir, 0x170, 0, NULL, ids);
    d = slurp(p);
    CHECK(d.n > 0x80 + 352 && d.b[0x68 + 0x60 + 0x5c + 352] != 0,
          "fixture: the 0x170 header carries a link");
    CHECK(read_songs(&d, &ss) == NSONG - 1, "0x170-header songs");
    CHECK(ss.s[0].art_id == 0, "0x170 header: +352 trusted");
    free(d.b);
    /* an empty or absent file */
    {
        struct buf e = { (uint8_t *)"", 0 };
        struct seen_img si;

        CHECK(read_art(&e, &si, NULL) < 0, "empty cover db");
        CHECK(read_songs(&e, &ss) < 0, "empty song db");
    }
}

/* 4. random damage never crashes and never reads out of bounds */
static void t_fuzz(const char *dir)
{
    uint32_t ids[NSONG], seed = 12345;
    struct seen_img si;
    struct seen_song ss;
    struct buf a, d;
    char p[512];
    int ok = 0;

    write_fixture(dir, 0x184, 0, NULL, ids);
    snprintf(p, sizeof(p), "%s/ArtworkDB", dir);
    a = slurp(p);
    snprintf(p, sizeof(p), "%s/iTunesDB", dir);
    d = slurp(p);
    for (int i = 0; i < 20000; i++)
    {
        struct buf *m = i & 1 ? &d : &a;
        uint32_t at, old;

        seed = seed * 1103515245 + 12345;
        at = (seed >> 8) % m->n;
        old = m->b[at];
        m->b[at] ^= 1 + (seed >> 24) % 255;
        if ((i & 1 ? read_songs(m, &ss) : read_art(m, &si, NULL)) >= 0)
            ok++;
        m->b[at] = old;
    }
    CHECK(read_art(&a, &si, NULL) == 10, "fuzz restored");
    printf("fuzz: 20000 single-byte damages, %d still parsed\n", ok);
    free(a.b);
    free(d.b);
}

/* -- the survey of a real folder (aggregates only) ------------------------ */
struct shape { uint32_t fmt, w, h, size, n; };
struct survey {
    struct shape sh[16];
    int nsh;
    uint32_t imgs, maxid;
    uint64_t prev_dbid;
    uint32_t repeat_dbid, shared_off;
    uint32_t last_off[16];
};

static int survey_cb(void *ctx, const struct artdb_img *im)
{
    struct survey *s = ctx;
    int i;

    for (i = 0; i < s->nsh; i++)
        if (s->sh[i].fmt == im->fmt && s->sh[i].w == im->w
            && s->sh[i].h == im->h)
            break;
    if (i == s->nsh && s->nsh < 16)
    {
        s->sh[i].fmt = im->fmt;
        s->sh[i].w = im->w;
        s->sh[i].h = im->h;
        s->sh[i].size = im->size;
        s->nsh++;
    }
    if (i < 16)
    {
        if (s->sh[i].n && s->last_off[i] == im->offset)
            s->shared_off++;            /* the same slot as the previous */
        s->last_off[i] = im->offset;
        s->sh[i].n++;
    }
    s->imgs++;
    return 0;
}

struct ssurvey { uint32_t n, linked, has1, has2, cnt0; };

static int ssurvey_cb(void *ctx, const struct artdb_song *so)
{
    struct ssurvey *s = ctx;

    s->n++;
    s->linked += so->art_id != 0;
    s->has1 += so->has_art == 1;
    s->has2 += so->has_art == 2;
    s->cnt0 += so->art_count == 0;
    return 0;
}

/* raw walk: header lengths per tag and mhii children */
static void tags_seen(const struct buf *m)
{
    static const char *const t[] = { "mhfd", "mhsd", "mhli", "mhii", "mhod",
        "mhni", "mhla", "mhlf", "mhif", "mhba", "mhia" };
    for (unsigned k = 0; k < sizeof(t) / sizeof(t[0]); k++)
    {
        uint32_t n = 0, hl_min = ~0u, hl_max = 0;

        for (uint32_t i = 0; i + 8 <= m->n; i += 4)
            if (!memcmp(m->b + i, t[k], 4))
            {
                uint32_t hl = m->b[i + 4] | m->b[i + 5] << 8;

                n++;
                hl_min = hl < hl_min ? hl : hl_min;
                hl_max = hl > hl_max ? hl : hl_max;
            }
        if (n)
            printf("  %s x%u header 0x%x..0x%x\n", t[k], n, hl_min, hl_max);
    }
}

static int survey(const char *dir)
{
    char p[1024];
    struct buf a, d;
    struct survey s;
    struct ssurvey ss;
    uint32_t bad = 0;
    int rc;

    snprintf(p, sizeof(p), "%s/ArtworkDB", dir);
    a = slurp(p);
    if (!a.b)
    {
        printf("survey: no cover database\n");
        return 1;
    }
    memset(&s, 0, sizeof(s));
    {
        struct afb_in in = { &a, buf_read };

        rc = artdb_read(&in, a.n, survey_cb, &s, &bad);
    }
    printf("survey cover db: %u bytes, rc=%d images=%u skipped=%u\n", a.n,
           rc, s.imgs, bad);
    printf("  version word +16 = %u, children +20 = %u\n",
           a.n >= 24 ? a.b[16] : 0, a.n >= 24 ? a.b[20] : 0);
    tags_seen(&a);
    for (int i = 0; i < s.nsh; i++)
        printf("  format %u: %ux%u, %u B, %u images\n", s.sh[i].fmt,
               s.sh[i].w, s.sh[i].h, s.sh[i].size, s.sh[i].n);
    printf("  consecutive images naming the same slot: %u\n", s.shared_off);
    snprintf(p, sizeof(p), "%s/../iTunes/iTunesDB", dir);
    d = slurp(p);
    if (d.b)
    {
        memset(&ss, 0, sizeof(ss));
        {
            struct afb_in in = { &d, buf_read };

            rc = artdb_songs(&in, d.n, ssurvey_cb, &ss);
        }
        printf("survey song db: %u bytes, rc=%d songs=%u linked(+352)=%u "
               "has_art 1/2=%u/%u art_count 0=%u, version word=0x%x\n",
               d.n, rc, ss.n, ss.linked, ss.has1, ss.has2, ss.cnt0,
               d.n >= 20 ? d.b[16] : 0);
    }
    else
        printf("survey song db: none at ../iTunes/iTunesDB\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--survey"))
        return survey(argv[2]);
    if (argc != 2)
    {
        fprintf(stderr, "usage: test_artdb DIR | --survey DIR\n");
        return 2;
    }
    t_roundtrip(argv[1]);
    t_label(argv[1]);
    t_bad_images(argv[1]);
    t_songs(argv[1]);
    t_fuzz(argv[1]);
    printf("syncart core: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
