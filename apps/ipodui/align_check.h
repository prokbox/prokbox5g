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
 * iPod UI shell
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
#ifndef _IPODUI_ALIGN_CHECK_H_
#define _IPODUI_ALIGN_CHECK_H_

/* CHECK_ALIGN(p): a struct carved out of a larger block (a view in the
 * library pool, the art cache's work and read blocks) must sit at an
 * address its type's alignment allows. The simulator's buflib hands out
 * 8-byte aligned blocks, so a carve-out at a bad offset panics there at
 * once, in every script, not only under UBSan (H1). On the target an
 * unaligned word load rotates the data instead of faulting; the check is
 * compiled out. */
#ifdef SIMULATOR
#include <stdint.h>
#include "panic.h"
#define CHECK_ALIGN(p) \
    do { \
        if ((uintptr_t)(p) & (__alignof__(*(p)) - 1)) \
            panicf("misaligned %s %p", #p, (void *)(p)); \
    } while (0)
#else
#define CHECK_ALIGN(p) ((void)0)
#endif

#endif /* _IPODUI_ALIGN_CHECK_H_ */
