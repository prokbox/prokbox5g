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
#include "config.h"

#ifdef HAVE_BUFLIB_HEAPCHECK
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "core_alloc.h"
#include "string-extra.h"
#include "heapchk.h"
#include "uilog.h"

static struct heapchk_stats hs;     /* ~100 B */

static uint32_t now_us(void)
{
#ifdef SIMULATOR
    return (uint32_t)current_tick * (1000000 / HZ);
#else
    return USEC_TIMER;
#endif
}

static void log_line(const char *line)
{
    ui_log("heap", "%s", line);
}

void heapchk_describe(const char *where, const struct buflib_walk *w,
                      void (*out)(const char *line))
{
    char line[64];

    if (!out)
        out = log_line;
    snprintf(line, sizeof(line), "BAD %s at=%.12s blk=%p",
             buflib_walk_why(w->bad), where, w->at);
    out(line);
    snprintf(line, sizeof(line), "BAD len=%ld h=%p o=%p", (long)w->len,
             w->handle, w->ops);
    out(line);
    snprintf(line, sizeof(line), "BAD crc=%08lx want=%08lx n=%d u=%d hd=%d",
             (unsigned long)w->crc, (unsigned long)w->want, w->blocks,
             w->used, w->handles);
    out(line);
    if (w->prev)
    {
        snprintf(line, sizeof(line), "BAD prev=%p b=%ld h=%d", w->prev,
                 (long)(w->prev_len * (long)sizeof(intptr_t)),
                 w->prev_handle);
        out(line);
        snprintf(line, sizeof(line), "BAD prev o=%p mv=%p", w->prev_ops,
                 w->prev_move);
        out(line);
    }
    else
        out("BAD prev none (the first block)");
}

int heapchk_walk(const char *where, struct buflib_walk *w, uint32_t *us)
{
    uint32_t t0 = now_us(), dt;
    int bad = core_heap_walk(w, HEAPCHK_MAX_BLOCKS);

    dt = now_us() - t0;
    if (us)
        *us = dt;
    hs.walks++;
    hs.last_us = dt;
    hs.last_blocks = w->blocks;
    if (dt > hs.max_us)
        hs.max_us = dt;
    if (bad)
    {
        if (!hs.bad++)
        {
            /* the first failure this boot: kept, and its lines now (a
               compaction may panic on it next) */
            strlcpy(hs.first_where, where, sizeof(hs.first_where));
            hs.first = *w;
            heapchk_describe(where, w, NULL);
        }
    }
    return bad;
}

int heapchk_at(const char *where)
{
    struct buflib_walk w;
    uint32_t us;
    int bad = heapchk_walk(where, &w, &us);

    ui_log("heap", "walk at=%.12s n=%d u=%d us=%lu%s", where, w.blocks,
           w.used, (unsigned long)us, bad ? " BAD" : "");
    return bad;
}

const struct heapchk_stats *heapchk_stats(void)
{
    return &hs;
}
#endif /* HAVE_BUFLIB_HEAPCHECK */
