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
 * USB mass storage prefetch cache (ProkBox #81): the bookkeeping only,
 * pure, so that tests/usb/pf_test.c runs it on the host against a model
 * disk with a model worker.
 *
 * Why: macOS's msdos driver reads the whole FAT (~250 MB on the 1 TB
 * volume) in ~12 KB READs before it mounts, and gives up after ~20 s. Bulk
 * Only is one command at a time, so a READ served by a flash read in
 * series runs at ~5 MB/s. Here a worker thread reads ahead in large reads
 * into a ring of RAM (taken only while USB owns the disk) while the USB
 * thread answers the host's READs from it: storage and USB overlap, and
 * the flash sees few, large commands.
 *
 * The ring holds the sectors [lo, hi) of one LUN, sector s at ring sector
 * s & (cap - 1). The worker reads [hi, hi + n) in one storage read; the
 * USB thread sends from the ring by DMA. Rules (usb_storage.c keeps them):
 * - One worker, one read in flight. A read never wraps the ring.
 * - pin: the sector the USB is sending from (or last sent from). The
 *   worker never overwrites it or anything after it: a planned read
 *   first evicts the oldest sectors, never those at or after pin.
 * - Any WRITE drops everything (upf_drop): the generation changes, so a
 *   read still in flight is thrown away when it lands. Reads issued after
 *   it see the disk after the WRITE (Bulk Only: the WRITE completes before
 *   the next command).
 * - The stream starts after two READs in a row that continue each other
 *   (same LUN), at the end of the second; a non-continuing miss keeps the
 *   cache (the host may come back) and a continuing one restarts it.
 * - The first read of a stream is small (chunk / 4) and the size doubles
 *   up to chunk, so a false start costs little.
 * - A failed read stops the stream; the host's READ then misses and the
 *   ordinary path reports the error.
 * - Random mode: after UPF_RANDOM_N READs in a row that do not continue
 *   the one before (fsck, a scattered metadata walk), the stream plans no
 *   read-ahead and no READ waits for the worker (PENDING counts as MISS)
 *   until a READ continues the previous one again. A read-ahead there is
 *   256 KB read for nothing, and a wait stalls the one thread that
 *   answers the host.
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
#ifndef USB_STORAGE_PF_H
#define USB_STORAGE_PF_H

#include <stdint.h>
#include <stdbool.h>

#define UPF_NONE UINT64_MAX
#define UPF_RANDOM_N 3      /* non-continuing READs in a row: random mode */

struct upf {
    uint64_t lo, hi;        /* valid in the ring: [lo, hi) */
    uint64_t pin;           /* nothing at or after it is overwritten */
    uint64_t next;          /* the sector after the last READ */
    uint64_t limit;         /* the stream's disk size, sectors */
    uint32_t cap;           /* ring size, sectors; a power of two */
    uint32_t chunk;         /* largest read, sectors; cap >= 4 * chunk */
    uint32_t ramp;          /* the next read's size, sectors */
    uint32_t inflight;      /* sectors being read at hi; 0: none */
    uint32_t gen;           /* changes when the ring's data is dropped */
    uint32_t nonseq;        /* consecutive READs that did not continue */
    int lun;                /* the ring's LUN; -1: none */
    int rlun;               /* the last READ's LUN */
    bool stream;            /* the worker reads ahead */
};

enum { UPF_MISS, UPF_HIT, UPF_PENDING };

static inline uint32_t upf_pos(const struct upf *r, uint64_t s)
{
    return (uint32_t)(s & (r->cap - 1));
}

/* Everything cached becomes invalid, and the stream stops. */
static inline void upf_drop(struct upf *r)
{
    r->lo = r->hi = r->pin = 0;
    r->inflight = 0;
    r->stream = false;
    r->next = UPF_NONE;
    r->lun = -1;
    r->rlun = -1;
    r->nonseq = 0;
    r->gen++;
}

static inline void upf_init(struct upf *r, uint32_t cap, uint32_t chunk)
{
    r->cap = cap;
    r->chunk = chunk;
    r->ramp = chunk;
    r->gen = 0;
    r->limit = 0;
    upf_drop(r);
}

static inline bool upf_random(const struct upf *r)
{
    return r->nonseq >= UPF_RANDOM_N;
}

/* Does a READ at s of `lun` continue the previous READ? */
static inline bool upf_is_seq(const struct upf *r, int lun, uint64_t s)
{
    return lun == r->rlun && s == r->next;
}

/* Can [s, s + n) of `lun` be sent from the ring: now (HIT), once the
 * stream has read it (PENDING), or not (MISS)? No side effects. */
static inline int upf_lookup(const struct upf *r, int lun, uint64_t s,
                             uint32_t n)
{
    if (lun != r->lun || s < r->lo)
        return UPF_MISS;
    if (s + n <= r->hi)
        return UPF_HIT;
    /* With pin at s the worker fills up to s + cap - chunk at least */
    if (r->stream && !upf_random(r) && s <= r->hi + r->inflight && n <= r->cap - r->chunk
        && s + n <= r->limit)
        return UPF_PENDING;
    return UPF_MISS;
}

/* The USB is about to send from s on (at a READ's start and per piece). */
static inline void upf_pin(struct upf *r, uint64_t s)
{
    r->pin = s;
}

/* A READ of n sectors at s starts; `cached` is whether it was a HIT or
 * PENDING. Returns true when the stream (re)started at s + n: wake the
 * worker. `limit` is the disk's sector count. */
static inline bool upf_note_read(struct upf *r, int lun, uint64_t s,
                                 uint32_t n, bool cached, uint64_t limit)
{
    bool seq = upf_is_seq(r, lun, s);

    if (seq)
        r->nonseq = 0;
    else if (r->nonseq < UPF_RANDOM_N)
        r->nonseq++;
    r->rlun = lun;
    r->next = s + n;
    if (cached || !seq || s + n >= limit)
        return false;
    r->gen++;                   /* a read in flight belongs to the old one */
    r->lo = r->hi = r->pin = s + n;
    r->inflight = 0;
    r->limit = limit;
    r->lun = lun;
    r->ramp = r->chunk / 4 ? r->chunk / 4 : 1;
    r->stream = true;
    return true;
}

/* The worker: the next read, or 0 for none. On a non-zero return the read
 * of *n sectors at *start goes to ring sector *pos; pass *gen back to
 * upf_done(). The sectors it will overwrite are already evicted. */
static inline uint32_t upf_plan(struct upf *r, uint64_t *start,
                                uint32_t *pos, uint32_t *gen)
{
    uint64_t base;
    uint32_t n, p;

    if (!r->stream || r->inflight || r->hi >= r->limit || upf_random(r))
        return 0;
    base = r->pin < r->lo ? r->lo : r->pin > r->hi ? r->hi : r->pin;
    n = r->ramp;
    if (r->cap - (r->hi - base) < n)
        return 0;               /* full up to pin */
    p = upf_pos(r, r->hi);
    if (n > r->cap - p)
        n = r->cap - p;         /* never wraps */
    if (n > r->limit - r->hi)
        n = (uint32_t)(r->limit - r->hi);
    if (r->hi + n - r->lo > r->cap)
        r->lo = r->hi + n - r->cap;     /* < base: never what is pinned */
    r->inflight = n;
    *start = r->hi;
    *pos = p;
    *gen = r->gen;
    return n;
}

/* The worker's read of n sectors at start has landed (ok) or failed.
 * Returns true when the data now counts as cached. */
static inline bool upf_done(struct upf *r, uint32_t gen, uint64_t start,
                            uint32_t n, bool ok)
{
    if (gen != r->gen)
        return false;           /* dropped while in flight */
    r->inflight = 0;
    if (!ok) {
        r->stream = false;
        return false;
    }
    r->hi = start + n;
    if (r->ramp < r->chunk)
        r->ramp = r->ramp * 2 < r->chunk ? r->ramp * 2 : r->chunk;
    return true;
}

#endif /* USB_STORAGE_PF_H */
