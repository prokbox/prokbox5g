/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2005 by Miika Pekkarinen
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
#ifdef HAVE_TAGCACHE
#ifndef _TAGCACHE_H
#define _TAGCACHE_H

#include "config.h"
#include "system.h"
#include "metadata.h"
#include "settings.h"

/**
 Note: When adding new tags, make sure to update index_entry_ec and tags_str in 
 tagcache.c and bump up the header version too.
 */
enum tag_type { tag_artist = 0, tag_album, tag_genre, tag_title,
    tag_filename, tag_composer, tag_comment, tag_albumartist, tag_grouping, tag_year,
    tag_discnumber, tag_tracknumber, tag_virt_canonicalartist, tag_bitrate, tag_length,
    tag_playcount, tag_rating, tag_playtime, tag_lastplayed, tag_commitid, tag_mtime,
    tag_lastelapsed, tag_lastoffset,
    /* Real tags end here, count them. */
    TAG_COUNT,
    /* Virtual tags */
    tag_virt_basename=TAG_COUNT,
    tag_virt_length_min, tag_virt_length_sec,
    tag_virt_playtime_min, tag_virt_playtime_sec,
    tag_virt_entryage, tag_virt_autoscore,
    TAG_COUNT_ALL};

/* How many entries to fetch to the seek table at once while searching. */
#define SEEK_LIST_SIZE 32

#define TAGCACHE_MAX_FILTERS 4
#define TAGCACHE_MAX_CLAUSES 32

/* Tag to be used on untagged files. */
#define UNTAGGED "<Untagged>"
/* Maximum length of a single tag. */
#define TAG_MAXLEN (MAX_PATH*2)
/* buffer size for all the (stack allocated & static) buffers handling tc data */
#define TAGCACHE_BUFSZ (TAG_MAXLEN+32)

/* Numeric tags (we can use these tags with conditional clauses). */
#define TAGCACHE_NUMERIC_TAGS ((1LU << tag_year) | (1LU << tag_discnumber) | \
    (1LU << tag_tracknumber) | (1LU << tag_length) | (1LU << tag_bitrate) | \
    (1LU << tag_playcount) | (1LU << tag_rating) | (1LU << tag_playtime) | \
    (1LU << tag_lastplayed) | (1LU << tag_commitid) | (1LU << tag_mtime) | \
    (1LU << tag_lastelapsed) | (1LU << tag_lastoffset) | \
    (1LU << tag_virt_length_min) | (1LU << tag_virt_length_sec) | \
    (1LU << tag_virt_playtime_min) | (1LU << tag_virt_playtime_sec) | \
    (1LU << tag_virt_entryage) | (1LU << tag_virt_autoscore))

#define TAGCACHE_IS_NUMERIC(tag) (BIT_N(tag) & TAGCACHE_NUMERIC_TAGS)

enum clause { clause_none, clause_is, clause_is_not, clause_gt, clause_gteq,
    clause_lt, clause_lteq, clause_contains, clause_not_contains, 
    clause_begins_with, clause_not_begins_with, clause_ends_with, clause_not_ends_with, 
	clause_oneof, clause_begins_oneof, clause_ends_oneof, clause_contains_oneof, 
	clause_not_oneof, clause_not_begins_oneof, clause_not_ends_oneof, clause_not_contains_oneof,
    clause_logical_or };

struct tagcache_stat {
    char db_path[MAX_PATHNAME+1];  /* Path to DB root directory */

    bool initialized;        /* Is tagcache currently busy? */
    bool readyvalid;         /* Has tagcache ready status been ascertained */
    bool ready;              /* Is tagcache ready to be used? */
    bool ramcache;           /* Is tagcache loaded in ram? */
    bool commit_delayed;     /* Has commit been delayed until next reboot? */
    bool econ;               /* Is endianess correction enabled? */
    volatile bool syncscreen;/* Synchronous operation with debug screen? */
    volatile const char 
        *curentry;           /* Path of the current entry being scanned. */

    int  commit_step;        /* Commit progress */
    int  ramcache_allocated; /* Has ram been allocated for ramcache? */
    int  ramcache_used;      /* How much ram has been really used */
    int  progress;           /* Current progress of disk scan */
    int  processed_entries;  /* Scanned disk entries so far */
    int  total_entries;      /* Total entries in tagcache */
    int  queue_length;       /* Command queue length */

    //const char *uimessage;   /* Pending error message. Implement soon. */
};

enum source_type {source_constant, 
                  source_runtime, 
                  source_current_path /* dont add items after this.
                                       it is used as an index 
                                       into id3_to_search_mapping */
                 };

struct tagcache_search_clause
{
    int tag;
    int type;
    bool numeric;
    int source;
    long numeric_data;
    char *str;
};

struct tagcache_seeklist_entry {
    int32_t seek;
    int32_t flag;
    int32_t idx_id;
};

#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE) && !defined(__PCTOOL__)
struct tagcache_context;
#endif
struct tagcache_search {
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE) && !defined(__PCTOOL__)
    /* Retained until search_finish; indices and filenames belong to this
     * snapshot even if a newer library is published while a read yields. */
    struct tagcache_context *context;
#endif
    /* For internal use only. */
    int fd, masterfd;
    int idxfd[TAG_COUNT];
    struct tagcache_seeklist_entry seeklist[SEEK_LIST_SIZE];
    int seek_list_count;
    int32_t filter_tag[TAGCACHE_MAX_FILTERS];
    int32_t filter_seek[TAGCACHE_MAX_FILTERS];
    int filter_count;
    struct tagcache_search_clause *clause[TAGCACHE_MAX_CLAUSES];
    int clause_count;
    int list_position;
    int seek_pos;
    long position;
    int entry_count;
    bool valid;
    bool initialized;
    uint32_t *unique_list;
    int unique_list_capacity;
    int unique_list_count;

    /* Exported variables. */
    bool ramsearch;      /* Is ram copy of the tagcache being used. */
    bool ramresult;      /* False if result is not static, and must be copied.
                            Currently always false since ramresult buffer is
                            movable */
    int type;            /* The tag type to be searched. Only nonvirtual tags */
    char *result;        /* The result data for all tags. */
    int result_len;      /* Length of the result including \0 */
    int32_t result_seek; /* Current position in the tag data. */
    int32_t idx_id;      /* Entry number in the master index. */
};

#ifdef __PCTOOL__
void tagcache_reverse_scan(void);
/* call this directly instead of tagcache_build in order to not pull
 * on global_settings */
void do_tagcache_build(const char *path[]);
#endif

const char* tagcache_tag_to_str(int tag);

bool tagcache_find_index(struct tagcache_search *tcs, const char *filename);
bool tagcache_check_clauses(struct tagcache_search *tcs,
                            struct tagcache_search_clause **clause, int count);
bool tagcache_search(struct tagcache_search *tcs, int tag);
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE) && !defined(__PCTOOL__)
bool tagcache_search_ctx(struct tagcache_context *ctx,
                         struct tagcache_search *tcs, int tag);
#endif
void tagcache_search_set_uniqbuf(struct tagcache_search *tcs,
                                 void *buffer, long length);
bool tagcache_search_add_filter(struct tagcache_search *tcs,
                                int tag, int seek);
bool tagcache_search_add_clause(struct tagcache_search *tcs,
                                struct tagcache_search_clause *clause);
bool tagcache_get_next(struct tagcache_search *tcs, char *buf, long size);
bool tagcache_retrieve(struct tagcache_search *tcs, int idxid, 
                       int tag, char *buf, long size);
void tagcache_search_finish(struct tagcache_search *tcs);
long tagcache_get_numeric(const struct tagcache_search *tcs, int tag);
long tagcache_increase_serial(void);
bool tagcache_import_changelog(void);
bool tagcache_create_changelog(struct tagcache_search *tcs);
void tagcache_update_numeric(int idx_id, int tag, long data);
#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE)
/* Runtime statistics (or a rating) of the file at 'path', found as
 * 'idx_id' in the snapshot 'token' names (tagcache_ui_bind_token(), 0 =
 * unknown). Never does disk I/O and never waits for a publication: the
 * audio thread calls it at every track change. A row it cannot bind from
 * RAM (another snapshot, no memo) goes to the tagcache thread, which binds
 * the full path; false only if that hand-off is full (dropped, counted). */
bool tagcache_ui_update_stats(const char *path, long idx_id, int32_t token,
                              int count, const unsigned char *tags,
                              const long *values);
/* OR'ed into a tag for tagcache_ui_update_stats(): the value is added to
 * the row's current value (play count, play time), never an absolute
 * read before the boot replay (#85 H3) */
#define TAGCACHE_UI_DELTA 0x80
/* The snapshot a successful tagcache_find_index() search belongs to */
int32_t tagcache_ui_bind_token(const struct tagcache_search *tcs);
#endif
bool tagcache_modify_numeric_entry(struct tagcache_search *tcs, 
                                   int tag, long data);

struct tagcache_stat* tagcache_get_stat(void);
int tagcache_get_commit_step(void);
bool tagcache_prepare_shutdown(void);
void tagcache_shutdown(void);
void tagcache_remove_statefile(void);

void tagcache_screensync_event(void);
void tagcache_screensync_enable(bool state);

#ifdef HAVE_TC_RAMCACHE
bool tagcache_is_in_ram(void);
#ifdef HAVE_DIRCACHE
bool tagcache_fill_tags(struct mp3entry *id3, const char *filename);
#endif
void tagcache_unload_ramcache(void);
#endif
void tagcache_commit_finalize(void);
void tagcache_init(void) INIT_ATTR;
bool tagcache_is_initialized(void);
bool tagcache_is_fully_initialized(void);
bool tagcache_is_usable(void);
void tagcache_start_scan(void);
void tagcache_stop_scan(void);
/* Called immediately before USB ownership is acknowledged. This is a
 * lock-free cancellation request: a long scan/commit checks it at its
 * existing checkpoints, then handles SYS_USB_CONNECTED and waits. */
void tagcache_usb_prepare(void);
/* Clears the GUI-side cancellation request once its USB screen has ended.
 * The tagcache thread also clears this after its own disconnect wait. */
void tagcache_usb_finish(void);
bool tagcache_update(void);
bool tagcache_rebuild(void);
#ifdef HAVE_IPOD_UI
/* User-requested rebuild: boosts the otherwise background UI scan. */
bool tagcache_rebuild_maintenance(void);
/* True from the UI request until its replacement database is complete. */
bool tagcache_maintenance_pending(void);
/* Directory scan in progress, even while the old RAM library is usable. */
bool tagcache_scan_active(void);
/* 3.20: the library is being updated (a scan past its read-only probe, a
 * commit, a reload, the lists' precompute, the changelog import). Any
 * thread; volatile reads only, no I/O. */
bool tagcache_ui_updating(void);
/* #84: "._" sidecars the last complete scan walked past; -1: none yet */
int tagcache_ui_sidecars(void);
void tagcache_ui_sidecars_reset(void);
/* A scan is owed (posted, or deferred while audio holds the memory). */
bool tagcache_ui_scan_owed(void);
#ifdef HAVE_TC_RAMCACHE
/* #87/#89 (apps/ipodui/bootstamp.h): the boot window starts the library's
 * boot check at once; its state: 0 pending, 1 walking (permille: the
 * walk's progress, -1 unknown), 2 done (skipped, walked or nothing to do) */
void tagcache_boot_window(void);
int tagcache_boot_check(int *permille);
#endif
#endif
int tagcache_get_max_commit_step(void);
#ifdef HAVE_IPOD_UI
/* True for podcasts and audiobooks: they keep a resume position. */
bool tagcache_ui_resumes(const struct mp3entry *id3);
#endif

#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE) && !defined(__PCTOOL__)
/* iPod UI shell (apps/ipodui/library.c): classification flags stored in
 * the index flag word at scan time, and read-only access to the RAM
 * cache. These read-only RAM getters do not yield;
 * returned pointers point into the movable RAM cache and are valid only
 * until the caller yields. */
#define TAGCACHE_UI_AUDIOBOOK   0x0020 /* mp3entry.is_audiobook or genre Audiobook */
#define TAGCACHE_UI_PODCAST     0x0040 /* genre Podcast or a /Podcasts/ folder */
#define TAGCACHE_UI_COMPILATION 0x0080 /* album artist "Various Artists" */
/* Phase 14a (docs/phase14a-brief.md 3.2, 4.2), set by ui_classify() from
 * vclass_classify(). The flag word keeps its low 16 bits from disk. */
#define TAGCACHE_UI_VIDEO       0x0100 /* plays on the video screen; never music */
#define TAGCACHE_UI_TV          0x0200 /* with VIDEO: a TV episode */
#define TAGCACHE_UI_VIDEO_CHECKED 0x0400 /* container video detection completed */
/* A video the player cannot play (not avc1/avc3; Videos builds only): in
 * neither Music nor Videos. Music excludes TAGCACHE_UI_ANY_VIDEO. */
#define TAGCACHE_UI_VIDEO_OTHER 0x1000
#define TAGCACHE_UI_ANY_VIDEO   (TAGCACHE_UI_VIDEO | TAGCACHE_UI_VIDEO_OTHER)
/* Bytes per entry the shell's library view pool (library.c, struct rec)
 * takes once a reload makes the database bigger: a commit or reload while
 * audio runs must leave room for it too. */
#define TAGCACHE_UI_VIEW_BYTES  12
/* playback.c (audio thread), right after a track's metadata was read:
 * looks up its index while the disk is awake, so that its statistics
 * event at the track change finds it without a read (S36, C1) */
void tagcache_ui_prefetch_index(const char *filename);

/* Changes whenever the RAM cache is loaded or dropped (entry numbers and
 * seeks from an older generation are meaningless). */
int  tagcache_ui_generation(void);
/* True while the first build or a RAM cache (re)load is pending. */
bool tagcache_ui_busy(void);
/* P18 (#86): a commit or reload owed (the sleep-entry window's job), and
 * the sleep-entry subset's library part (UI thread; boots: Q11 wakes) */
bool tagcache_ui_sleep_owed(void);
struct mp3entry;
void tagcache_ui_sleep_prepare(const struct mp3entry *id3);
bool tagcache_ui_sleep_walk(unsigned boots);
bool tagcache_ui_sleep_walking(void);
bool tagcache_ui_sleep_rebuild(void);
void tagcache_ui_sleep_cut(bool cut);
bool tagcache_ui_sleep_busy(void);
bool tagcache_ui_sleep_drained(void);
void tagcache_ui_sleep_woke(void);
/* The library's last empty window (8b): ticks of the post-USB scan's
 * start, of the RAM cache drop and of its reload (0 = none yet); 'ready'
 * is 0 while the window is open. */
void tagcache_ui_window(long *scan, long *empty, long *ready);
/* Number of master index entries, or -1 if the RAM cache is not usable. */
int  tagcache_ui_count(void);
/* Tag seeks (numeric tags hold their value) of entry 'idx_id', or NULL
 * if it is deleted or out of range. '*flag' gets the flag word. */
const int32_t *tagcache_ui_seeks(int idx_id, int32_t *flag);
/* Numeric tag, including updates still queued for the disk. Enqueue updates
 * its owning RAM snapshot immediately; this getter does not block or yield. */
long tagcache_ui_numeric(int idx_id, int tag);
/* String of a string tag (not tag_filename) at 'seek', or NULL if it is
 * untagged or unavailable. */
const char *tagcache_ui_str(int tag, int32_t seek);
/* Walks a string tag file in stored order -- with HAVE_IPOD_UI that is
 * the ui_sortkey() order, applied at commit. Start with *pos = 0; each
 * call gives the next entry's master index in *idx_id (-1 for the
 * uniqued tags) and advances *pos (the entry's seek is the old *pos).
 * False at the end. */
bool tagcache_ui_walk(int tag, int32_t *pos, int32_t *idx_id);
/* The shell's precomputed lists (apps/ipodui/libfile.h). The commit id of
 * the database in RAM, as it was loaded: with tagcache_ui_count() and
 * SORTKEY_VERSION, the stamp of the file made from it. */
int32_t tagcache_ui_commitid(void);
/* Explicit snapshot access for the worker's candidate precompute. Passing
 * another context never changes which context ordinary UI calls observe. */
struct tagcache_context *tagcache_context_active(void);
struct tagcache_context *tagcache_context_acquire(void);
void tagcache_context_release(struct tagcache_context *ctx);
struct tagcache_context *tagcache_context_acquire_passive(void);
void tagcache_context_release_passive(struct tagcache_context *ctx);
bool tagcache_context_published(struct tagcache_context *ctx);
int tagcache_ui_generation_ctx(struct tagcache_context *ctx);
int tagcache_ui_count_ctx(struct tagcache_context *ctx);
int32_t tagcache_ui_commitid_ctx(struct tagcache_context *ctx);
const char *tagcache_ui_db_path_ctx(struct tagcache_context *ctx);
const int32_t *tagcache_ui_seeks_ctx(struct tagcache_context *ctx,
                                   int idx_id, int32_t *flag);
long tagcache_ui_numeric_ctx(struct tagcache_context *ctx, int idx_id, int tag);
bool tagcache_ui_walk_ctx(struct tagcache_context *ctx, int tag,
                         int32_t *pos, int32_t *idx_id);
const char *tagcache_ui_str_ctx(struct tagcache_context *ctx,
                               int tag, int32_t seek);
/* The database directory (where the precompute file lives). */
const char *tagcache_ui_db_path(void);
/* The file was found damaged: the tagcache thread rewrites it (within a
 * second, once audio is stopped). Any thread. */
void tagcache_ui_precompute_request(void);
/* P19 19-1a: work is owed to the tagcache thread (any context): ends its
 * quiet wait at once; nothing if the thread is running or polling */
void tagcache_quiet_kick(void);
/* Entries of the database on disk (master index header, plus a scan
 * waiting for its commit), read synchronously; -1 if there is none. Any
 * thread; for sizing the shell's reservations at start (library.c). */
long tagcache_ui_disk_entries(void);
#endif /* HAVE_IPOD_UI */
#endif
#endif
