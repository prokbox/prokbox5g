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
 * iPod UI shell: developer screen "Loader & storage" (#106, tenant B)
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
#ifndef IPODUI_MODULES_DEVLOADER_H
#define IPODUI_MODULES_DEVLOADER_H

#include "ipodui/screen_stack.h"

/* The module loader's and the disk's live numbers, a read-only page in
 * the black-and-white developer style (modules/devloader.c). enter()'s
 * arg (const void *)1 renders the snapshot chrome only (mod_snap_*);
 * snapshot rows: UI_DARK_HDR_H (22) .. 239, below the dark header. In
 * the module build the core sees the stand-in name (modload.h). */
#define DEVLOADER_SNAP_ARG  ((const void *)1)
#define DEVLOADER_SNAP_Y    22          /* == UI_DARK_HDR_H */
#define DEVLOADER_SNAP_H    (240 - DEVLOADER_SNAP_Y)

extern const struct ui_screen_ops ui_devloader_screen;

#endif /* IPODUI_MODULES_DEVLOADER_H */
