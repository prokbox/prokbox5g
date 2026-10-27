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

/* The packed chapter table (chapters.h, P6 §3.1). Pure C: no Rockbox
 * headers beyond chapters.h, no statics, no allocation, so the parsers,
 * the art builder, the shell and the host tests (tests/abbook) share it.
 * Empty in the stock build (HAVE_METADATA_EXTRAS off), so stock is
 * byte-identical. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "chapters.h"

#ifdef HAVE_METADATA_EXTRAS

void ab_ch_init(struct audiobook_chapters *ab, int cap, int pool)
{
    ab->cap = (uint16_t)cap;
    ab->pool_size = (uint16_t)pool;
    ab->spare = 0;
    ab_ch_reset(ab);
}

void ab_ch_reset(struct audiobook_chapters *ab)
{
    ab->chapter_count = 0;
    ab->curr_chapter_idx = 0;
    ab->cut = 0;
    ab->expect = 0;
    ab->pool_used = 1;
    ab_ch_pool(ab)[0] = '\0';
}

void ab_ch_expect(struct audiobook_chapters *ab, int n)
{
    ab->expect = (uint16_t)(n < 0 ? 0 : n > ab->cap ? ab->cap : n);
}

/* largest n' <= n that does not split the UTF-8 character at s[n] */
static int utf8_cut(const char *s, int n)
{
    while (n > 0 && ((uint8_t)s[n] & 0xc0) == 0x80)
        n--;
    return n;
}

int ab_ch_commit(struct audiobook_chapters *ab, int i)
{
    struct ab_ch_entry *e = &ab_ch_ent(ab)[i];
    char *buf = ab_ch_pool(ab) + ab->pool_used;
    int usable = (int)ab->pool_size - AB_MAX_CHAPTER_NAME;
    int left = usable - (int)ab->pool_used;
    int len = 0, share, entries_left;

    while (len < AB_MAX_CHAPTER_NAME - 1 && buf[len])
        len++;

    /* fair share of what is left, incl. the NUL (Rev 1 item 15);
     * without ab_ch_expect() (or past it) the share is everything left */
    entries_left = ab->expect > i ? ab->expect - i : 1;
    share = left > 0 ? left / entries_left : 0;

    e->flags = 0;
    if (len > 0 && len + 1 > share)
    {
        len = share > 1 ? utf8_cut(buf, share - 1) : 0;
        e->flags = AB_CH_F_CUT;
        ab->cut++;
    }
    if (len == 0)
    {
        e->title_off = 0;           /* pool[0] = "" */
        e->title_len = 0;
        return 0;
    }
    buf[len] = '\0';
    e->title_off = ab->pool_used;
    e->title_len = (uint8_t)len;
    ab->pool_used += (uint16_t)(len + 1);
    return len;
}

int ab_ch_put(struct audiobook_chapters *ab, int i, uint32_t start_ms,
              const char *title, int len)
{
    char *buf = ab_ch_title_buf(ab, i);

    ab_ch_set_start(ab, i, start_ms);
    if (!title || len < 0)
        len = 0;
    if (len > AB_MAX_CHAPTER_NAME - 1)
        len = utf8_cut(title, AB_MAX_CHAPTER_NAME - 1);
    memcpy(buf, title, len);
    buf[len] = '\0';
    return ab_ch_commit(ab, i);
}

void ab_ch_sort(struct audiobook_chapters *ab)
{
    struct ab_ch_entry *e = ab_ch_ent(ab);
    int n = ab->chapter_count, i, j;

    for (i = 1; i < n; i++)
    {
        struct ab_ch_entry tmp;

        if (e[i].start_ms >= e[i - 1].start_ms)
            continue;
        tmp = e[i];
        for (j = i; j > 0 && e[j - 1].start_ms > tmp.start_ms; j--)
            ;
        memmove(&e[j + 1], &e[j], (size_t)(i - j) * sizeof(e[0]));
        e[j] = tmp;
    }
}

int ab_ch_find(const struct audiobook_chapters *ab, uint32_t ms)
{
    const struct ab_ch_entry *e = ab_ch_ent(ab);
    int lo = 0, hi = ab->chapter_count - 1;

    if (hi < 0)
        return -1;
    /* last index with start <= ms */
    while (lo < hi)
    {
        int mid = lo + (hi - lo + 1) / 2;
        if (e[mid].start_ms <= ms)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

int ab_ch_title(const struct audiobook_chapters *ab, int i,
                char *buf, int size)
{
    const struct ab_ch_entry *e = &ab_ch_ent(ab)[i];
    int len = e->title_len;

    if (size <= 0)
        return 0;
    if (len > size - 1)
        len = utf8_cut(ab_ch_pool(ab) + e->title_off, size - 1);
    memcpy(buf, ab_ch_pool(ab) + e->title_off, len);
    buf[len] = '\0';
    return len;
}

#endif /* HAVE_METADATA_EXTRAS */
