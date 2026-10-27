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
 * bcmfw probe v2 (BCMFW_PROBE builds only): an on-chip check of
 * instruction forms that compiled C uses and Apple's own code never
 * exercises in the vc2sim self-test (docs/research/bcm2722/compiler.md),
 * plus informational checks of the exception model.  Checks 0-8 are
 * required: their bits make the result the host compares with
 * BCMFW_SELFCHECK_ALL.  Checks 9-12 are informational: their outcome is
 * only in the results block (bringup.md, "Probe v2").  Before each check
 * the step word says which one runs; a fault inside a check is recorded
 * by the exception handler (crt0.S), which resumes at the next check.
 * Registers and a few guarded data words: no device access.
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
#if BCMFW_PROBE
    .text

/* uint32_t bcmfw_selfcheck(void): r0 = the required checks' pass bits.
 * Uses r0-r5.  Every check is self-contained (the handler clobbers r0-r5
 * and resumes at the next check's label in bcmfw_probe_resume with sp as
 * it was here), so pass bits live in the results block, not a register. */
    .global bcmfw_selfcheck
bcmfw_selfcheck:
    mov r0, lr
    st r0, bcmfw_probe_lr
    mov r0, sp
    st r0, bcmfw_probe_sp

/* 0: abs */
c0:
    mov r0, #0
    bl probe_begin
    mov r0, #-5
    abs r0, r0
    mov r1, #5
    cmp r0, r1
    bne c0_fail
    bl probe_pass
    b c1
c0_fail:
    bl probe_fail

/* 1: not */
c1:
    mov r0, #1
    bl probe_begin
    mov r0, #0x0F0F0000
    not r0, r0
    mov r1, #0xF0F0FFFF
    cmp r0, r1
    bne c1_fail
    bl probe_pass
    b c2
c1_fail:
    bl probe_fail

/* 2: mulhd.ss (signed high word of the product), two cases */
c2:
    mov r0, #2
    bl probe_begin
    mov r0, #-3
    mov r1, #0x40000000
    mulhd.ss r1, r0, r1
    mov r0, #-1
    cmp r0, r1
    bne c2_fail
    mov r0, #0x12345678
    mov r1, #0x7FFFFFFF
    mulhd.ss r1, r0, r1
    mov r0, #0x91A2B3B
    cmp r0, r1
    bne c2_fail
    bl probe_pass
    b c3
c2_fail:
    bl probe_fail

/* 3: three-operand eor */
c3:
    mov r0, #3
    bl probe_begin
    mov r0, #0xFF00FF00
    mov r1, #0x0FF00FF0
    eor r4, r0, r1
    mov r0, #0xF0F0F0F0
    cmp r0, r4
    bne c3_fail
    bl probe_pass
    b c4
c3_fail:
    bl probe_fail

/* 4: addscale6 (a + b * 64) */
c4:
    mov r0, #4
    bl probe_begin
    mov r0, #10
    mov r1, #3
    addscale6 r0, r0, r1
    mov r1, #202
    cmp r0, r1
    bne c4_fail
    bl probe_pass
    b c5
c4_fail:
    bl probe_fail

/* 5: post-increment ldh: zero-extended, pointer + 2 */
c5:
    mov r0, #5
    bl probe_begin
    mov r0, #probe_halves
    ldh r3, (r0)++
    ldh r4, (r0)++
    mov r1, #0x8001
    cmp r3, r1
    bne c5_fail
    mov r1, #0x7FFE
    cmp r4, r1
    bne c5_fail
    mov r1, #probe_halves+4
    cmp r0, r1
    bne c5_fail
    bl probe_pass
    b c6
c5_fail:
    bl probe_fail

/* 6: 48-bit pc-relative st and ld (every global access in compiled C).
 * The word sits between guard words, so a wrong base lands in a guard. */
c6:
    mov r0, #6
    bl probe_begin
    mov r0, #0xA5C31234
    st r0, probe_word
    ld r1, probe_word
    cmp r0, r1
    bne c6_fail
    mov r3, #probe_word
    ld r4, (r3)
    cmp r0, r4
    bne c6_fail
    bl probe_pass
    b c7
c6_fail:
    bl probe_fail

/* 7: 16-bit bhi (unsigned higher), not taken then taken */
c7:
    mov r0, #7
    bl probe_begin
    mov r0, #-1
    mov r1, #3
    cmp r1, r0
    bhi c7_fail
    cmp r0, r1
    bhi c7_ok
    b c7_fail
c7_ok:
    bl probe_pass
    b c8
c7_fail:
    bl probe_fail

/* 8: 32-bit bhi (targets out of 16-bit range), not taken then taken */
c8:
    mov r0, #8
    bl probe_begin
    mov r0, #-1
    mov r1, #3
    cmp r1, r0
    bhi c8_fail
    cmp r0, r1
    bhi c8_ok
    b c8_fail
    .rept 160
    nop
    .endr
c8_ok:
    bl probe_pass
    b c9
c8_fail:
    bl probe_fail

/* 9 (informational): device session 1 replayed.  The old check 7's
 * 'cmp r0, #5' was 0x7050 = ld r0, (--r5), run with r5 = the pass bits
 * (0x7F): a misaligned load.  Expected to fault: the record gives the
 * vector, the saved pc (detail = the instruction's address) and r5 (was
 * it decremented?).  No fault: pass, detail2 = r5 after. */
c9:
    mov r0, #9
    bl probe_begin
    mov r1, #c9_insn
    mov r2, #(BCMFW_PROBE_REC(9) + 40)
    st r1, (r2)
    nop
    nop
    nop
    nop
    mov r0, #5
    mov r5, #0x7F
c9_insn:
    .hword 0x7050               /* ld r0, (--r5) */
    nop
    nop
    nop
    nop
    mov r2, #(BCMFW_PROBE_REC(9) + 44)
    st r5, (r2)
    bl probe_pass
    b c10

/* 10 (informational): the same encoding, aligned.  r5 = &buf + 8: pass
 * if r5 becomes &buf + 4 and r0 = buf[1].  detail = r0, detail2 = r5. */
c10:
    mov r0, #10
    bl probe_begin
    mov r0, #0
    mov r5, #probe_buf + 8
    .hword 0x7050               /* ld r0, (--r5) */
    mov r2, #(BCMFW_PROBE_REC(10) + 40)
    st r0, (r2)
    st r5, (r2+4)
    mov r1, #probe_buf + 4
    cmp r5, r1
    bne c10_fail
    mov r1, #probe_buf
    ld r1, (r1+4)
    cmp r0, r1
    bne c10_fail
    bl probe_pass
    b c11
c10_fail:
    bl probe_fail

/* 11 (informational): pre-decrement ldb/stb, the forms Apple's memmove
 * uses: copy two bytes backwards.  Pass if the bytes and both pointers
 * are right.  detail = the copied halfword, detail2 = (src pointer -
 * src) | (dst pointer - dst) << 16 (0 when right). */
c11:
    mov r0, #11
    bl probe_begin
    mov r1, #probe_src + 2
    mov r2, #probe_dst + 2
    ldb r0, (--r1)
    stb r0, (--r2)
    ldb r0, (--r1)
    stb r0, (--r2)
    mov r3, #probe_dst
    ldh r4, (r3)
    mov r5, #(BCMFW_PROBE_REC(11) + 40)
    st r4, (r5)
    mov r0, #probe_src
    mov r3, r1
    sub r3, r0
    mov r0, #probe_dst
    mov r5, r2
    sub r5, r0
    lsl r5, #16
    or r3, r5
    mov r5, #(BCMFW_PROBE_REC(11) + 44)
    st r3, (r5)
    cmp r3, #0
    bne c11_fail
    mov r5, #0xC35A
    cmp r4, r5
    bne c11_fail
    bl probe_pass
    b c12
c11_fail:
    bl probe_fail

/* 12 (informational): an unaligned word load with the form Apple's code
 * uses everywhere, ld rd, (rs), followed by four 32-bit instructions with
 * distinct results.  Expected to fault: the saved pc against detail (the
 * load's address) says whether the offset counts bytes or instructions,
 * r2-r5 whether any later instruction ran first, and the status the
 * misaligned vector.  No fault: pass, detail2 = r0 (what was loaded). */
c12:
    mov r0, #12
    bl probe_begin
    mov r1, #c12_insn
    mov r2, #(BCMFW_PROBE_REC(12) + 40)
    st r1, (r2)
    mov r1, #probe_buf + 1
    mov r0, #0
    mov r2, #0
    mov r3, #0
    mov r4, #0
    mov r5, #0
c12_insn:
    ld r0, (r1)
    add r2, r2, #0x111
    add r3, r3, #0x222
    add r4, r4, #0x333
    add r5, r5, #0x444
    mov r2, #(BCMFW_PROBE_REC(12) + 44)
    st r0, (r2)
    bl probe_pass

c_done:
    mov r0, #-1
    st r0, bcmfw_probe_cur
    ld r0, bcmfw_probe_lr
    mov lr, r0
    mov r1, #(BCMFW_PROBE_RESULTS + 16)
    ld r0, (r1)
    mov r1, #BCMFW_PROBE_REQUIRED
    and r0, r1
    rts

/* probe_begin: r0 = check n.  Step word = CHECKING + n << 8, then the
 * handler's current check = n.  Uses r0-r2. */
probe_begin:
    mov r1, r0
    lsl r1, #8
    mov r2, #BCMFW_PROBE_CHECKING
    add r1, r2
    mov r2, #BCMFW_MB_PROBE_STEP
    st r1, (r2)
    st r0, bcmfw_probe_cur
    rts

/* probe_pass / probe_fail: record the current check's state with r0-r5
 * as they are at the call, and its pass bit.  Preserve r0-r5. */
probe_pass:
    push r0-r5, lr
    mov r0, #BCMFW_PROBE_PASSED
    b probe_finish
probe_fail:
    push r0-r5, lr
    mov r0, #BCMFW_PROBE_FAILED
probe_finish:
    ld r1, bcmfw_probe_cur
    mov r2, r1
    mul r2, #(4 * BCMFW_PROBE_REC_WORDS)
    mov r3, #BCMFW_PROBE_REC(0)
    add r2, r3
    mov r3, r1
    lsl r3, #16
    or r3, r0
    mov r4, #BCMFW_PROBE_ST_TAG
    or r3, r4
    st r3, (r2)
    ld r3, (sp+20)              /* r0-r5 (push: r5 lowest) */
    st r3, (r2+16)
    ld r3, (sp+16)
    st r3, (r2+20)
    ld r3, (sp+12)
    st r3, (r2+24)
    ld r3, (sp+8)
    st r3, (r2+28)
    ld r3, (sp+4)
    st r3, (r2+32)
    ld r3, (sp+0)
    st r3, (r2+36)
    mov r3, #BCMFW_PROBE_PASSED
    cmp r0, r3
    bne probe_finish_out
    mov r3, #1
    lsl r3, r1
    mov r4, #(BCMFW_PROBE_RESULTS + 16)
    ld r5, (r4)
    or r5, r3
    st r5, (r4)
probe_finish_out:
    pop r0-r5, pc

    .section .rodata
    .align 2
/* Where the handler resumes after a fault in check n: entry n + 1 */
    .global bcmfw_probe_resume
bcmfw_probe_resume:
    .word c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c_done

    .data
    .global bcmfw_probe_cur, bcmfw_probe_sp, bcmfw_probe_lr
bcmfw_probe_cur:
    .word -1                    /* the running check; -1: none */
bcmfw_probe_sp:
    .word 0
bcmfw_probe_lr:
    .word 0
probe_halves:
    .word 0x7FFE8001
    .word 0, 0, 0, 0
probe_word:
    .word 0
    .word 0, 0, 0, 0
probe_buf:
    .word 0x0BADF00D, 0x5EED1234, 0xC0FFEE55, 0x600DCAFE
probe_src:
    .byte 0x5A, 0xC3, 0, 0
probe_dst:
    .word 0
#endif
