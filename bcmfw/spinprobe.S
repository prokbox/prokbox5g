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
 * spin()'s loop at sixteen alignments (device session 5): the same three
 * instructions spin() compiles to (add r1, #-1; cmp r1, #-1; bne, 8
 * bytes), each copy starting 0, 2, .. 30 bytes into a 64-byte block (so
 * some straddle a word, 8, 16 and 32-byte boundary), timed over 1000
 * iterations on the 1 MHz counter.  Session 5 saw spin(1000) take 38
 * ticks in the hello and bench images and 75 in hello_dma, with the
 * loop's encodings identical and only its address different; this
 * tells alignment (fetch) from a slower core.  timer.c logs the result.
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
    .text

/* uint32_t bcmfw_spa_K(void): ticks for 1000 iterations, loop at 2K */
    .global bcmfw_spa_0
bcmfw_spa_0:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_0
    .align 6
spa_loop_0:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_0
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_1
bcmfw_spa_1:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_1
    .align 6
    .space 2
spa_loop_1:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_1
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_2
bcmfw_spa_2:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_2
    .align 6
    .space 4
spa_loop_2:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_2
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_3
bcmfw_spa_3:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_3
    .align 6
    .space 6
spa_loop_3:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_3
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_4
bcmfw_spa_4:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_4
    .align 6
    .space 8
spa_loop_4:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_4
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_5
bcmfw_spa_5:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_5
    .align 6
    .space 10
spa_loop_5:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_5
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_6
bcmfw_spa_6:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_6
    .align 6
    .space 12
spa_loop_6:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_6
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_7
bcmfw_spa_7:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_7
    .align 6
    .space 14
spa_loop_7:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_7
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_8
bcmfw_spa_8:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_8
    .align 6
    .space 16
spa_loop_8:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_8
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_9
bcmfw_spa_9:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_9
    .align 6
    .space 18
spa_loop_9:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_9
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_10
bcmfw_spa_10:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_10
    .align 6
    .space 20
spa_loop_10:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_10
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_11
bcmfw_spa_11:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_11
    .align 6
    .space 22
spa_loop_11:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_11
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_12
bcmfw_spa_12:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_12
    .align 6
    .space 24
spa_loop_12:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_12
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_13
bcmfw_spa_13:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_13
    .align 6
    .space 26
spa_loop_13:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_13
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_14
bcmfw_spa_14:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_14
    .align 6
    .space 28
spa_loop_14:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_14
    ld r0, (r2)
    sub r0, r3
    rts

    .global bcmfw_spa_15
bcmfw_spa_15:
    mov r2, #0x10001804         /* TIMER_CLO */
    mov r1, #1000
    ld r3, (r2)
    b spa_loop_15
    .align 6
    .space 30
spa_loop_15:
    add r1, #-1
    cmp r1, #-1
    bne spa_loop_15
    ld r0, (r2)
    sub r0, r3
    rts
