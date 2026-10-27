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
 * Bench row ALGN (bench builds): the scalar and vector alignment checks,
 * their table, and the call that survives a fault (algn.h).
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
#include "bench/algn.h"

    .syntax vc2
#if BCMFW_BENCH
    .text

/* uint32_t bcmfw_algn_call(uint32_t fn): call check fn; 0 if it returned,
 * 1 if crt0.S's hook caught a fault (bcmfw_algn_fault holds it).  The
 * check's r2-r5 go to bcmfw_algn_regs either way.  The nops after the
 * call give an imprecise exception (taken up to 4 instructions after its
 * access) room to arrive while the hook is still armed. */
    .global bcmfw_algn_call
bcmfw_algn_call:
    push r6, lr
    mov r6, r0
    mov r0, sp
    st r0, bcmfw_algn_sp
    mov r0, #algn_resumed
    st r0, bcmfw_algn_resume
    bl r6
    nop
    nop
    nop
    nop
    nop
    nop
    mov r0, #0
    st r0, bcmfw_algn_resume
    mov r1, #bcmfw_algn_regs
    st r2, (r1)
    st r3, (r1+4)
    st r4, (r1+8)
    st r5, (r1+12)
    pop r6, pc
/* the hook resumes here: sp as above, the resume word already cleared */
algn_resumed:
    mov r1, #bcmfw_algn_fault
    ld r2, (r1+16)              /* r2-r5 as the stub saved them */
    ld r3, (r1+20)
    ld r4, (r1+24)
    ld r5, (r1+28)
    mov r1, #bcmfw_algn_regs
    st r2, (r1)
    st r3, (r1+4)
    st r4, (r1+8)
    st r5, (r1+12)
    mov r0, #1
    pop r6, pc

/* Every scalar check: r4 = r5 = 0, the access (label algn_<tag>_at), then
 * four adds whose sums say how many ran before an imprecise exception
 * (r4, r5 = 0,0 none; 1,0 one; 1,1 two; 3,1 three; 3,3 four). */

/* ---- scalar loads from algn_src (byte k = 0x10 + k) ------------------ */

/* LW1C: ld (rs) at +1, the control: silicon S2/S3 faulted (vector 1)
 * after reading the word at the address rounded down, 4 adds later */
algn_lw1c:
    mov r4, #0
    mov r5, #0
    mov r0, #algn_src + 1
algn_lw1c_at:
    ld r2, (r0)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* LW2: ld (rs) at +2 */
algn_lw2:
    mov r4, #0
    mov r5, #0
    mov r0, #algn_src + 2
algn_lw2_at:
    ld r2, (r0)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* LW3: ld (rs) at +3 */
algn_lw3:
    mov r4, #0
    mov r5, #0
    mov r0, #algn_src + 3
algn_lw3_at:
    ld r2, (r0)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* LWO2: the 32-bit ld (rs + off) form at +2 (offset 64, base src - 62) */
algn_lwo2:
    mov r4, #0
    mov r5, #0
    mov r0, #algn_src + 2 - 64
algn_lwo2_at:
    ld r2, (r0+64)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* LWL2: the 48-bit ld (rs + off) form at +2 (offset 0x10000) */
algn_lwl2:
    mov r4, #0
    mov r5, #0
    mov r0, #algn_src + 2 - 65536
algn_lwl2_at:
    ld r2, (r0+65536)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* LWP2: ld (rs)++ at +2: r3 says whether the base was written back */
algn_lwp2:
    mov r4, #0
    mov r5, #0
    mov r3, #algn_src + 2
algn_lwp2_at:
    ld r2, (r3)++
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* LWX2: ld (rs + ri) at +6 (ri = 1, scaled by 4) */
algn_lwx2:
    mov r4, #0
    mov r5, #0
    mov r0, #algn_src + 2
    mov r1, #1
algn_lwx2_at:
    ld r2, (r0+r1)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* LH1: ldh (rs) at +1, an odd halfword */
algn_lh1:
    mov r4, #0
    mov r5, #0
    mov r0, #algn_src + 1
algn_lh1_at:
    ldh r2, (r0)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* LH3: ldh (rs) at +3, across the word */
algn_lh3:
    mov r4, #0
    mov r5, #0
    mov r0, #algn_src + 3
algn_lh3_at:
    ldh r2, (r0)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* LHO1: the 32-bit ldh (rs + off) form at +1 (offset 64) */
algn_lho1:
    mov r4, #0
    mov r5, #0
    mov r0, #algn_src + 1 - 64
algn_lho1_at:
    ldh r2, (r0+64)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* LSH1: ldsh (rs) at +1 */
algn_lsh1:
    mov r4, #0
    mov r5, #0
    mov r0, #algn_src + 1
algn_lsh1_at:
    ldsh r2, (r0)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* LHP1: ldh (rs)++ at +1 */
algn_lhp1:
    mov r4, #0
    mov r5, #0
    mov r3, #algn_src + 1
algn_lhp1_at:
    ldh r2, (r3)++
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* ---- scalar stores into algn_buf (filled with ALGN_FILL first) ------- */

/* SW1: st (rs) at +1: fault?  precise?  where do the bytes land? */
algn_sw1:
    mov r4, #0
    mov r5, #0
    mov r2, #0xC3C2C1C0
    mov r0, #algn_buf + 1
algn_sw1_at:
    st r2, (r0)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* SW2: st (rs) at +2 */
algn_sw2:
    mov r4, #0
    mov r5, #0
    mov r2, #0xC3C2C1C0
    mov r0, #algn_buf + 2
algn_sw2_at:
    st r2, (r0)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* SW3: st (rs) at +3 */
algn_sw3:
    mov r4, #0
    mov r5, #0
    mov r2, #0xC3C2C1C0
    mov r0, #algn_buf + 3
algn_sw3_at:
    st r2, (r0)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* SWO2: the 32-bit st (rs + off) form at +2 */
algn_swo2:
    mov r4, #0
    mov r5, #0
    mov r2, #0xC3C2C1C0
    mov r0, #algn_buf + 2 - 64
algn_swo2_at:
    st r2, (r0+64)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* SWP2: st (rs)++ at +2 */
algn_swp2:
    mov r4, #0
    mov r5, #0
    mov r2, #0xC3C2C1C0
    mov r3, #algn_buf + 2
algn_swp2_at:
    st r2, (r3)++
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* SH1: sth (rs) at +1 */
algn_sh1:
    mov r4, #0
    mov r5, #0
    mov r2, #0xD1D0
    mov r0, #algn_buf + 1
algn_sh1_at:
    sth r2, (r0)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* SH3: sth (rs) at +3, across the word */
algn_sh3:
    mov r4, #0
    mov r5, #0
    mov r2, #0xD1D0
    mov r0, #algn_buf + 3
algn_sh3_at:
    sth r2, (r0)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* SHO1: the 32-bit sth (rs + off) form at +1 */
algn_sho1:
    mov r4, #0
    mov r5, #0
    mov r2, #0xD1D0
    mov r0, #algn_buf + 1 - 64
algn_sho1_at:
    sth r2, (r0+64)
    add r4, #1
    add r5, #1
    add r4, #2
    add r5, #2
    rts

/* ---- vector loads: one row from algn_src + k, stored back aligned ---- */

/* V16A8: vld16 at +8, the control: S18-S22 read from +0 (quirks #17a) */
algn_v16a8:
    mov r0, #algn_src + 8
algn_v16a8_at:
    vld16 d00, (r0)
    mov r1, #algn_buf
    vst16 d00, (r1)
    rts

/* V16A4: vld16 at +4 */
algn_v16a4:
    mov r0, #algn_src + 4
algn_v16a4_at:
    vld16 d00, (r0)
    mov r1, #algn_buf
    vst16 d00, (r1)
    rts

/* V16A2: vld16 at +2 */
algn_v16a2:
    mov r0, #algn_src + 2
algn_v16a2_at:
    vld16 d00, (r0)
    mov r1, #algn_buf
    vst16 d00, (r1)
    rts

/* V8A8: vld8 at +8 into an H8 row whose high bytes are zero, stored as
 * H16: halfword i = the byte loaded for lane i */
algn_v8a8:
    mov r1, #algn_zero
    vld16 d00, (r1)
    mov r0, #algn_src + 8
algn_v8a8_at:
    vld8 d00, (r0)
    mov r1, #algn_buf
    vst16 d00, (r1)
    rts

/* V8A4: vld8 at +4 */
algn_v8a4:
    mov r1, #algn_zero
    vld16 d00, (r1)
    mov r0, #algn_src + 4
algn_v8a4_at:
    vld8 d00, (r0)
    mov r1, #algn_buf
    vst16 d00, (r1)
    rts

/* V8A2: vld8 at +2 */
algn_v8a2:
    mov r1, #algn_zero
    vld16 d00, (r1)
    mov r0, #algn_src + 2
algn_v8a2_at:
    vld8 d00, (r0)
    mov r1, #algn_buf
    vst16 d00, (r1)
    rts

/* V16S40: vld16 REP 2 at a stride of 40: row 1 at +40 (8 mod 32), both
 * rows stored aligned; the record keeps row 1 (window 8) */
algn_v16s40:
    mov r0, #algn_src
    mov r1, #40
algn_v16s40_at:
    vld16 d00, (r0)+=r1 rep2
    mov r1, #algn_buf
    vst16 d00, (r1)
    vst16 d01, (r1+32)
    rts

/* VPG4: jv_yuv16's gather (gen_jdec.py): a lone vldp8, REP 16 at a
 * stride of 4, from +12, so rows 0-3 start at line phases 12, 0, 4, 8;
 * lanes 0-3 of each row lie in its first line.  Rows 0-3 zeroed first,
 * stored aligned; r2-r5 = word 0 (lanes 0 and 1) of rows 0-3 */
algn_vpg4:
    mov r1, #algn_zero
    vld16 d00, (r1)
    vld16 d01, (r1)
    vld16 d02, (r1)
    vld16 d03, (r1)
    mov r0, #algn_src + 12
    mov r1, #4
algn_vpg4_at:
    vldp8 d00, (r0)+=r1 rep16
    mov r1, #algn_buf
    vst16 d00, (r1)
    vst16 d01, (r1+32)
    vst16 d02, (r1+64)
    vst16 d03, (r1+96)
    ld r2, (r1)
    ld r3, (r1+32)
    ld r4, (r1+64)
    ld r5, (r1+96)
    rts

/* VPGX: a lone vldp8 whose lanes cross the line (from +14: lanes 0-1 in
 * the first line, 2-15 in the next): does vldp alone fetch the crossing
 * lanes (open-questions #21)?  Row 0 zeroed first; r2, r3 = words 0, 1
 * (lanes 0-1, 2-3) */
algn_vpgx:
    mov r1, #algn_zero
    vld16 d00, (r1)
    mov r0, #algn_src + 14
algn_vpgx_at:
    vldp8 d00, (r0)
    mov r1, #algn_buf
    vst16 d00, (r1)
    ld r2, (r1)
    ld r3, (r1+4)
    rts

/* VPQS: the vldp8/vldq8 pair, REP 4 at a stride of 20 from +4 (row
 * phases 4, 8, 12, 0): is each row split at its own line, or at the
 * first row's?  (Our display tile and decoder MC run pairs at strides
 * such as 88 and 120 for widths that are not multiples of 16.)  r2-r5 =
 * word 7 (lanes 14, 15: past the line for phases 4, 8, 12) of rows 0-3 */
algn_vpqs:
    mov r1, #algn_zero
    vld16 d00, (r1)
    vld16 d01, (r1)
    vld16 d02, (r1)
    vld16 d03, (r1)
    mov r0, #algn_src + 4
    mov r1, #20
algn_vpqs_at:
    vldp8 d00, (r0)+=r1 rep4
    vldq8 d00, (r0)+=r1 rep4
    mov r1, #algn_buf
    vst16 d00, (r1)
    vst16 d01, (r1+32)
    vst16 d02, (r1+64)
    vst16 d03, (r1+96)
    ld r2, (r1+28)
    ld r3, (r1+60)
    ld r4, (r1+92)
    ld r5, (r1+124)
    rts

/* ---- vector stores: an aligned row from algn_src, stored at buf + k -- */

/* VS16A8: vst16 at +8 */
algn_vs16a8:
    mov r1, #algn_src
    vld16 d00, (r1)
    mov r0, #algn_buf + 8
algn_vs16a8_at:
    vst16 d00, (r0)
    rts

/* VS16A4: vst16 at +4 */
algn_vs16a4:
    mov r1, #algn_src
    vld16 d00, (r1)
    mov r0, #algn_buf + 4
algn_vs16a4_at:
    vst16 d00, (r0)
    rts

/* VS16A2: vst16 at +2 */
algn_vs16a2:
    mov r1, #algn_src
    vld16 d00, (r1)
    mov r0, #algn_buf + 2
algn_vs16a2_at:
    vst16 d00, (r0)
    rts

/* VS8A0: vst8 from row 0 at +0: the layout itself (open-questions #2) */
algn_vs8a0:
    mov r1, #algn_src
    vld16 d00, (r1)
    mov r0, #algn_buf
algn_vs8a0_at:
    vst8 d00, (r0)
    rts

/* VS8A8: vst8 at +8 */
algn_vs8a8:
    mov r1, #algn_src
    vld16 d00, (r1)
    mov r0, #algn_buf + 8
algn_vs8a8_at:
    vst8 d00, (r0)
    rts

/* VS8A4: vst8 at +4 */
algn_vs8a4:
    mov r1, #algn_src
    vld16 d00, (r1)
    mov r0, #algn_buf + 4
algn_vs8a4_at:
    vst8 d00, (r0)
    rts

/* ---- the table (algn.h ALGN_E_*): tag, routine, access, flags | window */

    .section .rodata
    .align 2
    .global algn_tab
algn_tab:
    .word 0x4C573143, algn_lw1c, algn_lw1c_at, 0    /* LW1C */
    .word 0x4C575232, algn_lw2, algn_lw2_at, 0    /* LWR2 */
    .word 0x4C575233, algn_lw3, algn_lw3_at, 0    /* LWR3 */
    .word 0x4C574F32, algn_lwo2, algn_lwo2_at, 0    /* LWO2 */
    .word 0x4C574C32, algn_lwl2, algn_lwl2_at, 0    /* LWL2 */
    .word 0x4C575032, algn_lwp2, algn_lwp2_at, 0    /* LWP2 */
    .word 0x4C575832, algn_lwx2, algn_lwx2_at, 0    /* LWX2 */
    .word 0x4C485231, algn_lh1, algn_lh1_at, 0    /* LHR1 */
    .word 0x4C485233, algn_lh3, algn_lh3_at, 0    /* LHR3 */
    .word 0x4C484F31, algn_lho1, algn_lho1_at, 0    /* LHO1 */
    .word 0x4C534831, algn_lsh1, algn_lsh1_at, 0    /* LSH1 */
    .word 0x4C485031, algn_lhp1, algn_lhp1_at, 0    /* LHP1 */
    .word 0x53575231, algn_sw1, algn_sw1_at, 0    /* SWR1 */
    .word 0x53575232, algn_sw2, algn_sw2_at, 0    /* SWR2 */
    .word 0x53575233, algn_sw3, algn_sw3_at, 0    /* SWR3 */
    .word 0x53574F32, algn_swo2, algn_swo2_at, 0    /* SWO2 */
    .word 0x53575032, algn_swp2, algn_swp2_at, 0    /* SWP2 */
    .word 0x53485231, algn_sh1, algn_sh1_at, 0    /* SHR1 */
    .word 0x53485233, algn_sh3, algn_sh3_at, 0    /* SHR3 */
    .word 0x53484F31, algn_sho1, algn_sho1_at, 0    /* SHO1 */
    .word 0x564C3638, algn_v16a8, algn_v16a8_at, 0    /* VL68 */
    .word 0x564C3634, algn_v16a4, algn_v16a4_at, 0    /* VL64 */
    .word 0x564C3632, algn_v16a2, algn_v16a2_at, 0    /* VL62 */
    .word 0x564C3838, algn_v8a8, algn_v8a8_at, 0    /* VL88 */
    .word 0x564C3834, algn_v8a4, algn_v8a4_at, 0    /* VL84 */
    .word 0x564C3832, algn_v8a2, algn_v8a2_at, 0    /* VL82 */
    .word 0x564C5334, algn_v16s40, algn_v16s40_at, 8    /* VLS4 */
    .word 0x56504734, algn_vpg4, algn_vpg4_at, 0    /* VPG4 */
    .word 0x56504758, algn_vpgx, algn_vpgx_at, 0    /* VPGX */
    .word 0x56505153, algn_vpqs, algn_vpqs_at, 0    /* VPQS */
    .word 0x56533638, algn_vs16a8, algn_vs16a8_at, 0    /* VS68 */
    .word 0x56533634, algn_vs16a4, algn_vs16a4_at, 0    /* VS64 */
    .word 0x56533632, algn_vs16a2, algn_vs16a2_at, 0    /* VS62 */
    .word 0x56533830, algn_vs8a0, algn_vs8a0_at, 0    /* VS80 */
    .word 0x56533838, algn_vs8a8, algn_vs8a8_at, 0    /* VS88 */
    .word 0x56533834, algn_vs8a4, algn_vs8a4_at, 0    /* VS84 */
    .word 0x44333243, algn_dma_d32c, 0, ALGN_F_C | 0    /* D32C */
    .word 0x44345739, algn_dma_d4w9, 0, ALGN_F_C | 0    /* D4W9 */
    .word 0x44324833, algn_dma_d2h3, 0, ALGN_F_C | 0    /* D2H3 */
    .word 0x44314235, algn_dma_d1b5, 0, ALGN_F_C | 0    /* D1B5 */
    .word 0x4433324D, algn_dma_d32m, 0, ALGN_F_C | 0    /* D32M */
    .word 0, 0, 0, 0            /* the end: tag 0 */

    .data
    .align 2
    .global bcmfw_algn_resume, bcmfw_algn_sp, bcmfw_algn_regs, bcmfw_algn_fault
bcmfw_algn_resume:
    .word 0                     /* non-zero: crt0's hook resumes there */
bcmfw_algn_sp:
    .word 0
bcmfw_algn_regs:
    .word 0, 0, 0, 0
bcmfw_algn_fault:
    .word 0, 0, 0, 0, 0, 0, 0, 0, 0
    .align 5
    .global algn_src
algn_src:
    .byte 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17
    .byte 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
    .byte 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27
    .byte 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F
    .byte 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37
    .byte 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
    .byte 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47
    .byte 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F
    .byte 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57
    .byte 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F
    .byte 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67
    .byte 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F
    .byte 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77
    .byte 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F
    .byte 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87
    .byte 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F
algn_zero:
    .fill 32, 1, 0
    .global algn_buf
algn_buf:
    .fill 128, 1, 0              /* 4 * ALGN_BUF_WORDS */
#endif
