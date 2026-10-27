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
#ifndef IPODUI_SEARCH_H
#define IPODUI_SEARCH_H

#include <stdbool.h>
#include <stdint.h>

/* Phase 11 Search (docs/phase11-brief.md sections 3.1, 4.2). Contract,
 * committed before fan-out: search_core.c and library.c (L) implement
 * the functions, screens/search.c (U1) is the screen. */

#define SRCH_MAX_Q      32              /* folded bytes */
enum srch_kind { SRCH_ARTIST, SRCH_ALBUM, SRCH_SONG, SRCH_BOOK,
                 SRCH_EPISODE, SRCH_PLAYLIST, SRCH_KINDS };
#define SRCH_ROW(k, idx)   (((uint32_t)(k) << 29) | ((uint32_t)(idx) & 0x1fffffffu))
#define SRCH_ROW_KIND(r)   ((enum srch_kind)((uint32_t)(r) >> 29))
#define SRCH_ROW_IDX(r)    ((int32_t)((uint32_t)(r) & 0x1fffffffu))

/* The letter strip's characters, in wheel order (⚠️ S3: A-Z then 0-9,
 * clamped at both ends, no wrap; NEXT adds a space, PREV deletes). Lower
 * case here; the strip shows them in upper case. A compile-time constant
 * (owner, section 1.2), read by screens/search.c and by the sim's "type"
 * command (uisimulator/common/sim_cmd_p11.c). */
#define SRCH_ALPHABET   "abcdefghijklmnopqrstuvwxyz0123456789"
#define SRCH_ALPHABET_N 36

struct srch_query {
    char    folded[SRCH_MAX_Q + 1];     /* NUL-terminated, words split by ' ' */
    uint8_t len;
};
/* pure (search_core.c; host-tested, ELF instruction-counted) */
int  srch_fold_query(const char *utf8, struct srch_query *q);   /* returns len */
bool srch_match(const char *hay, const struct srch_query *q);   /* word-prefix (⚠️ S1) */
bool srch_extends(const struct srch_query *prev, const struct srch_query *q);

/* library.c (L). Search entry: loads the group kinds' precompute ROWS
 * (<= 4 section reads, no lib_open) into view-pool slack; returns a
 * handle, < 0 if nothing fits (group kinds then skipped, result capped). */
int  lib_search_begin(void);
void lib_search_end(void);               /* frees the ROWS and the result view */
/* Builds or refines THE one LIB_SEARCH view (returned; the same handle
 * every call). view >= 0 and srch_extends(prev, q): compacts view's rows
 * in place (the set only shrinks; no second view, no allocation).
 * Otherwise rebuilds it from the full sources. Yields every 4096 entries;
 * returns < 0 if 'cancel' says so or the tagcache generation changed
 * (caller retries). Never grows the pool while audio_status() != 0.
 * Rows sorted by sort key across kinds. *capped = true when rows were
 * dropped (the screen then shows "More..." last, ⚠️ S6). */
int  lib_search(const struct srch_query *q, int view, bool *capped,
                bool (*cancel)(void));
/* Trace: "search q=<len> kind_n=<a>,<al>,<s>,<b>,<e> n=<rows> us=<t>
 *         yields=<y> refine=<0|1> cap=<0|n>" */

/* screens/search.c (U1), for the sim's "type" command: the index into
 * SRCH_ALPHABET of the boxed letter while the letter strip shows, else
 * -1. No side effects. */
int  ui_search_strip_pos(void);

#endif /* IPODUI_SEARCH_H */
