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
/* Phase 6 pure resume-store rules (ab_store.h): record check and state,
 * the double header, the sector index, search and the eviction order.
 * No Rockbox headers, no statics, no allocation; host tests:
 * tests/abbook/test_abstore.c. */

#include <string.h>
#include "ab_store.h"

#define AB_SA2(c, l) typedef char ab_sa_##l[(c) ? 1 : -1] __attribute__((unused))
#define AB_SA1(c, l) AB_SA2(c, l)
#define AB_SA(c) AB_SA1(c, __LINE__)

AB_SA(sizeof(struct ab_rec) == 32);
AB_SA(sizeof(struct ab_store_hdr) == 32);
AB_SA(AB_STORE_PER_SECT * sizeof(struct ab_rec) == AB_STORE_SECTOR);
AB_SA(AB_STORE_MAX / AB_STORE_PER_SECT == AB_STORE_SECTORS_MAX);

/* ---- records -------------------------------------------------------------- */

uint8_t ab_rec_check(const struct ab_rec *r)
{
    struct ab_rec t = *r;
    const uint8_t *p = (const uint8_t *)&t;
    uint8_t c = 0xff;
    size_t i;
    int k;

    t.flags &= AB_REC_STATE_MASK;       /* the check byte itself */
    for (i = 0; i < sizeof(t); i++)     /* CRC-8, polynomial 0x07 */
    {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c ? c : 0x5a;                /* never 0: zeroed slots fail */
}

void ab_rec_seal(struct ab_rec *r)
{
    r->flags = (uint16_t)((r->flags & AB_REC_STATE_MASK)
                          | (ab_rec_check(r) << 8));
}

bool ab_rec_ok(const struct ab_rec *r)
{
    return r->book_key != 0 && (r->flags >> 8) == ab_rec_check(r);
}

enum ab_rec_state ab_rec_state(const struct ab_rec *r)
{
    if (!ab_rec_ok(r))
        return AB_REC_NONE;
    if (r->flags & AB_REC_F_FINISHED)
        return AB_REC_FINISHED;
    if (r->flags & AB_REC_F_STARTED)
        return AB_REC_STARTED;
    return AB_REC_NONE;
}

void ab_rec_set(struct ab_rec *r, ab_key_t key, uint32_t file_crc,
                int file_idx, uint32_t elapsed_ms, uint32_t offset,
                uint32_t book_ms, uint32_t len_ms, bool stopping,
                uint32_t stamp)
{
    memset(r, 0, sizeof(*r));
    r->book_key = key;
    r->stamp = stamp;
    if (stopping && ab_finished(book_ms, len_ms))
        r->flags = AB_REC_F_FINISHED;   /* Rev 2: no resume spot */
    else
    {
        r->flags = AB_REC_F_STARTED;
        r->file_crc = file_crc;
        r->file_idx = (uint16_t)file_idx;
        r->elapsed_ms = elapsed_ms;
        r->offset = offset;
        r->book_ms = book_ms;
    }
    ab_rec_seal(r);
}

/* ---- header ---------------------------------------------------------------- */

void ab_store_hdr_seal(struct ab_store_hdr *h)
{
    h->magic = AB_STORE_MAGIC;
    h->version = AB_STORE_VERSION;
    h->rec_size = sizeof(struct ab_rec);
    h->sum = ab_word_sum(h, offsetof(struct ab_store_hdr, sum));
}

bool ab_store_hdr_ok(const struct ab_store_hdr *h)
{
    return h && h->magic == AB_STORE_MAGIC
        && h->version == AB_STORE_VERSION
        && h->rec_size == sizeof(struct ab_rec)
        && h->count <= AB_STORE_MAX
        && h->sectors == (uint32_t)ab_store_sectors(h->count)
        && h->sum == ab_word_sum(h, offsetof(struct ab_store_hdr, sum));
}

const struct ab_store_hdr *ab_store_hdr_pick(const struct ab_store_hdr *a,
                                             const struct ab_store_hdr *b)
{
    bool oa = ab_store_hdr_ok(a), ob = ab_store_hdr_ok(b);

    if (oa && ob)
        return (int32_t)(b->seq - a->seq) > 0 ? b : a;
    return oa ? a : ob ? b : NULL;
}

/* ---- index and search -------------------------------------------------------- */

int ab_store_sector_for(const struct ab_store_index *ix, ab_key_t key)
{
    int lo = 0, hi = (int)ix->sectors - 1;

    if (hi < 0)
        return -1;
    while (lo < hi)
    {
        int mid = lo + (hi - lo + 1) / 2;
        if (ix->first_key[mid] <= key)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

void ab_store_index_add(struct ab_store_index *ix, int s,
                        const struct ab_rec *recs, int n)
{
    int i;

    if (s < 0 || s >= AB_STORE_SECTORS_MAX || n <= 0)
        return;
    ix->first_key[s] = recs[0].book_key;
    if ((uint32_t)s + 1 > ix->sectors)
        ix->sectors = s + 1;
    for (i = 0; i < n; i++)
    {
        if (recs[i].book_key == 0)
            continue;
        ix->count++;
        if (ab_rec_ok(&recs[i]) && recs[i].stamp + 1 > ix->clock)
            ix->clock = recs[i].stamp + 1;
    }
}

int ab_store_rec_find(const struct ab_rec *recs, int n, ab_key_t key)
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
    return -lo - 1;
}

int ab_store_evict_pick(const struct ab_rec *recs, int n)
{
    /* class: 0 = bad check (lost anyway), 1 = finished, 2 = started */
    int i, best = -1, best_cls = 3;
    uint32_t best_stamp = 0;

    for (i = 0; i < n; i++)
    {
        enum ab_rec_state st = ab_rec_state(&recs[i]);
        int cls = st == AB_REC_FINISHED ? 1 : st == AB_REC_STARTED ? 2 : 0;

        if (cls < best_cls || (cls == best_cls && recs[i].stamp < best_stamp))
        {
            best = i;
            best_cls = cls;
            best_stamp = recs[i].stamp;
        }
    }
    return best;
}
