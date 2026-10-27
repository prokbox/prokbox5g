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

/* The SDK's resident side, seen by the core (not by games) */
#ifndef IPODUI_SDK_INT_H
#define IPODUI_SDK_INT_H

#include <stdbool.h>
#include <stdint.h>

struct ui_screen_ops;

/* sdk_table.c: the table the stubs jump through (sdk.sym order) */
extern const void *const sdk_table[];
extern const unsigned sdk_table_n;

/* sdk.c: the screen that runs one game; push arg (intptr_t)enum mod_id */
extern const struct ui_screen_ops sdk_game_screen;
/* sdk.c: the SDK's own Settings screen for the running game */
extern const struct ui_screen_ops sdk_settings_screen;
/* sdk_games.c: the resident Games list (Extras > Games) */
extern const struct ui_screen_ops sdk_games_screen;

/* ui_main.c's global holds: a long MENU on a Class A game is the game's
 * exit (Q29), not the jump to the main menu */
bool sdk_takes_long_menu(void);
/* input.c, SYS_POWEROFF: the running game's state record is written */
void sdk_poweroff(void);
/* modload.c's accountant: the running game's module when ops is its
 * screen (the game screen is resident, owned by no module), else -1 */
int sdk_screen_mod(const struct ui_screen_ops *ops);
/* usbscreen.c, after the ack (true) and at the session's end (false) */
void sdk_usb(bool on);
/* sleep.c, before a soft sleep's quiesce */
void sdk_sleep_enter(bool timer);
/* sleep.c's persist(), charging_sleep_prepare(): the record is written
 * with their saves (suspending the game first if it runs) */
void sdk_persist(void);

/* The games registry (sdk_games.c): our games, their names (firmware
 * language ids, rev 0) and static export tables in the simulator */
const void *const *sdk_games_static_exports(int mod, int *n);
/* The games file (section 8.5): plays and kills of a module; false if
 * the module is hidden by its kills */
bool sdk_game_offered(int mod);
void sdk_games_note(int mod, bool launched, bool ended, bool killed);
uint16_t sdk_game_plays(int mod);

#ifdef SIMULATOR
/* sim_cmd_extras.c's `sdk_bad <mode>`: writes the test game's record */
void sdk_sim_badmode(int mode);
/* `sdk_clear`: every game's .state and the games file go */
void sdk_sim_clear(void);
#endif

#endif /* IPODUI_SDK_INT_H */
