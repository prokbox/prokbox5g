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
 * Host test of the sync-time index format, apps/ipodui/xindex_core.c
 * (P12, brief 3.5). W0 skeleton: the format cases; W1-N, W3-K and W3-C
 * add the parsers and their fixtures
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
#include <string.h>
#include "ipodui/xindex_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EQ(a, b) do { long _a = (long)(a), _b = (long)(b); if (_a != _b) { \
    fails++; fprintf(stderr, "FAIL %s:%d: %s = %ld, want %ld\n", \
    __FILE__, __LINE__, #a, _a, _b); } } while (0)

/* The firmware's crc_32 (firmware/common/crc32.c), bitwise */
uint32_t crc_32(const void *src, uint32_t len, uint32_t crc);
uint32_t crc_32(const void *src, uint32_t len, uint32_t crc)
{
    const unsigned char *p = src;
    while (len--)
    {
        int b;
        crc ^= (uint32_t)*p++ << 24;
        for (b = 0; b < 8; b++)
            crc = crc & 0x80000000u ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}

static uint32_t buf[1024];              /* 4 KB, aligned */

/* A two-note index: header, 2 records, strings */
static uint32_t notes(void)
{
    static const char str[] = "Instructions\0/Notes/Instructions.txt\0"
                              "Shopping\0/Notes/Shopping.txt";
    struct xidx_hdr *h = (struct xidx_hdr *)buf;
    struct xnote_rec *r = (struct xnote_rec *)(h + 1);
    uint32_t bytes;

    memset(buf, 0, sizeof(buf));
    h->kind = XIDX_NOTES;
    h->sig = 0x12345678;
    h->count = 2;
    h->rec_off = sizeof(*h);
    h->str_off = h->rec_off + 2 * sizeof(*r);
    h->str_bytes = sizeof(str);
    r[0].name_off = 0;  r[0].path_off = 13; r[0].size = 812;
    r[1].name_off = 37; r[1].path_off = 46; r[1].size = 90;
    memcpy((char *)buf + h->str_off, str, sizeof(str));
    bytes = h->str_off + h->str_bytes;
    xidx_seal(buf, bytes);
    return bytes;
}

static void format(void)
{
    struct xidx_hdr *h = (struct xidx_hdr *)buf;
    uint32_t n = notes();
    char *c = (char *)buf;

    EQ(xidx_rec_size(XIDX_NOTES), 12);
    EQ(xidx_rec_size(XIDX_CONTACTS), 8);
    EQ(xidx_rec_size(XIDX_CAL), 20);
    EQ(xidx_rec_size(0), 0);
    /* the round trip */
    CHECK(h->magic == XIDX_MAGIC && h->version == XIDX_VERSION);
    EQ(xidx_check(h, n), XIDX_OK);
    EQ(xidx_check_body(buf, n), XIDX_OK);
    CHECK(strcmp(c + h->str_off + ((struct xnote_rec *)(h + 1))[1].name_off,
                 "Shopping") == 0);
    /* the CRC covers records and strings */
    c[h->str_off + 2] ^= 1;
    EQ(xidx_check_body(buf, n), XIDX_ECRC);
    n = notes();
    ((struct xnote_rec *)(h + 1))[0].size++;
    EQ(xidx_check_body(buf, n), XIDX_ECRC);
    /* header checks */
    n = notes();
    EQ(xidx_check(h, 31), XIDX_ESHORT);
    EQ(xidx_check(h, n - 1), XIDX_EBOUNDS);         /* strings cut */
    h->magic ^= 1;
    EQ(xidx_check(h, n), XIDX_EMAGIC);
    n = notes();
    h->version++;
    EQ(xidx_check(h, n), XIDX_EMAGIC);
    /* an index of the first format (version 1, before W3-K's pages) with
     * a sound CRC and the folder's signature: the builder's signature
     * test (xidx_check_body) refuses it, so it is rebuilt, never kept as
     * fresh */
    n = notes();
    EQ(XIDX_VERSION, 2);
    h->version = 1;
    EQ(xidx_check_body(buf, n), XIDX_EMAGIC);
    n = notes();
    h->kind = 9;
    EQ(xidx_check(h, n), XIDX_EKIND);
    n = notes();
    h->count = 3;                                   /* records overlap */
    EQ(xidx_check(h, n), XIDX_EBOUNDS);
    /* the caps */
    n = notes();
    EQ(xidx_check(h, XIDX_NOTES_FILE_MAX + 1), XIDX_ECAP);
    h->kind = XIDX_CAL;
    h->count = XCAL_MAX_INSTANCES + 1;
    h->str_off = h->rec_off + h->count * 20;
    h->str_bytes = 0;
    EQ(xidx_check(h, h->str_off), XIDX_ECAP);
}

static void signature(void)
{
    uint32_t base, s;

    EQ(xidx_sig_add(XIDX_SIG_INIT, "a.txt", 100, 5), 0x095c4f7f);
    base = xidx_sig_add(xidx_sig_add(XIDX_SIG_INIT, "a.txt", 100, 5),
                        "b.txt", 200, 6);
    s = xidx_sig_add(xidx_sig_add(XIDX_SIG_INIT, "a.txt", 100, 5),
                     "b.txt", 200, 6);
    CHECK(s == base);                               /* deterministic */
    s = xidx_sig_add(xidx_sig_add(XIDX_SIG_INIT, "a.txt", 100, 5),
                     "c.txt", 200, 6);
    CHECK(s != base);                               /* renamed */
    s = xidx_sig_add(xidx_sig_add(XIDX_SIG_INIT, "a.txt", 100, 5),
                     "b.txt", 201, 6);
    CHECK(s != base);                               /* resized */
    s = xidx_sig_add(xidx_sig_add(XIDX_SIG_INIT, "a.txt", 100, 5),
                     "b.txt", 200, 7);
    CHECK(s != base);                               /* touched */
    s = xidx_sig_add(base, "d.txt", 1, 1);
    CHECK(s != base);                               /* added */
    s = xidx_sig_add(XIDX_SIG_INIT, "a.txt", 100, 5);
    CHECK(s != base);                               /* removed */
}

/* -- Notes (W1-N) --------------------------------------------------------- */

/* the test's collation: case-insensitive bytes (the firmware's is
 * ui_sortkey(); the layout does not depend on which) */
static int ci_cmp(const char *a, int alen, const char *b, int blen)
{
    int i;

    for (i = 0; i < alen && i < blen; i++)
    {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];

        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y)
            return x - y;
    }
    return alen - blen;
}

static char pool[XNOTE_POOL];
static struct xnote_src src[XNOTE_MAX + 1];
static uint16_t scratch[XNOTE_SCRATCH(XNOTE_MAX + 1)];
static uint32_t big[XIDX_NOTES_FILE_MAX / 4];
static uint32_t pool_used;

static int add(const char *name, uint32_t size, int parent, int dir)
{
    static int n;
    int i;

    if (!name)                          /* reset */
    {
        n = 0;
        pool_used = 0;
        return 0;
    }
    i = n++;
    src[i].name = pool_used;
    src[i].size = size;
    src[i].parent = parent < 0 ? XNOTE_PARENT_ROOT : (uint16_t)parent;
    src[i].dir = dir;
    strcpy(pool + pool_used, name);
    pool_used += strlen(name) + 1;
    return i;
}

/* row i through the directory and its page (the reader's path) */
static const void *page_of(const void *f, int i, const struct xidx_pdir **d)
{
    const struct xidx_hdr *h = f;
    const struct xnote_tab *t = (const void *)(h + 1);
    const struct xidx_pdir *dir = (const void *)(t + 1);
    int p = xidx_dir_find(dir, t->npage, i);

    if (p < 0)
        return NULL;
    *d = &dir[p];
    return (const char *)f + dir[p].off;
}

static const struct xnote_rec *rec(const void *f, int i)
{
    const struct xidx_pdir *d;
    const void *pg = page_of(f, i, &d);

    return pg ? xidx_page_rec(pg, i - d->row0, sizeof(struct xnote_rec))
              : NULL;
}

static const char *name_of(const void *f, int i)
{
    const struct xidx_pdir *d;
    const void *pg = page_of(f, i, &d);

    return xidx_page_str(pg, rec(f, i)->name_off);
}

/* the directory and every page are sound; returns the page count */
static int pages_ok(const void *f, uint32_t bytes)
{
    const struct xidx_hdr *h = f;
    const struct xnote_tab *t = (const void *)(h + 1);
    const struct xidx_pdir *dir = (const void *)(t + 1);
    int p;

    EQ(t->hdr_end, 40 + 8 * t->npage);
    EQ(xidx_dir_check(dir, t->npage, h->count, t->hdr_end, bytes), XIDX_OK);
    if (t->npage)
        EQ(dir[0].off, t->hdr_end);     /* page 0 follows the directory */
    for (p = 0; p < t->npage; p++)
    {
        EQ(xidx_page_check((const char *)f + dir[p].off, &dir[p],
                           xidx_page_rows(dir, t->npage, p, h->count),
                           sizeof(struct xnote_rec)), XIDX_OK);
        CHECK(dir[p].bytes <= XNOTE_PAGE_LIMIT);
        /* the minimum, but on the last page */
        CHECK(p + 1 == t->npage || xidx_page_rows(dir, t->npage, p, h->count)
                                   >= XIDX_PAGE_ROWS_MIN);
    }
    return t->npage;
}

static void notes_layout(void)
{
    const struct xidx_hdr *h = (const struct xidx_hdr *)big;
    int32_t n;
    int work, misc;
    uint32_t s;

    EQ(xnote_display_len("Shopping.txt", 0), 8);
    EQ(xnote_display_len("Shopping.TXT", 0), 8);
    EQ(xnote_display_len("Trips.txt", 1), 9);       /* a folder: whole */
    EQ(xnote_display_len(".txt", 0), 4);
    EQ(xnote_display_len("readme", 0), 6);
    CHECK(xnote_is_note("a.txt") && xnote_is_note("B.Txt"));
    CHECK(!xnote_is_note("._a.txt") && !xnote_is_note(".txt"));
    CHECK(!xnote_is_note("a.md") && !xnote_is_note("txt"));

    /* the walk's order: root entries and two folders, notes inside */
    add(NULL, 0, 0, 0);
    add("zebra.txt", 10, -1, 0);                    /* 0 */
    work = add("Work", 0, -1, 1);                   /* 1 */
    add("apple.TXT", 20, -1, 0);                    /* 2 */
    misc = add("misc", 0, -1, 1);                   /* 3 */
    add("Plan.txt", 30, work, 0);                   /* 4 */
    add("agenda.txt", 40, work, 0);                 /* 5 */
    add("note.txt", 50, misc, 0);                   /* 6 */
    n = xnote_emit(big, sizeof(big), src, 7, pool, 0xabcd1234, ci_cmp,
                   scratch);
    CHECK(n > 0);
    EQ(xidx_check(h, n), XIDX_OK);
    EQ(xidx_check_body(big, n), XIDX_OK);
    EQ(h->kind, XIDX_NOTES);
    EQ(h->count, 8);
    EQ(h->sig, 0xabcd1234);
    /* record 0: the folder; its four entries sorted, folders mixed in */
    CHECK(rec(big, 0)->size & XNOTE_DIR);
    EQ(XNOTE_FIRST(rec(big, 0)->size), 1);
    EQ(XNOTE_COUNT(rec(big, 0)->size), 4);
    CHECK(!strcmp(name_of(big, 1), "apple.TXT"));
    CHECK(!strcmp(name_of(big, 2), "misc"));
    CHECK(!strcmp(name_of(big, 3), "Work"));
    CHECK(!strcmp(name_of(big, 4), "zebra.txt"));
    EQ(rec(big, 1)->size, 20);
    /* misc's note, then Work's two, in the root's order of the folders */
    EQ(XNOTE_FIRST(rec(big, 2)->size), 5);
    EQ(XNOTE_COUNT(rec(big, 2)->size), 1);
    EQ(XNOTE_FIRST(rec(big, 3)->size), 6);
    EQ(XNOTE_COUNT(rec(big, 3)->size), 2);
    CHECK(!strcmp(name_of(big, 5), "note.txt"));
    CHECK(!strcmp(name_of(big, 6), "agenda.txt"));
    CHECK(!strcmp(name_of(big, 7), "Plan.txt"));
    EQ(rec(big, 7)->size, 30);
    EQ(rec(big, 6)->path_off, rec(big, 6)->name_off);
    /* one page holds the eight rows */
    EQ(pages_ok(big, n), 1);
    for (s = 1; s < 8; s++)
        CHECK(rec(big, s)->name_off > rec(big, s - 1)->name_off);

    /* an empty folder: just the root */
    n = xnote_emit(big, sizeof(big), src, 0, pool, XIDX_SIG_INIT, ci_cmp,
                   scratch);
    EQ(xidx_check_body(big, n), XIDX_OK);
    EQ(h->count, 1);
    EQ(XNOTE_COUNT(rec(big, 0)->size), 0);

    /* a folder inside a folder is refused (one level, stock) */
    add(NULL, 0, 0, 0);
    work = add("a", 0, -1, 1);
    misc = add("b", 0, work, 1);
    add("c.txt", 1, misc, 0);
    EQ(xnote_emit(big, sizeof(big), src, 3, pool, 0, ci_cmp, scratch),
       XIDX_EBOUNDS);
    /* caps: too many entries; too small an output */
    EQ(xnote_emit(big, sizeof(big), src, XNOTE_MAX + 1, pool, 0, ci_cmp,
                  scratch), XIDX_ECAP);
    add(NULL, 0, 0, 0);
    add("a.txt", 1, -1, 0);
    EQ(xnote_emit(big, 32 + 24, src, 1, pool, 0, ci_cmp, scratch),
       XIDX_ECAP);
}

/* The scale (brief 8 row 9): 1,000 notes with long names fit the 64 KB
 * index and the builder's pool */
static void notes_scale(void)
{
    const struct xidx_hdr *h = (const struct xidx_hdr *)big;
    char name[64];
    int32_t n;
    int i;

    add(NULL, 0, 0, 0);
    for (i = 0; i < XNOTE_MAX; i++)
    {
        snprintf(name, sizeof(name), "Meeting notes %04d.txt",
                 (i * 7919) % XNOTE_MAX);
        add(name, 100 + i, -1, 0);
    }
    CHECK(pool_used <= XNOTE_POOL);
    n = xnote_emit(big, sizeof(big), src, XNOTE_MAX, pool, 1, ci_cmp,
                   scratch);
    CHECK(n > 0 && n <= XIDX_NOTES_FILE_MAX);
    EQ(xidx_check_body(big, n), XIDX_OK);
    EQ(h->count, XNOTE_MAX + 1);
    /* 1,001 rows of ~34 B: pages of about 15 rows */
    i = pages_ok(big, n);
    CHECK(i >= 60 && i <= 80);
    CHECK(!strcmp(name_of(big, 1), "Meeting notes 0000.txt"));
    CHECK(!strcmp(name_of(big, XNOTE_MAX), "Meeting notes 0999.txt"));
    for (i = 2; i <= XNOTE_MAX; i++)
        CHECK(strcmp(name_of(big, i - 1), name_of(big, i)) < 0);
    /* the largest pool the builder takes fits the cap: header, table and
     * directory, each page's header and pad, each row's record, and every
     * name twice at most (a cut label and its far name) */
    CHECK(XNOTE_HEAD_MAX + 7 * XNOTE_PDIR_MAX + 12 * (XNOTE_MAX + 1)
          + 2 * (XNOTE_POOL + XNOTE_MAX + 1) <= XIDX_NOTES_FILE_MAX);
    /* and the worst case built: the pool full of names just past a
     * label, every one cut and far */
    add(NULL, 0, 0, 0);
    for (i = 0; pool_used + 50 <= XNOTE_POOL && i < XNOTE_MAX; i++)
    {
        snprintf(name, sizeof(name), "%04d%041d.txt", i, 0);
        add(name, 1, -1, 0);
    }
    n = xnote_emit(big, sizeof(big), src, i, pool, 1, ci_cmp, scratch);
    CHECK(n > 0 && n <= XIDX_NOTES_FILE_MAX);
    EQ(xidx_check_body(big, n), XIDX_OK);
    pages_ok(big, n);
}

/* -- W3-K review: long names, windows across pages ------------------------ */

/* Every reader window (the rows from just above the top to just below
 * the last visible row, as screens/notes.c keep_window) of a list at
 * [first, first + count) lies in at most two pages, and a cache of two
 * of the largest pages beside the head shows every row of it: the
 * reader's path, top by top down the list and back up in wheel steps */
static void windows(const void *f, uint32_t first, uint32_t count,
                    uint32_t page_limit, uint32_t rec_size,
                    const char *(*row_name)(const void *, uint32_t))
{
    static uint8_t cb[2 * (XIDX_PAGE_MAX + 3)];
    const struct xidx_hdr *h = f;
    const struct xnote_tab *t = (const void *)(h + 1);
    const struct xidx_pdir *dir = (const void *)(t + 1);
    struct xidx_pcache c;
    uint32_t vis = 9, top, lo, hi, r, steps, pass;
    int p, p0, p1;

    xidx_pc_init(&c, cb, 2 * (page_limit + 3));
    for (pass = 0; pass < 2; pass++)
        for (steps = 0; steps < count; steps++)
        {
            top = pass ? count - 1 - steps : (steps * 5) % count;
            lo = first + top > first ? first + top - 1 : first + top;
            hi = first + top + vis < first + count ? first + top + vis
                                                   : first + count - 1;
            p0 = xidx_dir_find(dir, t->npage, lo);
            p1 = xidx_dir_find(dir, t->npage, hi);
            CHECK(p0 >= 0 && p1 >= p0 && p1 - p0 <= 1);
            for (p = p0; p <= p1; p++)
                if (!xidx_pc_get(&c, 0, p))
                {
                    uint8_t *at = xidx_pc_room(&c, dir[p].bytes);

                    CHECK(at != NULL);
                    if (!at)
                        return;
                    memcpy(at, (const char *)f + dir[p].off, dir[p].bytes);
                    xidx_pc_add(&c, 0, p, dir[p].bytes);
                }
            xidx_pc_get(&c, 0, p0);
            /* every row of the window from the cache: never a blank */
            for (r = lo; r <= hi; r++)
            {
                const void *pg;
                const struct xnote_rec *e;
                const char *lab;

                p = xidx_dir_find(dir, t->npage, r);
                pg = xidx_pc_peek(&c, 0, p);
                CHECK(pg != NULL);
                if (!pg)
                    return;
                e = xidx_page_rec(pg, r - dir[p].row0, rec_size);
                lab = e ? xidx_page_str(pg, e->name_off) : NULL;
                CHECK(lab && *lab && strlen(lab) < XIDX_LABEL_MAX);
                CHECK(lab && !strcmp(lab, row_name(f, r)));
            }
        }
}

static const char *label_from_file(const void *f, uint32_t r)
{
    return name_of(f, (int)r);
}

/* The note's file name: the label, or the far name */
static const char *file_name(const void *f, int i)
{
    const struct xidx_hdr *h = f;
    const struct xnote_rec *e = rec(f, i);

    if (!(e->path_off & XNOTE_PATH_FAR))
        return name_of(f, i);
    CHECK((e->path_off & ~XNOTE_PATH_FAR) < h->str_bytes);
    return (const char *)f + h->str_off + (e->path_off & ~XNOTE_PATH_FAR);
}

static void notes_long_names(void)
{
    const struct xidx_hdr *h = (const struct xidx_hdr *)big;
    char name[512], want[256];
    const char *lab;
    int32_t n;
    int i, fold, far = 0;

    /* labels: kept whole below the cap; cut at a character, no space
     * before the ellipsis, the cap with its NUL at most */
    EQ(xidx_label_put(name, "Short", 5), 6);
    CHECK(!strcmp(name, "Short"));
    memset(want, 'a', 47);
    EQ(xidx_label_put(name, want, 47), 48);
    EQ(xidx_label_put(name, want, 48), 45 + 3);
    CHECK(!memcmp(name + 44, XIDX_ELLIPSIS, 4));
    strcpy(want, "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"
                 "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"
                 "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"
                 "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"
                 "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9");      /* 56 B */
    i = (int)xidx_label_put(name, want, strlen(want));
    CHECK(i <= XIDX_LABEL_MAX && (i - 4) % 2 == 0);  /* whole characters */
    strcpy(want, "Twenty one characters                       and more");
    xidx_label_put(name, want, strlen(want));
    CHECK(!strcmp(name, "Twenty one characters" XIDX_ELLIPSIS));

    /* 120 notes of ~214-byte names (FAT allows 255) and a folder of a
     * 200-byte name with 20 more, mixed with short ones */
    add(NULL, 0, 0, 0);
    memset(want, 'x', 200);
    want[200] = '\0';
    fold = add(want, 0, -1, 1);
    for (i = 0; i < 140; i++)
    {
        if (i % 7 == 3)
            snprintf(name, sizeof(name), "Note %03d.txt", i);
        else
            snprintf(name, sizeof(name), "Long note %03d %s.txt", i,
                     want + 1);
        add(name, 10 + i, i < 120 ? -1 : fold, 0);
    }
    CHECK(pool_used <= XNOTE_POOL);
    n = xnote_emit(big, sizeof(big), src, 141, pool, 7, ci_cmp, scratch);
    CHECK(n > 0 && n <= XIDX_NOTES_FILE_MAX);
    EQ(xidx_check_body(big, n), XIDX_OK);
    EQ(h->count, 142);
    CHECK(pages_ok(big, n) >= 2);
    CHECK(h->str_off + h->str_bytes == (uint32_t)n && h->str_bytes > 0);
    for (i = 1; i < (int)h->count; i++)
    {
        const struct xnote_rec *e = rec(big, i);
        const char *fn = file_name(big, i);
        int is_dir = (e->size & XNOTE_DIR) != 0;
        int dl = xnote_display_len(fn, is_dir);

        lab = name_of(big, i);
        CHECK(strlen(lab) < XIDX_LABEL_MAX);
        if (e->path_off & XNOTE_PATH_FAR)
        {
            far++;
            /* the display name, cut: a prefix and the ellipsis */
            CHECK(strlen(fn) + 1 > XIDX_LABEL_MAX);
            CHECK(!memcmp(lab, fn, strlen(lab) - 3));
            CHECK(!strcmp(lab + strlen(lab) - 3, XIDX_ELLIPSIS));
            CHECK((int)strlen(lab) - 3 < dl);
        }
        else
        {
            EQ(e->path_off, e->name_off);
            CHECK(!strcmp(lab, fn));
        }
    }
    EQ(far, 121);                       /* the folder and its long notes */
    /* the folder's record: its notes' range, its far name whole */
    for (i = 1; i < (int)h->count; i++)
        if (rec(big, i)->size & XNOTE_DIR)
        {
            EQ(XNOTE_COUNT(rec(big, i)->size), 20);
            CHECK(!strcmp(file_name(big, i), want));
            windows(big, XNOTE_FIRST(rec(big, i)->size), 20,
                    XNOTE_PAGE_LIMIT, sizeof(struct xnote_rec),
                    label_from_file);
        }
    /* the root's list: every window in two pages, never a blank row */
    windows(big, 1, XNOTE_COUNT(rec(big, 0)->size), XNOTE_PAGE_LIMIT,
            sizeof(struct xnote_rec), label_from_file);
    /* the cache the reader keeps (head and two pages) fits its block */
    CHECK(40 + 8 * ((const struct xnote_tab *)(h + 1))->npage
          <= XNOTE_HEAD_MAX);
}

/* The page-load rate: the loads of the last second */
static void load_rate(void)
{
    struct xidx_rate r;
    uint32_t t;

    memset(&r, 0, sizeof(r));
    EQ(xidx_rate_recent(&r, 1000, 100), 0);
    for (t = 0; t < 6; t++)
        xidx_rate_add(&r, 1000 + t * 10);
    EQ(xidx_rate_recent(&r, 1060, 100), 6);
    EQ(xidx_rate_recent(&r, 1125, 100), 3);         /* 1030..1050 left */
    for (t = 0; t < 10; t++)
        xidx_rate_add(&r, 2000 + t);
    EQ(r.n, XIDX_RATE_N);
    EQ(xidx_rate_recent(&r, 2010, 100), XIDX_RATE_N);
    EQ(xidx_rate_recent(&r, 2200, 100), 0);
}

/* The reader's page cache: LRU eviction moves the later slots down */
static void page_cache(void)
{
    static uint8_t cb[1000];
    struct xidx_pcache c;
    uint8_t *p;
    int i;

    xidx_pc_init(&c, cb, sizeof(cb));
    CHECK(xidx_pc_get(&c, 0, 0) == NULL);
    for (i = 0; i < 3; i++)
    {
        p = xidx_pc_room(&c, 301);
        CHECK(p != NULL);
        memset(p, 'a' + i, 301);
        xidx_pc_add(&c, 0, i, 301);
    }
    EQ(c.used, 3 * 304);
    p = (uint8_t *)xidx_pc_get(&c, 0, 0);           /* page 0: newest now */
    CHECK(p && p[0] == 'a');
    p = xidx_pc_room(&c, 200);                      /* evicts page 1 */
    CHECK(p != NULL);
    CHECK(xidx_pc_get(&c, 0, 1) == NULL);
    p = (uint8_t *)xidx_pc_get(&c, 0, 2);
    CHECK(p && p[0] == 'c' && p[300] == 'c' && p == cb + 304);
    p = (uint8_t *)xidx_pc_get(&c, 0, 0);
    CHECK(p && p == cb && p[300] == 'a');
    CHECK(xidx_pc_room(&c, 1001) == NULL);          /* never fits */
    xidx_pc_add(&c, 1, 0, 200);
    CHECK(xidx_pc_get(&c, 1, 0) == cb + 608);
    CHECK(xidx_pc_get(&c, 0, 0) != NULL);           /* list 0 != list 1 */
}

int main(void)
{
    format();
    signature();
    notes_layout();
    notes_scale();
    notes_long_names();
    load_rate();
    page_cache();
    if (fails)
        return 1;
    printf("ok   sync-time index format (round trip, CRC, header checks, "
           "caps, version 1 refused, folder signature, paged Notes layout, "
           "1,000-note scale, long names and windows across pages, load "
           "rate, page cache)\n");
    return 0;
}
