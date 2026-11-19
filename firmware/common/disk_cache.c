/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2014 by Michael Sevakis
 * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
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
#include "debug.h"
#include "system.h"
#include "linked_list.h"
#include "disk_cache.h"
#include "fs_defines.h"
#include "bitarray.h"
#include "xcore.h"        /* ProkBox p19-copfileio: the CPU-only check */
#if defined(HAVE_IPOD_UI_DEVMODE) && !defined(BOOTLOADER)
#include "panic.h"        /* ProkBox int4-dcache: dc_dirty_buf()'s check */
#endif

/* Cache: LRU cache with separately-chained hashtable
 *
 * Each entry of the map is the mapped location of the hashed sector value
 * where each bit in each map entry indicates which corresponding cache
 * entries are occupied by sector values that collide in that map entry.
 *
 * Each volume is given its own bit map.
 *
 * To probe for a specific key, each bit in the map entry must be examined,
 * its position used as an index into the cache_entry array and the actual
 * sector information compared for that cache entry. If the search exhausts
 * all bits, the sector is not cached.
 *
 * To avoid long chains, the map entry count should be much greater than the
 * number of cache entries. Since the cache is an LRU design, no buffer entry
 * in the array is intrinsically associated with any particular sector number
 * or volume.
 *
 * Example 6-sector cache with 8-entry map:
 * cache entry 543210
 * cache map   100000 <- sector number hashes into map
 *             000000
 *             000100
 *             000000
 *             010000
 *             000000
 *             001001 <- collision
 *             000000
 * volume map  111101 <- entry usage by the volume (OR of all map entries)
 */

enum dce_flags /* flags for each cache entry */
{
    DCE_INUSE = 0x01, /* entry in use and valid */
    DCE_DIRTY = 0x02, /* entry is dirty in need of writeback */
    DCE_BUF   = 0x04, /* entry is being used as a general buffer */
};

struct disk_cache_entry
{
    struct lldc_node node;  /* LRU list links */
    unsigned char flags;    /* entry flags */
#ifdef HAVE_MULTIVOLUME
    unsigned char volume;   /* volume of sector */
#endif
    sector_t sector;   /* cached disk sector number */
};

BITARRAY_TYPE_DECLARE(cache_map_entry_t, cache_map, DC_NUM_ENTRIES)

static inline unsigned int map_sector(sector_t sector)
{
    /* keep sector hash simple for now */
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    /* 8b-T: 32 buckets (fs_defines.h). Folding in higher bits spreads the
     * first sectors of 8..128-sector clusters over all 32 buckets (a plain
     * modulo puts every cluster start of >= 32 sectors in one) */
    sector ^= (sector >> 5) ^ (sector >> 10);
#endif
    return sector % DC_MAP_NUM_ENTRIES;
}

static struct lldc_head cache_lru; /* LRU cache list (head = LRU item) */
static struct disk_cache_entry cache_entry[DC_NUM_ENTRIES];
static cache_map_entry_t cache_map_entry[NUM_VOLUMES][DC_MAP_NUM_ENTRIES];
static cache_map_entry_t cache_vol_map[NUM_VOLUMES] IBSS_ATTR;
static uint8_t cache_buffer[DC_NUM_ENTRIES][DC_CACHE_BUFSIZE] CACHEALIGN_ATTR;
struct mutex disk_cache_mutex SHAREDBSS_ATTR;

#define CACHE_MAP_ENTRY(volume, mapnum) \
    cache_map_entry[IF_MV_VOL(volume)][mapnum]
#define CACHE_VOL_MAP(volume) \
    cache_vol_map[IF_MV_VOL(volume)]

#define DCE_LRU()      ((struct disk_cache_entry *)cache_lru.head)
#define DCE_NEXT(fce)  ((struct disk_cache_entry *)(fce)->node.next)
#define NODE_DCE(node) ((struct disk_cache_entry *)(node))

/* get the cache index from a pointer anywhere inside a buffer.
 * ProkBox (#355): from the byte offset.  The upstream form subtracted
 * pointers to DC_CACHE_BUFSIZE-byte arrays, which C defines only for a
 * pointer to a buffer's start; GCC took the byte distance to be a whole
 * number of buffers and folded the callers' range check into "offset <=
 * (DC_NUM_ENTRIES - 1) * DC_CACHE_BUFSIZE", so a directory entry inside
 * the last buffer was never marked dirty and its change was lost.
 * Unsigned: a pointer below the cache also fails the range check. */
#define DCIDX_FROM_BUF(buf) \
    ((unsigned int)(((uintptr_t)(buf) - (uintptr_t)cache_buffer) \
                    / DC_CACHE_BUFSIZE))

#if defined(HAVE_IPOD_UI_DEVMODE) && !defined(BOOTLOADER)
#include "panic.h"
/* developer builds: a buffer that is not the cache's, or a slot in the
 * wrong state, is a caller's bug; the panic record names the caller */
static void NORETURN_ATTR dc_bad_buf(const char *what, const void *buf,
                                     void *lr)
{
    panicf("dc %s buf=%08lx lr=%08lx", what, (unsigned long)buf,
           (unsigned long)lr);
}
#define DC_CHECK_BUF(ok, what, buf) \
    do { if (!(ok)) dc_bad_buf((what), (buf), \
                               __builtin_return_address(0)); } while (0)
#else
#define DC_CHECK_BUF(ok, what, buf) do { } while (0)
#endif

#define DCIDX_FROM_DCE(dce) \
    ((dce) - cache_entry)

#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
/* ProkBox (int4-dcache, #355): references the cache's own invariants rule
 * out, counted.  A map bit naming an entry that is not a cached sector
 * (free, or handed out by dc_get_buffer()) is a lost bitmap clear, the
 * shape a stale cache-line write-back leaves (pp5022-reference quirks.md
 * #88); a dirty mark for a buffer outside the cache, or for an entry not
 * in use, is a client writing through a pointer it no longer owns.  The
 * probe and the peek pass such an entry by; developer builds also panic
 * in dc_dirty_buf() */
volatile unsigned int dc_bad_refs;

/* the entry caches this sector: in use as a cached sector, not handed out
 * as a general buffer */
static inline bool dce_holds(const struct disk_cache_entry *dce,
                             sector_t sector)
{
    if (dce->sector != sector)
        return false;
    if ((dce->flags & (DCE_INUSE | DCE_BUF)) != DCE_INUSE)
    {
        dc_bad_refs++;
        DEBUGF("dc: map bit for a free entry\n");
        return false;
    }
    return true;
}
#endif /* HAVE_IPOD_UI && !BOOTLOADER */

/* set the in-use bit in the map */
static inline void cache_bitmap_set_bit(int volume, unsigned int mapnum,
                                        unsigned int bitnum)
{
    cache_map_set_bit(&CACHE_MAP_ENTRY(volume, mapnum), bitnum);
    cache_map_set_bit(&CACHE_VOL_MAP(volume), bitnum);
    (void)volume;
}

/* clear the in-use bit in the map */
static inline void cache_bitmap_clear_bit(int volume, unsigned int mapnum,
                                          unsigned int bitnum)
{
    cache_map_clear_bit(&CACHE_MAP_ENTRY(volume, mapnum), bitnum);
    cache_map_clear_bit(&CACHE_VOL_MAP(volume), bitnum);
    (void)volume;
}

/* make entry MRU by moving it to the list tail */
static inline void touch_cache_entry(struct disk_cache_entry *which)
{
    struct lldc_node *lru = cache_lru.head;
    struct lldc_node *node = &which->node;

    if (node == lru->prev)  /* already MRU */
        ; /**/
    else if (node == lru)   /* is the LRU? just rotate list */
        cache_lru.head = lru->next;
    else                    /* somewhere else; move it */
    {
        lldc_remove(&cache_lru, node);
        lldc_insert_last(&cache_lru, node);
    }
}

/* remove LRU entry from the cache list to use as a buffer */
static struct disk_cache_entry * cache_remove_lru_entry(void)
{
    struct lldc_node *lru = cache_lru.head;

    /* at least one is reserved for client */
    if (lru == lru->next)
        return NULL;

    /* remove it; next-LRU becomes the LRU */
    lldc_remove(&cache_lru, lru);
    return NODE_DCE(lru);
}

/* return entry to the cache list and set it LRU */
static void cache_return_lru_entry(struct disk_cache_entry *fce)
{
    lldc_insert_first(&cache_lru, &fce->node);
}

/* discard the entry's data and mark it unused */
static inline void cache_discard_entry(struct disk_cache_entry *dce,
                                       unsigned int index)
{
    cache_bitmap_clear_bit(IF_MV_VOL(dce->volume), map_sector(dce->sector),
                           index);
    dce->flags = 0;
}

#ifdef DC_ORDERED_WRITEBACK
/* write back every dirty "first" sector (the FAT) of a volume */
static void commit_first(IF_MV_NONVOID(int volume))
{
    FOR_EACH_BITARRAY_SET_BIT(&CACHE_VOL_MAP(volume), index)
    {
        struct disk_cache_entry *dce = &cache_entry[index];

        if ((dce->flags & DCE_DIRTY) &&
            dc_writeback_first(IF_MV(volume,) dce->sector))
        {
            dc_writeback_callback(IF_MV(volume,) dce->sector,
                                  cache_buffer[index]);
            dce->flags &= ~DCE_DIRTY;
        }
    }
}

/* before a dirty sector that may reference allocations goes out alone */
static void evict_ordered(struct disk_cache_entry *dce)
{
    if (!dc_writeback_first(IF_MV(dce->volume,) dce->sector))
        commit_first(IF_MV(dce->volume));
}
#endif /* DC_ORDERED_WRITEBACK */

/* search the cache for the specified sector, returning a buffer, either
   to the specified sector, if it exists, or a new/evicted entry that must
   be filled */
void * dc_cache_probe(IF_MV(int volume,) sector_t sector,
                      unsigned int *flagsp)
{
    unsigned int mapnum = map_sector(sector);

    FOR_EACH_BITARRAY_SET_BIT(&CACHE_MAP_ENTRY(volume, mapnum), index)
    {
        struct disk_cache_entry *dce = &cache_entry[index];

#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
        if (dce_holds(dce, sector))
#else
        if (dce->sector == sector)
#endif
        {
            *flagsp = DCE_INUSE;
            touch_cache_entry(dce);
            return cache_buffer[index];
        }
    }

    /* sector not found so the LRU is the victim */
    struct disk_cache_entry *dce = DCE_LRU();
    cache_lru.head = dce->node.next;

    unsigned int index = DCIDX_FROM_DCE(dce);
    void *buf = cache_buffer[index];
    unsigned int old_flags = dce->flags;

    if (old_flags)
    {
        int old_volume = IF_MV_VOL(dce->volume);
        sector_t sector = dce->sector;
        unsigned int old_mapnum = map_sector(sector);

        if (old_flags & DCE_DIRTY)
        {
#ifdef DC_ORDERED_WRITEBACK
            evict_ordered(dce);
#endif
            dc_writeback_callback(IF_MV(old_volume,) sector, buf);
        }

        if (mapnum == old_mapnum IF_MV( && volume == old_volume ))
            goto finish_setup;

        cache_bitmap_clear_bit(old_volume, old_mapnum, index);
    }

    cache_bitmap_set_bit(IF_MV_VOL(volume), mapnum, index);

finish_setup:
    dce->flags  = DCE_INUSE;
#ifdef HAVE_MULTIVOLUME
    dce->volume = volume;
#endif
    dce->sector = sector;

    *flagsp = 0;
    return buf;
}

#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
/* ProkBox (R1, #107): the buffer caching a sector, if any, without
 * evicting or claiming an entry. fat.c's directory burst reader serves
 * this copy first: it is the newest one (dirty or equal to the disk) */
void * dc_peek(IF_MV(int volume,) sector_t sector)
{
    FOR_EACH_BITARRAY_SET_BIT(&CACHE_MAP_ENTRY(volume, map_sector(sector)),
                              index)
    {
        struct disk_cache_entry *dce = &cache_entry[index];

        if (dce_holds(dce, sector))
            return cache_buffer[index];
    }

    return NULL;
}
#endif /* HAVE_IPOD_UI && !BOOTLOADER */

/* mark in-use cache entry as dirty by buffer */
void dc_dirty_buf(void *buf)
{
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
    /* ProkBox (int4-dcache): the index from the byte offset (the array
     * pointer difference above is undefined for a pointer that is not on
     * an entry's start).  A pointer inside an entry is fine: fat.c marks
     * a directory entry's or FSInfo's sector dirty through a pointer into
     * it.  An entry not in use was freed or handed out under the caller */
    uintptr_t off = (uintptr_t)buf - (uintptr_t)cache_buffer;
    unsigned int index = off / DC_CACHE_BUFSIZE;

    if (off >= sizeof (cache_buffer) ||
        !(cache_entry[index].flags & DCE_INUSE))
    {
        dc_bad_refs++;
#ifdef HAVE_IPOD_UI_DEVMODE
        panicf("dc_dirty_buf: bad buffer %p from %p", buf,
               __builtin_return_address(0));
#endif
        DEBUGF("dc_dirty_buf: bad buffer %p\n", buf);
        return;
    }
#else
    unsigned int index = DCIDX_FROM_BUF(buf);

    DC_CHECK_BUF(index < DC_NUM_ENTRIES, "dirty", buf);
    if (index >= DC_NUM_ENTRIES)
        return;
#endif

    /* dirt remains, sticky until flushed */
    struct disk_cache_entry *fce = &cache_entry[index];
    DC_CHECK_BUF(fce->flags & DCE_INUSE, "dirty !inuse", buf);
    if (fce->flags & DCE_INUSE)
        fce->flags |= DCE_DIRTY;
}

/* discard in-use cache entry by buffer */
void dc_discard_buf(void *buf)
{
    unsigned int index = DCIDX_FROM_BUF(buf);

    DC_CHECK_BUF(index < DC_NUM_ENTRIES, "discard", buf);
    if (index >= DC_NUM_ENTRIES)
        return;

    struct disk_cache_entry *dce = &cache_entry[index];
    DC_CHECK_BUF(dce->flags & DCE_INUSE, "discard !inuse", buf);
    if (dce->flags & DCE_INUSE)
        cache_discard_entry(dce, index);
}

/* commit all dirty cache entries to storage for a specified volume */
void dc_commit_all(IF_MV_NONVOID(int volume))
{
    DEBUGF("dc_commit_all()\n");

#ifdef DC_ORDERED_WRITEBACK
    commit_first(IF_MV(volume));
#endif
    FOR_EACH_BITARRAY_SET_BIT(&CACHE_VOL_MAP(volume), index)
    {
        struct disk_cache_entry *dce = &cache_entry[index];
        unsigned int flags = dce->flags;

        if (flags & DCE_DIRTY)
        {
            dc_writeback_callback(IF_MV(volume,) dce->sector,
                                  cache_buffer[index]);
            dce->flags = flags & ~DCE_DIRTY;
        }
    }
}

/* discard all cache entries from the specified volume */
void dc_discard_all(IF_MV_NONVOID(int volume))
{
    DEBUGF("dc_discard_all()\n");

    FOR_EACH_BITARRAY_SET_BIT(&CACHE_VOL_MAP(volume), index)
        cache_discard_entry(&cache_entry[index], index);
}

/* expropriate a buffer from the cache */
void * dc_get_buffer(void)
{
    XCORE_FS_ASSERT_CPU("dc_get_buffer");   /* ProkBox: CPU only */
    dc_lock_cache();

    void *buf = NULL;
    struct disk_cache_entry *dce = cache_remove_lru_entry();

    if (dce)
    {
        unsigned int index = DCIDX_FROM_DCE(dce);
        unsigned int flags = dce->flags;

        buf = cache_buffer[index];

        if (flags)
        {
            /* must first commit this sector if dirty */
            if (flags & DCE_DIRTY)
            {
#ifdef DC_ORDERED_WRITEBACK
                evict_ordered(dce);
#endif
                dc_writeback_callback(IF_MV(dce->volume,) dce->sector, buf);
            }

            cache_discard_entry(dce, index);
        }

        dce->flags = DCE_BUF;
    }
    /* cache is out of buffers */

    dc_unlock_cache();
    return buf;
}

/* return buffer to the cache by buffer */
void dc_release_buffer(void *buf)
{
    unsigned int index = DCIDX_FROM_BUF(buf);

    DC_CHECK_BUF(index < DC_NUM_ENTRIES, "release", buf);
    if (index >= DC_NUM_ENTRIES)
        return;

    dc_lock_cache();

    struct disk_cache_entry *dce = &cache_entry[index];
    DC_CHECK_BUF(dce->flags & DCE_BUF, "release !buf", buf);

    if (dce->flags & DCE_BUF)
    {
        dce->flags = 0;
        cache_return_lru_entry(dce);
    }

    dc_unlock_cache();
}

/* one-time init at startup */
void dc_init(void)
{
    mutex_init(&disk_cache_mutex);
    lldc_init(&cache_lru);
    for (unsigned int i = 0; i < DC_NUM_ENTRIES; i++)
        lldc_insert_last(&cache_lru, &cache_entry[i].node);
}
