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
 * USB mass storage read-ahead (ProkBox #63): the bookkeeping only, pure so
 * that tests/usb/ra_test.c runs it on the host against a model disk.
 *
 * Why: a SCSI READ that fits one transfer buffer is served strictly in
 * series (CBW, flash read, USB send, CSW), so flash latency and USB time
 * add up for every command. macOS's mount-time FAT scan is ~8,600 such
 * commands averaging 12 KB. When the host reads sequentially, the next
 * buffer's worth is read while the current reply is still on the wire, and
 * the following commands are answered from RAM.
 *
 * Rules (usb_storage.c keeps them):
 * - Memory: the two halves of the existing double buffer, nothing more.
 *   The prefetch sits in the half the USB is not sending from.
 * - Only after two READs in a row that continue each other (same LUN).
 * - Each prefetched sector is sent at most once, so the sector filters
 *   (usb_storage_xlate.c) see every sector once, freshly read, as before.
 * - Any WRITE, a new session, or any miss drops it. Nothing else writes
 *   the disk while the host owns it (exclusive storage).
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
#ifndef USB_STORAGE_RA_H
#define USB_STORAGE_RA_H

#include <stdint.h>
#include <stdbool.h>

struct ura {
    uint64_t sector;        /* first prefetched sector not yet sent */
    uint64_t next;          /* the sector after the last READ */
    uint32_t count;         /* prefetched sectors left; 0 = empty */
    uint32_t off;           /* sectors into the half where `sector` is */
    uint32_t last_count;    /* sectors in the last READ */
    int half;               /* buffer half holding the prefetch */
    int lun;
    bool seq;               /* the last READ continued the one before */
};

static inline void ura_reset(struct ura *r)
{
    r->count = 0;
    r->next = UINT64_MAX;
    r->lun = -1;
    r->seq = false;
}

/* Any WRITE: the prefetch may be stale, and the sequence starts over. */
static inline void ura_write(struct ura *r)
{
    r->count = 0;
    r->next = UINT64_MAX;
    r->seq = false;
}

/* A READ of `count` sectors at `sector` starts; `chunk` is one buffer half
 * in sectors. Returns how many sectors its first chunk takes from the
 * prefetch (0: none, read as usual), with the place in *half / *off:
 * - the whole READ, when it fits in what is left, or
 * - a full, untouched prefetch as the first chunk of a longer READ (the
 *   rest is read into the other half as usual).
 * Served sectors are consumed: never sent twice. */
static inline uint32_t ura_read(struct ura *r, int lun, uint64_t sector,
                                uint32_t count, uint32_t chunk,
                                int *half, uint32_t *off)
{
    uint32_t n = 0;

    r->seq = lun == r->lun && sector == r->next;
    r->lun = lun;
    r->next = sector + count;
    r->last_count = count;

    if (r->count && r->seq && sector == r->sector) {
        if (count <= r->count)
            n = count;
        else if (r->off == 0 && r->count == chunk)
            n = chunk;
    }
    if (!n) {
        r->count = 0;
        return 0;
    }
    *half = r->half;
    *off = r->off;
    r->sector += n;
    r->off += n;
    r->count -= n;
    return n;
}

/* After a READ's last chunk went out of half `sent_half`, with no error:
 * returns how many sectors to prefetch at *start into the other half
 * (0: none). `limit` is the disk's sector count. */
static inline uint32_t ura_plan(const struct ura *r, uint64_t limit,
                                uint32_t chunk, uint64_t *start)
{
    if (!r->seq || r->next >= limit)
        return 0;
    if (r->count && r->count >= r->last_count)
        return 0;               /* another READ this size still fits */
    *start = r->next;           /* == r->sector when something is left */
    return (limit - r->next) < chunk ? (uint32_t)(limit - r->next) : chunk;
}

static inline void ura_filled(struct ura *r, uint64_t start, uint32_t n,
                              int half)
{
    r->sector = start;
    r->count = n;
    r->off = 0;
    r->half = half;
}

#endif /* USB_STORAGE_RA_H */
