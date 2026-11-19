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
 * iPod UI shell: the custom EQ editor (#82)
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
#ifndef IPODUI_EQEDIT_H
#define IPODUI_EQEDIT_H

struct ui_screen_ops;

/* The editor for one custom slot (arg: the slot 0..2 as a pointer-sized
 * integer) */
extern const struct ui_screen_ops ui_eqedit_screen;
/* Extras > Custom EQ: the list of the 3 slots, each opening the editor */
extern const struct ui_screen_ops ui_eqslots_screen;
/* pushes the editor for slot 0..2; ui_push()'s result */
int ui_eqedit_open(int slot);

#endif
