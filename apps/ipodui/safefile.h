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
#ifndef IPODUI_SAFEFILE_H
#define IPODUI_SAFEFILE_H

#include <stdbool.h>

/* One replace primitive (#91, docs/phase16-brief.md 3.3). Replace dest
 * with tmp so that a power cut at any point leaves either the old or the
 * new file, never two directory entries for a name and never none:
 *   1. the caller writes, fsyncs and closes tmp;
 *   2. remove prev;  3. rename dest -> prev;
 *   4. rename tmp -> dest;  5. remove prev.
 * No step renames onto an existing name, so rbfs_rename()'s overwrite
 * path (duplicates when the victim's removal fails, -338 EIO on the
 * device) is never taken. A dest missing with prev present on entry (a
 * cut between 3 and 4 that recovery has not seen) is put back first.
 * Returns 0, or -1 with dest holding the old file (tmp is left for the
 * caller to remove or to retry with).
 *
 * safefile_recover(): dest missing and prev present -> rename prev back
 * (the old file: the in-flight change is lost, nothing else); dest and
 * prev both present -> remove prev (dest is the new file: only a whole
 * tmp is ever renamed onto it), or only prev's name when it shares
 * dest's chain (a cut inside step 3); tmp (may be NULL) present ->
 * remove it, or only its name when it shares dest's chain (inside 4).
 * prev may be NULL (nothing to put back). Boot order (Revision 1, M3):
 *   playlist control file  playlist_resume()          (playlist.c)
 *   journal + generations  tagcache_thread() start     (tagcache.c)
 *   audiobook store        ab_store_load()             (ab_store.c)
 *   On-The-Go              otg's first sync            (otg.c)
 *   saved playlists        first /Playlists listing    (playlists.c)
 *
 * safefile_recover_dir(): saved playlists in /Playlists only (never a
 * scan of the user's own folders, M1). Those are written remove-then-
 * rename with a marker (safefile_intent_set()); a cut save is finished
 * from the marker by safefile_intent_recover() first. "<name><suffix>"
 * left after that is stray and goes (only its name when it shares
 * <name>'s chain); it is never renamed into place (L1), and <name>, a
 * user's playlist, is never touched. A temporary the marker (may be
 * NULL) still names is left for the marker's next recovery. Only names
 * whose base ends ".m3u" or ".m3u8" count. */
/* Aliases (H1): a cut inside a rename (fat.c FAT_ORDERED_DIRENTRIES) can
 * leave both names on one cluster chain; removing either frees the
 * other's data. Every removal of a name whose partner may exist goes
 * through these:
 * safefile_drop_alias(): path is a second name on keep's chain -> only
 *   path's entries go (1); different files, one missing or keep a
 *   directory (0, M1); error (-1).
 *   Always 0 in the simulator and host builds (no shared chains there).
 * safefile_remove(): remove path, or only its name when it is keep's
 *   alias (keep may be NULL: a plain remove). On an undecided alias check
 *   nothing is freed. true when path is gone.
 * safefile_remove_in(): the same, checked against every name in dir (a
 *   temporary whose destination name is not known, e.g. the next free
 *   "New Playlist N"); recovery only, it reads the whole directory. */
int  safefile_drop_alias(const char *path, const char *keep);
bool safefile_remove(const char *path, const char *keep);
bool safefile_remove_in(const char *path, const char *dir);
int  safefile_replace(const char *tmp, const char *dest, const char *prev);
/* A playlist save's remove-then-rename (M1): safefile_intent_set()
 * records dest and the closed temporary's size in a marker file before
 * the remove, the caller removes the marker after the rename.
 * safefile_intent_recover() at boot: dest<suffix> present -> removed when
 * dest exists (or only its name, when it shares dest's chain), renamed
 * into place when dest is missing and it has the recorded size (it was
 * closed before dest was removed; any other temporary stays, L1); then
 * the marker goes. */
/* the playlist save's marker (playlist.c; read by the /Playlists recovery) */
#define SAFEFILE_PL_INTENT ROCKBOX_DIR "/.playlist_save"
bool safefile_intent_set(const char *marker, const char *dest, long size);
void safefile_intent_recover(const char *marker, const char *suffix);
void safefile_recover(const char *dest, const char *tmp, const char *prev);
void safefile_recover_dir(const char *dir, const char *suffix,
                          const char *marker);

#endif /* IPODUI_SAFEFILE_H */
