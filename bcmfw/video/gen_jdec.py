#!/usr/bin/env python3
#***************************************************************************
#               __________                   __    __________
#    Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
#    Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
#    Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
#    Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
#                                                \/        \/               \/
#
# Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
# Not affiliated with or endorsed by Apple Inc.
#
# Copyright (C) 2026 prokopto-dev
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
#***************************************************************************

"""Generate jdec's vector kernels (jdec_kernels.S) for the BCM image's
JPEG decode (bcmfw/video/jdec_vpu.c, through the decoder's hooks): the
colour stage (jhook.h jdec_yuv565), YCbCr to RGB565 with the ordered
dither, bit for bit the C path (jpeg_common.h yuv_to_rgb, then bmp.c
output_row_8_native's dither and packing; algorithms.md section 4); and
the IDCTs (jhook.h jdec_idct), jpeg_load.c's 4-, 8- and 16-point islow
(jpeg_idct4v/4h, 8v/8h, 16v/16h) on four blocks at once, bit for bit for
any int16 coefficients (algorithms.md section 1, jpeg-spike.md sections
13-14); and the put and the DC fill of the IDCTs' blocks into the row
buffer (jhook.h jdec_put, jdec_fill; jpeg-spike.md section 16).

  jv_yuv16(dst r0, src r1, delta r2)
      16 pixels: src holds them as three planes, 32-byte aligned: Y at
      +0, Cb at +32, Cr at +64, 16 bytes each (jdec_vpu.c yuv565 splits
      the decoder's struct uint8_rgb pixels into them); delta the 16
      dither offsets as halfwords (dither_table[x] ^ DITHERY(row), 32-byte
      aligned), dst 16 RGB565 pixels (32-byte aligned).

The arithmetic, per pixel (every intermediate fits a 16-bit lane):
  - C's (128 y + 64 + c) / 128, clamped to 0..255, is clamp(y + ((64 + c)
    >> 7)) with an arithmetic shift: for a negative numerator both clamp
    to 0, and 128 y leaves the floor exactly.  The terms, with the 128
    offsets folded in (lane products wrap mod 2^16, the sums fit):
      R: 179 v - 22848;  G: -43 u - 91 v + 17216;  B: 227 u - 28992
    then vasr #7, and a saturating add of y into an 8-bit D (the clamp).
  - The dither: (31 c + (c >> 3) + d) >> 8 = ((249 c) >> 3 + d) >> 8, and
    (63 c + (c >> 2) + d) >> 8 = ((253 c) >> 2 + d) >> 8 (31 c is whole);
    249 * 255 and 253 * 255 fit an unsigned 16-bit lane and vlsr is
    logical.  The >> 8 is the sum's high byte, read as an H8 operand
    (sums below 8,448).
  - The pack: R5 * 2048 + G6 * 32 + B5.

The load: three plain vld8s of 16 bytes at 32-byte aligned addresses put
Y, Cb and Cr in H8 rows 29, 30 and 31, lane i being pixel i; every ALU op
then works on all 16 pixels at once (REP 1) and writes H16 rows.  (Until
2026-10-25 a lone vldp8 REP 16 at a stride of 4 gathered the interleaved
pixels into V8 columns: a form unknown on silicon, which vc2sim's strict
alignment mode traps; alignment-audit.md.)  jdec_vpu.c's start-up check
compares the kernel with the C on 64 pixels and keeps the C on any
difference (jobs.h J_VPU_YUV).  The result, H16 row 0,
is stored by one 48-bit vst16 from d00 at offset 0, the device-checked
store form.

Forms, against the device-checked set (gen_xfade.py, gen_jobs.py, gen_disp
ADSN): the ALU operations are MU8R's vmul D16 A8 Breg, ADIR's vadd #imm,
ALUX's vadd D16 A16 B16, AD68's vadd D16 A16 B8 and ADSN's vadds D8 A8
B16.  Unproven on silicon (Apple kinds, shown by vc2sim and Apple's
corpus only):
  - vld8 into H8 rows 29-31 (REP 1, 32-byte aligned; LD8 and LD8R are
    the device rows of the form);
  - vasr D16 A16 #7 (the key's #15 is also unproven) and vlsr D16 A16
    #imm (LSR8 is the 8-bit form);
  - vld16 of the delta row into row 24 (REP 1 loads into row 0 are
    checked).

  jv_idct4n(px r0, coef r1, d r2)
      four blocks at one size: coef (512 bytes, 32-byte aligned) holds
      block q's F[v][u] at halfword (q >> 1) * 128 + (q & 1) * 8 + 16 v +
      u, and is left 0; px (1 KB, 32-byte aligned) gets block q's pixel
      (n, k) at byte 32 k + 2 n + (q & 1), + 16 (q >> 1) (4 and 8 points)
      or + 512 (q >> 1) (16 points).  d: the size's descriptor
      (jv_idct_d4, d8, d16): M, the outputs, pass 1's group step, pass
      2's group limit, the first output rounding with 1023.

The IDCT, as a matrix: jpeg_load.c's 1-D islow is linear, out_n = sum_j
M[n][j] x_j + its rounding term (idct_matrix(); every M[n][j] fits 16
bits; the 4-point's columns 4-7 and the 16-point's 16 rows of 8 inputs
the same way), so each output is eight 16 x 16 products summed in 32
bits, which
the vector unit makes exactly through its 32-bit accumulator (isa.md "The
32-bit accumulator"): vmul ... x15 adds a product's low 16 bits at bit 0
(zero-extended), vmulhd ... acc its floored high half at bit 16, so the
sum is exact mod 2^32, as C's is.  Two REP 4 pairs (jv_dot8) do the eight
products of an output: A (the data, an H16 row) and B (the constants, a
V16 column of CM, M's row n copied down 16 rows by one vld16 at stride 0)
both step a row or a column per repetition.  Two chains, not one REP 8
pair, because silicon's accumulator saturates past 2^31 (quirks #31,
#32) and the 16-point's eight products can reach 81,678 x 32,768; each
chain of four stays under 2^31, and they are joined in wrapping 16-bit
lanes (jv_dot8).
  - Pass 1, four blocks as two groups of 16 lanes: the C's int16
    workspace value is bits 11..26 of sum + 1024, (hi << 5) | (lo >> 11)
    of the accumulator's halves, written as a V16 column, so the
    transpose costs nothing (H/V aliasing).  Any int16 input: no prescale,
    so no |coef| <= 2047 limit (the prototype's, algobench/vpu_islow.py),
    and no block goes to C.
  - Pass 2: sum + (16 + (128 << 5)) << 13, seeded as 514 at the high half;
    (hi >> 2) clamped to 0..255 (vclip into an H8 row, so the pixels come
    out transposed, two blocks to a row's bytes) is the C's
    range_limit(sum >> 18) for any 32-bit sum.
  - The constants: 128 bytes (jv_idct_m, two rows of M a load; an odd
    row's lanes moved down by two H8 copies).  The coefficients are zeroed
    after the load (16 rows of 0 stored), and the pixels stored, by
    jobs_kernels.S's jv_store16 (block-0 rows, single vst16s: the
    device-checked store).
  - About 600 instructions a call (vector ops counted as one), 155 a
    block; the C islow is 656 for a DC-only block, 1,623 with AC terms.
  - The 4-point: the 8-point's layout (the 4 x 4 coefficients are the
    first rows and columns of the block's places; the others, which the
    decoder may store, meet M's zero columns), four outputs a pass, and
    pass 1 rounding outputs 2 and 3 with 1023 (gen_jdec idct4_1d).
  - The 16-point: pass 1 writes group g's sixteen columns into rows 16 +
    16g (P1 rows 16-47), pass 2 runs four groups of 16 lanes (a block's
    16 rows each) and puts block q's pixels at H8 x 16q: H16 rows 0-15 x
    0 then x 32, two stores.

Forms the IDCT adds, none of them checked on silicon (bench-p19.md rows
X15M, X15S, X15Z and MHDU check the accumulator's, REPA the REP 8 pair
itself; the rest are [sim] here, Apple kinds in vc2sim; the start-up
check, jdec_vpu.c idct_check,
falls back to the C IDCT if any of them misbehaves on the chip):
  - x15 (hw5[15]) on vmul D16 with clra, alone and with acc;
  - vmulhd (op 18) D16 A16 B16 with acc;
  - REP 8 accumulating across repetitions, with A (H16 +r4, stepping a
    row) and B (V16, stepping a column) both stepping;
  - a vertical 16-bit B operand (V16, '.1.t') stepping per REP;
  - D with +r: V16 (pass 1, x = 8g + n, or y = 16 g and x = n) and H8
    (pass 2, y = k, x = 16 q');
  - vmov D16 rN (pass 1's rounding term from a register);
  - vld16 REP 16 at a stride of 0 into rows 48-63 (the lane-shift check's
    load, into rows 0-15 there);
  - an H16 A at x 32 stepping a row (REP 16): the 16-point's second half
    moved to x 0 for its store;
  - vclip D8 (H8) A16 #255, vshl/vasr/vlsr D16 A16 #imm, vmov D16 #imm
    REP 16 (the zero rows);
  - H8 A operands at x = 8 and 24 (constant x, REP 16): CM's odd rows.
--unrolled: the eight products as sixteen REP-1 ops in a subroutine
(jv_dot8_1), should silicon not accumulate across repetitions.

  jv_put(px r0, to r1, n r2, rowstep r3, size r4, grey r5)
      n blocks' pixels from jv_idct4n's px into the decoder's row buffer
      (jhook.h jdec_put), the pixels' other bytes kept.
  jv_fill(v r0, out r1, grey r2, rowstep r3, size r4)
      one block of the value v the same way (jhook.h jdec_fill).
Each block row is a 32-byte line (two for 16 points) of the row buffer,
loaded into the VRF, its component bytes written there, and stored back
(see "The put and the DC fill" below; jpeg-spike.md section 16).

Forms the put and the fill add (none checked on silicon; the start-up
check, jdec_vpu.c chk_size, compares them with the C at every size and
keeps the C path if any misbehaves):
  - vadd V8 D (a byte column) with +r, x = (t >> 1) + 16 (t & 1) + 2k
    (+ 32) and y 0 (silicon aligns a vertical D's +r y down to 16: bench
    PUTV, sitting 2; the y 8 and 12 before put lane 0 into row 0 and
    failed the start-up check), from an H8 A with +r at x = 0, 8, 16, 24
    or 32, 48 (rows 48-63);
  - vmov H8 D with +r (x 0 or 16, and + 32) from a scalar B, REP 16
    stepping a row, ifnz after vbitplanes rN setf (MVNZ's predicate, ADSN
    family; gen_jobs' jv_gather16 uses it on a 16-bit D);
  - vld16 REP 16 into rows 48-63 at x 0 and x 32, REP 8 into rows 0-7,
    REP 4 into rows 0-3, REP 16 into rows 0-15 at x 32 (the loads with a
    stride: device-checked into row 0 only, REP 1);
  - vadd H16 x 0 from H16 x 32, REP 16 (jv_idct4n's, unchecked too);
  - the stores are single block-0 vst16s (the device-checked form):
    jv_put_out's rows 0-7 or 0-3, jobs_kernels.S's jv_store16 for 16.

Usage: bcmfw/video/gen_jdec.py [--check] [--mutate NAME --out DIR] [--unrolled]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'h264'))
from vpu_asm import H8, H16, V8, V16, DISCARD, Reg, alu, mem, bitplanes, \
    out_paths, write_text  # noqa: E402

OUT_S = os.path.join(HERE, 'jdec_kernels.S')

# VRF:
#   H8 rows 29 (Y), 30 (Cb), 31 (Cr) x 0: the planes as loaded, lane i
#     pixel i
#   F  H16 row 0 x 0: the RGB565 result (stored from d00)
#   TR, TG, TB, TT  H16 rows 16-19 x 0: 16-bit terms
#   R8, G8, B8  H8 rows 20, 21, 22 x 0: the clamped channels
#   DR, DG, DB  H16 rows 23, 25, 26 x 0: the dithered sums; their high
#     bytes (H8 x 16) are the 5- and 6-bit channels
#   DL  H16 row 24 x 0: the 16 dither offsets
#   P, Q  H16 rows 27, 28 x 0: the pack
VY, VU, VV = H8(29, 0), H8(30, 0), H8(31, 0)
F = H16(0, 0)
TR, TG, TB, TT = H16(16, 0), H16(17, 0), H16(18, 0), H16(19, 0)
R8, G8, B8 = H8(20, 0), H8(21, 0), H8(22, 0)
DR, DL, DG, DB = H16(23, 0), H16(24, 0), H16(25, 0), H16(26, 0)
DRH, DGH, DBH = H8(23, 16), H8(25, 16), H8(26, 16)
P, Q = H16(27, 0), H16(28, 0)

MUTATE = None          # --mutate NAME: a deliberately broken kernel
MUTATIONS = {'g': 'jv_yuv16 rounds G\'s term one unit high (+17217)',
             'dither': 'jv_yuv16 adds no dither to B',
             'clamp': 'jv_yuv16 adds R\'s term without saturating (wraps)'}


def yuv16():
    out = []

    def v(op, d, a, b):
        out.append(alu(op, d, a, b))

    # r0 dst, r1 src, r2 delta; vector bases and strides only r0-r6, and
    # scalar B operands r0-r6 (r7 reads 0 on silicon)
    out.append('push r6, lr')
    out.append('mov r6, r0')                    # dst, for the store
    out.append(mem('vld16', DL, 2, 0))          # the dither offsets
    out.append(mem('vld8', VY, 1, 0))           # the planes, 32-byte aligned
    out.append(mem('vld8', VU, 1, 32))
    out.append(mem('vld8', VV, 1, 64))
    out.append('mov r0, #179')
    out.append('mov r1, #-43')
    out.append('mov r2, #-91')
    out.append('mov r3, #227')
    v('vmul', TR, VV, Reg(0))                   # R: 179 v - 22848
    v('vadd', TR, TR, -22848)
    v('vasr', TR, TR, 7)
    v('vadds' if MUTATE != 'clamp' else 'vadd', R8, VY, TR)
    v('vmul', TG, VU, Reg(1))                   # G: -43 u - 91 v + 17216
    v('vmul', TT, VV, Reg(2))
    v('vadd', TG, TG, TT)
    v('vadd', TG, TG, 17216 if MUTATE != 'g' else 17217)
    v('vasr', TG, TG, 7)
    v('vadds', G8, VY, TG)
    v('vmul', TB, VU, Reg(3))                   # B: 227 u - 28992
    v('vadd', TB, TB, -28992)
    v('vasr', TB, TB, 7)
    v('vadds', B8, VY, TB)
    out.append('mov r4, #249')
    out.append('mov r5, #253')
    v('vmul', DR, R8, Reg(4))                   # ((249 r) >> 3 + d) >> 8
    v('vlsr', DR, DR, 3)
    v('vadd', DR, DR, DL)
    v('vmul', DG, G8, Reg(5))                   # ((253 g) >> 2 + d) >> 8
    v('vlsr', DG, DG, 2)
    v('vadd', DG, DG, DL)
    v('vmul', DB, B8, Reg(4))                   # ((249 b) >> 3 + d) >> 8
    v('vlsr', DB, DB, 3)
    if MUTATE != 'dither':
        v('vadd', DB, DB, DL)
    out.append('mov r4, #2048')
    out.append('mov r5, #32')
    v('vmul', P, DRH, Reg(4))                   # R5 << 11
    v('vmul', Q, DGH, Reg(5))                   # G6 << 5
    v('vadd', P, P, Q)
    v('vadd', F, P, DBH)                        # + B5
    out.append(mem('vst16', F, 6, 0))
    out.append('pop r6, pc')
    return out


# --------------------------------------------------------------------------
# The islow IDCTs (jhook.h jdec_idct): jpeg_load.c's 4-, 8- and 16-point,
# four blocks at a time, exact for any int16 coefficients.

# jpeg_load.c's constants (CONST_BITS 13)
F0298, F0390, F0541, F0765 = 2446, 3196, 4433, 6270
F0899, F1175, F1501, F1847 = 7373, 9633, 12299, 15137
F1961, F2053, F2562, F3072 = 16069, 16819, 20995, 25172


def idct1(x, bias):
    """jpeg_load.c's 8-point islow on x[0..7] before its final shift: the
    eight 32-bit sums out0..out7, with bias added to x0 << 13"""
    z1 = (x[2] + x[6]) * F0541
    tmp2, tmp3 = z1 - x[6] * F1847, z1 + x[2] * F0765
    tmp0 = (x[0] << 13) + bias + (x[4] << 13)
    tmp1 = (x[0] << 13) + bias - (x[4] << 13)
    t10, t13, t11, t12 = tmp0 + tmp3, tmp0 - tmp3, tmp1 + tmp2, tmp1 - tmp2
    o0, o1, o2, o3 = x[7], x[5], x[3], x[1]
    z1, z2, z3, z4 = o0 + o3, o1 + o2, o0 + o2, o1 + o3
    z5 = (z3 + z4) * F1175
    o0, o1, o2, o3 = o0 * F0298, o1 * F2053, o2 * F3072, o3 * F1501
    z1, z2 = -z1 * F0899, -z2 * F2562
    z3, z4 = -z3 * F1961 + z5, -z4 * F0390 + z5
    o0 += z1 + z3
    o1 += z2 + z4
    o2 += z2 + z3
    o3 += z1 + z4
    return [t10 + o3, t11 + o2, t12 + o1, t13 + o0, t13 - o0, t12 - o1, t11 - o2, t10 - o3]


def FIX(x):
    return int(x * 8192 + 0.5)


def idct4_1d(x, bias):
    """jpeg_load.c's 4-point (jpeg_idct4v/4h) on x[0..3] before its final
    shift, bias added to the even part.  jpeg_idct4v rounds and shifts its
    odd part alone, floor((odd + 1024) / 2048), and adds it to the even
    part shifted, (x0 +- x2) << 2, or takes it away.  Added, that is the
    whole sum + 1024, >> 11 (8192 (x0 +- x2) is a multiple of 2048);
    taken away, the whole sum + 1023, >> 11, since -floor(a / b) =
    floor((-a + b - 1) / b): outputs 2 and 3 round with 1023 in pass 1
    (P1_BIAS)"""
    e0, e1 = ((x[0] + x[2]) << 13) + bias, ((x[0] - x[2]) << 13) + bias
    z1 = (x[1] + x[3]) * F0541
    t0, t2 = z1 - x[3] * F1847, z1 + x[1] * F0765
    return [e0 + t2, e1 + t0, e1 - t0, e0 - t2]


def idct16_1d(x, bias):
    """jpeg_load.c's 16-point (jpeg_idct16v/16h) on x[0..7] before its
    final shift, bias added to x0 << 13: the sixteen 32-bit sums"""
    tmp0 = (x[0] << 13) + bias
    z1 = x[4]
    tmp1, tmp2 = z1 * FIX(1.306562965), z1 * F0541
    tmp10, tmp11, tmp12, tmp13 = tmp0 + tmp1, tmp0 - tmp1, tmp0 + tmp2, tmp0 - tmp2
    z1, z2 = x[2], x[6]
    z3 = z1 - z2
    z4, z3 = z3 * FIX(0.275899379), z3 * FIX(1.387039845)
    tmp0, tmp1 = z3 + z2 * F2562, z4 + z1 * F0899
    tmp2, tmp3 = z3 - z1 * FIX(0.601344887), z4 - z2 * FIX(0.509795579)
    t20, t27, t21, t26 = tmp10 + tmp0, tmp10 - tmp0, tmp12 + tmp1, tmp12 - tmp1
    t22, t25, t23, t24 = tmp13 + tmp2, tmp13 - tmp2, tmp11 + tmp3, tmp11 - tmp3
    z1, z2, z3, z4 = x[1], x[3], x[5], x[7]
    tmp11 = z1 + z3
    tmp1, tmp2 = (z1 + z2) * FIX(1.353318001), tmp11 * FIX(1.247225013)
    tmp3, tmp10 = (z1 + z4) * FIX(1.093201867), (z1 - z4) * FIX(0.897167586)
    tmp11, tmp12 = tmp11 * FIX(0.666655658), (z1 - z2) * FIX(0.410524528)
    tmp0 = tmp1 + tmp2 + tmp3 - z1 * FIX(2.286341144)
    tmp13 = tmp10 + tmp11 + tmp12 - z1 * FIX(1.835730603)
    z = (z2 + z3) * FIX(0.138617169)
    tmp1 += z + z2 * FIX(0.071888074)
    tmp2 += z - z3 * FIX(1.125726048)
    z = (z3 - z2) * FIX(1.407403738)
    tmp11 += z - z3 * FIX(0.766367282)
    tmp12 += z + z2 * FIX(1.971951411)
    z2 += z4
    z = z2 * -FIX(0.666655658)
    tmp1 += z
    tmp3 += z + z4 * FIX(1.065388962)
    z2 = z2 * -FIX(1.247225013)
    tmp10 += z2 + z4 * FIX(3.141271809)
    tmp12 += z2
    z2 = (z3 + z4) * -FIX(1.353318001)
    tmp2 += z2
    tmp3 += z2
    z2 = (z4 - z3) * FIX(0.410524528)
    tmp10 += z2
    tmp11 += z2
    a = [t20 + tmp0, t21 + tmp1, t22 + tmp2, t23 + tmp3,
         t24 + tmp10, t25 + tmp11, t26 + tmp12, t27 + tmp13]
    b = [t27 - tmp13, t26 - tmp12, t25 - tmp11, t24 - tmp10,
         t23 - tmp3, t22 - tmp2, t21 - tmp1, t20 - tmp0]
    return a + b


IDCT_1D = {4: (idct4_1d, 4), 8: (idct1, 8), 16: (idct16_1d, 8)}
# pass 1's rounding term, output by output: 1024, but 1023 where the C
# subtracts a separately rounded odd part (the 4-point's outputs 2, 3)
P1_BIAS = {4: (1024, 1024, 1023, 1023), 8: (1024,) * 8, 16: (1024,) * 16}


def idct_matrix(n=8):
    """The n-point 1-D transform as an integer matrix (it is linear):
    out_k = sum_j M[k][j] x_j + bias, n rows of 8 (inputs past the
    transform's, 0).  Every entry fits 16 bits"""
    fn, inputs = IDCT_1D[n]
    M = [[0] * 8 for _ in range(n)]
    for j in range(inputs):
        for k, v in enumerate(fn([int(i == j) for i in range(inputs)], 0)):
            M[k][j] = v
    assert all(-32768 <= v < 32768 for r in M for v in r)
    return M


# VRF of jv_idct4n (16-bit rows of 16 lanes; a row's low bytes are H8
# columns x..x+15, its high bytes x+16..x+31).  Four blocks a call, block
# q = 2g + p; n the vertical pass's outputs (rows), k the horizontal's
# (columns):
#   IN  H16 rows 0-15 x 0, as loaded: row 8g + v, lane 8p + u is
#       coefficient F[v][u] of block q (u, v < 8; the 4-point uses u, v
#       < 4, and M's zero columns drop the rest)
#   P1  pass 1's output n of group g is the V16 column (lane 8p + u: the
#       transpose is free), at 16 + n... for 4 and 8 points the column
#       8g + n of rows 16-31; for 16 points column n of rows 16 + 16g ..
#       So H16 row 16 + 8q' + u (q' = p, or 2g + p for 16 points), lane
#       n (+ 8g for 4 and 8), is block q's workspace value at row n,
#       column u
#   CM  rows 48-63 x 0-31: M's row n replicated down 16 rows, so the V16
#       column j is M[n][j] on every lane (one load at stride 0)
#   TL, TH  H16 rows 48, 49 x 32: the accumulator's low and high halves
#   OUT pass 2's pixels (IN is spent by then): output k of group q'
#       (lanes n, or 8g + n) is H8 row k at x 16q'.  4 and 8 points: H16
#       row k, lane 8g + n holds block 2g's pixel (n, k) in its low byte
#       and block 2g + 1's in its high byte; 16 points: H16 row k x 0
#       holds blocks 0 and 1 (lane n), x 32 blocks 2 and 3
CM, TL, TH = 48, H16(48, 32), H16(49, 32)
TH1 = H16(50, 32)               # jv_dot8: the first chain's high half
P1 = 16
# per size: the kernel's descriptor (jv_idct_d<n>): its M, its outputs,
# pass 1's group step for D's +r (8 columns, or 16 rows), pass 2's last
# group's A offset + 512 (2 or 4 groups of 8 rows), pass 1's first output
# rounding with 1023 (P1_BIAS)
SIZES = {4: (4, 8, 1024 + 2 * 512, 2), 8: (8, 8, 1024 + 2 * 512, 8),
         16: (16, 16 * 64, 1024 + 4 * 512, 16)}

MUTATIONS.update({
    'round': 'jv_idct4n rounds pass 1 one unit high (1025 for 1024)',
    'rep8': 'jv_idct4n sums the eight products in one REP 8 chain (the form '
            'before: a partial sum past 2^31 saturates on silicon)',
    'x15': 'jv_idct4n drops the first chain\'s low half (jv_dot8\'s carry-in; '
           'pass 2\'s low halves with --unrolled)',
    'lane': 'jv_idct4n writes pass 2\'s column 5 into lane 6 of block 2g + 1',
    'm4': 'jv_idct_m4: the 4-point\'s M[1][3] one unit off',
    'm16': 'jv_idct_m16: the 16-point\'s M[9][5] one unit off',
    'store16': 'jv_idct4n: the 16-point\'s blocks 2 and 3 not stored',
})


# The eight products of one output: REP 8 by default (A steps a row, B a
# column, every repetition adding into the same accumulator, as vc2sim and
# isa-vector.md's "one repetition" model it); --unrolled: sixteen REP-1
# ops in a subroutine, should silicon not accumulate across repetitions.
UNROLLED = False


def products(pas):
    """TL/TH = the accumulator after M[n] . A's rows 0..7 (+r4), each
    product split exactly: vmul x15 adds its low 16 bits (zero-extended),
    vmulhd acc its floored high half at bit 16.  The caller seeds the
    accumulator; jv_dot8 writes the low and high halves"""
    if UNROLLED:
        return ['bl jv_dot8_%d' % (2 if MUTATE == 'x15' and pas == 2 else 1)]
    if MUTATE == 'rep8':            # the one REP 8 chain before (it saturates)
        a, b = H16(0, 0, step=True, reg=4), V16(CM, 0, step=True)
        return [alu('vmul', TL, a, b, rep=8, x15=True),
                alu('vmulhd', TH, a, b, rep=8, acc=True)]
    return ['bl jv_dot8']


def dot8_split():
    """jv_dot8: the eight products in two chains of four (REP 4: A steps a
    row, B a column), so no partial sum leaves 32 bits: silicon's
    accumulator saturates there (quirks #31, #32; vc2sim v_acc_sat), where
    C's sum wraps.  |M[n][j]| summed over j < 4 or j >= 4 is at most
    42,170 (the 16-point's; the 8-point's 39,891), times 32,768 under 2^31
    with the seed (514 << 16) and the low halves; all eight together reach
    81,678 (16 points) and could saturate.  Chain 1 (the caller's seed, j
    0-3) gives TH1 and its low half TL1; chain 2 starts from TL1 alone
    (clra x15: zero-extended), adds j 4-7, and writes the sum's low half
    (TL) and high half less TH1; TH = that + TH1 in 16-bit lanes, which
    wrap: the sum mod 2^32 exactly, as C's, under every saturation rule"""
    a0, b0 = H16(0, 0, step=True, reg=4), V16(CM, 0, step=True)
    a4, b4 = H16(4, 0, step=True, reg=4), V16(CM, 4, step=True)
    carry = MUTATE != 'x15'
    out = [alu('vmul', TL, a0, b0, rep=4, x15=True),
           alu('vmulhd', TH1, a0, b0, rep=4, acc=True)]
    out += [alu('vmov', DISCARD, None, TL, clra=True, x15=True)] if carry else \
        [alu('vmov', DISCARD, None, 0, clra=True, acc=True)]
    out += [alu('vmul', TL, a4, b4, rep=4, x15=True),
            alu('vmulhd', TH, a4, b4, rep=4, acc=True),
            alu('vadd', TH, TH, TH1),
            'rts']
    return out


def dot8(x15=True):
    """jv_dot8_1 (--unrolled): products() as sixteen REP-1 ops"""
    out = []
    for j in range(8):
        a, b = H16(j, 0, reg=4), V16(CM, j)
        last = j == 7
        out.append(alu('vmul', TL if last else DISCARD, a, b, x15=x15))
        out.append(alu('vmulhd', TH if last else DISCARD, a, b, acc=True))
    out.append('rts')
    return out


def idct4n():
    """jv_idct4n(px r0, coef r1, d r2), jhook.h jdec_idct: the four
    blocks at coef (IN's layout, 512 bytes, 32-byte aligned) to px (OUT's
    rows, 512 bytes, or 1 KB for 16 points, 32-byte aligned); coef zeroed
    after (the decoder fills only nonzero coefficients).  d: the size's
    descriptor, jv_idct_d4/d8/d16: M's rows (two to 32 bytes), the
    outputs, pass 1's group step, pass 2's group limit.

    Registers: r4 = A's +r (the group's first row), r5 = D's +r, r6 = n
    (or k), r1 = the next row pair of M, r3 = 0 (the stride of CM's load),
    r8/r9 = px/coef, r10 = the outputs, r11 = pass 1's step, r12 = pass
    2's limit, r13 = M."""
    out = ['push r6-r13, lr', 'mov r8, r0', 'mov r9, r1',
           'ld r13, (r2)', 'ld r10, (r2+4)', 'ld r11, (r2+8)', 'ld r12, (r2+12)',
           'ld r2, (r2+16)',
           'mov r3, #32',
           mem('vld16', H16(0, 0), 1, 0, 3, 16),        # IN
           'mov r3, #0']
    for pas in (1, 2):
        lb = 'jv_idct4n_p%d' % pas
        out += ['mov r6, #0', 'mov r1, r13', lb + ':', 'bl jv_idct_cm',
                'mov r4, #%d' % (0 if pas == 1 else 64 * P1), 'mov r5, r6']
        if pas == 2:
            out.append('lsl r5, #6')                # output k: H8 row k
        out.append(lb + 'g:')
        if pas == 1:
            # sum + 1024 (C's rounding term; 1023 from output r2 on) over
            # IN's rows 8g..8g + 7; the int16 result is bits 11..26:
            # (hi << 5) | (lo >> 11)
            out[-1:-1] = ['mov r0, #%d' % (1025 if MUTATE == 'round' else 1024),
                          'cmp r6, r2', 'blt %s_b' % lb, 'add r0, #-1', '%s_b:' % lb]
            out += [alu('vmov', DISCARD, None, Reg(0), clra=True, x15=True),
                    ] + products(1) + [
                    alu('vshl', TH, TH, 5), alu('vlsr', TL, TL, 11),
                    alu('vadd', V16(P1, 0, reg=5), TH, TL),
                    'add r4, #512', 'add r5, r11', 'cmp r4, #1024']
        else:
            # sum + (16 + (128 << 5)) << 13 = 514 << 16 over P1's rows
            # 16 + 8q'..; (A >> 18) clamped to 0..255, into H8 row k at
            # x 16q'
            if MUTATE == 'lane':
                out += ['cmp r5, #%d' % (5 * 64 + 16), 'bne %s_l' % lb, 'add r5, #1',
                        '%s_l:' % lb]
            out += [alu('vmov', DISCARD, None, 514, clra=True, acc=True),
                    ] + products(2) + [
                    alu('vasr', TH, TH, 2),
                    alu('vclip', H8(0, 0, reg=5), TH, 255)]
            if MUTATE == 'lane':
                out += ['cmp r5, #%d' % (5 * 64 + 17), 'bne %s_m' % lb, 'add r5, #-1',
                        '%s_m:' % lb]
            out += ['add r4, #512', 'add r5, #16', 'cmp r4, r12']
        out += ['bne %sg' % lb, 'add r6, #1', 'cmp r6, r10', 'bne ' + lb]
    out += ['mov r0, r8', 'mov r1, #32', 'bl jv_store16',          # OUT
            'cmp r10, #16', 'bne jv_idct4n_z']
    if MUTATE != 'store16':
        out += [alu('vadd', H16(0, 0).stepped(), H16(0, 32).stepped(), 0, rep=16),
                'bl jv_store16']                                   # 16: blocks 2, 3
    out += ['jv_idct4n_z:',
            alu('vmov', H16(0, 0).stepped(), None, 0, rep=16),
            'mov r0, r9', 'bl jv_store16',                         # coef = 0
            'pop r6-r13, pc']
    return out


def idct_cm():
    """jv_idct_cm: CM = M's row r6: an even row loads its pair (r1, at
    stride r3 = 0: one 32-byte row read 16 times), an odd one moves lanes
    8-15 down"""
    return ['mov r7, r6', 'bmask r7, #1', 'cmp r7, #0', 'bne jv_idct_cm_odd',
            mem('vld16', H16(CM, 0), 1, 0, 3, 16), 'add r1, #32', 'rts',
            'jv_idct_cm_odd:',
            alu('vadd', H8(CM, 0).stepped(), H8(CM, 8).stepped(), 0, rep=16),
            alu('vadd', H8(CM, 16).stepped(), H8(CM, 24).stepped(), 0, rep=16),
            'rts']


def idct_table():
    """jv_idct_m<n>: M's rows, 32-byte aligned, two rows to 32 bytes; and
    the descriptors jv_idct_d<n>"""
    lines = ['    .section .rodata']
    for n in (8, 4, 16):
        M = idct_matrix(n)
        if MUTATE == 'm4' and n == 4:
            M[1][3] += 1
        if MUTATE == 'm16' and n == 16:
            M[9][5] += 1
        lines += ['    .balign 32', '    .global jv_idct_m%d' % n, 'jv_idct_m%d:' % n]
        lines += ['    .hword ' + ', '.join(str(v) for v in r) for r in M]
    for n in (4, 8, 16):
        lines += ['    .balign 4', '    .global jv_idct_d%d' % n, 'jv_idct_d%d:' % n,
                  '    .word jv_idct_m%d, %d, %d, %d, %d' % ((n,) + SIZES[n])]
    return lines + ['    .text', '']


# --------------------------------------------------------------------------
# The put and the DC fill (jhook.h jdec_put, jdec_fill): a block's pixels
# into the decoder's row buffer, one component byte of each 4-byte pixel
# (struct uint8_rgb), the other bytes kept.
#
# A 32-byte line of the buffer (8 pixels) loaded as one H16 row at (y, x)
# puts its byte b in column x + (b >> 1) + 16 (b & 1) (isa-vector.md "VRF
# layout"): the component byte c of pixel k, b = 4 k + c, is column
# x + (c >> 1) + 16 (c & 1) + 2 k.  A block row is one line (4 and 8
# points; a 4-point block is half of one) or two (16 points: the second at
# x 32), loaded whole and stored whole, block after block; between the
# load and the store only the block's own component bytes are written:
#   - the put: the IDCT's pixel column k (an H8 row of jv_idct4n's px,
#     reloaded into rows 48-63: lanes n are the block's rows) moved into
#     the V8 column of pixel k (lane n into the copy's row n), two moves a
#     column pair, k and k + 8 (the second line; for 4 and 8 points the
#     second move lands outside what is stored);
#   - the fill: one H8 move of the value, REP 16, on the lanes of the
#     component's bytes only (vbitplanes, then ifnz): 0x55 (4 points) or
#     0x5555, << (t >> 1), at x 16 (t & 1) and x 32 + 16 (t & 1).
# t is the block's place in its line (to & 31: the component, + 16 for a
# 4-point block in a line's second half).  Blocks one after another in one
# line (Cb and Cr of an MCU, two 4-point blocks side by side) share one
# load and one store.  The copy sits in rows 16 - size .. 15, so
# jv_store16 entered at row 16 - size (jv_store16, _8, _12: jobs_kernels.S)
# stores exactly the block's rows, by single block-0 vst16s.  Grey (a
# one-component scan): Y into bytes 0, 1 and 2 (the put: three passes, t,
# t + 1, t + 2; the fill: 0xFFFF at x 0 and 0x5555 at x 16, shifted for a
# 4-point block).  The line and the row step are multiples of 32 (jhook.h).

MUTATIONS.update({
    'puty': 'jv_put: the copy in rows 16 - size .. 15 through a vertical D\'s '
            '+r y of 8 or 12 (the form before: silicon aligns that y down to 16)',
    'mask': 'jv_fill: the lane mask one lane on (0xAAAA for 0x5555)',
    'stride': 'jv_put: pixel k into column 3 k (the interleave stride 3)',
    'dc': 'jv_fill writes the value + 1',
})


def put_in():
    """jv_put_in: the block's rows from the line r7 (row step r3, size r4)
    into rows 0 .. size - 1 (x 0, and x 32 for 16 points); r1 clobbered"""
    if MUTATE == 'puty':
        ld = lambda y, rep: mem('vld16', H16(y, 0), 1, 0, 3, rep)
        return ['mov r1, r7', 'cmp r4, #8', 'blt jv_put_i4', 'beq jv_put_i8',
                ld(0, 16), 'add r1, #32', mem('vld16', H16(0, 32), 1, 0, 3, 16), 'rts',
                'jv_put_i8:', ld(8, 8), 'rts',
                'jv_put_i4:', ld(12, 4), 'rts']
    ld = lambda rep: mem('vld16', H16(0, 0), 1, 0, 3, rep)
    return ['mov r1, r7', 'cmp r4, #8', 'blt jv_put_i4', 'beq jv_put_i8',
            ld(16), 'add r1, #32', mem('vld16', H16(0, 32), 1, 0, 3, 16), 'rts',
            'jv_put_i8:', ld(8), 'rts',
            'jv_put_i4:', ld(4), 'rts']


def put_out():
    """jv_put_out: jv_put_in's rows back to the line r7, by single
    block-0 vst16s (jv_store16's device-checked form): rows 0 .. size - 1,
    or jv_store16 twice for 16 points; r0, r1 clobbered"""
    out = ['mov r0, r7', 'mov r1, r3', 'cmp r4, #8', 'bgt jv_put_o16']
    if MUTATE == 'puty':
        out += ['blt jv_store16_12', 'b jv_store16_8']
    for y in range(8):
        out += [mem('vst16', H16(y, 0), 0, 0), 'add r0, r1']
        if y == 3:
            out += ['cmp r4, #4', 'beq jv_put_o4']
    out += ['jv_put_o4:', 'rts',
            'jv_put_o16:',
            'push r6, lr', 'bl jv_store16',
            alu('vadd', H16(0, 0).stepped(), H16(0, 32).stepped(), 0, rep=16),
            'mov r0, r7', 'add r0, #32', 'bl jv_store16', 'pop r6, pc']
    return out


def put():
    """jv_put(px r0, to r1, n r2, rowstep r3, size r4, grey r5).
    Registers: r6 t, r7 the line, r8 to, r9 q, r10 the block's px column
    (A's +r), r11 its shift (8 or 32 a block pair), r12 n, r14 the
    components left; r5 the components (1 or 3); r0-r2 scratch (r1 A's
    +r, r2 D's: x only, the copy's rows start at row 0: a vertical D's +r
    y is aligned down to 16 on silicon, bench PUTV, sitting 2)"""
    k_step = 3 if MUTATE == 'stride' else 2
    return ['push r6-r14, lr', 'mov r6, #32',
            mem('vld16', H16(48, 0), 0, 0, 6, 16),            # px, reloaded
            'add r0, #512',
            mem('vld16', H16(48, 32), 0, 0, 6, 16),
            'mov r8, r1', 'mov r12, r2', 'mov r9, #0', 'mov r7, #1',
            'lsl r5, #1', 'add r5, #1',
            'mov r11, r4', 'lsr r11, #4', 'lsl r11, #1', 'add r11, #3',  # 3, or 5 at 16
            'jv_put_q:',
            'ld r0, (r8)', 'add r8, #4',
            'mov r6, r0', 'bmask r6, #5', 'sub r0, r6',         # t, the line
            'cmp r0, r7', 'mov r7, r0', 'beq jv_put_t',         # loaded already
            'bl jv_put_in',
            'jv_put_t:',
            # px column of block q: 16 (q & 1) + (q >> 1) << r11
            'mov r10, r9', 'bmask r10, #1', 'lsl r10, #4',
            'mov r1, r9', 'lsr r1, #1', 'lsl r1, r11', 'add r10, r1',
            'mov r14, r5',
            'jv_put_j:',
            # D's +r: column (t >> 1) + 16 (t & 1), row 0
            'mov r2, r6', 'bmask r2, #1', 'lsl r2, #4', 'mov r0, r6', 'lsr r0, #1',
            'add r2, r0'] + ([
            'mov r0, #16', 'sub r0, r4', 'lsl r0, #6', 'add r2, r0'] if MUTATE == 'puty' else []) + [
            'mov r1, r10', 'mov r0, #8', 'cmp r4, #4', 'bne jv_put_k', 'mov r0, #4',
            'jv_put_k:',
            alu('vadd', V8(0, 0, reg=2), H8(48, 0, reg=1), 0),
            alu('vadd', V8(0, 32, reg=2), H8(56, 0, reg=1), 0),
            'add r2, #%d' % k_step, 'add r1, #64',
            'addcmpbne r0, #-1, #0, jv_put_k',
            'add r6, #1', 'addcmpbne r14, #-1, #0, jv_put_j',
            # the next block in the same line (Cr after Cb): its put first
            'add r9, #1', 'cmp r9, r12', 'beq jv_put_o',
            'ld r0, (r8)', 'lsr r0, #5', 'lsl r0, #5', 'cmp r0, r7', 'beq jv_put_q',
            'jv_put_o:',
            'bl jv_put_out',
            'cmp r9, r12', 'bne jv_put_q',
            'pop r6-r14, pc']


def fill():
    """jv_fill(v r0, out r1, grey r2, rowstep r3, size r4).  r6 t, r7 the
    line; r5 the lane mask, r1 its x"""
    out = ['push r6-r7, lr']
    if MUTATE == 'dc':
        out.append('add r0, #1')
    out += ['mov r6, r1', 'bmask r6, #5', 'sub r1, r6', 'mov r7, r1',  # t, the line
            'bl jv_put_in',
            'mov r5, #0x5555', 'cmp r4, #4', 'bne jv_fill_4', 'mov r5, #0x55',
            'jv_fill_4:',
            'mov r1, r6', 'lsr r1, #1', 'lsl r5, r1',           # lanes 2k + (t >> 1)
            'cmp r2, #0', 'beq jv_fill_c',
            # grey (t: byte 0): byte 1 (x 16, lanes 2k), then bytes 0 and 2 (x 0)
            bitplanes(Reg(5)),
            alu('vmov', H8(0, 16, step=True), None, Reg(0), rep=16, pred='ifnz'),
            alu('vmov', H8(0, 48, step=True), None, Reg(0), rep=16, pred='ifnz'),
            'mov r1, r5', 'lsl r1, #1', 'add r5, r1', 'b jv_fill_m',
            'jv_fill_c:']
    if MUTATE == 'mask':
        out.append('lsl r5, #1')
    out += ['jv_fill_m:',
            'mov r1, r6', 'bmask r1, #1', 'lsl r1, #4',         # x 16 (t & 1)
            bitplanes(Reg(5)),
            alu('vmov', H8(0, 0, step=True, reg=1), None, Reg(0), rep=16, pred='ifnz'),
            alu('vmov', H8(0, 32, step=True, reg=1), None, Reg(0), rep=16, pred='ifnz'),
            'bl jv_put_out',
            'pop r6-r7, pc']
    return out


def kernels():
    k = (('jv_yuv16', yuv16, '16 pixels YCbCr to dithered RGB565 (jdec_vpu.c)'),
         ('jv_idct4n', idct4n, 'four blocks, jpeg_load.c\'s 4-, 8- or 16-point '
          'IDCT (jdec_vpu.c, jhook.h jdec_idct)'),
         ('jv_idct_cm', idct_cm, 'jv_idct4n\'s constants for one output'),
         ('jv_put', put, 'a batch\'s 8- or 16-point pixels into the row buffer '
          '(jhook.h jdec_put)'),
         ('jv_fill', fill, 'an 8- or 16-point block of one value into the row buffer '
          '(jhook.h jdec_fill)'),
         ('jv_put_in', put_in, 'a block\'s rows of the row buffer into the VRF'),
         ('jv_put_out', put_out, 'and back'))
    if not UNROLLED:
        k += (('jv_dot8', dot8_split, 'jv_idct4n\'s eight exact products, two '
               'chains of four (no saturation)'),)
    if UNROLLED:
        k += (('jv_dot8_1', dot8, 'jv_idct4n\'s eight exact products'),)
        if MUTATE == 'x15':
            k += (('jv_dot8_2', lambda: dot8(False), 'MUTATED: pass 2\'s products'),)
    return k


def gen_s():
    lines = ['/* GENERATED by bcmfw/video/gen_jdec.py: do not edit.  jdec\'s',
             ' * vector kernel (jdec_vpu.c; algorithms.md section 4). */',
             '', '    .syntax vc2', '    .text', '']
    for name, fn, what in kernels():
        lines += ['/* %s */' % what, '    .global ' + name, name + ':']
        lines += ['    ' + l if not l.endswith(':') else l for l in fn()] + ['']
    lines += ['/* jv_idct4n\'s M: the 1-D islow as a matrix, out_n = sum_j M[n][j] x_j */']
    lines += idct_table()
    return '\n'.join(lines)


def main(argv):
    global MUTATE, UNROLLED
    UNROLLED = '--unrolled' in argv
    if '--mutate' in argv:
        MUTATE = argv[argv.index('--mutate') + 1]
        assert MUTATE in MUTATIONS, 'mutations: ' + ', '.join(MUTATIONS)
    text = prokbox_header.convert(gen_s(), '.S')[0]
    if '--check' in argv:
        if not os.path.exists(OUT_S) or open(OUT_S).read() != text:
            print('gen_jdec: stale: jdec_kernels.S')
            return 1
        print('gen_jdec: up to date')
        return 0
    out_s, = out_paths(argv, [OUT_S], MUTATE, 'gen_jdec')
    write_text(out_s, text)
    if MUTATE:
        print('gen_jdec: MUTATED (%s): %s; in %s' % (MUTATE, MUTATIONS[MUTATE], out_s))
    print('gen_jdec: %s' % ', '.join('%s %d instructions' % (n, len(fn()))
                                     for n, fn, _ in kernels()))
    return 0


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
