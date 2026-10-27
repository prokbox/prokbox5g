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
}
