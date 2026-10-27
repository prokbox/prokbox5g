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

/* iPod UI shell: MOD_NOTES's export table (P12 W1-N): slot 0 its module
 * header, slot 1 the Notes list (its root screen, extras_roots.h). Built
 * as a module (IPODUI_IN_MODULE) or linked statically as
 * mod_exports_notes (the simulator, a resident build). */
#include "config.h"
#include "lang.h"
#include "ipodui/modload.h"
#include "ipodui/screen_stack.h"
#include "ipodui/screens/notes.h"

/* The working block (screens/notes.c NOTES_WORK_KB): 16 KB from the
 * module arena (notes.mod's work); 8 KB from the screen arena in a
 * resident build */
#ifdef IPODUI_MODULES
MOD_DESC_A_PAGED(notes, LANG_IPODUI_X_NOTES, 16, NOTES_PAGES);
#else
MOD_DESC_A_PAGED(notes, LANG_IPODUI_X_NOTES, 8, NOTES_PAGES);
#endif

MOD_EXPORTS(notes, MOD_SLOT_FIRST + 1) = {
    [MOD_SLOT_DESC]  = &mod_desc_notes,
    [MOD_SLOT_FIRST] = &ui_x_notes_screen,
};
