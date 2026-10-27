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
#ifndef IPODUI_HOLDMENU_H
#define IPODUI_HOLDMENU_H

#include <stdbool.h>
#include "ipodui/screen_stack.h"

/* Phase 11 hold-SELECT menu (docs/phase11-brief.md sections 3.3, 4.5).
 * Contract, committed before fan-out; screens/holdmenu.c (U1) implements
 * it. A full-screen ordinary list under the normal header, pushed with
 * UI_T_PUSH; Cancel or MENU pops. */

enum ui_hold_item { UI_HOLD_ADD_OTG, UI_HOLD_REMOVE_OTG, UI_HOLD_BROWSE_ALBUM,
                    UI_HOLD_BROWSE_ARTIST, UI_HOLD_REBUILD,
                    UI_HOLD_BUILD_ART, UI_HOLD_CANCEL, UI_HOLD_ITEMS };
enum ui_hold_ctx  { UI_HOLD_NP, UI_HOLD_SONG, UI_HOLD_GROUP, UI_HOLD_IN_OTG,
                    UI_HOLD_MUSIC };
struct ui_hold_arg {                     /* copied by enter() */
    enum ui_hold_ctx ctx;
    long tc_idx;                         /* 1-based, 0 = none (identity) */
    int  db_gen;                         /* validity of tc_idx */
    int  view, idx;                      /* the row held (lists) */
    int  otg_pos;                        /* UI_HOLD_IN_OTG */
    bool spoken;                         /* a book or podcast episode (track class
                                            or held view): no Browse Album/Artist */
};
/* Pure bitmask of (1u << enum ui_hold_item) (⚠️ H1): songs and Now
 * Playing get Add + Browse Album + Browse Artist + Cancel; group rows Add
 * + Cancel; rows inside On-The-Go Remove + Browse Album + Browse Artist +
 * Cancel. Drops BROWSE_* when spoken (Rev 1 L-6) or tc_idx is 0. */
unsigned ui_hold_items(const struct ui_hold_arg *a);
/* holdmenu.c enter() sets a swallow flag: the SELECT release of the hold
 * that opened it is dropped (Rev 1 L-6).
 * Trace: "hold open ctx=<c> items=<mask>", "hold act <item>". */
extern const struct ui_screen_ops ui_hold_screen;        /* arg: struct ui_hold_arg */
/* The nowplaying.c hook (Q applies the one call): hold SELECT with a
 * track opens the menu in the UI_HOLD_NP context. */
void ui_hold_open_np(long tc_idx, int db_gen);
/* Holding SELECT on Music in the main menu opens its maintenance menu. */
void ui_hold_open_music(void);
/* tagcache_ui_generation() for list callers filling db_gen (so they need
 * not include tagcache.h); -1 without a RAM cache. */
int  ui_hold_db_gen(void);
/* Pushes the menu (as ui_push(&ui_hold_screen, a)) with the header title
 * of the screen on top now: lists use this, ui_hold_open_np() too. */
void ui_hold_push(const struct ui_hold_arg *a);

#endif /* IPODUI_HOLDMENU_H */
