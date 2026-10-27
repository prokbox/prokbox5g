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
 * iPod UI shell: album art cache
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
#ifndef _IPODUI_ARTCACHE_H_
#define _IPODUI_ARTCACHE_H_

#include <stdbool.h>
#include <stddef.h>
#include "artkey.h"     /* artcache_key_t, artcache_key() */
#include "artfmt.h"     /* enum artcache_size, the size table, the formats */

/* Pre-scaled album art in .rockbox/artcache/ (docs/design/artcache.md;
 * Phase 4b builds ARTCACHE_THUMB, H1 adds ARTCACHE_PANEL for the panel
 * set). Browsing never decodes: the UI
 * reads finished RGB565 thumbnails out of one pack file, and a background
 * builder thread makes them after each library update.
 *
 * Memory (all allocated once, by artcache_reserve(), at shell start before
 * the audio buffer exists; nothing is allocated later):
 *  - "artcache-build": the builder's work block (mp3entry, tagcache
 *    search, paths, index pages, slot and JPEG decode buffers; see
 *    struct bwork in artcache.c). Movable: pinned only while the builder
 *    works on one album or one index commit, never across a wait or a
 *    sleep; the pointer is fetched again after every wait.
 *  - "artcache-read": the UI's index reader (two 4 KB pages: fences and
 *    one page of records). UI thread only, pinned only inside a call.
 *  - "artcache-stack": the builder thread's stack (pinned: a running
 *    thread's stack cannot move).
 *
 * Album table (input; written by library.c, read by the builder only):
 *   the LIBFILE_CAT_ALBUMTAB section of the library precompute file
 *   <db dir>/LIBFILE_NAME (libfile.h; reader in artfmt.h): one
 *   struct libfile_album per album, book and show, in folder order. The
 *   builder checks the file header (sum; commit id, entry count and
 *   SORTKEY_VERSION of the database in RAM) and the section's sum; a
 *   header for another database waits for the next request (library.c
 *   writes the new file, then lib_committed() -> artcache_build_request()),
 *   a damaged file is rewritten (tagcache_ui_precompute_request()).
 *
 * Builder (one thread, PRIORITY_BACKGROUND, CPU core, never boosts:
 * resize.c's boost is off for this thread, resize_set_noboost_thread()):
 *  - per album: resolve the representative's path through tagcache (its
 *    own tagcache_search), get_metadata() into the work block, pick the
 *    source in the stock order (image file found by search_albumart_files()
 *    then embedded art; swapped by "prefer embedded"), CRC its first 16 KB,
 *    decode baseline JPEG or BMP straight to the fill size and centre-crop
 *    it (progressive JPEG, PNG and other failures: FAILED), write the slot.
 *    Records are committed every ARTCACHE_COMMIT_EVERY albums or 30 s
 *    (index.tmp + rename + generation + 1); UI readers reopen on the next
 *    call. Publication keeps index.prev; its thumbnail slots stay protected
 *    too. Automatic eviction is disabled: unrelated cached art survives
 *    temporary library changes and local repair.
 *  - first in every pass: the albums the UI asked for and found
 *    PENDING (list rows on screen, the Now Playing album: a small ring
 *    of keys, see artcache_get()), then the panel set's albums and their
 *    ARTCACHE_PANEL images (artcache_panel_count() below), then the rest
 *    in table (folder) order, then the panels again (#58).
 *  - an album whose record has the table's max_mtime is skipped; once per
 *    update its image-file source is checked again (search + size), so a
 *    new or replaced cover.jpg is noticed. Failed replacements preserve
 *    previous good records/bundles and remain due, with bounded backoff.
 *    A lost VERSION marker never erases otherwise compatible artwork.
 *  - waits (unpinned) while: artcache_build_pause() holds it;
 *    tagcache_ui_busy() or a commit runs; USB; the battery is under 20 %
 *    and not charging; and, stopped or paused, a key was pressed in the
 *    last 2 s (artcache_note_input(), button_status()).
 *  - while audio plays (#58 idle build, owner 2026-10-09) it does nothing
 *    while the user interacts, and runs continuously, waking storage as
 *    it needs, while the user is idle: the unlit clock face is up or the
 *    backlight is off (no key held), or on the charger no key for 2 s.
 *    The wait blocks on an event (artcache_note_idle(), playback stop or
 *    pause, a request, USB, a power-off), never a poll. A user who comes
 *    back mid-album stops it before its next read or write; the album is
 *    redone at the next idle. Once the backlog is done the thread sleeps
 *    until the next request (database change) or USB: no wakes.
 *  - SYS_USB_CONNECTED: closes every fd (it holds none across a wait),
 *    acks, waits for the disconnect, then starts the pass again.
 *    SYS_POWEROFF / SYS_REBOOT: stops at the next album boundary; a cut
 *    append or an unrenamed index.tmp is harmless (see artfmt.h).
 */

#define ARTCACHE_DIR          ROCKBOX_DIR "/artcache"

enum artcache_result {
    ARTCACHE_OK      =  0,
    ARTCACHE_NOART   =  1,  /* known: no art (or undecodable) -> note glyph */
    ARTCACHE_PENDING =  2,  /* not built yet -> note glyph, may appear later */
    ARTCACHE_ERR     = -1,  /* I/O, bad header, buffer too small */
};

struct dim;
struct bitmap;
struct mp3entry;

/* Allocates the three blocks above and starts the builder thread. Call
 * once at shell start, before anything can start playback (the audio
 * buffer takes what is left). Safe to call again (no-op). False if memory
 * was short: the cache then only reads (PENDING for everything new). */
bool artcache_reserve(void);

/* Key from the same strings the album lists group by (artkey.h). */
artcache_key_t artcache_key_for_id3(const struct mp3entry *id3);

/* Dimensions for a size id (from the size table). */
void artcache_dim(enum artcache_size size, struct dim *out);

/* Synchronous read into a caller buffer (>= w*h*2 bytes, any alignment;
 * 512-aligned is fastest). Fills bm->{width,height,data}. Never decodes,
 * never splashes. Thumbs: one lseek + read on the cached pak fd. UI thread
 * only (not the audio or codec threads).
 * Fit sizes (artcache_size_fits(): ARTCACHE_NOWPLAYING, Phase 5): bm gets
 * the bundle's own fitted w x h (aspect kept, <= the box of the size
 * table); bufsize must still hold the whole box. A bundle per chapter
 * image is read the same way with artcache_key_chapter() (#39).
 * QW2 (Phase 11): with bufsize >= ARTCACHE_NP_ONE_READ, a NOWPLAYING
 * bundle is ONE read() of header + pixels (artbundle_read_one()), so
 * bm->data is buf + ARTBUNDLE_HDR_BYTES (buf 4-aligned). The image is
 * then either the composed art (NP_COMPOSED_W x _H, np_draw.h; a bundle
 * composed for another NP_LAYOUT_HASH answers PENDING until the builder
 * composes it again) or a legacy fitted one (<= the box). */
#define ARTCACHE_NP_ONE_READ (512 + 137 * 187 * 2)
int artcache_get(artcache_key_t key, enum artcache_size size,
                 struct bitmap *bm, void *buf, size_t bufsize);

/* Same, into a new movable core_alloc handle the caller owns (core_free).
 * Returns the handle (> 0), or <= 0 with the result in *res. Refused
 * (ARTCACHE_ERR) while audio plays or is paused: a core_alloc then would
 * shrink the audio buffer (phase4b-brief.md, Revision 1, decision 1). */
int artcache_get_handle(artcache_key_t key, enum artcache_size size,
                        enum artcache_result *res);

/* npfix: the Now Playing bundle read off the UI thread. take: if the
 * builder thread holds a current copy of 'key''s bundle, copies it into
 * buf (>= ARTCACHE_NP_ONE_READ, as artcache_get() would read it) and
 * returns its result (ARTCACHE_OK, or PENDING: no bundle), else
 * ARTCACHE_NP_NOTYET (no storage either way). request: asks the builder
 * thread to read it (it may power storage up, never the UI thread); a
 * take some passes later finds it. A fill's end also reads the next
 * track's ahead, while storage is up. UI thread only. */
#define ARTCACHE_NP_NOTYET  (-2)
int artcache_np_take(artcache_key_t key, struct bitmap *bm, void *buf,
                     size_t bufsize);
void artcache_np_request(artcache_key_t key);
/* The builder thread's pump, woken to serve another reader's file
 * (art_bcm_viz_serve(): the visualizer's overlay). Any thread. */
void artcache_aux_wake(void);
/* review M3: Now Playing runs a pass (shown) or leaves: a builder caught
 * mid-decode parks while it is lit, on a charger too. UI thread. */
void artcache_note_np(bool shown);

/* QW2: the disk is spinning now (a read costs no spin-up). Now Playing
 * shows the placeholder first when it is not (the sim models it like the
 * builder's disk_active()). */
bool artcache_disk_awake(void);

/* Lookup only (index.dat), for deciding whether to show a glyph. */
enum artcache_result artcache_state(artcache_key_t key);

/* Panel art (H1; docs/design/artcache.md §7): the main menu's and Music
 * menu's split-screen panel cycles through the panel set, at most
 * LIBFILE_PANELS_MAX (32) albums chosen at every library precompute
 * (library.c): music albums (the most recently played, the newest, then
 * random ones) and, for the Audiobooks rows (#58), up to
 * 8 books (library.c PANEL_BOOKS: the most recently played, the newest).
 * Their ARTCACHE_PANEL images, 300x300 RGB565 (180,000 B), are in
 * panels.pak, built by the art builder at the start of each pass (the
 * panel set's albums first, so the drift fills early, #58) and kept up
 * to date at its end. A panel decodes straight from the source into
 * bands of the builder's reserved decode block and is written band by
 * band (#58): no transient block, so it builds like a thumbnail, during
 * playback too (inside audio fill windows only).
 *
 * Sets: ARTCACHE_PSET_MUSIC (Music and its rows, Shuffle Songs, Now
 * Playing) and ARTCACHE_PSET_BOOKS (the Audiobooks rows). An index is
 * per set.
 * artcache_panel_count(): the set's panels with art right now (0..32).
 * Reads the 1 KB header of panels.pak when the builder changed it (or
 * after USB), else nothing. The count and the order can change after any
 * build: index i is only meaningful until the next call that returns a
 * different count; a cover is identified by its key.
 * artcache_panel_keys(): the same, with the keys of panels 0..count-1 in
 * keys[] (at most 'max'), for the drift's picker (drift_pick.h).
 * artcache_panel_get(): copies panel i (0..count-1) of the set into 'buf'
 * (any alignment; 'size' >= 300 * 300 * 2) and fills bm->{width, height,
 * format, data}; *key (if not NULL) gets its key. One open + lseek + read
 * of 180,000 B; never decodes, never allocates, never splashes.
 * ARTCACHE_OK, or ARTCACHE_ERR (no such panel, buffer too small, I/O
 * error, or the slot is being rebuilt: keep showing the current one). UI
 * thread only, like artcache_get(); the caller decides when storage may
 * be woken (artcache.md §7).
 * artcache_get(key, ARTCACHE_PANEL, ...) is not used for these (it
 * answers PENDING). */
enum artcache_panel_set {
    ARTCACHE_PSET_MUSIC = 0,
    ARTCACHE_PSET_BOOKS = 1,
#ifdef HAVE_BCMFW_VIDEO
    /* Phase 14a (docs/phase14a-brief.md 3.5): the posters of the Videos
     * panel, the album-table rows with LIBFILE_ALB_VIDEO (movies and TV
     * shows; library.c pre_panels() gives them their share). Podcast
     * show art (8b) adds ARTCACHE_PSET_PODCASTS the same way, over the
     * LIBFILE_ALB_PODCAST rows: one more PSET_OF() case, nothing else. */
    ARTCACHE_PSET_VIDEOS = 2,
#endif
};
int artcache_panel_count(int set);
/* + 1 at every panels.pak header write and at USB: a kept panel image
 * may be stale once it moved. One volatile read, no I/O. */
int artcache_panel_seq(void);
int artcache_panel_keys(int set, uint64_t *keys, int max);
int artcache_panel_get(int set, int i, struct bitmap *bm, void *buf,
                       size_t size, uint64_t *key);
/* #92: the drift's bank refresh. Up to n (<= ARTCACHE_PANEL_RUN_MAX)
 * covers of 'set' from its i0-th, in set order, wrapping, the cover keyed
 * 'skip' left out, into 'buf' (size >= n * ARTCACHE_PANEL_SLOT_BYTES) at
 * that stride: cover j's pixels at buf + j * ARTCACHE_PANEL_SLOT_BYTES.
 * Covers in consecutive slots of panels.pak are read in one read (a
 * grouped file: one read for the whole run). keys[j], idx[j] (its index
 * in the set) and cw[j] (0 =
 * the whole slot; NULL for none) name what came. Returns how many came
 * (0..n; a torn or rewritten slot ends the run), -1 on bad arguments.
 * UI thread; the caller decides when storage may be read. */
#define ARTCACHE_PANEL_SLOT_BYTES 180224    /* artpak_slot_bytes(300, 300) */
#define ARTCACHE_PANEL_RUN_MAX    4
int artcache_panel_run(int set, int i0, int n, uint64_t skip, void *buf,
                       size_t size, uint64_t *keys, int *cw, int *idx);
/* The main menu's Now Playing row (np-art-quality): the w x h window at
 * x0, y0 of 'key''s 300 x 300 panel into dst (stride in pixels), from
 * panels.pak's default set or panelpool.pak when the shown set (the
 * drift's, artcache_panel_keys()) does not name it, or (np-sharp)
 * npcover.pak. 'scratch' (>= 4 KB;
 * more means fewer reads) takes the pool table and the rows. A few reads
 * (a 1 KB header, a 3.6 KB table, the rows in runs, the tag); none for a
 * key found missing under the same artcache_panel_seq(). ARTCACHE_OK,
 * PENDING (none built), or ERR (dst may hold part of it). UI thread; the
 * caller decides when storage may be read. */
int artcache_panel_window(uint64_t key, int x0, int y0, int w, int h,
                          void *dst, int stride, void *scratch,
                          size_t size);
/* np-sharp (owner 2026-10-24): the playing album has no built panel:
 * queue its 300 px render into npcover.pak (one album queued at a time,
 * the newest wins; the builder renders it only with storage already
 * awake, the user idle and Now Playing dark, and drops it once that album
 * no longer plays). 'why': the row drew its stand-in (ROW), or the album
 * played 30 s (PLAYED, artcache_rotate_poll()). Any thread. */
/* RESIDENT (P19 19-6, the end state): the panel is built, but the BCM
 * lost its resident copy (an epoch change): decode it there again, no
 * file written (art_bcm.h) */
enum { ARTCACHE_NPR_ROW = 0, ARTCACHE_NPR_PLAYED, ARTCACHE_NPR_RESIDENT };
void artcache_npcover_queue(artcache_key_t key, int why);
/* Moves when a render into npcover.pak has landed (or failed): a row
 * showing the stand-in reads the window again */
int artcache_npcover_seq(void);
#ifdef HAVE_BCMFW_VIDEO
/* Phase 14a: artcache_panel_get(), plus where the picture is inside the
 * 300 x 300 image: *cw 0 = all of it (every square cover), else a
 * portrait picture cw px wide (even, 200..268), centred, full height
 * (artfmt.h ARTPANEL_CW: a 2:3 poster is 200). Chosen by the source's
 * aspect at build time, whatever the set. The columns outside it are
 * black; the drift pans inside it (preview.c). 'cw' may be NULL. */
int artcache_panel_get_cw(int set, int i, struct bitmap *bm, void *buf,
                          size_t size, uint64_t *key, int *cw);
#endif

/* Builder control. */
void artcache_build_request(void);     /* after each library (re)load;
                                          any thread (a queue post) */
/* Music hold-menu rebuild: stays USB-safe and defers while playback is lit. */
void artcache_build_maintenance_request(void);
bool artcache_maintenance_pending(void);
void artcache_build_pause(bool pause); /* nests; e.g. Cover Flow open */
enum artcache_build_wait {
    ARTCACHE_BUILD_READY,
    ARTCACHE_BUILD_WAIT_DB,
    ARTCACHE_BUILD_WAIT_PAUSE,
    ARTCACHE_BUILD_WAIT_PLAYBACK,
    ARTCACHE_BUILD_WAIT_BATTERY,
    ARTCACHE_BUILD_WAIT_OFF,
    ARTCACHE_BUILD_WAIT_RETRY,
};
enum artcache_build_phase {
    ARTCACHE_BUILD_PHASE_PREPARE,
    ARTCACHE_BUILD_PHASE_REQUESTED,
    ARTCACHE_BUILD_PHASE_ALBUMS,
    ARTCACHE_BUILD_PHASE_PANELS,
    ARTCACHE_BUILD_PHASE_CHAPTERS,
    ARTCACHE_BUILD_PHASE_BOOKS,
    ARTCACHE_BUILD_PHASE_FINALIZE,
};
struct artcache_build_status {
    int done, total;
    int phase_done, phase_total;
    bool running;
    enum artcache_build_wait wait;
    enum artcache_build_phase phase;
};
void artcache_build_status(struct artcache_build_status *st);

/* Index commit sequence: + 1 after every index commit (index.tmp renamed),
 * 0 before the first. A UI poller (the album list's art fill, #38 F9)
 * redraws stand-in art only when this changed since its last look, not on
 * build progress. Any thread; one volatile read, no I/O. */
int artcache_commit_seq(void);

/* The shell's input loop calls this on every event: stopped or paused,
 * the builder keeps off the disk and CPU for 2 s after the last one;
 * playing on the charger too. */
void artcache_note_input(void);
/* #88 (maint.c): the builder's idle predicate (dark, no key; on the
 * charger no key for 2 s), one definition; and a wake for the builder
 * when the paused window opens with chapter-image books owed */
bool artcache_user_idle(void);
void artcache_maint_kick(void);
/* #92/#90: the builder owes a maintenance window work (the panels.pak
 * regroup, the signature fill): maint_poll() kicks it when the window
 * opens */
bool artcache_maint_owed(void);
/* #100: maint_poll(), once a second: a panel set rotation due while
 * storage is awake wakes the builder (never storage) */
void artcache_rotate_poll(void);
/* P18 Q8 (sleep.c): the rotation is due by the RTC (time asleep counts)
 * and there is a panel set to rotate; the sleep-entry window owes it
 * (SJ_ROTATION) and the builder runs it there, storage awake or not */
bool artcache_rotate_owed(void);
/* maint_poll() while stopped (#92 review M2): wakes the builder when the
 * stopped-and-idle window opens with that work owed */
void artcache_maint_poll_stopped(void);

/* The UI thread calls this when the user goes idle (the backlight went
 * off: SYS_IPODUI_BACKLIGHT_OFF, before or without the clock face). It
 * only wakes a builder that waits for an idle user during playback. */
void artcache_note_idle(void);
/* P19 19-1a (tagcache thread): the tick at which the playing album will
 * have played long enough to queue its Now Playing cover; false if none
 * is owed or it is due now */
bool artcache_poll_due(long *at);

/* Arm before USB storage acknowledges exclusive filesystem ownership.  An
 * active panel-art decode observes it cooperatively; finish covers the case
 * where no builder exists to consume the USB event. */
void artcache_usb_prepare(void);
void artcache_usb_finish(void);
/* P18: sleep entry (true) to the wake (false): the builder starts no
 * filesystem operation, as while USB owns the disk; UI thread */
void artcache_sleep_hold(bool hold);

#endif /* _IPODUI_ARTCACHE_H_ */
