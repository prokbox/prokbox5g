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
/* Round-trip test of the packed chapter table (chapters.h, abpack.c).
 * Built twice by tests/abbook/run.sh:
 *  - with -DWARBLE (HAVE_METADATA_EXTRAS: the packed layout): put, commit
 *    in place, sort, find, title copies, the fair pool share and its cut
 *    on a UTF-8 boundary, 1,024 entries at the default size, and the
 *    parsers' sequences (chpl then text track, the ID3 CHAP count);
 *  - without (stock): the accessor macros are the old array expressions.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "chapters.h"
#include "check.h"

#ifdef HAVE_METADATA_EXTRAS

static uint32_t mem[(AB_CH_TABLE_SIZE + 3) / 4];

static struct audiobook_chapters *fresh(int cap, int pool)
{
    struct audiobook_chapters *ab = (struct audiobook_chapters *)mem;
    memset(mem, 0xee, sizeof(mem));
    ab_ch_init(ab, cap, pool);
    return ab;
}

static void test_layout(void)
{
    CHECK_EQ(sizeof(struct ab_ch_entry), 8);
    CHECK_EQ(sizeof(struct audiobook_chapters), AB_CH_HDR_SIZE);
    CHECK_EQ(AB_CH_TABLE_SIZE, 32784);          /* §3.1 */
    CHECK_EQ(AB_CH_SIZE(10, 100), 16 + 80 + 100);
}

static void test_put_title(void)
{
    struct audiobook_chapters *ab = fresh(8, 1000);
    char buf[64];

    CHECK_EQ(ab->chapter_count, 0);
    CHECK_EQ(ab->pool_used, 1);
    CHECK_EQ(ab_ch_put(ab, 0, 0, "Preface", 7), 7);
    CHECK_EQ(ab_ch_put(ab, 1, 1000, "", 0), 0);
    CHECK_EQ(ab_ch_put(ab, 2, 2000, "Chapter 1", 9), 9);
    ab->chapter_count = 3;
    CHECK(!strcmp(ab_ch_title_ptr(ab, 0), "Preface"));
    CHECK(!strcmp(ab_ch_title_ptr(ab, 1), ""));
    CHECK_EQ(ab_ch_ent(ab)[1].title_off, 0);    /* the shared "" */
    CHECK(!strcmp(ab_ch_title_ptr(ab, 2), "Chapter 1"));
    CHECK_EQ(ab_ch_start(ab, 2), 2000);
    CHECK_EQ(ab->pool_used, 1 + 8 + 10);
    CHECK_EQ(ab->cut, 0);
    /* copies: terminated, cut on a boundary */
    CHECK_EQ(ab_ch_title(ab, 2, buf, sizeof(buf)), 9);
    CHECK(!strcmp(buf, "Chapter 1"));
    CHECK_EQ(ab_ch_title(ab, 2, buf, 5), 4);
    CHECK(!strcmp(buf, "Chap"));
    /* in-place decode + commit, the parsers' way */
    ab_ch_set_start(ab, 3, 3000);
    strcpy(ab_ch_title_buf(ab, 3), "Decoded");
    CHECK_EQ(ab_ch_commit(ab, 3), 7);
    ab->chapter_count = 4;
    CHECK(!strcmp(ab_ch_title_ptr(ab, 3), "Decoded"));
    /* earlier titles are untouched by a later decode */
    CHECK(!strcmp(ab_ch_title_ptr(ab, 2), "Chapter 1"));
    /* longer than 63 bytes: cut at 63 on a UTF-8 boundary ("é" = 2 B at
     * bytes 62-63 would straddle) */
    {
        char t[80];
        memset(t, 'a', 62);
        t[62] = (char)0xc3; t[63] = (char)0xa9;  /* é */
        strcpy(t + 64, "tail");
        CHECK_EQ(ab_ch_put(ab, 4, 4000, t, (int)strlen(t)), 62);
    }
    /* reset keeps cap and pool */
    ab_ch_reset(ab);
    CHECK_EQ(ab->chapter_count, 0);
    CHECK_EQ(ab->cap, 8);
    CHECK_EQ(ab->pool_size, 1000);
    CHECK_EQ(ab->pool_used, 1);
}

static void test_sort_find(void)
{
    struct audiobook_chapters *ab = fresh(16, 2000);
    static const uint32_t st[] = { 5000, 0, 3000, 3000, 1000 };
    static const char *const ti[] = { "e", "a", "c1", "c2", "b" };
    int i;

    ab_ch_expect(ab, 5);
    for (i = 0; i < 5; i++)
        ab_ch_put(ab, i, st[i], ti[i], (int)strlen(ti[i]));
    ab->chapter_count = 5;
    ab_ch_sort(ab);
    CHECK(!strcmp(ab_ch_title_ptr(ab, 0), "a"));
    CHECK(!strcmp(ab_ch_title_ptr(ab, 1), "b"));
    CHECK(!strcmp(ab_ch_title_ptr(ab, 2), "c1"));   /* stable */
    CHECK(!strcmp(ab_ch_title_ptr(ab, 3), "c2"));
    CHECK(!strcmp(ab_ch_title_ptr(ab, 4), "e"));
    CHECK_EQ(ab_ch_find(ab, 0), 0);
    CHECK_EQ(ab_ch_find(ab, 999), 0);
    CHECK_EQ(ab_ch_find(ab, 1000), 1);
    CHECK_EQ(ab_ch_find(ab, 3000), 3);              /* last of equal */
    CHECK_EQ(ab_ch_find(ab, 99999), 4);
    ab->chapter_count = 0;
    CHECK_EQ(ab_ch_find(ab, 5), -1);
}

/* pool exhaustion: fair share = left / entries left, cut on UTF-8,
 * counted in cut, the flag set; never past the pool */
static void test_pool(void)
{
    /* usable = pool - 64 = 41 bytes for 4 titles of 30 B */
    struct audiobook_chapters *ab = fresh(4, 105);
    const char *t = "\xc3\xa9" "bcdefghijklmnopqrstuvwxyz0123";  /* 30 B */
    int i, len[4];

    ab_ch_expect(ab, 4);
    for (i = 0; i < 4; i++)
        len[i] = ab_ch_put(ab, i, i * 10, t, 30);
    ab->chapter_count = 4;
    /* left 40 (41 - pool[0]... usable 41, used 1 -> 40): 40/4 = 10 B with
     * the NUL -> 9; then (40-10)/3 = 10 -> 9; 20/2 = 10 -> 9; 10 -> 9 */
    for (i = 0; i < 4; i++)
    {
        CHECK_EQ(len[i], 9);
        CHECK(ab_ch_ent(ab)[i].flags & AB_CH_F_CUT);
    }
    CHECK_EQ(ab->cut, 4);
    CHECK(ab->pool_used <= ab->pool_size - AB_MAX_CHAPTER_NAME);
    /* a cut never splits the 2-byte é at the start */
    {
        struct audiobook_chapters *b = fresh(3, 1 + 64 + 2);
        ab_ch_expect(b, 1);
        /* left 2 -> share 2 -> 1 byte would split é: empty instead */
        CHECK_EQ(ab_ch_put(b, 0, 0, t, 30), 0);
        CHECK_EQ(b->cut, 1);
        CHECK(!strcmp(ab_ch_title_ptr(b, 0), ""));
    }
    /* exhausted: later titles are empty (min 0), still counted */
    {
        struct audiobook_chapters *c = fresh(3, 1 + 64 + 4);
        ab_ch_expect(c, 3);
        CHECK_EQ(ab_ch_put(c, 0, 0, "abcdef", 6), 0);  /* 3/3 = 1: "" */
        CHECK_EQ(c->cut, 1);
    }
}

/* 1,024 entries at the default size: all stored, sorted from reversed */
static void test_1024(void)
{
    struct audiobook_chapters *ab = fresh(AB_MAX_CHAPTERS, AB_CH_POOL);
    char t[32];
    int i, ok = 1, n;

    ab_ch_expect(ab, AB_MAX_CHAPTERS);
    for (i = 0; i < AB_MAX_CHAPTERS; i++)
    {
        n = snprintf(t, sizeof(t), "Chapter %d", AB_MAX_CHAPTERS - i);
        ab_ch_put(ab, i, (uint32_t)(AB_MAX_CHAPTERS - 1 - i) * 1000, t, n);
    }
    ab->chapter_count = AB_MAX_CHAPTERS;
    CHECK_EQ(ab->cut, 0);
    ab_ch_sort(ab);
    for (i = 0; i < AB_MAX_CHAPTERS; i++)
    {
        snprintf(t, sizeof(t), "Chapter %d", i + 1);
        ok &= ab_ch_start(ab, i) == (uint32_t)i * 1000
              && !strcmp(ab_ch_title_ptr(ab, i), t);
    }
    CHECK(ok);
    CHECK_EQ(ab_ch_find(ab, 1023500), 1023);
    /* 63-byte titles x 1,024 overflow 24,576 B: shares, never past it */
    ab_ch_reset(ab);
    ab_ch_expect(ab, AB_MAX_CHAPTERS);
    memset(t, 'x', sizeof(t));
    for (i = 0; i < AB_MAX_CHAPTERS; i++)
    {
        char big[64];
        memset(big, 'y', 63);
        ab_ch_put(ab, i, i, big, 63);
    }
    CHECK(ab->cut > 0);
    CHECK(ab->pool_used <= AB_CH_POOL - AB_MAX_CHAPTER_NAME);
    printf("   1,024 x 63 B titles: %d cut, pool %u of %u B\n", ab->cut,
           (unsigned)ab->pool_used, (unsigned)AB_CH_POOL);
}

/* The parsers' sequences (M, P6 Rev 1 item 15):
 *  - mp4: a 'chpl' (cut at 255) is read, then the longer text track wins:
 *    ab_ch_reset() gives the whole pool back, so 400 titles of 60 B (24,400
 *    B incl. NULs <= 24,511 usable) are all stored uncut;
 *  - ID3 with the CHAP count announced: 450 titles of 60 B UTF-8 (6 ASCII
 *    + 27 x 2-byte é) all get the same share (>= 52 B: 24,511 / 450 = 54
 *    incl. the NUL, cut back to a character boundary), none empty;
 *  - an underestimated count: entries past it take what is left
 *    (first-come), never past the pool. */
static int valid_utf8_cut(const char *s, int len)
{
    /* no partial character at the end */
    int i = len;
    while (i > 0 && ((uint8_t)s[i - 1] & 0xc0) == 0x80)
        i--;
    if (i == 0)
        return len == 0;
    uint8_t c = (uint8_t)s[i - 1];
    int need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    return len - (i - 1) == need;
}

static void test_parser_sequences(void)
{
    struct audiobook_chapters *ab = fresh(AB_MAX_CHAPTERS, AB_CH_POOL);
    char t[64];
    int i, ok, minlen;

    /* mp4: chpl first ... */
    memset(t, 'x', 60);
    ab_ch_expect(ab, 255);
    for (i = 0; i < 255; i++)
        ab_ch_put(ab, i, (uint32_t)i * 2, t, 60);
    CHECK_EQ(ab->cut, 0);
    CHECK_EQ(ab->pool_used, 1 + 255 * 61);
    /* ... then the text track replaces it (mp4_read_chapters) */
    ab_ch_reset(ab);
    CHECK_EQ(ab->pool_used, 1);
    ab_ch_expect(ab, 400);
    for (i = 0; i < 400; i++)
        ab_ch_put(ab, i, (uint32_t)i * 2, t, 60);
    ab->chapter_count = 400;
    CHECK_EQ(ab->cut, 0);
    CHECK_EQ(ab->pool_used, 1 + 400 * 61);
    CHECK_EQ(strlen(ab_ch_title_ptr(ab, 399)), 60);

    /* ID3 with the count */
    ab_ch_reset(ab);
    ab_ch_expect(ab, 450);
    memcpy(t, "C001 ", 5);
    for (i = 5; i < 59; i += 2)
    {
        t[i] = (char)0xc3;
        t[i + 1] = (char)0xa9;
    }
    for (i = 0; i < 450; i++)
        ab_ch_put(ab, i, (uint32_t)i * 2, t, 59);   /* 5 + 54 B */
    ab->chapter_count = 450;
    CHECK_EQ(ab->cut, 450);
    ok = 1;
    minlen = 64;
    for (i = 0; i < 450; i++)
    {
        const char *s = ab_ch_title_ptr(ab, i);
        int len = (int)strlen(s);
        ok &= valid_utf8_cut(s, len) && len == ab_ch_ent(ab)[i].title_len;
        if (len < minlen)
            minlen = len;
    }
    CHECK(ok);
    CHECK(minlen >= 52);
    CHECK(ab->pool_used <= AB_CH_POOL - AB_MAX_CHAPTER_NAME);

    /* an underestimated count: 10 announced, 40 come */
    ab = fresh(64, 64 + 1 + 400);                   /* 400 usable after "" */
    ab_ch_expect(ab, 10);
    memset(t, 'z', 60);
    for (i = 0; i < 40; i++)
        ab_ch_put(ab, i, (uint32_t)i, t, 30);
    CHECK(ab->pool_used <= ab->pool_size - AB_MAX_CHAPTER_NAME);
    CHECK_EQ(strlen(ab_ch_title_ptr(ab, 0)), 30);   /* 400/10 = 40 >= 31 */
    CHECK_EQ(strlen(ab_ch_title_ptr(ab, 39)), 0);   /* nothing left */
}

int main(void)
{
    test_layout();
    test_put_title();
    test_sort_find();
    test_pool();
    test_1024();
    test_parser_sequences();
    CHECK_DONE("test_abpack (packed)");
}

#else /* stock: the accessors are the old array */

static struct audiobook_chapters ab;

int main(void)
{
    char *t;

    CHECK_EQ(AB_MAX_CHAPTERS, 255);
    CHECK_EQ(AB_CH_TABLE_SIZE, sizeof(struct audiobook_chapters));
    CHECK_EQ(ab_ch_cap(&ab), 255);
    ab.chapter_count = 0;
    ab.curr_chapter_idx = 0;
    ab_ch_reset(&ab);                       /* nothing */
    ab_ch_expect(&ab, 3);                   /* nothing */
    ab_ch_set_start(&ab, 2, 4242);
    CHECK_EQ(ab.chapters[2].start_ms, 4242);
    CHECK_EQ(ab_ch_start(&ab, 2), 4242);
    t = ab_ch_title_buf(&ab, 2);
    CHECK(t == ab.chapters[2].title);
    strcpy(t, "Title");
    ab_ch_commit(&ab, 2);                   /* nothing */
    CHECK(ab_ch_title_ptr(&ab, 2) == ab.chapters[2].title);
    CHECK(!strcmp(ab_ch_title_ptr(&ab, 2), "Title"));
    CHECK(&ab_ch_start(&ab, 1) == &ab.chapters[1].start_ms);
    CHECK_DONE("test_abpack (stock)");
}

#endif
