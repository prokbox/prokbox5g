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
 * iPod UI shell: recoverable database generations
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
#ifndef IPODUI_DBGEN_H
#define IPODUI_DBGEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DBGEN_PATH_MAX 260
#define DBGEN_GENERATIONS 4     /* database_gen0..3 under the root */
#define DBGEN_MAX_FILES 32
#define DBGEN_SCRATCH_MIN 1024

enum dbgen_result {
    DBGEN_OK = 0,
    DBGEN_LEGACY,             /* Both manifests absent; root remains untouched. */
    DBGEN_MISSING,
    DBGEN_CORRUPT,
    DBGEN_IO,
    DBGEN_CANCELLED,
    DBGEN_INVALID,
    DBGEN_BUSY,              /* No unreferenced directory is available. */
    DBGEN_FOREIGN,           /* CRC-good manifest of another version/schema:
                              * never selected, but reserves its directory. */
    DBGEN_SALVAGED           /* select only: no usable manifest; out names a
                              * structurally validated directory instead. */
};

struct dbgen_file {
    const char *name;          /* Basename only; stable order across boots. */
    bool immutable;           /* Whole-file CRC; false for mutable master. */
    bool copy;                /* Copy during prepare (false for lib precompute). */
};

/* Called on the tagcache worker, never a UI draw/browse path. The caller
 * validates all private-format headers, lengths, clean master state, tag
 * offsets and matching libfile stamp/sections. Return a checksum of master
 * structural data only (exclude runtime statistics) through structural_sum
 * and the master's commit ID through found_commitid.
 * -1/-1 means discover the master stamp: used only to salvage a directory
 * when neither manifest is usable (never to trust a manifest generation).
 * Files are exclusively builder-owned while preparing/publishing. Callback
 * scratch passed through opaque MUST be disjoint from the helper's supplied
 * scratch: the latter contains live manifests as well as I/O workspace. */
typedef enum dbgen_result (*dbgen_validate_fn)(
    const char *path, int32_t commitid, int32_t entries,
    uint32_t *structural_sum, int32_t *found_commitid, void *opaque);

struct dbgen_ops {
    const struct dbgen_file *files;
    unsigned count;
    dbgen_validate_fn validate;
    /* Called between bounded I/O chunks: may yield; true cancels. */
    bool (*cancel)(void *opaque);
    void *opaque;
};

/* Paths are native filesystem paths. scratch belongs to this worker and is
 * reused for bulk I/O; no allocation/static bulk buffer is used here.
 * Two manifests retain complete generations; a third directory is scratch.
 * No operation removes or rewrites either retained generation.
 * When NEITHER manifest is usable (torn/foreign, no I/O ambiguity), select
 * salvages: the structurally validating directory (gen0-3 or legacy root)
 * with the highest commit ID is returned as DBGEN_SALVAGED. An intact
 * manifest's verdict is authoritative: with one present, CORRUPT returns.
 * A directory unreadable (IO) during salvage defers it: IO returns. */
enum dbgen_result dbgen_select(const char *root, char out[DBGEN_PATH_MAX],
    void *scratch, size_t bytes, const struct dbgen_ops *ops);

/* active is root for first migration, or the path returned by select.
 * Without an intact manifest (torn, or a salvaged library), only a
 * directory with no declared files is scratch, as for prepare_empty.
 * If ALL copied files are absent, prepare a fresh empty candidate. A partial
 * source database is never treated as empty. Every other copy
 * error fails; no partially copied candidate is selected or published.
 * The caller must not use out unless this returns DBGEN_OK. */
enum dbgen_result dbgen_prepare(const char *root, const char *active,
    char out[DBGEN_PATH_MAX], void *scratch, size_t bytes,
    const struct dbgen_ops *ops);

/* Explicit fresh recovery after definitive source-format failure. Clears
 * only the declared files in an unreferenced candidate; never clones or
 * changes legacy root, active, or either manifest-referenced directory.
 * Intact and foreign manifests reserve their directory even if bad.
 * A torn manifest may name any directory: then only a directory whose
 * declared files are ALL absent is used (gen0-3; gen3 is the last resort
 * otherwise, and an ordinary generation once published); with none, the
 * oldest one (lowest commit ID, a damaged one first) but the active. IO ambiguity always defers. The caller decides whether
 * a fresh scan is warranted. */
enum dbgen_result dbgen_prepare_empty(const char *root, const char *active,
    char out[DBGEN_PATH_MAX], void *scratch, size_t bytes,
    const struct dbgen_ops *ops);

/* The highest commit ID a valid manifest names, -1 without one: a fresh
 * build commits above every library there was (format change, damage). */
int32_t dbgen_newest_commit(const char *root, void *scratch, size_t bytes,
                            const struct dbgen_ops *ops);

/* Candidate must be the directory returned by prepare. Validates all files,
 * fsyncs them, writes/verifies an inactive manifest temporary file, then
 * publishes that manifest last. A failed return never authorizes switching
 * RAM snapshots; call select to reconcile an ambiguous close/rename error.
 * Only the pair's stale slot is removed, just before the final rename. */
enum dbgen_result dbgen_publish(const char *root, const char *candidate,
    int32_t commitid, int32_t entries, void *scratch, size_t bytes,
    const struct dbgen_ops *ops);

/* Split publication for callers with a short reader/statistics boundary.
 * Stage performs all bulk validation, file flushes and manifest readback.
 * Between stage and finish only runtime statistics excluded by validate's
 * structural checksum may change. Caller serializes generation writers.
 * Finish does ONE metadata rename, no bulk reads or allocations. */
struct dbgen_publication {
    char temporary[DBGEN_PATH_MAX], destination[DBGEN_PATH_MAX];
    bool ready;
};
enum dbgen_result dbgen_stage_publish(const char *root, const char *candidate,
    int32_t commitid, int32_t entries, void *scratch, size_t bytes,
    const struct dbgen_ops *ops, struct dbgen_publication *publication);
enum dbgen_result dbgen_finish_publish(struct dbgen_publication *publication);

/* Boot recovery (#91, before the tagcache thread selects): removes the
 * manifest temporaries a cut between stage and finish left. A cut inside
 * finish (stale slot removed, temporary not yet renamed) needs nothing
 * more: the other manifest names the previous complete generation. */
void dbgen_recover(const char *root);

#ifdef DBGEN_HOST
enum dbgen_test_op {
    DBGEN_TEST_OPEN, DBGEN_TEST_READ, DBGEN_TEST_WRITE, DBGEN_TEST_FLUSH,
    DBGEN_TEST_RENAME, DBGEN_TEST_CLOSE, DBGEN_TEST_SHORT_READ,
    DBGEN_TEST_SHORT_WRITE, DBGEN_TEST_RENAME_POST, DBGEN_TEST_REMOVE,
    DBGEN_TEST_NOPS
};
void dbgen_test_fail(enum dbgen_test_op op, uint64_t mask, int error);
void dbgen_test_reset(void);
#endif
#endif
