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
 * section writes its output straight into the chain's last state.
 * Loops run two samples per pass (entered half-way for an odd count); a
 * chain of up to 3 sections keeps the end pointer in r11. The relaxed
 * 2+1-section design (hb_r) runs both chains in one pass. */
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

@ One sample: load, chain, store.
@ mode 0: x2 chain (read +4, write +8); 1: /2 chain 0 (read +8, write +4);
@ mode 2: /2 chain 1 (read +8, dst = (dst + y) >> 1, +4)
.macro BODY k, mode, res
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
.endm

@ Frame (sp): 0 dst, 4 src, 8 st, 12 end of dst, 16 return to, then the
@ saved r4-r11, lr, then the caller's 5th argument (c) at 56.
@ Two samples per pass; an odd count enters at the second. A chain of up
@ to 3 sections has r11 free for the end pointer.
.macro LOOP k, mode, res
  .if \k < 4
    ldr     r11, [sp, #12]
    sub     r2, r11, r0
  .else
    ldr     r2, [sp, #12]
    sub     r2, r2, r0
  .endif
  .if \mode == 0
    tst     r2, #8
  .else
    tst     r2, #4
  .endif
    bne     2f
1:
    BODY    \k, \mode, \res
2:
    BODY    \k, \mode, \res
  .if \k < 4
    cmp     r0, r11
  .else
    ldr     r2, [sp, #12]
    cmp     r0, r2
  .endif
    blo     1b
    ldr     pc, [sp, #16]
.endm

@ Branch to \label when c (r12) has k0 = 2, k1 = 1 (hb_r)
.macro IF_21 label
    ldr     r2, [r12, #32]
    ldr     lr, [r12, #36]
    cmp     r2, #2
    cmpeq   lr, #1
    beq     \label
.endm

@ 2+1 sections, both chains in one pass: chain 0 states r3-r5,
@ coefficients r8, r9; chain 1 states r6, r7, coefficient r10; end in r11.
@ r3 = st, r12 = c on entry; leaves end - dst in r2.
.macro LOAD_21
    ldr     r8, [r12]
    ldr     r9, [r12, #4]
    ldr     r10, [r12, #16]
    ldr     r6, [r3, #20]
    ldr     r7, [r3, #24]
    ldmia   r3, { r3-r5 }
    ldr     r11, [sp, #12]
    sub     r2, r11, r0
.endm

.macro SAVE_21
    ldr     r2, [sp, #8]
    stmia   r2, { r3-r5 }
    str     r6, [r2, #20]
    str     r7, [r2, #24]
    add     sp, sp, #20
    ldmpc   regs=r4-r11
.endm

@ x2, 2+1 sections: x feeds both chains; outputs stored as a pair
.macro UP21
    ldr     r12, [r1], #4
    SEC     r12, lr, r3, r4, r8
    SEC     lr, r5, r4, r5, r9
    SEC     r12, r7, r6, r7, r10
    stmia   r0!, { r5, r7 }
.endm

@ /2, 2+1 sections: older sample (r12) to chain 1, newer (lr) to chain 0
.macro DN21
    ldmia   r1!, { r12, lr }
    SEC     r12, r7, r6, r7, r10
    SEC     lr, r12, r3, r4, r8
    SEC     r12, r5, r4, r5, r9
    add     r2, r5, r7
    mov     r2, r2, asr #1
    str     r2, [r0], #4
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
    ldr     r12, [sp, #56]              @ c
    IF_21   .Lup_21
    adr     r2, .Lup_chain1
    str     r2, [sp, #16]
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
.Lup_21:
    LOAD_21
    tst     r2, #8
    bne     2f
1:  UP21
2:  UP21
    cmp     r0, r11
    blo     1b
    SAVE_21
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
    ldr     r12, [sp, #56]
    IF_21   .Ldn_21
    adr     r2, .Ldn_chain1
    str     r2, [sp, #16]
    add     r1, r1, #4                  @ chain 0 takes the newer sample
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
.Ldn_21:
    LOAD_21
    tst     r2, #4
    bne     2f
1:  DN21
2:  DN21
    cmp     r0, r11
    blo     1b
    SAVE_21
    .size   hb_down2, .-hb_down2

#endif
