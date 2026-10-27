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

/* Phase 9 scratch slot: plugin_get_buffer() without the plugin API.
 * Contract: apps/scratchbuf.h. Built under IPODUI_TRIM_PLUGINS only
 * (apps/SOURCES). The 0x14000 case relies on PLUGIN_BUFFER_SIZE being
 * IPODUI_SCRATCH_SIZE (ipodvideo.h, Phase 9 B), which sizes pluginbuf. */
#include "config.h"
#include "system.h"
#include "scratchbuf.h"

#ifndef IPODUI_TRIM_PLUGINS
#error "scratchbuf.c is built only with IPODUI_TRIM_PLUGINS"
#endif

#if IPODUI_SCRATCH_SIZE == 0

void *plugin_get_buffer(size_t *buffer_size)
{
    if (buffer_size)
        *buffer_size = 0;
    return NULL;
}

#else /* IPODUI_SCRATCH_SIZE != 0 */

#include "cuesheet.h"

_Static_assert(sizeof(struct cuesheet) <= IPODUI_SCRATCH_SIZE,
               "scratch slot must hold a cuesheet");

#if (CONFIG_PLATFORM & PLATFORM_HOSTED)
static unsigned char pluginbuf[IPODUI_SCRATCH_SIZE];
#else
extern unsigned char pluginbuf[];   /* app-pp.lds, PLUGIN_BUFFER_SIZE */
#endif

void *plugin_get_buffer(size_t *buffer_size)
{
    *buffer_size = IPODUI_SCRATCH_SIZE;
    return pluginbuf;
}

#endif /* IPODUI_SCRATCH_SIZE */
