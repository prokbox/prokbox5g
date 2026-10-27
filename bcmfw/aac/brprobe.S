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
 * The 32-bit bcs/bcc probe (M4, audio-spike.md section 7): the two branch
 * kinds the AAC decoder executes that neither Apple's code in the vc2sim
 * self-test nor an earlier device check has run (cmp ra, rb, then a far
 * bcs or bcc: fft-ffmpeg.c's pass() loop bound, huffman.c's bounds).
 * Each case runs both, taken or not; the targets sit past 160 bytes of
 * padding, out of the 16-bit form's reach (as bench_scalar.S does for
 * bgt/blt/ble/bls).  Registers only, plus reads of the case table.
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

    .syntax vc2
#if BCMFW_AAC
    .text

/* uint32_t aac_brprobe(const uint32_t (*cases)[2], uint32_t n): bit k set
 * when case k's bcs was taken, bit 16 + k when its bcc was (n <= 16).
 * Uses r0-r5. */
    .global aac_brprobe
aac_brprobe:
    mov r4, r0                  /* the case */
    mov r5, r1                  /* cases left */
    mov r2, #0                  /* taken bits */
    mov r3, #1                  /* this case's bit */
bp_loop:
    ld r0, (r4)
    ld r1, (r4+4)
    cmp r0, r1
    bcs bp_cs
    b bp_cs_done
    .rept 80
    nop
    .endr
bp_cs:
    or r2, r3
bp_cs_done:
    cmp r0, r1
    bcc bp_cc
    b bp_next
    .rept 80
    nop
    .endr
bp_cc:
    mov r1, r3
    lsl r1, #16
    or r2, r1
bp_next:
    lsl r3, #1
    add r4, #8
    add r5, #-1
    cmp r5, #0
    bne bp_loop
    mov r0, r2
    rts
#endif
