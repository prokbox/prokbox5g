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

/* tests/fatcrash/dcidx.c: the disk cache finds the right slot for a pointer
 * anywhere inside a cache buffer (#355).
 *
 * fat.c marks directory entries dirty through a pointer to the 32-byte
 * entry, not to the start of its sector.  The upstream DCIDX_FROM_BUF took
 * a difference of pointers to 512-byte arrays; GCC may then assume the
 * byte distance is a whole number of buffers and folded dc_dirty_buf()'s
 * range check into "byte offset <= 63 * 512", so an entry inside the LAST
 * buffer was never marked dirty and its change was lost at eviction or
 * commit.
 *
 * This builds the real firmware/common/disk_cache.c (included, so the test
 * can read cache_entry[] and cache_buffer[]) and, for every slot and a set
 * of offsets inside it, checks that dc_dirty_buf() sets DCE_DIRTY on that
 * slot and that dc_commit_all() writes that sector exactly once; then
 * that dc_discard_buf() through an interior pointer drops the right slot,
 * and that pointers outside the cache touch nothing.  run.sh builds it
 * with the firmware's optimisation levels (-Os, -O2) and the host's GCC
 * where there is one.  Verdict line: "dcidx: PASS" / "dcidx: FAIL".
 * DCIDX_SRC names the disk_cache.c to test (the red run passes the old
 * file).
 */
#include <stdio.h>
#include <stdint.h>
#include "config.h"

#ifndef DCIDX_SRC
#define DCIDX_SRC "disk_cache.c"
#endif
#include DCIDX_SRC

static sector_t wb_sector[DC_NUM_ENTRIES * 4];
static int wb_n;

void dc_writeback_callback(IF_MV(int volume,) sector_t sector, void *buf)
{
    (void)buf;
    if (wb_n < (int)(sizeof wb_sector / sizeof wb_sector[0]))
        wb_sector[wb_n] = sector;
    wb_n++;
}

#ifdef DC_ORDERED_WRITEBACK
bool dc_writeback_first(IF_MV(int volume,) sector_t sector)
{
    (void)sector;
    return false;
}
#endif

#define BASE_SECTOR 1000

/* opaque to the optimiser: the test must not let the compiler see the
 * pointer arithmetic that dc_dirty_buf() undoes */
static void * volatile launder_sink;
static void *launder(void *p)
{
    launder_sink = p;
    return launder_sink;
}

int main(void)
{
    static const unsigned offs[] = { 0, 1, 32, 256, 480, 511 };
    unsigned int nslot = 0, ndirty = 0, nwrite = 0, ndiscard = 0, nout = 0;
    unsigned int fails = 0;
    int last32 = 0;   /* the #355 case: last slot, entry 1 of the sector */

    dc_init();

    /* claim every slot: slot i caches sector BASE_SECTOR + i */
    sector_t slot_sector[DC_NUM_ENTRIES];
    for (unsigned int i = 0; i < DC_NUM_ENTRIES; i++)
        slot_sector[i] = (sector_t)-1;
    for (unsigned int s = 0; s < DC_NUM_ENTRIES; s++)
    {
        unsigned int flags;
        uint8_t *buf = dc_cache_probe(IF_MV(0,) BASE_SECTOR + s, &flags);
        unsigned int idx = (unsigned int)((buf - &cache_buffer[0][0])
                                          / DC_CACHE_BUFSIZE);
        if (idx >= DC_NUM_ENTRIES || slot_sector[idx] != (sector_t)-1)
        {
            printf("setup: probe %u gave a bad or repeated slot\n", s);
            printf("dcidx: FAIL\n");
            return 1;
        }
        slot_sector[idx] = BASE_SECTOR + s;
    }
    for (unsigned int i = 0; i < DC_NUM_ENTRIES; i++)
        if (!(cache_entry[i].flags & DCE_INUSE))
        {
            printf("setup: slot %u not in use\n", i);
            printf("dcidx: FAIL\n");
            return 1;
        }
    nslot = DC_NUM_ENTRIES;

    /* dirty through an interior pointer, then commit */
    for (unsigned int i = 0; i < DC_NUM_ENTRIES; i++)
        for (unsigned int k = 0; k < sizeof offs / sizeof offs[0]; k++)
        {
            void *p = launder(&cache_buffer[i][offs[k]]);
            wb_n = 0;
            dc_dirty_buf(p);
            int dirty = !!(cache_entry[i].flags & DCE_DIRTY);
            /* no other slot may have been marked */
            int other = 0;
            for (unsigned int j = 0; j < DC_NUM_ENTRIES; j++)
                if (j != i && (cache_entry[j].flags & DCE_DIRTY))
                    other = 1;
            dc_commit_all(IF_MV(0));
            int wrote = wb_n == 1 && wb_sector[0] == slot_sector[i];
            if (dirty && !other)
                ndirty++;
            if (wrote)
                nwrite++;
            if (i == DC_NUM_ENTRIES - 1 && offs[k] == 32)
                last32 = dirty && !other && wrote;
            if (!dirty || other || !wrote)
            {
                if (fails < 12)
                    printf("slot %2u +%3u: dirty=%d other=%d writes=%d "
                           "(want 1 of sector %lu)\n", i, offs[k], dirty,
                           other, wb_n, (unsigned long)slot_sector[i]);
                fails++;
            }
        }

    /* pointers outside the cache are ignored (release behaviour) */
    {
        uint8_t *lo = &cache_buffer[0][0];
        void *outside[] = {
            launder(lo - 1), launder(lo - DC_CACHE_BUFSIZE),
            launder(lo + DC_NUM_ENTRIES * DC_CACHE_BUFSIZE),
            launder(lo + DC_NUM_ENTRIES * DC_CACHE_BUFSIZE + 32),
        };
        for (unsigned int k = 0; k < sizeof outside / sizeof outside[0]; k++)
        {
            wb_n = 0;
            dc_dirty_buf(outside[k]);
            dc_commit_all(IF_MV(0));
            if (wb_n == 0)
                nout++;
            else
            {
                printf("outside pointer %u marked a slot dirty\n", k);
                fails++;
            }
        }
    }

    /* discard through an interior pointer drops exactly that slot */
    for (unsigned int i = 0; i < DC_NUM_ENTRIES; i++)
    {
        dc_discard_buf(launder(&cache_buffer[i][32]));
        int dropped = cache_entry[i].flags == 0;
        int others = 0;
        for (unsigned int j = i + 1; j < DC_NUM_ENTRIES; j++)
            if (!(cache_entry[j].flags & DCE_INUSE))
                others = 1;
        if (dropped && !others)
            ndiscard++;
        else
        {
            if (fails < 24)
                printf("discard slot %2u +32: dropped=%d others=%d\n",
                       i, dropped, others);
            fails++;
        }
    }

    unsigned int ncase = DC_NUM_ENTRIES * (sizeof offs / sizeof offs[0]);
    printf("dcidx: %u slots; dirty %u/%u, written %u/%u, discard %u/%u, "
           "outside ignored %u/4 (last slot +32: %s)\n",
           nslot, ndirty, ncase, nwrite, ncase, ndiscard,
           (unsigned)DC_NUM_ENTRIES, nout,
           last32 ? "ok" : "LOST");
    printf("dcidx: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
