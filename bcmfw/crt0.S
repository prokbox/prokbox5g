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
 * bcmfw start-up: vector table, entry, .bss clear, the exception stubs
 * and handler, and the idle timer handler.  Native VideoCore II assembly (vc2dis syntax).
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
#include "hw.h"

    .syntax vc2

/* 64 vector words at 0: 0x00-0x7C exceptions, 0x80-0xFC interrupt
 * vectors 32-63 (source n uses vector 32 + n).  Vector k points at its
 * own 8-byte stub (bcmfw_vstubs + 8k), which saves r0-r5 and lr before
 * anything else and tells the common handler its number, so a stray
 * exception is visible to the host with its vector.  0x100-0x1FF stays
 * zero: the host mailbox lives there. */
#define VS(k) bcmfw_vstubs + 8 * k
    .section .vectors
#if BCMFW_PROBE
    b probe_from0               /* records the entry, then _start */
    .align 2
#elif BCMFW_RESET_BRANCH
    b _start
    .align 2
#else
    .word VS(0)
#endif
    .word VS(1), VS(2), VS(3), VS(4), VS(5), VS(6), VS(7), VS(8)
    .word VS(9), VS(10), VS(11), VS(12), VS(13), VS(14), VS(15), VS(16)
    .word VS(17), VS(18), VS(19), VS(20), VS(21), VS(22), VS(23), VS(24)
    .word VS(25), VS(26), VS(27), VS(28), VS(29), VS(30), VS(31), VS(32)
    .word VS(33), VS(34), VS(35), VS(36), VS(37), VS(38), VS(39), VS(40)
    .word VS(41), VS(42), VS(43), VS(44), VS(45), VS(46), VS(47), VS(48)
#if BCMFW_IDLE_SLEEP
    .word bcmfw_idle_tick       /* vector 49: timer compare 1 */
#else
    .word VS(49)
#endif
#if BCMFW_IDLE_SLEEP && BCMFW_IDLE_DOORBELL
    .word VS(50), VS(51), VS(52), VS(53), VS(54)
    .word bcmfw_idle_bell       /* vector 55: the host doorbell [M] */
    .word VS(56)
#else
    .word VS(50), VS(51), VS(52), VS(53), VS(54), VS(55), VS(56)
#endif
    .word VS(57), VS(58), VS(59), VS(60), VS(61), VS(62), VS(63)

    .text
    .global _start
_start:
    di
    mov sp, #BCMFW_STACK_TOP
#if BCMFW_AAC
    /* the AAC decoder's SDRAM tables (vc2cc .xdata) ride in the load
     * image after .data, where .bss is about to be cleared: park them at
     * the top of the decoder arena until the first H.264 OPEN copies them
     * into SDRAM (bcmfw/aac/aac_image.c) */
    mov r0, #__xdata_load
    mov r1, #__xdata_end
    mov r2, #__xdata_start
    sub r1, r2                  /* bytes, a whole number of words */
    mov r3, #BCMFW_AAC_PARK_END
    sub r3, r1                  /* where they go */
    add r1, r0                  /* the end of what is copied */
    b park_test
park_loop:
    ld r2, (r0)++
    st r2, (r3)++
park_test:
    cmp r0, r1
    bne park_loop
#endif
    mov r0, #__bss_start        /* clear .bss, a word at a time */
    mov r1, #__bss_end
    mov r2, #0
    b bss_test
bss_loop:
    st r2, (r0)++
bss_test:
    cmp r0, r1
    bne bss_loop
    bl bcmfw_main
    /* bcmfw_main never returns; if it does, report it as a fault */
    b bcmfw_fault
#if BCMFW_PROBE

/* The core started at 0.  First entry (the step word is still 0: the
 * image has not run yet): say so in the mailbox, then start normally.
 * Any later entry is an exception through vector 0 (or a jump to 0):
 * handled as vector 0, with r0 restored before the stub saves it. */
probe_from0:
    st r0, bcmfw_from0_r0
    mov r0, #BCMFW_MB_PROBE_STEP
    ld r0, (r0)
    cmp r0, #0
    bne from0_again
    mov r0, #BCMFW_MB_PROBE_FROM0
    mov r1, #1
    st r1, (r0)
    b _start
from0_again:
    ld r0, bcmfw_from0_r0
    b bcmfw_vstubs
#endif

/* The vector stubs: 64 x 8 bytes.  The nop is never executed: it only
 * makes the stride 8, so bl's return address names the vector. */
    .global bcmfw_vstubs
bcmfw_vstubs:
    .rept 64
    push r0-r5, lr
    bl bcmfw_exc_common
    nop
    .endr

/* Every exception.  On entry: [sp+0..20] = r5..r0 at the exception (push
 * puts the highest register lowest), [sp+24] = lr, [sp+28] = the frame
 * (sr, then pc at +32); lr = stub + 6.  A fault while this runs goes
 * straight to the legacy stop.  Probe builds fill a record of the
 * results block (bringup.md, "Probe v2"): inside a self-check check, the
 * check's record, then the self-check resumes at the next check; outside,
 * record U, then the legacy stop. */
    .global bcmfw_exc_common
bcmfw_exc_common:
    mov r1, #bcmfw_exc_busy     /* (absolute: forms the self-test runs) */
    ld r0, (r1)
    cmp r0, #0
    bne exc_stop                /* nested: the frame is the new fault's */
    mov r0, #1
    st r0, (r1)
#if BCMFW_BENCH
    /* bench row ALGN (bench/algn.h): a check armed the resume word */
    mov r0, #bcmfw_algn_resume
    ld r0, (r0)
    cmp r0, #0
    bne algn_hook
#endif
#if BCMFW_PROBE
    mov r0, lr                  /* r0 = vector = (lr - stubs - 6) / 8 */
    mov r1, #bcmfw_vstubs + 6
    sub r0, r1
    lsr r0, #3
    mov r1, #bcmfw_probe_cur    /* r1 = check, -1 outside the self-check */
    ld r1, (r1)
    mov r2, r1                  /* r2 = its record (-1: record U) */
    mul r2, #(4 * BCMFW_PROBE_REC_WORDS)
    mov r3, #BCMFW_PROBE_REC(0)
    add r2, r3
    mov r3, r0                  /* status */
    lsl r3, #8
    mov r4, r1
    lsl r4, #24
    lsr r4, #8
    or r3, r4
    mov r4, #(BCMFW_PROBE_ST_TAG + BCMFW_PROBE_FAULT)
    or r3, r4
    st r3, (r2)
    ld r3, (sp+28)              /* frame sr, pc, address */
    st r3, (r2+4)
    ld r3, (sp+32)
    st r3, (r2+8)
    mov r3, sp
    add r3, #28
    st r3, (r2+12)
    ld r3, (sp+20)              /* r0-r5 as the stub saved them */
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
    mov r3, #-1
    cmp r1, r3
    beq exc_stop                /* outside any check */
    mov r3, #1                  /* the check's fault bit: block and mailbox */
    lsl r3, r1
    mov r4, #(BCMFW_PROBE_RESULTS + 20)
    ld r5, (r4)
    or r5, r3
    st r5, (r4)
    mov r4, #BCMFW_MB_PROBE_FAULTED
    st r5, (r4)
    mov r4, #bcmfw_probe_resume + 4     /* resume at check n + 1 */
    lsl r1, #2
    add r4, r1
    ld r5, (r4)
    mov r3, #bcmfw_probe_sp     /* rti frame at the self-check's sp */
    ld r3, (r3)
    add r3, #-8
    ld r4, (sp+28)              /* sr as it was before the exception */
    st r4, (r3)
    st r5, (r3+4)
    mov r0, #0
    mov r1, #bcmfw_exc_busy
    st r0, (r1)
    mov sp, r3
    rti
#endif
#if BCMFW_BENCH
/* An ALGN check faulted (r0 = its resume pc): record the vector, the
 * saved pc, r0-r5 and the count in bcmfw_algn_fault, disarm, and resume
 * in bcmfw_algn_call with the sp it saved (an rti frame built there,
 * the sr as it was before the exception) */
algn_hook:
    mov r2, #bcmfw_algn_fault
    mov r3, lr                  /* vector = (lr - stubs - 6) / 8 */
    mov r4, #bcmfw_vstubs + 6
    sub r3, r4
    lsr r3, #3
    st r3, (r2)
    ld r3, (sp+32)              /* the frame's pc */
    st r3, (r2+4)
    ld r3, (sp+20)              /* r0-r5 as the stub saved them */
    st r3, (r2+8)
    ld r3, (sp+16)
    st r3, (r2+12)
    ld r3, (sp+12)
    st r3, (r2+16)
    ld r3, (sp+8)
    st r3, (r2+20)
    ld r3, (sp+4)
    st r3, (r2+24)
    ld r3, (sp+0)
    st r3, (r2+28)
    ld r3, (r2+32)
    add r3, #1
    st r3, (r2+32)
    mov r3, #bcmfw_algn_sp
    ld r3, (r3)
    add r3, #-8
    ld r4, (sp+28)              /* sr as it was */
    st r4, (r3)
    st r0, (r3+4)
    mov r1, #0
    mov r4, #bcmfw_algn_resume
    st r1, (r4)
    mov r4, #bcmfw_exc_busy
    st r1, (r4)
    mov sp, r3
    rti
#endif
exc_stop:
    add sp, #28                 /* sp = the exception frame */
    /* fall through */

/* Legacy stop: tell the host (pc and sp of the exception frame at sp in
 * the mailbox, then the status word) and stop.  Never returns. */
    .global bcmfw_fault
bcmfw_fault:
    di
    mov r0, #BCMFW_MB_FAULT_SP
    mov r1, sp
    st r1, (r0)
    ld r1, (sp+4)               /* return pc of the exception frame */
    mov r0, #BCMFW_MB_FAULT_PC
    st r1, (r0)
    mov r1, #BCMFW_STATUS_FAULT
    mov r0, #BCMFW_MB_COMMAND
    st r1, (r0)
fault_stop:
    sleep
    b fault_stop

    .data
    .global bcmfw_exc_busy
bcmfw_exc_busy:
    .word 0
#if BCMFW_PROBE
bcmfw_from0_r0:
    .word 0
#endif
    .text

#if BCMFW_IDLE_SLEEP
/* Timer compare 1: re-arm one tick from now and clear the match.  The
 * idle loop only enables interrupts around its sleep instruction. */
bcmfw_idle_tick:
    add sp, #-8
    st r0, (sp+0)
    st r1, (sp+4)
    mov r0, #TIMER_CLO
    ld r1, (r0)
    mov r0, #BCMFW_IDLE_TICK_US
    add r1, r0
    mov r0, #TIMER_C(1)
    st r1, (r0)
    mov r0, #TIMER_CS
    mov r1, #2
    st r1, (r0)
    mov r0, #BCMFW_MB_IDLE_TICKS
    ld r1, (r0)
    add r1, #1
    st r1, (r0)
    ld r0, (sp+0)
    ld r1, (sp+4)
    add sp, #8
    rti
#if BCMFW_IDLE_DOORBELL
/* Interrupt source 23, the host doorbell [M]: acknowledge it (Apple's
 * handler writes 1 to 0x10003800, @d56a) and count it.  The idle loop
 * reads the command word itself once awake. */
bcmfw_idle_bell:
    add sp, #-8
    st r0, (sp+0)
    st r1, (sp+4)
    mov r0, #BCMFW_DOORBELL_ACK
    mov r1, #1
    st r1, (r0)
    mov r0, #BCMFW_MB_IDLE_BELLS
    ld r1, (r0)
    add r1, #1
    st r1, (r0)
    ld r0, (sp+0)
    ld r1, (sp+4)
    add sp, #8
    rti
#endif
#endif
