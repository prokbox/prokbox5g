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

#ifndef IPODUI_DBQUEUE_CORE_H
#define IPODUI_DBQUEUE_CORE_H

/* Queue by database index (#109, docs/design/queue-by-index.md): the pure
 * part. File formats, the T line's tag, the sentinel name, the resolve
 * mode table, the all-slot rebase decision, the capacity plan of
 * playlist_reserve_ipodui() and a key sort. No I/O, no allocation, no
 * locks: host-tested by tests/dbqueue. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A playlist word of a database entry: PLAYLIST_INSERT_TYPE_INSERT |
 * DBQ_FLAG | k (design 1.1). k is the slot's insertion number. */
#define DBQ_FLAG        0x08000000u
#define DBQ_SLOT        0x07FFFFFFu
#define DBQ_LOST        0xFFFFFFFFu     /* a slot that can't be resolved */

#define DBQ_MAX_BLOCKS  4
#define DBQ_PATH_LEN    32              /* "/.rockbox/database_gen123" fits */
#define DBQ_SEQ_MASK    0x00FFFFFFu

#define DBQ_BLOCK_MAGIC 0x44425131u     /* payload file head */
#define DBQ_HDR_MAGIC   0x44425148u     /* dbq.hdr */

enum dbq_mode {
    DBQ_MODE_IDX = 0,   /* master index in the stamp's generation */
    DBQ_MODE_SEEK,      /* filename-file seek in the stamp's generation */
    DBQ_MODE_PATH,      /* offset into dbqpath.dat */
};

/* Which generation a block's values belong to (design 4.1, L6) */
struct dbq_stamp {
    int32_t commitid;
    int32_t entries;
    char    db_path[DBQ_PATH_LEN];
};

/* Head of a block in dbq0.dat / dbq1.dat, then count x uint32 */
struct dbq_block {
    uint32_t magic;
    uint32_t seq;
    uint32_t count;
    uint32_t mode;
    uint32_t crc;       /* of the count x uint32 that follow */
};

/* One row of dbq.hdr's block table */
struct dbq_entry {
    uint8_t  file;      /* 0: dbq0.dat, 1: dbq1.dat */
    uint8_t  mode;      /* enum dbq_mode */
    uint16_t pad;
    uint32_t seq;
    uint32_t file_off;  /* where its struct dbq_block starts */
    uint32_t count;
    struct dbq_stamp st;
};

struct dbq_hdr {
    uint32_t magic;
    uint32_t nblocks;   /* e[0] is the current queue's, e[1] the previous */
    uint32_t next_seq;
    uint32_t cur_file;  /* the payload file e[0] lives in */
    struct dbq_entry e[DBQ_MAX_BLOCKS];
    uint32_t crc;       /* of everything before it */
};

/* The CRC used for both seals (firmware's crc_32()) */
uint32_t dbq_crc(const void *p, uint32_t len);

/* T:<first_k>:<count>:<tag>, tag = file << 24 | seq (design P2) */
static inline uint32_t dbq_tag(unsigned file, uint32_t seq)
{
    return ((uint32_t)(file & 1) << 24) | (seq & DBQ_SEQ_MASK);
}
static inline uint32_t dbq_tag_seq(uint32_t tag)  { return tag & DBQ_SEQ_MASK; }
static inline unsigned dbq_tag_file(uint32_t tag) { return (tag >> 24) & 1; }

/* The next queue's seq: never 0, never reused within 2^24 queues */
uint32_t dbq_seq_next(uint32_t seq);

void dbq_block_seal(struct dbq_block *b, uint32_t seq, uint32_t count,
                    uint32_t mode, const uint32_t *vals);
bool dbq_block_check(const struct dbq_block *b, const uint32_t *vals,
                     uint32_t seq, uint32_t count);
void dbq_hdr_seal(struct dbq_hdr *h);
bool dbq_hdr_check(const struct dbq_hdr *h);
/* The row a T line names, or NULL (the replay fails with -19): the seq
 * and the count must agree, and the slots must start at 0 (one block per
 * queue). The file a payload lives in is the row's, not the tag's: a mode
 * change moves it to the other file (design 5.1). */
const struct dbq_entry *dbq_hdr_find(const struct dbq_hdr *h, uint32_t tag,
                                     uint32_t first_k, uint32_t count);

/* The name of a slot that can't be resolved (H1): "/<dbq>/<k>". '<' and
 * '>' can't be in a FAT name, so file_exists() is false and playback
 * skips the one entry. Returns its length. */
int  dbq_sentinel(uint32_t k, char *buf, size_t len);
bool dbq_is_sentinel(const char *path);

enum dbq_why {
    DBQ_OK = 0,
    DBQ_WHY_DELETED,    /* FLAG_DELETED in its generation */
    DBQ_WHY_LOST,       /* DBQ_LOST, or no generation holds it */
    DBQ_WHY_IO,         /* a read failed or came back malformed */
    DBQ_WHY_BUSY,       /* no snapshot reference and no disk copy */
};
const char *dbq_why_name(int why);

/* Ends every resolve: why != DBQ_OK or an empty name writes the sentinel.
 * Always returns 0 (get_track_filename() must never fail, H1). */
int dbq_resolve_finish(int why, uint32_t k, char *buf, size_t len);

/* A filename tag file entry read back: hdr = {tag_length, idx_id}; name
 * holds 'got' bytes of its data. expect_idx DBQ_LOST: any. DBQ_OK when
 * the name is whole and NUL-terminated. */
int dbq_check_name(const int32_t hdr[2], uint32_t expect_idx,
                   const char *name, int got, size_t len);

/* How slot value v resolves (design 3.1). act: the snapshot in RAM
 * (NULL: none loaded or no reference); dir_holds: the stamp's directory
 * still holds that commit (checked on disk). */
enum dbq_res {
    DBQ_RES_LIVE,       /* IDX, the RAM snapshot is the stamp's */
    DBQ_RES_DISK,       /* IDX, from the stamp's master index on disk */
    DBQ_RES_SEEK,
    DBQ_RES_PATH,
    DBQ_RES_LOST,
};
bool dbq_stamp_eq(const struct dbq_stamp *a, const struct dbq_stamp *b);
enum dbq_res dbq_res_mode(int mode, uint32_t v, const struct dbq_stamp *blk,
                          const struct dbq_stamp *act, bool dir_holds);

/* The rebase over every slot, history included (H2, design 4.2.2).
 * fp(snap, idx): snap 0 = the block's (old) snapshot, 1 = the new one;
 * DBQ_FP_OK with *fp and *seek (the entry's filename seek), DBQ_FP_DELETED
 * (FLAG_DELETED there) or DBQ_FP_RANGE (idx past that snapshot's
 * entries: a rebuild that shrank, a mismatch, review M-D). */
enum { DBQ_FP_RANGE = -1, DBQ_FP_DELETED = 0, DBQ_FP_OK = 1 };
struct dbq_fp_ops {
    int  (*fp)(void *c, int snap, uint32_t idx, uint32_t *fp, int32_t *seek);
    void (*yield)(void *c);     /* may be NULL; every 256 slots */
    void *c;
};
enum dbq_rebase {
    DBQ_REBASE_STAMP,   /* every live slot matches: re-stamp, values kept */
    DBQ_REBASE_SEEK,    /* values are now the old snapshot's seeks */
};
enum dbq_rebase dbq_rebase(uint32_t *vals, uint32_t n,
                           const struct dbq_fp_ops *ops);
/* The same in pieces, so the runtime works in place on the slot table a
 * chunk at a time under its mutex (no transient, review M-A):
 * dbq_rebase_check(): slots deleted in the old snapshot become LOST;
 * true at the first mismatch. dbq_rebase_seek(): every slot to its old
 * filename seek, or LOST. */
bool dbq_rebase_check(uint32_t *vals, uint32_t n,
                      const struct dbq_fp_ops *ops);
void dbq_rebase_seek(uint32_t *vals, uint32_t n,
                     const struct dbq_fp_ops *ops);
/* The mode slot k resolves in while a SEEK conversion is under way:
 * slots below 'split' are converted already (IDX block only). */
static inline int dbq_slot_mode(int mode, uint32_t split, uint32_t k)
{
    return mode == DBQ_MODE_IDX && k < split ? DBQ_MODE_SEEK : mode;
}
/* playlist_resume()'s codes that mean "don't resume" (S1, review M-C):
 * -1 (nothing to resume) and -19 (a database block not confirmed). The
 * others (a torn last line, -15, ...) resume what was read, as upstream. */
static inline bool dbq_resume_refused(int rc)
{
    return rc == -1 || rc == -19;
}

/* The capacity playlist_reserve_ipodui() allocates (H3, H4): 0 = keep
 * what there is; -1 = refuse (keep, and the words don't fit). */
int dbq_reserve_target(int cur, int setting, int want, int amount, bool keep);
/* The words a reserve carries over: amount when keep, else none */
int dbq_reserve_copy(unsigned long *dst, const unsigned long *src,
                     int amount, int newcap, bool keep);

/* perm[0..n) = the slots ordered by key[] (DBQ_LOST last): one
 * sequential pass over a tag file. In place, no allocation. */
void dbq_sort_perm(uint32_t *perm, const uint32_t *key, uint32_t n);

#endif /* IPODUI_DBQUEUE_CORE_H */
