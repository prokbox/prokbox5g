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
 * iPod UI shell: library shuffle resume record (#27, Phase 5)
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

/* Pure C (host-tested by tests/nowplaying/test_shuffle_rec.c). */

#include <stddef.h>
#include <string.h>
#include "lib_shuffle.h"
#include "libfile.h"        /* libfile_sum_*: the same word checksum */

typedef char lib_shuf_rec_size_check[
    sizeof(struct lib_shuffle_rec) == LIB_SHUF_REC_SIZE
    && offsetof(struct lib_shuffle_rec, crc) == LIB_SHUF_REC_SIZE - 4
    && sizeof(struct shuf_songs) <= 44
    && sizeof(struct shuf_albums) <= 44 ? 1 : -1];

uint32_t lib_shuf_rec_sum(const struct lib_shuffle_rec *r)
{
    struct libfile_sum_state s;

    libfile_sum_init(&s);
    libfile_sum_add(&s, r, offsetof(struct lib_shuffle_rec, crc));
    return libfile_sum_end(&s);
}

void lib_shuf_rec_seal(struct lib_shuffle_rec *r)
{
    r->magic = LIB_SHUF_MAGIC;
    r->version = LIB_SHUF_VERSION;
    r->crc = lib_shuf_rec_sum(r);
}

static bool stamp_eq(const struct lib_shuf_stamp *a,
                     const struct lib_shuf_stamp *b, bool albums)
{
    return a->commitid == b->commitid && a->entries == b->entries
        && a->sortkey_version == b->sortkey_version
        && (!albums || a->albums_sum == b->albums_sum);
}

enum lib_shuf_check lib_shuf_rec_check(const struct lib_shuffle_rec *r,
                                       size_t len,
                                       const struct lib_shuf_stamp *now,
                                       uint32_t queued_now)
{
    if (len < sizeof(*r) || r->magic != LIB_SHUF_MAGIC
        || r->version != LIB_SHUF_VERSION || r->crc != lib_shuf_rec_sum(r)
        || r->source > LIB_SHUF_SRC_ALBUMS || r->mode > NP_SHUFFLE_ALBUMS)
        return LIB_SHUF_BAD;
    if (r->source == LIB_SHUF_SRC_NONE)
        return LIB_SHUF_RESUME;
    if (!stamp_eq(&r->stamp, now, r->source == LIB_SHUF_SRC_ALBUMS)
        || r->queued != queued_now)
        return LIB_SHUF_RESTART;
    return LIB_SHUF_RESUME;
}

void lib_shuf_rec_restart(struct lib_shuffle_rec *r, uint32_t n,
                          const struct lib_shuf_stamp *now)
{
    if (r->source == LIB_SHUF_SRC_ALBUMS)
    {
        uint32_t seed = shuf_lap_seed(r->it.a.seed, r->it.a.lap + 1);
        shuf_albums_init(&r->it.a, n, seed);
    }
    else if (r->source == LIB_SHUF_SRC_SONGS)
    {
        uint32_t seed = shuf_lap_seed(r->it.s.seed, r->it.s.lap + 1);
        shuf_songs_init(&r->it.s, n, seed, SHUF_NONE);
    }
    r->stamp = *now;
    r->flags |= LIB_SHUF_F_LAP;     /* the counter starts again at 1 */
}

bool lib_shuf_rec_upgrade(struct lib_shuffle_rec *r, size_t len)
{
    struct libfile_sum_state s;
    uint32_t crc;

    if (len != LIB_SHUF_V1_SIZE || r->magic != LIB_SHUF_MAGIC
        || r->version != 1)
        return false;
    /* version 1: the same fields up to 'count', then its crc there */
    memcpy(&crc, (const char *)r + LIB_SHUF_V1_SIZE - 4, sizeof(crc));
    libfile_sum_init(&s);
    libfile_sum_add(&s, r, LIB_SHUF_V1_SIZE - 4);
    if (libfile_sum_end(&s) != crc)
        return false;
    memset(&r->count, 0, sizeof(r->count));
    lib_shuf_rec_seal(r);
    return true;
}

/* ---- the counter ------------------------------------------------------- */

uint32_t lib_shuf_rec_lap(const struct lib_shuffle_rec *r)
{
    return r->source == LIB_SHUF_SRC_ALBUMS ? r->it.a.lap
         : r->source == LIB_SHUF_SRC_SONGS ? r->it.s.lap : 0;
}

void lib_shuf_count_note_lap(struct lib_shuffle_rec *r, uint32_t lap_before)
{
    if (lib_shuf_rec_lap(r) != lap_before)
        r->flags |= LIB_SHUF_F_LAP;
}

/* A lap starts at ordinal 'at' (<= total) */
static void count_lap_at(struct lib_shuf_count *c, uint32_t at)
{
    if (at > c->lap_at)
        c->prev_at = c->lap_at;     /* the lap before ends where this one
                                       starts, even if cut short */
    else if (at < c->lap_at && c->prev_at > at)
        c->prev_at = at;            /* moved back: no earlier lap known */
    c->lap_at = at;
}

void lib_shuf_count_queued(struct lib_shuffle_rec *r)
{
    if (r->flags & LIB_SHUF_F_LAP)
    {
        count_lap_at(&r->count, r->count.total);
        r->flags &= ~LIB_SHUF_F_LAP;
    }
    r->count.total++;
}

void lib_shuf_count_unqueue(struct lib_shuffle_rec *r, uint32_t k)
{
    struct lib_shuf_count *c = &r->count;

    c->total = k < c->total ? c->total - k : 0;
    if (c->lap_at > c->total)
        c->lap_at = c->total;
    if (c->prev_at > c->lap_at)
        c->prev_at = c->lap_at;
}

void lib_shuf_count_extra(struct lib_shuffle_rec *r, uint32_t k)
{
    r->count.total += k;
}

void lib_shuf_count_lap_from(struct lib_shuffle_rec *r, uint32_t back)
{
    struct lib_shuf_count *c = &r->count;

    if (back > c->total)
        back = c->total;
    count_lap_at(c, c->total - back);
}

uint32_t lib_shuf_count_pos(const struct lib_shuffle_rec *r, uint32_t after)
{
    const struct lib_shuf_count *c = &r->count;
    uint32_t t, len, d;

    if (after >= c->total)
        return 0;                   /* queued before the count began */
    t = c->total - 1 - after;       /* the entry's ordinal */
    if (t >= c->lap_at)
        return t - c->lap_at + 1;
    len = c->lap_at - c->prev_at;   /* an earlier lap */
    if (len == 0)
        return 0;
    d = c->lap_at - t;              /* 1 = the previous lap's last */
    return len - (d - 1) % len;
}
