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
 * Host test for the Apple fallback's pure half (apps/ipodui/afb_core.c,
 * #137): the database writer against the reference writer's bytes
 * (tools/itunesdb/mkitdb.py --golden), the verifier (good, cut, flipped),
 * the link verify rule, the park cycle's plan with a power cut at every
 * step, and the link table's records.
 *   test_core --spec FILE      write the golden spec (JSON)
 *   test_core --write FILE     write the same database with the C writer
 *   test_core [--golden FILE]  run the checks (and compare with FILE)
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "afb_core.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* -- the golden set ------------------------------------------------------ */
#define TS     0xe1f2a3b4u
#define DB_ID  0x0123456789abcdefull
#define PL_ID  0x0fedcba987654321ull
#define FIRST  100

static const struct itdb_track golden[] = {
    { "Stop Arguing Over Me", ":iPod_Control:Music:PKB:P0000001.MP3",
      "Peaches!", "The Black Keys", "Blues", 0x1111111111111111ull,
      3456789, 201000, 2, 2024, 192, 44100, 0, AFB_K_MP3 },
    { "Caf\xc3\xa9 \xe2\x80\x94 na\xc3\xafve \xf0\x9f\x8e\xb5",
      ":iPod_Control:Music:PKB:P0000002.MP3", "", "Bj\xc3\xb6rk", NULL,
      0x2222222222222222ull, 5000000, 240500, 0, 0, 167, 48000, 1,
      AFB_K_MP3 },
    { "Chapter Book", ":iPod_Control:Music:PKB:P0000003.M4B", "A Book",
      "An Author", "Audiobook", 0x3333333333333333ull, 98765432, 36000000,
      1, 2019, 64, 44100, 0, AFB_K_M4B },
    { "Synced Song", ":iPod_Control:Music:F07:ABCD.m4a", "Album", "Artist",
      "Pop", 0x4444444444444444ull, 4000000, 180000, 5, 2007, 256, 44100,
      0, AFB_K_M4A },
    { "Field Recording", ":iPod_Control:Music:PKB:P0000004.WAV", NULL, NULL,
      NULL, 0x5555555555555555ull, 52920044, 300000, 0, 0, 1411, 44100, 0,
      AFB_K_WAV },
    { "Master", ":iPod_Control:Music:PKB:P0000005.AIF", "Studio", "Band",
      "Rock", 0x6666666666666666ull, 10584044, 60000, 9, 1999, 1411, 44100,
      0, AFB_K_AIF },
};
#define NGOLD (sizeof(golden) / sizeof(golden[0]))

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++)
    {
        if (*s == '"' || *s == '\\')
            fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static void write_spec(const char *path)
{
    static const char *const kn[] = { "mp3", "mp3", "m4a", "m4b", "wav",
                                      "aif" };
    FILE *f = fopen(path, "w");

    fprintf(f, "{\"ts\": %u, \"db_id\": %llu, \"pl_id\": %llu, "
            "\"first_id\": %d, \"name\": \"ProkBox\", \"tracks\": [\n",
            TS, (unsigned long long)DB_ID, (unsigned long long)PL_ID, FIRST);
    for (unsigned i = 0; i < NGOLD; i++)
    {
        const struct itdb_track *t = &golden[i];

        fprintf(f, " {\"title\": ");
        json_str(f, t->title);
        fprintf(f, ", \"loc\": ");
        json_str(f, t->location);
        fprintf(f, ", \"album\": ");
        json_str(f, t->album);
        fprintf(f, ", \"artist\": ");
        json_str(f, t->artist);
        fprintf(f, ", \"genre\": ");
        json_str(f, t->genre);
        fprintf(f, ", \"dbid\": %llu, \"size\": %u, \"ms\": %u, "
                "\"track\": %u, \"year\": %u, \"kbps\": %u, \"sr\": %u, "
                "\"vbr\": %u, \"kind\": \"%s\"}%s\n",
                (unsigned long long)t->dbid, t->size, t->ms, t->track,
                t->year, t->kbps, t->sr, t->vbr, kn[t->kind],
                i + 1 < NGOLD ? "," : "");
    }
    fprintf(f, "]}\n");
    fclose(f);
}

/* an in-memory output */
struct mem {
    uint8_t *b;
    uint32_t n, cap;
};

static int mem_write(void *ctx, const void *p, uint32_t n)
{
    struct mem *m = ctx;

    if (m->n + n > m->cap)
    {
        m->cap = (m->n + n) * 2;
        m->b = realloc(m->b, m->cap);
    }
    memcpy(m->b + m->n, p, n);
    m->n += n;
    return 0;
}

static int mem_patch(void *ctx, uint32_t off, const void *p, uint32_t n)
{
    struct mem *m = ctx;

    if (off + n > m->n)
        return -1;
    memcpy(m->b + off, p, n);
    return 0;
}

static int mem_read(void *ctx, uint32_t off, void *p, uint32_t n)
{
    struct mem *m = ctx;

    if (off + n > m->n)
        return -1;
    memcpy(p, m->b + off, n);
    return 0;
}

static struct mem build_golden(void)
{
    struct mem m = { 0 };
    struct afb_out o = { &m, mem_write, mem_patch };
    struct itdb_w w;
    long len;

    itdb_begin(&w, &o, TS, DB_ID, PL_ID, FIRST);
    for (unsigned i = 0; i < NGOLD; i++)
        itdb_add(&w, &golden[i]);
    len = itdb_end(&w, "ProkBox");
    CHECK(len == (long)m.n, "itdb_end %ld vs %u", len, m.n);
    return m;
}

static int verify_mem(struct mem *m, bool strict, struct itdb_stats *st)
{
    struct afb_in in = { m, mem_read };

    return itdb_verify(&in, m->n, strict, st);
}

static void test_db(const char *golden_path)
{
    struct mem m = build_golden();
    struct itdb_stats st;
    int rc = verify_mem(&m, true, &st);

    CHECK(rc == ITDB_OK, "verify own db rc=%d", rc);
    CHECK(st.tracks == NGOLD && st.items == NGOLD && st.playlists == 1,
          "counts t=%u i=%u p=%u", st.tracks, st.items, st.playlists);
    CHECK(st.min_id == FIRST && st.max_id == FIRST + NGOLD - 1, "ids");
    if (golden_path)
    {
        FILE *f = fopen(golden_path, "rb");
        uint8_t *g = malloc(m.n + 1);
        size_t n = f ? fread(g, 1, m.n + 1, f) : 0;

        CHECK(f && n == m.n && !memcmp(g, m.b, m.n),
              "C writer differs from mkitdb.py (%zu vs %u bytes)", n, m.n);
        if (f && n == m.n)
            for (uint32_t i = 0; i < m.n; i++)
                if (g[i] != m.b[i])
                {
                    printf("  first difference at 0x%x\n", i);
                    break;
                }
        if (f)
            fclose(f);
        free(g);
    }

    /* cut: every shorter length fails */
    {
        uint32_t full = m.n, bad = 0;

        for (uint32_t cut = 1; cut < full; cut += 7)
        {
            m.n = cut;
            if (verify_mem(&m, true, &st) == ITDB_OK)
                bad++;
        }
        m.n = full;
        CHECK(bad == 0, "%u cut files verified", bad);
    }
    /* a version we never proved: refused when strict */
    {
        uint8_t v = m.b[16];

        m.b[16] = 0x19;
        CHECK(verify_mem(&m, true, &st) == ITDB_E_VERSION, "strict version");
        CHECK(verify_mem(&m, false, &st) == ITDB_OK, "lenient version");
        m.b[16] = v;
    }
    /* any change to a chunk's length or count field fails */
    {
        uint32_t bad = 0, tried = 0;

        for (uint32_t off = 0; off + 12 <= m.n; off++)
        {
            if (memcmp(m.b + off, "mh", 2))
                continue;
            if (!memcmp(m.b + off, "mhod", 4))
                continue;               /* a string's total also fails, below */
            for (int f = 4; f <= 8; f += 4)
            {
                m.b[off + f] ^= 0x04;
                tried++;
                if (verify_mem(&m, true, &st) == ITDB_OK)
                    bad++, printf("  flip at 0x%x+%d passed\n", off, f);
                m.b[off + f] ^= 0x04;
            }
        }
        CHECK(bad == 0 && tried > 20, "%u of %u header flips verified", bad,
              tried);
    }
    /* random flips: never a crash, never a read outside the file */
    {
        srand(137);
        for (int i = 0; i < 20000; i++)
        {
            uint32_t off = rand() % m.n;
            uint8_t x = 1u << (rand() % 8);

            m.b[off] ^= x;
            (void)verify_mem(&m, (i & 1), &st);
            m.b[off] ^= x;
        }
        CHECK(verify_mem(&m, true, &st) == ITDB_OK, "after fuzz");
    }
    free(m.b);
}

/* -- names ------------------------------------------------------------- */
static void test_names(void)
{
    char b[64];

    CHECK(afb_kind_of("/Music/a/b.MP3") == AFB_K_MP3, "mp3");
    CHECK(afb_kind_of("/x.m4a") == AFB_K_M4A, "m4a");
    CHECK(afb_kind_of("/x.M4B") == AFB_K_M4B, "m4b");
    CHECK(afb_kind_of("/x.aiff") == AFB_K_AIF, "aiff");
    CHECK(afb_kind_of("/x.wav") == AFB_K_WAV, "wav");
    CHECK(afb_kind_of("/x.flac") == AFB_K_NONE, "flac");
    CHECK(afb_kind_of("/x.ogg") == AFB_K_NONE, "ogg");
    CHECK(afb_kind_of("/Music/._x.mp3") == AFB_K_NONE, "mac sidecar");
    afb_link_name(b, sizeof(b), 42, AFB_K_M4B);
    CHECK(!strcmp(b, "P0000042.M4B"), "link name %s", b);
    CHECK(afb_location(b, sizeof(b), "/Music/Peaches!/02 Song.mp3")
          && !strcmp(b, ":Music:Peaches!:02 Song.mp3"), "location %s", b);
    CHECK(!afb_location(b, 8, "/Music/long.mp3"), "location too long");
    CHECK(afb_in_link_dir("/iPod_Control/Music/PKB/P0000001.MP3"), "pkb");
    CHECK(afb_in_link_dir("/ipod_control/music/pkb/x.mp3"), "pkb case");
    CHECK(!afb_in_link_dir("/iPod_Control/Music/PKBX/x.mp3"), "pkbx");
    CHECK(afb_in_music_dir("/iPod_Control/Music/F00/A.mp3"), "f00");
    CHECK(!afb_in_music_dir("/Music/A.mp3"), "not music dir");
}

/* -- the verify rule (docs/research/apple-fallback-links.md 5) --------- */
static void test_classify(void)
{
    struct afb_facts ok = { .orig_exists = true, .orig_fst = 10,
        .orig_size = 200000, .orig_healthy = true, .orig_eq_runs = true,
        .runs_known = true, .link_exists = true, .link_fst = 10,
        .link_size = 200000, .rec_fst = 10, .rec_size = 200000 };
    struct afb_facts f;

    CHECK(afb_classify(&ok) == AFB_V_OK, "ok");
    f = ok; f.orig_exists = false;
    CHECK(afb_classify(&f) == AFB_V_ORPHAN, "orphan");
    f = ok; f.orig_fst = 0; f.orig_size = 0; f.orig_healthy = false;
    CHECK(afb_classify(&f) == AFB_V_EMPTIED, "emptied");
    f.link_fst = 11;                    /* the link is not ours either */
    CHECK(afb_classify(&f) != AFB_V_EMPTIED, "emptied needs the link");
    f = ok; f.orig_size = 250000; f.orig_eq_runs = false;   /* appended */
    CHECK(afb_classify(&f) == AFB_V_STALE, "stale size");
    f = ok; f.orig_fst = 45;            /* cp over: new chain */
    CHECK(afb_classify(&f) == AFB_V_STALE, "stale fst");
    f = ok; f.orig_fst = 45; f.orig_healthy = false;
    CHECK(afb_classify(&f) == AFB_V_DAMAGED, "changed and broken");
    f = ok; f.orig_healthy = false; f.runs_free = true; f.link_exists = false;
    CHECK(afb_classify(&f) == AFB_V_FREED, "freed");
    f.runs_free = false;                /* reused by the host */
    CHECK(afb_classify(&f) == AFB_V_DAMAGED, "damaged");
    f.runs_free = true; f.runs_known = false;
    CHECK(afb_classify(&f) == AFB_V_DAMAGED, "freed, runs unknown");
    f = ok; f.orig_eq_runs = false;     /* same fst/size, other chain */
    CHECK(afb_classify(&f) == AFB_V_STALE, "stale chain");
    f = ok; f.link_exists = false;
    CHECK(afb_classify(&f) == AFB_V_LINK_GONE, "link gone");
    f = ok; f.runs_known = false; f.orig_eq_runs = false;
    CHECK(afb_classify(&f) == AFB_V_OK, "fragmented, unrecorded runs");
}

/* -- the park cycle: the plan, and a power cut at every step ----------- */
struct disk {
    uint8_t state;              /* the state file */
    bool links;                 /* the link folder */
    bool db_ok;                 /* iTunesDB matches the links */
    bool fp_same;
};

/* the steps unpark takes, in order (afb.c); returns after `cut` steps */
static int run(struct disk *d, unsigned plan, int cut)
{
    int step = 0;
#define STEP(x) do { if (step++ == cut) return -1; x; } while (0)
    if (plan & AFB_DO_PARK)
        STEP(d->links = false);
    if (plan & AFB_DO_TEARDOWN)
    {
        STEP(d->state = AFB_ST_NONE);
        return 0;
    }
    if (plan & AFB_DO_UNPARK)
    {
        STEP(d->state = AFB_ST_UNPARKING);
        STEP(d->links = true);          /* the folder and its links */
        STEP(d->db_ok = false);         /* iTunesDB.new written */
        STEP(d->db_ok = true);          /* the rename */
        STEP((d->state = AFB_ST_LIVE, d->fp_same = true));
    }
#undef STEP
    return 0;
}

static unsigned plan_for(const struct disk *d, bool on, int when)
{
    struct afb_situation s = { on, d->state, d->links, (uint8_t)when,
                               d->fp_same };
    return afb_plan(&s);
}

static void test_plan(void)
{
    struct afb_situation s;

    /* the table of the design */
    s = (struct afb_situation){ true, AFB_ST_NONE, false, AFB_AT_USB, 0 };
    CHECK(afb_plan(&s) == AFB_DO_UNPARK, "first run");
    s = (struct afb_situation){ true, AFB_ST_LIVE, false, AFB_AT_USB, 0 };
    CHECK(afb_plan(&s) == AFB_DO_UNPARK, "after a parked session");
    s = (struct afb_situation){ true, AFB_ST_LIVE, true, AFB_AT_BOOT, 1 };
    CHECK(afb_plan(&s) == 0, "trusted boot");
    s.fp_same = false;
    CHECK(afb_plan(&s) == (AFB_DO_VERIFY | AFB_DO_PARK | AFB_DO_UNPARK),
          "boot after a foreign mount");
    s = (struct afb_situation){ true, AFB_ST_LIVE, true, AFB_AT_USB, 1 };
    CHECK(afb_plan(&s) == (AFB_DO_VERIFY | AFB_DO_PARK | AFB_DO_UNPARK),
          "links survived a session");
    s = (struct afb_situation){ true, AFB_ST_UNPARKING, true, AFB_AT_BOOT, 1 };
    CHECK(afb_plan(&s) == (AFB_DO_PARK | AFB_DO_UNPARK), "cut unpark");
    s = (struct afb_situation){ false, AFB_ST_LIVE, true, AFB_AT_USB, 1 };
    CHECK(afb_plan(&s) == (AFB_DO_PARK | AFB_DO_TEARDOWN), "switch off");
    s = (struct afb_situation){ false, AFB_ST_NONE, false, AFB_AT_BOOT, 1 };
    CHECK(afb_plan(&s) == 0, "off and clean");

    /* every cut, then boots until settled: always LIVE, links, DB good */
    for (int start = 0; start < 3; start++)
        for (int cut = 0; cut < 8; cut++)
        {
            struct disk d = { 0 };
            int boots = 0;

            if (start == 1)             /* live, then a USB session */
                d = (struct disk){ AFB_ST_LIVE, false, true, false };
            if (start == 2)             /* live, Apple's disk mode after */
                d = (struct disk){ AFB_ST_LIVE, true, true, false };
            run(&d, plan_for(&d, true, AFB_AT_USB), cut);
            while (boots < 4)
            {
                unsigned p = plan_for(&d, true, AFB_AT_BOOT);
                if (!p)
                    break;
                boots++;
                run(&d, p, -1);
            }
            CHECK(d.state == AFB_ST_LIVE && d.links && d.db_ok && boots <= 1,
                  "start %d cut %d: state=%u links=%d db=%d boots=%d", start,
                  cut, d.state, d.links, d.db_ok, boots);
        }
    /* switch off with a cut anywhere: the next boot finishes it */
    for (int cut = 0; cut < 4; cut++)
    {
        struct disk d = { AFB_ST_LIVE, true, true, true };

        run(&d, plan_for(&d, false, AFB_AT_USB), cut);
        run(&d, plan_for(&d, false, AFB_AT_BOOT), -1);
        CHECK(d.state == AFB_ST_NONE && !d.links, "off cut %d", cut);
        CHECK(plan_for(&d, false, AFB_AT_BOOT) == 0, "off settled %d", cut);
    }
}

/* -- records ----------------------------------------------------------- */
static void test_recs(void)
{
    uint8_t buf[512];
    struct afb_rec r = { .fst = 10, .size = 200000, .linkno = 7,
        .nruns = 2, .kind = AFB_K_MP3, .runs = { 10, 5, 20, 8 } };
    struct afb_rec g;
    const char *path = "/Music/Peaches!/01 Song One.mp3", *p;
    uint32_t n, need;

    r.pathlen = strlen(path);
    n = afb_rec_bytes(&r);
    CHECK(n % 4 == 0 && n >= 16 + 16 + r.pathlen, "rec bytes %u", n);
    afb_rec_put(buf, &r, path);
    CHECK(afb_rec_get(buf, n, &g, &p, &need) == n, "rec get");
    CHECK(g.fst == 10 && g.size == 200000 && g.linkno == 7 && g.nruns == 2
          && g.runs[2] == 20 && g.runs[3] == 8 && g.kind == AFB_K_MP3
          && g.pathlen == r.pathlen && !memcmp(p, path, r.pathlen), "rec eq");
    CHECK(afb_rec_get(buf, n - 1, &g, &p, &need) == 0 && need == n,
          "short record");
    r.nruns = 0xff;
    afb_rec_put(buf, &r, path);
    CHECK(afb_rec_get(buf, afb_rec_bytes(&r), &g, &p, &need)
          == afb_rec_bytes(&r) && g.nruns == 0xff, "unrecorded runs");
    buf[14] = 9;                        /* nruns over the cap */
    CHECK(afb_rec_get(buf, sizeof(buf), &g, &p, &need) == 0 && need == 0,
          "bad record");
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--spec"))
    {
        write_spec(argv[2]);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--write"))
    {
        struct mem m = build_golden();
        FILE *f = fopen(argv[2], "wb");

        fwrite(m.b, 1, m.n, f);
        fclose(f);
        return 0;
    }
    test_names();
    test_db(argc == 3 && !strcmp(argv[1], "--golden") ? argv[2] : NULL);
    test_classify();
    test_plan();
    test_recs();
    printf("applefb core: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
