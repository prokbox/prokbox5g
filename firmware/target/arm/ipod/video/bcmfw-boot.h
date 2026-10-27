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
 * The BCM2722 boot path's level 1 calls (#62, HAVE_BCMFW_L1; bcmfw-boot.c,
 * docs/research/bcm2722/independence.md).  The rest of the boot path's
 * API is in bcm-devtest.h.
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
#ifndef BCMFW_BOOT_H
#define BCMFW_BOOT_H

#ifdef HAVE_BCMFW_L1
#include <stdbool.h>

/* apps/main.c, right after the disk mounted: log the BCM's state at
 * lcd_init, then the swap (the shell's later bcm_boot_switch() is then a
 * no-op).  escape: the Developer escape is held, so the list draws on
 * Apple's vmcs and the shell's call swaps afterwards. */
void bcm_boot_early(bool escape);

/* The debug menu's failures for session 9 (independence.md section 5):
 * the next boot's DMA rung (the CPU-push rung takes over), every rung
 * (Apple's vmcs, marker kept), or the next wake's handshake (one rung
 * down at once; no strike) */
enum bcmfw_inject { BCMFW_INJECT_SWAP = 0, BCMFW_INJECT_SWAP_ALL = 1,
                    BCMFW_INJECT_WAKE = 2 };
void bcm_boot_inject(enum bcmfw_inject what);
#endif

#endif /* BCMFW_BOOT_H */
