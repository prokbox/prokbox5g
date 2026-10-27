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

/* host stub for tests/ipodui/test_library.c: movable blocks (every
 * core_alloc() and yield() moves all unpinned blocks, so a pointer kept
 * across one is caught: the old copy is poisoned and freed) */
#ifndef STUB_CORE_ALLOC_H
#define STUB_CORE_ALLOC_H
#include <stddef.h>
int   core_alloc(size_t size);
size_t core_allocatable(void);
int   core_free(int handle);            /* returns 0 */
void *core_get_data(int handle);
void  core_pin(int handle);
void  core_unpin(int handle);
#endif
