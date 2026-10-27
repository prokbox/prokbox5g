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
#ifndef _IPODUI_SHUFFLE_CORE_H_
#define _IPODUI_SHUFFLE_CORE_H_

/* Pure C: no Rockbox headers, no statics, no allocation, integer maths
 * (host-tested by tests/nowplaying/). Design: docs/design/shuffle.md.
 *
 * A shuffled order over N items costs O(1) RAM: position k (0..N-1) maps
 * to item shuf_perm_at(p, k), a keyed 4-round Feistel permutation of the
 * smallest even-bit power of two >= N, cycle-walked into [0, N). It is a
 * bijection, so every item comes exactly once per lap (no repeats until
 * exhausted), it is deterministic for a seed (resume = seed + position),
 * and it runs both ways (previous track = position k - 1). Every struct
 * here is plain data and may be saved as is in a resume record. */

#include <stdbool.h>
#include <stdint.h>

#define SHUF_NONE 0xffffffffu

struct shuf_perm {
    uint32_t n;             /* items */
    uint32_t half_bits;     /* Feistel half width; domain = 4^half_bits */
    uint32_t key[4];        /* round keys, from the seed */
};

/* n may be 0 (empty: shuf_perm_at() returns SHUF_NONE). n <= 2^30. */
void     shuf_perm_init(struct shuf_perm *p, uint32_t n, uint32_t seed);
/* Item at position i (i < n), else SHUF_NONE. Average cost: under 4
 * Feistel evaluations (4 rounds, 2 multiplies each) because the domain is
 * less than 4n. */
uint32_t shuf_perm_at(const struct shuf_perm *p, uint32_t i);

/* Seed for lap 'lap' (0, 1, ...) of a run started with 'seed' (repeat on:
 * every lap is a fresh order, still deterministic). */
uint32_t shuf_lap_seed(uint32_t seed, uint32_t lap);

/* ---- Songs: one order over every library entry ------------------------
 * Items are tagcache master index entries 0..N-1 (N = tagcache_ui_count()
 * when the run started). The caller skips entries that are not music or
 * are deleted (shuf_songs_next() just yields indices; see the design doc
 * for the filter), so there is nothing per track in RAM. 'first' is the
 * entry that started the run (the song the user picked, or SHUF_NONE):
 * it plays first and is skipped when the order reaches it. */
struct shuf_songs {
    struct shuf_perm p;
    uint32_t seed;          /* run seed (lap 0) */
    uint32_t lap;
    uint32_t pos;           /* next position in this lap */
    uint32_t first;         /* SHUF_NONE or the entry played first */
    uint32_t last;          /* last entry returned (lap boundary check) */
};

void shuf_songs_init(struct shuf_songs *s, uint32_t n, uint32_t seed,
                     uint32_t first);
/* Next entry, or SHUF_NONE at the end of the lap ('repeat' false) or if
 * n is 0. With 'repeat', a new lap starts with a new order (and its first
 * entry is never the one just played, when n > 1). */
uint32_t shuf_songs_next(struct shuf_songs *s, bool repeat);
/* Step back one position in this lap (previous track past the playlist
 * window): the entry before the last one returned, or SHUF_NONE at the
 * start of the lap. */
uint32_t shuf_songs_prev(struct shuf_songs *s);

/* ---- Albums: album order shuffled, tracks in album order --------------
 * Items are albums 0..A-1 (rows of the library's album list); the caller
 * gives each album's track count through album_len() (0 = skip it, e.g.
 * no music left in it). Yields (album, track-in-album) pairs. */
typedef uint32_t (*shuf_album_len_fn)(void *ctx, uint32_t album);

struct shuf_albums {
    struct shuf_perm p;
    uint32_t seed;
    uint32_t lap;
    uint32_t apos;          /* position of the current album in the order */
    uint32_t track;         /* next track in it */
    bool started;
};

void shuf_albums_init(struct shuf_albums *s, uint32_t albums, uint32_t seed);
/* Next (album, track). False at the end of the lap (!repeat) or if there
 * is nothing to play. */
bool shuf_albums_next(struct shuf_albums *s, shuf_album_len_fn len,
                      void *ctx, bool repeat, uint32_t *album,
                      uint32_t *track);
/* The pair before the last one returned (false at the lap start). */
bool shuf_albums_prev(struct shuf_albums *s, shuf_album_len_fn len,
                      void *ctx, uint32_t *album, uint32_t *track);

/* ---- Albums on an ordinary queue (Revision 1 item 10) ----------------
 * Reorders a[0..n) in place so that its groups (bit k of 'starts' set: a
 * group starts at k; k = 0 always does) come in the order of a
 * shuf_perm over the groups seeded 'seed', each keeping its entries in
 * order. 'placed' is caller scratch of (groups + 7) / 8 bytes, at most
 * (n + 7) / 8. No allocation; O(groups x n) bit steps plus O(n) moves per
 * group (rotations), ~1-2 M cycles at n = 2,000. Returns the groups. */
uint32_t shuf_group_order(unsigned long *a, uint32_t n,
                          const uint8_t *starts, uint8_t *placed,
                          uint32_t seed);

#endif /* _IPODUI_SHUFFLE_CORE_H_ */
