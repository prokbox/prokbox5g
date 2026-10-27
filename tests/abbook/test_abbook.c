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
/* Round-trip test of the books.pak / books.idx records and the pure book
 * rules (ab_book.h, ab_book_core.c, with abpack.c): blob build -> parse,
 * the index lookup and validity, whole-book position across files, the
 * counter source (whole-book for multi-file books; podcasts keep the
 * queue), the counter is the ENTRY POSITION with the title from the same
 * entry (Preface / Introduction before "Chapter 1"), the files-as-entries
 * fallback, ticks and their rescale, "finished within 30 s", speed. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ab_book.h"
#include "check.h"

static uint32_t tab_mem[(AB_CH_TABLE_SIZE + 3) / 4];
static uint64_t blob_mem[(AB_BLOB_MAX_SIZE + 7) / 8];  /* 8-aligned */
static struct ab_book_map map;

static struct audiobook_chapters *table(void)
{
    struct audiobook_chapters *t = (struct audiobook_chapters *)tab_mem;
    ab_ch_init(t, AB_MAX_CHAPTERS, AB_CH_POOL);
    return t;
}

static void map_from(const struct ab_blob_view *v)
{
    int q;

    memset(&map, 0, sizeof(map));
    map.book_key = v->hdr->book_key;
    map.len_ms = v->hdr->len_ms;
    map.file_count = v->hdr->file_count;
    map.entries = (uint16_t)v->tab->chapter_count;
    map.src = AB_MAP_PAK;
    for (q = 0; q < map.file_count; q++)
    {
        map.file_start_ms[q] = v->file_start_ms[q];
        map.file_crc[q] = v->file_crc[q];
        map.file_first[q] = v->file_first[q];
    }
    ab_book_ticks(&ab_ch_start(v->tab, 0), sizeof(struct ab_ch_entry),
                  v->tab->chapter_count, map.len_ms, map.ticks);
}

/* Single file, chapters "Preface", "Introduction", "Chapter 1", ... */
static void test_single_file(void)
{
    struct audiobook_chapters *t = table();
    static const char *const ti[] = { "Preface", "Introduction",
                                      "Chapter 1", "Chapter 2", "" };
    uint32_t start = 0, crc = ab_path_crc("/Books/A/a.m4b");
    uint16_t first = 0;
    struct ab_blob_view v;
    char buf[64];
    size_t n;
    int i, e;

    ab_ch_expect(t, 5);
    for (i = 0; i < 5; i++)
        ab_ch_put(t, i, (uint32_t)i * 600000, ti[i], (int)strlen(ti[i]));
    t->chapter_count = 5;
    n = ab_blob_build(blob_mem, sizeof(blob_mem), 0xabcdefull, 1700000000,
                      3000000, 1, &start, &crc, &first, t);
    CHECK(n > 0 && n % 8 == 0);
    CHECK(ab_blob_parse(blob_mem, n, &v));
    CHECK_EQ(v.hdr->book_key, 0xabcdefull);
    CHECK_EQ(v.hdr->flags, 0);                  /* real chapters */
    CHECK_EQ(v.tab->chapter_count, 5);
    CHECK_EQ(v.tab->cap, 5);                    /* trimmed */
    CHECK(!strcmp(ab_ch_title_ptr(v.tab, 2), "Chapter 1"));
    /* a corrupted blob is refused */
    ((uint8_t *)blob_mem)[n - 1] ^= 1;
    CHECK(!ab_blob_parse(blob_mem, n, &v));
    ((uint8_t *)blob_mem)[n - 1] ^= 1;
    CHECK(ab_blob_parse(blob_mem, n, &v));
    CHECK(!ab_blob_parse(blob_mem, n - 4, &v));
    CHECK(ab_blob_parse(blob_mem, n, &v));

    map_from(&v);
    /* 20:00.5 into the book: entry 3 of 5, and its title is "Chapter 1"
     * (never parsed from the title: the counter is the position) */
    e = ab_book_entry(v.tab, 1200500, buf, sizeof(buf));
    CHECK_EQ(e + 1, 3);
    CHECK(!strcmp(buf, "Chapter 1"));
    CHECK_EQ(ab_book_entry_at(&map, 0, 2), 2);  /* via (file, chapter) */
    CHECK_EQ(map.entries, 5);
    /* untitled entry: "" (U draws "Chapter 5", the position) */
    e = ab_book_entry(v.tab, 2999999, buf, sizeof(buf));
    CHECK_EQ(e, 4);
    CHECK(!strcmp(buf, ""));
    /* single-file position = elapsed */
    CHECK_EQ(ab_book_pos(&map, 0, 1234), 1234);
    CHECK_EQ(ab_book_pos(&map, 0, 9999999), 3000000);   /* clamped */
}

/* Multi-file: file 0 has 3 chapters, file 1 none (an entry of its own),
 * file 2 has 2 chapters. Entries = 6; book-relative starts. */
static void test_multi_file(void)
{
    struct audiobook_chapters *t = table();
    uint32_t fs[3] = { 0, 1800000, 2400000 };
    uint32_t fc[3];
    uint16_t ff[3] = { 0, 3, 4 };
    struct ab_blob_view v;
    char buf[64];
    size_t n;
    int i = 0, q;
    uint32_t in;

    fc[0] = ab_path_crc("/B/01.mp3");
    fc[1] = ab_path_crc("/B/02.mp3");
    fc[2] = ab_path_crc("/B/03.mp3");
    CHECK(fc[0] != fc[1] && fc[1] != fc[2]);
    ab_ch_expect(t, 6);
    ab_ch_put(t, i++, 0, "Opening", 7);
    ab_ch_put(t, i++, 600000, "Part One", 8);
    ab_ch_put(t, i++, 1200000, "Part Two", 8);
    ab_ch_put(t, i, 1800000, "Track Two", 9);       /* the file itself */
    ab_ch_ent(t)[i++].flags |= AB_CH_F_FILE;
    ab_ch_put(t, i++, 2400000, "Part Three", 10);
    ab_ch_put(t, i++, 3000000, "Epilogue", 8);
    t->chapter_count = i;
    n = ab_blob_build(blob_mem, sizeof(blob_mem), 77, 5, 3600000, 3, fs, fc,
                      ff, t);
    CHECK(ab_blob_parse(blob_mem, n, &v));
    CHECK_EQ(v.hdr->flags, 0);
    map_from(&v);

    /* whole-book position across a file boundary */
    CHECK_EQ(ab_book_pos(&map, 1, 30000), 1830000);
    CHECK_EQ(ab_book_pos(&map, 2, 0), 2400000);
    q = ab_book_file_at(&map, 2399999, &in);
    CHECK_EQ(q, 1);
    CHECK_EQ(in, 599999);
    q = ab_book_file_at(&map, 2400000, &in);
    CHECK_EQ(q, 2);
    CHECK_EQ(in, 0);
    /* whole-book counter (Rev 2): file 1 (no chapters) is entry 4 of 6;
     * file 2 chapter 1 is entry 6 of 6; clamped to the file's entries */
    CHECK_EQ(ab_book_entry_at(&map, 1, 0) + 1, 4);
    CHECK_EQ(ab_book_entry_at(&map, 1, 7) + 1, 4);
    CHECK_EQ(ab_book_entry_at(&map, 2, 1) + 1, 6);
    CHECK_EQ(ab_book_entry_at(&map, 0, 2) + 1, 3);
    CHECK_EQ(ab_book_entry_at(&map, 3, 0), -1);
    CHECK_EQ(map.entries, 6);
    /* title from the same entry as the counter */
    CHECK_EQ(ab_book_entry(v.tab, ab_book_pos(&map, 1, 5000), buf,
                           sizeof(buf)), 3);
    CHECK(!strcmp(buf, "Track Two"));
    /* ticks: 5 starts inside (0, len) */
    CHECK_EQ(ab_book_ticks(&ab_ch_start(v.tab, 0), sizeof(struct ab_ch_entry),
                           v.tab->chapter_count, map.len_ms, map.ticks), 5);
    {
        uint8_t dst[AB_TICK_BYTES];
        CHECK_EQ(ab_ticks_scale(map.ticks, 154, dst), 5);
        CHECK_EQ(ab_ticks_scale(map.ticks, AB_TICK_RES, dst), 5);
        CHECK_EQ(ab_ticks_scale(map.ticks, 4, dst), 3);    /* merged */
    }
}

/* Files as entries (no file has chapters, or the pak missed: the RAM
 * database fallback builds the same shape): counter counts files. */
static void test_files_as_entries(void)
{
    struct audiobook_chapters *t = table();
    enum { F = 163 };
    static uint32_t fs[F], fc[F];
    static uint16_t ff[F];
    struct ab_blob_view v;
    char name[32];
    size_t n;
    int q;

    ab_ch_expect(t, F);
    for (q = 0; q < F; q++)
    {
        int len = snprintf(name, sizeof(name), "Track %d", q + 1);
        fs[q] = (uint32_t)q * 300000;
        snprintf(name + 16, 16, "/b/%03d.mp3", q % 1000);
        fc[q] = ab_path_crc(name + 16);
        ff[q] = (uint16_t)q;
        name[len] = '\0';
        ab_ch_put(t, q, fs[q], name, len);
        ab_ch_ent(t)[q].flags |= AB_CH_F_FILE;
    }
    t->chapter_count = F;
    n = ab_blob_build(blob_mem, sizeof(blob_mem), 5, 5, F * 300000u, F, fs,
                      fc, ff, t);
    CHECK(ab_blob_parse(blob_mem, n, &v));
    CHECK_EQ(v.hdr->flags, AB_BLOB_F_FILES);
    map_from(&v);
    CHECK_EQ(ab_book_entry_at(&map, 99, 0) + 1, 100);
    CHECK_EQ(map.entries, F);
    /* 1,024 files fit; 1,025 do not (⚠️8) */
    CHECK_EQ(ab_blob_build(blob_mem, sizeof(blob_mem), 5, 5, 1, 1025, fs, fc,
                           ff, t), 0);
}

static void test_index(void)
{
    struct ab_idx_rec r[4];
    int i;

    memset(r, 0, sizeof(r));
    for (i = 0; i < 4; i++)
    {
        r[i].book_key = (ab_key_t)(i + 1) << 40;
        r[i].max_mtime = 100 + i;
        r[i].file_count = (uint16_t)(i + 1);
    }
    CHECK_EQ(ab_idx_find(r, 4, (ab_key_t)3 << 40), 2);
    CHECK_EQ(ab_idx_find(r, 4, 5), -1);
    CHECK_EQ(ab_idx_find(r, 0, 5), -1);
    CHECK(ab_idx_valid(&r[2], 102, 3));
    CHECK(!ab_idx_valid(&r[2], 103, 3));        /* a file changed */
    CHECK(!ab_idx_valid(&r[2], 102, 4));        /* a file added */
    CHECK_EQ(sizeof(struct ab_idx_rec), 24);
    CHECK_EQ(sizeof(struct ab_idx_hdr), 32);
    CHECK_EQ(sizeof(struct ab_blob_hdr), 24);
    CHECK_EQ(AB_BLOB_MAX_SIZE, 43048);
    CHECK_EQ(sizeof(struct ab_book_block), AB_BOOK_BLOCK_SIZE);
}

static void test_rules(void)
{
    /* counter source */
    CHECK_EQ(ab_counter_source(true, true, true, false, 0, 12), AB_CNT_BOOK);
    CHECK_EQ(ab_counter_source(true, true, true, false, 30, 1), AB_CNT_BOOK);
    CHECK_EQ(ab_counter_source(true, false, true, false, 30, 1), AB_CNT_FILE);
    CHECK_EQ(ab_counter_source(true, false, true, false, 0, 9), AB_CNT_QUEUE);
    CHECK_EQ(ab_counter_source(false, false, true, false, 30, 40), AB_CNT_FILE);
    CHECK_EQ(ab_counter_source(true, true, true, true, 30, 5), AB_CNT_QUEUE);
    CHECK_EQ(ab_counter_source(false, false, false, false, 0, 5), AB_CNT_QUEUE);
    CHECK_EQ(ab_counter_source(true, true, true, false, 3, 0), AB_CNT_NONE);
    /* finished: within 30 s of the end (Rev 2) */
    CHECK(ab_finished(3570000, 3600000));
    CHECK(!ab_finished(3569999, 3600000));
    CHECK(ab_finished(3600000, 3600000));
    CHECK(!ab_finished(0, 20000));              /* a 20 s book: only at */
    CHECK(ab_finished(20000, 20000));           /* its very end */
    CHECK(!ab_finished(5000, 0));               /* unknown length */
    /* speed (AB-2 confirmed) */
    CHECK_EQ(ab_speed_factor(AB_SPEED_SLOWER), 7500);
    CHECK_EQ(ab_speed_factor(AB_SPEED_NORMAL), 10000);
    CHECK_EQ(ab_speed_factor(AB_SPEED_FASTER), 12500);
    CHECK_EQ(ab_speed_factor(7), 10000);
    CHECK_EQ(ab_speed_for_track(AB_SPEED_FASTER, true, false), 12500);
    CHECK_EQ(ab_speed_for_track(AB_SPEED_FASTER, true, true), 10000);
    CHECK_EQ(ab_speed_for_track(AB_SPEED_SLOWER, false, false), 10000);
    /* path CRC: CRC-32 reference value */
    CHECK_EQ(ab_path_crc("123456789"), 0xcbf43926u);
    CHECK_EQ(ab_path_crc(""), 0);
}

int main(void)
{
    test_single_file();
    test_multi_file();
    test_files_as_entries();
    test_index();
    test_rules();
    CHECK_DONE("test_abbook");
}
