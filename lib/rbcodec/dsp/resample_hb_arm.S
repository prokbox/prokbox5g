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

/* ARM (ARMv4, ARM7TDMI) half-band stages for resample_hb.h (P19 19-13).
 * Bit-exact with the C reference in resample_hb.c (tests/resample).
 *
 * Each chain runs over the whole block with its states (r3-r7) and
 * coefficients (r8-r11) in registers; chain 0 then chain 1. A section is
 *     sub   r2, in, s[k+1]          @ d = x - last output
 *     smull r2, out, a[k], r2       @ out = (a * d) >> 32
 *     add   out, s[k], out, lsl #1  @ y = last input + 2 * out
 *     mov   s[k], in                @ last input = x
 * about 9 cycles on an ARM7TDMI (SMULL 3-6 by the size of d). The last
 * section writes its output straight into the chain's last state. */
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

.macro SEC in, out, sk, sk1, ak
    sub     r2, \in, \sk1
    smull   r2, \out, \ak, r2
    add     \out, \sk, \out, lsl #1
    mov     \sk, \in
.endm

@ One sample through k sections: input r12, result in s[k] (r4..r7)
.macro CHAIN k
  .if \k == 1
    SEC     r12, r4, r3, r4, r8
  .else
    SEC     r12, lr, r3, r4, r8
  .endif
  .if \k == 2
    SEC     lr, r5, r4, r5, r9
  .elseif \k > 2
    SEC     lr, r12, r4, r5, r9
  .endif
  .if \k == 3
    SEC     r12, r6, r5, r6, r10
  .elseif \k > 3
    SEC     r12, lr, r5, r6, r10
  .endif
  .if \k == 4
    SEC     lr, r7, r6, r7, r11
  .endif
.endm

@ Frame (sp): 0 dst, 4 src, 8 st, 12 end of dst, 16 return to, then the
@ saved r4-r11, lr, then the caller's 5th argument (c) at 56.
@ mode 0: x2 chain (read +4, write +8); 1: /2 chain 0 (read +8, write +4);
@ mode 2: /2 chain 1 (read +8, dst = (dst + y) >> 1, +4)
.macro LOOP k, mode, res
1:
  .if \mode == 0
    ldr     r12, [r1], #4
  .else
    ldr     r12, [r1], #8
  .endif
    CHAIN   \k
  .if \mode == 0
    str     \res, [r0], #8
  .elseif \mode == 1
    str     \res, [r0], #4
  .else
    ldr     r2, [r0]
    add     r2, r2, \res
    mov     r2, r2, asr #1
    str     r2, [r0], #4
  .endif
    ldr     r2, [sp, #12]
    cmp     r0, r2
    blo     1b
    ldr     pc, [sp, #16]
.endm

@ Jump to loop <base><k> for k in r2 (1..4)
.macro DISPATCH base
    add     pc, pc, r2, lsl #2
    nop
    nop
    b       \base\()1
    b       \base\()2
    b       \base\()3
    b       \base\()4
.endm

/* void hb_up2(int32_t *dst, const int32_t *src, int n, int32_t *st,
 *             const struct hb_coefs *c) */
    .global hb_up2
    .type   hb_up2, %function
hb_up2:
    stmfd   sp!, { r4-r11, lr }
    sub     sp, sp, #20
    add     r12, r0, r2, lsl #3         @ end = dst + 2n samples
    stmia   sp, { r0, r1, r3, r12 }     @ dst, src, st, end
    adr     r2, .Lup_chain1
    str     r2, [sp, #16]
    ldr     r12, [sp, #56]              @ c
    ldmia   r12, { r8-r11 }             @ chain 0 coefficients
    ldr     r2, [r12, #32]              @ k0
    ldmia   r3, { r3-r7 }               @ chain 0 states
    DISPATCH .Lup
.Lup_chain1:
    ldr     r2, [sp, #8]
    stmia   r2, { r3-r7 }               @ save chain 0 states
    ldmia   sp, { r0, r1 }              @ dst, src
    ldr     r2, [sp, #12]
    add     r0, r0, #4                  @ odd outputs
    add     r2, r2, #4
    str     r2, [sp, #12]
    adr     r2, .Lup_done
    str     r2, [sp, #16]
    ldr     r12, [sp, #56]
    add     r2, r12, #16
    ldmia   r2, { r8-r11 }              @ chain 1 coefficients
    ldr     r2, [r12, #36]              @ k1
    ldr     r3, [sp, #8]
    add     r3, r3, #20
    ldmia   r3, { r3-r7 }               @ chain 1 states
    DISPATCH .Lup
.Lup_done:
    ldr     r2, [sp, #8]
    add     r2, r2, #20
    stmia   r2, { r3-r7 }
    add     sp, sp, #20
    ldmpc   regs=r4-r11
.Lup1:  LOOP 1, 0, r4
.Lup2:  LOOP 2, 0, r5
.Lup3:  LOOP 3, 0, r6
.Lup4:  LOOP 4, 0, r7
    .size   hb_up2, .-hb_up2

/* void hb_down2(int32_t *dst, const int32_t *src, int n, int32_t *st,
 *               const struct hb_coefs *c) */
    .global hb_down2
    .type   hb_down2, %function
hb_down2:
    stmfd   sp!, { r4-r11, lr }
    sub     sp, sp, #20
    add     r12, r0, r2, lsl #2         @ end = dst + n
    stmia   sp, { r0, r1, r3, r12 }     @ dst, src, st, end
    adr     r2, .Ldn_chain1
    str     r2, [sp, #16]
    add     r1, r1, #4                  @ chain 0 takes the newer sample
    ldr     r12, [sp, #56]
    ldmia   r12, { r8-r11 }
    ldr     r2, [r12, #32]
    ldmia   r3, { r3-r7 }
    DISPATCH .Ldn0_
.Ldn_chain1:
    ldr     r2, [sp, #8]
    stmia   r2, { r3-r7 }
    ldmia   sp, { r0, r1 }              @ dst, src (older samples)
    adr     r2, .Ldn_done
    str     r2, [sp, #16]
    ldr     r12, [sp, #56]
    add     r2, r12, #16
    ldmia   r2, { r8-r11 }
    ldr     r2, [r12, #36]
    ldr     r3, [sp, #8]
    add     r3, r3, #20
    ldmia   r3, { r3-r7 }
    DISPATCH .Ldn1_
.Ldn_done:
    ldr     r2, [sp, #8]
    add     r2, r2, #20
    stmia   r2, { r3-r7 }
    add     sp, sp, #20
    ldmpc   regs=r4-r11
.Ldn0_1: LOOP 1, 1, r4
.Ldn0_2: LOOP 2, 1, r5
.Ldn0_3: LOOP 3, 1, r6
.Ldn0_4: LOOP 4, 1, r7
.Ldn1_1: LOOP 1, 2, r4
.Ldn1_2: LOOP 2, 2, r5
.Ldn1_3: LOOP 3, 2, r6
.Ldn1_4: LOOP 4, 2, r7
    .size   hb_down2, .-hb_down2

#endif
