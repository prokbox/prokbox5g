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
 * Production tagcache compaction/queue caller, host platform shims only.
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
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dbjournal.h"

#define ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX_PATH 260
#define TAGCACHE_BUFSZ 1024
#define TAGCACHE_COMMAND_QUEUE_LENGTH 8
#define FLAG_DELETED 1
#define FLAG_DIRTYNUM 4
#define FLAG_UI_STATS_PENDING 0x0800
#define FLAG_UI_STATS_FOLDED 0x2000
#define FLAG_UI_STATS_LIVE(slot) (0x10000 << (slot))
#define FLAG_UI_STATS_LIVE_ALL (0x3f << 16)
enum { tag_filename, tag_playcount, tag_rating, tag_playtime,
       tag_lastplayed, tag_lastelapsed, tag_lastoffset, TAG_COUNT };
#define TAGCACHE_IS_NUMERIC(t) ((t) > tag_filename && (t) < TAG_COUNT)
enum { CMD_UPDATE_NUMERIC, CMD_UPDATE_MASTER_HEADER };
struct index_entry { int32_t tag_seek[TAG_COUNT]; int32_t flag; };
#ifdef RECOVERY_BIG
#define ROWS 50000          /* the checkpoint measure: a 50k library */
#else
#define ROWS 2
#endif
struct ram_header { struct index_entry indices[ROWS]; };
struct master_header { struct { int entry_count; } tch; long serial; };
struct tagcache_context {
    struct master_header master;
    struct { bool ramcache; int queue_length; } stat;
    struct { struct ram_header *hdr; } ram;
    unsigned refs;
    int generation;
};
struct tagcache_command_entry {
    int command, idx_id, tag;
    long data;
    struct tagcache_context *context;
};
static struct tagcache_command_entry command_queue[TAGCACHE_COMMAND_QUEUE_LENGTH];
static int command_queue_ridx, command_queue_widx, command_queue_mutex;
#define COMMAND_QUEUE_IS_EMPTY (command_queue_ridx == command_queue_widx)
static struct tagcache_context context, *ui_active = &context;
static struct ram_header ram;
static struct dbjournal ui_stats_journal;
static bool ui_stats_restart, ui_stats_recovering, ui_commit_cancel;
static bool ui_boot_stats_replayed = true;
static unsigned ui_stats_dropped, ui_stats_dropped_logged;
static uint64_t ui_append_maybe;
static bool ui_journal_full, ui_candidate_changed;
/* MED-B: a candidate being built (ui_build != ui_active) */
static struct tagcache_context cand;
static struct tagcache_context *ui_build = &context;

static bool ui_shutdown_flushing;
static bool ui_stats_live;
static volatile bool ui_stats_replaying;
#define TAGCACHE_UI_DELTA 0x80
static int disable_irq_save(void) { return 0; }
static void restore_irq(int level) { (void)level; }
static struct tagcache_context *ui_import_ctx;
#define UI_IMPORT_CTX (ui_import_ctx ? ui_import_ctx : ui_build)
#define TAGCACHE_UI_REL 0x40
#define UI_STAT_TAG(t) ((t) & 0x3f)
#define FLAG_UI_STATS_REPLAYED 0x400000
static volatile bool ui_drain_due, ui_drain_force;
static bool on_worker = true;    /* R-H2: journal work on the worker only */
static unsigned compact_stages;
static bool ui_on_worker(void) { return on_worker; }
static bool disk_active;
static bool storage_disk_is_active(void) { return disk_active; }
static int ui_stat_slot(int tag)
{ return tag >= tag_playcount && tag <= tag_lastoffset ? tag - tag_playcount : -1; }
static bool tagcache_usb_quiescing;
static int lock_depth, stage_inject, waited, playback;
static size_t free_bytes = 1024 * 1024;
static unsigned checks, stages, regrows, fault_cases, filename_reads;
static unsigned char open_buffer[8192];
static const char *names[] = { "/Music/Álbum/song.flac", "/Music/Other/song.flac" };
static char journal_root[DBJOURNAL_ROOT_MAX];
static void queue_command(int cmd, long idx_id, int tag, long data);
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL recovery line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)
static void ui_log(const char *category, const char *format, ...)
{ (void)category; (void)format; }
static void mutex_lock(int *mutex)
{ (void)mutex; CHECK(lock_depth == 0); lock_depth++; }
static void mutex_unlock(int *mutex)
{ (void)mutex; CHECK(lock_depth == 1); lock_depth--; }
static __attribute__((unused)) void test_sleep(int ticks)
{ (void)ticks; waited++; CHECK(!"queue blocked instead of coalescing"); }
#define sleep test_sleep
static int audio_status(void) { return playback; }
static size_t core_allocatable(void) { return free_bytes; }
static bool ui_generation_cancel(void *opaque)
{ (void)opaque; return ui_commit_cancel || tagcache_usb_quiescing; }
static struct tagcache_context *tagcache_context_acquire_passive(void)
{ context.refs++; return &context; }
static void tagcache_context_release_passive(struct tagcache_context *ctx)
{ CHECK(ctx->refs > 0); ctx->refs--; }
static bool ui_context_ref(struct tagcache_context *ctx, bool operation)
{ (void)operation; ctx->refs++; return true; }
static void ui_context_put(struct tagcache_context *ctx, bool operation)
{ (void)operation; CHECK(ctx->refs > 0); ctx->refs--; }
static const int buflib_ops_locked;
static void *handles[16];
static int core_alloc_ex(size_t bytes, const void *ops)
{
    (void)ops;
    CHECK(lock_depth == 0);
    for (unsigned h = 1; h < ARRAYLEN(handles); h++)
        if (!handles[h]) { handles[h] = calloc(1, bytes); return handles[h] ? (int)h : -1; }
    return -1;
}
static void *core_get_data(int h) { CHECK(h > 0 && h < 16); return handles[h]; }
static void core_free(int h)
{ CHECK(!lock_depth && handles[h] != NULL); free(handles[h]); handles[h] = NULL; }
struct ui_generation_io { void *buffer; size_t size; };
static int ui_generation_buffer(struct ui_generation_io *io)
{
    int h = core_alloc_ex(16384, NULL);
    if (h > 0) { io->buffer = handles[h]; io->size = 16384; }
    return h;
}
static void audio_buffer_regrow(void) { regrows++; }
static bool ui_entry_filename(struct tagcache_context *ctx, int row,
                              char *filename, size_t capacity)
{
    CHECK(ctx == &context && row >= 0 && row < ROWS);
    filename_reads++;
    if (row >= 2)
    {
        snprintf(filename, capacity, "/Music/t%05d.flac", row);
        return true;
    }
    CHECK(strlen(names[row]) < capacity);
    if (!lock_depth)
    {
        stages++;
        if (stage_inject == 1)
        {
            stage_inject = 0;
            queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, 3);
        }
        else if (stage_inject == 2)
        {
            stage_inject = 0;
            ui_commit_cancel = true;
        }
        else if (stage_inject == 3)
            queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount,
                          ram.indices[0].tag_seek[tag_playcount] + 1);
        else if (stage_inject == 4)
        {
            stage_inject = 0;
            tagcache_usb_quiescing = true;
        }
    }
    strcpy(filename, names[row]);
    return true;
}
#define ui_db_root journal_root
static bool ui_reload_pending, ui_busy;
/* the master on disk, read in bulk by the checkpoint and the restore */
static struct index_entry disk[ROWS];
static long seek_pos;
static int fail_read_index;     /* > 0: that read fails */
static off_t test_lseek(int fd, off_t pos, int whence)
{ (void)fd; (void)whence; seek_pos = pos; return pos; }
static ssize_t read_index_entries(struct tagcache_context *c, int fd,
                                  struct index_entry *e, size_t n)
{
    (void)c; (void)fd;
    long row = (seek_pos - (long)sizeof(struct master_header)) / (long)sizeof(*e);
    if (fail_read_index && !--fail_read_index)
        return -1;
    CHECK(row >= 0 && row + (long)n <= ROWS);
    memcpy(e, &disk[row], n * sizeof(*e));
    return n * sizeof(*e);
}
static bool fail_open_master;
static unsigned master_opens;
static int open_master_fd(struct tagcache_context *c, struct master_header *h,
                          bool write)
{
    (void)c; (void)write; memset(h, 0, sizeof(*h));
    master_opens++;
    return fail_open_master ? -1 : dup(0);
}
enum { UI_RETRY_JOURNAL, UI_RETRY_REPLAY, UI_RETRY_COMPACT, UI_RETRY_COUNT };
static struct { long next, delay; } ui_retry[UI_RETRY_COUNT];
static unsigned retry_calls;
static bool ui_retry_due(int which) { return !ui_retry[which].delay; }
static void ui_retry_later(int which, const char *what, int rc)
{ (void)what; (void)rc; ui_retry[which].delay = 1; retry_calls++; }
/* Check the scheduling contract while executing the real implementation:
 * bulk staging and staged tails outside the producer lock, one publication
 * boundary inside it. Ordinary live queue drain is intentionally serialized. */
static enum dbjournal_result checked_stage(struct dbjournal *source,
    dbjournal_next_fn next, void *opaque, struct dbjournal *staged)
{
    CHECK(lock_depth == 0);
    compact_stages++;
    return dbjournal_compact_stage(source, next, opaque, staged);
}
static enum dbjournal_result checked_append(struct dbjournal *j,
    const char *filename, uint32_t tag, int32_t absolute)
{
    if (j->staged) CHECK(lock_depth == 0);
    return dbjournal_append(j, filename, tag, absolute);
}
static enum dbjournal_result checked_finish(struct dbjournal *live,
    const struct dbjournal *staged, const struct dbjournal_cursor *cursor)
{
    CHECK(lock_depth == 1);
    return dbjournal_compact_finish(live, staged, cursor);
}
#define dbjournal_compact_stage checked_stage
#define dbjournal_append checked_append
#define dbjournal_compact_finish checked_finish
#define lseek test_lseek
#include "recovery_actual.inc"
#undef lseek
#undef dbjournal_compact_stage
#undef dbjournal_append
#undef dbjournal_compact_finish

/* The boot replay (ui_stats_apply) against the active snapshot, and a
 * candidate's replay (MED-A) into its own master: 'cand_disk', whose rows
 * are in the other order (a rebuild renumbers them) */
struct ui_import { int masterfd; };
static struct index_entry cand_disk[2];
static bool write_index(struct tagcache_context *c, int fd, int id,
                        struct index_entry *idx)
{
    (void)fd;
    CHECK(c == &cand && id >= 0 && id < 2);
    cand_disk[id] = *idx;
    return true;
}
static bool ui_import_lookup_io_failed;
static bool ui_runtime_tag(int tag) { return ui_stat_slot(tag) >= 0; }
static int fail_lookup_after = -1;          /* replay I/O failure */
static long ui_import_lookup(const struct ui_import *imp, const char *name,
                             struct index_entry *idx)
{
    (void)imp;
    if (fail_lookup_after >= 0 && !fail_lookup_after--)
    {
        ui_import_lookup_io_failed = true;
        return -1;
    }
    for (long row = 0; row < 2; row++)
        if (!strcmp(name, names[row]))
        {
            if (UI_IMPORT_CTX == &cand)
            {
                *idx = cand_disk[1 - row];
                return 1 - row;
            }
            *idx = disk[row];
            return row;
        }
    return -1;
}
static struct ui_import ui_stats_lookup = { 3 };
#define lseek test_lseek
#include "replay_actual.inc"
#undef lseek

/* The boot replay as ui_candidate_stats_prepare() runs it: its drain,
 * then ui_stats_replay_boot() over the master's loaded serial
 * then the replay under ui_stats_replaying, then its end */
static long loaded_serial;      /* the master header's, at load */
static bool boot_replay(void)
{
    mutex_lock(&command_queue_mutex);
    bool ok = ui_command_drain_locked(open_buffer, sizeof(open_buffer), true);
    struct dbjournal snapshot = ui_stats_journal;
    ui_stats_journal.scratch = NULL;
    mutex_unlock(&command_queue_mutex);
    unsigned char scratch[8192];
    snapshot.scratch = scratch;
    snapshot.bytes = sizeof(scratch);
    ui_import_ctx = ui_active;
    ui_stats_replaying = true;
    ok = ui_stats_replay_boot(&snapshot, ok, loaded_serial);
    ui_import_ctx = NULL;
    return ok;
}

struct observed { int32_t values[2][TAG_COUNT]; uint64_t sequence; int32_t serial; };
static enum dbjournal_result replay(void *opaque, const char *filename,
    uint32_t tag, int32_t value, uint64_t sequence)
{
    struct observed *o = opaque;
    unsigned t = UI_STAT_TAG(tag);
    CHECK(sequence > o->sequence && t < TAG_COUNT);
    o->sequence = sequence;
    for (unsigned row = 0; row < 2; row++)
        if (!strcmp(filename, names[row]))
        {
            /* the same rules as ui_stats_apply(): absolutes set, deltas
             * add, relative lastplayed goes after every one so far */
            if (tag & TAGCACHE_UI_DELTA)
                value += o->values[row][t];
            else if (tag & TAGCACHE_UI_REL)
                value += o->serial;
            if (t == tag_lastplayed && value >= o->serial)
                o->serial = value + 1;
            /* Play counts in these fixtures never go down: the old queue's
             * 1/2 must never land after the captured newer value 3. */
            if (row == 0 && t == tag_playcount)
                CHECK(value >= o->values[row][t]);
            o->values[row][t] = value;
            return DBJOURNAL_OK;
        }
    CHECK(!"unexpected filename");
    return DBJOURNAL_INVALID;
}
static void reset(void)
{
    const char *leaves[] = { "database_stats.jnl", "database_stats.prev", "database_stats.tmp" };
    for (unsigned i = 0; i < ARRAYLEN(leaves); i++)
    {
        char path[512];
        CHECK(snprintf(path, sizeof(path), "%s/%s", journal_root, leaves[i]) < (int)sizeof(path));
        unlink(path);
    }
    memset(&ram, 0, sizeof(ram));
    memset(disk, 0, sizeof(disk));
    ui_stats_live = ui_stats_replaying = false;
    fail_lookup_after = -1;
    fail_read_index = 0;
    fail_open_master = false;
    loaded_serial = 0;
    ui_append_maybe = 0;
    ui_journal_full = false;
    ui_build = &context;
    ui_reload_pending = ui_busy = false;
    context.generation = 0;
    memset(&context, 0, sizeof(context));
    memset(command_queue, 0, sizeof(command_queue));
    command_queue_ridx = command_queue_widx = 0;
    context.master.tch.entry_count = 2;
    context.stat.ramcache = true;
    context.ram.hdr = &ram;
    ui_stats_restart = ui_stats_recovering = ui_commit_cancel = false;
    tagcache_usb_quiescing = false;
    ui_boot_stats_replayed = true;
    ui_stats_dropped = ui_stats_dropped_logged = 0;
    memset(ui_retry, 0, sizeof(ui_retry));
    retry_calls = 0;
    stage_inject = waited = stages = 0;
    playback = 0;
    free_bytes = 1024 * 1024;
    dbjournal_test_reset();
    CHECK(dbjournal_open_cancel(journal_root, &ui_stats_journal,
                                open_buffer, sizeof(open_buffer),
                                ui_generation_cancel, NULL) == DBJOURNAL_MISSING);
}
static void fill(void)
{
    enum dbjournal_result rc;
    do { rc = dbjournal_append(&ui_stats_journal, names[1], tag_rating, 0); }
    while (rc == DBJOURNAL_OK);
    CHECK(rc == DBJOURNAL_FULL);
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, 1);
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, 2);
    mutex_lock(&command_queue_mutex);
    CHECK(!ui_command_drain_locked(open_buffer, sizeof(open_buffer), false));
    mutex_unlock(&command_queue_mutex);
    CHECK(ui_stats_recovering && ram.indices[0].tag_seek[tag_playcount] == 2);
}
static struct observed observe(void)
{
    struct dbjournal reopened;
    unsigned char scratch[8192];
    CHECK(dbjournal_open(journal_root, &reopened, scratch, sizeof(scratch)) == DBJOURNAL_OK);
    struct observed o = {0};
    CHECK(dbjournal_replay(&reopened, replay, &o) == DBJOURNAL_OK);
    return o;
}
static int32_t durable(void)
{
    struct dbjournal reopened;
    unsigned char scratch[8192];
    CHECK(dbjournal_open(journal_root, &reopened, scratch, sizeof(scratch)) == DBJOURNAL_OK);
    struct observed o = {0};
    CHECK(dbjournal_replay(&reopened, replay, &o) == DBJOURNAL_OK);
    return o.values[0][tag_playcount];
}
int main(int argc, char **argv)
{
    CHECK(argc == 2 && strlen(argv[1]) < sizeof(journal_root));
    strcpy(journal_root, argv[1]);
#ifdef RECOVERY_BIG
    /* (built with a 1 MiB journal: 300 frames do not fit in 4 KiB) */
    /* More than 256 tracks before the replay (drained as they come): all
     * counted, the play count never lowered on the way (observer). */
    reset();
    ui_boot_stats_replayed = false;
    disk[0].tag_seek[tag_playcount] = ram.indices[0].tag_seek[tag_playcount] = 3;
    CHECK(dbjournal_append(&ui_stats_journal, names[0], tag_playcount, 10) == DBJOURNAL_OK);
    for (int i = 0; i < 300; i++)
    {
        queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount | TAGCACHE_UI_DELTA, 1);
        if (i % 4 == 3)
            ui_stats_drain();
    }
    ui_stats_drain();
    CHECK(COMMAND_QUEUE_IS_EMPTY && !ui_stats_dropped && durable() == 310);
    CHECK(boot_replay() && ram.indices[0].tag_seek[tag_playcount] == 310);

    /* MED-5: a recovery checkpoint of a 50k library with 1,000 played
     * tracks writes only the values that differ from the master on disk
     * (play count, play time, lastplayed: 3 per played track), not 6 per
     * row (300,000 frames, ~19 MB with these paths). */
    reset();
    context.master.tch.entry_count = ROWS;
    for (int row = 0; row < ROWS; row++)
    {
        disk[row].tag_seek[tag_rating] = ram.indices[row].tag_seek[tag_rating] = row % 11;
        disk[row].tag_seek[tag_playcount] = ram.indices[row].tag_seek[tag_playcount] = row % 7;
    }
    for (int row = 0; row < ROWS; row += ROWS / 1000)
    {
        ram.indices[row].tag_seek[tag_playcount] += 1;
        ram.indices[row].tag_seek[tag_playtime] += 180000;
        ram.indices[row].tag_seek[tag_lastplayed] = 1000 + row;
    }
    ui_stats_recovering = true;
    filename_reads = 0;
    ui_journal_compact_worker();
    CHECK(!ui_stats_recovering);
    {
        struct dbjournal reopened;
        static unsigned char scratch[65536];
        CHECK(dbjournal_open(journal_root, &reopened, scratch, sizeof(scratch)) == DBJOURNAL_OK);
        printf("checkpoint: %lu frames, %lu bytes, %u filename reads for %d rows, 1000 played\n",
               (unsigned long)(reopened.seq - reopened.base),
               (unsigned long)reopened.end, filename_reads, ROWS);
        CHECK(reopened.seq - reopened.base == 3000 && filename_reads == 1000);
    }
    printf("production journal, 300 plays before the replay: %u checks\n", checks);
    return 0;
#endif
    reset(); fill();
    stage_inject = 1;
    ui_journal_compact_worker();
    CHECK(!ui_stats_recovering && COMMAND_QUEUE_IS_EMPTY && !waited);
    CHECK(context.refs == 0 && context.stat.queue_length == 0);
    CHECK(ram.indices[0].tag_seek[tag_playcount] == 3 && durable() == 3);
    CHECK(stages > 0 && regrows > 0 && !lock_depth);

    /* The actual caller must preserve pending RAM and the old durable
     * journal when staging/publication fails. Retry includes the newest RAM
     * value, not a replay of superseded queue entries over that checkpoint. */
    for (unsigned op = 0; op < DBJOURNAL_TEST_NOPS; op++)
        for (unsigned position = 0; position < 20; position++)
        {
            reset(); fill();
            stage_inject = 1;
            dbjournal_test_fail(op, UINT64_C(1) << position, EIO);
            ui_journal_compact_worker();
            dbjournal_test_reset();
            CHECK(!lock_depth && !waited);
            if (ui_stats_recovering)
            {
                fault_cases++;
                CHECK(!COMMAND_QUEUE_IS_EMPTY);
                CHECK(context.refs == 2 && context.stat.queue_length == 2);
                CHECK(durable() == 0);
                /* A failure before enumeration never ran the injection. */
                queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, 3);
                CHECK(ram.indices[0].tag_seek[tag_playcount] == 3);
                stage_inject = 0;
                /* LOW-4: the failed pass backs off; no retry until due */
                CHECK(ui_retry[UI_RETRY_COMPACT].delay);
                ui_journal_compact_worker();
                CHECK(ui_stats_recovering);
                ui_retry[UI_RETRY_COMPACT].delay = 0;   /* ran out */
                ui_journal_compact_worker();
                CHECK(!ui_retry[UI_RETRY_COMPACT].delay);
            }
            CHECK(!ui_stats_recovering && COMMAND_QUEUE_IS_EMPTY);
            CHECK(context.refs == 0 && context.stat.queue_length == 0);
            CHECK(ram.indices[0].tag_seek[tag_playcount] == 3);
            CHECK(durable() == 3);
        }

    reset(); fill();
    stage_inject = 2;
    ui_journal_compact_worker();
    CHECK(ui_commit_cancel && ui_stats_recovering);
    CHECK(context.refs == 2 && context.stat.queue_length == 2);
    CHECK(durable() == 0);
    /* Cancelling a build does not cancel numeric RAM updates. */
    for (int value = 3; value <= 200; value++)
        queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, value);
    CHECK(!waited && context.stat.queue_length == 2);
    CHECK(ram.indices[0].tag_seek[tag_playcount] == 200);
    ui_commit_cancel = false;
    ui_journal_compact_worker();
    CHECK(!ui_stats_recovering && COMMAND_QUEUE_IS_EMPTY);
    CHECK(context.refs == 0 && context.stat.queue_length == 0);
    CHECK(durable() == 200);

    reset(); fill();
    stage_inject = 4;
    ui_journal_compact_worker();
    CHECK(tagcache_usb_quiescing && ui_stats_recovering);
    CHECK(context.refs == 2 && context.stat.queue_length == 2);
    CHECK(durable() == 0);
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, 3);
    tagcache_usb_quiescing = false;
    ui_journal_compact_worker();
    CHECK(!ui_stats_recovering && durable() == 3 && !waited);

    /* A producer outrunning each catch-up pass defers after bounded work,
     * without losing its RAM value or ever dropping the original queue. */
    reset(); fill();
    stage_inject = 3;
    ui_journal_compact_worker();
    CHECK(ui_stats_recovering && context.refs == 2);
    CHECK(context.stat.queue_length == 2 && !waited);
    CHECK(durable() == 0);
    int32_t latest = ram.indices[0].tag_seek[tag_playcount];
    CHECK(latest > 3);
    stage_inject = 0;
    CHECK(ui_retry[UI_RETRY_COMPACT].delay);   /* a deferred pass waits */
    ui_retry[UI_RETRY_COMPACT].delay = 0;
    ui_journal_compact_worker();
    CHECK(!ui_stats_recovering && durable() == latest);
    CHECK(context.refs == 0 && context.stat.queue_length == 0);

    /* Playing/paused allocation is permitted only from free space. Low
     * memory must leave the active library and pending updates untouched. */
    reset(); fill();
    playback = 1;
    free_bytes = 0;
    ui_journal_compact_worker();
    CHECK(ui_stats_recovering && context.refs == 2 && stages == 0);
    CHECK(durable() == 0 && !waited);
    for (unsigned h = 1; h < ARRAYLEN(handles); h++) CHECK(!handles[h]);
    free_bytes = 1024 * 1024;
    stage_inject = 1;
    ui_journal_compact_worker();
    CHECK(!ui_stats_recovering && durable() == 3);
    CHECK(context.refs == 0 && context.stat.queue_length == 0);
    for (unsigned h = 1; h < ARRAYLEN(handles); h++) CHECK(!handles[h]);

    /* Queue exhaustion can precede any attempted append: while audio owns
     * memory, its producer still must not wait for flush scratch to exist. */
    reset();
    playback = 1;
    free_bytes = 0;
    for (int value = 1; value <= 50; value++)
        queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, value);
    CHECK(ui_stats_recovering && !waited);
    CHECK(context.refs == TAGCACHE_COMMAND_QUEUE_LENGTH - 1);
    CHECK(context.stat.queue_length == TAGCACHE_COMMAND_QUEUE_LENGTH - 1);
    CHECK(ram.indices[0].tag_seek[tag_playcount] == 50);
    ui_journal_compact_worker();
    CHECK(ui_stats_recovering && !waited);
    playback = 0;
    free_bytes = 1024 * 1024;
    ui_journal_compact_worker();
    CHECK(!ui_stats_recovering && durable() == 50);
    CHECK(context.refs == 0 && context.stat.queue_length == 0);

    /* No RAM library to coalesce into: the producer drops and counts, it
     * never sleeps (playback thread) and never takes a stale namespace. */
    reset();
    context.stat.ramcache = false;
    for (int value = 1; value <= 50; value++)
        queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, value);
    CHECK(!waited && !ui_stats_recovering);
    CHECK(context.stat.queue_length == TAGCACHE_COMMAND_QUEUE_LENGTH - 1);
    CHECK(ui_stats_dropped == 50 - (TAGCACHE_COMMAND_QUEUE_LENGTH - 1));
    /* Round 4 (R-H1): before the boot replay the worker drains as usual
     * (deltas and relative values need no base), so the queue fills only
     * when it cannot drain at all: then it drops and counts, and never
     * coalesces onto a stale base. Queued absolutes reach RAM at once. */
    reset();
    ui_boot_stats_replayed = false;
    for (int value = 1; value <= 50; value++)
        queue_command(CMD_UPDATE_NUMERIC, 0, tag_lastelapsed, value);
    CHECK(!waited && !ui_stats_recovering);
    CHECK(ui_stats_dropped == 50 - (TAGCACHE_COMMAND_QUEUE_LENGTH - 1));
    CHECK(ram.indices[0].tag_seek[tag_lastelapsed]
          == TAGCACHE_COMMAND_QUEUE_LENGTH - 1);
    ui_journal_compact_worker();            /* not before the replay */
    CHECK(!ui_stats_recovering && stages == 0);
    CHECK(boot_replay() && ui_boot_stats_replayed && !ui_stats_recovering);
    CHECK(COMMAND_QUEUE_IS_EMPTY && context.refs == 0);
    CHECK(observe().values[0][tag_lastelapsed] == TAGCACHE_COMMAND_QUEUE_LENGTH - 1);
    /* No live writes: the replay completes without a checkpoint. */
    reset();
    ui_boot_stats_replayed = false;
    CHECK(boot_replay());
    CHECK(ui_boot_stats_replayed && !ui_stats_recovering);

    /* H3 / round 4: play count and play time are journaled as deltas, a
     * lastplayed before the replay relative to the loaded serial; neither
     * needs a base. The journal has 10, the master 3, a track finishes
     * before the replay: 11. */
    reset();
    ui_boot_stats_replayed = false;
    disk[0].tag_seek[tag_playcount] = ram.indices[0].tag_seek[tag_playcount] = 3;
    disk[0].tag_seek[tag_playtime] = ram.indices[0].tag_seek[tag_playtime] = 300;
    context.master.serial = 50;

    CHECK(dbjournal_append(&ui_stats_journal, names[0], tag_playcount, 9) == DBJOURNAL_OK);
    CHECK(dbjournal_append(&ui_stats_journal, names[0], tag_playcount, 10) == DBJOURNAL_OK);
    CHECK(dbjournal_append(&ui_stats_journal, names[0], tag_playtime, 1000) == DBJOURNAL_OK);
    CHECK(dbjournal_append(&ui_stats_journal, names[1], tag_lastplayed, 500) == DBJOURNAL_OK);
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount | TAGCACHE_UI_DELTA, 1);
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_playtime | TAGCACHE_UI_DELTA, 25);
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_lastplayed, 50);
    CHECK(ram.indices[0].tag_seek[tag_playcount] == 3 && !ui_stats_live);
    CHECK(boot_replay());
    CHECK(ram.indices[0].tag_seek[tag_playcount] == 11);
    CHECK(ram.indices[0].tag_seek[tag_playtime] == 1025);
    /* Recently Played: this boot's track after the journal's newest (500) */
    CHECK(ram.indices[0].tag_seek[tag_lastplayed] > 500);
    CHECK(context.master.serial > ram.indices[0].tag_seek[tag_lastplayed]);
    CHECK(!ui_stats_recovering && COMMAND_QUEUE_IS_EMPTY);
    {
        struct observed o = observe();
        CHECK(o.values[0][tag_playcount] == 11 && o.values[0][tag_playtime] == 1025);
        CHECK(o.values[0][tag_lastplayed] > o.values[1][tag_lastplayed]);
    }
    /* after the replay a delta adds to the current value at once */
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount | TAGCACHE_UI_DELTA, 1);
    CHECK(ram.indices[0].tag_seek[tag_playcount] == 12);
    ui_stats_drain();
    CHECK(COMMAND_QUEUE_IS_EMPTY && durable() == 12);

    /* H-1: a delta queued during a replay that fails; the retry (after its
     * backoff) counts it once, and nothing wedges the queue. */
    for (int at = 0; at < 3; at++)
    {
        reset();
        ui_boot_stats_replayed = false;
        disk[0].tag_seek[tag_playcount] = ram.indices[0].tag_seek[tag_playcount] = 3;
        CHECK(dbjournal_append(&ui_stats_journal, names[0], tag_playcount, 10) == DBJOURNAL_OK);
        CHECK(dbjournal_append(&ui_stats_journal, names[0], tag_playcount | TAGCACHE_UI_DELTA, 1) == DBJOURNAL_OK);
        CHECK(dbjournal_append(&ui_stats_journal, names[1], tag_rating, 4) == DBJOURNAL_OK);
        mutex_lock(&command_queue_mutex);
        CHECK(ui_command_drain_locked(open_buffer, sizeof(open_buffer), true));
        struct dbjournal snapshot = ui_stats_journal;
        ui_stats_journal.scratch = NULL;
        mutex_unlock(&command_queue_mutex);
        unsigned char scratch[8192];
        struct ui_import imp = { 0 };
        snapshot.scratch = scratch;
        snapshot.bytes = sizeof(scratch);
        ui_import_ctx = ui_active;
        ui_stats_replaying = true;
        queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount | TAGCACHE_UI_DELTA, 1);
        fail_lookup_after = at;     /* after 0, 1 or 2 frames */
        CHECK(dbjournal_replay(&snapshot, ui_stats_apply, &imp) != DBJOURNAL_OK);
        ui_stats_replay_end(false, 3);
        ui_import_ctx = NULL;
        CHECK(!ui_boot_stats_replayed && context.stat.ramcache);
        CHECK(!(ram.indices[0].flag & FLAG_UI_STATS_REPLAYED));
        ui_retry[UI_RETRY_REPLAY].delay = 0;    /* the backoff ran out */
        CHECK(boot_replay());
        CHECK(ram.indices[0].tag_seek[tag_playcount] == 12);
        CHECK(COMMAND_QUEUE_IS_EMPTY && durable() == 12 && !ui_retry[UI_RETRY_REPLAY].delay);
    }

    /* MED-1: an append that reached disk but whose readback or close
     * failed is not appended again (a delta would count twice). */
    for (int op = 0; op < 2; op++)
    {
        reset();
        queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount | TAGCACHE_UI_DELTA, 1);
        /* the append's own close / readback read (create_empty's first) */
        dbjournal_test_fail(op ? DBJOURNAL_TEST_READ : DBJOURNAL_TEST_CLOSE, 2, EIO);
        ui_stats_drain();
        dbjournal_test_reset();
        CHECK(!COMMAND_QUEUE_IS_EMPTY && ui_append_maybe);
        ui_stats_drain();               /* reopened: it is there */
        CHECK(COMMAND_QUEUE_IS_EMPTY && durable() == 1);
    }

    /* MED-2: lastplayed before the replay over 8 boots: every play sorts
     * above the journal's newest and above the earlier plays, and the
     * serial grows by one per play, never by a moving offset. */
    reset();
    CHECK(dbjournal_append(&ui_stats_journal, names[1], tag_lastplayed, 500) == DBJOURNAL_OK);
    {
        int32_t last = 500;
        int plays = 0;
        for (int boot = 0; boot < 8; boot++)
        {
            memset(&ram, 0, sizeof(ram));
            context.master.serial = 0;  /* the published master's */
            ui_boot_stats_replayed = false;
            if (boot)
                CHECK(dbjournal_open_cancel(journal_root, &ui_stats_journal,
                    open_buffer, sizeof(open_buffer), ui_generation_cancel,
                    NULL) == DBJOURNAL_OK);
            for (int i = 0; i < 3; i++, plays++)
            {
                queue_command(CMD_UPDATE_NUMERIC, 0, tag_lastplayed,
                              context.master.serial++);
                ui_stats_drain();
            }
            CHECK(boot_replay());
            CHECK(ram.indices[0].tag_seek[tag_lastplayed] > last);
            last = ram.indices[0].tag_seek[tag_lastplayed];
            CHECK(context.master.serial <= 501 + plays);
        }
        struct observed o = observe();
        CHECK(o.values[0][tag_lastplayed] == last && o.serial <= 501 + plays);
    }

    /* MED-4: a failed replay whose restore cannot read the master forces
     * a RAM reload instead of retrying onto half-replayed rows. */
    reset();
    ui_boot_stats_replayed = false;
    CHECK(dbjournal_append(&ui_stats_journal, names[0], tag_playcount, 10) == DBJOURNAL_OK);
    CHECK(dbjournal_append(&ui_stats_journal, names[0], tag_playcount | TAGCACHE_UI_DELTA, 1) == DBJOURNAL_OK);
    CHECK(dbjournal_append(&ui_stats_journal, names[1], tag_rating, 4) == DBJOURNAL_OK);
    queue_command(CMD_UPDATE_NUMERIC, 1, tag_lastelapsed, 1234);   /* a resume */
    fail_lookup_after = 2;
    fail_read_index = 1;
    CHECK(!boot_replay());
    CHECK(!context.stat.ramcache && ui_reload_pending && ui_busy);
    /* ... and a restore that works keeps this session's queued absolutes */
    reset();
    ui_boot_stats_replayed = false;
    CHECK(dbjournal_append(&ui_stats_journal, names[0], tag_playcount, 10) == DBJOURNAL_OK);
    CHECK(dbjournal_append(&ui_stats_journal, names[0], tag_playcount | TAGCACHE_UI_DELTA, 1) == DBJOURNAL_OK);
    CHECK(dbjournal_append(&ui_stats_journal, names[1], tag_rating, 4) == DBJOURNAL_OK);
    fail_lookup_after = 2;
    {
        mutex_lock(&command_queue_mutex);
        CHECK(ui_command_drain_locked(open_buffer, sizeof(open_buffer), true));
        struct dbjournal snapshot = ui_stats_journal;
        ui_stats_journal.scratch = NULL;
        mutex_unlock(&command_queue_mutex);
        unsigned char scratch[8192];
        struct ui_import imp = { 0 };
        snapshot.scratch = scratch;
        snapshot.bytes = sizeof(scratch);
        ui_import_ctx = ui_active;
        queue_command(CMD_UPDATE_NUMERIC, 1, tag_lastelapsed, 1234);
        CHECK(dbjournal_replay(&snapshot, ui_stats_apply, &imp) != DBJOURNAL_OK);
        ui_stats_replay_end(false, 3);
        ui_import_ctx = NULL;
    }
    CHECK(context.stat.ramcache && ram.indices[0].tag_seek[tag_playcount] == 0);
    CHECK(ram.indices[1].tag_seek[tag_lastelapsed] == 1234);

    /* Played until power-off with no replay this boot: the shutdown drain
     * journals the plays; the next boot's replay counts them. */
    reset();
    ui_boot_stats_replayed = false;
    disk[0].tag_seek[tag_playcount] = ram.indices[0].tag_seek[tag_playcount] = 3;
    CHECK(dbjournal_append(&ui_stats_journal, names[0], tag_playcount, 10) == DBJOURNAL_OK);
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount | TAGCACHE_UI_DELTA, 1);
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount | TAGCACHE_UI_DELTA, 1);
    ui_shutdown_flushing = true;
    ui_stats_drain();
    ui_shutdown_flushing = false;
    CHECK(COMMAND_QUEUE_IS_EMPTY);
    /* the next boot: RAM from the master, the journal reopened */
    memset(&ram, 0, sizeof(ram));
    ram.indices[0].tag_seek[tag_playcount] = 3;
    ui_boot_stats_replayed = false;
    CHECK(dbjournal_open_cancel(journal_root, &ui_stats_journal, open_buffer,
                                sizeof(open_buffer), ui_generation_cancel, NULL)
          == DBJOURNAL_OK);
    CHECK(boot_replay() && ram.indices[0].tag_seek[tag_playcount] == 12);

    /* R-H2: off the tagcache thread nothing opens or salvages the journal
     * (the storage thread's 2 KiB stack); it is only marked due. */
    reset();
    ui_stats_journal.ready = false;
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, 7);
    on_worker = false;
    ui_drain_due = false;
    mutex_lock(&command_queue_mutex);
    CHECK(!ui_command_drain_locked(open_buffer, sizeof(open_buffer), false));
    mutex_unlock(&command_queue_mutex);
    CHECK(ui_drain_due && !ui_stats_journal.ready && !ui_stats_recovering);
    CHECK(context.stat.queue_length == 1);
    on_worker = true;
    mutex_lock(&command_queue_mutex);
    CHECK(ui_command_drain_locked(open_buffer, sizeof(open_buffer), false));
    mutex_unlock(&command_queue_mutex);
    CHECK(ui_stats_journal.ready && COMMAND_QUEUE_IS_EMPTY && durable() == 7);

    /* B-M5: a clean shutdown with RAM-coalesced values owed appends the
     * queued prefix, then the pending rows, as frames: no compaction. */
    reset();
    playback = 1;
    free_bytes = 0;
    for (int value = 1; value <= 50; value++)
        queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, value);
    CHECK(ui_stats_recovering && !waited);
    playback = 0;
    compact_stages = 0;
    ui_stats_drain();
    CHECK(!ui_stats_recovering && COMMAND_QUEUE_IS_EMPTY && !compact_stages);
    CHECK(!(ram.indices[0].flag & FLAG_UI_STATS_PENDING));
    CHECK(context.refs == 0 && context.stat.queue_length == 0);
    CHECK(durable() == 50 && !lock_depth);
    for (unsigned h = 1; h < ARRAYLEN(handles); h++) CHECK(!handles[h]);
    /* H2: a compaction that does not finish (here cancelled during its
     * enumeration, as at a stop or USB) gives back the PENDING marks it
     * took; the next drain, at shutdown too, then appends every row. */
    reset();
    playback = 1;
    free_bytes = 0;
    for (int value = 1; value <= 50; value++)
        queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, value);
    queue_command(CMD_UPDATE_NUMERIC, 1, tag_rating, 9);
    CHECK(ui_stats_recovering && (ram.indices[1].flag & FLAG_UI_STATS_PENDING));
    playback = 0;
    free_bytes = 1024 * 1024;
    stage_inject = 2;
    ui_journal_compact_worker();
    CHECK(ui_commit_cancel && ui_stats_recovering);
    CHECK((ram.indices[0].flag & FLAG_UI_STATS_PENDING)
          && (ram.indices[1].flag & FLAG_UI_STATS_PENDING));
    ui_commit_cancel = false;
    ui_shutdown_flushing = true;
    ui_stats_drain();
    ui_shutdown_flushing = false;
    CHECK(!ui_stats_recovering && COMMAND_QUEUE_IS_EMPTY && !lock_depth);
    {
        struct dbjournal reopened;
        unsigned char scratch[8192];
        CHECK(dbjournal_open(journal_root, &reopened, scratch, sizeof(scratch)) == DBJOURNAL_OK);
        struct observed o = {0};
        CHECK(dbjournal_replay(&reopened, replay, &o) == DBJOURNAL_OK);
        CHECK(o.values[0][tag_playcount] == 50 && o.values[1][tag_rating] == 9);
    }
    /* ... and a journal that cannot take them leaves the RAM recovery in
     * place (logged; lost only if the power really goes). */
    reset(); fill();
    ui_stats_drain();
    CHECK(ui_stats_recovering && ram.indices[0].tag_seek[tag_playcount] == 2);
    CHECK(retry_calls == 1);    /* retried with backoff, not every pass */
    ui_drain_force = true;
    CHECK(ui_drain_wanted());   /* a system_flush() caller still asks */
    ui_drain_force = false;
    CHECK(!lock_depth);
    /* The worker writes queued statistics only while the disk is awake
     * anyway (the idle notice comes just before it sleeps), or once the
     * queue is half full: no spin-up of their own per track change. */
    reset();
    CHECK(!ui_drain_wanted());
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, 1);
    ui_drain_kick();
    CHECK(ui_drain_due && !ui_drain_wanted());
    disk_active = true;
    CHECK(ui_drain_wanted());
    disk_active = false;
    for (int i = 2; i <= TAGCACHE_COMMAND_QUEUE_LENGTH / 2; i++)
        queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, i);
    CHECK(ui_drain_wanted());
    /* M-1: the count across the ring's wrap (widx below ridx) */
    command_queue_ridx = TAGCACHE_COMMAND_QUEUE_LENGTH - 1;
    command_queue_widx = 1;         /* 2 queued: not half, disk asleep */
    CHECK(!ui_drain_wanted());
    command_queue_widx = TAGCACHE_COMMAND_QUEUE_LENGTH / 2 - 1;  /* half */
    CHECK(ui_drain_wanted());
    command_queue_ridx = command_queue_widx = 0;

    /* A corrupt journal is salvaged by the production caller: statistics
     * resume, publication is not blocked by a recovery that never ends,
     * and RAM is checkpointed (the bad frame's value comes back). */
    reset();
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, 5);
    queue_command(CMD_UPDATE_NUMERIC, 1, tag_rating, 4);
    mutex_lock(&command_queue_mutex);
    CHECK(ui_command_drain_locked(open_buffer, sizeof(open_buffer), false));
    mutex_unlock(&command_queue_mutex);
    {
        char path[512];
        unsigned char b;
        snprintf(path, sizeof(path), "%s/database_stats.jnl", journal_root);
        int fd = open(path, O_RDWR);
        CHECK(fd >= 0 && pread(fd, &b, 1, ui_stats_journal.end - 4) == 1);
        b ^= 0x40;
        CHECK(pwrite(fd, &b, 1, ui_stats_journal.end - 4) == 1 && close(fd) == 0);
    }
    ui_stats_journal.ready = false;         /* e.g. after a failed append */
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, 6);
    mutex_lock(&command_queue_mutex);
    CHECK(ui_command_drain_locked(open_buffer, sizeof(open_buffer), false));
    mutex_unlock(&command_queue_mutex);
    CHECK(ui_stats_recovering && ui_stats_journal.ready && !retry_calls);
    ui_journal_compact_worker();
    CHECK(!ui_stats_recovering && COMMAND_QUEUE_IS_EMPTY && !waited);
    {
        struct dbjournal reopened;
        unsigned char scratch[8192];
        CHECK(dbjournal_open(journal_root, &reopened, scratch, sizeof(scratch)) == DBJOURNAL_OK);
        struct observed o = {0};
        CHECK(dbjournal_replay(&reopened, replay, &o) == DBJOURNAL_OK);
        CHECK(o.values[0][tag_playcount] == 6 && o.values[1][tag_rating] == 4);
    }

    /* Round 6 MED-A: compaction, publication, a compaction that would omit
     * R (equal to the new master), the new generation invalid at boot, the
     * fallback (the generation it was built from): R keeps its value. */
    reset();
    ram.indices[0].tag_seek[tag_playcount] = 5;     /* R played, N-1 has 0 */
    ui_stats_recovering = true;
    ui_journal_compact_worker();                    /* checkpoint over N-1 */
    CHECK(!ui_stats_recovering && observe().values[0][tag_playcount] == 5);
    {
        /* N: its import gave R N-1's value (0); its replay folds R = 5 */
        memset(&cand, 0, sizeof(cand));
        memset(cand_disk, 0, sizeof(cand_disk));
        cand.master.tch.entry_count = 2;
        struct dbjournal snapshot = ui_stats_journal;
        unsigned char scratch[8192];
        snapshot.scratch = scratch;
        snapshot.bytes = sizeof(scratch);
        ui_import_ctx = &cand;
        CHECK(dbjournal_replay(&snapshot, ui_stats_apply, &ui_stats_lookup)
              == DBJOURNAL_OK);
        ui_import_ctx = NULL;
        CHECK(cand_disk[1].tag_seek[tag_playcount] == 5);
        CHECK((cand_disk[1].flag & FLAG_UI_STATS_FOLDED)
              && !(cand_disk[0].flag & FLAG_UI_STATS_FOLDED));
        /* published: its master on disk and RAM, in this harness's order */
        disk[0] = cand_disk[1];
        disk[1] = cand_disk[0];
        memset(&ram, 0, sizeof(ram));
        ram.indices[0] = disk[0];
        ram.indices[1] = disk[1];
    }
    ui_stats_recovering = true;
    ui_journal_compact_worker();                    /* checkpoint over N */
    CHECK(!ui_stats_recovering);
    {
        /* N fails validation: N-1 (R = 0) plus the journal */
        struct observed o = observe();
        CHECK(o.values[0][tag_playcount] == 5);
        for (int t = 0; t < TAG_COUNT; t++)
            CHECK(o.values[1][t] == 0);             /* unfolded: omitted */
    }

    /* LOW-1: an append that failed before its write, then a checkpoint
     * whose sequence lands on the remembered one: the next new play is
     * appended, never taken for the failed one. */
    reset();
    disk[0].tag_seek[tag_playcount] = 1;
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount | TAGCACHE_UI_DELTA, 1);
    CHECK(ram.indices[0].tag_seek[tag_playcount] == 1);
    dbjournal_test_fail(DBJOURNAL_TEST_WRITE, 1, EIO);
    ui_stats_drain();
    dbjournal_test_reset();
    CHECK(ui_append_maybe && ui_stats_recovering);
    {
        uint64_t remembered = ui_append_maybe;
        ui_journal_compact_worker();    /* nothing differs: empty checkpoint */
        CHECK(!ui_stats_recovering && COMMAND_QUEUE_IS_EMPTY);
        CHECK(ui_stats_journal.seq == remembered);      /* the trap */
        CHECK(!ui_append_maybe);                        /* forgotten */
    }
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount | TAGCACHE_UI_DELTA, 1);
    ui_stats_drain();
    CHECK(COMMAND_QUEUE_IS_EMPTY);
    {
        /* the master's 1, the checkpoint (nothing), the new play: 2 */
        struct observed o = observe();
        CHECK(1 + o.values[0][tag_playcount] == 2);
    }

    /* LOW-2: a relative lastplayed resolves over the master's loaded
     * serial, not over this boot's pre-replay increase_serial() bumps: a
     * play from an earlier boot stays below that boot's later plays. */
    reset();
    loaded_serial = context.master.serial = 1000;
    ui_boot_stats_replayed = false;
    queue_command(CMD_UPDATE_NUMERIC, 0, tag_lastplayed,
                  context.master.serial++);         /* A, before the replay */
    ui_stats_drain();
    CHECK(boot_replay());
    queue_command(CMD_UPDATE_NUMERIC, 1, tag_lastplayed,
                  context.master.serial++);         /* B, after it */
    ui_stats_drain();
    CHECK(ram.indices[0].tag_seek[tag_lastplayed]
          < ram.indices[1].tag_seek[tag_lastplayed]);
    /* the next boot: five tracks started before its replay */
    memset(&ram, 0, sizeof(ram));
    context.master.serial = 1000 + 5;
    ui_boot_stats_replayed = false;
    CHECK(dbjournal_open_cancel(journal_root, &ui_stats_journal, open_buffer,
                                sizeof(open_buffer), ui_generation_cancel, NULL)
          == DBJOURNAL_OK);
    CHECK(boot_replay());
    CHECK(ram.indices[0].tag_seek[tag_lastplayed]
          < ram.indices[1].tag_seek[tag_lastplayed]);
    CHECK(context.master.serial >= 1005);           /* no serial reused */

    /* LOW-4: a compaction that keeps failing waits for its backoff (the
     * disk can sleep) instead of retrying at every 1 s timeout. */
    reset(); fill();
    fail_open_master = true;
    master_opens = 0;
    ui_journal_compact_worker();
    CHECK(ui_stats_recovering && master_opens == 1 && retry_calls == 1);
    for (int i = 0; i < 5; i++)
        ui_journal_compact_worker();
    CHECK(master_opens == 1 && retry_calls == 1);
    ui_retry[UI_RETRY_COMPACT].delay = 0;           /* due again */
    fail_open_master = false;
    ui_journal_compact_worker();
    CHECK(!ui_stats_recovering && master_opens == 2 && durable() == 2);

    /* Round 7 MED-B: no compaction while a candidate is unpublished (its
     * prepared replay folded values a sparse checkpoint may omit, and a
     * re-prepare does not re-clone it). Drains still append owed rows. */
    reset();
    ui_build = &cand;
    playback = 1;
    free_bytes = 0;
    for (int value = 1; value <= 50; value++)
        queue_command(CMD_UPDATE_NUMERIC, 0, tag_playcount, value);
    CHECK(ui_stats_recovering);
    playback = 0;
    free_bytes = 1024 * 1024;
    master_opens = compact_stages = 0;
    ui_journal_compact_worker();
    CHECK(ui_stats_recovering && !master_opens && !compact_stages);
    CHECK(ui_build == &cand);
    ui_stats_drain();                   /* the owed path clears it */
    CHECK(!ui_stats_recovering && durable() == 50 && ui_build == &cand);
    /* ... but a full journal (no drain can append) gets a checkpoint of
     * every row, whole: correct over the candidate's master too (published
     * or not), the candidate kept, no rescan (R7a/R7b). Only two rows
     * here: each gets all six values, unchanged ones included. */
    reset(); fill();
    ram.indices[1].tag_seek[tag_lastoffset] = 0;    /* equal to the master */
    ui_stats_drain();
    CHECK(ui_journal_full && ui_stats_recovering);
    ui_build = &cand;
    ui_journal_compact_worker();
    CHECK(ui_build == &cand && !ui_stats_recovering && !ui_journal_full);
    {
        /* any master (here: one whose row 1 holds a stale resume) gets the
         * current values: every tag of every row is in the checkpoint */
        struct observed o = { 0 };
        for (int t = 0; t < TAG_COUNT; t++)
            o.values[1][t] = 777;
        struct dbjournal reopened;
        unsigned char scratch[8192];
        CHECK(dbjournal_open(journal_root, &reopened, scratch, sizeof(scratch)) == DBJOURNAL_OK);
        CHECK(dbjournal_replay(&reopened, replay, &o) == DBJOURNAL_OK);
        CHECK(o.values[0][tag_playcount] == 2);
        for (int t = tag_playcount; t < TAG_COUNT; t++)
            CHECK(o.values[1][t] == ram.indices[1].tag_seek[t]);
    }
    /* R7a: during playback with no room it stays a deferral (backoff), with
     * nothing dropped or rescanned */
    reset(); fill();
    ui_stats_drain();
    ui_build = &cand;
    playback = 1;
    free_bytes = 0;
    master_opens = 0;
    ui_journal_compact_worker();
    CHECK(ui_build == &cand && ui_stats_recovering && !master_opens);

    printf("production journal hard-full recovery: %u checks, %u injected failing caller positions passed\n",
           checks, fault_cases);
    return 0;
}
