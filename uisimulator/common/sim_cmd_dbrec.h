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
#ifndef SIM_CMD_DBREC_H
#define SIM_CMD_DBREC_H

#include <stdbool.h>

/* #85 library boot recovery (tests/ui/ipodui-library-boot-recovery.script):
 * damage the TEST disk's database generations, manifests and statistics
 * journal, then check what the next boot chose. --ipodui sim only. The
 * mark lives in /.rockbox/dbrec.mark so it survives "reboot". "Newest" and
 * "older" mean the two manifests by sequence number at the last mark.
 *
 *   dbrec mark               remember the active entry count, both
 *                            manifests (bytes, seq, gen) and fingerprints
 *                            of gen0-3, the root's idx/0-3 files
 *   dbrec mark_files         the same, keeping the remembered count
 *   dbrec corrupt_newest     flip a byte mid database_3.tcd of the newest
 *                            manifest's generation
 *   dbrec corrupt_referenced the same in both manifests' generations
 *   dbrec truncate_newest_manifest   cut the newest manifest to 100 bytes
 *   dbrec foreign_manifests  both manifests: version 2, CRC re-sealed
 *   dbrec partial_legacy <path> <rating>
 *                            a pre-generation root: no manifests or
 *                            journal; the root keeps only idx/0-3 (copied
 *                            from the active generation) and a changelog
 *                            giving <path> (%20 = space) that rating
 *   dbrec set_rating <path> <value>   path-bound rating (tagcache)
 *   dbrec corrupt_journal    flip a byte in the last full frame of the
 *                            newest statistics journal checkpoint
 *   dbrec expect_active older|newest|unreferenced|gen<N> [ms]
 *                            the active library directory, against the
 *                            mark (polled up to ms, default 0)
 *   dbrec expect_selected older|newest|unreferenced|gen<N>
 *                            the directory this boot's selection chose
 *   dbrec expect_unchanged older|referenced|root|manifests
 *                            fingerprints equal the mark's
 *   dbrec expect_slots_changed <n>   exactly n manifest files differ
 *   dbrec expect_count_ge    active entries >= the marked count
 *   dbrec expect_not_updating [ms]   ready and not "Updating Library..."
 *                            within ms (default 30000)
 *   dbrec expect_published [ms]      the active directory is named by an
 *                            intact manifest of this version
 *   dbrec expect_retries <max>       background retries this boot <= max
 *   dbrec expect_prepares <max>      candidate prepares this boot <= max
 *   dbrec expect_journal_bad         database_stats.bad exists
 *
 * Every expect_* logs "dbrec expect_<x> ok|failed" (failed fails the run
 * in scripts/shots.sh). Returns false for any other command. */
bool sim_cmd_dbrec(const char *cmd, char *argv[], int argc, int lineno);

#endif /* SIM_CMD_DBREC_H */
