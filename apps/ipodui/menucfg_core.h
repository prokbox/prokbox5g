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
#ifndef IPODUI_MENUCFG_CORE_H
#define IPODUI_MENUCFG_CORE_H

#include <stdbool.h>

/* The pure part of the Main Menu / Music Menu checklists (phase 11
 * brief sections 3.4, 4.5): masks in, rows out. No settings, no LCD, so
 * tests/menucfg compiles it on the host. The checklist shows the OFFERED
 * bits in canonical order = ascending bit number (the enums in
 * menucfg.h are in the 7G's fixed-slot order, main-menu-custom-hold.jpg),
 * then "Reset Menu". */

/* Number of offered rows. */
static inline int mc_count(unsigned offered)
{
    int n = 0;

    for (; offered; offered &= offered - 1)
        n++;
    return n;
}

/* The bit shown on row 'n' (0-based) of the offered rows, -1 past the
 * end. */
static inline int mc_nth_bit(unsigned offered, int n)
{
    int b;

    for (b = 0; b < 32; b++)
        if ((offered >> b) & 1u)
        {
            if (n == 0)
                return b;
            n--;
        }
    return -1;
}

/* The row of 'bit' among the offered rows, -1 if not offered. */
static inline int mc_row_of(unsigned offered, int bit)
{
    int b, n = 0;

    if (bit < 0 || bit >= 32 || !((offered >> bit) & 1u))
        return -1;
    for (b = 0; b < bit; b++)
        n += (offered >> b) & 1u;
    return n;
}

/* Whether the row shows a check: the mask has it, or it is the pinned
 * bit (Settings is never hidden). Only offered bits count. */
static inline bool mc_checked(unsigned mask, unsigned offered, int bit,
                              int pinned_bit)
{
    return bit == pinned_bit || (((mask & offered) >> bit) & 1u);
}

/* The mask after a toggle of 'bit'; unchanged for a bit that isn't
 * offered or for the pinned one (stock refuses to uncheck Settings). The
 * bits that are not offered (reserved: Cover Flow, Genius, Videos, ...)
 * are kept as they were. */
static inline unsigned mc_toggle(unsigned mask, unsigned offered, int bit,
                                 int pinned_bit)
{
    if (bit < 0 || bit >= 32 || !((offered >> bit) & 1u) || bit == pinned_bit)
        return mask;
    return mask ^ (1u << bit);
}

#endif /* IPODUI_MENUCFG_CORE_H */
