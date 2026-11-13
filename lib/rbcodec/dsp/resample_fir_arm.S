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

/* ARM (ARMv4, ARM7TDMI) polyphase FIR kernels for resample_fir.h (#271).
 * Bit-exact with the C reference in resample_fir.c (tests/resample/arm.sh).
 * RS_FIR_TAPS is 10; both kernels are written out for it. */
#ifndef RESAMPLE_HB_STANDALONE
#include "config.h"
#endif

#if defined(RESAMPLE_HB_STANDALONE) || !defined(CPU_ARM_MICRO)

#ifdef RESAMPLE_HB_STANDALONE
.macro ldmpc cond="", order="ia", regs
    ldm\order\cond sp!, { \regs, pc }
.endm
#endif

    .syntax unified
    .arm
    .text

/* void rs_fir_batch(int32_t *dst, const int32_t *x, const uintptr_t *tag,
 *                   int n)
 * r0 dst, r1 x, r2 tag, r3 end of dst, r12 row, lr x walk,
 * r4-r6 samples, r7-r9 coefficients, r10:r11 the 64-bit sum. Per output:
 * 10 multiplies (SMULL, 9 SMLAL) and 8 loads, about 125 cycles. */
    .global rs_fir_batch
    .type   rs_fir_batch, %function
rs_fir_batch:
    stmfd   sp!, { r4-r11, lr }
    add     r3, r0, r3, lsl #2
1:
    ldr     r12, [r2], #4
    and     lr, r12, #3
    add     r1, r1, lr, lsl #2          @ x += tag & 3
    bic     r12, r12, #3                @ row
    mov     lr, r1
    ldmia   lr!, { r4-r6 }
    ldmia   r12!, { r7-r9 }
    smull   r10, r11, r4, r7
    smlal   r10, r11, r5, r8
    smlal   r10, r11, r6, r9
    ldmia   lr!, { r4-r6 }
    ldmia   r12!, { r7-r9 }
    smlal   r10, r11, r4, r7
    smlal   r10, r11, r5, r8
    smlal   r10, r11, r6, r9
    ldmia   lr!, { r4-r6 }
    ldmia   r12!, { r7-r9 }
    smlal   r10, r11, r4, r7
    smlal   r10, r11, r5, r8
    smlal   r10, r11, r6, r9
    ldr     r4, [lr]
    ldr     r7, [r12]
    smlal   r10, r11, r4, r7
    mov     r10, r10, lsr #31
    orr     r10, r10, r11, lsl #1       @ sum >> 31
    str     r10, [r0], #4
    cmp     r0, r3
    blo     1b
    ldmpc   regs=r4-r11
    .size   rs_fir_batch, .-rs_fir_batch

/* void rs_fir_row(int32_t *c, uint32_t frac)
 * c[i] = r0[i] + (((int64)(r1[i] - r0[i]) * eta) >> 31), r1 = r0 + 10,
 * computed as r0[i] + (hi << 1) + (lo >> 31) (the same sum, mod 2^32).
 * r0 c, r1 eta, r2 row j; five taps at a time: r4-r8 from row j,
 * r9-r12 and lr from row j + 1 (addressed through r3, which then serves as
 * the low word of the products). The multiplier's Rs is the difference
 * between neighbouring rows, which is small, so the SMULLs run short on the
 * ARM7TDMI's early-terminating multiplier. */
.macro LERP a, b
    sub     \b, \b, \a
    smull   r3, \b, r1, \b
    add     \a, \a, \b, lsl #1
    add     \a, \a, r3, lsr #31
.endm
.macro ROW5
    add     r3, r2, #40                 @ the same taps of row j + 1
    ldmia   r2!, { r4-r8 }
    ldmia   r3, { r9-r12, lr }
    LERP    r4, r9
    LERP    r5, r10
    LERP    r6, r11
    LERP    r7, r12
    LERP    r8, lr
    stmia   r0!, { r4-r8 }
.endm
    .global rs_fir_row
    .type   rs_fir_row, %function
rs_fir_row:
    stmfd   sp!, { r4-r11, lr }
    ldr     r2, =rs_fir_proto
    mov     r3, r1, lsr #26             @ j
    add     r3, r3, r3, lsl #2          @ 5j
    add     r2, r2, r3, lsl #3          @ proto + 40j bytes
    mov     r1, r1, lsl #6
    mov     r1, r1, lsr #1              @ eta, Q31
    ROW5
    ROW5
    ldmpc   regs=r4-r11
    .size   rs_fir_row, .-rs_fir_row
    .ltorg

#endif
