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

#ifndef FATCRASH_SYSTEM_H
#define FATCRASH_SYSTEM_H
#include "config.h"
#include <stdlib.h>
#define letoh16(x) (x)
#define letoh32(x) (x)
#define htole16(x) (x)
#define htole32(x) (x)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define ALIGN_DOWN(n,a) ((n)/(a)*(a))
#define ALIGN_UP(n,a) ALIGN_DOWN((n)+((a)-1),a)
#define UNLIKELY(x) __builtin_expect(!!(x),0)
#define LIKELY(x) __builtin_expect(!!(x),1)
#define ARRAYLEN(a) (sizeof(a)/sizeof((a)[0]))
#define strmemdupa(s,l) ({ char *_d = alloca((l)+1); memcpy(_d,(s),(l)); _d[l]=0; _d; })
#include <alloca.h>
#include <limits.h>
#define PTR_IN_ARRAY(a, p, numelem) \
    ((uintptr_t)(p) - (uintptr_t)(a) < (uintptr_t)(numelem)*sizeof((a)[0]))
#define IBSS_ATTR
#define SHAREDBSS_ATTR
#define IDATA_ATTR
#define ICODE_ATTR
static inline int find_first_set_bit(uint32_t v) { return v ? __builtin_ctz(v) : 32; }
#endif
