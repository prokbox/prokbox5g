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
 * bcmfw pixel push: RGB565 pixels from SRAM to the LCD data register,
 * one zero-extended 16-bit value per 32-bit write (as Apple's code
 * writes panel registers).  Native VideoCore II assembly.
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

    .syntax vc2
    .text

/* void bcmfw_push(const uint16_t *src, uint32_t n, volatile uint32_t *port)
 * r0 src (halfword aligned), r1 pixel count, r2 port.  Uses r3-r5. */
    .global bcmfw_push
bcmfw_push:
    cmp r1, #0
    beq push_done
    btst r0, #1                 /* word-align the source first */
    beq push_aligned
    ldh r3, (r0)
    st r3, (r2)
    add r0, #2
    add r1, #-1
push_aligned:
    cmp r1, #4
    bcs push_tail               /* fewer than 4 left (unsigned lower) */
push_quad:                      /* 4 pixels, 11 instructions */
    ld r3, (r0)++
    ld r4, (r0)++
    lsr r5, r3, #16
    bmask r3, #16
    st r3, (r2)
    st r5, (r2)
    lsr r5, r4, #16
    bmask r4, #16
    st r4, (r2)
    st r5, (r2)
    addcmpbhi r1, #-4, #3, push_quad
push_tail:
    cmp r1, #2
    bcs push_last
    ld r3, (r0)++
    lsr r5, r3, #16
    bmask r3, #16
    st r3, (r2)
    st r5, (r2)
    add r1, #-2
push_last:
    cmp r1, #0
    beq push_done
    ldh r3, (r0)
    st r3, (r2)
push_done:
    rts
