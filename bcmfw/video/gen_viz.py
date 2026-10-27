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
kernels (bcmfw/video/viz_kernels.S): the spectrum of 256 or 512 real
samples and the bars, bit for bit bcmfw/jobs/jviz.c's reference (jviz.h;
docs/research/bcm2722/visualizer.md).

  jv_fft256(raw r0, pcm r1, tabs r2), jv_fft512(raw r0, pcm r1, tabs r2)
      pcm: n samples in split order (evens, then odds), 32-byte aligned;
      tabs: the size's struct jviz_tabs (win, tw, ws); raw: JVIZ_RAW_HW
      halfwords, 32-byte aligned: the powers' low halves (16 rows of 16),
      then their high halves; bin k = k2 + rows k1 at row k2, lane k1.

The FFT (m = n / 2 complex points, m = rows x 16, rows 8 or 16):
  - Load and window: H16 rows 0.. (block 0): z[16 r + l] = x[2 (16 r + l)]
    + i x[2 (16 r + l) + 1] at row r, lane l (real parts at x 0, imaginary
    at x 32), each (pcm * win) >> 16 by one vmulhd.
  - Step 1, down the rows (radix-2 DIF over the rows; the 16 lanes are 16
    independent transforms): per stage, REP h vsub/vadd/vasr over whole
    rows, then each b row times its scalar twiddle W_16^e: e = 0 a shift,
    e = 4 (-i) a swap, else four exact products a component (vmul x15 for
    the low halves, vmulhd acc for the high: one floor of the exact sum).
    The last stage (h = 1) is unscaled, into block 1 in natural order.
  - Step 2 (jv_fft_tw): every element times W_m^(n1 k2), Q15, floored by
    2^16 (the last stage's halving), block 1 to block 0.
  - Step 3 (jv_fft_cols), along the lanes: the same stages over V16
    columns of block 0 (the transpose is free: H16 rows and V16 columns
    alias), the last into block 1's columns in natural order; then the
    partners: block 2 = block 1 with its columns reversed (Z[m - k] for
    rows past 0), block 3 the same rotated one column (row 0's).
  - The split (jv_fft_split), a row at a time: S, D, T, U of Z and its
    partner, X = (S 2^15 + Wr T + Wi D) / 2^16 + i (U 2^15 + Wi T - Wr D)
    / 2^16, then |X|^2 exactly (its halves to block 0).
Every add stays in 16 bits for a window peak below JVIZ_HANN_PEAK (the
complex samples stay under 2^14).

Forms (none checked on silicon; the IDCT's, gen_jdec.py, plus): x15 and
vmulhd acc chains with immediate and fixed-row B operands; vshl #15 x15
and vasr #1 acc (a 16-bit value times 2^15 into the accumulator); V16 A,
B and D operands stepping per REP in vadd/vsub/vasr; vrsub #0; vld16 into
rows past 15 and x 32.  The image runs a start-up check against the C
(viz_vpu.c) and keeps the C if it disagrees.

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
MUTATIONS = {'sign': 'step 3, stage 1: W_16^1\'s imaginary part with the wrong sign',
             'scale': 'step 1, stage 1: the a rows\' real parts not halved',
             'lane': 'the partner block: column 3 taken from column 11 (for 12)',
             'mask': 'jv_bars16: the bar mask 1 for a kept pixel (vlsr for vasr)',
             'grad': 'jv_bars16: the gradient row of the group below (rows 16 off)',
             'cap': 'jv_bars16: a pixel in either of its cap bounds is capped (vmax)',
             'tw1': 'jv_fft_tw: each component\'s two products in one chain (the form '
                    'before: past 2^31 it saturates on silicon)',
             'acc1': 'every sum in one chain: the kernel before the saturating '
                     'accumulator (quirks #31, #32), byte for byte'}


def one_chain(kind):
    """the mutation keeps this chain's one-chain form (the kernel before)"""
    return MUTATE == kind or MUTATE == 'acc1'


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
    out += ['#endif /* JVIZ_N256_ONLY */', '', '#endif /* BCMFW_JVIZ_TAB_H */', '']
    return '\n'.join(out)


# --------------------------------------------------------------------------
# The kernel.  VRF blocks (16 rows each; real parts at x 0, imaginary at
# x 32): B0 rows 0-15, B1 16-31, B2 32-47, B3 48-63.
B0, B1, B2, B3 = 0, 16, 32, 48
# B3's fixed rows (H16 at x 0): the split's and step 2's temporaries; row
# 48 is block 3's row 0 (the partner of row 0), rows 48-55 step 1's d
TMP = {'pr': 49, 'pi': 50, 'wr': 51, 'wi': 52, 'nwr': 53, 's': 54, 'd': 55,
       't': 56, 'u': 57, 'xr': 58, 'xi': 59, 'tr': 60, 'ti': 61, 'nti': 62}
# a split sum's first chain (chain()): its low half (row 63 x 0) and high
# half (row 63 x 32); B3's rows past 48 are free in steps 2 and the split
CL, CH = H16(63, 0), H16(63, 32)


def T(name):
    return H16(TMP[name], 0)


def vec(orient, y0, idx, comp, step=False, reg=None):
    """Element idx of a transform's sequence: a row (H16) or a column
    (V16) of the block at y0; comp 0 real, 1 imaginary"""
    if orient == 'row':
        return H16(y0 + idx, 32 * comp, step, reg)
    return V16(y0, idx + 32 * comp, step, reg)


def brev(i, bits):
    r = 0
    for _ in range(bits):
        r, i = r << 1 | (i & 1), i >> 1
    return r


def twiddle(o, bdst, d, e, stage1=False):
    """b = d * W_16^e / 2 (jviz.c twiddle); d: (re, im) operands"""
    br, bi = bdst
    dr, di = d
    if e == 0:
        return [alu('vasr', br, dr, 1), alu('vasr', bi, di, 1)]
    if e == 4:
        return [alu('vasr', br, di, 1), alu('vrsub', bi, dr, 0), alu('vasr', bi, bi, 1)]
    wr, wi = tw16()[e]
    if MUTATE == 'sign' and stage1 and e == 1:
        wi = -wi
    return [alu('vmul', DISCARD, dr, wr, clra=True, x15=True),
            alu('vmul', DISCARD, di, -wi, x15=True),
            alu('vmulhd', DISCARD, dr, wr, acc=True),
            alu('vmulhd', br, di, -wi, acc=True),
            alu('vmul', DISCARD, dr, wi, clra=True, x15=True),
            alu('vmul', DISCARD, di, wr, x15=True),
            alu('vmulhd', DISCARD, dr, wi, acc=True),
            alu('vmulhd', bi, di, wr, acc=True)]


def chain(dst, first, second, lo_dst=DISCARD, one=False):
    """dst = the high half of the sum of first's and second's terms, exact
    mod 2^32 as jviz.c's: each term (lo op, its B, hi op, its B, A) adds
    its low 16 bits by an x15 op and its high half by an acc op.  Silicon's
    accumulator saturates past +-2^31 (quirks #31, #32; vc2sim
    v_acc_sat), where C's sum wraps, so the terms are summed as two chains,
    each under 2^31 for any 16-bit operands: the first gives its low half
    CL and high half CH; the second starts from CL alone (clra x15:
    zero-extended), adds its terms and gives the sum's low half (lo_dst:
    the power's) and its high half less CH; CH is then added in wrapping
    16-bit lanes (gen_jdec.py's jv_dot8 does the same).  one: the form
    before, one chain (the mutations).

    Which sums can pass 2^31 in one chain (jviz.c's wrap them):
      - step 2's (jv_fft_tw), with tables past |W| <= 1: y = (-32768,
        -32768) times tr = -32768 and -ti = -32768 is 2^31 exactly; the
        start-up check feeds it that (viz_vpu.c).  With the shipped
        tables (|wr| + |wi| <= 46,340) no input reaches it;
      - the split's: bounded by step 3's last halving (Z in -16,384 ..
        16,383, so |S| + |D| <= 32,768 and |T| <= 32,768): S 2^15 + T Wr
        + D Wi stays within +-(2^31 - 32,768) (a search over the corners
        and 2 M random points near them agrees);
      - the power's: only X = (-32768, -32768) reaches 2^31, and no Z or
        W gives both (the same search);
      - steps 1 and 3's twiddles: immediates, at most 32,768 x 46,340.
    The split and the power go in two chains anyway (6 ops a row), so no
    bound rests on another step's scaling."""
    def run(terms, clra, lo_last, hi_last):
        out = []
        for k, (lo, blo, hi, bhi, a) in enumerate(terms):
            out.append(alu(lo, lo_last if k == len(terms) - 1 else DISCARD, a, blo,
                           clra=clra and not k, x15=True))
        for k, (lo, blo, hi, bhi, a) in enumerate(terms):
            out.append(alu(hi, hi_last if k == len(terms) - 1 else DISCARD, a, bhi,
                           acc=True))
        return out
    if one:
        return run(first + second, True, lo_dst, dst)
    return (run(first, True, CL, CH) + [alu('vmov', DISCARD, None, CL, clra=True, x15=True)] +
            run(second, False, lo_dst, dst) + [alu('vadd', dst, dst, CH)])


def prod(a, b):
    """the term a * b: vmul x15 (its low half), vmulhd acc (its high)"""
    return ('vmul', b, 'vmulhd', b, a)


def shl15(a):
    """the term a * 2^15: vshl #15 x15 (its low half), vasr #1 acc (high)"""
    return ('vshl', 15, 'vasr', 1, a)


def stages(o, y0, L):
    """jviz.c stages(): radix-2 DIF over L elements, h = L / 2 .. 2"""
    out = []
    h = L // 2
    while h >= 2:
        for g in range(0, L, 2 * h):
            for c in (0, 1):
                a = vec(o, y0, g, c, step=True)
                b = vec(o, y0, g + h, c, step=True)
                t = vec(o, B3, 0, c, step=True)
                out += [alu('vsub', t, a, b, rep=h), alu('vadd', a, a, b, rep=h)]
                if not (MUTATE == 'scale' and o == 'row' and h == L // 2 and c == 0):
                    out.append(alu('vasr', a, a, 1, rep=h))
            for j in range(h):
                out += twiddle(o, (vec(o, y0, g + j + h, 0), vec(o, y0, g + j + h, 1)),
                               (vec(o, B3, j, 0), vec(o, B3, j, 1)), j * (16 // (2 * h)),
                               stage1=(o == 'col' and h == 8))
        h //= 2
    return out


def fft_n(n):
    """jv_fft<n>: the size's own code (loads, step 1), then the shared
    steps"""
    rows = n // 32
    bits = 4 if rows == 16 else 3
    out = ['push r6-r13, lr', 'mov r8, r0', 'mov r9, r2', 'mov r3, #32',
           mem('vld16', H16(B0, 0), 1, 0, 3, rows),
           'add r1, #%d' % (32 * rows),
           mem('vld16', H16(B0, 32), 1, 0, 3, rows),
           'ld r4, (r9)',
           mem('vld16', H16(B1, 0), 4, 0, 3, rows),
           'add r4, #%d' % (32 * rows),
           mem('vld16', H16(B1, 32), 4, 0, 3, rows),
           alu('vmulhd', H16(B0, 0, True), H16(B0, 0, True), H16(B1, 0, True), rep=rows),
           alu('vmulhd', H16(B0, 32, True), H16(B0, 32, True), H16(B1, 32, True), rep=rows),
           'ld r4, (r9+4)',
           mem('vld16', H16(B2, 0), 4, 0, 3, rows),
           'add r4, #%d' % (32 * rows),
           mem('vld16', H16(B2, 32), 4, 0, 3, rows)]
    out += stages('row', B0, rows)
    # the last stage, unscaled, to B1 in natural order
    for i in range(0, rows, 2):
        for c in (0, 1):
            a, b = H16(B0 + i, 32 * c), H16(B0 + i + 1, 32 * c)
            out += [alu('vadd', H16(B1 + brev(i, bits), 32 * c), a, b),
                    alu('vsub', H16(B1 + brev(i + 1, bits), 32 * c), a, b)]
    out += ['mov r10, #%d' % (64 * rows), 'bl jv_fft_tw', 'bl jv_fft_cols',
            'ld r4, (r9+8)',
            mem('vld16', H16(B0, 0), 4, 0, 3, rows),
            'add r4, #%d' % (32 * rows),
            mem('vld16', H16(B0, 32), 4, 0, 3, rows),
            'bl jv_fft_split',
            'mov r0, r8', 'mov r1, #32', 'bl jv_store16',
            alu('vadd', H16(0, 0, True), H16(0, 32, True), 0, rep=16),
            'mov r0, r8', 'add r0, #512', 'mov r1, #32', 'bl jv_store16',
            'pop r6-r13, pc']
    return out


def fft_tw():
    """jv_fft_tw (r10 = rows * 64): B0 row = B1 row * B2 row (W_m^(n1 k2)),
    each component one exact sum floored, its two products in two chains
    (chain(): any 16-bit operands, the tables' included, stay exact)"""
    a = lambda y, c: H16(y, 32 * c, True, 4)        # noqa: E731
    one = one_chain('tw1')
    return (['mov r4, #0', 'jv_fft_tw_l:',
             alu('vadd', T('tr'), a(B2, 0), 0),
             alu('vadd', T('ti'), a(B2, 1), 0),
             alu('vrsub', T('nti'), a(B2, 1), 0)] +
            chain(H16(B0, 0, reg=4), [prod(a(B1, 0), T('tr'))], [prod(a(B1, 1), T('nti'))],
                  one=one) +
            chain(H16(B0, 32, reg=4), [prod(a(B1, 0), T('ti'))], [prod(a(B1, 1), T('tr'))],
                  one=one) +
            ['add r4, #64', 'cmp r4, r10', 'bne jv_fft_tw_l', 'rts'])


def fft_cols():
    """jv_fft_cols: step 3 over B0's columns, the last stage into B1's
    columns in natural order; the partners: B2 = B1's columns reversed,
    B3 = B2's rotated one column on"""
    out = stages('col', B0, 16)
    for i in range(0, 16, 2):
        for c in (0, 1):
            a, b = V16(B0, i + 32 * c), V16(B0, i + 1 + 32 * c)
            p, q = V16(B1, brev(i, 4) + 32 * c), V16(B1, brev(i + 1, 4) + 32 * c)
            out += [alu('vadd', p, a, b), alu('vasr', p, p, 1),
                    alu('vsub', q, a, b), alu('vasr', q, q, 1)]
    for c in (0, 1):
        for k in range(16):
            src = 15 - k
            if MUTATE == 'lane' and k == 3 and c == 0:
                src = 11
            out.append(alu('vadd', V16(B2, k + 32 * c), V16(B1, src + 32 * c), 0))
        # B3 column k = B2 column k - 1, column 0 = B2's 15
        k = 1
        for rep in (8, 4, 2, 1):
            out.append(alu('vadd', V16(B3, k + 32 * c, True), V16(B2, k - 1 + 32 * c, True), 0,
                           rep=rep))
            k += rep
        out.append(alu('vadd', V16(B3, 32 * c), V16(B2, 15 + 32 * c), 0))
    return out + ['rts']


def fft_split():
    """jv_fft_split (r10 = rows * 64): per row k2, Z (B1 + r4) and its
    partner (B2 + r5: row rows - k2, or B3's row 0 for k2 = 0), the split
    twiddles (B0 + r4), to |X|^2's halves in B0's row (x 0 low, x 32
    high).  X's three terms and |X|^2's two squares each in two chains
    (chain()), exact for any 16-bit operands"""
    a = lambda y, c, r: H16(y, 32 * c, True, r)      # noqa: E731
    out = ['mov r4, #0', 'mov r5, #%d' % (64 * (B3 - B2)), 'jv_fft_split_l:',
           alu('vadd', T('pr'), a(B2, 0, 5), 0),
           alu('vadd', T('pi'), a(B2, 1, 5), 0),
           alu('vadd', T('wr'), a(B0, 0, 4), 0),
           alu('vadd', T('wi'), a(B0, 1, 4), 0),
           alu('vrsub', T('nwr'), a(B0, 0, 4), 0),
           alu('vadd', T('s'), a(B1, 0, 4), T('pr')),
           alu('vsub', T('d'), a(B1, 0, 4), T('pr')),
           alu('vadd', T('t'), a(B1, 1, 4), T('pi')),
           alu('vsub', T('u'), a(B1, 1, 4), T('pi'))]
    for x, base, p1, p2 in (('xr', 's', ('t', 'wr'), ('d', 'wi')),
                            ('xi', 'u', ('t', 'wi'), ('d', 'nwr'))):
        out += chain(T(x), [shl15(T(base)), prod(T(p1[0]), T(p1[1]))],
                     [prod(T(p2[0]), T(p2[1]))], one=MUTATE == 'acc1')
    out += chain(H16(B0, 32, reg=4), [prod(T('xr'), T('xr'))], [prod(T('xi'), T('xi'))],
                 lo_dst=H16(B0, 0, reg=4), one=MUTATE == 'acc1')
    return out + ['add r4, #64', 'mov r5, r10', 'sub r5, r4',
                  'cmp r4, r10', 'bne jv_fft_split_l', 'rts']


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
    return (('jv_fft256', lambda: fft_n(256), 'the spectrum of 256 samples (jviz.h)'),
            ('jv_fft512', lambda: fft_n(512), 'the spectrum of 512 samples (jviz.h)'),
            ('jv_fft_tw', fft_tw, 'step 2: B0 = B1 * W_m^(n1 k2)'),
            ('jv_fft_cols', fft_cols, 'step 3 along the lanes, then the partners'),
            ('jv_fft_split', fft_split, 'the real FFT\'s split step and |X|^2'),
            ('jv_bars16', bars16, 'one 16-row group of BARS (viz_vpu.c viz_bars)'))


def gen_s():
    lines = ['/* GENERATED by bcmfw/video/gen_viz.py: do not edit.  The visualizer\'s',
             ' * vector kernel (viz_vpu.c; jviz.h, visualizer.md). */',
             '', '    .syntax vc2', '    .text', '']
    for name, fn, what in kernels():
        lines += ['/* %s */' % what, '    .global ' + name, name + ':']
        lines += ['    ' + l if not l.endswith(':') else l for l in fn()] + ['']
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
