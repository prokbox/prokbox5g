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

/* Phase 9 scratch slot (docs/phase9-brief.md 4.3): stands in for the plugin
 * buffer when the plugin API is trimmed (IPODUI_TRIM_PLUGINS). Same name
 * and signature as plugin.c's, so upstream callers are unchanged.
 * Slot size = IPODUI_SCRATCH_SIZE (ipodvideo-variant.h):
 *   0       -> returns NULL, *buffer_size = 0; no pluginbuf reserved;
 *   0x14000 -> holds a struct cuesheet; returns pluginbuf.
 * Every caller must handle NULL. */
#ifndef _SCRATCHBUF_H_
#define _SCRATCHBUF_H_

#include <stddef.h>

void *plugin_get_buffer(size_t *buffer_size);  /* same name: callers unchanged */

#endif /* _SCRATCHBUF_H_ */
