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

#ifndef IPODUI_DBQUEUE_H
#define IPODUI_DBQUEUE_H

/* Queue by database index (#109, docs/design/queue-by-index.md): the
 * runtime. A queue built from the library holds one playlist word per
 * entry (INSERT | DBQ_FLAG | k) and a slot table k -> master index (or,
 * after a renumbering commit, a seek, then a path offset). The names are
 * resolved when a track loads (get_track_filename(), P1).
 *
 * Locks: playlist -> dbq, never the reverse. dbq_filename() runs under
 * the playlist mutex (any thread: audio, UI, iAP); its slot table is
 * movable and only read into locals under the dbq mutex, before I/O. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dbqueue_core.h"

struct tagcache_context;

/* playlist_init(): safefile recovery of dbq.hdr and dbqpath.dat, then
 * dbq.hdr loaded (design 5.3 steps 1-2); sets dbq_booted. */
void dbq_init(void);
/* Slots of the largest persisted block: resume's pre-reserve (H4). */
uint32_t dbq_total(void);
/* playlist_create(): a new queue (k from 0); the slot table goes. Called
 * with the playlist mutex held. */
void dbq_reset(void);
/* True while the current queue's slot table exists (P6's bounded scan). */
bool dbq_queue_live(void);

/* P1: the name of slot k into buf. Always 0: a failure writes the
 * sentinel "/<dbq>/<k>" (H1). scratch: MAX_PATH+1 B (the caller's
 * dir_buf). Playlist mutex held. */
int dbq_filename(uint32_t k, char *buf, size_t len, char *scratch);

/* P3: a T line at resume, playlist mutex held. Loads the block named by
 * tag (dbq.hdr's row with that seq and count) as the current queue's.
 * 0, or -1 (the replay fails with -19). */
int dbq_resume_block(uint32_t tag, uint32_t first_k, uint32_t count);

/* Building a queue (library.c play_view(), UI thread), after
 * playlist_create(): dbq_need() is the allocation for m slots;
 * dbq_begin() makes it (false: no memory, or no database in RAM);
 * dbq_add() appends a master index and returns its k (-1: full);
 * dbq_commit() writes the payload file and dbq.hdr (design 5.2 steps
 * 2-3) and gives the T line's tag; the slot count, or -1 (the queue is
 * then not resumable and the table is dropped). */
size_t dbq_need(int m);
bool dbq_begin(int m);
int  dbq_add(int32_t idx);
int  dbq_commit(uint32_t *tag);

/* The queue's album groups (bit k: slot k starts an album) and a
 * scratch bitmap of the same size, inside the slot table's allocation.
 * Valid until the next yield; NULL when there is no table. */
uint8_t *dbq_groups(int *n, uint8_t **placed);

/* Tagcache thread (UI_COMMIT). dbq_idx_live(): a block in IDX mode
 * exists (resident or persisted), so a publication must keep the old
 * snapshot. dbq_published(): the publish hook, inside the no-yield
 * window: stores the passive reference (or drops it) and sets a flag,
 * takes no lock (M4). dbq_tc_poll(): the rebase and the materialization,
 * at the top of the thread's loop, unboosted, yielding. dbq_prepare_wait():
 * true while a new generation must not be prepared (not booted, a rebase
 * or a materialization owed, design 4.2.3-4). */
bool dbq_idx_live(void);
void dbq_published(struct tagcache_context *old);
void dbq_tc_poll(void);
bool dbq_prepare_wait(void);
/* 3.20: a rebase or materialization runs now (the header's busy glyph).
 * Any thread; one volatile read. */
bool dbq_working(void);

#endif /* IPODUI_DBQUEUE_H */
