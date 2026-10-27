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
 * iPod UI shell: library-backed shuffle source (#27, Phase 5)
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

/* The runtime of lib_shuffle.h (design: docs/design/shuffle.md, items
 * 1-8; contract D7-D9). The playlist is a window of at most a few hundred
 * entries over a keyed permutation of the whole library: shuffle_core.c
 * gives the order, this file feeds it into the playlist LIB_SHUF_WINDOW
 * entries at a time.
 *
 * Threads: UI thread, except lib_shuffle_track_changed() and
 * lib_shuffle_active() (audio thread: a flag and two word reads).
 * Refills run from np_pb_idle() after a posted NP_PB_WAKE: never polled.
 * The playlist mutex is taken per insert/delete by playlist.c and never
 * held across a file-name read (tagcache_retrieve) or a yield.
 *
 * Static RAM: the 80 B record plus 16 B of flags and stamps. Nothing is
 * allocated: file names go through library.c's play scratch, stacked on
 * its existing pool (as lib_path() does).
 *
 * Trace (UPDATE_TRACE: sim.log; logf with LOGF_ENABLE; nothing on a
 * release build). <m> is off | songs | albums, <src> none | songs | albums:
 *   shuf: start <m> ok queued=<n>
 *   shuf: start <m> fail <why>    why: off | empty | no-albums | create
 *                                 | refill
 *   shuf: mode <m> ok src=<src> queued=<n> cur=<playlist index>
 *   shuf: mode <m> fail <why>     why: empty | no-albums | queue
 *                                 (ordinary queue: playlist reorder)
 *   shuf: queue <m> groups=<n> | queue <m> in order (no groups)
 *   shuf: refill +<added> -<deleted> skipped=<n> src=<src> queued=<n>
 *   shuf: refill retry (<why>)    why: db | no-albums | scratch
 *   shuf: refill stop (<why>)     why: gen | insert
 *   shuf: lap n=<n>               a new lap (database changed)
 *   shuf: boot record <c>         0 resume, 1 restart, 2 bad
 *   shuf: record write failed | record write short
 *   shuf: no file name e=<entry> */

#include <string.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "file.h"
#include "audio.h"
#include "playlist.h"
#include "settings.h"
#include "storage.h"
#include "tagcache.h"
#include "logf.h"
#include "appevents.h"
#include "ipodui/anim.h"            /* UPDATE_TRACE */
#include "rbpaths.h"
#include "ipodui/sortkey.h"
#include "ipodui/library.h"
#include "ipodui/lib_shuffle.h"
#include "ipodui/dbqueue.h"
#include "ipodui/uilog.h"
#include "buffering.h"              /* buffering_is_filling() (npfix) */
#ifdef SIMULATOR
#include <stdio.h>                  /* shuf_expect_settled */
#endif

#define SHUF_PATH   ROCKBOX_DIR "/" LIB_SHUF_FILE
#define SKIP_LIMIT  (LIB_SHUF_WINDOW * 64)  /* non-music entries per refill */

static struct lib_shuffle_rec rec;      /* source NONE: an ordinary queue */
static unsigned long src_tick;          /* playlist created_tick of the source */
static int src_gen = -1;                /* RAM cache generation it runs on */
static volatile bool want_refill;       /* audio thread: a track changed */
static bool loaded;                     /* the record was read at boot */
static bool boot_check;                 /* resumed record not checked yet */
static volatile bool disk_ran;          /* buffering thread: storage ran */

const char *lib_shuffle_mode_name(enum np_shuffle mode)
{
    return mode == NP_SHUFFLE_ALBUMS ? "albums"
         : mode == NP_SHUFFLE_SONGS ? "songs" : "off";
}

static inline const char *src_name(void)
{
    return rec.source == LIB_SHUF_SRC_ALBUMS ? "albums"
         : rec.source == LIB_SHUF_SRC_SONGS ? "songs" : "none";
}

static void on_buffer(unsigned short id, void *data)
{
    (void)id; (void)data;
    disk_ran = true;
}

/* ---- record I/O --------------------------------------------------------- */

static void rec_write(void)
{
    int fd;

    rec.repeat = (uint8_t)global_settings.repeat_mode;
    lib_shuf_rec_seal(&rec);
    fd = open(SHUF_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        UPDATE_TRACE("shuf: record write %s", "failed");
        return;
    }
    if (write(fd, &rec, sizeof(rec)) != (ssize_t)sizeof(rec))
        UPDATE_TRACE("shuf: record write %s", "short");
    close(fd);
}

/* Boot: one 80 B read, once (lib_shuffle_resume() or the first idle). */
static void rec_load(void)
{
    struct lib_shuffle_rec r;
    ssize_t got = -1;
    int fd;

    if (loaded)
        return;
    loaded = true;
    add_event(BUFFER_EVENT_FILL_DONE, on_buffer);
    add_event(BUFFER_EVENT_REBUFFER, on_buffer);
    fd = open(SHUF_PATH, O_RDONLY);
    if (fd >= 0)
    {
        got = read(fd, &r, sizeof(r));
        close(fd);
    }
    /* intactness only here; the stamp is checked at the first refill,
     * once the database is in RAM */
    if (got != (ssize_t)sizeof(r) || r.magic != LIB_SHUF_MAGIC
        || r.version != LIB_SHUF_VERSION || r.crc != lib_shuf_rec_sum(&r)
        || r.source > LIB_SHUF_SRC_ALBUMS || r.mode > NP_SHUFFLE_ALBUMS)
    {
        memset(&rec, 0, sizeof(rec));
        rec.mode = global_settings.playlist_shuffle ? NP_SHUFFLE_SONGS
                                                    : NP_SHUFFLE_OFF;
        return;
    }
    rec = r;
    boot_check = rec.source != LIB_SHUF_SRC_NONE;
}

static bool stamp_now(struct lib_shuf_stamp *st, uint32_t albums_sum)
{
    int n = tagcache_ui_count();

    if (n < 0)
        return false;
    st->commitid = tagcache_ui_commitid();
    st->entries = n;
    st->sortkey_version = SORTKEY_VERSION;
    st->albums_sum = albums_sum;
    return true;
}

/* ---- the iterator ------------------------------------------------------- */

static uint32_t album_len(void *ctx, uint32_t album)
{
    return lib_tracks_len((struct lib_tracks *)ctx, album);
}

static bool repeat_on(void)
{
    return global_settings.repeat_mode == REPEAT_ALL;
}

static bool is_music(uint32_t e)
{
    int32_t flag;

    return tagcache_ui_seeks((int)e, &flag)
        /* Phase 14a 3.2a: never a video in Shuffle Songs (any build:
         * the plain shell detects videos too, #85) */
        && !(flag & TAGCACHE_UI_ANY_VIDEO)
        && !(flag & (TAGCACHE_UI_AUDIOBOOK | TAGCACHE_UI_PODCAST));
}

/* Next master entry of the run, or -1 at the end of the lap (repeat off)
 * or of the album list. Songs: deleted and non-music entries skipped
 * (RAM only). */
static int next_entry(struct lib_tracks *lt, int *skips)
{
    if (rec.source == LIB_SHUF_SRC_SONGS)
    {
        for (;;)
        {
            uint32_t e = shuf_songs_next(&rec.it.s, repeat_on());

            if (e == SHUF_NONE)
                return -1;
            if (is_music(e))
                return (int)e;
            if (++*skips > SKIP_LIMIT)
                return -1;
        }
    }
    if (rec.source == LIB_SHUF_SRC_ALBUMS && lt->fd >= 0)
    {
        uint32_t a, k;

        if (shuf_albums_next(&rec.it.a, album_len, lt, repeat_on(), &a, &k))
            return lib_tracks_entry(lt, a, k);
    }
    return -1;
}

/* A new lap over the database now (shuffle.md item 6). */
static bool restart_lap(struct lib_tracks *lt)
{
    struct lib_shuf_stamp now;
    uint32_t n;

    if (!stamp_now(&now, rec.source == LIB_SHUF_SRC_ALBUMS ? lt->rows_sum : 0))
        return false;
    n = rec.source == LIB_SHUF_SRC_ALBUMS ? lt->albums
                                          : (uint32_t)now.entries;
    lib_shuf_rec_restart(&rec, n, &now);
    UPDATE_TRACE("shuf: lap n=%lu", (unsigned long)n);
    return true;
}

/* The stamp the source runs on still names the database in RAM? */
static bool db_same(const struct lib_tracks *lt)
{
    struct lib_shuf_stamp now;

    if (!stamp_now(&now, rec.source == LIB_SHUF_SRC_ALBUMS ? lt->rows_sum : 0))
        return false;
    return now.commitid == rec.stamp.commitid
        && now.entries == rec.stamp.entries
        && now.sortkey_version == rec.stamp.sortkey_version
        && (rec.source != LIB_SHUF_SRC_ALBUMS
            || now.albums_sum == rec.stamp.albums_sum);
}

/* ---- refill --------------------------------------------------------------- */

static int left_ahead(void)
{
    return playlist_amount() - playlist_get_display_index();
}

/* A refill adds up to LIB_SHUF_WINDOW entries at the end of the
 * playlist, trims the history, syncs the control file, then writes the
 * record. npfix: from the UI pass (lib_shuffle_idle()) it runs in chunks
 * of SHUF_CHUNK entries, one per pass (each entry is a file-name read and
 * a control-file append: ~100 small I/Os for a whole window, which held
 * the UI thread, and Now Playing's clock, for seconds when they queued
 * behind a buffering fill), and waits while a fill runs unless the queue
 * is nearly out (LIB_SHUF_URGENT). The record is written once, at the
 * end, as before: a power cut mid-refill leaves more entries queued than
 * the record says, which boot_resume_check() restarts as a new lap (the
 * case of a cut between the sync and the record, already handled).
 * Chosen over moving refills to the tagcache thread: the record, the
 * library's play scratch and the playlist calls stay on one thread
 * (CLAUDE.md rule 4), and the change is local to this file.
 * Trace: "shuf: chunk +<n> total=<n>"; ipodui.log "shuf refill ms=<UI
 * time> max=<longest chunk> chunks=<n> n=<added> disk=<storage up at the
 * start>". */
#define SHUF_CHUNK  5
static struct {
    bool on;                    /* a refill is under way */
    bool disk;                  /* storage was up when it began */
    int gen;                    /* tagcache_ui_generation() at the start */
    int added, skips, chunks;
    long ms, max_ms;            /* UI-thread time in it, longest chunk */
} ses;

static int refill_begin(void)
{
    struct lib_tracks lt;
    int gen = tagcache_ui_generation();

    lt.fd = -1;
    lt.vh = -1;
    if (tagcache_ui_count() < 0 || tagcache_ui_busy())
    {
        UPDATE_TRACE("shuf: refill retry (%s)", "db");
        return -1;              /* fresh / rebuilding: the window plays on */
    }
    if (rec.source == LIB_SHUF_SRC_ALBUMS && lib_tracks_open(&lt) < 0)
    {
        UPDATE_TRACE("shuf: refill retry (%s)", "no-albums");
        return -1;
    }
    /* dirty or changed (a commit, a sync): a new lap with the new N */
    if (src_gen != gen && !db_same(&lt))
    {
        if (!restart_lap(&lt))
        {
            lib_tracks_close(&lt);
            return -1;
        }
    }
    lib_tracks_close(&lt);
    src_gen = gen;
    memset(&ses, 0, sizeof(ses));
    ses.on = true;
    ses.gen = gen;
    ses.disk = storage_disk_is_active();
    return 0;
}

/* The trim, the sync and the record: the end of a refill */
static int refill_finish(void)
{
    int del;

    ses.on = false;
    /* keep LIB_SHUF_HISTORY played entries (the playlist is in insert
     * order: entry 0 is the oldest). One call, no audio flush: what
     * plays next does not change (playlist_delete() in a loop posted one
     * Q_AUDIO_FLUSH per entry and overflowed the audio queue) */
    del = playlist_get_display_index() - 1 - LIB_SHUF_HISTORY;
    if (del > 0)
        del = playlist_delete_range_ipodui(NULL, 0, MIN(del, LIB_SHUF_WINDOW));
    else
        del = 0;

    if (ses.added || del)
        playlist_sync(NULL);    /* control file synced before the record */
    src_tick = playlist_get_current()->created_tick;
    rec.queued = (uint32_t)playlist_amount();
    rec_write();
    UPDATE_TRACE("shuf: refill +%d -%d skipped=%d src=%s queued=%lu",
                 ses.added, del, ses.skips, src_name(),
                 (unsigned long)rec.queued);
    return ses.added;
}

/* Up to 'max' more entries. Returns 1 while the refill goes on, else
 * what refill_finish() returns (entries added), or -1 (not ready: the
 * next event retries). */
static int refill_step(int max)
{
    struct lib_tracks lt;
    int sh, n = 0;
    bool done = false;

    lt.fd = -1;
    lt.vh = -1;
    if (rec.source == LIB_SHUF_SRC_ALBUMS && lib_tracks_open(&lt) < 0)
    {
        UPDATE_TRACE("shuf: refill retry (%s)", "no-albums");
        return ses.added ? refill_finish() : (ses.on = false, -1);
    }
    sh = lib_scratch_open();
    if (sh < 0)
    {
        lib_tracks_close(&lt);
        UPDATE_TRACE("shuf: refill retry (%s)", "scratch");
        return ses.added ? refill_finish() : (ses.on = false, -1);
    }
    while (n < max && ses.added < LIB_SHUF_WINDOW)
    {
        const char *path;
        int e = next_entry(&lt, &ses.skips);

        if (e < 0)
        {
            done = true;
            break;
        }
        path = lib_scratch_path(sh, e);         /* yields */
        if (tagcache_ui_generation() != ses.gen)
        {
            UPDATE_TRACE("shuf: refill stop (%s)", "gen");
            done = true;        /* USB or a reload: retry at the next event */
            break;
        }
        if (!path)
        {
            UPDATE_TRACE("shuf: no file name e=%d", e);
            continue;
        }
        if (playlist_insert_track(NULL, path, PLAYLIST_INSERT_LAST,
                                  false, false) < 0)
        {
            UPDATE_TRACE("shuf: refill stop (%s)", "insert");
            done = true;
            break;
        }
        n++;
        ses.added++;
    }
    lib_scratch_close(sh);
    lib_tracks_close(&lt);
    ses.chunks++;
    if (max == SHUF_CHUNK)
        UPDATE_TRACE("shuf: chunk +%d total=%d", n, ses.added);
    if (done || ses.added >= LIB_SHUF_WINDOW)
        return refill_finish();
    return 1;
}

/* review L2: a chunked refill open on a queue that has since been
 * replaced (lib_play(), an OTG queue, a book) ends unwritten: its record
 * belongs to the old queue */
static void ses_check(void)
{
    if (ses.on && !lib_shuffle_active())
    {
        UPDATE_TRACE("shuf: refill dropped (%s)", "queue");
        ses.on = false;
    }
}

/* A whole refill at once (a start, a mode change: the user waits for it
 * anyway, and the new queue must be there before playback starts). A
 * chunked refill under way is finished first. */
static int refill(void)
{
    int rc;

    ses_check();
    if (ses.on)
        while (refill_step(LIB_SHUF_WINDOW) == 1)
            ;
    if (refill_begin() < 0)
        return -1;
    while ((rc = refill_step(LIB_SHUF_WINDOW)) == 1)
        ;
    return rc;
}

/* First refill after a boot resume: is the record for this queue and
 * this database? (D8) */
static void boot_resume_check(void)
{
    struct lib_tracks lt;
    struct lib_shuf_stamp now;
    uint32_t q = (uint32_t)playlist_amount();
    enum lib_shuf_check c;

    boot_check = false;
    lt.fd = -1;
    lt.vh = -1;
    if (rec.source == LIB_SHUF_SRC_ALBUMS && lib_tracks_open(&lt) < 0)
        lt.rows_sum = 0;
    if (!stamp_now(&now, rec.source == LIB_SHUF_SRC_ALBUMS ? lt.rows_sum : 0))
    {
        boot_check = true;      /* not ready: check at the next event */
        lib_tracks_close(&lt);
        return;
    }
    /* a different queue (not the one resumed): an ordinary queue. A
     * power cut between the control-file sync and the record leaves at
     * most one refill more in the playlist: that one restarts a lap. */
    if (q < rec.queued || q > rec.queued + LIB_SHUF_WINDOW)
        c = LIB_SHUF_BAD;
    else
        c = lib_shuf_rec_check(&rec, sizeof(rec), &now, q);
    switch (c)
    {
    case LIB_SHUF_RESUME:
        break;
    case LIB_SHUF_RESTART:
        lib_shuf_rec_restart(&rec, rec.source == LIB_SHUF_SRC_ALBUMS
                             ? lt.albums : (uint32_t)now.entries, &now);
        break;
    default:
        rec.source = LIB_SHUF_SRC_NONE;
        break;
    }
    lib_tracks_close(&lt);
    src_tick = playlist_get_current()->created_tick;
    src_gen = tagcache_ui_generation();
    UPDATE_TRACE("shuf: boot record %d", (int)c);
}

/* ---- API ---------------------------------------------------------------- */

bool lib_shuffle_active(void)
{
    if (rec.source == LIB_SHUF_SRC_NONE)
        return false;
    return boot_check || playlist_get_current()->created_tick == src_tick;
}

enum np_shuffle lib_shuffle_mode(void)
{
    rec_load();
    if (lib_shuffle_active())
        return (enum np_shuffle)rec.mode;
    if (!global_settings.playlist_shuffle)
        return NP_SHUFFLE_OFF;
    return rec.mode == NP_SHUFFLE_ALBUMS ? NP_SHUFFLE_ALBUMS
                                         : NP_SHUFFLE_SONGS;
}

static uint32_t new_seed(void)
{
    uint32_t s = (uint32_t)current_tick * 2654435761u;
#ifdef USEC_TIMER
    s ^= (uint32_t)USEC_TIMER;
#endif
    return s ? s : 1;
}

/* Iterator for 'mode' over the database now, 'first' playing first.
 * Everything is checked before 'rec' is touched (finding 5): on failure
 * the running source (and its iterator) is left exactly as it was, and
 * *why names the reason. */
static bool source_init(enum np_shuffle mode, uint32_t first,
                        struct lib_tracks *lt, const char **why)
{
    struct lib_shuf_stamp now;
    int n = tagcache_ui_count();

    if (n <= 0)
    {
        *why = "empty";
        return false;
    }
    if (mode == NP_SHUFFLE_ALBUMS
        && (lib_tracks_open(lt) < 0 || lt->albums == 0))
    {
        *why = "no-albums";
        return false;
    }
    if (!stamp_now(&now, mode == NP_SHUFFLE_ALBUMS ? lt->rows_sum : 0))
    {
        *why = "empty";
        return false;
    }
    memset(&rec.it, 0, sizeof(rec.it));
    if (mode == NP_SHUFFLE_ALBUMS)
    {
        rec.source = LIB_SHUF_SRC_ALBUMS;
        shuf_albums_init(&rec.it.a, lt->albums, new_seed());
        (void)first;            /* albums start with a random album */
    }
    else
    {
        rec.source = LIB_SHUF_SRC_SONGS;
        shuf_songs_init(&rec.it.s, (uint32_t)n, new_seed(), first);
    }
    rec.stamp = now;
    rec.mode = (uint8_t)mode;
    src_gen = tagcache_ui_generation();
    boot_check = false;
    return true;
}

int lib_shuffle_start(enum np_shuffle mode, uint32_t first)
{
    struct lib_tracks lt;
    struct lib_shuffle_rec keep;
    const char *why = "off";
    int rc;

    rec_load();
    ses.on = false;             /* a new queue: a refill under way ends */
    keep = rec;                 /* after the load: a failure restores it */
    lt.fd = -1;
    lt.vh = -1;
    if (mode == NP_SHUFFLE_OFF || !source_init(mode, first, &lt, &why))
    {
        lib_tracks_close(&lt);
        rec = keep;
        UPDATE_TRACE("shuf: start %s fail %s", lib_shuffle_mode_name(mode),
                     why);
        return -1;
    }
    lib_tracks_close(&lt);
    if (playlist_create(NULL, NULL) < 0)
    {
        rec = keep;
        UPDATE_TRACE("shuf: start %s fail %s", lib_shuffle_mode_name(mode),
                     "create");
        return -1;
    }
    src_tick = playlist_get_current()->created_tick;
    rec.flags &= ~LIB_SHUF_F_BOOK;  /* a library source is not a book */
    rc = refill();
    if (rc <= 0)
    {
        rec.source = LIB_SHUF_SRC_NONE;
        rec_write();
        UPDATE_TRACE("shuf: start %s fail %s", lib_shuffle_mode_name(mode),
                     "refill");
        return -1;
    }
    global_settings.playlist_shuffle = true;
    playlist_start(0, 0, 0);
    UPDATE_TRACE("shuf: start %s ok queued=%d", lib_shuffle_mode_name(mode),
                 playlist_amount());
    return 0;
}

void lib_shuffle_track_changed(void)
{
    want_refill = true;
}

/* review L1: a power-off (input.c) or a sleep entry (sleep.c persist())
 * with a chunked refill open: it ends here, at the entries it has, so the
 * record matches the queue (rec.queued == playlist_amount()) and the next
 * boot resumes the lap instead of restarting it; the rest of the window
 * follows at the next track change. UI thread. */
void lib_shuffle_settle(void)
{
    if (!ses.on)
        return;
    rec_load();
    ses_check();
    if (!ses.on)
        return;
    UPDATE_TRACE("shuf: settle chunks=%d +%d", ses.chunks, ses.added);
    refill_finish();
    want_refill = true;
}

#ifdef SIMULATOR
/* tests (shuf_refill_force, ipodui-np-stall): the next UI pass begins a
 * refill as if the queue were low and storage up */
static volatile bool sim_force;
/* shuf_chunk_hold (review L1/L2): no chunk after the first while set */
static volatile bool sim_chunk_hold;

void sim_shuf_chunk_hold(bool hold)
{
    sim_chunk_hold = hold;
    if (!hold)
        np_pb_ab_wake();
}

/* shuf_expect_settled: no refill open, and the record names the queue */
void sim_shuf_expect_settled(int lineno)
{
    rec_load();
    if (!ses.on && (!lib_shuffle_active()
                    || rec.queued == (uint32_t)playlist_amount()))
        fprintf(stderr, "sim_script: shuf_expect_settled ok queued=%lu\n",
                (unsigned long)rec.queued);
    else
        fprintf(stderr, "sim_script:%d: shuf_expect_settled failed on=%d "
                "queued=%lu amount=%d\n", lineno, (int)ses.on,
                (unsigned long)rec.queued, playlist_amount());
}

void sim_shuf_refill_force(void)
{
    sim_force = true;
    want_refill = true;
    np_pb_ab_wake();
}
#endif

int lib_shuffle_idle(void)
{
    int left;
    bool hot = disk_ran;

    rec_load();
    disk_ran = false;
    ses_check();                /* the queue was replaced under it */
    if (ses.on)
    {
        long t0 = current_tick, dt;
        int rc;

#ifdef SIMULATOR
        if (sim_chunk_hold && ses.chunks)
            return HZ / 5;      /* shuf_chunk_hold: the next chunk waits */
#endif
        /* a fill holds the disk: the next chunk after it, unless the
         * queue is nearly out */
        if (left_ahead() > LIB_SHUF_URGENT && buffering_is_filling())
            return HZ / 5;
        rc = refill_step(SHUF_CHUNK);
        dt = (long)(current_tick - t0) * (1000 / HZ);
        ses.ms += dt;
        if (dt > ses.max_ms)
            ses.max_ms = dt;
        if (rc == 1)
            return HZ / 20;     /* the next chunk: the UI runs between */
        if (rc < 0)
            want_refill = true; /* not ready: the next event retries */
        /* npfix: the UI thread's time in it, the longest chunk */
        ui_log("shuf", "refill ms=%ld max=%ld chunks=%d n=%d disk=%d",
               ses.ms, ses.max_ms, ses.chunks, rc, (int)ses.disk);
        return TIMEOUT_BLOCK;
    }
    if (!want_refill || !lib_shuffle_active())
        return TIMEOUT_BLOCK;
    if (boot_check)
    {
        boot_resume_check();
        if (!lib_shuffle_active())
            return TIMEOUT_BLOCK;
    }
    left = left_ahead();
#ifdef SIMULATOR
    if (sim_force)
    {
        sim_force = false;      /* shuf_refill_force: a refill now */
        left = LIB_SHUF_LOW;
        hot = true;
    }
#endif
    if (left > LIB_SHUF_LOW)
    {
        want_refill = false;
        return TIMEOUT_BLOCK;
    }
    /* not urgent: only with storage already awake (the event came from
     * buffering, or the disk is spinning); else the next event retries */
    if (left > LIB_SHUF_URGENT && !hot && !storage_disk_is_active())
        return TIMEOUT_BLOCK;
    want_refill = false;
    if (refill_begin() < 0)
    {
        want_refill = true;     /* not ready: the next event retries */
        return TIMEOUT_BLOCK;
    }
    return 1;                   /* the first chunk: the next pass */
}

void lib_shuffle_resume(void)
{
    rec_load();
}

void lib_shuffle_note_book(bool book)
{
    uint32_t q = (uint32_t)playlist_amount();
    uint8_t f;

    rec_load();
    ses.on = false;             /* lib_play() replaced the queue (review
                                 * L2: a book or not) */
    f = book ? rec.flags | LIB_SHUF_F_BOOK : rec.flags & ~LIB_SHUF_F_BOOK;
    if (f == rec.flags && (!book || (rec.source == LIB_SHUF_SRC_NONE
                                     && rec.queued == q)))
        return;                 /* unchanged: no write */
    rec.flags = f;
    if (book)
    {
        /* lib_play() replaced the playlist: an ordinary queue now */
        rec.source = LIB_SHUF_SRC_NONE;
        rec.queued = q;
        boot_check = false;
    }
    rec_write();
}

bool lib_shuffle_resumed_book(void)
{
    rec_load();
    return (rec.flags & LIB_SHUF_F_BOOK) && rec.source == LIB_SHUF_SRC_NONE
           && rec.queued == (uint32_t)playlist_amount();
}

/* Ordinary queue, Albums: the queue's album groups in a shuffled order,
 * each album in its own order (Revision 1 item 10). */
struct grp_ctx {
    const uint8_t *bits;
    int n;
    uint32_t seed;
};

static void grp_reorder(unsigned long *indices, int amount, void *ctx)
{
    struct grp_ctx *g = ctx;
    uint8_t *placed;
    int n;

    /* #109: the bits and the scratch bitmap live in the queue's movable
     * slot table, any length: fetched again here, under the playlist
     * mutex, and used before anything yields */
    if (!lib_queue_groups(&g->bits, &n) || n != g->n || amount != n
        || !dbq_groups(NULL, &placed))
        return;
    shuf_group_order(indices, (uint32_t)amount, g->bits, placed, g->seed);
}

static int ordinary_set_mode(enum np_shuffle mode)
{
    struct grp_ctx g;
    int rc = 0;

    if (playlist_amount() <= 0)
        return -1;
    switch (mode)
    {
    case NP_SHUFFLE_SONGS:
        rc = playlist_randomise(NULL, current_tick, true);
        break;
    case NP_SHUFFLE_ALBUMS:
        g.seed = new_seed();
        if (lib_queue_groups(&g.bits, &g.n))
        {
            UPDATE_TRACE("shuf: queue %s groups=%d", "albums", g.n);
            rc = playlist_reorder_ipodui(NULL, grp_reorder, &g, g.seed);
        }
        else
        {
            UPDATE_TRACE("shuf: queue %s in order (no groups)", "albums");
            rc = playlist_sort(NULL, true);
        }
        break;
    default:
        rc = playlist_sort(NULL, true);
        break;
    }
    if (rc < 0)
        return -1;
    global_settings.playlist_shuffle = mode != NP_SHUFFLE_OFF;
    return 0;
}

static int mode_done(enum np_shuffle mode)
{
    (void)mode;                 /* trace only */
    UPDATE_TRACE("shuf: mode %s ok src=%s queued=%d cur=%d",
                 lib_shuffle_mode_name(mode), src_name(), playlist_amount(),
                 playlist_get_current()->index);
    return 0;
}

static int mode_fail(enum np_shuffle mode, const char *why)
{
    (void)mode; (void)why;      /* trace only */
    UPDATE_TRACE("shuf: mode %s fail %s", lib_shuffle_mode_name(mode), why);
    return -1;
}

int lib_shuffle_set_mode(enum np_shuffle mode)
{
    rec_load();
    ses_check();                /* review L2 */
    if (ses.on)                 /* a chunked refill: its record first */
        while (refill_step(LIB_SHUF_WINDOW) == 1)
            ;
    if (lib_shuffle_active() && boot_check)
        boot_resume_check();
    if (!lib_shuffle_active())
    {
        if (ordinary_set_mode(mode) < 0)
            return mode_fail(mode, "queue");
        rec.source = LIB_SHUF_SRC_NONE;
        rec.mode = (uint8_t)mode;
        rec.queued = (uint32_t)playlist_amount();
        rec_write();
        return mode_done(mode);
    }

    if (mode == NP_SHUFFLE_OFF)
    {
        /* the window stays, in play order; the library source ends */
        rec.source = LIB_SHUF_SRC_NONE;
        rec.mode = NP_SHUFFLE_OFF;
        global_settings.playlist_shuffle = false;
        rec_write();
        return mode_done(mode);
    }
    {
        struct mp3entry *id3 = audio_current_track();
        struct lib_tracks lt;
        const char *why = "empty";
        uint32_t cur = SHUF_NONE;
        int gone;

        lt.fd = -1;
        lt.vh = -1;
        if (id3 && id3->tagcache_idx > 0)
            cur = (uint32_t)(id3->tagcache_idx - 1);
        if (!source_init(mode, cur, &lt, &why))
        {
            lib_tracks_close(&lt);
            return mode_fail(mode, why);    /* the old source runs on */
        }
        lib_tracks_close(&lt);
        if (rec.source == LIB_SHUF_SRC_SONGS && cur != SHUF_NONE)
            (void)shuf_songs_next(&rec.it.s, false);    /* playing now */
        /* replace what is queued after the current track: one delete call
         * (under the playlist lock: the index cannot move inside it), the
         * refill, then one audio flush so the buffered next track is the
         * new source's (known item 2: a playlist_delete() per entry posted
         * a flush each and overflowed the audio queue mid-loop) */
        gone = playlist_delete_range_ipodui(NULL, -1, playlist_amount());
        src_tick = playlist_get_current()->created_tick;
        refill();               /* traces "shuf: refill ..." */
        audio_flush_and_reload_tracks();
        UPDATE_TRACE("shuf: mode %s replaced=%d", lib_shuffle_mode_name(mode),
                     gone);
        (void)gone;             /* trace only */
        return mode_done(mode);
    }
}
