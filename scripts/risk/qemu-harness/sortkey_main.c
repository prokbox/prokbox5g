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

/* Bare-metal ARM driver for apps/ipodui/sortkey.c under qemu-system-arm
 * + the instruction-count plugin (risk tool 1). Representative names
 * (same average length / HASH_GROUP rate as the host driver's measured
 * distribution: scripts/risk/drivers/sortkey_drv.c, ~11.8 chars, ~2.9%
 * HASH_GROUP) as literals, so no RNG/libc is needed here -- the real,
 * unmodified ui_sortkey() from apps/ipodui/sortkey.c is the only thing
 * under test; this file just repeats a representative sample of calls.
 */
#include "ipodui/sortkey.h"

static const char *const NAMES[] = {
    "Golden Horizon", "Dawn", "Silver Maple Drift", "Crimson Tide Harbour",
    "Echo Of The Forest", "Iron Garnet", "Pale Winter Signal", "Juniper",
    "Comet Glacier Bloom", "Hazel Ridge", "Opal Summit", "Faded Lantern",
    "Spruce And Ivory", "North Of The Island", "Velvet Oasis",
    "Quartz Meadow Drift", "Raven Copper Falcon", "Topaz Mirror",
    "The Bellwethers", "Kestrel Nectar", "1999 Echo Signal", "Zero Point",
    "Sable River", "Marble Dune", "Zephyr Of Autumn", "Granite Harbour",
    "Pine And Cedar", "Solar Wind Garnet", "Yellow Birch", "Misty Onyx",
    "Fjord Of Glass", "Noble Pebble", "Slate Horizon", "Willow Nectar",
    "Timber Orbit", "Upper Basalt", "Olive And Indigo", "Frost Line",
    "Amber Signal Drift", "42",
};
#define NNAMES ((int)(sizeof(NAMES) / sizeof(NAMES[0])))

/* How many ui_sortkey() calls make up one qemu run: pick enough that the
 * plugin's fixed per-run overhead (a few hundred instructions of boot
 * code) is negligible, but few enough that TCG-with-callback stays fast.
 * scripts/risk/arm_insn_count.sh scales the result by
 * (workload n) / CALLS to get the instruction total at library scale. */
#define CALLS 2000

volatile int sink;   /* keeps the calls from being optimized away */

int main(void)
{
    char key[32];
    int i;

    for (i = 0; i < CALLS; i++)
        sink += ui_sortkey(NAMES[i % NNAMES], key, sizeof key);
    return 0;
}
