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
#ifndef _IPODUI_NP_PLAYBACK_H_
#define _IPODUI_NP_PLAYBACK_H_

/* Contract (docs/phase5-brief.md, "Contracts (committed)"). P implements
 * it in np_playback.c, which replaces the shared stub np_playback_stub.c
 * (filled from audio_current_track(): no lyrics read, no chapter titles,
 * no chapter art, no library shuffle, so the screen works from hour one).
 * N (screens/nowplaying.c, np_draw.c) is the only caller, except where
 * marked. Changes go through the orchestrator.
 *
 * Threads: every function is UI-thread only unless marked "audio
 * thread". Nothing here allocates, splashes or boosts. Anything that can
 * touch storage says so.
 *
 * This header includes no Rockbox headers (the host tests check the
 * layout of struct np_track against the "np" block, np_layout.h). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "artkey.h"         /* artcache_key_t */
#include "np_scrub.h"       /* struct np_scrub_ctx */
#include "np_state.h"       /* enum np_shuffle */
#include "np_pbcore.h"      /* ratings, counter source, seqlock rule */

#define NP_TXT 128          /* bytes per text field, incl. the NUL */

/* P6 (§3.3-3.4, Rev 2): the whole-book view of a book queue. Filled by
 * np_pb_snapshot() from the book block (ab_book.h) when the queue is a
 * book (lib_queue_is_book()) and the playing file's path CRC matches the
 * map's entry for its queue position; else active = false and the screen
 * keeps the P5 per-file bar and counter (traced "ab map" mismatch).
 * book_ms advances with np_pb_elapsed_ms(): position = file_start +
 * elapsed (ab_book_pos()); N recomputes it at 1 Hz, no I/O. */
#define NP_BOOK_TICK_BYTES 28   /* = AB_TICK_BYTES (asserted, ab_book_core.c) */
struct np_book_pos {
    bool     active;            /* bar, times and counter span the book */
    uint32_t book_ms;           /* whole-book position at the snapshot */
    uint32_t len_ms;            /* whole-book length (0: unknown) */
    uint32_t file_start_ms;     /* the playing file's start in the book */
    int      entry;             /* 0-based book entry at the snapshot; its
                                   title is t->album (same entry, owner
                                   2026-10-08) */
    int      entries;           /* M of "N of M" */
    uint8_t  ticks[NP_BOOK_TICK_BYTES]; /* the book map's entry starts at
                                   AB_TICK_RES (= NP_BAR_W) bits over
                                   len_ms; N rescales with
                                   ab_ticks_scale() to the drawn bar */
};

/* The current track as the screen draws it. Copied by np_pb_snapshot()
 * at a seq change into the screen's "np" block (never on the stack). */
struct np_track {
    uint32_t seq;               /* np_pb_seq() the copy was taken under */
    uint32_t track_id;          /* track identity (Rev 3): changes only on a
                                   real track change, never on a chapter
                                   table rewrite; seq guards the table only */
    char title[NP_TXT];         /* "" never: the file name if untagged */
    char artist[NP_TXT];        /* may be "" */
    char album[NP_TXT];         /* audiobook with chapters: the title of
                                   the chapter current at the snapshot;
                                   at a chapter change N refreshes it with
                                   np_pb_chapter_title() */
    bool is_audiobook;          /* book or podcast: the audiobook cycle
                                   (NP-2: no Shuffle step) */
    bool is_podcast;            /* the queue counter, never chapters */
    bool has_lyrics;            /* embedded lyrics located (no I/O) */
    int  stars;                 /* 0..5 from the database at the snapshot
                                   (np_rating_from_tag(id3->rating)); the
                                   screen applies its np_rating_ovr */
    int  queue_index;           /* 0-based position in the play queue */
    int  queue_count;           /* entries in the play queue (0: none) */
    unsigned long length_ms;    /* the current file (a single-file book is
                                   the whole book; multi-file books: per
                                   file until P6, frozen default 9.12) */
    struct np_scrub_ctx scrub;  /* length_ms, and the chapter starts of
                                   the current file read in place from
                                   playback's chapter table (fixed carve):
                                   valid only while np_pb_seq() == seq,
                                   checked with no yield before each use
                                   (np_pbcore.h). snap = is_audiobook. */
    artcache_key_t art_key;     /* album or book cover (0: none); per
                                   chapter: np_pb_art_key() */
    long tc_idx;                /* id3->tagcache_idx: 1-based, 0 = none */
    uint32_t path_crc;          /* ab_path_crc(id3->path): which file the
                                   snapshot is of (np_pb_entry_moved()) */
    int  db_gen;                /* tagcache_ui_generation() at the copy */
    int32_t bind;               /* id3->tagcache_bind: tc_idx's snapshot */
    struct np_book_pos book;    /* P6: whole-book bar/counter (book queue) */
};

/* What np_pb_snapshot() found */
enum np_snap {
    NP_SNAP_NONE = 0,           /* stopped (paused still snapshots):
                                   pop (frozen default 9.15) */
    NP_SNAP_OK,                 /* *t filled, t->seq stable */
    NP_SNAP_BUSY,               /* playback is rewriting the track's data
                                   (seq odd or moved during the copy):
                                   *t unchanged, try again next tick */
};

/* ---- reading ------------------------------------------------------------ */

/* The seqlock counter (np_pbcore.h): one volatile read. Poll it from
 * tick() and before using t->scrub.ch; a change means "snapshot again". */
uint32_t      np_pb_seq(void);

/* The playing track's identity: +1 at every track change (the value a
 * snapshot taken now would put in track_id). One volatile read; for the
 * trace that checks a drawn frame against what plays. */
uint32_t      np_pb_track(void);

/* Copies the current track (audio_current_track(), the playlist position,
 * the chapter column) under the seqlock. No I/O, no yield. */
enum np_snap  np_pb_snapshot(struct np_track *t);

/* Whether id3 is a book, as the snapshot's is_audiobook has it: the
 * parser's flag (M4B, chapters), or the scan's audiobook or podcast class
 * in the RAM index; a video never is.  *is_podcast: the podcast class.
 * No I/O, no yield: the visualizer's tap uses it too (viz_tap.c), so the
 * audiobook Now Playing and the tap's refusal can't disagree. */
struct mp3entry;
bool          np_pb_is_book(const struct mp3entry *id3, bool *is_podcast);

/* #69 (npmark.h): the playing book's key (*key, 0 none: no book queue
 * or stopped) and its current entry as Now Playing's counter shows it
 * (book_fill(): a single-file book's chapter, else the map's entry),
 * at np_pb_elapsed_ms(). Returns the entry, -1 when no mapped book file
 * plays (*key may still be set), -2 while playback rewrites the track's
 * data (ask again). No I/O, no yield: one path CRC and a chapter search. */
int np_pb_book_entry(uint64_t *key);

/* Rev 3 (known item 8): the current file's length can arrive after the
 * snapshot (a placeholder id3 from the playlist or the database while
 * the track loads, a length the codec corrects). Call at 1 Hz (with the
 * elapsed refresh): if t->track_id is still the playing track and its
 * length changed, updates t->length_ms and t->scrub.length_ms and
 * returns true (redraw the bar row). length_ms 0 means "unknown": hide
 * the fill and the remaining time, never clamp elapsed to it. No I/O. */
bool np_pb_refresh_length(struct np_track *t);

/* #58: the track's database entry can arrive after the snapshot. On a
 * start or a skip to a track not yet loaded, TRACK_CHANGE copies a
 * placeholder id3 (tagcache_idx 0, rating 0); the loaded id3, with the
 * index and rating tagtree's TRACK_BUFFER handler put in, is copied over
 * it later (playback.c playing_id3_sync()) with no seq move. Identity
 * stays t->track_id; validity is the entry: true when t->track_id is
 * still the playing track and its tagcache_idx differs from t->tc_idx
 * (snapshot again: the audiobook/podcast class, the stars and the
 * rating target come from it). One volatile read and a compare, no I/O:
 * poll from tick() with the seq. */
bool np_pb_entry_moved(const struct np_track *t);

/* Elapsed ms of the current file (id3->elapsed, or the pinned value in
 * the sim). 0 before the position has loaded (owner note: a brief 0:00
 * before a resume position loads matches stock). */
unsigned long np_pb_elapsed_ms(void);

/* Chapter 'chapter' (array index) of the current track's table, copied
 * into buf (always terminated). False (buf "") if seq != np_pb_seq() or
 * there is no such chapter. No I/O. */
bool np_pb_chapter_title(uint32_t seq, int chapter, char *buf, size_t size);

/* The art cache key to show for 'chapter' (< 0: the cover): the chapter's
 * own key (artcache_key_chapter()) when the art cache holds an image for
 * it, else t->art_key. One index lookup (RAM fences, at most one 4 KB
 * page read): call it only while Now Playing is lit and visible
 * (Revision 1 item 11). Stub: t->art_key. */
artcache_key_t np_pb_art_key(const struct np_track *t, int chapter);

/* The playing track's cover key (t->art_key without a snapshot), or 0:
 * stopped, no track, or playback rewriting it (ask again). Under the
 * seqlock; no I/O, no yield. The main menu's Now Playing panel. */
artcache_key_t np_pb_cover_key(void);

/* Reads the current track's embedded lyrics (metadata_read_lyrics()) into
 * buf: one open + at most 'size' bytes. Returns the length, or -1 (no
 * lyrics, a read error, or a track change since the snapshot under
 * 'seq'; a same-track seq move keeps the read, Rev 3). Storage: one wake. Called
 * on entering Lyrics, after the body push's strip capture (Revision 1
 * item 5). Stub: -1. */
int  np_pb_read_lyrics(uint32_t seq, char *buf, size_t size, bool *truncated);

/* ---- writing ------------------------------------------------------------ */

/* Seeks the current file to ms, once per scrub gesture (np_state's
 * result.seek) or chapter key. 'seq' is the snapshot's t->seq: the seek
 * is dropped (false, traced) only if playback stopped, seq is odd, or a
 * real track change happened since that snapshot (Rev 3: a chapter-table
 * rewrite on the same track moves seq but keeps the seek). Calls
 * audio_pre_ff_rewind() then audio_ff_rewind(), as the other seek
 * paths. */
bool np_pb_seek(unsigned long ms, uint32_t seq);

/* P6 §3.4: seeks to whole-book position book_ms (a book queue, t->book
 * active): within the playing file = np_pb_seek(); in another file of the
 * book = one queue skip to that file with a resume offset (one seek per
 * gesture). Dropped (false, traced) if track_id is no longer the playing
 * track or the book map does not cover book_ms. Stub: false. */
bool np_pb_seek_book(uint32_t book_ms, uint32_t track_id);

/* Volume: 'clicks' wheel clicks (+ = louder), one sound step each,
 * clamped. Saved with the settings when storage is next idle. */
void np_pb_volume_step(int clicks);
int  np_pb_volume_pct(void);    /* 0..100 for the volume bar */

/* Rating: writes np_rating_to_tag(stars) to master index tc_idx - 1 of
 * the library snapshot 'bind' (np_track.bind), queued without disk I/O.
 * Another snapshot by now (an update published meanwhile): the tagcache
 * thread binds 'path' (the rated file, np_pb_copy_path()) instead. False
 * (logged) if dropped. N then sets its override. */
bool np_pb_set_rating(long tc_idx, int32_t bind, const char *path, int stars);
/* The playing file's path into dst if its ab_path_crc() is path_crc, else
 * "" (false): the path a later rating of that track binds to. */
bool np_pb_copy_path(char *dst, size_t size, uint32_t path_crc);

/* The shelf (np_state's shuffle_commit). Library source: re-init from the
 * current entry (lib_shuffle_set_mode()). Ordinary queue: Songs =
 * playlist_randomise, Off = playlist_sort, Albums = the album-group
 * reorder in RAM (Revision 1 item 10); one control-file rewrite. The
 * setting is global_settings.playlist_shuffle plus the albums flag in
 * npshuf.dat (frozen default 9.13). */
enum np_shuffle np_pb_shuffle(void);
void np_pb_set_shuffle(enum np_shuffle s);
int  np_pb_repeat(void);        /* REPEAT_* (glyph under the header) */

/* ---- background --------------------------------------------------------- */

/* Called by ui_nowplaying_idle() (NAV-H2) on every main-loop pass, shown
 * or not: runs the library-shuffle refill (lib_shuffle_idle()) when a
 * posted event asked for it. Returns ticks until it wants the next call,
 * or TIMEOUT_BLOCK (< 0). Never blocks on I/O unless a refill is due.
 * Stub: TIMEOUT_BLOCK. */
int  np_pb_idle(void);

/* Refill trigger events (Revision 1 item 2): np_playback.c registers
 * PLAYBACK_EVENT_TRACK_CHANGE and BUFFER_EVENT_FILL_DONE / REBUFFER
 * callbacks (audio/buffering thread). They only set a flag and, once per
 * flag, post NP_PB_WAKE to the button queue (button_queue_post()); the
 * shell's input loop passes the SYS event to default_event_handler(),
 * which ignores it, and the next loop pass calls np_pb_idle(). No polling:
 * with the screen dark the loop sleeps in TIMEOUT_BLOCK until the post. */
#ifdef MAKE_SYS_EVENT
#define NP_PB_WAKE  MAKE_SYS_EVENT(SYS_EVENT_CLS_MISC, 0x20)
#endif

/* ---- audio-thread hooks (playback.c, guarded by HAVE_IPOD_UI) ----------
 * P6 Rev 1 item 3: playback keeps NP_AB_SLOTS packed chapter tables
 * (chapters.h, AB_CH_TABLE_SIZE each) in the audio scratch area. The
 * writer (buf_read_audiobook()) always fills the slot NOT referenced by
 * id3_get(PLAYING_ID3)->audiobook and brackets the rewrite with
 * begin(slot) / end(slot); the scratch re-carve (which moves both) uses
 * NP_AB_SLOT_ALL. Each slot has its own seq (validity): begin makes
 * np_pb_ab_seq(slot) odd, end even again (+1 each). Both also move
 * np_pb_seq() by +1 (it stays the screen's "snapshot again" poll).
 * Readers: identity first (the slot audio_current_track()->audiobook
 * points at, with the track id unchanged across the copy), validity
 * second (that slot's seq even and unchanged, np_seq_read_ok()). An
 * identity mismatch reads as "no chapters yet", never the other slot.
 * Slot index: 0..NP_AB_SLOTS-1, in scratch order. Audio thread only,
 * except np_pb_ab_seq() (any thread, one volatile read). */
#define NP_AB_SLOTS     2
#define NP_AB_SLOT_ALL  (-1)
void     np_pb_ab_write_begin(int slot);
void     np_pb_ab_write_end(int slot);
uint32_t np_pb_ab_seq(int slot);

#ifdef SIMULATOR
/* T's np_pin script command (sim_cmd_np.c): pins the elapsed time
 * (< 0: unpin) and the volume bar's percentage (< 0: unpin) for
 * deterministic screenshots. */
void np_pb_sim_pin(long elapsed_ms, int volume_pct);
#endif

#endif /* _IPODUI_NP_PLAYBACK_H_ */
