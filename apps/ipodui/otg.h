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
#ifndef IPODUI_OTG_H
#define IPODUI_OTG_H

#include <stdint.h>
#include <stddef.h>

/* Phase 11 On-The-Go store (docs/phase11-brief.md sections 3.2, 4.4).
 * Contract, committed before fan-out; otg.c (L) implements it.
 *
 * Files (hidden; the format is documented for #53):
 *   OTG_DIR/OTG_LIST  UTF-8 paths, one per line, a plain .m3u8 (readable
 *                     over USB; Save is a byte copy). NEVER handed to the
 *                     playlist engine (Rev 1 H-2): playing it builds a
 *                     fresh queue from its paths.
 *   OTG_IDX           struct otg_idx_hdr, then one struct otg_rec per line.
 * Identity of an entry = its path line. Validity of the whole idx =
 * commitid == tagcache_ui_commitid(); of one cached index = check ==
 * otg_check(idx). Written on the UI thread only. */
#define OTG_DIR      ROCKBOX_DIR "/ipodui"
#define OTG_LIST     "otg.m3u8"          /* in OTG_DIR: a plain playlist */
#define OTG_IDX      OTG_DIR "/otg.idx"
#define OTG_MAX      2000                /* = max_files_in_playlist on this target */
#define OTG_MAGIC    0x544f5049u         /* "IPOT" */
#define OTG_VERSION  1
struct otg_idx_hdr {                     /* 20 B */
    uint32_t magic, version;             /* version 1 */
    uint32_t count;                      /* records; must equal the list's lines */
    uint32_t sum;                        /* libfile_sum of the records */
    int32_t  commitid;                   /* tagcache_ui_commitid() at resolve;
                                            != current -> resolve (Rev 1 L-3) */
};
/* An append writes the list lines, then the records, then this header
 * (2 writes to otg.idx). count < lines after a cut -> resolve the tail. */
struct otg_rec { int32_t idx; uint32_t check; };   /* 8 B; idx -1 = unresolved */

#ifndef OTG_FORMAT_ONLY                  /* host tools: the format alone */
uint32_t otg_check(int32_t idx);         /* hash(title string, tag_length); RAM only */
int  otg_count(void);                    /* cached; no I/O after the first call */
int  otg_add_track(long tc_idx);         /* 1-based tc_idx as np_track; returns added or < 0 */
int  otg_add_row(int view, int idx);     /* a song or a group row: all its tracks, capped */
int  otg_remove(int pos);
int  otg_clear(void);
int  otg_save(char *name, size_t len);   /* "New Playlist N" -> /Playlists, then clear */
int  otg_open_view(void);                /* LIB_OTG view; resolves once if needed:
                                            ONE bulk pass, buffers in the list
                                            strip (32 KB read + 16 KB set) */
int  otg_play(int pos);                  /* a fresh current playlist built from the
                                            store's paths (playlist_insert_context_*,
                                            as play_view(), capped OTG_MAX), then
                                            start at pos. NEVER playlist_create()s
                                            the store file (Rev 1 H-2) */
#endif
/* Trace: "otg add n=%d reads=%d writes=%d", "otg resolve n=%d unresolved=%d
 *         bytes=%lu reads=%d us=%lu", "otg play n=%d pos=%d reads=%d",
 *         "otg save name=%s n=%d", "otg clear" */

#endif /* IPODUI_OTG_H */
