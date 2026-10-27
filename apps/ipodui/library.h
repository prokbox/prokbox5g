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
 * iPod UI shell
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
#ifndef _IPODUI_LIBRARY_H_
#define _IPODUI_LIBRARY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The music library, as sorted views over the tagcache RAM cache.
 *
 * Views
 *  - A view is an index array (one int32 master index entry per row,
 *    4 B x rows) plus a fixed label cache of LIB_CACHE_ROWS x
 *    LIB_CACHE_LEN bytes (~2 KB), inside the library's one movable
 *    core_alloc pool (12 B per database entry + 64 KB; see library.c for
 *    why one block). Nothing is held as strings; labels are copied out of
 *    the RAM cache on demand. Handles are small integers, not core_alloc
 *    handles.
 *  - Order is ui_sortkey() (sortkey.h): case-insensitive, accents folded,
 *    "The " ignored, "#" group after Z. tagcache stores its tag files in
 *    that order (sorted at commit, in the background), so title-ordered
 *    views (Songs; a genre's or composer's songs) are a walk of the title
 *    file with no sort at all. Other views sort once, at lib_open(), on
 *    8-byte key prefixes (12 B/row while lib_open() runs, trimmed to
 *    4 B/row before it returns); groups are de-duplicated first.
 *  - Group rows (artists, albums, genres, composers, books, shows) are
 *    represented by one of their tracks; lib_id() returns that track's
 *    master index, and a filter matches tracks whose tag (album artist,
 *    album + album artist, genre, composer) equals the representative's,
 *    case-insensitively.
 *
 * What each category lists
 *  - LIB_ARTISTS: album artists of music tracks, or the track artist when
 *    there is no album artist ("Albums under an artist use album artist").
 *    Compilations are left out.
 *  - LIB_ALBUMS: albums of music tracks, one row per album + album artist
 *    (so two "Greatest Hits" stay apart), compilations included.
 *  - LIB_COMPILATIONS: albums whose album artist is "Various Artists".
 *  - LIB_SONGS: music tracks. Ordered by title; under an album by disc and
 *    track number; under an artist without an album (All Songs) by album,
 *    then disc and track. With an album filter, any class (so a book's or
 *    show's tracks are LIB_SONGS of that album, too).
 *  - LIB_GENRES, LIB_COMPOSERS: tagged genres / composers of music tracks.
 *  - LIB_AUDIOBOOKS: unfiltered, one row per book (album + album artist;
 *    a file without an album is a book of its own, by title). With an
 *    album filter: that book's tracks in disc/track order.
 *  - LIB_PODCASTS: unfiltered, one row per show (album, as for books).
 *    With an album filter: the episodes, newest file first.
 *  Untagged artists/albums read "Unknown Artist" / "Unknown Album";
 *  untagged titles are the file name (stored at scan time).
 *
 * Classification (decided at scan time, stored in the index flag word,
 * see TAGCACHE_UI_* in tagcache.h):
 *  - podcast: genre "Podcast(s)" or a path containing "/Podcasts/";
 *  - audiobook (if not a podcast): mp3entry.is_audiobook (M4B brand,
 *    >= 2 chapters, or .m4b) or genre "Audiobook(s)";
 *  - compilation: album artist "Various Artists" or "Various" (tagcache
 *    has no compilation flag).
 *  Audiobooks and podcasts never appear in the music categories or in
 *  lib_shuffle_all().
 *
 * Database state
 *  - In the HAVE_IPOD_UI build the RAM cache and automatic updates are on
 *    by default (settings_list.c). When there is no usable database (first
 *    boot, or one from another build: the magic differs) the tagcache
 *    thread builds one silently about 1 s after boot, then loads it into
 *    RAM; later boots load it and rescan in the background, unboosted.
 *    No config.cfg entry is needed; "tagcache_ram: off" would make the
 *    library LIB_UNAVAILABLE.
 *  - Precomputed lists (Phase 4b, #37): after every RAM cache load and
 *    deleted-file pass, the tagcache thread writes every unfiltered
 *    category's rows, their track counts and art keys to a file
 *    (libfile.h; lib_precompute()); lib_open() of an unfiltered category
 *    reads it instead of scanning the RAM cache. Until it exists (audio
 *    playing, a first build), or if it is old or damaged, views are
 *    built by the scan as before, without counts and keys (-1 / 0);
 *    lib_generation() changes once the file is written, so screens
 *    reopen their views and pick the counts up.
 *  - Views read the RAM cache only. When it is dropped or reloaded (a
 *    commit after an update) open views go stale: lib_count() stays, labels
 *    still in the view's cache (the rows last drawn) stay, others come
 *    back empty, and lib_play() fails. Screens reopen their view once
 *    lib_state() is LIB_READY with a new lib_generation() (browse.c),
 *    keeping the selection through lib_find().
 *
 * Trace (UPDATE_TRACE, i.e. logf + DEBUGF, for tests/ui check_trace.py):
 *    lib open <cat> n=<rows> us=<t> yields=<y> src=<file|scan> el_us=<e>
 *                                     every lib_open(): t = microseconds
 *                                     computing (USEC_TIMER on the device,
 *                                     thread CPU time in the macOS sim),
 *                                     y = yields while building, src =
 *                                     precompute file or RAM cache scan,
 *                                     e = elapsed microseconds (file I/O)
 *    lib precompute <done|stopped> n=<entries> us=<t> el_ms=<e>
 *                   yields=<y> bytes=<b>
 *                                     every lib_precompute() that ran
 *                                     (tagcache thread)
 *    lib precompute current n=<entries>   the file on disk was current
 *    lib books order=<title|author|fallback> n=<rows>
 *                                     every unfiltered LIB_AUDIOBOOKS
 *                                     open from the file (#59)
 *    lib fetch n=<labels>             per lib_trace_frame() call that had
 *                                     cache misses (labels copied from
 *                                     tagcache since the previous call)
 *    lib row <cat> <label>            every label copied from tagcache
 *    lib play n=<tracks> us=<t> cpu_us=<c>
 *                                     lib_play() / lib_shuffle_all():
 *                                     elapsed and computing microseconds
 */

enum lib_cat {
    LIB_ARTISTS, LIB_ALBUMS, LIB_SONGS, LIB_GENRES, LIB_COMPOSERS,
    LIB_COMPILATIONS, LIB_AUDIOBOOKS, LIB_PODCASTS,
    /* P11 (docs/phase11-brief.md section 4.2), appended (no renumbering):
     * LIB_SEARCH, the one Search result view (rows SRCH_ROW(kind, idx),
     * search.h; built by lib_search(), never by lib_open()); LIB_OTG, the
     * On-The-Go list (otg_open_view(), otg.h; rows = resolved master
     * indices, unresolved rows carry the line number with a flag bit).
     * lib_label(), lib_sublabel(), lib_play() and lib_path() accept both:
     * a search row resolves by kind; lib_play() on a song or episode row
     * plays the matching rows of that kind from there. */
    LIB_SEARCH, LIB_OTG,
#ifdef HAVE_BCMFW_VIDEO
    /* Phase 14a (docs/phase14a-brief.md 3.3, 3.6, 4.4), appended; V3
     * implements them in library.c from the precompute file:
     * LIB_MOVIES      one row per movie (VIDEO without TV or PODCAST),
     *                 title order; lib_art_key() its poster.
     * LIB_TVSHOWS     one row per show (VIDEO | TV, grouped by the stored
     *                 show, the album tag); lib_child_count() = episodes;
     *                 lib_art_key() the show's art. lib_id() = the show id
     *                 a child view filters by (filter.album).
     * LIB_TVSEASONS   filter.album = a show: one row per season (the
     *                 stored discnumber), ascending; lib_id() = the
     *                 season number (filter.season of the episodes).
     * LIB_TVEPISODES  filter.album = a show, filter.season a season or
     *                 LIB_ANY (every season, as when the show has one):
     *                 its episodes ordered season, episode, title.
     * Video rows play through ui_video_play() (screens/video.h), never
     * lib_play()'s audio playlist. */
    LIB_MOVIES, LIB_TVSHOWS, LIB_TVSEASONS, LIB_TVEPISODES,
#endif
};
#define LIB_ANY (-1)
struct lib_filter {             /* ids from lib_id() of a parent view */
    int artist, album, genre, composer;   /* LIB_ANY = unfiltered */
#ifdef HAVE_BCMFW_VIDEO
    int season;                 /* LIB_TVEPISODES; LIB_ANY = all */
#endif
};
/* An unfiltered filter. Every initialiser of a whole struct lib_filter
 * uses it, so the video build's extra field is never left out (the five
 * 4-value initialisers in library.c become this: V3). */
#ifdef HAVE_BCMFW_VIDEO
#define LIB_FILTER_ANY { LIB_ANY, LIB_ANY, LIB_ANY, LIB_ANY, LIB_ANY }
#else
#define LIB_FILTER_ANY { LIB_ANY, LIB_ANY, LIB_ANY, LIB_ANY }
#endif
enum lib_state { LIB_READY, LIB_UPDATING, LIB_UNAVAILABLE };

#define LIB_CACHE_ROWS  24      /* label cache: ~2x the visible rows */
#define LIB_CACHE_LEN   80      /* longer labels are fetched every time */

enum lib_state lib_state(void);          /* cheap; poll from tick() */
/* Opens a view. Returns a handle > 0, or < 0 on error (logged). The
 * view's memory is core_alloc'd and freed by lib_close(). 'f' may be NULL
 * (unfiltered). */
int  lib_open(enum lib_cat cat, const struct lib_filter *f);
void lib_close(int view);
int  lib_count(int view);
/* Label for row idx (copied into buf; the view caches recent rows). */
const char *lib_label(int view, int idx, char *buf, size_t len);
/* The label (and sort key) of an untagged artist / album row. library.c
 * stays free of the language system (host test); a screen compares a
 * label with these and shows LANG_IPODUI_UNKNOWN_ARTIST / _ALBUM. */
extern const char lib_unknown_artist[];
extern const char lib_unknown_album[];
int  lib_id(int view, int idx);          /* id to filter children by */
char lib_letter(int view, int idx);      /* quick-scroll letter */
/* #59: the order of the unfiltered LIB_AUDIOBOOKS list (Settings >
 * Audiobooks > Sort By; global_settings.ipodui_ab_sort, which library.c
 * does not read: host test). Title: by title, then author (also the
 * scan's order). Author: by author (album artist, else artist; untagged
 * as lib_unknown_artist), then title; lib_letter() is then the author's.
 * Both orders are precomputed (libfile.h, LIBFILE_CAT_BOOKS_AUTHOR), so a
 * change never sorts; without the author slice the list stays in title
 * order (trace "lib books order=fallback"). UI thread; a change moves
 * lib_generation(), so an open books list reopens (keeping its
 * selection through lib_find()). */
enum lib_book_order { LIB_BOOKS_BY_TITLE, LIB_BOOKS_BY_AUTHOR };
void lib_set_book_order(int order);
/* #67 (Phase 8b §3.2, owner 2026-10-11): the last-played book is row 0
 * of the unfiltered LIB_AUDIOBOOKS view, whatever the order; the other
 * rows keep it. The book comes from ab_store_pin_key() (RAM only); the
 * same row moves (rows, counts, keys), before progress is computed; no
 * mark, and lib_letter() returns 0 for it. Not for filtered views,
 * Podcasts, or fewer than 2 rows. Traced "lib books pin row= key= src=
 * order= path=" or "lib books pin none why=<nokey|absent|small>".
 *
 * Pure (host-tested): the row whose key equals k in a view's key column,
 * stored as (low, high) 32-bit pairs, keys[2*i] = low, keys[2*i+1] =
 * high; -1 if absent, if k is 0, or if n < 2 (nothing to move). */
int lib_pin_find(const uint32_t *keys, int n, uint64_t k);
/* Books only: moves when the pin key changes. Checked only by
 * LIB_AUDIOBOOKS views (browse.c reopen_due()); lib_generation() is NOT
 * touched, so no other browse view and no Search reopens. UI thread. */
int lib_pin_generation(void);
/* Songs/audiobook/podcast views: replace the playlist with the view's
 * tracks, starting at idx (shuffled if 'shuffle'), and start playback.
 * Quiet: no splash. Returns 0 or < 0.
 * On a group view (books, shows, albums, ...) row idx stands for all of
 * its tracks; for a book or a show, playback resumes where it was left
 * (the most recently played track, at its saved position).
 * At most max_files_in_playlist tracks go in (2000 on this target): a
 * longer view plays the rows from idx on, or a random sample if shuffled.
 * Builds the playlist synchronously before playback starts (see
 * library.c for the measured cost). */
int  lib_play(int view, int idx, bool shuffle);
int  lib_shuffle_all(void);              /* all music (not books/podcasts) */
/* Audiobooks/podcasts: listened fraction 0..1000 from the saved resume
 * position, or -1 if unknown/never played. Book and show rows are
 * computed once, all together, by lib_open() (two passes over the
 * database); an episode's or a track's is one lookup. Never blocks on
 * the database except for that lookup. */
int  lib_progress(int view, int idx);

/* -- additions to the Phase 4 contract ---------------------------------- */

/* Audiobook/podcast views: recompute the progress of the row playing now
 * and of the row that showed a live position before, e.g. after Now
 * Playing (two passes over the database, those rows only). */
void lib_progress_refresh(int view);

/* Changes whenever the database in RAM is reloaded; views opened under an
 * older generation are stale (see above). */
int  lib_generation(void);
/* Row of 'view' whose lib_id() is 'id', or -1 (linear, ~4 B per row read:
 * for keeping the selection when a screen reopens its view). Ids survive
 * an incremental update (new entries are appended, deleted ones only
 * flagged), not a rebuild. */
int  lib_find(int view, int id);
/* Songs/audiobook/podcast views: full file path of row idx, copied into
 * buf. Returns buf, or NULL if the row has no file (e.g. non-song views)
 * -- logged. Reads the database's file name list on disk (file names are
 * not kept in RAM): call it on an action (On-The-Go), not while drawing. */
const char *lib_path(int view, int idx, char *buf, size_t len);
/* Call once after each drawn frame or batch of rows: logs
 * "lib fetch n=<labels>" if any label missed the cache since the last
 * call, then resets the count. Cheap; does nothing otherwise. */
void lib_trace_frame(void);

/* -- Phase 4b additions (docs/phase4b-brief.md) ------------------------- */

/* Tracks under a row, for its second line: an album's or a book's tracks
 * ("13 Songs"), a show's episodes ("7 Episodes"), an artist's songs (the
 * Artists list: what its "All Songs" holds). From the precompute (O(1));
 * -1 for other views, or until the precompute exists. In an artist's
 * album list the counts come from the precomputed album table at
 * lib_open() (a few small reads of the file). A count is of the row's
 * own class (an album's music tracks, a show's episodes). */
int  lib_child_count(int view, int idx);
/* Podcasts: an episode never played (no play counted, no saved position,
 * not playing now), or a show with such an episode. Live: an episode's
 * is read from the RAM index on each call (may block briefly, like
 * lib_progress()); a show's is computed at lib_open() with its progress
 * and refreshed by lib_progress_refresh(). false for other views. */
bool lib_unplayed(int view, int idx);
/* Album, compilation, book and show rows: the art cache key
 * (artcache_key() over the strings the row groups by; a show by its
 * album only; an untagged album by its first track's folder), from the
 * precompute: no disk read. 0 = none (or not computed yet). */
uint64_t lib_art_key(int view, int idx);
/* Second line of row idx, copied into buf, or NULL if the view's rows
 * have none: the album artist (else the artist) under Albums,
 * Compilations and Audiobooks rows ("Various Artists" for compilations;
 * a book's author), the artist under a song (the track artist, else the
 * album artist; Songs, an artist's, genre's or composer's songs; not an
 * album's track list). An empty string when untagged (or while the view
 * is stale): the caller shows LANG_IPODUI_UNKNOWN_ARTIST. RAM index
 * only, cached like labels (24 x 48 B per view that has second lines). */
const char *lib_sublabel(int view, int idx, char *buf, size_t len);

/* -- H1 additions (docs/phase-nav-brief.md, #41.2) ---------------------- */

/* Once, at shell start, before anything can start playback: reserves the
 * precompute's work block (20 B per entry + 6.6 KB) for the database on
 * disk + 25 % (tagcache_ui_disk_entries(), read synchronously), so the
 * precompute can run while audio plays. No database: nothing reserved
 * (the precompute then runs only while audio is stopped, in a transient
 * block). A second call replaces the block (host tests). */
void lib_reserve(void);
/* fb-stall: the view pool is reserved by lib_reserve() too, and re-sized
 * for a reloaded database only while audio is stopped (never shrinks the
 * audio buffer). The main loop calls this every pass: with audio stopped
 * it settles a size owed since a reload during playback. UI thread. */
void lib_pool_idle(void);

/* Tagcache thread only (the guarded hook in tagcache.c): recompute the
 * precompute file for the database in RAM, unless the file on disk is
 * already for it ('force': rewrite anyway, it was found damaged). Yields
 * every few thousand entries and returns LIB_PRE_WAIT as soon as 'stop'
 * says so (an event to handle); the caller retries. It runs at any time
 * in the block lib_reserve() reserved; a database bigger than that
 * block (or none reserved) needs a transient one, so then it waits while
 * audio plays and stops when playback starts (LIB_PRE_WAIT).
 * Calls lib_committed() when the file is current. */
enum { LIB_PRE_DONE, LIB_PRE_WAIT, LIB_PRE_ERROR };
int  lib_precompute(bool force, bool (*stop)(void));
struct tagcache_context;
bool lib_generation_pool_prepare(struct tagcache_context *ctx);
void lib_generation_pool_abandon(struct tagcache_context *ctx);
int lib_precompute_ctx(struct tagcache_context *ctx, bool force,
                       bool (*stop)(void));
/* After a commit + RAM cache load, once the precomputed lists (and the
 * album table the art builder reads) are current: asks the art cache to
 * build (artcache_build_request()). Called on the tagcache thread. */
void lib_committed(void);
/* Shows in the library (rows of LIB_PODCASTS), as of the last precompute
 * file written or found current; the last known value while a new one
 * waits, -1 before any. Cheap: no allocation, no storage read (panels). */
int  lib_podcast_count(void);
#ifdef SIMULATOR
int  lib_open_calls(void);              /* lib_open() calls so far */
#endif

/* -- Phase 5 additions (#27, #49; P) ------------------------------------- */

/* Album groups of the queue the last lib_play() built (Revision 1 item
 * 10): bit k of *bits set = queue entry k (insert order) starts an
 * album. False if the playlist is no longer that queue. No I/O. The bits
 * live in the queue's movable slot table (#109): valid until a yield. */
bool lib_queue_groups(const uint8_t **bits, int *n);
/* The playlist is the queue the last lib_play() built for one audiobook
 * (Revision 2: a book plays alone and ends by stopping, even with Repeat
 * All: playlist.c get_next_index()), and after a reboot the
 * resumed queue too, from the shuffle record's book flag (P3,
 * lib_queue_resumed()). No I/O, any thread. */
struct playlist_info;
bool lib_queue_is_book(const struct playlist_info *pl);
/* After a boot's playlist_resume(): marks the resumed queue as a book if
 * the record says it was one. UI thread (one 80 B read at most, once). */
void lib_queue_resumed(void);

/* File names of master entries for the library source (lib_shuffle.c):
 * a pinned search on the pool, as lib_path() uses. path: NULL if none
 * (logged by the caller). Reads the file name list on disk; yields. */
int  lib_scratch_open(void);
const char *lib_scratch_path(int sh, int entry);
void lib_scratch_close(int sh);

/* Album rows of the precompute file and their tracks (#49 TRACKS, else
 * the album filter, built once per album). One open per refill. */
struct lib_tracks {
    int fd, vh, gen;
    uint32_t albums;            /* LIB_ALBUMS rows */
    uint32_t rows_off, rows_sum;
    uint32_t tracks_off, tracks_count;  /* 0: no TRACKS section */
    uint32_t valbum;
};
int  lib_tracks_open(struct lib_tracks *t);     /* 0 or -1 */
void lib_tracks_close(struct lib_tracks *t);
uint32_t lib_tracks_len(struct lib_tracks *t, uint32_t album);
int  lib_tracks_entry(struct lib_tracks *t, uint32_t album, uint32_t k);

/* A book's files from the precompute file's LIB_AUDIOBOOKS TRACKS section
 * (P6 fix L2): the library's own grouping and list order (disc, track),
 * the queue lib_play() builds. 'rep' is the book row's representative
 * (the album table's rep_idx). Fills idx[] and returns the file count
 * (> max: the count only, idx untouched), or -1: no current file or no
 * section (the caller falls back to its own match), or the database
 * changed meanwhile. 'buf' (the caller's, any size >= 4) is where the
 * ROWS are searched. Any thread: no view pool, no statics; one open,
 * ROWS / 'size' reads + 2. */
int  lib_book_tracks(int32_t rep, uint32_t *idx, int max, void *buf,
                     size_t size);

/* -- Phase 6 glue between B's files (not a screen API; the screens use
 *    ab_book.h's ab_page_*). library.c, ab_book.c, ab_store.c,
 *    np_playback.c. ------------------------------------------------------ */
struct ab_rec;
struct ab_book_map;
struct ab_blob_view;
struct ab_idx_rec;
struct mp3entry;
/* ab_book.c */
bool ab_book_playing(void);             /* a mapped book queue plays */
bool ab_book_checkpoint(struct ab_rec *r, bool stopping);
bool ab_book_checkpoint_id3(struct ab_rec *r, const struct mp3entry *id3,
                            int q, bool stopping, bool ended);
bool ab_pak_lookup(uint64_t key, struct ab_idx_rec *out, uint32_t *gen);
size_t ab_pak_read(const struct ab_idx_rec *rec, uint32_t gen, void *buf,
                   size_t size, struct ab_blob_view *v);
struct ab_book_map *ab_map_begin(uint64_t key, int files);
bool ab_map_pak(const struct ab_blob_view *page);
void ab_map_fallback(void);
void ab_map_resumed(const struct mp3entry *id3, int q, int files);
/* np_playback.c: posts NP_PB_WAKE (once per pass), any thread */
void np_pb_ab_wake(void);
/* library.c: the tagcache migration's walk (ab_store_seed()). begin:
 * only with the database ready and audio stopped, a transient block of
 * 'bytes' (handle, or < 0); seed: records of books with stats (key,
 * file, position; the caller stamps and seals), or < 0 */
int  lib_book_seed_begin(size_t bytes, void **buf);
int  lib_book_seed(struct ab_rec *out, int max);
void lib_book_seed_end(int h);
/* library.c: the boot resume's map refill, once audio plays (np_pb_idle) */
void lib_book_resumed_idle(void);

/* -- Phase 11 additions (docs/phase11-brief.md section 4.3; L) ----------- */

/* Library totals for About (P11): computed by lib_precompute() at sync time
 * (bitrate x length summed per music track, no file reads) and kept in a
 * 24 B static when the file is written or found current; no I/O here.
 * WRITTEN on the tagcache thread (lib_precompute), READ on the UI thread
 * (Rev 1 L-4): a sequence counter guards it. The writer increments seq
 * (odd), stores the fields, increments seq (even). lib_stats() copies the
 * fields between two reads of seq and retries while seq is odd or changed
 * (no lock, no yield; a retry costs a 20 B copy). */
struct lib_stats {
    int32_t  songs, episodes, books;     /* tracks of each class */
    uint32_t audio_mib;                  /* sum(bitrate kbps * length ms) / 8 / 2^20 */
    int32_t  gen;                        /* tagcache_ui_generation() it is for */
};
bool lib_stats(struct lib_stats *out);   /* false before any precompute */
/* library.c static: struct { volatile uint32_t seq; struct lib_stats s; } (24 B)
 * libfile.h (L): LIBFILE_CAT_STATS 0x7d, col 0, one struct libfile_stats
 * (16 B); an ADDED section (LIBFILE_VERSION stays 1). */

/* -- Phase 11 glue (L: library.c <-> otg.c, and About's fallback; not
 *    in the contract headers) ------------------------------------------ */

/* lib_id() of a LIB_SEARCH row is its SRCH_ROW() value (search.h; cast
 * to uint32_t, decode with SRCH_ROW_KIND() / SRCH_ROW_IDX(): kinds 4 and
 * 5 make it negative as an int). lib_id() of an unresolved LIB_OTG row
 * is -2 (not in the database); a resolved one is the master index. */
/* Unresolved LIB_OTG rows: this flag | the offset of the row's name (the
 * path's file name) in the view's name area. */
#define LIB_OTG_UNRES 0x40000000
/* A LIB_OTG view of n rows (all -1) with a name area of 'names' bytes
 * (0 allowed), stacked on the pool like any view (never grown while
 * audio runs). Works while the database is rebuilt (names only). */
int  lib_otg_view_new(int n, size_t names);
int32_t *lib_view_rows(int view);        /* valid until the next yield */
char *lib_otg_names(int view, size_t *cap);
/* The tracks a row stands for, as a new track view (lib_close() it):
 * a song, episode or resolved On-The-Go row its one track; an artist,
 * album, genre, composer, book or show row (also a Search row of those
 * kinds) its tracks, in the order lib_play() plays them. -1 if none. */
int  lib_row_tracks(int view, int idx);
/* About before any precompute (lib_stats() false): the same totals from
 * one pass over the RAM database (~0.5 M instructions at 50k, yields;
 * UI thread), traced "lib stats live". False if no database. */
bool lib_stats_live(struct lib_stats *out);

#ifdef HAVE_BCMFW_VIDEO
/* -- Phase 14a additions (docs/phase14a-brief.md 3.2a, 3.3, 4.4; V3) ---- */

struct libfile_vstats;                  /* libfile.h */
/* True if row idx of a view is a video (TAGCACHE_UI_VIDEO in its flag
 * word): any view whose rows are tracks (songs, episodes, search song or
 * episode rows, On-The-Go, LIB_MOVIES, LIB_TVEPISODES). From the RAM
 * index's flag word: no I/O, no yield, no allocation. false for group
 * rows and out of range. Used by browse.c (Podcasts' video glyph, which
 * screen opens), holdmenu.c and lib_play(). */
bool lib_is_video(int view, int idx);
/* The video totals (About, the Videos panel), as lib_stats(): computed
 * by lib_precompute() at sync time and kept in a static guarded by the
 * same kind of sequence counter (written on the tagcache thread, read on
 * the UI thread, no lock, no I/O). false before any precompute. */
bool lib_vstats(struct libfile_vstats *out);
#endif

#endif /* _IPODUI_LIBRARY_H_ */
