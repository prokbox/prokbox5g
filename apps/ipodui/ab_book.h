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
#ifndef _IPODUI_AB_BOOK_H_
#define _IPODUI_AB_BOOK_H_

/* Phase 6 contract (docs/phase6-brief.md, "Contracts (committed)"):
 * books as first-class objects. FROZEN: changes go through the
 * orchestrator.
 *
 *  - books.pak / books.idx: per-book blobs (file map + packed entry table
 *    with book-relative starts), built by the art builder while stopped
 *    (Rev 1 item 4). Identity = book key; validity = {max_mtime,
 *    file_count}; the index carries a generation (readers re-check it).
 *  - The book block: the playing book's map, its tick bitmap and the
 *    books.dat sector index (ab_store.h), one permanent core_alloc at
 *    shell start (before any playback).
 *  - Pure rules (ab_book_core.c, host-tested in tests/abbook): position,
 *    counter source and entry, ticks, finished, speed factor.
 *  - Runtime entry points (ab_book.c / ab_speed.c / library.c, B; the
 *    shared stub ab_stub.c until B lands).
 *  - The trace lines (AB_TR_*), logged with UPDATE_TRACE (anim.h; logf is
 *    a no-op in apps/ipodui). T's check_trace.py rules match them.
 *
 * Pure part: no Rockbox headers beyond chapters.h (host tests). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "chapters.h"           /* struct audiobook_chapters (packed) */

typedef uint64_t ab_key_t;      /* lib_art_key() of the book row
                                   (artcache_key_t); 0 = none */

/* ---- limits ------------------------------------------------------------ */
#define AB_MAP_FILES_MAX   1024     /* ⚠️8: a bigger book keeps the per-file
                                       bar and queue counter (traced) */
#define AB_FINISH_MS       30000    /* Rev 2: within 30 s of the end */
#define AB_TICK_RES        216      /* tick bitmap bits (= NP_BAR_W,
                                       asserted where both are seen) */
#define AB_TICK_BYTES      28       /* (AB_TICK_RES + 7) / 8, 4-aligned */

/* ---- books.idx (ARTCACHE_DIR "/books.idx") -----------------------------
 * Header, then 'count' records sorted by book_key (unique). Written whole
 * by temp + rename (books.idx.tmp), generation + 1 each time. Native byte
 * order. sum = ab_word_sum() over the header (with sum = 0) and the
 * records. Wrong magic/version/sum = no index: every book is owed. */
#define AB_IDX_MAGIC     0x58425049u    /* "IPBX" */
#define AB_PAK_VERSION   1              /* idx and pak together */

struct ab_idx_hdr {             /* 32 B */
    uint32_t magic;
    uint16_t version;
    uint16_t rec_size;          /* sizeof(struct ab_idx_rec) */
    uint32_t generation;
    uint32_t count;
    uint32_t pak_size;          /* books.pak bytes this index covers */
    uint32_t dead_bytes;        /* superseded blob bytes (compaction at
                                   > 50 % of pak_size, ⚠️15) */
    uint32_t spare;             /* 0 */
    uint32_t sum;
};

struct ab_idx_rec {             /* 24 B */
    ab_key_t book_key;
    uint32_t max_mtime;         /* validity, with file_count */
    uint16_t file_count;
    uint16_t flags;             /* AB_BLOB_F_* copy of the blob's */
    uint32_t offset;            /* blob offset in books.pak */
    uint32_t size;              /* blob bytes */
};

/* ---- books.pak (ARTCACHE_DIR "/books.pak") ------------------------------
 * Append-only blobs (Rev 1 item 4); compaction copies the live ones into
 * books.pak.tmp and renames it, then rewrites the index. One blob:
 *
 *   struct ab_blob_hdr                       24 B
 *   uint32_t file_start_ms[file_count]       book-relative file starts
 *   uint32_t file_crc[file_count]            ab_path_crc() of each path
 *   uint16_t file_first[file_count]          first entry of each file
 *   (pad to 4)
 *   struct audiobook_chapters table          trimmed: cap = entries,
 *                                            pool_size = pool_used
 *
 * Entries (§3.3): file by file in disc/track order, that file's chapters
 * (book-relative starts) if it has any, else the file itself (title = the
 * track title, flag AB_CH_F_FILE). A single-file book's entries are its
 * chapters (or one file entry). Max blob = 24 + 10 x 1,024 + 32,784 =
 * 43,048 B (D7). sum = ab_word_sum() over everything after the header.
 * Alignment: blobs start at 8-aligned pak offsets and their sizes are
 * multiples of 8 (the header holds a uint64_t); read them into an
 * 8-aligned buffer. The same holds for books.idx and the book block
 * (buflib aligns to 4 on the target: round the base up, as artcache.c
 * does). */
#define AB_BLOB_F_FILES  0x0001     /* no file has chapters: entries are
                                       the files */

struct ab_blob_hdr {            /* 24 B */
    ab_key_t book_key;          /* identity: must equal the idx record's */
    uint32_t max_mtime;         /* validity: must equal the idx record's */
    uint16_t file_count;        /* 1 .. AB_MAP_FILES_MAX */
    uint16_t flags;             /* AB_BLOB_F_* */
    uint32_t len_ms;            /* whole-book length */
    uint32_t sum;
};

#define AB_BLOB_MAP_SIZE(files) \
    (((size_t)(files) * 10 + 3) & ~(size_t)3)
#define AB_BLOB_MAX_SIZE \
    (sizeof(struct ab_blob_hdr) + AB_BLOB_MAP_SIZE(AB_MAP_FILES_MAX) \
     + AB_CH_TABLE_SIZE)                                    /* 43,048 */

/* Pointers into a blob in RAM (no copy); false if the blob is short,
 * inconsistent or its sum is wrong. */
struct ab_blob_view {
    const struct ab_blob_hdr *hdr;
    const uint32_t *file_start_ms;
    const uint32_t *file_crc;
    const uint16_t *file_first;
    const struct audiobook_chapters *tab;   /* chapter_count = entries */
};
bool ab_blob_parse(const void *blob, size_t size, struct ab_blob_view *v);

/* Lays out a blob for the builder: writes the header fields, copies the
 * map and the trimmed table (tab's chapter_count entries, pool_used
 * bytes; tab may be any cap), seals the sum. Returns the blob size, or 0
 * if it does not fit in 'size' or file_count is out of range. */
size_t ab_blob_build(void *blob, size_t size, ab_key_t key,
                     uint32_t max_mtime, uint32_t len_ms, int file_count,
                     const uint32_t *file_start_ms, const uint32_t *file_crc,
                     const uint16_t *file_first,
                     const struct audiobook_chapters *tab);

/* 32-bit word sum (the libfile_sum style, not a CRC); n is a multiple
 * of 4. */
uint32_t ab_word_sum(const void *p, size_t n);
/* CRC-32 (zlib polynomial) of a path's bytes: the file identity in a map
 * and in a resume record (rebuild-safe). */
uint32_t ab_path_crc(const char *path);

/* index lookup: position of key in recs[0..n) (sorted), or -1 */
int ab_idx_find(const struct ab_idx_rec *recs, int n, ab_key_t key);
/* the record is current for a book whose files now have these values */
static inline bool ab_idx_valid(const struct ab_idx_rec *r,
                                uint32_t max_mtime, int file_count)
{
    return r->max_mtime == max_mtime && r->file_count == file_count;
}

/* ---- the book block ----------------------------------------------------
 * One permanent core_alloc at shell start (ab_book_init()), before any
 * playback: the playing book's map + ticks, and books.dat's sector index.
 * Lean rule 9: shares with nothing (it must live while a book plays and
 * the screen is anywhere); can't share with the view pool because the
 * pool is rebuilt on every lib_open(). */
enum ab_map_src {
    AB_MAP_NONE = 0,            /* no book queue, or a bigger book (⚠️8) */
    AB_MAP_PAK,                 /* from the book's books.pak blob */
    AB_MAP_FALLBACK,            /* files as entries, from the RAM database */
    AB_MAP_RESUMED,             /* refilled by lib_queue_resumed() (boot) */
};

struct ab_book_map {
    ab_key_t book_key;          /* identity of the playing book */
    uint32_t len_ms;            /* whole book */
    uint16_t file_count;        /* = the book queue's length */
    uint16_t entries;           /* M of "N of M" */
    uint8_t  src;               /* enum ab_map_src */
    uint8_t  spare[3];
    uint32_t gen;               /* +1 per fill (readers re-check) */
    ab_key_t old_key;           /* #47: its pre-#47 key (books.pak entries
                                 * filed under it until the art pass
                                 * rebuilds them), 0: the same */
    uint8_t  ticks[AB_TICK_BYTES];  /* every entry start but the first, at
                                       AB_TICK_RES over len_ms */
    uint32_t file_start_ms[AB_MAP_FILES_MAX];
    uint32_t file_crc[AB_MAP_FILES_MAX];
    uint16_t file_first[AB_MAP_FILES_MAX];
};

#define AB_STORE_SECTORS_MAX  128   /* 2,048 records / 16 per sector */
struct ab_store_index {             /* books.dat's sector-first keys */
    uint32_t sectors;           /* record sectors in use */
    uint32_t count;             /* records */
    uint32_t clock;             /* next write stamp (ab_store.h) */
    uint32_t gen;               /* +1 per load or insert */
    ab_key_t first_key[AB_STORE_SECTORS_MAX];
};

struct ab_book_block {
    struct ab_book_map map;
    struct ab_store_index store;
};
#define AB_BOOK_BLOCK_SIZE  11344   /* static-asserted in ab_book_core.c */

/* ---- pure rules (ab_book_core.c) ---------------------------------------- */

/* Whole-book position of 'elapsed_ms' into map file q (clamped to len) */
uint32_t ab_book_pos(const struct ab_book_map *m, int q, uint32_t elapsed_ms);
/* The file holding book_ms (the last file start <= book_ms), and the
 * position within it; -1 if the map is empty. */
int ab_book_file_at(const struct ab_book_map *m, uint32_t book_ms,
                    uint32_t *file_ms);
/* Book entry of (file q, local chapter c of that file; c = 0 for a file
 * without chapters), clamped to that file's entries. -1 if q is out of
 * range. The counter is entry + 1 "of" m->entries. */
int ab_book_entry_at(const struct ab_book_map *m, int q, int c);
/* The entry holding book_ms in a blob's table (ab_ch_find()), with its
 * title copied into buf from THE SAME entry (owner 2026-10-08); an empty
 * title leaves buf "" (U draws "Chapter %d" with entry + 1, ⚠️6). */
int ab_book_entry(const struct audiobook_chapters *tab, uint32_t book_ms,
                  char *buf, int size);
/* Tick bitmap at AB_TICK_RES bits over len_ms: a bit for every start
 * after the first, strictly inside (0, len). Returns the bits set. */
int ab_book_ticks(const uint32_t *starts, size_t stride, int count,
                  uint32_t len_ms, uint8_t bits[AB_TICK_BYTES]);
/* Rescales an AB_TICK_RES bitmap to a bar of w pixels (w <= AB_TICK_RES):
 * pixel x is set if any source bit maps to it. Returns the bits set. */
int ab_ticks_scale(const uint8_t src[AB_TICK_BYTES], int w, uint8_t *dst);

/* Which "N of M" Now Playing shows (Rev 2, owner 2026-10-08) */
enum ab_counter_src {
    AB_CNT_NONE = 0,            /* no queue: blank */
    AB_CNT_QUEUE,               /* queue position (music, podcasts, a
                                   book file from a user playlist without
                                   chapters) */
    AB_CNT_FILE,                /* the file's chapters (P5: a chaptered
                                   book file outside a book queue, or a
                                   book whose map did not match) */
    AB_CNT_BOOK,                /* the book's entries (book queue, map
                                   matched): whole-book for multi-file */
};
enum ab_counter_src ab_counter_source(bool book_queue, bool map_ok,
                                      bool is_audiobook, bool is_podcast,
                                      int file_chapters, int queue_count);

/* Rev 2 / ⚠️13: at stop, a book within AB_FINISH_MS of its end is
 * finished (its resume spot is cleared). A book shorter than that is
 * finished only at its very end. len 0 = unknown: never. */
bool ab_finished(uint32_t book_ms, uint32_t len_ms);

/* ---- speed (§3.5, AB-2 owner-confirmed) --------------------------------- */
enum ab_speed {
    AB_SPEED_SLOWER = 0,        /* 0.75x */
    AB_SPEED_NORMAL = 1,        /* 1x, the default */
    AB_SPEED_FASTER = 2,        /* 1.25x */
    AB_SPEED_COUNT
};
#define AB_SPEED_DEFAULT  AB_SPEED_NORMAL
/* The setting is global_settings.ipodui_ab_speed (int, enum ab_speed;
 * settings_list.c under HAVE_IPOD_UI, cfg name "ipodui audiobook speed",
 * hidden from stock menus; B adds it). */

/* Factor in PITCH_SPEED_100 units (100 % = 10,000): 7,500 / 10,000 /
 * 12,500; out-of-range settings read as Normal. */
long ab_speed_factor(int speed);
/* Speed applies to audiobooks only (⚠️10): a book (id3->is_audiobook or
 * the database's TAGCACHE_UI_AUDIOBOOK flag, RAM only) that is not a
 * podcast. Everything else plays at PITCH_SPEED_100. */
long ab_speed_for_track(int speed, bool is_audiobook, bool is_podcast);

/* ---- runtime (B: ab_book.c, ab_speed.c, library.c; stub: ab_stub.c) ----- */
struct mp3entry;

/* Shell start, before any playback: allocates the book block, loads
 * books.dat's header and sector index (1 read of <= 64 KB). */
void ab_book_init(void);
/* The block (NULL before init). Pointers into it are valid until the
 * next yield (it is a movable core_alloc unless B pins it). UI thread. */
struct ab_book_block *ab_book_block(void);

/* Audio thread, codec track start (M's guarded hook in playback.c):
 * sets the time-stretch factor for this track (ab_speed_for_track() of
 * the setting), traced AB_TR_SPEED. Must not save global_status. */
void ab_speed_track_start(const struct mp3entry *id3);
/* UI thread: the setting changed (Settings > Audiobooks): applies at once
 * if a book is playing. */
void ab_speed_changed(void);

/* playback.c's audiobook auto-resume (Rev 1 item 1): true when the shell
 * owns resume for the queue (a book queue, lib_queue_is_book()), so
 * playback must not resume from tagcache stats. Audio thread, no I/O. */
bool ab_shell_owns_resume(void);

/* The opened book's page (U's Book list; §3.7, Rev 2). One page at a
 * time; the entries live in the library view pool (chapter view, "ab
 * view cap" traced). UI thread; storage per open: AB_TR_OPEN reads. */
int  ab_page_open(int view, int idx);   /* a LIB_AUDIOBOOKS row; returns
                                           the entry count (>= 1), or -1 */
bool ab_page_has_resume(void);          /* Rev 2: show "Resume" only if a
                                           saved spot exists */
int  ab_page_entries(void);
/* entry i's title into buf; "" = untitled (draw LANG "Chapter %d", i+1) */
const char *ab_page_entry_title(int i, char *buf, size_t len);
bool ab_page_resume(void);              /* play the book at its spot */
bool ab_page_play_entry(int i);         /* play the book from entry i */
ab_key_t ab_page_key(void);             /* #69: the open book's key (the
                                           book map's when it plays), 0 */
void ab_page_close(void);

/* ---- trace lines (UPDATE_TRACE(AB_TR_..., args)) ------------------------
 * Exact formats; check_trace.py (T) matches the text before the first
 * '%'. Keys print their low 32 bits: (unsigned long)(uint32_t)key. */
#define AB_TR_SLOT_W     "ab slot w=%d n=%d cut=%d"          /* slot,n,cut */
#define AB_TR_SLOT_READ  "ab slot read slot=%d ok=%d"        /* M/B reader */
#define AB_TR_PAK        "ab pak %s book=%08lx"              /* hit|miss */
#define AB_TR_OPEN       "ab open book=%08lx reads=%d"
#define AB_TR_MAP        "ab map src=%s book=%08lx files=%d entries=%d"
                                       /* pak|fallback|resumed|none */
#define AB_TR_MAP_MISS   "ab map mismatch q=%d"              /* path CRC */
#define AB_TR_VIEW_CAP   "ab view cap=%d"
#define AB_TR_STORE_W    "ab store w=%s book=%08lx ms=%lu"   /* now|idle */
#define AB_TR_STORE_EVICT "ab store evict book=%08lx"
#define AB_TR_SEED       "ab seed n=%d"
#define AB_TR_SPEED      "ab speed %d"                       /* percent */
#define AB_TR_RESUME     "ab resume book=%08lx file=%d ms=%lu"

#endif /* _IPODUI_AB_BOOK_H_ */
