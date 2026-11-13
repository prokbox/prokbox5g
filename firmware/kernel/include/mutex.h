/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2002 by Björn Stenberg
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

#ifndef MUTEX_H
#define MUTEX_H

#include "thread.h"

struct mutex
{
    struct __wait_queue queue;     /* waiter list */
    int                 recursion; /* lock owner recursion count */
    struct blocker      blocker;   /* priority inheritance info
                                      for waiters and owner*/
    IF_COP( struct corelock cl; )  /* multiprocessor sync */
};

extern void mutex_init(struct mutex *m);
extern void mutex_lock(struct mutex *m);
extern void mutex_unlock(struct mutex *m);
#if defined(HAVE_IPOD_UI) || defined(HAVE_BCMFW_DEVTEST)
/* ProkBox #324: owned if free (or already this thread's), else false at
   once; never blocks (a caller bounds its own wait: lcd-video.c) */
extern bool mutex_trylock(struct mutex *m);
#endif

#endif /* MUTEX_H */
