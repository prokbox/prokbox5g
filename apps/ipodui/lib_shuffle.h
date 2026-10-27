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
#ifndef _IPODUI_LIB_SHUFFLE_H_
#define _IPODUI_LIB_SHUFFLE_H_

/* Contract (docs/phase5-brief.md, "Contracts (committed)"; design:
 * docs/design/shuffle.md, integration points 1-8). P owns lib_shuffle.c;
 * until it lands the shared stub (np_playback_stub.c) answers "no library
 * source". The resume record and its checks are pure (lib_shuffle_rec.c,
 * host-tested by tests/nowplaying/test_shuffle_rec.c). No Rockbox headers
 * here. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "shuffle_core.h"   /* struct shuf_songs, struct shuf_albums */
#include "np_state.h"       /* enum np_shuffle */

/* Refill window (shuffle.md item 3) */
#define LIB_SHUF_WINDOW     50      /* entries added per refill */
#define LIB_SHUF_LOW        10      /* refill when this few are left... */
#define LIB_SHUF_URGENT     2       /* ...at once (wake the disk) at this */
#define LIB_SHUF_HISTORY    100     /* played entries kept before deleting */

/* ---- the resume record: ROCKBOX_DIR "/" LIB_SHUF_FILE -------------------
 * Written (whole, 80 B) after every refill and every shelf commit, AFTER
 * the playlist control file is synced (a power cut in between leaves an
 * older record: 'queued' then disagrees and the run restarts a lap,
 * shuffle.md item 5 and Device checks). Native byte order. It also holds
 * the shelf's albums flag for ordinary queues (frozen default 9.13). */
#define LIB_SHUF_FILE       "npshuf.dat"
#define LIB_SHUF_MAGIC      0x46535049u     /* "IPSF" */
#define LIB_SHUF_VERSION    1

enum lib_shuf_src {
    LIB_SHUF_SRC_NONE = 0,      /* an ordinary queue */
    LIB_SHUF_SRC_SONGS,         /* Shuffle Songs / over-cap Songs */
    LIB_SHUF_SRC_ALBUMS,        /* Albums on a library source */
};

/* The database the iterator's numbers belong to (Revision 1 item 6).
 * tagcache_ui_generation() is not in it: it changes at every boot's RAM
 * load, so it is kept in RAM only (see lib_shuffle.c). */
struct lib_shuf_stamp {         /* 16 B */
    int32_t  commitid;          /* tagcache_ui_commitid() */
    int32_t  entries;           /* tagcache_ui_count() */
    uint32_t sortkey_version;   /* SORTKEY_VERSION */
    uint32_t albums_sum;        /* libfile_sec.sum of the LIB_ALBUMS ROWS
                                   section (album numbers are its rows);
                                   0 when the source is not ALBUMS */
};

struct lib_shuffle_rec {        /* 80 B, checked by a static assert */
    uint32_t magic, version;
    struct lib_shuf_stamp stamp;
    uint8_t  source;            /* enum lib_shuf_src */
    uint8_t  mode;              /* enum np_shuffle: the shelf setting */
    uint8_t  repeat;            /* REPEAT_* when written (informative) */
    uint8_t  flags;             /* LIB_SHUF_F_* (was 'pad', always 0: a
                                   record from before P3 reads as none) */
    uint32_t queued;            /* playlist_amount() after the refill the
                                   record follows (control file synced) */
    union {
        struct shuf_songs  s;   /* LIB_SHUF_SRC_SONGS */
        struct shuf_albums a;   /* LIB_SHUF_SRC_ALBUMS */
    } it;
    uint32_t crc;               /* lib_shuf_rec_sum() of the bytes before */
};
#define LIB_SHUF_REC_SIZE   80
/* The queue is one audiobook queued alone by lib_play() (Revision 2: it
 * ends by stopping, even with Repeat All). Kept across a reboot: valid
 * for an ordinary queue (source NONE) of exactly 'queued' entries. */
#define LIB_SHUF_F_BOOK     0x01

enum lib_shuf_check {
    LIB_SHUF_RESUME = 0,        /* continue: same database, same queue */
    LIB_SHUF_RESTART,           /* valid, but the database or the queue
                                   changed: lib_shuf_rec_restart(), keep
                                   the queued window playing */
    LIB_SHUF_BAD,               /* no record (short read), bad magic,
                                   version or CRC: an ordinary queue,
                                   shelf mode from playlist_shuffle */
};

/* Fills magic, version and crc (flags are kept). */
void lib_shuf_rec_seal(struct lib_shuffle_rec *r);
/* 'len' = bytes read from the file. now/queued_now: the database and the
 * playlist after the boot's resume. A record of an ordinary queue
 * (source NONE) only needs to be intact: RESUME (its mode is the shelf
 * setting). */
enum lib_shuf_check lib_shuf_rec_check(const struct lib_shuffle_rec *r,
                                       size_t len,
                                       const struct lib_shuf_stamp *now,
                                       uint32_t queued_now);
/* A new lap over the current database (shuffle.md item 6): Songs over n
 * entries, Albums over n albums, seeded shuf_lap_seed(seed, lap + 1),
 * 'first' none; the stamp becomes *now. Then seal and write again. */
void lib_shuf_rec_restart(struct lib_shuffle_rec *r, uint32_t n,
                          const struct lib_shuf_stamp *now);
uint32_t lib_shuf_rec_sum(const struct lib_shuffle_rec *r);

/* ---- the runtime (lib_shuffle.c, P; UI thread unless marked) ------------ */

/* Replaces the playlist with a library source and starts playback:
 * mode NP_SHUFFLE_SONGS over every music entry ('first' = the master index
 * the user picked, plays first, or SHUF_NONE), NP_SHUFFLE_ALBUMS over the
 * album list (TRACKS column, libfile.h; 'first' = album row or SHUF_NONE).
 * Adds LIB_SHUF_WINDOW entries, writes the record. Quiet. 0 or < 0. */
int  lib_shuffle_start(enum np_shuffle mode, uint32_t first);
bool lib_shuffle_active(void);  /* the queue is a library source */
/* The shelf setting: a library source's mode, else playlist_shuffle plus
 * the record's albums flag. */
enum np_shuffle lib_shuffle_mode(void);
/* Shelf commit on a library source: re-init the iterator in 'mode' from
 * the current entry and replace what is queued after it (re-reading
 * playlist->index under the playlist mutex, Revision 1 item 2). OFF ends
 * the library source (the window stays, in play order). 0 or < 0. */
int  lib_shuffle_set_mode(enum np_shuffle mode);
/* "off" / "songs" / "albums" (trace lines) */
const char *lib_shuffle_mode_name(enum np_shuffle mode);
/* Audio thread (np_playback.c's track-change callback): sets a flag only. */
void lib_shuffle_track_changed(void);
/* From np_pb_idle(): the refill if the flag is set and the window is low
 * (shuffle.md item 3; never holds the playlist mutex across
 * tagcache_retrieve() or a yield). Ticks to the next check, or
 * TIMEOUT_BLOCK. */
int  lib_shuffle_idle(void);
/* A power-off or a sleep entry: a chunked refill still open ends now,
 * so the record matches the queue (no restart lap at the next boot).
 * UI thread. */
void lib_shuffle_settle(void);
/* Boot, after the playlist resume: reads and checks the record. */
void lib_shuffle_resume(void);
/* lib_play() built a queue: one audiobook alone (book) or anything else.
 * Writes the record (80 B) only when the persisted flag changes, or for
 * a book whose queue length differs. UI thread. */
void lib_shuffle_note_book(bool book);
/* After a boot's playlist_resume(): the resumed queue is the book the
 * record names (flag set, an ordinary queue, same length). UI thread. */
bool lib_shuffle_resumed_book(void);

#endif /* _IPODUI_LIB_SHUFFLE_H_ */
