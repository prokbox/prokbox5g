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

/* host stub for tests/heapwalk: what buflib_mempool.c takes from
   firmware/export/system.h (the same definitions) */
#ifndef STUB_SYSTEM_H
#define STUB_SYSTEM_H
#include <stddef.h>
#include <stdint.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ALIGN_DOWN(n, a)     ((typeof(n))((uintptr_t)(n)/(a)*(a)))
#define ALIGN_UP(n, a)       ALIGN_DOWN((n)+((a)-1),a)
#define ALIGN_BUFFER(ptr, size, align) \
({                                           \
    size_t    __sz = (size);                 \
    size_t   __ali = (align);                \
    uintptr_t __a1 = (uintptr_t)(ptr);       \
    uintptr_t __a2 = __a1 + __sz;            \
    __a1 = ALIGN_UP(__a1, __ali);            \
    __a2 = ALIGN_DOWN(__a2, __ali);          \
    (ptr)  = (typeof (ptr))__a1;             \
    (size) = __a2 > __a1 ?  __a2 - __a1 : 0; \
})
#define IS_ALIGNED(x, a) (((x) & ((typeof(x))(a) - 1)) == 0)
#ifndef alignof
#define alignof __alignof__
#endif
#endif
