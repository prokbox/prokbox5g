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

/* GPL-2.0-or-later. Actual journal implementation over isolated real files. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "dbjournal.h"

static unsigned checks, fault_cases;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; \
} } while (0)
static unsigned char scratch[8192], saved[4096];
static char root[DBJOURNAL_ROOT_MAX];
static const char *names[] = { "/Music/Álbum/Track.flac", "/Music/álbum/Track.flac" };
struct values { int32_t v[2]; unsigned calls; uint64_t last; };
static enum dbjournal_result apply(void *opaque, const char *filename,
    uint32_t tag, int32_t value, uint64_t sequence)
{
    struct values *v = opaque;
    assert((tag & 0x3f) == 3 && sequence > v->last);
    int i;
    for (i = 0; i < 2 && strcmp(filename, names[i]); i++)
        ;
    assert(i < 2);
    v->v[i] = value;
    v->last = sequence;
    v->calls++;
    return DBJOURNAL_OK;
}
struct enumeration { unsigned i; };
struct bulk_enumeration { unsigned at, count; const char *filename; };
static int bulk_next(void *opaque, char *filename, size_t capacity,
                     uint32_t *tag, int32_t *value)
{
    struct bulk_enumeration *e = opaque;
    if (e->at == e->count) return 0;
    const char *name = e->filename ? e->filename : names[e->at & 1];
    assert(strlen(name) < capacity);
    strcpy(filename, name);
    *tag = 3;
    *value = e->at++ & 1 ? 90 : 20;
    return 1;
}
static enum dbjournal_result bulk_apply(void *opaque, const char *filename,
    uint32_t tag, int32_t value, uint64_t sequence)
{
    (void)sequence;
    struct bulk_enumeration *e = opaque;
    assert(tag == 3 && !strcmp(filename, e->filename));
    assert(value == (e->at & 1 ? 90 : 20));
    e->at++;
    return DBJOURNAL_OK;
}
static int next(void *opaque, char *filename, size_t capacity,
                 uint32_t *tag, int32_t *value)
{
    struct enumeration *e = opaque;
    if (e->i == 2)
        return 0;
    assert(strlen(names[e->i]) < capacity);
    strcpy(filename, names[e->i]);
    *tag = 3;
    *value = e->i++ ? 90 : 20;
    return 1;
}
static void filename(char *out, const char *leaf)
{ assert(snprintf(out, DBJOURNAL_ROOT_MAX, "%s/%s", root, leaf) < DBJOURNAL_ROOT_MAX); }
static void filename_unlink(const char *leaf);
static void clear(void)
{
    const char *leaves[] = { "database_stats.jnl", "database_stats.prev", "database_stats.tmp",
                             "database_stats.jnl.new", "database_stats.prev.new" };
    char p[DBJOURNAL_ROOT_MAX];
    for (unsigned i = 0; i < 5; i++) { filename(p, leaves[i]); unlink(p); }
}
static void filename_unlink(const char *leaf)
{ char p[DBJOURNAL_ROOT_MAX]; filename(p, leaf); unlink(p); }
static size_t save(void)
{
    char p[DBJOURNAL_ROOT_MAX]; filename(p, "database_stats.jnl");
    int fd = open(p, O_RDONLY); assert(fd >= 0);
    ssize_t n = read(fd, saved, sizeof(saved)); assert(n > 0);
    assert(close(fd) == 0); return (size_t)n;
}
static void restore(size_t n)
{
    clear();
    char p[DBJOURNAL_ROOT_MAX]; filename(p, "database_stats.jnl");
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666); assert(fd >= 0);
    assert(write(fd, saved, n) == (ssize_t)n && close(fd) == 0);
}
static void change_byte_in(const char *leaf, off_t offset)
{
    char p[DBJOURNAL_ROOT_MAX]; filename(p, leaf);
    int fd = open(p, O_RDWR); unsigned char b;
    assert(fd >= 0 && pread(fd, &b, 1, offset) == 1);
    b ^= 0x40;
    assert(pwrite(fd, &b, 1, offset) == 1 && close(fd) == 0);
}
static uint32_t frame_bytes(const char *name)
{ return (40 + (uint32_t)strlen(name) + 1 + 3) & ~3u; }
static bool exists(const char *leaf)
{ char p[DBJOURNAL_ROOT_MAX]; filename(p, leaf); return access(p, F_OK) == 0; }
/* Checkpoint in .prev (20/90), then 30/91 appended; the last frame's
 * payload is damaged, so open reports a full-length bad frame. */
static void corrupt_checkpoint(struct dbjournal *j)
{
    clear();
    filename_unlink("database_stats.bad");
    assert(dbjournal_open(root, j, scratch, sizeof(scratch)) == DBJOURNAL_MISSING);
    assert(dbjournal_append(j, names[0], 3, 10) == DBJOURNAL_OK);
    assert(dbjournal_append(j, names[1], 3, 90) == DBJOURNAL_OK);
    struct enumeration e = {0};
    assert(dbjournal_compact(j, next, &e) == DBJOURNAL_OK && j->previous);
    assert(dbjournal_append(j, names[0], 3, 30) == DBJOURNAL_OK);
    assert(dbjournal_append(j, names[1], 3, 91) == DBJOURNAL_OK);
    change_byte_in("database_stats.prev", j->end - frame_bytes(names[1]) + 41);
}
static void change_byte(off_t offset)
{
    char p[DBJOURNAL_ROOT_MAX]; filename(p, "database_stats.jnl");
    int fd = open(p, O_RDWR); unsigned char b;
    assert(fd >= 0 && pread(fd, &b, 1, offset) == 1);
    b ^= 0x40;
    assert(pwrite(fd, &b, 1, offset) == 1 && close(fd) == 0);
}

static bool cancel_after(void *opaque)
{
    unsigned *remaining = opaque;
    if (!*remaining) return true;
    --*remaining;
    return false;
}

static enum dbjournal_result append_tail(void *opaque, const char *filename,
    uint32_t tag, int32_t value, uint64_t sequence)
{
    (void)sequence;
    return dbjournal_append(opaque, filename, tag, value);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2 && strlen(argv[1]) < sizeof(root));
    strcpy(root, argv[1]);
    struct dbjournal j;
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_MISSING);
    CHECK(dbjournal_append(&j, names[0], 3, 10) == DBJOURNAL_OK);
    CHECK(dbjournal_append(&j, names[0], 3, 20) == DBJOURNAL_OK);
    CHECK(dbjournal_append(&j, names[1], 3, 90) == DBJOURNAL_OK);
    struct values v = {{0, 0}, 0, 0};
    CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK);
    CHECK(v.v[0] == 20 && v.v[1] == 90 && v.calls == 3);
    v.calls = 0; v.last = 0;
    CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK);
    CHECK(v.v[0] == 20 && v.v[1] == 90); /* Absolute, not duplicate increments. */
    size_t n = save();
    unsigned remaining = 1;
    struct dbjournal cancelled;
    CHECK(dbjournal_open_cancel(root, &cancelled, scratch, sizeof(scratch),
                                cancel_after, &remaining) == DBJOURNAL_CANCELLED);
    CHECK(!cancelled.ready);
    j.cancel = cancel_after;
    j.cancel_opaque = &remaining;
    v = (struct values){{0,0},0,0};
    CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_CANCELLED);
    struct enumeration cancelled_enum = {0};
    CHECK(dbjournal_compact(&j, next, &cancelled_enum) == DBJOURNAL_CANCELLED);
    CHECK(cancelled_enum.i == 0);
    j.cancel = NULL;
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
    v = (struct values){{0,0},0,0};
    CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK);
    CHECK(v.v[0] == 20 && v.v[1] == 90);
    uint64_t original_seq = j.seq;
    struct dbjournal_cursor cursor;
    CHECK(dbjournal_checkpoint(&j, &cursor) == DBJOURNAL_OK);
    struct dbjournal snapshot = j;
    CHECK(dbjournal_append(&j, names[0], 3, 30) == DBJOURNAL_OK);
    v = (struct values){{0,0},0,0};
    CHECK(dbjournal_replay(&snapshot, apply, &v) == DBJOURNAL_OK);
    CHECK(v.calls == 3 && v.v[0] == 20); /* Concurrent later append excluded. */
    v = (struct values){{20,90},0,original_seq};
    CHECK(dbjournal_replay_since(&j, &cursor, apply, &v) == DBJOURNAL_OK);
    CHECK(v.calls == 1 && v.v[0] == 30 && cursor.seq == j.seq);
    CHECK(dbjournal_replay_since(&j, &cursor, apply, &v) == DBJOURNAL_OK);
    CHECK(v.calls == 1);             /* No repeated scan/application. */
    struct enumeration e = {0};
    CHECK(dbjournal_compact(&j, next, &e) == DBJOURNAL_OK);
    CHECK(dbjournal_replay_since(&j, &cursor, apply, &v) == DBJOURNAL_INVALID);
    restore(n);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);

    /* Bulk staging never locks/changes the live journal. An append during
     * staging must be replayed before the bounded publication can finish. */
    unsigned char stage_scratch[8192];
    struct dbjournal source = j, staged, reopened;
    source.scratch = stage_scratch;
    struct enumeration stage_enum = {0};
    CHECK(dbjournal_checkpoint(&j, &cursor) == DBJOURNAL_OK);
    CHECK(dbjournal_compact_stage(&source, next, &stage_enum, &staged)
          == DBJOURNAL_OK);
    CHECK(dbjournal_open(root, &reopened, scratch, sizeof(scratch)) == DBJOURNAL_OK);
    CHECK(reopened.seq == j.seq && reopened.base == j.base);
    CHECK(dbjournal_append(&j, names[0], 3, 40) == DBJOURNAL_OK);
    CHECK(dbjournal_compact_finish(&j, &staged, &cursor) == DBJOURNAL_INVALID);
    CHECK(dbjournal_replay_since(&j, &cursor, append_tail, &staged) == DBJOURNAL_OK);
    CHECK(dbjournal_compact_finish(&j, &staged, &cursor) == DBJOURNAL_OK);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
    v = (struct values){{0,0},0,0};
    CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK);
    CHECK(v.v[0] == 40 && v.v[1] == 90);
    restore(n);

    /* L4: a compaction cut after its whole stage took its own name and the
     * inactive checkpoint went: open rolls the stage forward. A stage with
     * frames appended after it (a failed finish) goes instead. */
    for (int round = 0; round < 2; round++)
    {
        char tmpf[DBJOURNAL_ROOT_MAX], stagef[DBJOURNAL_ROOT_MAX];
        CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
        bool was_prev = j.previous;
        const char *inactive = was_prev ? "database_stats.jnl" : "database_stats.prev";
        const char *stage = was_prev ? "database_stats.jnl.new" : "database_stats.prev.new";
        struct enumeration se = {0};
        CHECK(dbjournal_checkpoint(&j, &cursor) == DBJOURNAL_OK);
        CHECK(dbjournal_compact_stage(&j, next, &se, &staged) == DBJOURNAL_OK);
        filename(tmpf, "database_stats.tmp");
        filename(stagef, stage);
        CHECK(rename(tmpf, stagef) == 0);
        if (round == 0)
            filename_unlink(inactive);
        else
            CHECK(dbjournal_append(&j, names[0], 3, 50) == DBJOURNAL_OK);
        dbjournal_recover(root);
        CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
        CHECK(!exists(stage) && !exists("database_stats.tmp"));
        v = (struct values){{0,0},0,0};
        CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK);
        if (round == 0)
            CHECK(j.previous != was_prev && j.seq == staged.seq
                  && v.v[0] == 20 && v.v[1] == 90);
        else
            CHECK(j.previous == was_prev && v.v[0] == 50);
    }
    restore(n);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);

    /* Torn final header is ignored and removed only before a new append. */
    char p[DBJOURNAL_ROOT_MAX]; filename(p, "database_stats.jnl");
    int fd = open(p, O_WRONLY | O_APPEND); CHECK(fd >= 0);
    CHECK(write(fd, "partial", 7) == 7 && close(fd) == 0);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
    CHECK(j.seq == original_seq);
    CHECK(dbjournal_append(&j, names[0], 3, 30) == DBJOURNAL_OK);
    v = (struct values){{0,0},0,0};
    CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK && v.v[0] == 30);

    /* Corrupted header lengths cannot masquerade as a cut payload. */
    restore(n); change_byte(24 + 8);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_CORRUPT);
    restore(n); change_byte(24 + 20);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_CORRUPT);
    restore(n);
    dbjournal_test_fail(DBJOURNAL_TEST_SHORT_READ, 1, EIO);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_IO);
    dbjournal_test_reset();

    /* Every append failure retains a replayable prefix, even when bytes
     * actually reached disk before a reported flush/close/readback error. */
    for (int op = 0; op < DBJOURNAL_TEST_NOPS; op++)
        for (unsigned bit = 0; bit < 10; bit++)
        {
            restore(n);
            CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
            dbjournal_test_fail(op, 1ull << bit, EIO);
            enum dbjournal_result rc = dbjournal_append(&j, names[0], 3, 30);
            dbjournal_test_reset();
            if (rc != DBJOURNAL_OK) fault_cases++;
            CHECK(rc == DBJOURNAL_OK || rc == DBJOURNAL_IO);
            CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
            v = (struct values){{0,0},0,0};
            CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK);
            CHECK((v.v[0] == 20 || v.v[0] == 30) && v.v[1] == 90);
        }
    /* A failed initial creation remains retryable, never corrupt primary. */
    for (int op = 0; op < DBJOURNAL_TEST_NOPS; op++)
        for (unsigned bit = 0; bit < 12; bit++)
        {
            clear();
            CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_MISSING);
            dbjournal_test_fail(op, 1ull << bit, ENOSPC);
            enum dbjournal_result rc = dbjournal_append(&j, names[0], 3, 20);
            dbjournal_test_reset();
            if (rc != DBJOURNAL_OK) fault_cases++;
            rc = dbjournal_open(root, &j, scratch, sizeof(scratch));
            CHECK(rc == DBJOURNAL_OK || rc == DBJOURNAL_MISSING);
            CHECK(dbjournal_append(&j, names[0], 3, 20) == DBJOURNAL_OK);
        }
    /* Compaction replaces only the inactive checkpoint, never the last
     * published source. Its absolute replay agrees with the old journal. */
    for (int op = 0; op < DBJOURNAL_TEST_NOPS; op++)
        for (unsigned bit = 0; bit < 14; bit++)
        {
            restore(n);
            CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
            struct enumeration e = {0};
            dbjournal_test_fail(op, 1ull << bit, EIO);
            enum dbjournal_result rc = dbjournal_compact(&j, next, &e);
            dbjournal_test_reset();
            if (rc != DBJOURNAL_OK) fault_cases++;
            CHECK(rc == DBJOURNAL_OK || rc == DBJOURNAL_IO);
            CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
            v = (struct values){{0,0},0,0};
            CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK);
            CHECK(v.v[0] == 20 && v.v[1] == 90);
        }
    restore(n);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
    /* 100 checkpoint records must be written in caller-buffer-sized
     * batches, not 100 small syscalls. Includes one file-header write. */
    struct bulk_enumeration bulk = { .count = 100 };
    dbjournal_test_reset();
    struct dbjournal bulk_staged;
    CHECK(dbjournal_compact_stage(&j, bulk_next, &bulk, &bulk_staged) == DBJOURNAL_OK);
    uint64_t write_calls, write_bytes;
    dbjournal_test_write_stats(&write_calls, &write_bytes);
    CHECK(bulk.at == 100 && write_calls > 1 && write_calls <= 5);
    CHECK(write_bytes == bulk_staged.end);

    /* Worst-length frames fit even at the 4 KiB minimum, without sharing
     * bytes with enumeration's filename in the second scratch half. */
    char longest[DBJOURNAL_PATH_MAX];
    memset(longest, 'a', sizeof(longest));
    longest[0] = '/'; longest[sizeof(longest) - 1] = '\0';
    j.bytes = DBJOURNAL_SCRATCH_MIN;
    bulk = (struct bulk_enumeration){ .count = 2, .filename = longest };
    CHECK(dbjournal_compact_stage(&j, bulk_next, &bulk, &bulk_staged) == DBJOURNAL_OK);
    bulk.at = 0;
    CHECK(dbjournal_replay(&bulk_staged, bulk_apply, &bulk) == DBJOURNAL_OK);
    CHECK(bulk.at == 2);
    j.bytes = sizeof(scratch);
    CHECK(dbjournal_append(&j, "\xc0\xaf", 3, 1) == DBJOURNAL_INVALID);
    CHECK(dbjournal_append(&j, "", 3, 1) == DBJOURNAL_INVALID);
    j.end = DBJOURNAL_MAX_BYTES;
    CHECK(dbjournal_append(&j, names[0], 3, 1) == DBJOURNAL_FULL);
    j.seq = UINT64_MAX;
    CHECK(dbjournal_append(&j, names[0], 3, 1) == DBJOURNAL_INVALID);

    /* One bad full frame must not make the journal unusable forever: the
     * file is set aside, the valid prefixes (older checkpoint first) are
     * merged, and the journal opens and appends again. */
    corrupt_checkpoint(&j);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_CORRUPT);
    CHECK(dbjournal_salvage(root, scratch, sizeof(scratch), NULL, NULL) == DBJOURNAL_OK);
    CHECK(exists("database_stats.bad") && !exists("database_stats.tmp"));
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
    v = (struct values){{0,0},0,0};
    CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK);
    CHECK(v.v[0] == 30 && v.v[1] == 90);
    CHECK(dbjournal_append(&j, names[1], 3, 92) == DBJOURNAL_OK);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
    v = (struct values){{0,0},0,0};
    CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK);
    CHECK(v.v[0] == 30 && v.v[1] == 92);
    /* Interrupted salvage: either the corrupt original remains (salvage
     * reruns) or the complete merge is in place. The prefix never goes. */
    unsigned salvage_faults = 0;
    for (int op = 0; op < DBJOURNAL_TEST_NOPS; op++)
        for (unsigned bit = 0; bit < 12; bit++)
        {
            corrupt_checkpoint(&j);
            dbjournal_test_fail(op, 1ull << bit, EIO);
            enum dbjournal_result rc = dbjournal_salvage(root, scratch,
                sizeof(scratch), NULL, NULL);
            dbjournal_test_reset();
            if (rc != DBJOURNAL_OK) salvage_faults++;
            CHECK(rc == DBJOURNAL_OK || rc == DBJOURNAL_IO);
            rc = dbjournal_open(root, &j, scratch, sizeof(scratch));
            if (rc == DBJOURNAL_CORRUPT)
            {
                CHECK(dbjournal_salvage(root, scratch, sizeof(scratch),
                                        NULL, NULL) == DBJOURNAL_OK);
                rc = dbjournal_open(root, &j, scratch, sizeof(scratch));
            }
            CHECK(rc == DBJOURNAL_OK);
            v = (struct values){{0,0},0,0};
            CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK);
            CHECK(v.v[0] == 30 && v.v[1] == 90);
        }
    /* A lone corrupt file keeps its prefix; a second salvage keeps only
     * the newest diagnostic copy. */
    clear();
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_MISSING);
    CHECK(dbjournal_append(&j, names[0], 3, 10) == DBJOURNAL_OK);
    CHECK(dbjournal_append(&j, names[0], 3, 20) == DBJOURNAL_OK);
    CHECK(dbjournal_append(&j, names[1], 3, 90) == DBJOURNAL_OK);
    change_byte_in("database_stats.jnl", j.end - frame_bytes(names[1]) + 41);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_CORRUPT);
    CHECK(dbjournal_salvage(root, scratch, sizeof(scratch), NULL, NULL) == DBJOURNAL_OK);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
    v = (struct values){{0,0},0,0};
    CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK);
    CHECK(v.v[0] == 20 && v.v[1] == 0 && v.calls == 2);
    /* Damaged header too: nothing salvageable, the journal starts fresh. */
    change_byte_in("database_stats.jnl", 0);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_CORRUPT);
    CHECK(dbjournal_salvage(root, scratch, sizeof(scratch), NULL, NULL) == DBJOURNAL_OK);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_MISSING);
    CHECK(exists("database_stats.bad"));
    CHECK(dbjournal_append(&j, names[0], 3, 40) == DBJOURNAL_OK);

    /* R-M4: the newest checkpoint damaged inside its checkpoint frames
     * (good header): only its valid prefix survives. The older file's
     * superseded value (50) must not come back for the rows past the bad
     * frame; RAM is re-checkpointed by the caller. */
    clear();
    filename_unlink("database_stats.bad");
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_MISSING);
    CHECK(dbjournal_append(&j, names[0], 3, 10) == DBJOURNAL_OK);
    CHECK(dbjournal_append(&j, names[1], 3, 50) == DBJOURNAL_OK);
    {
        struct enumeration e = {0};
        CHECK(dbjournal_compact(&j, next, &e) == DBJOURNAL_OK && j.previous);
    }
    change_byte_in("database_stats.prev", 24 + frame_bytes(names[0]) + 41);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_CORRUPT);
    CHECK(dbjournal_salvage(root, scratch, sizeof(scratch), NULL, NULL) == DBJOURNAL_OK);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
    v = (struct values){{0,0},0,0};
    CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK);
    CHECK(v.v[0] == 20 && v.v[1] == 0 && v.calls == 1);

    /* R-Low: the diagnostic copy is written once per corruption (a retry
     * after a failed salvage does not copy it again) and never into a
     * disk too short for it. */
    {
        uint64_t calls, first, again;
        corrupt_checkpoint(&j);
        dbjournal_test_reset();
        dbjournal_test_fail(DBJOURNAL_TEST_RENAME, 1, EIO);
        CHECK(dbjournal_salvage(root, scratch, sizeof(scratch), NULL, NULL) == DBJOURNAL_IO);
        dbjournal_test_write_stats(&calls, &first);
        dbjournal_test_reset();
        CHECK(exists("database_stats.bad"));
        CHECK(dbjournal_salvage(root, scratch, sizeof(scratch), NULL, NULL) == DBJOURNAL_OK);
        dbjournal_test_write_stats(&calls, &again);
        CHECK(again + j.end <= first);      /* no second copy of the file */
        corrupt_checkpoint(&j);             /* removes the old copy */
        dbjournal_test_free_bytes(1024 * 1024);
        CHECK(dbjournal_salvage(root, scratch, sizeof(scratch), NULL, NULL) == DBJOURNAL_OK);
        CHECK(!exists("database_stats.bad"));
        dbjournal_test_free_bytes(UINT64_MAX);
        CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
        v = (struct values){{0,0},0,0};
        CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK);
        CHECK(v.v[0] == 30 && v.v[1] == 90);
    }

    /* Round 4: a version-1 journal (absolute frames only, written by the
     * earlier #85 builds) still opens, replays and takes version-2 frames,
     * whose tags may carry the caller's flags. */
    clear();
    dbjournal_test_version(1);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_MISSING);
    CHECK(dbjournal_append(&j, names[0], 3, 10) == DBJOURNAL_OK);
    dbjournal_test_version(2);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
    CHECK(dbjournal_append(&j, names[1], 3 | 0x80, 5) == DBJOURNAL_OK);
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_OK);
    v = (struct values){{0,0},0,0};
    CHECK(dbjournal_replay(&j, apply, &v) == DBJOURNAL_OK);
    CHECK(v.calls == 2 && v.v[0] == 10);
    dbjournal_test_version(3);      /* a future format is not read */
    clear();
    CHECK(dbjournal_open(root, &j, scratch, sizeof(scratch)) == DBJOURNAL_MISSING);
    CHECK(dbjournal_append(&j, names[0], 3, 10) == DBJOURNAL_INVALID
          || dbjournal_open(root, &j, scratch, sizeof(scratch)) != DBJOURNAL_OK);
    dbjournal_test_version(2);
    clear();

    printf("ok dbjournal: %u checks, %u actual injected failures, %u interrupted salvages; absolute replay retained\n",
           checks, fault_cases, salvage_faults);
    return 0;
}
