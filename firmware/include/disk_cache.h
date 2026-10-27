/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2014 by Michael Sevakis
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
#ifndef DISK_CACHE_H
#define DISK_CACHE_H

#include "mutex.h"
#include "mv.h"

static inline void dc_lock_cache(void)
{
    extern struct mutex disk_cache_mutex;
    mutex_lock(&disk_cache_mutex);
}

static inline void dc_unlock_cache(void)
{
    extern struct mutex disk_cache_mutex;
    mutex_unlock(&disk_cache_mutex);
}

void * dc_cache_probe(IF_MV(int volume,) sector_t secnum,
                      unsigned int *flags);
void dc_dirty_buf(void *buf);
#if defined(HAVE_IPOD_UI) && !defined(BOOTLOADER)
/* ProkBox (R1, #107): the cached copy of a sector or NULL, claiming nothing */
void * dc_peek(IF_MV(int volume,) sector_t secnum);
#endif
void dc_discard_buf(void *buf);
void dc_commit_all(IF_MV_NONVOID(int volume));
void dc_discard_all(IF_MV_NONVOID(int volume));

void dc_init(void) INIT_ATTR;

/* in addition to filling, writeback is implemented by the client */
extern void dc_writeback_callback(IF_MV(int volume, ) sector_t sector,
                                  void *buf);

#if defined(HAVE_IPOD_UI) && !defined(DC_UNORDERED_WRITEBACK)
/* ProkBox (#crash-fat): allocation reaches the disk before any reference
 * to it. Dirty sectors the client calls "first" (the FAT) are written
 * before every other dirty sector of the volume, at a commit and before a
 * dirty "later" sector (a directory entry, FSInfo) is evicted. A halt
 * between the two leaves orphan clusters rather than an entry pointing at
 * a free cluster (which the next allocation turns into a cross-link).
 * Frees ride the same order: a truncate committed with its size update
 * can leave a chain shorter than the size, never a cross-link. */
#define DC_ORDERED_WRITEBACK
extern bool dc_writeback_first(IF_MV(int volume, ) sector_t sector);
#endif


/** These synchronize and can be called by anyone **/

/* expropriate a buffer from the cache of DC_CACHE_BUFSIZE bytes */
void * dc_get_buffer(void);
/* return buffer to the cache by buffer */
void dc_release_buffer(void *buf);

#endif /* DISK_CACHE_H */
