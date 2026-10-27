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

/* iPod UI shell: MOD_XINDEX's export table (P12 W1-N): slot 0 its module
 * header, slot 1 the builders (struct xidx_ops, xindex.h). Built as a
 * module (IPODUI_IN_MODULE) or linked statically as mod_exports_xindex
 * (the simulator, a resident build), which xindex.c calls directly. */
#include "config.h"
#include "lang.h"
#include "ipodui/modload.h"
#include "ipodui/xindex.h"

extern const struct xidx_ops xidx_ops;      /* screens/xindex.c */

/* Class A, context = sync (xindex.mod): no working block of its own; the
 * driver's maint_take() block is the job's memory */
MOD_DESC_A(xindex, LANG_IPODUI_X_NOTES, 0);

MOD_EXPORTS(xindex, MOD_SLOT_FIRST + 1) = {
    [MOD_SLOT_DESC]  = &mod_desc_xindex,
    [MOD_SLOT_FIRST] = &xidx_ops,
};
