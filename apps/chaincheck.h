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

#ifndef CHAINCHECK_H
#define CHAINCHECK_H

#include <stdint.h>

/* p081-fatdb: a file's cluster chain, read only, before anything removes
 * or truncates it. remove() and O_TRUNC walk the chain and free every
 * cluster it links to; on a chain that is cut (ends on a free cluster) or
 * runs on past its size into clusters another file or a directory owns,
 * that frees live data (the b10 gen2 damage, 2026-11-09).
 *
 * path: a file. runs (start, count pairs) gets up to maxruns runs when
 * healthy; *nruns is -1 when the chain has more runs than that.
 * Returns CHAIN_ABSENT, CHAIN_OK, CHAIN_DAMAGED or CHAIN_UNREADABLE. */
enum {
    CHAIN_UNREADABLE = -1,  /* an I/O error: nothing decided */
    CHAIN_DAMAGED = 0,      /* never remove or truncate through it */
    CHAIN_OK = 1,           /* every cluster allocated, end mark at size */
    CHAIN_ABSENT = 2,
};
int chain_check(const char *path, uint32_t *runs, int maxruns, int *nruns);

/* chain_check() with what the guard's log needs (rep may be NULL): the
 * entry's size and first cluster, and where the walk stopped (stop is
 * fat.h's FAT_CHAIN_*; walked/want in clusters; next the FAT entry there) */
struct chain_report {
    uint32_t size, fst, walked, want, last, next;
    uint8_t stop;
};
int chain_check_why(const char *path, uint32_t *runs, int maxruns,
                    int *nruns, struct chain_report *rep);

/* The guard's log line for a failed check, numbers first (a log line is
 * cut at 64): "<why> s<size> c<first> <walked>/<want> @<last>=<entry>
 * <name>"; why: F free, S short, P past its size, R out of range or no
 * chain, U unreadable, E an empty file naming a cluster. Returns buf */
char *chain_report_line(char *buf, int len, const char *path,
                        const struct chain_report *r);

/* Every cached FAT and directory sector written back and dropped: the
 * next lookups read the medium (dbgen's read-backs). No-op in the sim */
void chain_cache_drop(void);

#endif
