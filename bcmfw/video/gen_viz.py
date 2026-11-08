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

"""Generate the visualizer's tables (bcmfw/jobs/jviz_tab.h) and its vector
kernels (bcmfw/video/viz_kernels.S): the spectrum of 512 real samples and
the bars, bit for bit bcmfw/jobs/jviz.c's definitions (jviz.h;
docs/research/bcm2722/visualizer.md).

  jv_vfft512(raw r0, pcm r1, tabs r2)
      pcm: 512 samples in split order (evens, then odds), 32-byte
      aligned; tabs: jviz.h struct jviz_vtabs (jviz_vfft_tabs); raw:
      JVIZ_RAW_HW halfwords, 32-byte aligned: bin k's power's low half at
      k, its high half at 256 + k (natural order).

The FFT (#126, docs/design/viz-fft-vc.md; its host model and proof are
tests/vizfft/vfft_vpu.c, which this emits instruction for instruction):
a 256-point complex FFT of x[2j] + i x[2j+1] as a four-step FFT on one
16 x 16 block (real parts at x 0, imaginary at x 32), then the real
split and |X|^2:
  - window: each sample times the window by one vmulhd;
  - step 1: a 16-point radix-2 DIF down the rows (REP h butterflies over
    whole rows; each b row times W_16^e: a shift, a swap, or four exact
    products a component by immediates, vmul x15 / vmulhd acc), the last
    stage straight to bit-reversed rows (natural order, no permutation);
  - step 2: each element times W_256^(n2 k2), each product floored on its
    own (vmulhd) and the products added in 16 bits (jviz.c's "sep");
  - step 3: one transpose (vmov from V16 columns, REP 16: bench GTHR's
    form) and the same row stages, halved: Z[k] at row k >> 4, lane k & 15;
  - the partner Z[256 - k]: a lane reversal by two transposes and row
    moves, lane 0 one row further by a predicated copy (setf / ifn);
  - the split X = S/2 + T Wr + D Wi + i (U/2 + T Wi - D Wr), each product
    floored on its own; |X|^2 exact in the accumulator (vmul low halves,
    vmulhd high halves, summed by vmov acc/x15, read back).
Every one of its 26 vector forms is filed silicon or apple in
tools/bcmre/proven_forms.txt (viz_test.py checks it): no V16 A operand, no
accumulator op with a vertical operand, no +r.  The image runs a start-up
check (viz_vpu.c viz_vpu_check: three frames against the Mac's goldens
and the C, jobs.h J_VFFT) and keeps the C if it disagrees.
The bars (BARS on the vector unit; viz_vpu.c viz_bars, the C driver):

  jv_bars16(job r0, d r1, rr r2, rows r3, group bit r4)
      one 16-row group (rect rows rr .. rr + 15) of the frame, block by
      block across the rect's 16-pixel column blocks from d (the block
      holding the rect's first pixel; rows 32-byte aligned).  rows: f of
      each of the group's rows (jviz_bars_draw's t * 256 / h), then their
      indices rr + k; job: viz_vpu.c struct jv_bars_job (the pitch, the
      blocks, the lane arrays, their bounds, bg, cap, the gradient's
      channels).

Each pixel is the cap's colour if its row lies in its column's cap rows,
else the gradient's colour of its row if the row is at or below its
column's first bar row (the bar's top, one lower on a rounded bar's edge
columns), else bg on the background's lanes, else kept: jviz_bars_draw's
pixels exactly (its bars never share a pixel and its caps are drawn
last).  The C writes per lane (column) its first bar row and its cap's
rows (0x4000: none), and per block their bounds: the groups that hold a
bar's top or a cap row (bits) and the background's lane mask.  Per
group, the kernel computes the 16 colours (per channel c0 + ((c1 - c0) f
>> 8), packed) and broadcasts them and the row indices down the V16
columns of one row each (GC, RI: row k holds row k's on every lane).
Per block:
  - X = bg on every lane (no load; kept from the block before when that
    was bg alone), or the block loaded and bg on the mask's lanes
    (vbitplanes, vmov ifnz), or loaded, or nothing to draw: skipped;
  - a group with a bar's top in the block: m = (RI - first) >> 15 (-1
    above the bar, 0 in it), X = GC + (GC - X) m; else the lanes whose bar
    covers the whole group (first <= rr: N of first - (rr + 1), setf) take
    GC under ifn;
  - a group with cap rows: m = min(RI - cap first, cap last - RI) >> 15,
    X = cap + (cap - X) m;
  - stored by jv_store16 (single block-0 vst16s).
A frame of 16 to 256 rows, at most VIZ_BARS_CB_MAX blocks wide; the
last group moves up to end on the rect's last row (it overlaps the one
before: every pixel's colour depends only on where it is).

Its forms: device-checked are the loads (vld16 REP 16 with a stride, REP 1
into a row), the stores (jv_store16), vmul D16 A16 by a register (MULR),
vadd D16 A16 B16 (ALUX; at REP 16 with A and B stepping, and vsub so,
Apple's self-test runs them), vbitplanes, vmin/vmax 16-bit (S4); unchecked on
silicon (bench row BARV runs one block through all of them,
bench-p19.md): vadd D16 by a register (ADRG's, first silicon run pending)
and with REP 16; vmul D16 A16 #imm; vasr D16 #8 and #15 (REP 16); the
broadcast, vadd into a V16 D stepping a column per REP from an unstepped
H16 A; vmov D16 from a register, REP 16 under ifnz; vsub into a discarded
D with setf, then vadd D16 #0 REP 16 under ifn; vsub/vrsub with an
unstepped H16 B row at REP 16; vmul, vrsub and vmin with stepped 16-bit B
at REP 16.  The start-up check (viz_vpu.c viz_bars_init, J_VPU_BARS)
compares five frames with the C (two of them flipped: J_BARS_FLIP is the
C driver's addressing, the kernel unchanged) and keeps the C bars if any
differs.

Usage: bcmfw/video/gen_viz.py [--check] [--mutate NAME --out DIR]
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(FW, 'h264'))
from vpu_asm import H16, V16, DISCARD, Reg, alu, bitplanes, mem, out_paths, \
    write_text  # noqa: E402

OUT_S = os.path.join(HERE, 'viz_kernels.S')
OUT_H = os.path.join(FW, 'jobs', 'jviz_tab.h')

HANN_PEAK = 23000       # jviz.h JVIZ_HANN_PEAK
Q = 32767               # twiddles: round(Q cos), round(-Q sin)

MUTATE = None
MUTATIONS = {'lane0': 'jv_vfft512: the partner\'s lane-0 fix skipped',
             'twsign': 'jv_vfft512: step 2, the sine\'s sign flipped',
             'brev': 'jv_vfft512: step 3\'s last stage, bit-reversed rows 1 and 2 swapped',
             'mask': 'jv_bars16: the bar mask 1 for a kept pixel (vlsr for vasr)',
             'grad': 'jv_bars16: the gradient row of the group below (rows 16 off)',
             'cap': 'jv_bars16: a pixel in either of its cap bounds is capped (vmax)'}


def tw16():
    """W_16^e for e < 8: (re, im) as the immediates and jviz_w16"""
    return [(round(Q * math.cos(2 * math.pi * e / 16)),
             round(-Q * math.sin(2 * math.pi * e / 16))) for e in range(8)]


def tables(n):
    m, rows = n // 2, n // 32
    h = [round(HANN_PEAK * 0.5 * (1 - math.cos(2 * math.pi * i / n))) for i in range(n)]
    win = [h[2 * k] for k in range(m)] + [h[2 * k + 1] for k in range(m)]
    tw = [None] * (2 * m)
    ws = [None] * (2 * m)
    for k2 in range(rows):
        for l in range(16):
            a = 2 * math.pi * l * k2 / m
            tw[16 * k2 + l] = round(Q * math.cos(a))
            tw[16 * (rows + k2) + l] = round(-Q * math.sin(a))
            k = k2 + rows * l
            a = 2 * math.pi * k / n
            ws[16 * k2 + l] = round(Q * math.cos(a))
            ws[16 * (rows + k2) + l] = round(-Q * math.sin(a))
    return win, tw, ws


def log16():
    """jviz_l2's mantissa: round(16 log2(1 + f / 16))"""
    return [min(15, round(16 * math.log2(1 + f / 16))) for f in range(16)]


def gen_h():
    out = ['#ifndef BCMFW_JVIZ_TAB_H', '#define BCMFW_JVIZ_TAB_H', '',
           '/* GENERATED by bcmfw/video/gen_viz.py: do not edit.  jviz.c\'s tables',
           ' * (jviz.h struct jviz_tabs): the split-order Hann window (peak %d),' % HANN_PEAK,
           ' * the four-step twiddles W_m^(n1 k2) and the split step\'s W_n^k, as',
           ' * round(%d cos), round(-%d sin); W_16^e; log2\'s mantissa. */' % (Q, Q), '',
           '#define JVIZ_ALIGN __attribute__((aligned(32)))', '']

    def arr(name, vals, t='int16_t'):
        out.append('static const %s %s[%d] JVIZ_ALIGN =' % (t, name, len(vals)))
        out.append('{')
        for i in range(0, len(vals), 12):
            out.append('    ' + ', '.join(str(v) for v in vals[i:i + 12]) + ',')
        out.append('};')
        out.append('')
    arr('jviz_w16', [v for p in tw16() for v in p])
    arr('jviz_log16', log16(), 'uint8_t')
    for n in (256, 512):
        win, tw, ws = tables(n)
        if n == 512:
            out += ['/* not in the PP\'s build (JVIZ_N256_ONLY: its CPU path runs 256 samples) */',
                    '#ifndef JVIZ_N256_ONLY', '']
        arr('jviz_win%d' % n, win)
        arr('jviz_tw%d' % n, tw)
        arr('jviz_ws%d' % n, ws)
    # the vector kernel's (jv_vfft512): the split's W_512^k in natural
    # order (row k >> 4, lane k & 15), and the lane-0 flag row
    win, tw, ws = tables(512)
    nat = [ws[16 * (k & 15) + (k >> 4)] for k in range(256)] + \
          [ws[256 + 16 * (k & 15) + (k >> 4)] for k in range(256)]
    arr('jviz_wsn512', nat)
    arr('jviz_lane0', [-1] + [0] * 15)
    out += ['#endif /* JVIZ_N256_ONLY */', '', '#endif /* BCMFW_JVIZ_TAB_H */', '']
    return '\n'.join(out)


# --------------------------------------------------------------------------
# The FFT kernel, jv_vfft512 (#126; docs/design/viz-fft-vc.md section 6):
# tests/vizfft/vfft_vpu.c's program, instruction for instruction and in
# the same order (VFFT_SEP), so the host model's proof that it equals the
# definition carries over; viz_test.py checks the emitted kernel in
# vc2sim against jviz.c's jviz_raw_sep512 and that every form it uses is
# filed silicon or apple in tools/bcmre/proven_forms.txt.  VRF blocks (16
# rows each; real parts at x 0, imaginary at x 32): B0 rows 0-15, B1
# 16-31, B2 32-47, B3 48-63.
B0, B1, B2, B3 = 0, 16, 32, 48
RE, IM = 0, 32
ZERO = 5                # r5 = 0: the lane-0 flag's B operand
STRIDE = 3              # r3 = 32: a row of 16 halfwords


def brev4(i):
    return (i & 1) << 3 | (i & 2) << 1 | (i & 4) >> 1 | (i & 8) >> 3


def H(y, x, rep=1):
    """an H16 row operand, stepping a row per repetition at REP > 1"""
    return H16(y, x, rep > 1)


def chain_imm(d, a1, w1, a2, w2):
    """d = the floor of (a1 w1 + a2 w2) / 2^16, exactly: low halves by vmul
    x15, high halves by vmulhd acc (silicon X15M; Apple's centre half-pel)"""
    return [alu('vmul', DISCARD, a1, w1, clra=True, x15=True),
            alu('vmul', DISCARD, a2, w2, x15=True),
            alu('vmulhd', DISCARD, a1, w1, acc=True),
            alu('vmulhd', d, a2, w2, acc=True)]


def dif16(x, tmp, y, half):
    """vfft_vpu.c dif16: a 16-point radix-2 DIF down the rows of block x,
    every lane at once; d in block tmp; the last stage into block y's rows
    in bit-reversed order (y in natural order), halved if half"""
    out = []
    h = 8
    while h >= 2:
        for g in range(0, 16, 2 * h):
            a0, b0 = x + g, x + g + h
            for c in (RE, IM):
                out += [alu('vsub', H(tmp, c, h), H(a0, c, h), H(b0, c, h), rep=h),
                        alu('vadd', H(a0, c, h), H(a0, c, h), H(b0, c, h), rep=h),
                        alu('vasr', H(a0, c, h), H(a0, c, h), 1, rep=h)]
            for j in range(h):
                dr, di = H(tmp + j, RE), H(tmp + j, IM)
                b = b0 + j
                e = j * (8 // h)
                if e == 0:
                    out += [alu('vasr', H(b, RE), dr, 1), alu('vasr', H(b, IM), di, 1)]
                elif e == 4:
                    out += [alu('vasr', H(b, RE), di, 1), alu('vrsub', H(b, IM), dr, 0),
                            alu('vasr', H(b, IM), H(b, IM), 1)]
                else:
                    wr, wi = tw16()[e]
                    out += chain_imm(H(b, RE), dr, wr, di, -wi)
                    out += chain_imm(H(b, IM), dr, wi, di, wr)
        h //= 2
    for i in range(0, 16, 2):
        ri, rj = brev4(i), brev4(i + 1)
        if MUTATE == 'brev' and half and ri in (1, 2):
            ri ^= 3
        for c in (RE, IM):
            out += [alu('vadd', H(y + ri, c), H(x + i, c), H(x + i + 1, c)),
                    alu('vsub', H(y + rj, c), H(x + i, c), H(x + i + 1, c))]
    if half:
        for c in (RE, IM):
            out.append(alu('vasr', H(y, c, 16), H(y, c, 16), 1, rep=16))
    return out


def load16(y, x, base):
    """16 rows of 16 halfwords from r(base), 32 bytes apart (REP 16)"""
    return mem('vld16', H16(y, x), base, 0, STRIDE, 16)


def vfft512():
    """jv_vfft512(raw r0, pcm r1, tabs r2): tabs is viz_vpu.c's struct
    jv_vfft_tabs (win, tw, wsn, lane0: jviz_tab.h's 512-sample tables)"""
    out = ['mov r%d, #32' % STRIDE, 'mov r%d, #0' % ZERO]
    # 0. load and window: B0 = the samples, B1 = the window, B0 = B0 B1 >> 16
    out += [load16(B0, RE, 1), 'add r1, #512', load16(B0, IM, 1),
            'ld r4, (r2)', load16(B1, RE, 4), 'add r4, #512', load16(B1, IM, 4)]
    for r in range(16):
        for c in (RE, IM):
            out.append(alu('vmulhd', H(B0 + r, c), H(B0 + r, c), H(B1 + r, c)))
    # 1. step 1: B0 down the rows into B1 (natural order), d in B3
    out += dif16(B0, B3, B1, False)
    # 2. step 2: B1 times W_256^(n2 k2) (B2), each product floored: B0
    out += ['ld r4, (r2+4)', load16(B2, RE, 4), 'add r4, #512', load16(B2, IM, 4)]
    for r in range(16):
        out += [alu('vmulhd', H(B0 + r, RE), H(B1 + r, RE), H(B2 + r, RE)),
                alu('vmulhd', H(B0 + r, IM), H(B1 + r, IM), H(B2 + r, IM)),
                alu('vmulhd', H(B3 + r, RE), H(B1 + r, RE), H(B2 + r, IM)),
                alu('vmulhd', H(B3 + r, IM), H(B1 + r, IM), H(B2 + r, RE))]
    if MUTATE == 'twsign':          # the sine's sign flipped
        out += [alu('vadd', H(B0, RE, 16), H(B0, RE, 16), H(B0, IM, 16), rep=16),
                alu('vsub', H(B0, IM, 16), H(B3, IM, 16), H(B3, RE, 16), rep=16)]
    else:
        out += [alu('vsub', H(B0, RE, 16), H(B0, RE, 16), H(B0, IM, 16), rep=16),
                alu('vadd', H(B0, IM, 16), H(B3, RE, 16), H(B3, IM, 16), rep=16)]
    # 3. the transpose (bench GTHR's form): B1 row n2 = B0 column n2
    out += [alu('vmov', H16(B1, RE, True), None, V16(B0, 0, True), rep=16),
            alu('vmov', H16(B1, IM, True), None, V16(B0, 32, True), rep=16)]
    # 4. step 3: B1 down the rows, halved, into B0: Z[k] at row k >> 4,
    #    lane k & 15
    out += dif16(B1, B3, B0, True)
    # 5. the partner P[a][b] = Z[(256 - 16 a - b) & 255] into B1: a lane
    #    reversal by transposes, rows reversed, lane 0 by a predicated copy
    out += [alu('vmov', H16(B2, RE, True), None, V16(B0, 0, True), rep=16),
            alu('vmov', H16(B2, IM, True), None, V16(B0, 32, True), rep=16)]
    for b in range(16):
        for c in (RE, IM):
            out.append(alu('vadd', H(B3 + b, c), H(B2 + ((16 - b) & 15), c), 0))
    out += [alu('vmov', H16(B1, RE, True), None, V16(B3, 0, True), rep=16),
            alu('vmov', H16(B1, IM, True), None, V16(B3, 32, True), rep=16)]
    for c in (RE, IM):
        out.append(alu('vadd', H(32, c), H(B1, c), 0))
        for a in range(16):
            out.append(alu('vadd', H(33 + a, c), H(B1 + 15 - a, c), 0))
        out.append(alu('vadd', H(B1, c, 16), H(33, c, 16), 0, rep=16))
    out += ['ld r4, (r2+12)', mem('vld16', H16(63, RE), 4),
            alu('vsub', DISCARD, H(63, RE), Reg(ZERO), setf=True)]
    if MUTATE != 'lane0':
        for c in (RE, IM):
            out.append(alu('vadd', H(B1, c, 16), H(32, c, 16), 0, rep=16, pred='ifn'))
    # 6. the split: Z (B0), P (B1), W_512^k (B2, natural order) to X:
    #    Xr = S/2 + T Wr + D Wi in B3 x 0, Xi = U/2 + T Wi - D Wr in B1 x 32
    out += ['ld r4, (r2+8)', load16(B2, RE, 4), 'add r4, #512', load16(B2, IM, 4)]
    R16 = lambda y, c: H(y, c, 16)     # noqa: E731
    out += [alu('vadd', R16(B3, RE), R16(B0, RE), R16(B1, RE), rep=16),
            alu('vsub', R16(B3, IM), R16(B0, RE), R16(B1, RE), rep=16),
            alu('vadd', R16(B1, RE), R16(B0, IM), R16(B1, IM), rep=16),
            alu('vsub', R16(B1, IM), R16(B0, IM), R16(B1, IM), rep=16),
            alu('vasr', R16(B3, RE), R16(B3, RE), 1, rep=16),
            alu('vasr', R16(B1, IM), R16(B1, IM), 1, rep=16)]
    for r in range(16):
        out += [alu('vmulhd', H(B0 + r, RE), H(B1 + r, RE), H(B2 + r, RE)),
                alu('vmulhd', H(B0 + r, IM), H(B3 + r, IM), H(B2 + r, IM))]
    out += [alu('vadd', R16(B3, RE), R16(B3, RE), R16(B0, RE), rep=16),
            alu('vadd', R16(B3, RE), R16(B3, RE), R16(B0, IM), rep=16)]
    for r in range(16):
        out += [alu('vmulhd', H(B0 + r, RE), H(B1 + r, RE), H(B2 + r, IM)),
                alu('vmulhd', H(B0 + r, IM), H(B3 + r, IM), H(B2 + r, RE))]
    out += [alu('vadd', R16(B1, IM), R16(B1, IM), R16(B0, RE), rep=16),
            alu('vsub', R16(B1, IM), R16(B1, IM), R16(B0, IM), rep=16)]
    # 7. the power, exact: low halves by vmul (REP 16), high halves by
    #    vmulhd, summed per row in the accumulator by vmov, read back
    out += [alu('vmul', R16(B0, RE), R16(B3, RE), R16(B3, RE), rep=16),
            alu('vmul', R16(B0, IM), R16(B1, IM), R16(B1, IM), rep=16)]
    for r in range(16):
        out += [alu('vmulhd', H(B2 + r, RE), H(B3 + r, RE), H(B3 + r, RE)),
                alu('vmulhd', H(B2 + r, IM), H(B1 + r, IM), H(B1 + r, IM))]
    for r in range(16):
        out += [alu('vmov', DISCARD, None, H(B2 + r, RE), clra=True, acc=True),
                alu('vmov', DISCARD, None, H(B0 + r, RE), x15=True),
                alu('vmov', DISCARD, None, H(B0 + r, IM), x15=True),
                alu('vmov', DISCARD, None, H(B2 + r, IM), acc=True),
                alu('vmov', H(B0 + r, RE), None, 0, x15=True),
                alu('vmov', H(B0 + r, IM), None, 0, acc=True)]
    # 8. the stores from B0's rows at x 0 (the store form silicon ran)
    out += [mem('vst16', H16(0, 0), 0, 0, STRIDE, 16),
            alu('vadd', R16(B0, RE), R16(B0, IM), 0, rep=16),
            'add r0, #512',
            mem('vst16', H16(0, 0), 0, 0, STRIDE, 16),
            'rts']
    return out


def bars_cb_max():
    """viz_vpu.h VIZ_BARS_CB_MAX: the lane arrays' length in blocks"""
    with open(os.path.join(HERE, 'viz_vpu.h')) as f:
        for line in f:
            w = line.split()
            if len(w) >= 3 and w[:2] == ['#define', 'VIZ_BARS_CB_MAX']:
                return int(w[2])
    raise ValueError('viz_vpu.h: no VIZ_BARS_CB_MAX')


def bars16():
    """jv_bars16(job r0, d r1, rr r2, rows r3, group bit r4): one 16-row
    group of BARS (viz_vpu.c viz_bars), its column blocks from d on.

    VRF: X (rows 0-15 x 0) the block, stored by jv_store16; T (x 32) and M
    (rows 16-31 x 0) temporaries; GC (rows 16-31 x 32) the gradient, row k
    the colour of the group's row k on every lane; RI (rows 32-47 x 0) its
    index in the rect, rr + k; rows 48-55 x 0 the block's first bar rows,
    first and last cap rows, the group's f and indices, the gradient's
    channels.

    Registers in the block loop: r0 scratch (jv_store16's base), r1 the
    pitch (bytes), r2 the lane arrays at this block, r3 bg, r4 cap, r5 the
    background's lane mask, r6 the block, r7 the bars' top rows in this
    group, r8 the blocks left, r9 the block's bounds (viz_vpu.c struct
    jv_bars_job desc), r10 rr, r11 1 while X holds bg on every lane, r12
    scratch, r13 the group bit, r14 the caps in this group"""
    X, T, M, GC, RI = H16(0, 0), H16(0, 32), H16(16, 0), H16(16, 32), H16(32, 0)
    BST, CS, CE, F, RROW, CR, CG, CB = (H16(y, 0) for y in range(48, 56))
    s = lambda o: o.stepped()       # noqa: E731
    ls = 32 * bars_cb_max()
    lb = 'jv_bars16_'
    out = ['push r6-r14, lr', 'mov r13, r4',
           mem('vld16', F, 3, 64 if MUTATE == 'grad' else 0),
           mem('vld16', RROW, 3, 32)]
    # the gradient (jviz.c jviz_mix565): per channel c0 + ((c1 - c0) f >> 8),
    # f of each row from the C, the channels' ends from the job
    for d, off, scale in ((CR, 24, 2048), (CG, 32, 32), (CB, 40, 0)):
        out += ['ld r4, (r0+%d)' % (off + 4), alu('vmul', d, F, Reg(4)),
                alu('vasr', d, d, 8), 'ld r4, (r0+%d)' % off, alu('vadd', d, d, Reg(4))]
        if scale:
            out.append(alu('vmul', d, d, scale))
    out += [alu('vadd', CR, CR, CG), alu('vadd', CR, CR, CB),
            # GC: row k of every column the colour of the group's row k; RI
            # its index rr + k (V16 columns of one row: the broadcast)
            alu('vadd', V16(16, 32, True), CR, 0, rep=16),
            alu('vadd', V16(32, 0, True), RROW, 0, rep=16),
            'mov r10, r2', 'mov r6, r1', 'mov r11, #0',
            'ld r8, (r0+4)', 'ld r2, (r0+8)', 'ld r9, (r0+12)', 'ld r3, (r0+16)',
            'ld r4, (r0+20)', 'ld r1, (r0)',
            lb + 'c:',
            'ld r5, (r9+12)',
            'ld r7, (r9+4)', 'and r7, r13',
            'ld r14, (r9+8)', 'and r14, r13',
            # X: bg on every lane (no load; kept from the block before if it
            # was that), or loaded and bg on the mask's lanes, or loaded (the
            # pixels kept where nothing is drawn), or nothing to do
            'mov r12, r5', 'lsr r12, #16', 'cmp r12, #0', 'beq %sp' % lb,
            'cmp r11, #0', 'bne %sw' % lb,
            alu('vmov', s(X), None, Reg(3), rep=16),
            'mov r11, #1',
            'b %sw' % lb,
            lb + 'p:',
            'mov r11, #0',
            'cmp r5, #0', 'bne %spb' % lb,
            'ld r12, (r9)', 'cmp r12, r10', 'ble %spl' % lb,
            'mov r12, r7', 'or r12, r14', 'cmp r12, #0', 'beq %sn' % lb,
            lb + 'pl:',
            mem('vld16', s(X), 6, 0, 1, 16),
            'b %sw' % lb,
            lb + 'pb:',
            mem('vld16', s(X), 6, 0, 1, 16),
            bitplanes(Reg(5)),
            alu('vmov', s(X), None, Reg(3), rep=16, pred='ifnz'),
            lb + 'w:',
            # the bars.  A top in this group: m = (r - first) >> 15, -1
            # above the bar, 0 in it; X = GC + (GC - X) m.  Else the lanes
            # whose bar covers the group (first <= rr) take GC
            'cmp r7, #0', 'bne %sbp' % lb,
            'ld r12, (r9)', 'cmp r12, r10', 'bgt %swc' % lb,
            'mov r11, #0',
            mem('vld16', BST, 2, 0),
            'mov r0, r10', 'add r0, #1',
            alu('vsub', DISCARD, BST, Reg(0), setf=True),
            alu('vadd', s(X), s(GC), 0, rep=16, pred='ifn'),
            'b %swc' % lb,
            lb + 'bp:',
            'mov r11, #0',
            mem('vld16', BST, 2, 0),
            alu('vsub', s(M), s(RI), BST, rep=16),
            alu('vlsr' if MUTATE == 'mask' else 'vasr', s(M), s(M), 15, rep=16),
            alu('vsub', s(T), s(GC), s(X), rep=16),
            alu('vmul', s(T), s(T), s(M), rep=16),
            alu('vadd', s(X), s(GC), s(T), rep=16),
            lb + 'wc:',
            # the caps: m = min(r - first, last - r) >> 15, 0 in the cap
            'cmp r14, #0', 'beq %ss' % lb,
            'mov r11, #0',
            mem('vld16', CS, 2, ls), mem('vld16', CE, 2, 2 * ls),
            alu('vsub', s(M), s(RI), CS, rep=16),
            alu('vrsub', s(T), s(RI), CE, rep=16),
            alu('vmax' if MUTATE == 'cap' else 'vmin', s(M), s(M), s(T), rep=16),
            alu('vasr', s(M), s(M), 15, rep=16),
            alu('vrsub', s(T), s(X), Reg(4), rep=16),
            alu('vmul', s(T), s(T), s(M), rep=16),
            alu('vadd', s(X), s(T), Reg(4), rep=16),
            lb + 's:',
            'mov r0, r6', 'bl jv_store16',
            lb + 'n:',
            'add r6, #32', 'add r2, #32', 'add r9, #16',
            'add r8, #-1', 'cmp r8, #0', 'bne %sc' % lb,
            'pop r6-r14, pc']
    return out


def kernels():
    return (('jv_vfft512', vfft512, 'the raw powers of 512 samples (jviz.h '
             'jviz_raw_sep512; docs/design/viz-fft-vc.md)'),
            ('jv_bars16', bars16, 'one 16-row group of BARS (viz_vpu.c viz_bars)'))


def gen_s():
    lines = ['/* GENERATED by bcmfw/video/gen_viz.py: do not edit.  The visualizer\'s',
             ' * vector kernel (viz_vpu.c; jviz.h, visualizer.md). */',
             '', '    .syntax vc2', '    .text', '']
    for name, fn, what in kernels():
        # the vector FFT is left out where its 7.7 KB doesn't fit (the
        # display-only image's overlay: build.py build_overlay)
        vfft = name == 'jv_vfft512'
        lines += (['#ifndef JVIZ_NO_VFFT'] if vfft else []) + \
            ['/* %s */' % what, '    .global ' + name, name + ':']
        lines += ['    ' + l if not l.endswith(':') else l for l in fn()] + \
            (['#endif', ''] if vfft else [''])
    return '\n'.join(lines)


def main(argv):
    global MUTATE
    if '--mutate' in argv:
        MUTATE = argv[argv.index('--mutate') + 1]
        assert MUTATE in MUTATIONS, 'mutations: ' + ', '.join(MUTATIONS)
    s_text = prokbox_header.convert(gen_s(), '.S')[0]
    h_text = prokbox_header.convert(gen_h(), '.h')[0]
    if '--check' in argv:
        bad = [p for p, t in ((OUT_S, s_text), (OUT_H, h_text))
               if not os.path.exists(p) or open(p).read() != t]
        for p in bad:
            print('gen_viz: stale: %s' % os.path.basename(p))
        if not bad:
            print('gen_viz: up to date')
        return 1 if bad else 0
    out_s, out_h = out_paths(argv, [OUT_S, OUT_H], MUTATE, 'gen_viz')
    write_text(out_s, s_text)
    write_text(out_h, h_text)
    if MUTATE:
        print('gen_viz: MUTATED (%s): %s; in %s' % (MUTATE, MUTATIONS[MUTATE], out_s))
    print('gen_viz: %s' % ', '.join('%s %d instructions' % (n, len(fn()))
                                    for n, fn, _ in kernels()))
    return 0


sys.path.insert(0, os.path.join(FW, '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
