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
 * The core heap's integrity walk, logged (developer builds only).
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
#ifndef _IPODUI_HEAPCHK_H_
#define _IPODUI_HEAPCHK_H_

/* Sitting 4gh-b5 (build 4c241d1df3): the last bench step's first
 * allocation set off a compaction that panicked in check_block_handle()
 * on a corrupt header ("handle unaligned").  The corruption came earlier,
 * most likely during duty's arm F (the decode on the COP); suspects, all
 * [hypothesis]: COP writes or dirty lines landing after buflib moved a
 * block, a stray FIQ write on the COP, an overrun from the block before.
 *
 * heapchk walks every block header of the core heap (core_heap_walk():
 * length bounds, handle alignment and range, the handle's back-pointer
 * into the block, the header's CRC word, and the live handle count) and
 * logs the first bad block with the block before it.  It runs at every
 * bench step's start and end, around every audio-buffer shrink and give,
 * at each codec place or unplace between the cores, and every cycle of
 * the bench row "heap" (devbench_cop.c).  On the CPU only, never on the
 * COP; bounded (HEAPCHK_MAX_BLOCKS); a few hundred microseconds.
 *
 * Lines (ipodui.log, tag "heap"): "walk at=<where> n=<blocks> u=<used>
 * us=<cost>" and, the first time a walk fails, "BAD ..." lines naming the
 * block, its words and the block before it (its ops and move callback:
 * the owner, through the link map; this buflib keeps no names). */

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

#ifdef HAVE_BUFLIB_HEAPCHECK
#include "buflib.h"

#define HEAPCHK_MAX_BLOCKS  4096

struct heapchk_stats
{
    uint32_t walks, bad;        /* walks run, of them failed */
    uint32_t max_us, last_us;   /* the slowest walk, the last */
    int last_blocks;            /* the last walk's block count */
    char first_where[16];       /* the first failure's place */
    struct buflib_walk first;   /* and its walk */
};

/* One walk, counted, no line unless it fails (the first failure's lines
 * then, once a boot).  *us: its cost.  0: good, else enum buflib_walk_bad */
int heapchk_walk(const char *where, struct buflib_walk *w, uint32_t *us);
/* One walk and its line ("walk at=..."); 0: good */
int heapchk_at(const char *where);
const struct heapchk_stats *heapchk_stats(void);
/* The "BAD ..." lines for w, through out (ui_log's "heap" lines when
 * out is NULL) */
void heapchk_describe(const char *where, const struct buflib_walk *w,
                      void (*out)(const char *line));
#else
static inline int heapchk_at(const char *where)
{
    (void)where;
    return 0;
}
#endif

#endif /* _IPODUI_HEAPCHK_H_ */
