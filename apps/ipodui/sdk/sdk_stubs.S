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

/* The SDK modules' stub library (docs/design/sdk.md section 11.1),
 * linked into each game as libsdk.a (modules.make), never into the core.
 *
 * One stub per sdk.sym entry, each in its own section so --gc-sections
 * keeps only the ones a game calls (20 B each). A stub jumps through the
 * resident SDK table, whose address the SDK writes into sdk_tab_cell (the
 * game's export slot 2, in its .bss) before every call into the game:
 *
 *     ldr ip, =sdk_tab_cell   @ the cell: a module word, relocated
 *     ldr ip, [ip]            @ the table
 *     ldr ip, [ip, #4*index]  @ the entry: sdk.sym's line order
 *     bx  ip                  @ interworks (the core is ARM)
 *
 * ip (r12) is the AAPCS scratch register: arguments in r0-r3 and on the
 * stack pass through untouched, and lr still returns to the game. The
 * stubs are ARM code; the linker adds the Thumb-to-ARM veneers. */

    .syntax unified
    .set sdk_i, 0

    .macro SDKSTUB name
    .section .text.\name, "ax", %progbits
    .arm
    .align 2
    .global \name
    .type \name, %function
\name:
    ldr ip, 1f
    ldr ip, [ip]
    ldr ip, [ip, #(4 * sdk_i)]
    bx  ip
1:  .word sdk_tab_cell
    .size \name, . - \name
    .set sdk_i, sdk_i + 1
    .endm

#define SDK_SYM(n) SDKSTUB n
#include "ipodui/sdk/sdk.sym"
#undef SDK_SYM

    /* the table pointer: zeroed with .bss at every load, written by the
     * SDK before each call (sdk.c) */
    .section .bss.sdk_tab_cell, "aw", %nobits
    .align 2
    .global sdk_tab_cell
    .type sdk_tab_cell, %object
sdk_tab_cell:
    .space 4
    .size sdk_tab_cell, 4

    /* libgcc's division-by-zero hooks: return 0, so its division
     * helpers pull in nothing else (no raise(), no abort()) */
    .section .text.__aeabi_idiv0, "ax", %progbits
    .arm
    .align 2
    .weak __aeabi_idiv0
    .weak __aeabi_ldiv0
    .type __aeabi_idiv0, %function
    .type __aeabi_ldiv0, %function
__aeabi_idiv0:
__aeabi_ldiv0:
    mov r0, #0
    bx  lr
    .size __aeabi_idiv0, . - __aeabi_idiv0
