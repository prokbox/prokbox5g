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
/* Phase 6 pure book rules (ab_book.h): the books.pak blob, the book map
 * maths, the counter source, ticks, finished and the speed factor. No
 * Rockbox headers, no statics beyond const tables, no allocation; host
 * tests: tests/abbook/test_abbook.c. */

#include <string.h>
#include "ab_book.h"
#include "np_layout.h"          /* NP_BAR_W, struct np_book_pos */

#define AB_SA2(c, l) typedef char ab_sa_##l[(c) ? 1 : -1] __attribute__((unused))
#define AB_SA1(c, l) AB_SA2(c, l)
#define AB_SA(c) AB_SA1(c, __LINE__)

AB_SA(sizeof(struct ab_idx_hdr) == 32);
AB_SA(sizeof(struct ab_idx_rec) == 24);
AB_SA(sizeof(struct ab_blob_hdr) == 24);
AB_SA(sizeof(struct ab_book_block) == AB_BOOK_BLOCK_SIZE);
AB_SA(AB_TICK_BYTES * 8 >= AB_TICK_RES && AB_TICK_BYTES % 4 == 0);
AB_SA(AB_MAP_FILES_MAX <= 65535);
AB_SA(AB_TICK_RES == NP_BAR_W && AB_TICK_BYTES == NP_BOOK_TICK_BYTES);

/* ---- blob ---------------------------------------------------------------- */

uint32_t ab_word_sum(const void *p, size_t n)
{
    const uint32_t *w = p;
    uint32_t s = 0x41425355u;           /* "USBA": an empty blob is not 0 */
    size_t i;

    for (i = 0; i < n / 4; i++)
        s += w[i] ^ (uint32_t)i;
    return s;
}

uint32_t ab_path_crc(const char *path)
{
    static const uint32_t nib[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    uint32_t c = 0xffffffffu;

    while (path && *path)
    {
        c ^= (uint8_t)*path++;
        c = (c >> 4) ^ nib[c & 15];
        c = (c >> 4) ^ nib[c & 15];
    }
    return ~c;
}

bool ab_blob_parse(const void *blob, size_t size, struct ab_blob_view *v)
{
    const uint8_t *b = blob;
    const struct ab_blob_hdr *h = blob;
    const struct audiobook_chapters *tab;
    size_t map, off;

    memset(v, 0, sizeof(*v));
    if (!blob || size < sizeof(*h) || (size & 7) || ((uintptr_t)blob & 7))
        return false;
    if (h->file_count == 0 || h->file_count > AB_MAP_FILES_MAX)
        return false;
    if (ab_word_sum(b + sizeof(*h), size - sizeof(*h)) != h->sum)
        return false;
    map = AB_BLOB_MAP_SIZE(h->file_count);
    off = sizeof(*h) + map;
    if (size < off + AB_CH_HDR_SIZE)
        return false;
    tab = (const struct audiobook_chapters *)(b + off);
    if (tab->chapter_count < 1 || tab->chapter_count > tab->cap
        || tab->pool_used > tab->pool_size || tab->pool_used < 1
        || size < off + AB_CH_SIZE(tab->cap, tab->pool_size))
        return false;
    v->hdr = h;
    v->file_start_ms = (const uint32_t *)(b + sizeof(*h));
    v->file_crc = v->file_start_ms + h->file_count;
    v->file_first = (const uint16_t *)(v->file_crc + h->file_count);
    v->tab = tab;
    return true;
}

size_t ab_blob_build(void *blob, size_t size, ab_key_t key,
                     uint32_t max_mtime, uint32_t len_ms, int file_count,
                     const uint32_t *file_start_ms, const uint32_t *file_crc,
                     const uint16_t *file_first,
                     const struct audiobook_chapters *tab)
{
    uint8_t *b = blob;
    struct ab_blob_hdr *h = blob;
    struct audiobook_chapters *out;
    int n = tab->chapter_count, i, files = 0;
    size_t map, off, pool, total;

    if (file_count < 1 || file_count > AB_MAP_FILES_MAX || n < 1
        || ((uintptr_t)blob & 7))
        return 0;
    map = AB_BLOB_MAP_SIZE(file_count);
    off = sizeof(*h) + map;
    pool = tab->pool_used;                  /* padded: size 8-aligned */
    pool += (8 - (off + AB_CH_SIZE(n, pool)) % 8) % 8;
    total = off + AB_CH_SIZE(n, pool);
    if (total > size || pool > 65535)
        return 0;

    /* The map is copied first and nothing is cleared ahead of it: the
     * books builder keeps the map arrays in this same block, behind where
     * the map lands (artcache.c bk_layout(), fix round L1), so they may be
     * overwritten only by the table that follows. Every byte up to
     * 'total' is still written (the pads are cleared one by one). */
    memset(h, 0, sizeof(*h));
    h->book_key = key;
    h->max_mtime = max_mtime;
    h->file_count = (uint16_t)file_count;
    h->len_ms = len_ms;
    memmove(b + sizeof(*h), file_start_ms, file_count * 4);
    memmove(b + sizeof(*h) + file_count * 4, file_crc, file_count * 4);
    memmove(b + sizeof(*h) + file_count * 8, file_first, file_count * 2);
    memset(b + sizeof(*h) + file_count * 10, 0,
           map - (size_t)file_count * 10);

    out = (struct audiobook_chapters *)(b + off);
    *out = *tab;
    out->cap = (uint16_t)n;
    out->pool_size = (uint16_t)pool;
    out->curr_chapter_idx = 0;
    out->expect = 0;
    memcpy(ab_ch_ent(out), ab_ch_ent(tab), n * sizeof(struct ab_ch_entry));
    memcpy(ab_ch_pool(out), ab_ch_pool(tab), tab->pool_used);
    memset(ab_ch_pool(out) + tab->pool_used, 0, pool - tab->pool_used);
    for (i = 0; i < n; i++)
        files += (ab_ch_ent(out)[i].flags & AB_CH_F_FILE) != 0;
    h->flags = files == n ? AB_BLOB_F_FILES : 0;
    h->sum = ab_word_sum(b + sizeof(*h), total - sizeof(*h));
    return total;
}

int ab_idx_find(const struct ab_idx_rec *recs, int n, ab_key_t key)
{
    int lo = 0, hi = n - 1;

    while (lo <= hi)
    {
        int mid = lo + (hi - lo) / 2;
        if (recs[mid].book_key == key)
            return mid;
        if (recs[mid].book_key < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/* ---- the map ------------------------------------------------------------- */

uint32_t ab_book_pos(const struct ab_book_map *m, int q, uint32_t elapsed_ms)
{
    uint32_t pos;

    if (m->file_count == 0)
        return elapsed_ms;
    if (q < 0)
        q = 0;
    if (q >= m->file_count)
        q = m->file_count - 1;
    pos = m->file_start_ms[q] + elapsed_ms;
    if (m->len_ms && pos > m->len_ms)
        pos = m->len_ms;
    return pos;
}

int ab_book_file_at(const struct ab_book_map *m, uint32_t book_ms,
                    uint32_t *file_ms)
{
    int lo = 0, hi = m->file_count - 1;

    if (hi < 0)
        return -1;
    while (lo < hi)
    {
        int mid = lo + (hi - lo + 1) / 2;
        if (m->file_start_ms[mid] <= book_ms)
            lo = mid;
        else
            hi = mid - 1;
    }
    if (file_ms)
        *file_ms = book_ms >= m->file_start_ms[lo]
                   ? book_ms - m->file_start_ms[lo] : 0;
    return lo;
}

int ab_book_entry_at(const struct ab_book_map *m, int q, int c)
{
    int first, next;

    if (q < 0 || q >= m->file_count)
        return -1;
    first = m->file_first[q];
    next = q + 1 < m->file_count ? m->file_first[q + 1] : m->entries;
    if (next <= first)
        return first;
    if (c < 0)
        c = 0;
    if (first + c >= next)
        return next - 1;
    return first + c;
}

int ab_book_entry(const struct audiobook_chapters *tab, uint32_t book_ms,
                  char *buf, int size)
{
    int e = ab_ch_find(tab, book_ms);

    if (size > 0)
        buf[0] = '\0';
    if (e >= 0)
        ab_ch_title(tab, e, buf, size);
    return e;
}

int ab_book_ticks(const uint32_t *starts, size_t stride, int count,
                  uint32_t len_ms, uint8_t bits[AB_TICK_BYTES])
{
    int i, n = 0;

    memset(bits, 0, AB_TICK_BYTES);
    if (!len_ms)
        return 0;
    for (i = 0; i < count; i++)
    {
        uint32_t s = *(const uint32_t *)((const char *)starts + i * stride);
        uint32_t x;

        if (s == 0 || s >= len_ms)
            continue;
        x = (uint32_t)((uint64_t)s * AB_TICK_RES / len_ms);
        if (x == 0 || x >= AB_TICK_RES || (bits[x >> 3] & (1 << (x & 7))))
            continue;
        bits[x >> 3] |= 1 << (x & 7);
        n++;
    }
    return n;
}

int ab_ticks_scale(const uint8_t src[AB_TICK_BYTES], int w, uint8_t *dst)
{
    int b, n = 0;

    if (w <= 0)
        return 0;
    if (w > AB_TICK_RES)
        w = AB_TICK_RES;
    memset(dst, 0, (w + 7) / 8);
    for (b = 1; b < AB_TICK_RES; b++)
    {
        int x;

        if (!(src[b >> 3] & (1 << (b & 7))))
            continue;
        x = b * w / AB_TICK_RES;
        if (x <= 0 || x >= w || (dst[x >> 3] & (1 << (x & 7))))
            continue;
        dst[x >> 3] |= 1 << (x & 7);
        n++;
    }
    return n;
}

/* ---- rules ----------------------------------------------------------------- */

enum ab_counter_src ab_counter_source(bool book_queue, bool map_ok,
                                      bool is_audiobook, bool is_podcast,
                                      int file_chapters, int queue_count)
{
    if (queue_count <= 0)
        return AB_CNT_NONE;
    if (is_podcast)
        return AB_CNT_QUEUE;            /* even with chapters (2026-10-07) */
    if (book_queue && map_ok)
        return AB_CNT_BOOK;
    if (is_audiobook && file_chapters > 0)
        return AB_CNT_FILE;
    return AB_CNT_QUEUE;
}

bool ab_finished(uint32_t book_ms, uint32_t len_ms)
{
    if (len_ms == 0)
        return false;
    if (len_ms > AB_FINISH_MS)
        return book_ms >= len_ms - AB_FINISH_MS;
    return book_ms >= len_ms;
}

long ab_speed_factor(int speed)
{
    static const long f[AB_SPEED_COUNT] = { 7500, 10000, 12500 };

    return f[speed >= 0 && speed < AB_SPEED_COUNT ? speed : AB_SPEED_NORMAL];
}

long ab_speed_for_track(int speed, bool is_audiobook, bool is_podcast)
{
    return is_audiobook && !is_podcast ? ab_speed_factor(speed)
                                       : ab_speed_factor(AB_SPEED_NORMAL);
}
