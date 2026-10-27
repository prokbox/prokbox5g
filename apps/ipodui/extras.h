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
 * iPod UI shell: the Extras registry (P12 contract, docs/phase12-brief.md 3.2)
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
#ifndef IPODUI_EXTRAS_H
#define IPODUI_EXTRAS_H

/* The Extras list in stock order (spec extras.md; photos 1191/1197),
 * and the same entries for the Main Menu items (menucfg.h UI_MM_*).
 * Owner W1-F (extras.c, screens/extras.c).
 *
 * M2 (rev 2): the Extras list and the Games list are RESIDENT screens
 * drawn by the shared list.c; their per-item panel art is pre-rendered
 * into panels.pak, so no list module is held in the arena under a child.
 * Each Extra's own screens live in its module (enum mod_id); in the
 * resident build (modules off) they link statically under the same
 * names. */

#include <stdint.h>

enum extra_id { XT_CLOCKS, XT_CALENDARS, XT_CONTACTS, XT_ALARMS, XT_GAMES,
                XT_NOTES, XT_SCREENLOCK, XT_STOPWATCH, XT__N };

struct ui_preview_ops;                  /* preview.h */

struct extra_ent {
    uint16_t title_lang;                /* LANG_IPODUI_X_* */
    uint8_t  module;                    /* enum mod_id (XT_GAMES: none, the
                                           Games list is resident; 0xff) */
    uint8_t  slot;                      /* export slot of its root screen */
    uint8_t  transition;                /* UI_T_REVEAL for Clocks, else
                                           UI_T_PUSH (screen_stack.h) */
    uint8_t  split;                     /* 1: split list (Calendars, Alarms,
                                           Stopwatch); 0: full width */
    const struct ui_preview_ops *panel; /* resident panel of its row, or
                                           NULL: a pre-rendered tile from
                                           panels.pak */
};
#define XT_NO_MODULE 0xff               /* a resident root (Games) */

extern const struct extra_ent extras_table[XT__N];

/* Push an Extra (from the Extras list or a Main Menu item). A module the
 * unit cannot run is never offered (mod_runnable()); one that fails to
 * load shows the styled "Not Available" page. Returns ui_push()'s
 * result. */
int extras_open(enum extra_id x);

/* -- W1-F additions (additive to the 3.2 contract) ----------------------- */
#include <stdbool.h>

/* Each Extra's package registers its module with ONE line in
 * apps/ipodui/extras_roots.h (merged by the orchestrator):
 *   XT_ROOT(XT_CLOCKS, clocks, &ui_preview_x_clocks)
 * naming modules/<name>_mod.c's export table (MOD_EXPORTS(<name>, n):
 * slot 0 its struct mod_desc (MOD_DESC_A), slot 1 its root screen) and
 * its row's resident panel (or NULL: the plain Extras panel until its
 * tile lands). In a modules build the core reaches the root through the
 * module (mod_stub_extra); otherwise through mod_exports_<name>. An Extra
 * without a line is a placeholder row (the empty stub screen). */

/* The row is offered: its module is runnable here (hidden otherwise,
 * logged "mod hide", never an error), or the Extra is a placeholder */
bool extras_available(enum extra_id x);
/* The module (enum mod_id) whose Extra has a screen on the stack, the
 * newest one; -1 when no Extra is open (the accountant's top) */
int extras_top_module(void);
/* An Extra of module mod is open (its root screen is on the stack) */
bool extras_module_live(int mod);
/* A static build's export table of module mod and its length (NULL:
 * none). modload.c, the simulator and resident builds. */
const void *const *extras_static_exports(int mod, int *n);
/* The row's panel: the package's (extras_roots.h), else the table's,
 * else ui_preview_x_plain */
const struct ui_preview_ops *extras_panel(enum extra_id x);
/* Intent preload on highlight (menu.c) */
void extras_preload(enum extra_id x);

/* The resident Extras list (screens/extras.c): a split menu in stock
 * order with each row's panel; menu.c's Extras row pushes it. Rows open
 * through extras_open(); the Main Menu's Extras items share the actions. */
struct ui_menu;
extern struct ui_menu ui_extras_menu;
/* Row actions (struct ui_menu_item.action): extras_open() of each Extra,
 * for the Extras list and the Main Menu's Extras items; and whether a
 * menu action is one of them (its row shows the chevron) */
void extras_go_clocks(void);
void extras_go_calendars(void);
void extras_go_contacts(void);
void extras_go_alarms(void);
void extras_go_games(void);
void extras_go_notes(void);
void extras_go_screenlock(void);
void extras_go_stopwatch(void);
/* the Extra an action opens, or -1 */
int extras_of_action(void (*fn)(void));
/* Calendars' row panel: the time, today's calendar page, the weekday
 * (photo 1191); the plain Extras panel for rows whose art is not in yet */
extern const struct ui_preview_ops ui_preview_x_calendars;
extern const struct ui_preview_ops ui_preview_x_plain;
/* The other rows' icons (screens/panels.c, extrasart.h; owner 2026-10-24):
 * resident, so a module's row shows its icon before the module loads.
 * Alarms' is also its own split list's panel. */
extern const struct ui_preview_ops ui_preview_x_alarms;
extern const struct ui_preview_ops ui_preview_x_contacts;
extern const struct ui_preview_ops ui_preview_x_games;
extern const struct ui_preview_ops ui_preview_x_notes;
extern const struct ui_preview_ops ui_preview_x_screenlock;
/* The Games list's panel (sdk_games.c): a game's cover, item_arg a const
 * unsigned char UI_COVER_* (NULL or UI_COVER_NONE: the Games icon). The
 * covers are the shell's own art for our games, keyed here (an SDK cover
 * asset for other games is a follow-up, docs/design/sdk.md). Keep the
 * order of extrasart.py's covers. */
enum ui_game_cover { UI_COVER_NONE, UI_COVER_CHUTE, UI_COVER_BRICKS,
                     UI_COVER_QUADRUS, UI_COVER_SOLITAIRE, UI_COVER_CHESS,
                     UI_COVER__N };
extern const struct ui_preview_ops ui_preview_x_gamecover;

#endif /* IPODUI_EXTRAS_H */
