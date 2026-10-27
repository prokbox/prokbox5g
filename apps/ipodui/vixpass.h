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
 * iPod UI shell: video index pass (Phase 14a)
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
#ifndef _IPODUI_VIXPASS_H_
#define _IPODUI_VIXPASS_H_

/* docs/phase14a-brief.md 3.4; V3 owns this file, vixpass.c and its one
 * caller (library.c lib_precompute(), after the precompute file is
 * written). Video builds only (HAVE_BCMFW_VIDEO). Tagcache thread.
 *
 * Keeps the .vix index of every VIDEO entry of the RAM database current,
 * with the ledger VIX_DIR "/vix.dat" (struct vixpass_ent per indexed
 * video, written as .tmp then renamed): one read of the ledger per pass,
 * no I/O for an entry whose identity (vixpass_core.h vpc_id()) and
 * tag_mtime it holds; new or changed videos are opened for their size
 * and indexed (vix_build_file()) unless their index is there already;
 * the indexes of entries no longer in the database, renamed or moved
 * ones included, are removed. Runs only with audio stopped (re-checked
 * immediately before each file, no yield between); returns LIB_PRE_WAIT
 * (library.h) as soon as 'stop' says so or playback starts, else
 * LIB_PRE_DONE. 'scratch' (>= 4 KB) comes from the precompute's own
 * block. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vixpass_ent {            /* 16 bytes, native byte order */
    uint32_t id;                /* vpc_id(): tags and filename seek */
    uint32_t mtime;             /* tag_mtime == dir_get_info().mtime */
    uint64_t name;              /* vix_name()'s 16 hex digits; 0: none */
};

int vixpass_run(void *scratch, size_t size, bool (*stop)(void));

#endif /* _IPODUI_VIXPASS_H_ */
