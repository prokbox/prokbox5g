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

/* Host fakes for tests/ipodui/test_library.c: an in-memory tagcache RAM
 * cache (the tagcache_ui_* API of apps/tagcache.h), movable core_alloc
 * blocks, a recording playlist, the playing track and cpu_boost.
 *
 * The database mimics what library.c relies on:
 *  - string tags are uniqued per tag (equal strings, equal seek); seek 0
 *    of every tag is "<Untagged>" (tagcache_ui_str() gives NULL);
 *  - numeric tags hold their value in the seek array;
 *  - the title file is walked in ui_sortkey() order;
 *  - classification flags are set the way tagcache.c sets them at scan
 *    time (docs: apps/ipodui/library.h): genre Podcast or a /Podcasts/
 *    path -> podcast; else genre Audiobook (or an .m4b) -> audiobook;
 *    album artist "Various Artists" -> compilation;
 *  - deleted entries give NULL seeks.
 * Every core_alloc() and yield() moves all unpinned blocks (the old copy
 * is poisoned and freed): a pointer library.c kept across one of them
 * reads garbage, which the tests notice. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "core_alloc.h"
#include "tagcache.h"
#include "playlist.h"
#include "audio.h"
#include "ipodui/anim.h"
#include "sortkey.h"
#include "fake_tagcache.h"

/* -- database ---------------------------------------------------------- */

#define STR_TAGS 7              /* tag_artist .. tag_albumartist */
#define MAX_STRS 65536

static struct fake_entry *db;
static int db_n;
static int32_t (*seeks)[TAG_COUNT];
static int32_t *flags;
static bool *deleted;
static const char **strs[STR_TAGS];     /* per tag: seek -> string */
static int nstrs[STR_TAGS];
static int32_t *title_order;            /* master indices, title order */
static int generation = 1;
static bool usable = true;

struct tagcache_context { int unused; };
static struct tagcache_context context;
struct tagcache_context *tagcache_context_active(void) { return &context; }
struct tagcache_context *tagcache_context_acquire(void) { return &context; }
bool tagcache_search_ctx(struct tagcache_context *ctx,
                         struct tagcache_search *tcs, int tag)
{ (void)ctx; return tagcache_search(tcs, tag); }
void tagcache_context_release(struct tagcache_context *ctx) { (void)ctx; }
bool tagcache_context_published(struct tagcache_context *ctx)
{ (void)ctx; return false; }
int tagcache_ui_generation_ctx(struct tagcache_context *ctx)
{ (void)ctx; return tagcache_ui_generation(); }
int tagcache_ui_count_ctx(struct tagcache_context *ctx)
{ (void)ctx; return tagcache_ui_count(); }
int32_t tagcache_ui_commitid_ctx(struct tagcache_context *ctx)
{ (void)ctx; return tagcache_ui_commitid(); }
const char *tagcache_ui_db_path_ctx(struct tagcache_context *ctx)
{ (void)ctx; return tagcache_ui_db_path(); }
const int32_t *tagcache_ui_seeks_ctx(struct tagcache_context *ctx,
                                   int idx_id, int32_t *flag)
{ (void)ctx; return tagcache_ui_seeks(idx_id, flag); }
long tagcache_ui_numeric_ctx(struct tagcache_context *ctx, int idx_id, int tag)
{ (void)ctx; return tagcache_ui_numeric(idx_id, tag); }
bool tagcache_ui_walk_ctx(struct tagcache_context *ctx, int tag,
                         int32_t *pos, int32_t *idx_id)
{ (void)ctx; return tagcache_ui_walk(tag, pos, idx_id); }
const char *tagcache_ui_str_ctx(struct tagcache_context *ctx,
                               int tag, int32_t seek)
{ (void)ctx; return tagcache_ui_str(tag, seek); }

static int32_t intern(int tag, const char *s)
{
    int i;

    if (!s || !*s)
        return 0;
    if (tag != tag_title)       /* titles are per entry, as in tagcache */
        for (i = 1; i < nstrs[tag]; i++)
            if (!strcmp(strs[tag][i], s))
                return i;
    strs[tag][nstrs[tag]] = s;
    return nstrs[tag]++;
}

static int cmp_title(const void *a, const void *b)
{
    char ka[128], kb[128];
    int ia = *(const int32_t *)a, ib = *(const int32_t *)b, r;

    ui_sortkey(db[ia].title, ka, sizeof(ka));
    ui_sortkey(db[ib].title, kb, sizeof(kb));
    r = strcmp(ka, kb);
    return r ? r : ia - ib;
}

void fake_db_load(struct fake_entry *e, int n)
{
    int i, t;

    db = e;
    db_n = n;
    free(seeks);
    free(flags);
    free(deleted);
    free(title_order);
    seeks = calloc(n ? n : 1, sizeof(*seeks));
    flags = calloc(n ? n : 1, sizeof(*flags));
    deleted = calloc(n ? n : 1, sizeof(*deleted));
    title_order = calloc(n ? n : 1, sizeof(*title_order));
    for (t = 0; t < STR_TAGS; t++)
    {
        free(strs[t]);
        strs[t] = calloc(MAX_STRS, sizeof(char *));
        strs[t][0] = "<Untagged>";
        nstrs[t] = 1;
    }
    for (i = 0; i < n; i++)
    {
        const char *ext = strrchr(e[i].path ? e[i].path : "", '.');

        seeks[i][tag_artist] = intern(tag_artist, e[i].artist);
        seeks[i][tag_album] = intern(tag_album, e[i].album);
        seeks[i][tag_genre] = intern(tag_genre, e[i].genre);
        seeks[i][tag_title] = intern(tag_title, e[i].title);
        seeks[i][tag_filename] = 0;
        seeks[i][tag_composer] = intern(tag_composer, e[i].composer);
        seeks[i][tag_albumartist] = intern(tag_albumartist, e[i].albumartist);
        seeks[i][tag_discnumber] = e[i].disc;
        seeks[i][tag_tracknumber] = e[i].track;
        seeks[i][tag_length] = e[i].length;
        seeks[i][tag_mtime] = e[i].mtime;
        seeks[i][tag_lastelapsed] = e[i].lastelapsed;
        seeks[i][tag_lastoffset] = e[i].lastoffset;
        seeks[i][tag_playcount] = e[i].playcount;

        if ((e[i].genre && !strcasecmp(e[i].genre, "Podcast"))
            || (e[i].path && strstr(e[i].path, "/Podcasts/")))
            flags[i] = TAGCACHE_UI_PODCAST;
        else if ((e[i].genre && !strcasecmp(e[i].genre, "Audiobook"))
                 || (ext && !strcasecmp(ext, ".m4b")))
            flags[i] = TAGCACHE_UI_AUDIOBOOK;
        if (e[i].albumartist && !strcmp(e[i].albumartist, "Various Artists"))
            flags[i] |= TAGCACHE_UI_COMPILATION;
        deleted[i] = e[i].deleted;
        title_order[i] = i;
    }
    qsort(title_order, n, sizeof(*title_order), cmp_title);
    generation++;
    usable = true;
}

void fake_db_bump(void)
{
    generation++;
}

void fake_db_set_flags(int idx, int value)
{
    flags[idx] = value;
    generation++;
}

void fake_db_usable(bool on)
{
    usable = on;
    generation++;
}

/* A runtime statistic changed (as if queued by playback). */
void fake_db_set_stat(int idx, int tag, long value)
{
    seeks[idx][tag] = value;
}

static const char *db_dir = ".";
static int32_t commitid = 1;
int fake_pre_requests, fake_build_requests;

void fake_db_dir(const char *dir) { db_dir = dir; }
void fake_db_commit(void) { commitid++; }
int32_t tagcache_ui_commitid(void) { return commitid; }
const char *tagcache_ui_db_path(void) { return db_dir; }
void tagcache_ui_precompute_request(void) { fake_pre_requests++; }
long fake_disk_entries = -1;    /* no database on "disk" */
long tagcache_ui_disk_entries(void) { return fake_disk_entries; }
void artcache_build_request(void) { fake_build_requests++; }

bool tagcache_is_initialized(void) { return true; }
int  tagcache_get_commit_step(void) { return 0; }
int  tagcache_ui_generation(void) { return generation; }
bool tagcache_ui_busy(void) { return !usable; }
int  tagcache_ui_count(void) { return usable ? db_n : -1; }

const int32_t *tagcache_ui_seeks(int idx, int32_t *flag)
{
    if (!usable || idx < 0 || idx >= db_n || deleted[idx])
        return NULL;
    if (flag)
        *flag = flags[idx];
    return seeks[idx];
}

int fake_numeric_calls;

long tagcache_ui_numeric(int idx, int tag)
{
    const int32_t *s;

    fake_numeric_calls++;
    yield();            /* the real one may block on the queue's mutex */
    s = tagcache_ui_seeks(idx, NULL);
    return s ? s[tag] : -1;
}

const char *tagcache_ui_str(int tag, int32_t seek)
{
    if (!usable || tag < 0 || tag >= STR_TAGS || seek <= 0
        || seek >= nstrs[tag])
        return NULL;
    return strs[tag][seek];
}

bool tagcache_ui_walk(int tag, int32_t *pos, int32_t *idx)
{
    if (!usable || tag != tag_title || *pos >= db_n)
        return false;
    *idx = title_order[*pos];
    (*pos)++;
    return true;
}

bool tagcache_search(struct tagcache_search *tcs, int tag)
{
    tcs->type = tag;
    return usable;
}

void tagcache_search_finish(struct tagcache_search *tcs)
{
    tcs->type = -1;
}

bool tagcache_retrieve(struct tagcache_search *tcs, int idx, int tag,
                       char *buf, long size)
{
    (void)tcs;
    if (tag != tag_filename || idx < 0 || idx >= db_n)
        return false;
    /* "/e<idx>" first: the fake playlist reads the index back */
    snprintf(buf, size, "/e%d%s", idx, db[idx].path ? db[idx].path : "");
    return true;
}

/* -- movable memory ---------------------------------------------------- */

#define MAX_BLOCKS 64
static struct { void *p; size_t size; int pins; } blocks[MAX_BLOCKS];

static void move_all(void)
{
    int i;

    for (i = 1; i < MAX_BLOCKS; i++)
    {
        void *n;

        if (!blocks[i].p || blocks[i].pins)
            continue;
        n = malloc(blocks[i].size);
        memcpy(n, blocks[i].p, blocks[i].size);
        memset(blocks[i].p, 0xa5, blocks[i].size);
        free(blocks[i].p);
        blocks[i].p = n;
    }
}

int fake_blocks_used(void)
{
    int i, n = 0;

    for (i = 1; i < MAX_BLOCKS; i++)
        n += blocks[i].p != NULL;
    return n;
}

int fake_allocs;
int fake_alloc_fail;                    /* the next N core_alloc()s fail */

size_t core_allocatable(void)
{
    return 64 * 1024 * 1024;
}

int core_alloc(size_t size)
{
    int i;

    fake_allocs++;
    if (fake_alloc_fail > 0)
    {
        fake_alloc_fail--;
        return -1;
    }
    move_all();
    for (i = 1; i < MAX_BLOCKS && blocks[i].p; i++)
        ;
    if (i == MAX_BLOCKS)
        return -1;
    blocks[i].p = malloc(size ? size : 1);
    blocks[i].size = size;
    blocks[i].pins = 0;
    memset(blocks[i].p, 0x5a, size);
    return i;
}

int core_free(int h)
{
    if (h > 0 && h < MAX_BLOCKS && blocks[h].p)
    {
        memset(blocks[h].p, 0xa5, blocks[h].size);
        free(blocks[h].p);
        blocks[h].p = NULL;
    }
    return 0;
}

void *core_get_data(int h)
{
    return blocks[h].p;
}

void core_pin(int h)
{
    blocks[h].pins++;
}

void core_unpin(int h)
{
    blocks[h].pins--;
}

long current_tick = 1000;
int fake_yields;

void yield(void)
{
    size_t size = (db_n ? db_n : 1) * sizeof(*seeks);
    int32_t (*n)[TAG_COUNT] = malloc(size);

    fake_yields++;
    current_tick++;
    move_all();
    /* the RAM cache moves too (seek pointers held across a yield break) */
    memcpy(n, seeks, size);
    memset(seeks, 0xa5, size);
    free(seeks);
    seeks = n;
}

int fake_boost;

void cpu_boost(bool on)
{
    fake_boost += on ? 1 : -1;
}

/* -- playlist ---------------------------------------------------------- */

static struct playlist_info info = { 2000, 1 };
int fake_pl_n;
int fake_pl[40000];             /* master indices inserted, in order */
int fake_pl_start = -1, fake_pl_shuffled;
unsigned long fake_pl_elapsed;

struct playlist_info *playlist_get_current(void) { return &info; }
int playlist_amount(void) { return fake_pl_n; }

/* the library source (lib_shuffle.c, not linked here): records the call */
#include "lib_shuffle.h"
int fake_shuf_calls = 0, fake_shuf_mode = -1;
uint32_t fake_shuf_first;
int lib_shuffle_start(enum np_shuffle mode, uint32_t first)
{
    fake_shuf_calls++;
    fake_shuf_mode = (int)mode;
    fake_shuf_first = first;
    return 0;
}
/* the persisted book flag (P3): records the last note */
int fake_shuf_book = -1;
void lib_shuffle_note_book(bool book)
{
    fake_shuf_book = book;
}
bool lib_shuffle_resumed_book(void)
{
    return false;
}

int playlist_create(const char *dir, const char *file)
{
    (void)dir;
    (void)file;
    fake_pl_n = 0;
    fake_pl_start = -1;
    fake_pl_shuffled = 0;
    return 0;
}

int playlist_insert_context_create(struct playlist_info *playlist,
                                   struct playlist_insert_context *c,
                                   int position, bool queue, bool progress)
{
    (void)playlist;
    (void)position;
    (void)queue;
    if (progress)
        return -1;      /* the default context splashes: library must not */
    c->count = 0;
    return 0;
}

int playlist_insert_context_add(struct playlist_insert_context *c,
                                const char *filename)
{
    if (fake_pl_n >= info.max_playlist_size)
        return -1;
    fake_pl[fake_pl_n++] = atoi(filename + 2);
    c->count++;
    return 0;
}

void playlist_insert_context_release(struct playlist_insert_context *c)
{
    (void)c;
}

int playlist_shuffle(int seed, int start_index)
{
    (void)seed;
    fake_pl_shuffled = 1;
    return start_index < 0 ? 0 : start_index;
}

void playlist_start(int start_index, unsigned long elapsed,
                    unsigned long offset)
{
    (void)offset;
    fake_pl_start = start_index;
    fake_pl_elapsed = elapsed;
}

/* -- audio ------------------------------------------------------------- */

static struct mp3entry playing;
static int playing_on;

void fake_play(int idx, unsigned long elapsed)
{
    playing_on = idx >= 0;
    playing.tagcache_idx = idx + 1;
    playing.elapsed = elapsed;
}

int audio_status(void) { return playing_on; }
int fake_regrows;
void audio_buffer_regrow(void) { fake_regrows++; }
/* the paused maintenance window (apps/ipodui/maint.h, #88): never open
 * here while audio is on, so the library keeps its audio-on rules */
#include "ipodui/maint.h"
bool maint_window_open(void) { return !playing_on; }
bool maint_active(void) { return false; }
bool maint_take(enum maint_job j, size_t bytes)
{
    (void)j; (void)bytes;
    return !playing_on;
}

struct mp3entry *audio_current_track(void)
{
    return playing_on ? &playing : NULL;
}

/* -- trace ------------------------------------------------------------- */

int fake_trace_rows;
char fake_last_open[256], fake_last_pre[256], fake_last_pin[256];

void stub_trace(const char *fmt, ...)
{
    va_list ap;

    if (!strncmp(fmt, "lib row", 7))
        fake_trace_rows++;
    va_start(ap, fmt);
    if (!strncmp(fmt, "lib open", 8))
        vsnprintf(fake_last_open, sizeof(fake_last_open), fmt, ap);
    else if (!strncmp(fmt, "lib precompute", 14))
        vsnprintf(fake_last_pre, sizeof(fake_last_pre), fmt, ap);
    else if (!strncmp(fmt, "lib books pin", 13))    /* #67 */
        vsnprintf(fake_last_pin, sizeof(fake_last_pin), fmt, ap);
    va_end(ap);
    if (getenv("TEST_LIBRARY_TRACE"))
    {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        putchar('\n');
    }
}
