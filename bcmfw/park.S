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

#include "config.h"

#if BCMFW_HOSTPROTO
    .syntax vc2

/* The warm swap's park stub (HP_OP_PARK; jobs.md G9, bench row B13).
 * hostproto.c copies it to BCMFW_PARK_STUB, outside every image, and
 * calls it with r0 = BCMFW_WARM_PARKED | flags, interrupts off and no
 * transfer running.  It says it is parked, then spins until the host
 * (having uploaded the next image over the old one) writes
 * BCMFW_WARM_GO, and jumps to that image's entry.  Position-independent:
 * absolute constants and relative branches only.  Nothing here touches
 * a GPIO, so the panel's reset line (GPIO 48) holds its level. */
    .text
    .global bcmfw_park_stub
    .global bcmfw_park_stub_end
    .align 2                    /* copied a word at a time: 4-aligned */
bcmfw_park_stub:
    mov r1, #BCMFW_MB_WARM
    st r0, (r1)                 /* PARKED | flags: the host may upload */
    mov r2, #(BCMFW_WARM_GO >> 8)
park_spin:
    ld r0, (r1)
    mov r3, r0
    lsr r3, #8
    cmp r3, r2
    bne park_spin
    mov r1, #BCMFW_ENTRY        /* the new image, with GO | flags left */
    bl r1                       /* in the word for it (never returns) */
park_hang:
    b park_hang
bcmfw_park_stub_end:
#endif
