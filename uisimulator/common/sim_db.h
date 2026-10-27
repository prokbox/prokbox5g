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
 * Simulator-only database hooks for the script driver (sim_script.c)
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
#ifndef SIM_DB_H
#define SIM_DB_H

#include <stdbool.h>

enum sim_db_state {
    SIM_DB_NONE = -1,   /* this build has no database (no HAVE_TAGCACHE) */
    SIM_DB_BUSY,        /* booting, scanning or committing */
    SIM_DB_READY,       /* tagcache_is_usable(), no commit running */
    SIM_DB_MISSING,     /* checked at boot: no database, nothing running */
    SIM_DB_DELAYED,     /* scanned, but the commit waits for the next boot
                           (no buffer to commit into: the sim has no
                           dircache to steal, see commit() in tagcache.c) */
};

enum sim_db_state sim_db_state(void);
/* Commits that ran (past the dirty mark) and ended this boot, ok or
 * not: wait_commit latches on it; -1 in builds that do not count them */
long sim_db_commits(void);
/* Start a full database build (tagcache_rebuild(), no prompt) */
void sim_db_build(void);
/* Start an incremental update (tagcache_update(): scan for new files,
 * commit, reload the RAM cache, drop deleted files), as the background
 * update does; returns immediately */
void sim_db_update(void);
/* Rebuild the database from scratch (tagcache_rebuild(): remove the
 * files, scan, commit; the shell exports and re-imports the statistics) */
void sim_db_rebuild(void);
/* The saved resume position (tag_lastelapsed, ms) of the file at 'path',
 * -1 if none, -2 if it is not in the database */
long sim_db_lastelapsed(const char *path);
/* The tag_rating (stars x 2) of the file at 'path', -2: not in the database. */
long sim_db_rating(const char *path);
/* The album tag of the file at 'path' (false: not in the database). */
bool sim_db_album(const char *path, char *buf, int size);
/* Runs the changelog import (tagcache_import_changelog()) on the calling
 * thread, for timing; false if there was nothing to import */
bool sim_db_import(void);
/* Entries in the database's master index, -1 if unknown */
int  sim_db_entries(void);
/* audio_status() (AUDIO_STATUS_PLAY 1, _PAUSE 2), -1 without playback */
int  sim_audio_status(void);
/* Changes on every RAM cache load/drop (tagcache_ui_generation() in the
 * --ipodui sim), -2 while the cache is busy; -1 in builds without it */
int  sim_db_generation(void);
/* The --ipodui shell's Music menu shows "Updating Library..." (its
 * lib_state() is LIB_UPDATING); false in other builds */
bool sim_db_lib_updating(void);
/* The active library generation's number (changes at a publication or a
 * reload), busy or not; -1 in builds without the shell's cache */
int  sim_db_active_generation(void);
/* Marks the database on disk dirty, as a power cut in the middle of a
 * commit leaves it (the master header's dirty flag; the next boot finds
 * it unusable); false if there is no database file */
bool sim_db_set_dirty(void);
/* With library generations the database files live in the active
 * generation's directory: removes <that directory>/<leaf> (a script's
 * "rm_file /.rockbox/database_..." falls back to it); false otherwise */
bool sim_db_remove_active(const char *leaf);
/* Retain an actual search across publication and check uninterrupted access. */
bool sim_db_snapshot_begin(void);
bool sim_db_snapshot_switch(long timeout_ms);
bool sim_db_snapshot_stress_begin(void);
bool sim_db_snapshot_stress_switch(long timeout_ms);
bool sim_db_stale_begin(void);
bool sim_db_stale_restore(void);
bool sim_db_stale_finish(long timeout_ms);
bool sim_db_snapshot_deferred(long timeout_ms);
bool sim_db_snapshot_after_playback(long timeout_ms);
bool sim_db_snapshot_play(void);
void sim_db_snapshot_pause(void);

#endif /* SIM_DB_H */
