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
 * iPod UI shell: Now Playing's UI-thread facade over playback (Phase 5)
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

/* np_playback.h, implemented (replaces the shared stub). Contract:
 * docs/phase5-brief.md, "Contracts (committed)" D1-D10.
 *
 * Threads: the audio and buffering threads only touch pb_seq (the
 * seqlock, D2) and the wake flags (D7), and post one NP_PB_WAKE per flag
 * to the button queue. Everything else runs on the UI thread, never
 * allocates, never splashes, never boosts.
 *
 * Static RAM: 4 + 4 + 4 + 1 + 1 + 4 (+ 8 in the sim) bytes here; the
 * library source's state is in lib_shuffle.c.
 *
 * Trace (UPDATE_TRACE: sim.log in the simulator, logf with LOGF_ENABLE,
 * nothing on a release build). One line per event, never per tick:
 *   np snap id=<track_id> seq=<seq> len=<ms> ch=<chapters> book=<0|1>
 *                                 one per NP_SNAP_OK (a seq change)
 *   np length id=<track_id> <old> -> <new>
 *                                 np_pb_refresh_length() saw the current
 *                                 file's length change (0 = unknown)
 *   np seek ms=<ms> id=<track_id> pre=1
 *                                 a seek handed to playback
 *   np seek ms=<ms> dropped (<why>)
 *                                 why: stopped | busy | track
 *   np rating tc=<tc_idx> stars=<n> ok
 *   np rating tc=<tc_idx> stars=<n> dropped (<why>)
 *                                 why: no-entry | stale
 *   np lyrics dropped (<why>)     why: open | track
 *   np lyrics cut bytes=<n>
 *   np shuffle <from> -> <to>     the shelf commit (np_pb_set_shuffle);
 *                                 lib_shuffle.c then logs "shuf: mode"
 * Mode names: off, songs, albums. */

#include <string.h>
#include "string-extra.h"
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "button.h"
#include "file.h"
#include "audio.h"
#include "metadata.h"
#include "metadata/chapters.h"
#include "metadata/lyrics.h"
#include "playlist.h"
#include "playback.h"         /* audio_ab_slot_of() */
#include "settings.h"
#include "sound.h"
#include "misc.h"
#include "appevents.h"
#include "tagcache.h"
#include "logf.h"
#include "ipodui/anim.h"        /* UPDATE_TRACE */
#include "ipodui/artcache.h"
#include "ipodui/vlimit.h"
#include "ipodui/volcurve.h"
#include "ipodui/np_playback.h"
#include "ipodui/lib_shuffle.h"
#include "ipodui/np_layout.h"   /* its static asserts, on the target too */
#include "ipodui/ab_book.h"
#include "ipodui/ab_store.h"
#include "ipodui/library.h"

/* Written by the audio thread only (np_pbcore.h). */
static volatile uint32_t pb_seq;
/* per chapter slot validity (P6 Rev 1 item 3); audio thread writes */
static volatile uint32_t ab_seq[NP_AB_SLOTS];
/* Track identity (Rev 3): +1 at every PLAYBACK_EVENT_TRACK_CHANGE, and
 * the seq right after that event (seqs from 'track_seq' on belong to the
 * current track; a table rewrite moves seq, never these). Audio thread. */
static volatile uint32_t pb_track;
static volatile uint32_t track_seq;
static volatile bool wake_posted;   /* one NP_PB_WAKE in the queue at most */
static bool pb_hooked;
/* P6 store trigger: the stop's record (from the track's finish); the
 * write-now itself lives in the store (ab_store_pending_now(), H1) */
static struct ab_rec fin_rec;
static volatile bool fin_valid;
static uint32_t miss_track;         /* "ab map mismatch" once per track */
#ifdef SIMULATOR
static long pin_elapsed = -1;
static int pin_volume = -1;
#endif

/* ---- events (audio / buffering threads) -------------------------------- */

static void post_wake(void)
{
    if (!wake_posted)
    {
        wake_posted = true;
        button_queue_post(NP_PB_WAKE, 0);
    }
}

void np_pb_ab_wake(void)
{
    post_wake();
}

/* the map position of a track (by its path CRC), or -1 */
static int book_q_of(const struct mp3entry *id3)
{
    struct ab_book_block *b = ab_book_block();
    uint32_t crc;
    int q;

    if (!b || !id3 || b->map.src == AB_MAP_NONE)
        return -1;
    crc = ab_path_crc(id3->path);
    for (q = 0; q < b->map.file_count; q++)
        if (b->map.file_crc[q] == crc)
            return q;
    return -1;
}

static void track_changed(unsigned short id, void *data)
{
    struct track_event *te = data;
    struct ab_rec r;

    (void)id;
    pb_track++;
    pb_seq += 2;                /* stays even (odd only inside a rewrite) */
    track_seq = pb_seq;
    lib_shuffle_track_changed();
    /* P6: a track change within a book is a pending checkpoint (Rev 1
     * item 7), written when storage next runs; the UI pass registers it */
    if (te && ab_book_checkpoint_id3(&r, te->id3, book_q_of(te->id3),
                                     false, false))
    {
        ab_store_pending(&r);
        post_wake();
    }
    else if (lib_shuffle_active() || ab_shell_owns_resume())
        post_wake();
}

/* P6: the finishing track's spot, kept for a stop (the stop event comes
 * after playback wiped the track); a book queue's last file ending on
 * its own is the book's end (FINISHED, D17). */
static void track_finished(unsigned short id, void *data)
{
    struct track_event *te = data;
    struct ab_book_block *b = ab_book_block();
    int q;
    bool ended;

    (void)id;
    if (!te || !te->id3 || (te->flags & TEF_REWIND) || !b)
        return;
    q = book_q_of(te->id3);
    ended = (te->flags & TEF_AUTO_SKIP) && q >= 0
            && q == b->map.file_count - 1;
    fin_valid = ab_book_checkpoint_id3(&fin_rec, te->id3, q, true, ended);
}

static void stop_or_pause(unsigned short id, void *data)
{
    struct ab_rec r;

    (void)id; (void)data;
    if (!(audio_status() & AUDIO_STATUS_PLAY))
    {
        /* stopped (a queue or book that ended, or a stop): a seq move so
         * Now Playing re-snaps, sees nothing playing and pops (spec "When
         * the queue ends"); with no move it slept on, showing the
         * stale track */
        pb_seq += 2;
        post_wake();
    }
    if (audio_status() & AUDIO_STATUS_PAUSE)
    {
        /* the iPod has no stop: a pause is where a listener stops, so
         * within 30 s of the end it finishes the book (D17) */
        if (!ab_book_checkpoint(&r, true))
            return;
    }
    else if (fin_valid)
        r = fin_rec;            /* the stop: the finish's record */
    else
        return;
    fin_valid = false;
    /* pause/stop: write now (⚠️12); a flush before the UI pass (USB,
     * shutdown: clean_shutdown() stops audio, then flushes) writes it */
    ab_store_pending_now(&r);
    post_wake();
}

static void buffer_event(unsigned short id, void *data)
{
    (void)id; (void)data;
    /* storage just ran: a refill rides this wake (shuffle.md item 3) */
    if (lib_shuffle_active())
        post_wake();
}

static void hook(void)
{
    if (pb_hooked)
        return;
    pb_hooked = true;
    add_event(PLAYBACK_EVENT_TRACK_CHANGE, track_changed);
    add_event(PLAYBACK_EVENT_TRACK_FINISH, track_finished);
    add_event(PLAYBACK_EVENT_STOP_OR_PAUSE, stop_or_pause);
    add_event(BUFFER_EVENT_FILL_DONE, buffer_event);
    add_event(BUFFER_EVENT_REBUFFER, buffer_event);
}

static void ab_seq_bump(int slot)
{
    int i;

    for (i = 0; i < NP_AB_SLOTS; i++)
        if (slot == NP_AB_SLOT_ALL || slot == i)
            ab_seq[i]++;
    pb_seq++;
}

void np_pb_ab_write_begin(int slot)
{
    ab_seq_bump(slot);
}

void np_pb_ab_write_end(int slot)
{
    ab_seq_bump(slot);
}

uint32_t np_pb_ab_seq(int slot)
{
    return slot >= 0 && slot < NP_AB_SLOTS ? ab_seq[slot] : 1;
}

/* ---- reading ------------------------------------------------------------ */

uint32_t np_pb_seq(void)
{
    hook();
    return pb_seq;
}

uint32_t np_pb_track(void)
{
    return pb_track;
}

static void copy_str(char *dst, const char *src)
{
    strlcpy(dst, src ? src : "", NP_TXT);
}

static const struct audiobook_chapters *chapters_of(const struct mp3entry *id3)
{
    const struct audiobook_chapters *ab = id3->audiobook;

    return ab && ab->chapter_count > 0 ? ab : NULL;
}

/* P6 §3.4: the whole-book view of a book queue (D11, D13), from the
 * book block: active only if the playing file's path CRC is the map's
 * for its queue position. No I/O, no yield. */
static void book_fill(struct np_track *t, const struct mp3entry *id3,
                      const struct audiobook_chapters *ab)
{
    struct ab_book_block *b = ab_book_block();
    const struct ab_book_map *m;
    int q = t->queue_index, c = 0;

    if (!b || b->map.src == AB_MAP_NONE
        || !lib_queue_is_book(playlist_get_current()))
        return;
    m = &b->map;
    if (q < 0 || q >= m->file_count || m->file_count != t->queue_count
        || m->file_crc[q] != ab_path_crc(id3->path))
    {
        if (miss_track != t->track_id)
        {
            miss_track = t->track_id;
            UPDATE_TRACE(AB_TR_MAP_MISS, q);
        }
        return;
    }
    if (ab)
        c = np_chapter_step(&t->scrub.ch, id3->elapsed, 0);
    t->book.active = true;
    t->book.len_ms = m->len_ms;
    t->book.file_start_ms = m->file_start_ms[q];
    t->book.book_ms = ab_book_pos(m, q, id3->elapsed);
    if (m->file_count == 1 && ab && m->entries <= 1)
    {
        /* a single-file book mapped as its file: its chapters count */
        t->book.entries = ab->chapter_count;
        t->book.entry = c < 0 ? 0 : c;
        ab_book_ticks(&ab_ch_start(ab, 0), sizeof(struct ab_ch_entry),
                      ab->chapter_count, m->len_ms, t->book.ticks);
    }
    else
    {
        t->book.entries = m->entries;
        t->book.entry = ab_book_entry_at(m, q, c < 0 ? 0 : c);
        memcpy(t->book.ticks, m->ticks, sizeof(t->book.ticks));
    }
    /* the album line is the same entry's title: a chapter's (above), or
     * a file entered as itself: its track title (D13) */
    if ((!ab || ab->chapter_count == 0) && t->is_audiobook && !t->is_podcast)
        copy_str(t->album, t->title);
}

bool np_pb_is_book(const struct mp3entry *id3, bool *is_podcast)
{
    int32_t flag = 0;
    bool book;

    /* the classification stored at scan time (RAM index, no I/O):
     * podcasts and genre-tagged books count too */
    if (id3->tagcache_idx > 0
        && !tagcache_ui_seeks((int)id3->tagcache_idx - 1, &flag))
        flag = 0;
    *is_podcast = (flag & TAGCACHE_UI_PODCAST) != 0;
    book = id3->is_audiobook || *is_podcast
           || (flag & TAGCACHE_UI_AUDIOBOOK);
#ifdef HAVE_BCMFW_VIDEO
    /* a video (a chaptered movie, a video podcast) is not a book: no
     * audiobook cycle, no speed (3.2a); the chapters stay in scrub.ch */
    if (id3->is_video || (flag & TAGCACHE_UI_VIDEO))
        book = false;
#endif
    return book;
}

enum np_snap np_pb_snapshot(struct np_track *t)
{
    uint32_t before = np_pb_seq(), track = pb_track;
    const struct audiobook_chapters *ab;
    struct mp3entry *id3;
    const char *title;
    uint32_t slot_seq;
    int slot;

    if (!np_seq_stable(before))
        return NP_SNAP_BUSY;
    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return NP_SNAP_NONE;
    id3 = audio_current_track();
    if (!id3 || !id3->path[0])
        return NP_SNAP_NONE;

    title = id3->title;
    if (!title || !*title)
    {
        title = strrchr(id3->path, '/');
        title = title ? title + 1 : id3->path;
    }
    t->seq = before;
    t->track_id = track;
    copy_str(t->title, title);
    copy_str(t->artist, id3->artist ? id3->artist : id3->albumartist);
    t->is_audiobook = np_pb_is_book(id3, &t->is_podcast);
    t->has_lyrics = metadata_has_lyrics(id3);
    t->stars = np_rating_from_tag(id3->rating);
    t->queue_index = playlist_get_display_index() - 1;
    t->queue_count = playlist_amount();
    t->length_ms = id3->length;
    memset(&t->scrub, 0, sizeof(t->scrub));
    memset(&t->book, 0, sizeof(t->book));  /* P6: B fills it (book map) */
    t->scrub.length_ms = id3->length;
    t->scrub.ch.stride = sizeof(uint32_t);
    t->scrub.snap = t->is_audiobook;
    /* identity: the slot the playing id3 references; validity: that
     * slot's seq, even and unchanged across the copy (Rev 1 item 3) */
    slot = audio_ab_slot_of(id3->audiobook);
    slot_seq = np_pb_ab_seq(slot);
    if (slot >= 0 && (slot_seq & 1))
        return NP_SNAP_BUSY;
    ab = chapters_of(id3);
    copy_str(t->album, id3->album);
    if (ab)
    {
        int c;

        t->scrub.ch.start = &ab_ch_start(ab, 0);
        t->scrub.ch.stride = sizeof(struct ab_ch_entry);
        t->scrub.ch.count = ab->chapter_count;
        /* a book's album line is the chapter title (Revision 2) */
        c = np_chapter_step(&t->scrub.ch, id3->elapsed, 0);
        if (t->is_audiobook && !t->is_podcast && c >= 0
            && ab_ch_title_ptr(ab, c)[0])
            copy_str(t->album, ab_ch_title_ptr(ab, c));
    }
    book_fill(t, id3, ab);
    t->art_key = artcache_key_for_id3(id3);
    t->tc_idx = id3->tagcache_idx;
    t->bind = id3->tagcache_bind;
    t->path_crc = ab_path_crc(id3->path);
    t->db_gen = tagcache_ui_generation();

    if (slot >= 0)
    {
        bool ok = np_pb_ab_seq(slot) == slot_seq;

        UPDATE_TRACE(AB_TR_SLOT_READ, slot, (int)ok);
        if (!ok)
            return NP_SNAP_BUSY;
    }
    if (!np_seq_read_ok(before, pb_seq) || track != pb_track)
        return NP_SNAP_BUSY;    /* no yield above: cannot happen here */
    UPDATE_TRACE("np snap id=%lu seq=%lu len=%lu ch=%d book=%d",
                 (unsigned long)t->track_id, (unsigned long)t->seq,
                 t->length_ms, t->scrub.ch.count, (int)t->is_audiobook);
    return NP_SNAP_OK;
}

int np_pb_book_entry(uint64_t *key)
{
    struct ab_book_block *b = ab_book_block();
    const struct ab_book_map *m;
    const struct audiobook_chapters *ab;
    struct mp3entry *id3;
    uint32_t before = np_pb_seq(), slot_seq;
    int q, c = 0, slot, entry;

    *key = 0;
    if (!np_seq_stable(before))
        return -2;
    if (!b || b->map.src == AB_MAP_NONE || !b->map.book_key
        || !(audio_status() & AUDIO_STATUS_PLAY)
        || !lib_queue_is_book(playlist_get_current()))
        return -1;
    m = &b->map;
    *key = m->book_key;
    id3 = audio_current_track();
    q = playlist_get_display_index() - 1;
    if (!id3 || q < 0 || q >= m->file_count
        || m->file_count != playlist_amount()
        || m->file_crc[q] != ab_path_crc(id3->path))
        return -1;              /* a skip in flight: the key alone */
    slot = audio_ab_slot_of(id3->audiobook);
    slot_seq = np_pb_ab_seq(slot);
    if (slot >= 0 && (slot_seq & 1))
        return -2;
    ab = chapters_of(id3);
    if (ab)
    {
        struct np_chapters ch;

        ch.start = &ab_ch_start(ab, 0);
        ch.stride = sizeof(struct ab_ch_entry);
        ch.count = ab->chapter_count;
        c = np_chapter_step(&ch, np_pb_elapsed_ms(), 0);
        if (c < 0)
            c = 0;
    }
    /* as book_fill(): a single-file book mapped as its file counts its
     * chapters, any other its map's entries */
    if (m->file_count == 1 && ab && m->entries <= 1)
        entry = c;
    else
        entry = ab_book_entry_at(m, q, c);
    if ((slot >= 0 && np_pb_ab_seq(slot) != slot_seq)
        || !np_seq_read_ok(before, pb_seq))
        return -2;
    return entry;
}

bool np_pb_refresh_length(struct np_track *t)
{
    struct mp3entry *id3;
    unsigned long len;

    if (t->track_id != pb_track || !(audio_status() & AUDIO_STATUS_PLAY))
        return false;           /* another track: the seq poll resnaps */
    id3 = audio_current_track();
    if (!id3)
        return false;
    len = id3->length;
    if (len == t->length_ms)
        return false;
    UPDATE_TRACE("np length id=%lu %lu -> %lu",
                 (unsigned long)t->track_id, t->length_ms, len);
    t->length_ms = len;
    t->scrub.length_ms = len;
    return true;
}

bool np_pb_entry_moved(const struct np_track *t)
{
    struct mp3entry *id3;

    if (t->track_id != pb_track || !(audio_status() & AUDIO_STATUS_PLAY))
        return false;           /* another track: the seq poll resnaps */
    id3 = audio_current_track();
    if (!id3 || id3->tagcache_idx == t->tc_idx)
        return false;
    /* a skip swaps the playing id3 before the track-change event bumps
     * pb_track: that is the next file, not this one's entry (the seq
     * poll resnaps it once the event lands) */
    if (ab_path_crc(id3->path) != t->path_crc)
        return false;
    UPDATE_TRACE("np entry id=%lu tc=%ld -> %ld",
                 (unsigned long)t->track_id, t->tc_idx,
                 (long)id3->tagcache_idx);
    return true;
}

unsigned long np_pb_elapsed_ms(void)
{
    struct mp3entry *id3;

#ifdef SIMULATOR
    if (pin_elapsed >= 0)
        return (unsigned long)pin_elapsed;
#endif
    id3 = audio_current_track();
    return id3 ? id3->elapsed : 0;
}

bool np_pb_chapter_title(uint32_t seq, int chapter, char *buf, size_t size)
{
    const struct audiobook_chapters *ab;
    struct mp3entry *id3;

    if (!size)
        return false;
    buf[0] = '\0';
    if (seq != np_pb_seq() || !np_seq_stable(seq))
        return false;
    id3 = audio_current_track();
    ab = id3 ? chapters_of(id3) : NULL;
    if (!ab || chapter < 0 || chapter >= ab->chapter_count)
        return false;
    ab_ch_title(ab, chapter, buf, (int)size);
    return true;
}

artcache_key_t np_pb_art_key(const struct np_track *t, int chapter)
{
    artcache_key_t ck;

    if (chapter < 0 || !t->art_key || !t->is_audiobook)
        return t->art_key;
    ck = artcache_key_chapter(t->art_key, chapter);
    /* one index lookup: the chapter's own bundle if the builder made one
     * (chapter-art.md), else the cover */
    return ck && artcache_state(ck) == ARTCACHE_OK ? ck : t->art_key;
}

artcache_key_t np_pb_cover_key(void)
{
    uint32_t before = np_pb_seq();
    struct mp3entry *id3;
    artcache_key_t key;

    if (!np_seq_stable(before) || !(audio_status() & AUDIO_STATUS_PLAY))
        return 0;
    id3 = audio_current_track();
    if (!id3 || !id3->path[0])
        return 0;
    key = artcache_key_for_id3(id3);
    return np_pb_seq() == before ? key : 0;
}

static const char *seq_track_why(uint32_t seq);

/* Lyrics belong to the track, not the chapter table: a same-track seq
 * move (codec start, rebuffer) does not drop the read (Rev 3). */
int np_pb_read_lyrics(uint32_t seq, char *buf, size_t size, bool *truncated)
{
    char path[MAX_PATH];
    struct mp3entry *id3;
    int fd, n;

    if (size)
        buf[0] = '\0';
    if (truncated)
        *truncated = false;
    if (!size || seq_track_why(seq))
        return -1;
    id3 = audio_current_track();
    if (!id3 || !metadata_has_lyrics(id3))
        return -1;
    strlcpy(path, id3->path, sizeof(path));
    fd = open(path, O_RDONLY);          /* may yield */
    if (fd < 0)
    {
        UPDATE_TRACE("np lyrics dropped (%s)", "open");
        return -1;
    }
    /* id3 is re-fetched after the yield; the track's slot is only
     * rewritten at a track change, which moves seq */
    id3 = audio_current_track();
    n = -1;
    if (!seq_track_why(seq) && id3 && !strcmp(id3->path, path))
        n = metadata_read_lyrics(fd, id3, buf, size, truncated);
    close(fd);
    if (seq_track_why(seq))
    {
        UPDATE_TRACE("np lyrics dropped (%s)", "track");
        buf[0] = '\0';
        return -1;
    }
    if (truncated && *truncated)
        UPDATE_TRACE("np lyrics cut bytes=%d", n);
    return n;
}

/* ---- writing ------------------------------------------------------------ */

/* Does a snapshot taken under 'seq' still describe the playing track?
 * A chapter-table rewrite (codec start, seek rebuffer, scratch re-carve)
 * moves seq within one track and keeps it; only a TRACK_CHANGE ends it. */
static const char *seq_track_why(uint32_t seq)
{
    uint32_t now = np_pb_seq();

    if (!(audio_status() & AUDIO_STATUS_PLAY))
        return "stopped";
    if (!np_seq_stable(seq))
        return "busy";
    if ((int32_t)(seq - track_seq) < 0 || (int32_t)(now - seq) < 0)
        return "track";
    return NULL;
}

bool np_pb_seek(unsigned long ms, uint32_t seq)
{
    const char *why = seq_track_why(seq);

    if (why)
    {
        UPDATE_TRACE("np seek ms=%lu dropped (%s)", ms, why);
        return false;
    }
    UPDATE_TRACE("np seek ms=%lu id=%lu pre=%d", ms,
                 (unsigned long)pb_track, 1);
    /* as every other seek path: pause the PCM and mark ff/rw first */
    audio_pre_ff_rewind();
    audio_ff_rewind((long)ms);
    return true;
}

bool np_pb_seek_book(uint32_t book_ms, uint32_t track_id)
{
    struct ab_book_block *b = ab_book_block();
    const struct ab_book_map *m = b ? &b->map : NULL;
    struct playlist_info *pl = playlist_get_current();
    uint32_t file_ms;
    int q, now, n;

    if (track_id != pb_track || !(audio_status() & AUDIO_STATUS_PLAY)
        || !m || m->src == AB_MAP_NONE || !lib_queue_is_book(pl)
        || (n = playlist_amount()) != m->file_count
        || (m->len_ms && book_ms > m->len_ms))
    {
        UPDATE_TRACE("np seek book ms=%lu dropped (%s)",
                     (unsigned long)book_ms,
                     track_id != pb_track ? "track" : "map");
        return false;
    }
    q = ab_book_file_at(m, book_ms, &file_ms);
    now = playlist_get_display_index() - 1;
    if (q < 0)
        return false;
    UPDATE_TRACE("np seek book ms=%lu q=%d from=%d", (unsigned long)book_ms,
                 q, now);
    if (q == now)
    {
        audio_pre_ff_rewind();
        audio_ff_rewind((long)file_ms);
    }
    else
    {
        /* another file: one queue jump with the resume position (book
         * queues are never shuffled: index order is insert order) */
        playlist_start((pl->index + (q - now) + n) % n, file_ms, 0);
    }
    return true;
}

/* One wheel click is one step of the volume curve (volcurve.c, P15):
 * global_status.volume still stores dB, on the table, so upstream's
 * setvol(), the limit clamp and the saved setting keep working. A volume
 * saved between steps snaps down on the first click (never louder).
 * Trace: "vol step=<n> db=<v> bar=<0..1024>". */
void np_pb_volume_step(int clicks)
{
    int step, db, lim = global_settings.volume_limit;

    if (clicks == 0)
        return;
    step = vol_step_add(global_status.volume, clicks);
    db = vol_step_to_db(step);
    /* clamp in dB: a limit off the table (an old setting) must not turn
     * an up-click into a drop (P15 end review L1) */
    if (db > lim)
        db = lim;
    global_status.volume = db;
    setvol();                   /* sound_set_volume() clamps and applies */
    step = vol_db_to_step(global_status.volume);
    UPDATE_TRACE("vol step=%d db=%d bar=%d", step, global_status.volume,
                 vol_step_to_bar1024(step));
}

int np_pb_volume_pct(void)
{
#ifdef SIMULATOR
    if (pin_volume >= 0)
        return pin_volume;
#endif
    return ui_vol_frac(global_status.volume) * 100 / 1024;
}

/* vlimit.h (P11, Rev 1 L-8; P15): the volume bar's mapping, the volume
 * curve's step linearly in 1/1024 of the bar, clamped; a dB value between
 * steps shows at the step below. Pure: no sim pin (the pin is a display
 * value). */
int ui_vol_frac(int vol)
{
    return vol_step_to_bar1024(vol_db_to_step(vol));
}

bool np_pb_set_rating(long tc_idx, int32_t bind, const char *path, int stars)
{
    /* Bound to the snapshot the row number came from (bind); any other
     * snapshot goes by the full path to the tagcache thread: no disk I/O
     * here and never another file's row (R-M5) */
    unsigned char tag = tag_rating;
    long value = np_rating_to_tag(stars);
    bool ok = tc_idx > 0 && path
        && tagcache_ui_update_stats(path, tc_idx - 1, bind, 1, &tag, &value);
    UPDATE_TRACE("np rating tc=%ld stars=%d %s", tc_idx, stars,
                 ok ? "ok" : (tc_idx > 0 ? "dropped" : "dropped (no-entry)"));
    return ok;
}

bool np_pb_copy_path(char *dst, size_t size, uint32_t path_crc)
{
    struct mp3entry *id3 = audio_current_track();
    dst[0] = '\0';
    if (id3)
        strmemccpy(dst, id3->path, size);
    if (dst[0] && ab_path_crc(dst) == path_crc)
        return true;
    dst[0] = '\0';     /* another track by now, or torn: never mis-rated */
    return false;
}

enum np_shuffle np_pb_shuffle(void)
{
    return lib_shuffle_mode();
}

void np_pb_set_shuffle(enum np_shuffle s)
{
    enum np_shuffle was = np_pb_shuffle();

    if (s == was)
        return;
    UPDATE_TRACE("np shuffle %s -> %s", lib_shuffle_mode_name(was),
                 lib_shuffle_mode_name(s));
    lib_shuffle_set_mode(s);    /* traces "shuf: mode <m> ok|fail <why>" */
}

int np_pb_repeat(void)
{
    return global_settings.repeat_mode;
}

/* ---- background --------------------------------------------------------- */

int np_pb_idle(void)
{
    int a, b;

    hook();
    wake_posted = false;        /* the next event may post again */
    lib_book_resumed_idle();    /* P6: the boot resume's book map */
    a = lib_shuffle_idle();
    /* pause / stop write-now (one flash wake), inserts, forced
     * checkpoints, the seed */
    b = ab_store_idle(true);
    if (a < 0)
        return b;
    if (b < 0)
        return a;
    return MIN(a, b);
}

#ifdef SIMULATOR
void np_pb_sim_pin(long elapsed_ms, int volume_pct)
{
    pin_elapsed = elapsed_ms;
    pin_volume = volume_pct;
}
#endif
