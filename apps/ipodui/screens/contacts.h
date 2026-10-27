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

/* iPod UI shell: Extras > Contacts, the MOD_CONTACTS module's root screen
 * (P12 W3-K; screens/contacts.c) */
#ifndef IPODUI_SCREENS_CONTACTS_H
#define IPODUI_SCREENS_CONTACTS_H

/* Page loads a second the list may make while browsing (its manifest's
 * pages, contacts.mod; modpak.py check compares them): a fast spin through
 * the paged index, measured in the -windows scripts */
#define CONTACTS_PAGES 8

struct ui_screen_ops;
/* The Contacts list (the root), export slot MOD_SLOT_FIRST of MOD_CONTACTS */
extern const struct ui_screen_ops ui_x_contacts_screen;

#endif /* IPODUI_SCREENS_CONTACTS_H */
