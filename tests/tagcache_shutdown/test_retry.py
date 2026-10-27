#!/usr/bin/env python3
#***************************************************************************
#               __________                   __    __________
#    Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
#    Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
#    Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
#    Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
#                                                \/        \/               \/
#
# Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
# Not affiliated with or endorsed by Apple Inc.
#
# Copyright (C) 2026 prokopto-dev
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
#***************************************************************************

"""Actual tagcache retry/deferral/abandon code (#85 review findings 1, 3, 4)
compiled over a tiny platform shim: backoff instead of a retry every second,
no stuck "Updating Library...", a definitively bad candidate is dropped and
only content failures escalate a RAM load to a fresh rebuild."""
from pathlib import Path
import os
import subprocess
import tempfile

source = Path("apps/tagcache.c").read_text()


def function(signature):
    start = source.index(signature + "\n{")  # the definition, not a prototype
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


retry = source[source.index("enum { UI_RETRY_SELECT"):
               source.index("static bool ui_retry_due(int which)")]
harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dbgen.h"
#define HZ 100
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define TIME_BEFORE(a,b) ((long)((a)-(b)) < 0)
#define TAGCACHE_FILE_TEMP "database_tmp.tcd"
#define TAGCACHE_FILE_IMPORT "database_import.pending"
#define Q_REBUILD 7
struct master_header { struct { int32_t entry_count; } tch; int32_t commitid; };
struct tagcache_stat {
    bool ready, ramcache;
    int ramcache_allocated, ramcache_used;
    char db_path[260];
};
struct tagcache_context {
    struct tagcache_stat stat;
    struct master_header master;
    struct { int handle; void *hdr; } ram;
    int readers, operations, writer_count, reader_count, generation;
    bool reserved;
};
static struct tagcache_context ui_context_a, ui_context_b;
static struct tagcache_context *ui_build = &ui_context_a, *ui_active = &ui_context_a;
#define tc_stat (ui_build->stat)
#define current_tcmh (ui_build->master)
static long current_tick;
static char ui_db_root[] = "/.rockbox";
static int tagcache_queue, command_queue_mutex;
static bool ui_generation_unresolved, ui_generation_fresh, ui_db_fresh;
static bool ui_candidate_pending, ui_maintenance_for_snapshot, ui_import_pending;
static bool ui_pre_pending, ui_pre_request, ui_busy = true, ui_stats_recovering;
static bool ui_stats_restart, ui_candidate_maybe_published, ui_reload_pending;
static int ui_candidate_exported;
/* 3.20: the header's busy-glyph job count (tagcache_ui_updating()) */
#define UI_UPD_RUN(stmt) do { stmt; } while (0)
static int playing, mem_full;
static bool ui_begin_mem_wait;
static bool ui_candidate_changed, ui_active_pre_done, ui_commit_pending, ui_scan_pending;
static int audio_status(void) { return playing; }
/* the paused window (apps/ipodui/maint.h, #88) */
enum maint_job { MAINT_COMMIT, MAINT_CHAPTERS, MAINT_DBQ, MAINT_LISTS };
static size_t ui_maint_need;
static int lists_done;          /* P16: the precompute's tenancy ends */
static void maint_done(enum maint_job j, bool ok)
{ (void)ok; if (j == MAINT_LISTS) lists_done++; }
static int window_open, declined, take_fits;
static bool maint_window_open(void) { return window_open; }
static bool maint_declined(enum maint_job j) { (void)j; return declined; }
static bool maint_take_fits(size_t b) { (void)b; return take_fits; }
static size_t ui_peak_estimate(void) { return 1; }
#define UI_MAINT_SLACK 1
static long newest_manifest = -1, newest_export = -1;
int32_t dbgen_newest_commit(const char *r, void *s, size_t b, const struct dbgen_ops *o)
{ (void)r; (void)s; (void)b; (void)o; return newest_manifest; }
static long ui_changelog_scan(char *name) { name[0] = 0; return newest_export; }
static int exports, export_rc = 1;
static int ui_export_changelog(void) { exports++; return export_rc; }
static bool tagcache_context_published(struct tagcache_context *c) { (void)c; return true; }
static volatile bool ui_publishing;
static unsigned ui_load_failures;
static bool ui_boot_stats_replayed = true;
#define MAX(a,b) ((a) > (b) ? (a) : (b))
static enum dbgen_result prepare_rc = DBGEN_OK, stage_rc = DBGEN_OK;
static int prepares, stages, rebuilds, handles, logs;
struct ui_generation_io { void *buffer; size_t size; };
static unsigned char io_buffer[16384];
static void ui_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; logs++; }
static int disable_irq_save(void) { return 0; }
static void restore_irq(int level) { (void)level; }
static void mutex_lock(int *m) { (void)m; }
static void mutex_unlock(int *m) { (void)m; }
static void lib_generation_pool_abandon(struct tagcache_context *c) { (void)c; }
/* #109: no database queue in this harness: never waits, keeps nothing */
static bool dbq_prepare_wait(void) { return false; }
static bool dbq_idx_live(void) { return false; }
static bool ui_context_ref(struct tagcache_context *c, bool op)
{ (void)c; (void)op; return false; }
static void dbq_published(struct tagcache_context *c) { (void)c; }
static int pool_full;           /* P16: the view pool refused (free memory) */
static bool lib_generation_pool_prepare(struct tagcache_context *c)
{ (void)c; return !pool_full; }
static int ui_generation_buffer(struct ui_generation_io *io)
{ if (mem_full) return 0;
  io->buffer = io_buffer; io->size = sizeof(io_buffer); return ++handles; }
static struct dbgen_ops ui_generation_ops(struct ui_generation_io *io)
{ struct dbgen_ops ops = { 0, 0, 0, 0, io }; return ops; }
static int core_free(int h) { (void)h; handles--; return 0; }
static void audio_buffer_regrow(void) {}
static void strmemccpy(char *d, const char *s, size_t n) { snprintf(d, n, "%s", s); }
static int remove_db_file(struct tagcache_context *c, const char *f)
{ (void)c; (void)f; errno = ENOENT; return -1; }
enum dbgen_result dbgen_prepare(const char *r, const char *a, char out[DBGEN_PATH_MAX],
    void *s, size_t b, const struct dbgen_ops *o)
{ (void)r; (void)a; (void)s; (void)b; (void)o; prepares++;
  strcpy(out, "/.rockbox/database_gen2"); return prepare_rc; }
enum dbgen_result dbgen_prepare_empty(const char *r, const char *a, char out[DBGEN_PATH_MAX],
    void *s, size_t b, const struct dbgen_ops *o)
{ return dbgen_prepare(r, a, out, s, b, o); }
enum dbgen_result dbgen_stage_publish(const char *r, const char *c, int32_t id,
    int32_t n, void *s, size_t b, const struct dbgen_ops *o, struct dbgen_publication *p)
{ (void)r; (void)c; (void)id; (void)n; (void)s; (void)b; (void)o; (void)p;
  stages++; return stage_rc; }
enum dbgen_result dbgen_select(const char *r, char out[DBGEN_PATH_MAX], void *s,
    size_t b, const struct dbgen_ops *o)
{ (void)r; (void)s; (void)b; (void)o; strcpy(out, "/.rockbox/database_gen0"); return DBGEN_OK; }
enum dbgen_result dbgen_finish_publish(struct dbgen_publication *p) { (void)p; return DBGEN_OK; }
static bool ui_candidate_stats_finish(void *b, size_t n) { (void)b; (void)n; return true; }
static void ui_candidate_stats_release(void) {}
static void ui_publish_end(void) { ui_publishing = false; }
static long rebuild_data = -1;
static void queue_post(int *q, long id, intptr_t data)
{ (void)q; if (id == Q_REBUILD) { rebuilds++; rebuild_data = data; } }
static struct tagcache_context *ui_ram_yielded;  /* P16 RAM yield */
static size_t ui_yield_bytes;
static unsigned ui_yields;
#define UI_YIELD_MAX 4
static bool ui_publish_busy;
'''
harness += retry
harness += function("static bool ui_retry_due(int which)")
harness += function("static void ui_retry_later(int which, const char *what, int rc)")
harness += function("static bool ui_candidate_begin(void)")
harness += function("static bool ui_initial_build_begin(void)")
harness += function("static void ui_candidate_abandon(int rc)")
harness += function("static bool ui_candidate_publish(void)")
harness += source[source.index("static bool ui_rebuild_owed;"):
                  source.index("/* The owed fresh rebuild")]
harness += function("static void ui_rebuild_run(void)")
harness += function("static void ui_load_failed(bool content)")
harness += function("static void ui_yield_note(void)")
harness += function("static bool ui_yield_reload_can(void)")
harness += function("static bool ui_yield_reload_due(void)")
harness += r'''
static bool ui_shutdown_intent, ui_stats_prepared;
static struct tagcache_context *ui_import_ctx;
static int stats_prepares, prepare_ok;
static bool ui_candidate_stats_prepare(void)
{ stats_prepares++; ui_stats_prepared = prepare_ok; return prepare_ok; }
static void load_ramcache(void) {}
static int ram_yields;          /* P16: refused prepares that yield RAM */
static void ui_candidate_ram_yield(void) { ram_yields++; }
static int tagcache_ui_count_ctx(struct tagcache_context *c) { (void)c; return 1; }
static bool ui_precompute_event(void) { return false; }
#define LIB_PRE_DONE 0
static int lib_precompute_ctx(struct tagcache_context *c, bool f, bool (*e)(void))
{ (void)c; (void)f; (void)e; return LIB_PRE_DONE; }
'''
harness += function("static void ui_precompute_run(void)")
harness += r'''
static void reset(void)
{
    memset(&ui_context_a, 0, sizeof(ui_context_a));
    memset(&ui_context_b, 0, sizeof(ui_context_b));
    ui_build = ui_active = &ui_context_a;
    strcpy(ui_active->stat.db_path, "/.rockbox/database_gen0");
    memset(ui_retry, 0, sizeof(ui_retry));
    prepare_rc = stage_rc = DBGEN_OK;
    mem_full = 0;
    prepares = stages = rebuilds = 0;
    ui_busy = true; ui_generation_fresh = false; ui_load_failures = 0;
    ui_rebuild_owed = false; ui_rebuild_tried = false; rebuild_data = -1;
    ui_rebuild_exported = 0; exports = 0; export_rc = 1;
    newest_manifest = newest_export = -1;
    current_tick = 0;
}
/* One second per SYS_TIMEOUT for 'seconds' */
#define EVERY_SECOND(seconds, body) \
    for (int t_ = 0; t_ < (seconds); t_++, current_tick += HZ) { body; }
int main(void)
{
    /* Backoff: 2 s doubling, capped at 5 minutes */
    reset();
    long expect = 2 * HZ;
    for (int i = 0; i < 12; i++)
    {
        ui_retry_later(UI_RETRY_PUBLISH, "x", -1);
        assert(ui_retry[UI_RETRY_PUBLISH].delay == expect);
        assert(!ui_retry_due(UI_RETRY_PUBLISH));
        current_tick += expect;
        assert(ui_retry_due(UI_RETRY_PUBLISH));
        expect = MIN(expect * 2, 300 * HZ);
    }

    /* Finding 1: a fresh build that cannot find a safe directory defers
     * with backoff and never leaves "Updating Library..." stuck. */
    reset();
    prepare_rc = DBGEN_BUSY;
    EVERY_SECOND(60, assert(!ui_initial_build_begin()); assert(!ui_busy));
    assert(prepares >= 2 && prepares <= 5);
    prepare_rc = DBGEN_OK;
    current_tick += 300 * HZ;
    assert(ui_initial_build_begin() && ui_busy && ui_build != ui_active);
    assert(!ui_retry[UI_RETRY_BEGIN].delay);
    /* ... but while audio holds the memory a first build waits with
     * "Updating Library..." up (no library yet), cleared at the stop if
     * it still cannot begin */
    reset();
    mem_full = 1;
    playing = 1;
    assert(!ui_initial_build_begin() && ui_busy);
    playing = 0;
    prepare_rc = DBGEN_BUSY;
    mem_full = 0;
    current_tick += 300 * HZ;
    assert(!ui_initial_build_begin() && !ui_busy);
    /* a prepare failing for another reason while playing never keeps it
     * up (budget review L2) */
    reset();
    prepare_rc = DBGEN_BUSY;
    playing = 1;
    assert(!ui_initial_build_begin() && !ui_busy);
    playing = 0;

    /* Finding 4: a permanently failing publication is retried a few times
     * a minute, not every second with full validation passes ... */
    reset();
    ui_build = &ui_context_b;
    ui_build->stat.ready = ui_build->stat.ramcache = true;
    stage_rc = DBGEN_IO;
    EVERY_SECOND(60, assert(!ui_candidate_publish()));
    assert(stages >= 2 && stages <= 5);
    /* ... nothing is validated while a reader pins the active library ... */
    stages = 0;
    current_tick += 300 * HZ;
    ui_active->operations = 1;
    assert(!ui_candidate_publish() && stages == 0);
    ui_active->operations = 0;
    /* ... and a definitively invalid candidate is dropped, keeping the
     * active library, instead of failing forever. */
    stage_rc = DBGEN_CORRUPT;
    ui_busy = true;
    assert(!ui_candidate_publish() && stages == 1);
    assert(ui_build == ui_active && !ui_busy && !ui_candidate_pending);
    EVERY_SECOND(30, assert(ui_candidate_publish()));
    assert(stages == 1);
    /* Success resets the backoff. */
    reset();
    ui_build = &ui_context_b;
    ui_build->stat.ready = ui_build->stat.ramcache = true;
    ui_retry_later(UI_RETRY_PUBLISH, "x", -1);
    current_tick += 2 * HZ;
    assert(ui_candidate_publish() && ui_active == &ui_context_b);
    assert(!ui_retry[UI_RETRY_PUBLISH].delay && !ui_busy);

    /* Finding 3: I/O, memory or USB failures never escalate; three
     * content failures request one fresh rebuild elsewhere. */
    reset();
    ui_active->stat.ready = true;
    for (int i = 0; i < 10; i++)
        ui_load_failed(false);
    assert(!rebuilds && !ui_generation_fresh && ui_active->stat.ready);
    ui_reload_pending = true;
    ui_load_failed(true);
    ui_load_failed(true);
    assert(!rebuilds);
    ui_load_failed(true);
    assert(rebuilds == 1 && ui_generation_fresh && !ui_active->stat.ready);
    assert(!ui_reload_pending && !ui_load_failures);
    /* the request keeps the backoff (2: internal), the candidate has begun */
    assert(rebuild_data == 2 && ui_build != ui_active && !ui_rebuild_owed);
    /* R-M3/M2: a library failing again is not rebuilt again this boot
     * (no loop Q_REBUILD -> same failure -> Q_REBUILD; new commit IDs too) */
    ui_build = ui_active;
    ui_active->master.commitid++;
    for (int i = 0; i < 9; i++)
        ui_load_failed(true);
    assert(rebuilds == 1);
    /* R-M3: a rebuild that cannot begin yet is never lost, and the active
     * library is not declared unusable before its replacement begins:
     * it is retried from the loop under the begin backoff. */
    reset();
    ui_active->stat.ready = true;
    ui_active->master.commitid = 7;
    prepare_rc = DBGEN_BUSY;
    for (int i = 0; i < 3; i++)
        ui_load_failed(true);
    assert(!rebuilds && ui_rebuild_owed && ui_active->stat.ready);
    assert(!ui_generation_fresh && ui_build == ui_active);
    int tries = prepares;
    EVERY_SECOND(60, ui_rebuild_run());
    assert(!rebuilds && prepares - tries >= 2 && prepares - tries <= 5);
    prepare_rc = DBGEN_OK;
    EVERY_SECOND(300, ui_rebuild_run());
    assert(rebuilds == 1 && !ui_rebuild_owed && !ui_active->stat.ready);
    assert(ui_build != ui_active && ui_generation_fresh);
    /* MED-C: the fresh rebuild first exports the active library's master
     * on disk into its directory (the import reads it), once per request;
     * an export that fails (I/O) waits under the begin backoff, no
     * candidate begun, the active library kept */
    assert(exports == 1);
    reset();
    ui_active->stat.ready = true;
    export_rc = -1;
    for (int i = 0; i < 3; i++)
        ui_load_failed(true);
    assert(exports == 1 && !prepares && ui_rebuild_owed && ui_active->stat.ready);
    EVERY_SECOND(60, ui_rebuild_run());
    assert(exports >= 2 && exports <= 6 && !prepares && !rebuilds);
    tries = exports;
    export_rc = 0;                      /* unreadable for good: go on */
    prepare_rc = DBGEN_BUSY;            /* ... but the prepare waits */
    EVERY_SECOND(300, ui_rebuild_run());
    assert(exports == tries + 1 && prepares >= 2 && !rebuilds);
    prepare_rc = DBGEN_OK;
    EVERY_SECOND(300, ui_rebuild_run());
    assert(exports == tries + 1 && rebuilds == 1 && !ui_rebuild_exported);
    assert(ui_candidate_exported == 0);   /* unreadable: the best there is */
    /* R7b-1b: a complete export of the active master is the input */
    reset();
    ui_active->stat.ready = true;
    for (int i = 0; i < 3; i++)
        ui_load_failed(true);
    assert(rebuilds == 1 && ui_candidate_exported == 2);
    reset();                            /* a master with no statistics */
    ui_active->stat.ready = true;
    export_rc = 2;
    for (int i = 0; i < 3; i++)
        ui_load_failed(true);
    assert(rebuilds == 1 && ui_candidate_exported == 3);
    /* R7b-1a: a fresh build commits above every line there was: the
     * active master did not load (0), old manifests and exports did */
    reset();
    ui_active->master.commitid = 0;
    newest_manifest = 12;
    newest_export = 15;
    ui_generation_fresh = true;
    assert(ui_candidate_begin() && ui_build->master.commitid == 15);
    reset();
    ui_active->master.commitid = 20;
    newest_manifest = 12;
    newest_export = 15;
    ui_generation_fresh = true;
    assert(ui_candidate_begin() && ui_build->master.commitid == 20);
    /* A failing candidate load rebuilds the candidate only: the active
     * library is not declared unusable. */
    reset();
    ui_build = &ui_context_b;
    ui_active->stat.ready = true;
    for (int i = 0; i < 3; i++)
        ui_load_failed(true);
    assert(rebuilds == 1 && !ui_generation_fresh && ui_active->stat.ready);
    /* MED-3: while the active library's replay waits its backoff, no
     * candidate's statistics are prepared (a full filename table and a
     * journal replay each time), and a failing replay or candidate
     * prepare waits its backoff, not a second. */
    reset();
    ui_build = &ui_context_b;
    ui_build->stat.ready = ui_build->stat.ramcache = true;
    ui_active->stat.ramcache = true;
    ui_boot_stats_replayed = false;
    ui_pre_pending = true;
    ui_candidate_changed = true;        /* a candidate with new rows */
    prepare_ok = 0;
    stats_prepares = 0;
    EVERY_SECOND(300, ui_precompute_run());
    assert(stats_prepares >= 2 && stats_prepares <= 9);  /* the replay's tries */
    assert(ui_pre_pending && ui_build != ui_active);
    ui_boot_stats_replayed = true;              /* the replay is done */
    stats_prepares = ram_yields = 0;
    EVERY_SECOND(300, ui_precompute_run());
    assert(stats_prepares >= 2 && stats_prepares <= 9);  /* candidate's tries */
    assert(ui_build != ui_active);
    /* P16: every refused (due) prepare offers the candidate's RAM back */
    assert(ram_yields == stats_prepares);
    /* an update clone that wrote nothing is dropped, not prepared and
     * published as an identical generation; the active library's lists
     * are asked for again only if still owed */
    reset();
    ui_build = &ui_context_b;
    ui_build->stat.ready = ui_build->stat.ramcache = true;
    ui_active->stat.ramcache = true;
    ui_pre_pending = true;
    ui_candidate_changed = false;
    ui_active_pre_done = true;
    stats_prepares = stages = 0;
    ui_precompute_run();
    assert(ui_build == ui_active && !stats_prepares && !stages && !ui_pre_pending);
    reset();
    ui_build = &ui_context_b;
    ui_build->stat.ready = ui_build->stat.ramcache = true;
    ui_active->stat.ramcache = true;
    ui_pre_pending = true;
    ui_active_pre_done = false;
    ui_precompute_run();
    assert(ui_build == ui_active && ui_pre_pending);
    /* ... but not while its commit or scan is still owed */
    reset();
    ui_build = &ui_context_b;
    ui_build->stat.ready = ui_build->stat.ramcache = true;
    ui_pre_pending = true;
    ui_commit_pending = true;
    ui_precompute_run();
    assert(ui_build != ui_active);
    ui_commit_pending = false;

    /* P16 gate fix 1: PLAY between a stopped commit and its publication.
     * The statistics prepare succeeds, then the publication is refused
     * (the view pool from free memory, audio on): the candidate yields
     * (the real ui_candidate_ram_yield() drops the prepared lookup and its
     * RAM, while audio is on); the precompute's tenancy ends each pass. */
    reset();
    ui_build = &ui_context_b;
    ui_build->stat.ready = ui_build->stat.ramcache = true;
    ui_active->stat.ramcache = true;
    ui_pre_pending = true;
    ui_candidate_changed = true;
    prepare_ok = 1;
    ui_stats_prepared = false;
    pool_full = 1;
    playing = 1;
    stats_prepares = ram_yields = lists_done = 0;
    ui_precompute_run();
    assert(stats_prepares == 1 && ui_stats_prepared);
    assert(ram_yields == 1 && lists_done == 1 && ui_build != ui_active);
    /* ... but a reader holding the active library is not a refusal for
     * memory: published within a second, no yield */
    ram_yields = 0;
    pool_full = 0;
    ui_active->operations = 1;
    ui_pre_pending = true;
    ui_precompute_run();
    assert(ram_yields == 0 && ui_build != ui_active && ui_publish_busy);
    ui_active->operations = 0;
    ui_pre_pending = true;
    ui_precompute_run();
    assert(ram_yields == 0 && ui_active == &ui_context_b && !ui_publish_busy);
    playing = 0;

    /* P16 gate fix 5: a play/stop loop. Each yield's reload waits a
     * doubling backoff; after UI_YIELD_MAX yields it stops retrying (the
     * update still owed: the candidate stays), until a new request or
     * USB resets the backoffs, when it counts anew. */
    reset();
    ui_build = &ui_context_b;
    ui_ram_yielded = ui_build;
    ui_yields = 0;
    window_open = declined = take_fits = 0;
    int reloads = 0;
    for (int cycle = 0; cycle < 10; cycle++)
    {
        ui_yield_note();                /* PLAY: yielded again */
        assert(!ui_yield_reload_due()); /* not at once: the backoff */
        EVERY_SECOND(600, if (ui_yield_reload_due()) { reloads++; break; });
    }
    assert(reloads == UI_YIELD_MAX - 1 && ui_yields == 10);
    EVERY_SECOND(3600, assert(!ui_yield_reload_due()));
    assert(ui_ram_yielded == ui_build && ui_build != ui_active);
    memset(ui_retry, 0, sizeof(ui_retry));      /* USB, a new request */
    assert(ui_yield_reload_due());
    ui_yield_note();
    assert(ui_yields == 1);
    /* paused: only in an open window whose take would keep the floor
     * (decided with no disk access), never after a refusal this pause */
    current_tick += 600 * HZ;
    playing = 3;
    assert(!ui_yield_reload_due());
    window_open = 1;
    assert(!ui_yield_reload_due());
    take_fits = 1;
    assert(ui_yield_reload_due());
    declined = 1;
    assert(!ui_yield_reload_due());
    playing = window_open = declined = take_fits = 0;
    ui_ram_yielded = NULL;
    puts("tagcache retry: actual-C backoff/deferral/abandon/escalation checks pass");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="tagcache-retry-") as temporary:
    cfile = Path(temporary) / "retry.c"
    binary = Path(temporary) / "retry"
    cfile.write_text(harness)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-function", "-Iapps/ipodui",
                    str(cfile), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
