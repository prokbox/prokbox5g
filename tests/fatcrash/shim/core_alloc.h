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
/* Host shim for tests/fatcrash: the core allocator calls fat.c and
 * file_internal.c make (the directory burst buffer, R1; the walk lookup
 * cache, #243), backed by malloc */
#ifndef FATCRASH_CORE_ALLOC_H
#define FATCRASH_CORE_ALLOC_H
#include <stdlib.h>
#include <stdint.h>
#ifndef CACHEALIGN_SIZE
#define CACHEALIGN_SIZE 16
#endif
#ifndef CACHEALIGN_UP
#define CACHEALIGN_UP(x) \
    (((x) + CACHEALIGN_SIZE - 1) & ~(uintptr_t)(CACHEALIGN_SIZE - 1))
#endif
struct buflib_callbacks;
static void *fatcrash_core_blocks[4];
static inline int core_alloc_ex(size_t size, struct buflib_callbacks *ops)
{
    (void)ops;
    for (int i = 0; i < 4; i++)
        if (!fatcrash_core_blocks[i])
            return (fatcrash_core_blocks[i] = malloc(size)) ? i + 1 : -1;
    return -1;
}
static inline int core_alloc(size_t size)
{ return core_alloc_ex(size, NULL); }
static inline int core_free(int handle)
{
    free(fatcrash_core_blocks[handle - 1]);
    fatcrash_core_blocks[handle - 1] = NULL;
    return 0;
}
/* the walk cache's room check (#243); a test may set it (one variable
 * for the whole program) */
__attribute__((weak)) size_t fatcrash_core_room = 1 << 20;
static inline size_t core_allocatable(void) { return fatcrash_core_room; }
static inline void core_pin(int handle) { (void)handle; }
static inline void *core_get_data(int handle)
{ return fatcrash_core_blocks[handle - 1]; }
#endif
