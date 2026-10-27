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
 * iPod UI shell: stable-identity absolute statistics journal
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
#ifndef IPODUI_DBJOURNAL_H
#define IPODUI_DBJOURNAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DBJOURNAL_ROOT_MAX 260
#define DBJOURNAL_PATH_MAX 1024
#define DBJOURNAL_SCRATCH_MIN 4096
#ifndef DBJOURNAL_MAX_BYTES
#define DBJOURNAL_MAX_BYTES (128u * 1024u * 1024u)
#endif

enum dbjournal_result {
    DBJOURNAL_OK = 0, DBJOURNAL_MISSING, DBJOURNAL_CORRUPT,
    DBJOURNAL_IO, DBJOURNAL_INVALID, DBJOURNAL_FULL, DBJOURNAL_CANCELLED
};

/* Caller serializes ALL operations (including replay callbacks) and owns
 * scratch for the lifetime of an open context. No heap or permanent bulk
 * buffer. Callbacks must not reenter this journal or borrow its scratch.
 * File handles are closed before returning; close only forgets the context.
 * Canonical full filenames are identity, NOT destinations: preserve exact
 * UTF8 bytes/case/segments; never hash-match/truncate them. */
struct dbjournal {
    const char *root;          /* the caller's; must outlive the context */
    uint64_t base, seq;
    uint32_t end;              /* Bounded journal byte offset, not sequence. */
    void *scratch;
    size_t bytes;
    bool ready, previous, staged;
    bool (*cancel)(void *opaque);
    void *cancel_opaque;
};

typedef enum dbjournal_result (*dbjournal_apply_fn)(void *opaque,
    const char *filename, uint32_t tag, int32_t absolute, uint64_t sequence);

/* next: 1 emits one current absolute statistic; 0 ends; -1 fails.
 * filename must fit in capacity INCLUDING NUL, never be truncated. The
 * caller enumerates the active snapshot's current statistics under its lock.
 * Items absent from the active snapshot intentionally leave the journal.
 * New sequence numbers remain monotonic after compaction. */
typedef int (*dbjournal_next_fn)(void *opaque, char *filename, size_t capacity,
                                uint32_t *tag, int32_t *absolute);

enum dbjournal_result dbjournal_open(const char *root, struct dbjournal *j,
                                     void *scratch, size_t bytes);
/* Cancellation is checked between bounded records; true defers safely.
 * Callback must not reenter this journal or service USB with its fd open. */
enum dbjournal_result dbjournal_open_cancel(const char *root,
    struct dbjournal *j, void *scratch, size_t bytes,
    bool (*cancel)(void *opaque), void *opaque);
void dbjournal_close(struct dbjournal *j);
/* Boot recovery (#91), before the tagcache thread opens the journal: a
 * checkpoint set aside by a cut replace goes back under its name, and a
 * leftover temporary goes (an incomplete stage never outlives a boot). A
 * compaction cut after its stage was complete leaves it under its own
 * name; open rolls it forward when it is newer than both checkpoints (L4). */
void dbjournal_recover(const char *root);
/* After open returned CORRUPT: rebuild a valid journal from the validated
 * prefixes of both checkpoint files (older first; frames keep their
 * sequence numbers), set each corrupt file aside as database_stats.bad
 * (only the newest is kept), then publish the merged copy under the name
 * the newest checkpoint had. A later open succeeds (OK or MISSING). Frames
 * after a bad one are lost from disk; the caller re-checkpoints from RAM. */
enum dbjournal_result dbjournal_salvage(const char *root, void *scratch,
    size_t bytes, bool (*cancel)(void *opaque), void *opaque);
enum dbjournal_result dbjournal_append(struct dbjournal *j,
    const char *filename, uint32_t tag, int32_t absolute);
enum dbjournal_result dbjournal_replay(struct dbjournal *j,
    dbjournal_apply_fn apply, void *opaque);
/* replay reads only the validated prefix captured in j->end/seq. A copied
 * context can be replayed unlocked while the serialized live context appends
 * (never compacts/truncates that prefix); use separate scratch for the copy. */
enum dbjournal_result dbjournal_compact(struct dbjournal *j,
    dbjournal_next_fn next, void *opaque);
struct dbjournal_cursor;
/* Stage from a coherent RAM snapshot captured AFTER draining its queued
 * prefix. source has private scratch; live appends may continue. staged
 * inherits that scratch and is appendable, but is NOT selected by open.
 * Replay newer live events into staged with separate reader scratch before
 * finish. The final cursor must cover the entire live journal. */
enum dbjournal_result dbjournal_compact_stage(struct dbjournal *source,
    dbjournal_next_fn next, void *opaque, struct dbjournal *staged);
enum dbjournal_result dbjournal_compact_finish(struct dbjournal *live,
    const struct dbjournal *staged, const struct dbjournal_cursor *cursor);

/* Cursor is captured only after a complete validated replay. Since it was
 * captured, the caller allows append only, not prefix writes. A different
 * checkpoint/compaction invalidates it; replay the new checkpoint unlocked
 * before trying a short final publication boundary again. */
struct dbjournal_cursor {
    uint64_t base, seq;
    uint32_t end;
    bool previous;
};
enum dbjournal_result dbjournal_checkpoint(const struct dbjournal *j,
                                          struct dbjournal_cursor *cursor);
enum dbjournal_result dbjournal_replay_since(struct dbjournal *j,
    struct dbjournal_cursor *cursor, dbjournal_apply_fn apply, void *opaque);

/* MISSING from open means no journal or prior journal existed; the context
 * is ready for a first append. Incomplete trailing frames are ignored, then
 * removed before appending. A full-length bad frame is corruption even at
 * EOF; never skip it and accidentally discard later updates. Valid previous
 * is recoverable when primary is missing/corrupt, but I/O ambiguity defers.
 * FULL means compact from the active state then retry; no automatic evict.
 * Failed append may have reached disk: after reopening, seq says whether
 * it did (the caller must not append it again then: a delta frame adds).
 * Replaying a frame twice is NOT idempotent for delta frames. */

#ifdef DBJOURNAL_HOST
enum dbjournal_test_op {
    DBJOURNAL_TEST_OPEN, DBJOURNAL_TEST_READ, DBJOURNAL_TEST_WRITE,
    DBJOURNAL_TEST_FLUSH, DBJOURNAL_TEST_RENAME, DBJOURNAL_TEST_TRUNCATE,
    DBJOURNAL_TEST_SHORT_READ, DBJOURNAL_TEST_SHORT_WRITE,
    DBJOURNAL_TEST_CLOSE, DBJOURNAL_TEST_NOPS
};
void dbjournal_test_reset(void);
void dbjournal_test_fail(enum dbjournal_test_op op, uint64_t mask, int error);
void dbjournal_test_write_stats(uint64_t *calls, uint64_t *bytes);
void dbjournal_test_free_bytes(uint64_t bytes);     /* default: unlimited */
void dbjournal_test_version(uint32_t version);      /* write an old format */
#endif
#endif
