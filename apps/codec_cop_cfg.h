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

/* The codec's configure() calls carried from the COP to the codec thread
 * (codec_cop.c h_configure()). Most settings are values. A few are a
 * pointer to the caller's memory, a struct on the COP worker's stack, and
 * the CPU must not read that through its cache: its lines of the stack can
 * be stale: create_thread() fills the stack with DEADBEEF on the CPU and
 * writes it back without dropping it, and the worker is created again at
 * every codec load, so a load with no CPU discard before the codec's
 * first configure() can read the fill (the laptop's model, after a
 * hold-PLAY sleep).
 * The COP writes its cache back before the call (copj_cache_ops(),
 * COPJ_CALL), so the pointee is read once through its uncached alias into
 * a copy on the CPU's stack, and the setting gets the copy's address.
 * Pure: tests/codecid3 compiles it. */
#ifndef CODEC_COP_CFG_H
#define CODEC_COP_CFG_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The largest pointee copied (a static assert in codec_cop.c holds each
 * entry to it) */
#define COP_CFG_COPY_MAX 32

/* A configure setting whose value points at the caller's memory */
struct cop_cfg_ptr {
    int setting;
    size_t size;            /* bytes the setting reads */
};

/* The pointee's size for a pointer setting, else 0 (a value) */
static inline size_t cop_cfg_ptr_size(const struct cop_cfg_ptr *tab,
                                      size_t n, int setting)
{
    for (size_t i = 0; i < n; i++)
        if (tab[i].setting == setting)
            return tab[i].size;
    return 0;
}

/* The value to pass on the CPU: a value as is; for a pointer setting
 * (size > 0) the address of copy, filled from src, the pointee's uncached
 * alias. NULL stays NULL (REPLAYGAIN_SET_GAINS: "set the defaults"). */
static inline intptr_t cop_cfg_arg(size_t size, intptr_t value, void *copy,
                                   const void *src)
{
    if (!size || !value || size > COP_CFG_COPY_MAX)
        return value;
    memcpy(copy, src, size);
    return (intptr_t)copy;
}

#endif /* CODEC_COP_CFG_H */
