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
 * iPod UI shell: the Apple fallback's steps on the volume (#137): look
 * at an original's chain, make a link, drop the link folder (the park),
 * repair what a host or a disk checker did to a live link. Target: fat.c
 * (fat_link_*) and the file layer; simulator: a stand-in on the host's
 * files (empty marker files for links). Developer builds only.
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
#ifndef IPODUI_AFB_FAT_H
#define IPODUI_AFB_FAT_H

#include <stdbool.h>
#include <stdint.h>
#include "ipodui/afb_core.h"

struct afb_probe {
    bool exists;
    uint32_t fst, size;
    int healthy;                /* 1 healthy, 0 broken, -1 unreadable */
    int nruns;                  /* -1: more than AFB_MAXRUNS */
    uint32_t runs[2 * AFB_MAXRUNS];
};

/* path's entry (and its chain when chain): 0, or < 0 an I/O error */
int afb_vol_probe(const char *path, struct afb_probe *p, bool chain);
/* a link named name in AFB_LINK_DIR naming (fst, size), read-only +
 * hidden; 0 or < 0 */
int afb_vol_link(const char *name, uint32_t fst, uint32_t size);
/* EMPTIED: path's entry names (fst, size) again (no FAT change) */
int afb_vol_set(const char *path, uint32_t fst, uint32_t size);
/* FREED: 1 every cluster of the runs is free; rechain writes them again */
int afb_vol_runs_free(const uint32_t *runs, int nruns);
int afb_vol_rechain(const uint32_t *runs, int nruns);
/* the park: AFB_LINK_DIR goes, with every entry in it, no chain freed;
 * 0 dropped, 1 there was none, < 0 failed */
int afb_vol_park(void);
/* the volume's fingerprint (FSInfo free count and next free) */
void afb_vol_fp(uint32_t *fp_free, uint32_t *fp_next);

#endif /* IPODUI_AFB_FAT_H */
