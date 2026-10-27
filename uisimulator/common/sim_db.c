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
 * Simulator-only database hooks for the script driver (sim_script.c):
 * "wait_db", "build_db", "rebuild_db", "update_db", "update_db_wait",
 * "db_mark", "expect_db_grew", "expect_db_same", "expect_resume",
 * "import_changelog", "expect_playing", "rebuild_db_start", "dirty_db" and
 * "expect_lib_updating". Kept apart from sim_script.c
 * because tagcache.h pulls in apps/ headers (file.h) that clash with the host
 * stdio sim_script.c uses. Links in both the stock and the --ipodui sim.
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
#include "config.h"
#include "sim_db.h"
#include "audio.h"
#include "kernel.h"
#include <string.h>

#if defined(HAVE_TAGCACHE) && !defined(__PCTOOL__)
#include <stdio.h>
#include <stdint.h>
#include "file.h"
#include "tagcache.h"
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE)
#include "ipodui/library.h"
#include "core_alloc.h"
#include "metadata.h"
#endif

enum sim_db_state sim_db_state(void)
{
    struct tagcache_stat *st = tagcache_get_stat();

    if (tagcache_is_usable() && st->commit_step == 0)
        return SIM_DB_READY;
    if (st->commit_delayed)
        return SIM_DB_DELAYED;
    if (st->readyvalid && !st->ready && st->commit_step == 0
        && st->processed_entries == 0)
        return SIM_DB_MISSING;
    return SIM_DB_BUSY;
}

#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE)
extern long sim_tagcache_commits_ended(void);   /* tagcache.c */
#endif

long sim_db_commits(void)
{
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE)
    return sim_tagcache_commits_ended();
#else
    return -1;
#endif
}

void sim_db_build(void)
{
    tagcache_rebuild();
}

void sim_db_update(void)
{
    tagcache_update();
}

void sim_db_rebuild(void)
{
    tagcache_rebuild();
}

static long sim_db_numeric(const char *path, int tag)
{
    struct tagcache_search tcs;
    long v;

    if (!tagcache_find_index(&tcs, path))
        return -2;
    v = tagcache_get_numeric(&tcs, tag);
    tagcache_search_finish(&tcs);
    return v;
}

long sim_db_lastelapsed(const char *path)
{
    return sim_db_numeric(path, tag_lastelapsed);
}

long sim_db_rating(const char *path)
{
    return sim_db_numeric(path, tag_rating);
}

bool sim_db_album(const char *path, char *buf, int size)
{
    struct tagcache_search tcs;
    bool ok;

    if (!tagcache_find_index(&tcs, path))
        return false;
    ok = tagcache_retrieve(&tcs, tcs.idx_id, tag_album, buf, size);
    tagcache_search_finish(&tcs);
    return ok;
}

bool sim_db_import(void)
{
    return tagcache_import_changelog();
}

int sim_db_entries(void)
{
    return tagcache_get_stat()->total_entries;
}

int sim_db_generation(void)
{
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE)
    return tagcache_ui_busy() ? -2 : tagcache_ui_generation();
#else
    return -1;
#endif
}

int sim_db_active_generation(void)
{
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE)
    return tagcache_ui_generation();
#else
    return -1;
#endif
}

bool sim_db_lib_updating(void)
{
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE)
    return lib_state() == LIB_UPDATING;
#else
    return false;
#endif
}

bool sim_db_set_dirty(void)
{
    /* struct master_header (tagcache.c): tagcache_header (magic,
     * datasize, entry_count), serial, commitid, dirty */
    const int32_t dirty = 1;
    char path[MAX_PATH];
    int fd;
    bool ok = false;

#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE)
    /* With library generations "the library on disk unusable" is every
     * generation's: a walk at the script's first boot may have published
     * a second one, which the next boot would fall back to (tc_stat is
     * the build context's). They sit beside the active one. */
    char root[MAX_PATH - 40];          /* room for the leaf below */
    char *slash;

    snprintf(root, sizeof(root), "%s",
             tagcache_ui_db_path_ctx(tagcache_context_active()));
    slash = strrchr(root, '/');
    if (slash && !strncmp(slash + 1, "database_gen", 12))
    {
        *slash = '\0';
        for (int g = 0; g < 8; g++)
        {
            snprintf(path, sizeof(path), "%s/database_gen%d/"
                     "database_idx.tcd", root, g);
            fd = open(path, O_WRONLY);
            if (fd < 0)
                continue;
            if (lseek(fd, 5 * sizeof(int32_t), SEEK_SET) >= 0
                && write(fd, &dirty, sizeof(dirty)) == sizeof(dirty))
                ok = true;
            close(fd);
        }
        if (ok)
            return true;
    }
#endif
    snprintf(path, sizeof(path), "%s/database_idx.tcd",
             tagcache_get_stat()->db_path);
    fd = open(path, O_WRONLY);
    if (fd < 0)
        return false;
    ok = lseek(fd, 5 * sizeof(int32_t), SEEK_SET) >= 0
         && write(fd, &dirty, sizeof(dirty)) == sizeof(dirty);
    close(fd);
    return ok;
}

bool sim_db_remove_active(const char *leaf)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", tagcache_get_stat()->db_path, leaf);
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE)
    snprintf(path, sizeof(path), "%s/%s",
             tagcache_ui_db_path_ctx(tagcache_context_active()), leaf);
#endif
    return remove(path) == 0;
}

#else /* no database */

enum sim_db_state sim_db_state(void)
{
    return SIM_DB_NONE;
}

long sim_db_commits(void)
{
    return -1;
}

void sim_db_build(void)
{
}

void sim_db_update(void)
{
}

void sim_db_rebuild(void)
{
}

long sim_db_lastelapsed(const char *path)
{
    (void)path;
    return -2;
}

long sim_db_rating(const char *path)
{
    (void)path;
    return -2;
}

bool sim_db_album(const char *path, char *buf, int size)
{
    (void)path; (void)buf; (void)size;
    return false;
}

bool sim_db_import(void)
{
    return false;
}

int sim_db_entries(void)
{
    return -1;
}

int sim_db_generation(void)
{
    return -1;
}

int sim_db_active_generation(void)
{
    return -1;
}

bool sim_db_lib_updating(void)
{
    return false;
}

bool sim_db_set_dirty(void)
{
    return false;
}

bool sim_db_remove_active(const char *leaf)
{
    (void)leaf;
    return false;
}

#endif

int sim_audio_status(void)
{
    return audio_status();
}

#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE) && defined(HAVE_TAGCACHE)
/* Simulator-only: deliberately keep a reader of the retired snapshot alive. */
static struct tagcache_search snapshot_search;
static bool snapshot_held;
static int snapshot_generation, snapshot_count;
static long snapshot_rating;
static const char snapshot_path[] =
    "/Library/Basalt/Columns/01 Narrow Echo.mp3";

bool sim_db_snapshot_begin(void)
{
    if (snapshot_held)
        tagcache_search_finish(&snapshot_search);
    snapshot_held = false;
    if (lib_state() != LIB_READY || tagcache_ui_busy()
        || !tagcache_find_index(&snapshot_search, snapshot_path))
        return false;
    snapshot_held = true;
    snapshot_generation = tagcache_ui_generation();
    snapshot_count = tagcache_ui_count();
    snapshot_rating = tagcache_get_numeric(&snapshot_search, tag_rating);
    return true;
}

bool sim_db_snapshot_switch(long timeout_ms)
{
    bool ok = false;
    long deadline = current_tick + timeout_ms * HZ / 1000;
    char path[MAX_PATH];

    if (!snapshot_held)
        return false;
    do
    {
        if (lib_state() != LIB_READY || tagcache_ui_count() < 0
            || !tagcache_is_usable()
            || !tagcache_retrieve(&snapshot_search, snapshot_search.idx_id,
                                  tag_filename, path, sizeof(path))
            || strcmp(path, snapshot_path)
            || tagcache_get_numeric(&snapshot_search, tag_rating)
               != snapshot_rating)
            break;
        if (tagcache_ui_generation() != snapshot_generation)
        {
            struct tagcache_search fresh;
            if (tagcache_ui_count() > snapshot_count
                && tagcache_find_index(&fresh, snapshot_path))
            {
                ok = tagcache_get_numeric(&fresh, tag_rating) == snapshot_rating;
                tagcache_search_finish(&fresh);
            }
            break;
        }
        sleep(1);
    } while (TIME_BEFORE(current_tick, deadline));
    tagcache_search_finish(&snapshot_search);
    snapshot_held = false;
    return ok;
}

extern void sim_tagcache_snapshot_state(long state[6]);
static struct tagcache_context *stress_operation;
static int stress_generation, stress_entries;
static size_t stress_free_min, stress_audio_start;
static long stress_ram_peak;
static char stress_path[MAX_PATH];
static struct mp3entry stale_track;
static int stale_generation, stale_row;
static int32_t stale_token;
static long stale_neighbor_rating;
static char stale_neighbor[MAX_PATH];
static int deferred_generation;

bool sim_db_snapshot_play(void)
{
    int view = lib_open(LIB_SONGS, NULL);
    bool ok = view > 0 && lib_play(view, 0, false) == 0;
    if (view > 0)
        lib_close(view);
    return ok;
}

void sim_db_snapshot_pause(void) { audio_pause(); }

bool sim_db_snapshot_deferred(long timeout_ms)
{
    deferred_generation = tagcache_ui_generation();
    size_t bytes = audio_buffer_size();
    long deadline = current_tick + timeout_ms * HZ / 1000;
    bool ok = bytes > 0 && audio_status() != 0;
    while (ok && TIME_BEFORE(current_tick, deadline))
    {
        ok = audio_status() != 0 && audio_buffer_size() >= bytes
            && tagcache_ui_generation() == deferred_generation
            && lib_state() == LIB_READY && tagcache_ui_count() >= 0;
        sleep(1);
    }
    fprintf(stderr, "snapshot deferred ram_mb=%d audio=%lu status=%d ok=%d\n",
            MEMORYSIZE, (unsigned long)bytes, audio_status(), ok);
    return ok;
}

bool sim_db_snapshot_after_playback(long timeout_ms)
{
    long deadline = current_tick + timeout_ms * HZ / 1000;
    bool ok = true;
    while (ok && tagcache_ui_generation() == deferred_generation
           && TIME_BEFORE(current_tick, deadline))
    {
        ok = lib_state() == LIB_READY && tagcache_ui_count() >= 0;
        sleep(1);
    }
    return ok && tagcache_ui_generation() != deferred_generation
        && lib_state() == LIB_READY && tagcache_ui_count() >= 0;
}

static void snapshot_sample(void)
{
    long state[6];
    sim_tagcache_snapshot_state(state);
    size_t available = core_available();
    if (available < stress_free_min)
        stress_free_min = available;
    if (state[0] + state[1] > stress_ram_peak)
        stress_ram_peak = state[0] + state[1];
}

bool sim_db_snapshot_stress_begin(void)
{
    struct tagcache_search s;
    if (stress_operation || lib_state() != LIB_READY
        || !tagcache_search(&s, tag_filename))
        return false;
    bool ok = tagcache_get_next(&s, stress_path, sizeof(stress_path));
    tagcache_search_finish(&s);
    if (!ok)
        return false;
    stress_generation = tagcache_ui_generation();
    stress_entries = tagcache_ui_count();
    stress_free_min = core_available();
    stress_audio_start = audio_buffer_size();
    stress_ram_peak = 0;
    stress_operation = tagcache_context_acquire();
    snapshot_sample();
    return true;
}

bool sim_db_snapshot_stress_switch(long timeout_ms)
{
    long deadline = current_tick + timeout_ms * HZ / 1000;
    bool blocked = false, ok = stress_operation != NULL;
    long state[6];
    while (ok && TIME_BEFORE(current_tick, deadline))
    {
        snapshot_sample();
        sim_tagcache_snapshot_state(state);
        if (lib_state() != LIB_READY || tagcache_ui_count() != stress_entries
            || tagcache_ui_generation() != stress_generation)
        {
            ok = false;
            break;
        }
        if (state[4])
        {
            /* A real yielding library filename/art-key resolution while
             * publication is held at the short-operation boundary. */
            int v = lib_open(LIB_SONGS, NULL);
            char path[MAX_PATH];
            blocked = v > 0 && lib_count(v) > 0
                && lib_path(v, 0, path, sizeof(path)) != NULL;
            if (blocked)
                (void)lib_art_key(v, 0);
            if (v > 0)
                lib_close(v);
            break;
        }
        sleep(1);
    }
    if (stress_operation)
        tagcache_context_release(stress_operation);
    stress_operation = NULL;
    ok = ok && blocked;
    while (ok && tagcache_ui_generation() == stress_generation
           && TIME_BEFORE(current_tick, deadline))
    {
        snapshot_sample();
        ok = lib_state() == LIB_READY && tagcache_is_usable();
        sleep(1);
    }
    snapshot_sample();
    ok = ok && tagcache_ui_generation() != stress_generation
        && lib_state() == LIB_READY && tagcache_ui_count() > stress_entries;
    fprintf(stderr, "snapshot stress ram_mb=%d entries=%d peak_cache=%ld "
            "free_min=%lu audio_start=%lu audio_end=%lu blocked=%d\n",
            MEMORYSIZE, stress_entries, stress_ram_peak,
            (unsigned long)stress_free_min, (unsigned long)stress_audio_start,
            (unsigned long)audio_buffer_size(), blocked);
    return ok;
}

bool sim_db_stale_begin(void)
{
    struct tagcache_search s;
    const char *path = "/Library/Basalt/Columns/02 Tender Window.mp3";
    memset(&stale_track, 0, sizeof(stale_track));
    strcpy(stale_track.path, path);
    if (!tagcache_find_index(&s, path))
        return false;
    stale_row = s.idx_id;
    stale_generation = tagcache_ui_generation();
    stale_token = tagcache_ui_bind_token(&s);
    char removed[MAX_PATH];
    bool preceding = stale_row > 0
        && tagcache_retrieve(&s, 0, tag_filename, removed, sizeof(removed));
    tagcache_search_finish(&s);
    fprintf(stderr, "snapshot stale captured row=%d generation=%d path=%s\n",
            stale_row, stale_generation, stale_track.path);
    /* Delete a preceding entry only on this disposable simulator disk. A
     * full rebuild must then move the buffered track's numeric row. */
    /* Keep the file for restoration across the test's reboot. */
    int fd = open("/.rockbox/snapshot-removed-path", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    bool saved = preceding && fd >= 0
        && write(fd, removed, strlen(removed) + 1) == (long)strlen(removed) + 1;
    if (fd >= 0)
        close(fd);
    return saved && rename(removed, "/zz-stale-backup.dat") == 0;
}

bool sim_db_stale_restore(void)
{
    char path[MAX_PATH];
    int fd = open("/.rockbox/snapshot-removed-path", O_RDONLY);
    if (fd < 0)
        return false;
    int n = read(fd, path, sizeof(path));
    close(fd);
    if (n <= 1 || path[n - 1] != '\0'
        || rename("/zz-stale-backup.dat", path) < 0)
        return false;
    return remove("/.rockbox/snapshot-removed-path") == 0;
}

bool sim_db_stale_finish(long timeout_ms)
{
    long deadline = current_tick + timeout_ms * HZ / 1000;
    while (tagcache_ui_generation() == stale_generation
           && TIME_BEFORE(current_tick, deadline))
        sleep(1);
    struct tagcache_search s;
    bool ok = tagcache_ui_generation() != stale_generation
        && tagcache_find_index(&s, stale_track.path);
    if (!ok)
        return false;
    int new_row = s.idx_id;
    tagcache_search_finish(&s);
    fprintf(stderr, "snapshot stale rebuilt old_row=%d new_row=%d\n",
            stale_row, new_row);
    if (new_row == stale_row
        || !tagcache_search(&s, tag_filename))
        return false;
    ok = tagcache_retrieve(&s, stale_row, tag_filename,
                          stale_neighbor, sizeof(stale_neighbor));
    tagcache_search_finish(&s);
    stale_neighbor_rating = sim_db_rating(stale_neighbor);
    fprintf(stderr, "snapshot stale neighbor ok=%d path=%s rating=%ld\n",
            ok, stale_neighbor, stale_neighbor_rating);
    if (!ok || !strcmp(stale_neighbor, stale_track.path))
        return false;
    /* The buffered row and its old snapshot: never written as a row of
     * the new one; the tagcache thread binds the full path instead. */
    unsigned char tag = tag_rating;
    long value = 8;
    ok = tagcache_ui_update_stats(stale_track.path, stale_row, stale_token,
                                  1, &tag, &value);
    sleep(HZ + HZ / 2);
    ok = ok && sim_db_rating(stale_track.path) == 8
        && sim_db_rating(stale_neighbor) == stale_neighbor_rating;
    fprintf(stderr, "snapshot stale buffered_row=%d active_row=%d "
            "target_rating=%ld neighbor_rating=%ld ok=%d\n", stale_row,
            new_row, sim_db_rating(stale_track.path),
            sim_db_rating(stale_neighbor), ok);
    return ok;
}
#else
bool sim_db_snapshot_begin(void) { return false; }
bool sim_db_snapshot_switch(long timeout_ms)
{
    (void)timeout_ms;
    return false;
}
bool sim_db_snapshot_stress_begin(void) { return false; }
bool sim_db_snapshot_stress_switch(long timeout_ms)
{ (void)timeout_ms; return false; }
bool sim_db_stale_begin(void) { return false; }
bool sim_db_stale_restore(void) { return false; }
bool sim_db_stale_finish(long timeout_ms)
{ (void)timeout_ms; return false; }
bool sim_db_snapshot_deferred(long timeout_ms)
{ (void)timeout_ms; return false; }
bool sim_db_snapshot_after_playback(long timeout_ms)
{ (void)timeout_ms; return false; }
bool sim_db_snapshot_play(void) { return false; }
void sim_db_snapshot_pause(void) { }
#endif
