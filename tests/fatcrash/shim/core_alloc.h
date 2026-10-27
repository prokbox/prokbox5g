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
/* Host shim for tests/fatcrash: the core allocator calls fat.c makes
 * (the directory burst buffer, R1), backed by malloc */
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
static inline void core_pin(int handle) { (void)handle; }
static inline void *core_get_data(int handle)
{ return fatcrash_core_blocks[handle - 1]; }
#endif
