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
#ifndef _IPODUI_AB_STORE_H_
#define _IPODUI_AB_STORE_H_

/* Phase 6 contract: the per-book resume store, ARTCACHE_DIR "/books.dat"
 * (§3.6 + Rev 1 items 2, 7, 11, 12 + Rev 2). FROZEN: changes go through
 * the orchestrator. Outside the database, so it survives a rebuild and a
 * changelog loss. The single resume authority for book queues (Rev 1
 * item 1).
 *
 * File layout (512 B sectors, native byte order):
 *   sector 0                   struct ab_store_hdr, rest zero
 *   sectors 1 .. S             records, 16 per sector (never straddling),
 *                              sorted by book_key (unique); slots past
 *                              'count' are zero
 *   sector S + 1               the header again (Rev 1 item 7)
 * S = hdr.sectors = ceil(count / 16), count <= AB_STORE_MAX.
 * Reading the header: both copies are checked (magic, version, sum); the
 * valid one with the larger seq wins; neither valid = empty store
 * (logged), and books.tmp is tried when books.dat is missing.
 *
 * Writes (B): an update rewrites ONE record in place (read the sector,
 * patch the slot, write it, read it back); a sector write is not assumed
 * atomic: a torn sector loses at most its 16 records' latest checkpoints,
 * which then read as previous or unplayed (each record has its own check).
 * An insert or an eviction writes books.tmp, removes books.dat, renames.
 * Storage-idle callbacks (storage thread, small stack) only do in-place
 * record writes; inserts run on the UI thread (np_pb_idle()).
 *
 * Pure part (ab_store_core.c, host-tested by tests/abbook/test_abstore):
 * record seal/check/state, header pick, sector maths, sector-index
 * lookup, in-sector search, the eviction order. No Rockbox headers. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ab_book.h"            /* ab_key_t, struct ab_store_index */

#define AB_STORE_MAGIC     0x42415049u  /* "IPAB" */
#define AB_STORE_VERSION   1
#define AB_STORE_SECTOR    512
#define AB_STORE_PER_SECT  16           /* 512 / 32 */
#define AB_STORE_MAX       2048         /* ⚠️11 (64 KB of records) */

struct ab_store_hdr {           /* 32 B at the start of both copies */
    uint32_t magic;
    uint16_t version;
    uint16_t rec_size;          /* 32 */
    uint32_t seq;               /* +1 per header write; newer copy wins */
    uint32_t count;             /* records */
    uint32_t sectors;           /* record sectors (ceil(count / 16)) */
    uint32_t clock;             /* next record stamp (a floor: the
                                   loader takes max(clock, stamps + 1)) */
    uint32_t seeded;            /* 1: the tagcache migration ran (Rev 1
                                   item 12) */
    uint32_t sum;               /* ab_word_sum() of the first 28 B */
};

/* flags: low byte state bits, high byte the record check */
#define AB_REC_F_STARTED   0x0001       /* a resume spot exists */
#define AB_REC_F_FINISHED  0x0002       /* within 30 s of the end at stop,
                                           or the book's queue ended: no
                                           resume spot (Rev 2), Resume
                                           hidden; next play starts over */
#define AB_REC_F_SEEDED    0x0004       /* from tagcache stats (migration) */
#define AB_REC_STATE_MASK  0x00ff

struct ab_rec {                 /* 32 B, static-asserted */
    ab_key_t book_key;          /* identity (lib_art_key() of the book) */
    uint32_t file_crc;          /* ab_path_crc() of the resume file */
    uint16_t file_idx;          /* its position in the book (fallback
                                   when no file has file_crc) */
    uint16_t flags;             /* AB_REC_F_*; check in the high byte */
    uint32_t elapsed_ms;        /* in that file */
    uint32_t offset;            /* byte offset in that file (resume) */
    uint32_t book_ms;           /* whole-book position (finished rule) */
    uint32_t stamp;             /* store write clock: eviction order (D10;
                                   replaces §3.6's book_len_ms, which
                                   Rev 2's no-progress list left unused) */
};

enum ab_rec_state {
    AB_REC_NONE = 0,            /* no record, or its check failed */
    AB_REC_STARTED,             /* resume spot: Resume shown */
    AB_REC_FINISHED,            /* no resume spot */
};

/* ---- pure (ab_store_core.c) --------------------------------------------- */

/* the 8-bit check over the record (flags' high byte excluded); never 0
 * for an all-zero record, so zeroed slots read as AB_REC_NONE */
uint8_t ab_rec_check(const struct ab_rec *r);
void    ab_rec_seal(struct ab_rec *r);          /* sets the check byte */
bool    ab_rec_ok(const struct ab_rec *r);
enum ab_rec_state ab_rec_state(const struct ab_rec *r);
/* Fills a checkpoint (STARTED; FINISHED by ab_finished(book_ms, len_ms)
 * when 'stopping'; a finished record keeps no position), stamps and
 * seals it. */
void ab_rec_set(struct ab_rec *r, ab_key_t key, uint32_t file_crc,
                int file_idx, uint32_t elapsed_ms, uint32_t offset,
                uint32_t book_ms, uint32_t len_ms, bool stopping,
                uint32_t stamp);

void ab_store_hdr_seal(struct ab_store_hdr *h);
bool ab_store_hdr_ok(const struct ab_store_hdr *h);
/* the copy to use (newer valid seq), or NULL if neither is valid */
const struct ab_store_hdr *ab_store_hdr_pick(const struct ab_store_hdr *a,
                                             const struct ab_store_hdr *b);

/* sector maths */
static inline int ab_store_sectors(int count)
{
    return (count + AB_STORE_PER_SECT - 1) / AB_STORE_PER_SECT;
}
static inline int ab_store_rec_sector(int r)    /* file sector of rec r */
{
    return 1 + r / AB_STORE_PER_SECT;
}
static inline long ab_store_file_size(int count)
{
    return (long)(ab_store_sectors(count) + 2) * AB_STORE_SECTOR;
}

/* The record sector that would hold key (index into the record sectors,
 * 0-based): the last s with first_key[s] <= key; 0 if key is below all.
 * -1 if there are no sectors. One sector read then finds or rules out
 * the key (Rev 1 item 2). */
int ab_store_sector_for(const struct ab_store_index *ix, ab_key_t key);
/* Builds the index from one sector's records as they are loaded (call
 * for s = 0, 1, ... in order; recs = that sector's AB_STORE_PER_SECT
 * slots): first key and the clock. */
void ab_store_index_add(struct ab_store_index *ix, int s,
                        const struct ab_rec *recs, int n);
/* Binary search in n sorted records (a sector, or a whole file in RAM
 * while rewriting): index, or -(insertion point) - 1. Records whose check
 * fails keep their key for ordering (a torn record is still in place). */
int ab_store_rec_find(const struct ab_rec *recs, int n, ab_key_t key);
/* Rev 1 item 11: past AB_STORE_MAX, evict the oldest (lowest stamp)
 * FINISHED record, else the oldest STARTED, else the oldest of any (bad
 * check). Returns its index, -1 if n == 0. */
int ab_store_evict_pick(const struct ab_rec *recs, int n);

/* ---- runtime (B: ab_store.c; stub: ab_stub.c) -----------------------------
 * UI thread unless marked. Storage per call in brackets. */

/* (re)loads the header + sector index into the book block [1 read of the
 * file, <= 66 KB, once per boot/insert]; false = empty store */
bool ab_store_load(void);
/* the state of key's record [one sector read; 0 if the sector is the
 * cached one] */
enum ab_rec_state ab_store_find(ab_key_t key);
/* copies key's record into *out (false: none, or check failed) [one
 * sector read] */
bool ab_store_get(ab_key_t key, struct ab_rec *out);
/* write now (pause, stop, USB, shutdown; Rev 1 item 7): in place, or an
 * insert (books.tmp + rename) [1-3 writes, wakes the flash once];
 * traced AB_TR_STORE_W "now" */
bool ab_store_put(const struct ab_rec *r);
/* audio thread (a track change within a book, the 1 Hz position): copies
 * r into the 32 B pending record (word copy under disable_irq). Never I/O.
 */
void ab_store_pending(const struct ab_rec *r);
/* audio thread (pause, stop; fix round H1): the same, flagged write-now.
 * The next UI pass, or any flush (USB connect, shutdown), writes it. A
 * record older than the pending one, or than the last written one, of
 * the same book is dropped. */
void ab_store_pending_now(const struct ab_rec *r);
/* storage-idle callback side (register_storage_idle_func) and the UI
 * idle: writes the pending record in place if storage just ran, or once
 * per 30 min forced (⚠️12); inserts only from the UI thread. Traced
 * AB_TR_STORE_W "idle". Returns ticks until it wants the next call, or
 * < 0 for none. */
int  ab_store_idle(bool ui_thread);
/* the tagcache migration (§3.6, Rev 1 item 12): once, when the database
 * is ready; seeds books with stats only; traced AB_TR_SEED */
void ab_store_seed(void);

/* #67 (Phase 8b §3.2): the book to pin at the top of Music > Audiobooks,
 * first match wins:
 *   1. the book playing or paused now: ab_book_playing() and the book
 *      map's book_key (no store access);
 *   2. the book of the pending record (written or not), under the same
 *      disable_irq the pending copy already uses;
 *   3. the highest-stamped written record with ab_rec_ok() and without
 *      AB_REC_F_SEEDED (seeds are stamped in walk order by the migration,
 *      so their stamps say nothing);
 *   else 0 (no pin; O6f).
 * Rule 3's key is found at ab_store_load() from the records it already
 * reads, and updated by every record write (put_locked() for write-now,
 * the flush, the forced checkpoint and inserts; storage_pass()), on the
 * UI and storage threads, so the 64-bit key and its stamp are published
 * together under disable_irq and read the same way. *gen (if not NULL)
 * is a counter that moves whenever the returned key differs from the
 * previous call's. UI thread; RAM only, never I/O. */
ab_key_t ab_store_pin_key(uint32_t *gen);
/* The rule that gave ab_store_pin_key()'s last answer (trace only). */
enum ab_pin_src {
    AB_PIN_NONE = 0,
    AB_PIN_PLAY,                /* rule 1 */
    AB_PIN_PENDING,             /* rule 2 */
    AB_PIN_STAMP,               /* rule 3 */
};
enum ab_pin_src ab_store_pin_src(void);

#endif /* _IPODUI_AB_STORE_H_ */
