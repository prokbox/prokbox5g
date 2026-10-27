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
 * bcmfw timing bench (BCMFW_BENCH builds): on-chip checks of the scalar
 * instruction kinds the H.264 decoder's C code executes and Apple's code
 * never does in the vc2sim self-test (decoder.md, "Device checks", item
 * 4), beyond the probe's ten (probe.S): max with an immediate, asr rd, rs,
 * the 32-bit bgt/blt/ble/bls, and ldsb at (rs), (rs+ri) and (rs+off).  Each check
 * sets one bit; the step word names the one running.  Registers only,
 * plus reads of one data block: no device access.
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
#if BCMFW_BENCH
    .text

/* uint32_t bench_scalar_check(void): r0 = pass bits.  Uses r0-r5. */
    .global bench_scalar_check
bench_scalar_check:
    mov r5, #0
    mov r2, #BCMFW_MB_PROBE_STEP

/* 0: max rd, rs, #u5 (signed compare, positive immediate): the 32-bit
 * three-operand form vc2cc emits for max #imm.  There is no 16-bit
 * max #imm5: 7000-77ff is ld/st (--rs) (isa.md). */
    mov r3, #(BCMFW_BENCH_STEP_SCALAR + 0)
    st r3, (r2)
    mov r0, #-5
    max r0, r0, #3
    mov r1, #3
    cmp r0, r1
    bne s1
    mov r0, #9
    max r0, r0, #3
    mov r1, #9
    cmp r0, r1
    bne s1
    mov r1, #1
    or r5, r1

/* 1: asr rd, rs */
s1:
    mov r3, #(BCMFW_BENCH_STEP_SCALAR + 1)
    st r3, (r2)
    mov r0, #-256
    mov r1, #4
    asr r0, r1
    mov r1, #-16
    cmp r0, r1
    bne s2
    mov r0, #0x40000000
    mov r1, #30
    asr r0, r1
    mov r1, #1
    cmp r0, r1
    bne s2
    mov r0, #0x80000000
    mov r1, #31
    asr r0, r1
    mov r1, #-1
    cmp r0, r1
    bne s2
    mov r1, #2
    or r5, r1

/* 2: 32-bit bgt (signed greater): not taken, then taken.  The targets
 * sit past 160 bytes of padding, out of the 16-bit form's reach. */
s2:
    mov r3, #(BCMFW_BENCH_STEP_SCALAR + 2)
    st r3, (r2)
    mov r0, #-1
    mov r1, #3
    cmp r0, r1
    bgt s3
    cmp r1, r0
    bgt s2_ok
    b s3
    .rept 80
    nop
    .endr
s2_ok:
    mov r1, #4
    or r5, r1

/* 3: 32-bit blt (signed less): not taken, then taken */
s3:
    mov r3, #(BCMFW_BENCH_STEP_SCALAR + 3)
    st r3, (r2)
    mov r0, #-1
    mov r1, #3
    cmp r1, r0
    blt s4
    cmp r0, r1
    blt s3_ok
    b s4
    .rept 80
    nop
    .endr
s3_ok:
    mov r1, #8
    or r5, r1

/* 4: 32-bit ble: not taken (greater), taken (equal), taken (less) */
s4:
    mov r3, #(BCMFW_BENCH_STEP_SCALAR + 4)
    st r3, (r2)
    mov r0, #-1
    mov r1, #3
    cmp r1, r0
    ble s5
    cmp r1, r1
    ble s4_eq
    b s5
    .rept 80
    nop
    .endr
s4_eq:
    cmp r0, r1
    ble s4_ok
    b s5
    .rept 80
    nop
    .endr
s4_ok:
    mov r1, #16
    or r5, r1

/* 5: 32-bit bls (unsigned lower or same): not taken (0xFFFFFFFF vs 3),
 * taken (3 vs 0xFFFFFFFF) */
s5:
    mov r3, #(BCMFW_BENCH_STEP_SCALAR + 5)
    st r3, (r2)
    mov r0, #-1
    mov r1, #3
    cmp r0, r1
    bls s6
    cmp r1, r0
    bls s5_ok
    b s6
    .rept 80
    nop
    .endr
s5_ok:
    mov r1, #32
    or r5, r1

/* 6: ldsb rd, (rs): sign-extended bytes */
s6:
    mov r3, #(BCMFW_BENCH_STEP_SCALAR + 6)
    st r3, (r2)
    mov r4, #bench_sbytes
    ldsb r0, (r4)
    mov r1, #-128
    cmp r0, r1
    bne s7
    add r4, #1
    ldsb r0, (r4)
    mov r1, #127
    cmp r0, r1
    bne s7
    mov r1, #64
    or r5, r1

/* 7: ldsb rd, (rs+ri) */
s7:
    mov r3, #(BCMFW_BENCH_STEP_SCALAR + 7)
    st r3, (r2)
    mov r4, #bench_sbytes
    mov r1, #2
    ldsb r0, (r4+r1)
    mov r1, #-1
    cmp r0, r1
    bne s8
    mov r1, #3
    ldsb r0, (r4+r1)
    mov r1, #1
    cmp r0, r1
    bne s8
    mov r1, #128
    or r5, r1

/* 8: ldsb rd, (rs+off) (48-bit form) */
s8:
    mov r3, #(BCMFW_BENCH_STEP_SCALAR + 8)
    st r3, (r2)
    mov r4, #bench_sbytes
    ldsb r0, (r4+100)
    mov r1, #-2
    cmp r0, r1
    bne s_done
    mov r1, #256
    or r5, r1

s_done:
    mov r0, r5
    rts

    .data
bench_sbytes:
    .byte 0x80, 0x7F, 0xFF, 0x01
    .fill 96, 1, 0
    .byte 0xFE, 0, 0, 0
#endif
