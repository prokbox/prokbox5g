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
 * iPod UI shell: library-backed shuffle, permutation core (#27)
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
/* Pure C on purpose: no Rockbox headers (host-tested by tests/nowplaying/).
 * Design and costs: docs/design/shuffle.md. */
#include "shuffle_core.h"

/* 32-bit integer hash (avalanche finalizer; two multiplies, ARM7TDMI MUL) */
static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void shuf_perm_init(struct shuf_perm *p, uint32_t n, uint32_t seed)
{
    uint32_t h = 1;
    int i;

    p->n = n;
    /* smallest h >= 1 with 4^h >= n: the domain is < 4n */
    while (h < 15 && ((uint32_t)1 << (2 * h)) < n)
        h++;
    p->half_bits = h;
    for (i = 0; i < 4; i++)
        p->key[i] = mix32(seed + 0x9e3779b9u * (uint32_t)(i + 1));
}

static uint32_t feistel(const struct shuf_perm *p, uint32_t x)
{
    uint32_t h = p->half_bits;
    uint32_t mask = ((uint32_t)1 << h) - 1;
    uint32_t l = x >> h, r = x & mask;
    int i;

    for (i = 0; i < 4; i++)
    {
        uint32_t nl = r;
        r = l ^ (mix32(r ^ p->key[i]) & mask);
        l = nl;
    }
    return (l << h) | r;
}

uint32_t shuf_perm_at(const struct shuf_perm *p, uint32_t i)
{
    uint32_t x = i;

    if (i >= p->n)
        return SHUF_NONE;
    /* cycle walking: the Feistel network permutes [0, 4^h); following the
     * cycle from i until it is back in [0, n) is a permutation of [0, n) */
    do
        x = feistel(p, x);
    while (x >= p->n);
    return x;
}

uint32_t shuf_lap_seed(uint32_t seed, uint32_t lap)
{
    if (lap == 0)
        return seed;
    return mix32(seed ^ mix32(lap * 0x85ebca6bu + 0x27d4eb2fu));
}

/* -- Songs ------------------------------------------------------------- */

void shuf_songs_init(struct shuf_songs *s, uint32_t n, uint32_t seed,
                     uint32_t first)
{
    shuf_perm_init(&s->p, n, seed);
    s->seed = seed;
    s->lap = 0;
    s->pos = 0;
    s->first = first < n ? first : SHUF_NONE;
    s->last = SHUF_NONE;
}

static bool songs_skip(const struct shuf_songs *s, uint32_t e)
{
    return s->lap == 0 && e == s->first;
}

uint32_t shuf_songs_next(struct shuf_songs *s, bool repeat)
{
    uint32_t n = s->p.n;

    if (n == 0)
        return SHUF_NONE;
    if (s->lap == 0 && s->pos == 0 && s->first != SHUF_NONE
        && s->last == SHUF_NONE)
    {
        s->last = s->first;
        return s->first;
    }
    for (;;)
    {
        while (s->pos < n)
        {
            uint32_t e = shuf_perm_at(&s->p, s->pos++);
            if (!songs_skip(s, e))
            {
                s->last = e;
                return e;
            }
        }
        if (!repeat)
            return SHUF_NONE;
        /* new lap; never start it with the entry just played */
        do
        {
            s->lap++;
            shuf_perm_init(&s->p, n, shuf_lap_seed(s->seed, s->lap));
        } while (n > 1 && shuf_perm_at(&s->p, 0) == s->last);
        s->pos = 0;
    }
}

uint32_t shuf_songs_prev(struct shuf_songs *s)
{
    uint32_t j;

    if (s->pos == 0)
        return SHUF_NONE;           /* at 'first', or nothing played yet */
    for (j = s->pos - 1; j-- > 0; )
    {
        uint32_t e = shuf_perm_at(&s->p, j);
        if (!songs_skip(s, e))
        {
            s->pos = j + 1;
            s->last = e;
            return e;
        }
    }
    if (s->lap == 0 && s->first != SHUF_NONE)
    {
        s->pos = 0;
        s->last = s->first;
        return s->first;
    }
    return SHUF_NONE;
}

/* -- Albums ------------------------------------------------------------ */

void shuf_albums_init(struct shuf_albums *s, uint32_t albums, uint32_t seed)
{
    shuf_perm_init(&s->p, albums, seed);
    s->seed = seed;
    s->lap = 0;
    s->apos = 0;
    s->track = 0;
    s->started = false;
}

bool shuf_albums_next(struct shuf_albums *s, shuf_album_len_fn len,
                      void *ctx, bool repeat, uint32_t *album,
                      uint32_t *track)
{
    uint32_t a_n = s->p.n;
    uint32_t budget;            /* album steps: a lap and a bit, then stop */

    if (a_n == 0)
        return false;
    budget = 2 * a_n + 1;
    if (!s->started)
    {
        s->started = true;
        s->apos = 0;
        s->track = 0;
    }
    while (budget-- > 0)
    {
        if (s->apos >= a_n)
        {
            uint32_t prev = s->apos > 0
                            ? shuf_perm_at(&s->p, a_n - 1) : SHUF_NONE;
            if (!repeat)
                return false;
            do
            {
                s->lap++;
                shuf_perm_init(&s->p, a_n, shuf_lap_seed(s->seed, s->lap));
            } while (a_n > 1 && shuf_perm_at(&s->p, 0) == prev);
            s->apos = 0;
            s->track = 0;
        }
        {
            uint32_t a = shuf_perm_at(&s->p, s->apos);
            if (s->track < len(ctx, a))
            {
                *album = a;
                *track = s->track++;
                return true;
            }
        }
        s->apos++;
        s->track = 0;
    }
    return false;
}

bool shuf_albums_prev(struct shuf_albums *s, shuf_album_len_fn len,
                      void *ctx, uint32_t *album, uint32_t *track)
{
    uint32_t ap;

    if (!s->started || s->apos >= s->p.n)
        return false;
    /* current = (apos, track - 1) */
    if (s->track >= 2)
    {
        s->track--;
        *album = shuf_perm_at(&s->p, s->apos);
        *track = s->track - 1;
        return true;
    }
    for (ap = s->apos; ap-- > 0; )
    {
        uint32_t a = shuf_perm_at(&s->p, ap);
        uint32_t l = len(ctx, a);
        if (l > 0)
        {
            s->apos = ap;
            s->track = l;
            *album = a;
            *track = l - 1;
            return true;
        }
    }
    return false;
}

/* ---- album groups ------------------------------------------------------ */

static bool bit_at(const uint8_t *b, uint32_t k)
{
    return (b[k >> 3] >> (k & 7)) & 1;
}

static void reverse(unsigned long *a, uint32_t lo, uint32_t hi)
{
    while (hi > lo + 1)
    {
        unsigned long t = a[lo];
        hi--;
        a[lo++] = a[hi];
        a[hi] = t;
    }
}

/* [lo, mid) [mid, hi) -> [mid, hi) [lo, mid) */
static void rotate(unsigned long *a, uint32_t lo, uint32_t mid, uint32_t hi)
{
    reverse(a, lo, mid);
    reverse(a, mid, hi);
    reverse(a, lo, hi);
}

uint32_t shuf_group_order(unsigned long *a, uint32_t n,
                          const uint8_t *starts, uint8_t *placed,
                          uint32_t seed)
{
    struct shuf_perm p;
    uint32_t groups = 0, k, done = 0;

    for (k = 0; k < n; k++)
        if (k == 0 || bit_at(starts, k))
            groups++;
    if (groups < 2)
        return groups;
    for (k = 0; k < (groups + 7) / 8; k++)
        placed[k] = 0;
    shuf_perm_init(&p, groups, seed);

    for (k = 0; k < groups; k++)
    {
        uint32_t want = shuf_perm_at(&p, k);
        uint32_t g = 0, i = 0, pos = done, size = 0;

        /* walk the original groups; the unplaced ones sit after 'done'
         * in their original order */
        while (i < n)
        {
            uint32_t j = i + 1;

            while (j < n && !bit_at(starts, j))
                j++;
            if (!bit_at(placed, g))
            {
                if (g == want)
                {
                    size = j - i;
                    break;
                }
                pos += j - i;
            }
            g++;
            i = j;
        }
        if (size && pos > done)
            rotate(a, done, pos, pos + size);
        placed[want >> 3] |= (uint8_t)(1u << (want & 7));
        done += size;
    }
    return groups;
}
