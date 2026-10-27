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

/* host stub for tests/uilog: the core allocator, for the bench session's
 * ring (ui_log_bench); t_core_fail makes the next allocation fail */
#ifndef STUB_UILOG_CORE_ALLOC_H
#define STUB_UILOG_CORE_ALLOC_H
#include <stddef.h>

extern int t_core_live;         /* allocations not yet freed */
extern int t_core_fail;         /* the next core_alloc fails */
int core_alloc(size_t size);
void *core_get_data(int h);
void core_pin(int h);
void core_unpin(int h);
int core_free(int h);
#endif
