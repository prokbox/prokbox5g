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
"""Generate the H.264 decoder's VideoCore II vector kernels.

Writes vpu_kernels.S (native VC II assembly) and vpu_kernels.h (their
prototypes and dispatch tables).  The kernels are our own, written from
ITU-T H.264 (8.4.2.2 interpolation, 8.5.12 transform, 8.7 loop filter);
decoder.md, "Vector kernels", has the data layouts.  Every vector
instruction goes through vpu_asm.py, which checks its operands against
the simulator's decoding.

Stores: a 48-bit vst8/vst16 only from d00 (device session 4: from any
other row it did not land; isa.md, "Vector stores on silicon"), so each
such kernel computes its last result straight into rows 0..; masked
stores use the 80-bit vstr (vc_storeback's form, source in A).  No
48-bit ALU immediates (their imm6 is signed): every immediate is 80-bit.

  bcmfw/h264/gen_vpu.py            regenerate
  bcmfw/h264/gen_vpu.py --check    fail if the checked-in files are stale

Register use: arguments in r0-r5 (the ABI), r5 as scratch when free;
r6/r7 are saved where used.  Every kernel is a leaf.  Where it costs at
most a scalar move, a kernel uses an operation form that Apple's kernels
run in the vc2sim self-test (multiply by a register rather than by an
immediate or a shift, compares against 8-bit VRF rows): test/vpu_forms.py
lists the forms that remain unexercised.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from vpu_asm import (H8, H16, V8, V16, DISCARD, Reg, alu, mem, vstr,  # noqa: E402
                     bitplanes)

CHUNKS = (16, 8, 4, 2, 1)


def chunks(n):
    out, k = [], 0
    for c in CHUNKS:
        while n - k >= c:
            out.append((k, c))
            k += c
    return out


def at(o, k):
    """Operand o moved k rows (H) or columns (V) on, if it steps."""
    if o is None or isinstance(o, (int, Reg)) or o == DISCARD or not o.step:
        return o
    if o.vert:
        return type(o)(o.kind, o.y, o.x + k, o.step, o.reg)
    return type(o)(o.kind, o.y + k, o.x, o.step, o.reg)


class K:
    """One kernel's instruction list."""
    def __init__(self, name, comment):
        self.name, self.comment, self.lines = name, comment, []

    def s(self, text):                      # scalar instruction
        self.lines.append(text)

    def label(self, l):
        self.lines.append(l + ':')

    def v(self, op, d, a, b, n=1, **kw):
        """Vector ALU op over n rows: split into REP 16/8/4/2/1 pieces."""
        for k, c in chunks(n):
            self.lines.append(alu(op, at(d, k), at(a, k), at(b, k), rep=c, **kw))

    def text(self):
        out = ['', '/* %s */' % self.comment, '    .global %s' % self.name,
               '%s:' % self.name]
        for l in self.lines:
            out.append(l if l.endswith(':') else '    ' + l)
        return out


# ---------------------------------------------------------------------------
# Luma interpolation (8.4.2.2.1).
#
# Arguments: r0 dst, r1 dst stride, r2 src (the integer sample), r3 src
# stride, r4 lane mask (w = 16, 8, 4: 0xffff, 0x00ff, 0x000f).  Every
# position computes 16 lanes; the mask stores w of them.
#
# VRF layout (rows y, columns x):
#   DATA  H8 rows 0..R-1, x 32-63: the source rows, from x-2 (c0 = 34) or
#         from x (c0 = 32); row 2 + r is output row r when the rows above
#         were loaded (R = h + 5), row r otherwise (R = h).
#   TA/TB 16-bit H rows 21-36 / 37-52 at x 32 (two temporaries)
#   P1/P2 8-bit H rows 21-36 / 37-52 at x 0 (planes to average or store)
#   centre j: S (unrounded horizontal sums b1) 16-bit rows 0-20 at x 0,
#         HI rows 21-41, LO rows 42-62 (S = 64 HI + LO), AUX 8-bit rows
#         0-15 at x 32 (over DATA, once it is consumed), J 8-bit rows
#         42-57 at x 0 (over LO).

TA, TB = H16(21, 32, True), H16(37, 32, True)
P1, P2 = H8(21, 0, True), H8(37, 0, True)


def hsum16(k, dst, tmp, row0, n, col0):
    """dst = unrounded horizontal 6-tap (b1) of DATA rows row0..row0+n-1,
    taps at columns col0 + i .. col0 + i + 5 (col0 = 32: x - 2)."""
    assert col0 == 32
    a = lambda c: H8(row0, c, True)              # noqa: E731
    # a horizontal B operand's +rN x is aligned down to 16 on silicon
    # (device session 22), so every tap at x 33-37 is an A operand (any
    # x) and B is the aligned 16-bit sum so far
    k.v('vadd', dst, a(col0 + 1), 0, n)
    k.v('vadd', dst, a(col0 + 4), dst, n)                        # U = E1 + E4
    k.v('vadd', tmp, a(col0 + 2), 0, n)
    k.v('vadd', tmp, a(col0 + 3), tmp, n)                        # V = E2 + E3
    k.v('vmul', tmp, tmp, Reg(2), n)                            # x 4 (r2)
    k.v('vsub', tmp, tmp, dst, n)                                # 4V - U
    k.v('vadd', dst, a(col0 + 5), H8(row0, 32, True), n)         # T = E0 + E5
    k.v('vmul', tmp, tmp, Reg(3), n)                            # x 5 (r3)
    k.v('vadd', dst, dst, tmp, n)                                # T + 5 (4V - U)


def vsum16(k, dst, tmp, row0, n, col):
    """dst = unrounded vertical 6-tap of DATA column col: output row r
    uses DATA rows row0 + r .. row0 + r + 5."""
    a = lambda dy: H8(row0 + dy, col, True)      # noqa: E731
    # column col is misaligned for a B operand (its +rN x is aligned down
    # to 16 on silicon, device session 22): every tap is an A operand and
    # B the aligned 16-bit sum so far
    k.v('vadd', dst, a(1), 0, n)
    k.v('vadd', dst, a(4), dst, n)
    k.v('vadd', tmp, a(2), 0, n)
    k.v('vadd', tmp, a(3), tmp, n)
    k.v('vmul', tmp, tmp, Reg(2), n)                            # x 4 (r2)
    k.v('vsub', tmp, tmp, dst, n)
    k.v('vadd', dst, a(0), 0, n)
    k.v('vadd', dst, a(5), dst, n)
    k.v('vmul', tmp, tmp, Reg(3), n)                            # x 5 (r3)
    k.v('vadd', dst, dst, tmp, n)


def vsum16_16(k, dst, tmp, src, n):
    """dst = vertical 6-tap of 16-bit rows src (x 0): output row r uses
    src rows r .. r + 5."""
    s = lambda dy: H16(src.y + dy, src.x, True)  # noqa: E731
    k.v('vadd', dst, s(1), s(4), n)
    k.v('vadd', tmp, s(2), s(3), n)
    k.v('vmul', tmp, tmp, Reg(2), n)                            # x 4 (r2)
    k.v('vsub', tmp, tmp, dst, n)
    k.v('vadd', dst, s(0), s(5), n)
    k.v('vmul', tmp, tmp, Reg(3), n)                            # x 5 (r3)
    k.v('vadd', dst, dst, tmp, n)


# An 8-bit row of zeros for the clips below, at a VRF place no luma or
# plane kernel uses.  On silicon vmulhrs into an 8-bit D keeps the low 8
# bits, it does not clamp (device session 5, isa.md MHRN); vadds D8 A8 B16
# does clamp to 0..255 (ADSN, device-verified), so a clip is the rounded
# value in 16 bits, then a saturating add to zero.
CLIP_ZERO8 = H8(63, 32)


def clip_zero(k, reg):
    """CLIP_ZERO8 = 0, through register reg (which it overwrites)."""
    k.s('mov r%d, #0' % reg)
    k.v('vmov', CLIP_ZERO8, None, Reg(reg))


def round8(k, dst8, src16, n, tmp16=None):
    """dst8 = clip((src16 + 16) >> 5): vmulhr by 2048 into 16 bits (tmp16,
    or src16 in place), then vadds to CLIP_ZERO8 (clip_zero first)."""
    t = tmp16 or src16
    k.v('vmulhr', t, src16, 0x0800, n)
    k.v('vadds', dst8, CLIP_ZERO8, t, n)


def avg(k, dst8, a8, b8, tmp16, n):
    """dst8 = (a8 + b8 + 1) >> 1 (b8 at a 16-column boundary)."""
    k.v('vadd', tmp16, a8, b8, n)
    k.v('vmulhrs', dst8, tmp16, 0x8000, n)


def load_rows(k, row0, n, halves):
    """DATA rows row0.. from r2 (+= r3 per row), 16 bytes per half at
    x 32 (and 48); vldp + vldq handle any alignment.  r2 is consumed."""
    cs = chunks(n)
    for i, (off, c) in enumerate(cs):
        for hf in range(halves):
            d = H8(row0 + off, 32 + 16 * hf)
            k.s(mem('vldp8', d, 2, 16 * hf, 3, c))
            k.s(mem('vldq8', d, 2, 16 * hf, 3, c))
        if i + 1 < len(cs):
            sh = {16: 4, 8: 3, 4: 2, 2: 1, 1: 0}[c]
            if sh:
                k.s('lsl r5, r3, #%d' % sh)
                k.s('add r2, r5')
            else:
                k.s('add r2, r3')


def store(k, src8, n):
    k.s(bitplanes(Reg(4)))
    for off, c in chunks(n):
        if off:
            k.s('mov r5, r1')
            k.s('mul r5, #%d' % off if off not in (1, 2, 4, 8) else
                'lsl r5, #%d' % {1: 0, 2: 1, 4: 2, 8: 3}[off])
            k.s('add r5, r0')
            k.s(vstr(at(src8, off).stepped(), 5, 1, c))
        else:
            k.s(vstr(src8.stepped(), 0, 1, c))


def luma_kernel(h, case):
    fx, fy = case & 3, case >> 2
    name = 'h264_vmc_luma%d_%d%d' % (h, fx, fy)
    k = K(name, 'luma %dxN-high block (h = %d), phase (%d, %d)' % (16, h, fx, fy))
    full_rows = fy != 0
    from_x2 = fx != 0
    R = h + 5 if full_rows else h
    roff = 2 if full_rows else 0
    c0 = 34 if from_x2 else 32
    # source address: src - 2 (columns) - 2 * stride (rows)
    if from_x2:
        k.s('add r2, #-2')
    if full_rows:
        k.s('sub r2, r3')
        k.s('sub r2, r3')
    load_rows(k, 0, R, 2 if from_x2 else 1)
    if case:
        clip_zero(k, 5)                      # r5 is free until the sums
        k.s('mov r2, #4')                    # tap weights as registers: the
        k.s('mov r3, #5')                    # multiply-by-register form
    G = lambda dy, dx: H8(roff + dy, c0 + dx, True)   # noqa: E731

    if case == 0:
        out = H8(0, 32, True)
    elif fy == 0:                            # a, b, c: b from the row
        hsum16(k, TA, TB, 0, h, 32)
        round8(k, P1, TA, h)
        out = P1
        if fx != 2:
            avg(k, P2, G(0, fx >> 1), P1, TA, h)
            out = P2
    elif fx == 0:                            # d, h, n: h from the column
        vsum16(k, TA, TB, 0, h, 32)
        round8(k, P1, TA, h)
        out = P1
        if fy != 2:
            avg(k, P2, G(fy >> 1, 0), P1, TA, h)
            out = P2
    elif fx == 2 or fy == 2:                 # j, f, q, i, k: the centre
        S, HI, LO = H16(0, 0, True), H16(21, 0, True), H16(42, 0, True)
        AUX, J = H8(0, 32, True), H8(42, 0, True)
        hsum16(k, S, HI, 0, R, 32)
        if fx == 2 and fy != 2:              # f, q: average with b (row fy >> 1)
            round8(k, AUX, H16(2 + (fy >> 1), 0, True), h, TA)   # S stays
        elif fy == 2 and fx != 2:            # i, k: average with h (column fx >> 1)
            vsum16(k, TA, TB, 0, h, c0 + (fx >> 1))
            round8(k, AUX, TA, h)
        k.v('vasr', HI, S, 6, R)                      # S = 64 HI + LO
        k.s('mov r5, #64')
        k.v('vmul', LO, HI, Reg(5), R)
        k.v('vsub', LO, S, LO, R)
        vsum16_16(k, TA, TB, HI, h)                   # tap of HI
        vsum16_16(k, S, HI, LO, h)                    # tap of LO
        k.v('vadd', S, S, 512, h)
        k.v('vasr', S, S, 6, h)                       # (L + 512) >> 6
        k.v('vadd', TA, TA, S, h)
        k.v('vmulms', J, TA, 16, h)                   # clip((H + q) >> 4)
        out = J
        if fx != fy:
            avg(k, J, J, AUX, TB, h)
    else:                                    # e, g, p, r: b and h
        hsum16(k, TA, TB, roff + (fy >> 1), h, 32)
        round8(k, P1, TA, h)
        vsum16(k, TA, TB, 0, h, c0 + (fx >> 1))
        round8(k, P2, TA, h)
        avg(k, P1, P1, P2, TA, h)
        out = P1
    store(k, out, h)
    k.s('rts')
    return k


# ---------------------------------------------------------------------------
# Chroma interpolation (8.4.2.2.2): ((8-fx)(8-fy) A + fx(8-fy) B +
# (8-fx) fy C + fx fy D + 32) >> 6, done separably: H = (8-fx) A + fx B
# per row, then (8-fy) H0 + fy H1, then (x + 32) >> 6.  One phase zero:
# ((8-f) A + f B + 4) >> 3.  Both zero: a copy.
#
# Arguments: r0 dst, r1 dst stride, r2 src, r3 src stride, r4 lane mask
# (w = 8, 4, 2), r5 = fx | fy << 8.  VRF: DATA H8 rows 0..h at x 32,
# 16-bit temporaries TA/TB as for luma, result in P1.

def chroma_kernel(h, kind):
    name = 'h264_vmc_chroma%d_%s' % (h, kind)
    k = K(name, 'chroma, h = %d, %s' % (h, {'copy': 'whole sample',
          'h': 'horizontal phase only', 'v': 'vertical phase only',
          'hv': 'both phases'}[kind]))
    rows = h + 1 if kind in ('v', 'hv') else h
    if kind == 'copy':
        load_rows(k, 0, rows, 1)
        store(k, H8(0, 32, True), h)
        k.s('rts')
        return k
    k.s('push r6-r7')
    k.s('mov r6, r5')                       # the loads use r5
    load_rows(k, 0, rows, 1)
    k.s('lsr r7, r6, #8')                   # fy
    k.s('bmask r6, #8')                     # fx
    if kind != 'hv':
        k.s('or r6, r7')                    # the non-zero phase f
    k.s('mov r5, #8')
    k.s('sub r5, r6')                       # 8 - f
    if kind in ('h', 'hv'):
        k.v('vmul', TA, H8(0, 33, True), Reg(6), rows)       # f B
        k.v('vmul', TB, H8(0, 32, True), Reg(5), rows)       # (8 - f) A
        k.v('vadd', TA, TA, TB, rows)
    if kind == 'v':
        k.v('vmul', TA, H8(1, 32, True), Reg(6), h)
        k.v('vmul', TB, H8(0, 32, True), Reg(5), h)
        k.v('vadd', TA, TA, TB, h)
        k.v('vmulhr', P1, TA, 8192, h)                     # (x + 4) >> 3
    elif kind == 'h':
        k.v('vmulhr', P1, TA, 8192, h)
    else:
        k.s('mov r6, r7')                                  # fy
        k.s('mov r5, #8')
        k.s('sub r5, r6')                                  # 8 - fy
        k.v('vmul', TB, H16(TA.y + 1, TA.x, True), Reg(6), h)   # fy H1
        k.v('vmul', TA, TA, Reg(5), h)                     # (8 - fy) H0
        k.v('vadd', TA, TA, TB, h)
        k.v('vmulhr', P1, TA, 1024, h)                     # (x + 32) >> 6
    k.s('pop r6-r7')
    store(k, P1, h)
    k.s('rts')
    return k


# ---------------------------------------------------------------------------
# Inverse transform and add (8.5.12), one 4x4 block.
#
# h264_vidct4_add(r0 dst, r1 stride, r2 c): c holds 16 int16 in raster
# order, 32-byte aligned.  VRF: coefficient row r is loaded into the
# 16-bit H row r (lanes 0-3; the rest of each 32-byte load is unused), so the 16-bit
# V columns 0-3 hold coefficient columns (lane = row) and the H rows 0-3
# hold coefficient rows.  The row pass runs on V16 columns (temporaries
# at columns 4-7, results F at columns 32-35), the column pass on the
# H16 rows of F (temporaries at rows 4-7, results G at rows 8-11, x 0).
# Pixels: dst rows in H8 rows 16-19 at x 32.

def butterfly(k, out, a, d, t):
    """out[0..3] = the 4-point inverse transform of a[0..3] (8.5.12.2)."""
    k.v('vadd', t[0], a[0], a[2])                 # e0 = d0 + d2
    k.v('vsub', t[1], a[0], a[2])                 # e1 = d0 - d2
    k.v('vasr', t[2], a[1], 1)
    k.v('vsub', t[2], t[2], a[3])                 # e2 = (d1 >> 1) - d3
    k.v('vasr', t[3], a[3], 1)
    k.v('vadd', t[3], t[3], a[1])                 # e3 = d1 + (d3 >> 1)
    k.v('vadd', out[0], t[0], t[3])
    k.v('vadd', out[1], t[1], t[2])
    k.v('vsub', out[2], t[1], t[2])
    k.v('vsub', out[3], t[0], t[3])


def idct_kernels():
    ks = []
    k = K('h264_vidct4_add', '4x4 inverse transform, add to dst, clear c')
    # each coefficient row loaded on its own: a B operand '.t+rN' (the
    # old row split, vmov from b00.t+r3) ignores rN on silicon (device
    # session 18: all four rows came out as row 0)
    # (and on silicon a plain vld16 aligns its address down to 32 bytes,
    # device session 22, so rows 1-3 can't be loaded at +8/+16/+24):
    # one aligned load of all 16, then each row's 8 bytes staged from row
    # 0 through an 8-bit A operand, which addresses any x (as the intra
    # 4x4 filters do, right on silicon since session 22)
    # (a 16-bit H row holds its lanes' low bytes at x 0-15 and their high
    # bytes at x 16-31: lane 4r's halves are at x = 4r and 16 + 4r)
    k.s(mem('vld16', H16(0, 0), 2))
    for r in (1, 2, 3):
        k.v('vadd', H8(r, 0), H8(0, 4 * r), 0)
        k.v('vadd', H8(r, 16), H8(0, 16 + 4 * r), 0)
    col = lambda x: V16(0, x)                                  # noqa: E731
    butterfly(k, [col(32 + i) for i in range(4)], [col(i) for i in range(4)],
              None, [col(4 + i) for i in range(4)])
    row = lambda y, x=0: H16(y, x)                             # noqa: E731
    k.v('vadd', row(0, 32), row(0, 32), 32)       # + 32 reaches every output
    butterfly(k, [row(8 + i) for i in range(4)], [row(i, 32) for i in range(4)],
              None, [row(4 + i) for i in range(4)])
    k.v('vasr', H16(8, 0, True), H16(8, 0, True), 6, 4)
    k.s(mem('vldp8', H8(16, 32), 0, 0, 1, 4))
    k.s(mem('vldq8', H8(16, 32), 0, 0, 1, 4))
    k.v('vadds', H8(16, 32, True), H8(16, 32, True), H16(8, 0, True), 4)
    k.s(bitplanes(0x000f))
    k.s(vstr(H8(16, 32, True), 0, 1, 4))
    k.s('mov r3, #0')
    k.v('vmov', H16(0, 0), None, Reg(3))          # row 0 is free: store from d00
    k.s(mem('vst16', H16(0, 0), 2))
    k.s('rts')
    ks.append(k)

    k = K('h264_vidct4_dc_add', 'DC-only block: (c[0] + 32) >> 6 added to 16 pixels')
    k.s('ldh r3, (r2)')
    k.s('lsl r3, #16')
    k.s('asr r3, #16')                            # c[0], signed
    k.s('mov r4, #0')
    k.s('sth r4, (r2)')
    k.s('add r3, #32')
    k.s('asr r3, #6')
    k.s(mem('vldp8', H8(16, 32), 0, 0, 1, 4))
    k.s(mem('vldq8', H8(16, 32), 0, 0, 1, 4))
    k.v('vmov', H16(20, 0), None, Reg(3))
    k.v('vadds', H8(16, 32, True), H8(16, 32, True), H16(20, 0), 4)
    k.s(bitplanes(0x000f))
    k.s(vstr(H8(16, 32, True), 0, 1, 4))
    k.s('rts')
    ks.append(k)
    return ks


# ---------------------------------------------------------------------------
# Loop filter (8.7.2), one edge.  Lanes are the edge's lines: 16 for luma,
# 8 for chroma (lanes 8-15 are masked off).  The samples p3..q3 of every
# line are 8-bit vectors: V8 columns 0-7 of rows loaded from pix - 4 (a
# vertical edge: one line per row) or H8 rows of the rows across the edge
# (a horizontal edge: one line per column).
#
# Arguments: r0 pix (q0 of the first line), r1 the stride (the plane's
# width), r2 alpha, r3 beta, r4 the lanes to filter (bS > 0), r5 a
# pointer to 16 tC0 bytes, one per lane (bS < 4 only).
# 16-bit temporaries: H16 rows at x 32; tC0: H8 row 48 at x 0.

T = [H16(y, 32) for y in range(32)]
TC0 = H8(48, 0)
ALPHA8, BETA8, ZERO8 = H8(49, 0), H8(50, 0), H8(51, 0)
BETA16, TC16, SAVED = T[31], T[30], T[29]


def flags_filter(k, S):
    """VRF rows alpha, beta (8 and 16 bits) and zero from r2, r3; then the
    N flag = the lanes to filter: masked in (r4), |p0 - q0| < alpha,
    |p1 - p0| < beta, |q1 - q0| < beta.  SAVED = -1 on those lanes, else
    0, to restore N later (vsub SAVED - 0, setf).  r2-r5 are free after."""
    k.v('vmov', ALPHA8, None, Reg(2))
    k.v('vmov', BETA8, None, Reg(3))
    k.v('vmov', BETA16, None, Reg(3))
    k.s('mov r2, #0')
    k.v('vmov', ZERO8, None, Reg(2))
    k.s(bitplanes(Reg(4)))
    k.v('vdist', T[0], S(-1), S(0))
    k.v('vsub', DISCARD, T[0], ALPHA8, setf=True, pred='ifnz')
    k.v('vdist', T[1], S(-2), S(-1))
    k.v('vsub', DISCARD, T[1], BETA8, setf=True, pred='ifn')
    k.v('vdist', T[1], S(1), S(0))
    k.v('vsub', DISCARD, T[1], BETA8, setf=True, pred='ifn')
    k.s('mov r3, #-1')
    k.v('vmov', SAVED, None, Reg(2))
    k.v('vmov', SAVED, None, Reg(3), pred='ifn')


def restore(k):
    k.v('vsub', DISCARD, SAVED, ZERO8, setf=True)          # N = filtered lanes


def narrow(k, m):
    k.v('vsub', DISCARD, m, ZERO8, setf=True, pred='ifn')  # N &= (m = -1)


def below_beta(k, t, a, b):
    """t = -1 where |a - b| < beta, else 0."""
    k.v('vdist', t, a, b)
    k.v('vsub', t, t, BETA16)
    k.v('vasr', t, t, 15)


def store8(k, dst, src16, pred='ifn'):
    """dst (8-bit samples) = src16 (in 0..255), saturating add to zero."""
    k.v('vadds', dst, ZERO8, src16, pred=pred)


def delta(k, t, S):
    """t = ((q0 - p0) * 4 + (p1 - q1) + 4) >> 3, unclipped; uses r2-r5."""
    k.s('mov r2, #4')
    k.s('mov r3, #-4')
    k.s('mov r4, #-1')
    k.v('vadd', DISCARD, S(-2), 4, clra=True, acc=True)      # p1 + 4
    k.v('vmul', DISCARD, S(0), Reg(2), acc=True)             # + 4 q0
    k.v('vmul', DISCARD, S(-1), Reg(3), acc=True)            # - 4 p0
    k.v('vmul', t, S(1), Reg(4), acc=True)                   # - q1
    k.v('vasr', t, t, 3)


def dbk_normal(k, S, luma):
    flags_filter(k, S)
    k.v('vadd', TC16, TC0, 0)                    # tc0, 16 bits
    if luma:
        below_beta(k, T[2], S(-3), S(-1))        # ap: -1 / 0
        below_beta(k, T[3], S(2), S(0))          # aq
        k.v('vsub', T[4], TC16, T[2])            # tc = tc0 + ap + aq
        k.v('vsub', T[4], T[4], T[3])
    else:
        k.v('vadd', T[4], TC0, 1)                # tc = tc0 + 1
    k.v('vrsub', T[5], T[4], 0)                  # -tc
    delta(k, T[6], S)
    k.v('vmax', T[6], T[6], T[5])
    k.v('vmin', T[6], T[6], T[4])                # delta
    if luma:
        k.s('mov r5, #1')
        k.s('mov r2, #-2')
        k.v('vadd', T[7], S(-1), S(0))
        # (p0 + q0 + 1) >> 1 by add and shift: vmulhr reads its B as
        # signed on silicon, so #0x8000 was -1/2 (device session 26)
        k.v('vadd', T[7], T[7], 1)
        k.v('vasr', T[7], T[7], 1)
        k.v('vrsub', T[8], TC16, 0)              # -tc0
        for t_a, x2, x1, tt in ((T[2], -3, -2, T[9]), (T[3], 2, 1, T[10])):
            # x1' = x1 + clip3(-tc0, tc0, (x2 + avg - 2 x1) >> 1) where ax
            k.v('vmul', DISCARD, T[7], Reg(5), clra=True, acc=True)
            k.v('vmul', DISCARD, S(x2), Reg(5), acc=True)
            k.v('vmul', tt, S(x1), Reg(2), acc=True)
            k.v('vasr', tt, tt, 1)
            k.v('vmax', tt, tt, T[8])
            k.v('vmin', tt, tt, TC16)
        for t_a, x1, tt in ((T[2], -2, T[9]), (T[3], 1, T[10])):
            restore(k)
            narrow(k, t_a)
            k.v('vadds', S(x1), S(x1), tt, pred='ifn')
        restore(k)
    k.v('vadds', S(-1), S(-1), T[6], pred='ifn')        # p0 + delta, clipped
    k.v('vrsub', T[6], T[6], 0)
    k.v('vadds', S(0), S(0), T[6], pred='ifn')          # q0 - delta, clipped


def dbk_strong(k, S, luma):
    if luma:
        k.s('lsr r5, r2, #2')
        k.s('add r5, #2')                        # (alpha >> 2) + 2
    flags_filter(k, S)
    k.s('mov r2, #1')
    k.s('mov r3, #2')
    k.s('mov r4, #3')
    if not luma:
        for x, t in ((-1, T[2]), (0, T[3])):     # (2 x1 + x0 + y1 + 2) >> 2
            y1 = 1 if x < 0 else -2
            x1 = -2 if x < 0 else 1
            k.v('vmul', DISCARD, S(x1), Reg(3), clra=True, acc=True)
            k.v('vadd', t, S(x), S(y1), acc=True)
            k.v('vmulhr', t, t, 0x4000)
        store8(k, S(-1), T[2])
        store8(k, S(0), T[3])
        return
    # strong where |x2 - x0| < beta and |p0 - q0| < (alpha >> 2) + 2
    k.v('vmov', T[28], None, Reg(5))
    k.v('vsub', T[4], T[0], T[28])
    k.v('vasr', T[4], T[4], 15)                  # small: -1 / 0
    below_beta(k, T[2], S(-3), S(-1))            # ap
    below_beta(k, T[3], S(2), S(0))              # aq
    sides = []
    for sg in (-1, 1):
        x = lambda i, sg=sg: S(-1 - i if sg < 0 else i)   # noqa: E731  x0..x3
        y = lambda i, sg=sg: S(i if sg < 0 else -1 - i)   # noqa: E731  y0, y1
        b = 6 if sg < 0 else 12
        s3, w0, s0, s1, s2 = T[b], T[b + 1], T[b + 2], T[b + 3], T[b + 4]
        k.v('vadd', DISCARD, x(0), y(0), clra=True, acc=True)
        k.v('vmul', s3, x(1), Reg(2), acc=True)              # s = x1 + x0 + y0
        k.v('vmul', DISCARD, x(1), Reg(3), clra=True, acc=True)
        k.v('vadd', w0, x(0), y(1), acc=True)
        k.v('vmulhr', w0, w0, 0x4000)                        # (2 x1 + x0 + y1 + 2) >> 2
        k.v('vmul', DISCARD, s3, Reg(3), clra=True, acc=True)
        k.v('vadd', s0, x(2), y(1), acc=True)
        k.v('vmulhr', s0, s0, 0x2000)                        # (x2 + 2 s + y1 + 4) >> 3
        k.v('vmul', DISCARD, s3, Reg(2), clra=True, acc=True)
        k.v('vmul', s1, x(2), Reg(2), acc=True)
        k.v('vmulhr', s1, s1, 0x4000)                        # (x2 + s + 2) >> 2
        k.v('vmul', DISCARD, s3, Reg(2), clra=True, acc=True)
        k.v('vmul', DISCARD, x(3), Reg(3), acc=True)
        k.v('vmul', s2, x(2), Reg(4), acc=True)
        k.v('vmulhr', s2, s2, 0x2000)                        # (2 x3 + 3 x2 + s + 4) >> 3
        sides.append((x, w0, s0, s1, s2, T[2] if sg < 0 else T[3]))
    for x, w0, s0, s1, s2, ax in sides:
        restore(k)
        store8(k, x(0), w0)
        narrow(k, ax)
        narrow(k, T[4])
        store8(k, x(0), s0)
        store8(k, x(1), s1)
        store8(k, x(2), s2)


def dbk_kernels():
    ks = []
    for luma in (True, False):
        for vert in (True, False):
            for strong in (False, True):
                name = 'h264_vdbk_%s_%s_%s' % ('luma' if luma else 'chroma',
                                               'v' if vert else 'h',
                                               'strong' if strong else 'normal')
                k = K(name, '%s %s edge, %s' % (
                    'luma' if luma else 'chroma', 'vertical' if vert else
                    'horizontal', 'bS = 4' if strong else 'bS < 4'))
                n = 16 if luma else 8
                if vert:
                    # line l = row l of the VRF, from pix - 4 (any alignment)
                    k.s('add r0, #-4')
                    k.s(mem('vldp8', H8(0, 0), 0, 0, 1, n))
                    k.s(mem('vldq8', H8(0, 0), 0, 0, 1, n))
                    S = lambda i: V8(0, 4 + i)       # noqa: E731
                else:
                    # sample i = VRF row 4 + i, from pix + (i) * stride
                    k.s('sub r0, r1')
                    k.s('sub r0, r1')
                    k.s('sub r0, r1')
                    k.s('sub r0, r1')
                    if luma:
                        k.s(mem('vld8', H8(0, 0), 0, 0, 1, 8))
                    else:
                        k.s(mem('vldp8', H8(0, 0), 0, 0, 1, 8))
                        k.s(mem('vldq8', H8(0, 0), 0, 0, 1, 8))
                    S = lambda i: H8(4 + i, 0)       # noqa: E731
                if not strong:
                    k.s(mem('vldp8', TC0, 5, 0))
                    k.s(mem('vldq8', TC0, 5, 0))
                (dbk_strong if strong else dbk_normal)(k, S, luma)
                if vert:
                    # a vstr writes only the lanes in its address's 16-byte
                    # line (device session 22: the q side of every edge at a
                    # line boundary was lost), so p and q are stored apart:
                    # p from pix - 4 (always inside one line), q from pix
                    # with the row shifted 4 lanes
                    k.s(bitplanes(0x000e if luma else 0x0008))
                    k.s(vstr(H8(0, 0, True), 0, 1, n))
                    k.s('add r2, r0, #4')
                    k.s(bitplanes(0x0007 if luma else 0x0001))
                    k.s(vstr(H8(0, 4, True), 2, 1, n))
                elif luma:
                    k.s(mem('vst8', H8(0, 0), 0, 0, 1, 8))
                else:
                    k.s(bitplanes(0x00ff))
                    k.s('mov r5, r1')
                    k.s('lsl r5, #1')
                    k.s('add r0, r5')                  # p1 row
                    k.s('add r0, r1')                  # p0 row
                    k.s(vstr(H8(3, 0, True), 0, 1, 2))
                k.s('rts')
                ks.append(k)
    return ks


# ---------------------------------------------------------------------------
# Intra prediction (8.3.1.2, 8.3.3, 8.3.4), written from the equations.
#
# Arguments: r0 dst, r1 stride, r2/r3 per kernel (the C wrapper in intra.c
# checks availability and picks the kernel).  Samples outside the block
# are read only where the mode uses them; the loads may touch up to 32
# bytes around those rows (always inside the picture or the arena).
#
# 4x4 (dst 4-aligned).  The edge row E (H8 row 44 at x 16) holds, as in
# intra.c, e[0..3] = p[-1,3..0], e[4] = p[-1,-1], e[5..12] = p[0..7,-1]
# and e[13] = e[12].  The left samples come from the rows loaded bottom up
# (negative stride), so their column reads in that order; the top row is
# loaded from dst - stride - 4 into H8 row 40 at x 16, so E[i] is that row
# at column 15 + i.  Then, in 16 lanes,
#   F1[i] = (e[i-1] + 2 e[i] + e[i+1] + 2) >> 2      H8 row 46, x 16
#   F2[i] = (e[i] + e[i+1] + 1) >> 1                  H8 row 47, x 16
# and every directional mode is made of 4-lane windows of F1 and F2.  For
# Horizontal_Down and Horizontal_Up the rows alternate F2 and F1 lanes;
# writing four V8 columns (F2[k], F1[k+1], F2[k+1], F1[k+2]) makes VRF row
# k exactly [F2[k], F1[k+1], F2[k+1], F1[k+2]], one output row (H/V
# aliasing does the interleave).  Horizontal_Up runs the same filters on
# the left column top down with p[-1,3] repeated (r2 = p[-1,3]): its
# S[z] (zHU = x + 2y) is F2[z/2] for even z and F1[(z+1)/2] for odd z.

I4_TOPROW = 40
I4_E, I4_LF = 44, 45
I4_F1, I4_F2 = 46, 47
I4_T1, I4_T2 = H16(48, 32), H16(49, 32)
I4_E1 = 50          # e[i + 1] at x 16, through an A operand (see i4_filters)


def i4_top(k):
    """H8 row 40 at x 16, lane j = p[j - 4, -1]; r4 = dst - stride - 4."""
    k.s('mov r4, r0')
    k.s('sub r4, r1')
    k.s('add r4, #-4')
    k.s(mem('vldp8', H8(I4_TOPROW, 16), 4))
    k.s(mem('vldq8', H8(I4_TOPROW, 16), 4))


def i4_left(k, reverse):
    """H8 rows 32-35 from dst - 4: column 3 = p[-1, y], y = 0..3 or, with
    reverse, 3..0 (then r5 = dst + 3 stride - 4 and r4 = -stride)."""
    if reverse:
        k.s('mov r5, r1')
        k.s('lsl r5, #1')
        k.s('add r5, r1')
        k.s('add r5, r0')
        k.s('add r5, #-4')
        k.s('neg r4, r1')
        k.s(mem('vldp8', H8(32, 0), 5, 0, 4, 4))
        k.s(mem('vldq8', H8(32, 0), 5, 0, 4, 4))
    else:
        k.s('mov r5, r0')
        k.s('add r5, #-4')
        k.s(mem('vldp8', H8(32, 0), 5, 0, 1, 4))
        k.s(mem('vldq8', H8(32, 0), 5, 0, 1, 4))


def i4_filters(k, row):
    """F1 and F2 of the 16 samples in H8 row `row` at x 16."""
    # e[i + 1]: a B operand addresses x only in steps of 16, and the old
    # 'B one column on with +r5' (no ++) ignores r5 on silicon (device
    # session 21), so the shifted row is staged through an A operand
    k.s('mov r4, #2')
    e = H8(row, 16)
    k.v('vadd', H8(I4_E1, 16), H8(row, 17), 0)
    e1 = H8(I4_E1, 16)
    k.v('vmul', DISCARD, e, Reg(4), clra=True, acc=True)       # 2 e[i]
    k.v('vadd', I4_T1, H8(row, 15), e1, acc=True)              # + e[i-1] + e[i+1]
    k.v('vmulhr', H8(I4_F1, 16), I4_T1, 0x4000)               # (x + 2) >> 2
    k.v('vadd', I4_T2, e, e1)
    k.v('vmulhrs', H8(I4_F2, 16), I4_T2, 0x8000)              # (x + 1) >> 1


def i4_win(k, y, f, s, pred=None):
    """Output row y (H8 row y, x 0) = F[s .. s + 3] (f = I4_F1 / I4_F2)."""
    k.v('vadd', H8(y, 0), H8(f, 16 + s), 0, pred=pred)


def i4_store(k, src=None, base=0, stride=1):
    k.s(bitplanes(0x000f))
    k.s(vstr((src or H8(0, 0)).stepped(), base, stride, 4))


def i4_kernels():
    ks = []
    k = K('h264_vpred4_v', '4x4 Vertical')
    k.s('mov r4, r0')
    k.s('sub r4, r1')
    k.s(mem('vldp8', H8(I4_TOPROW, 0), 4))
    k.s(mem('vldq8', H8(I4_TOPROW, 0), 4))
    k.v('vmov', H8(0, 0, True), None, H8(I4_TOPROW, 0), 4)
    i4_store(k)
    k.s('rts')
    ks.append(k)

    k = K('h264_vpred4_h', '4x4 Horizontal')
    i4_left(k, False)
    k.v('vmov', V8(32, 4, True), None, V8(32, 3), 4)     # columns 4-7 = column 3
    i4_store(k, H8(32, 4))
    k.s('rts')
    ks.append(k)

    for kind in ('lt', 'l', 't', 'none'):
        k = K('h264_vpred4_dc_%s' % kind, '4x4 DC, %s' % {
            'lt': 'left and top', 'l': 'left only', 't': 'top only',
            'none': 'no neighbours: 128'}[kind])
        if kind == 'none':
            k.s('mov r2, #128')
        else:
            if 't' in kind:
                k.s('mov r4, r0')
                k.s('sub r4, r1')
                k.s(mem('vldp8', H8(I4_TOPROW, 0), 4))
                k.s(mem('vldq8', H8(I4_TOPROW, 0), 4))
            if 'l' in kind:
                i4_left(k, False)
            k.s(bitplanes(0x000f))
            if 't' in kind:
                k.v('vmov', DISCARD, None, H8(I4_TOPROW, 0), pred='ifnz', sum_reg=2)
            if 'l' in kind:
                k.v('vmov', DISCARD, None, V8(32, 3), pred='ifnz',
                    sum_reg=3 if kind == 'lt' else 2)
            if kind == 'lt':
                k.s('add r2, r3')
                k.s('add r2, #4')
                k.s('lsr r2, #3')
            else:
                k.s('add r2, #2')
                k.s('lsr r2, #2')
        k.v('vmov', H8(0, 0, True), None, Reg(2), 4)
        i4_store(k)
        k.s('rts')
        ks.append(k)

    # Diagonal_Down_Left and Vertical_Left: the top row and its right
    # half; r2 = the sample for e[9..13] and r3 their lanes (the wrapper
    # passes p[3,-1] and 0x3e00 when p[4..7,-1] are not available, else
    # p[7,-1] and 0x2000, for e[13] = e[12])
    for name, rows, desc in (
            ('ddl', [(I4_F1, 6), (I4_F1, 7), (I4_F1, 8), (I4_F1, 9)],
             'Diagonal_Down_Left: row y = F1[6 + y ..]'),
            ('vl', [(I4_F2, 5), (I4_F1, 6), (I4_F2, 6), (I4_F1, 7)],
             'Vertical_Left: F2[5..], F1[6..], F2[6..], F1[7..]')):
        k = K('h264_vpred4_%s' % name, '4x4 ' + desc)
        i4_top(k)
        k.v('vadd', H8(I4_E, 16), H8(I4_TOPROW, 15), 0)     # e[i] = top lane i - 1
        k.s(bitplanes(Reg(3)))
        k.v('vmov', H8(I4_E, 16), None, Reg(2), pred='ifnz')
        i4_filters(k, I4_E)
        for y, (f, st) in enumerate(rows):
            i4_win(k, y, f, st)
        i4_store(k)
        k.s('rts')
        ks.append(k)

    # Diagonal_Down_Right, Vertical_Right, Horizontal_Down: the whole edge
    for name, desc in (('ddr', 'Diagonal_Down_Right: row y = F1[4 - y ..]'),
                       ('vr', 'Vertical_Right: F2[4..], F1[4..], F1[3] F2[4..], '
                              'F1[2] F1[4..]'),
                       ('hd', 'Horizontal_Down: rows of [F2[k], F1[k+1], '
                              'F2[k+1], F1[k+2]], bottom up')):
        k = K('h264_vpred4_%s' % name, '4x4 ' + desc)
        i4_top(k)
        i4_left(k, True)                                    # r4 = -stride, r5
        k.v('vadd', H8(I4_E, 16), H8(I4_TOPROW, 15), 0)
        k.s(bitplanes(0x000f))
        k.v('vmov', H8(I4_E, 16), None, V8(32, 3), pred='ifnz')   # e[0..3]
        if name == 'hd':
            k.s('mov r2, r5')                               # dst + 3 stride - 4
            k.s('mov r3, r4')                               # -stride
        i4_filters(k, I4_E)
        if name == 'ddr':
            for y in range(4):
                i4_win(k, y, I4_F1, 4 - y)
            i4_store(k)
        elif name == 'vr':
            i4_win(k, 0, I4_F2, 4)
            i4_win(k, 1, I4_F1, 4)
            i4_win(k, 2, I4_F2, 3)
            i4_win(k, 3, I4_F1, 3)
            k.s(bitplanes(0x0001))                          # lane 0 of rows 2, 3
            i4_win(k, 2, I4_F1, 3, 'ifnz')
            i4_win(k, 3, I4_F1, 2, 'ifnz')
            i4_store(k)
        else:
            hx_columns(k)
            # row 0 is [F2[3], F1[4], F1[5], F1[6]]: VRF row 19 has F2[4],
            # F1[5] in lanes 2, 3
            k.s(bitplanes(0x000c))
            k.v('vadd', H8(19, 0), H8(I4_F1, 16 + 3), 0, pred='ifnz')
            k.s('add r2, #4')                               # dst + 3 stride
            i4_store(k, H8(16, 0), 2, 3)                    # row 3 first
        k.s('rts')
        ks.append(k)

    k = K('h264_vpred4_hu', '4x4 Horizontal_Up: rows of [F2[y], F1[y+1], '
          'F2[y+1], F1[y+2]] over p[-1,0..3], p[-1,3] repeated (r2)')
    i4_left(k, False)
    k.v('vadd', H8(I4_LF, 16), V8(32, 3), 0)
    k.s(bitplanes(0xfff0))
    k.v('vmov', H8(I4_LF, 16), None, Reg(2), pred='ifnz')
    i4_filters(k, I4_LF)
    hx_columns(k)
    i4_store(k, H8(16, 0))
    k.s('rts')
    ks.append(k)
    return ks


def hx_columns(k):
    """VRF rows 16.. columns 0-3 = [F2[k], F1[k+1], F2[k+1], F1[k+2]]."""
    for c, (f, o) in enumerate(((I4_F2, 0), (I4_F1, 1), (I4_F2, 1), (I4_F1, 2))):
        k.v('vadd', V8(16, c), H8(f, 16 + o), 0)


# 16x16 (dst 16-aligned, so plain row loads and stores) and chroma 8x8
# (dst 8-aligned: vldp/vldq pairs and masked stores).  Left samples: the
# rows from dst - 16 (luma, column 15) or dst - 8 (chroma, column 7) in
# H8 rows 0-15.  DC sums come from the scalar-result unit.  Plane (8.3.3.4,
# 8.3.4.4): H = sum of (x - 7) p[x,-1] - 8 p[-1,-1] (luma; chroma (x - 3)
# and 4 p[-1,-1]) is one multiply by a ramp row with a sum, likewise V;
# then rows P0[x] = a - 7c + b (x - 7) (16 bits) plus c y, the c y term a
# matrix of V16 columns; round8 is clip((x + 16) >> 5).

RAMP, P0, CY = H16(48, 32), H16(49, 32), H16(50, 32)
DC_ROWS = H8(16, 0)          # chroma output rows 16..23 at x 0 (DC, V)
I16_OUT = H8(0, 0)           # 16x16 output rows 0..15 (48-bit stores only from d00)


def plane(k, n):
    """r0 dst, r1 stride; n = 16 (luma) or 8 (chroma)."""
    sh, top_x = (7, 15) if n == 16 else (3, 7)
    k.s('push r6-r7')
    k.s('mov r2, r0')
    k.s('sub r2, r1')                                     # the top row
    k.s('mov r3, r0')
    k.s('add r3, #-%d' % n)
    if n == 16:
        k.s(mem('vld8', H8(0, 0), 3, 0, 1, 16))           # left: column 15
        k.s(mem('vld8v', V8(32, 48), 2))                  # top as a column
        top = V8(32, 48)
    else:
        k.s(mem('vldp8', H8(0, 0), 3, 0, 1, 8))
        k.s(mem('vldq8', H8(0, 0), 3, 0, 1, 8))
        k.s(mem('vldp8', H8(32, 48), 2))
        k.s(mem('vldq8', H8(32, 48), 2))
        top = H8(32, 48)
    k.s('mov r5, #h264_vpu_ramp%d' % n)
    k.s(mem('vld16', RAMP, 5))
    k.v('vmul', DISCARD, RAMP, top, sum_reg=4)            # sum ramp[x] top[x]
    k.v('vmul', DISCARD, RAMP, V8(0, n - 1), sum_reg=5)   # sum ramp[y] left[y]
    k.s('ldb r3, (r2-1)')                                 # p[-1,-1]
    k.s('lsl r3, #%d' % (3 if n == 16 else 2))
    k.s('sub r4, r3')                                     # H
    k.s('sub r5, r3')                                     # V
    for r in (4, 5):                                      # b, c
        if n == 16:                                       # (5 x + 32) >> 6
            k.s('mov r3, r%d' % r)
            k.s('lsl r3, #2')
            k.s('add r%d, r3' % r)
        else:                                             # (34 x + 32) >> 6
            k.s('mov r3, r%d' % r)
            k.s('lsl r3, #5')
            k.s('lsl r%d, #1' % r)
            k.s('add r%d, r3' % r)
        k.s('add r%d, #32' % r)
        k.s('asr r%d, #6' % r)
    # a = 16 (p[-1,n-1] + p[n-1,-1]); r6 = a - sh c, r7 = sh c
    k.s('mov r6, r1')
    k.s('lsl r6, #%d' % (4 if n == 16 else 3))
    k.s('sub r6, r1')
    k.s('add r6, r0')
    k.s('ldb r6, (r6-1)')                                 # p[-1, n-1]
    k.s('ldb r3, (r2+%d)' % top_x)                        # p[n-1, -1]
    k.s('add r6, r3')
    k.s('lsl r6, #4')
    # sh c in r3, not r7: a scalar B operand in r7 reads as 0 on silicon
    # (device session 22: the plane was off by -7c), as a vector memory
    # base of 7 does
    k.s('mov r3, r5')
    k.s('lsl r3, #%d' % (3 if n == 16 else 2))
    k.s('sub r3, r5')                                     # sh c (7c or 3c)
    k.s('sub r6, r3')
    k.v('vmul', P0, RAMP, Reg(4))
    k.v('vadd', P0, P0, Reg(6))                           # a - sh c + b ramp[x]
    k.v('vmul', CY, RAMP, Reg(5))
    k.v('vadd', CY, CY, Reg(3))                           # c y
    k.s('pop r6-r7')
    k.v('vmov', V16(32, 0, True), None, CY, n)            # columns: row y = c y
    k.v('vadd', H16(16, 0, True), P0, H16(32, 0, True), n)
    # into rows 0.. (the left samples, consumed): 48-bit stores only from d00
    clip_zero(k, 2)
    round8(k, H8(0, 0, True), H16(16, 0, True), n)
    if n == 16:
        k.s(mem('vst8', H8(0, 0), 0, 0, 1, 16))
    else:
        k.s(bitplanes(0x00ff))
        k.s(vstr(H8(0, 0, True), 0, 1, 8))
    k.s('rts')


def i16_kernels():
    ks = []
    out = lambda k: k.s(mem('vst8', I16_OUT, 0, 0, 1, 16))   # noqa: E731

    k = K('h264_vpred16_v', '16x16 Vertical')
    k.s('mov r2, r0')
    k.s('sub r2, r1')
    k.s(mem('vld8', H8(40, 0), 2))
    k.v('vmov', I16_OUT.stepped(), None, H8(40, 0), 16)
    out(k)
    k.s('rts')
    ks.append(k)

    k = K('h264_vpred16_h', '16x16 Horizontal')
    k.s('mov r2, r0')
    k.s('add r2, #-16')
    k.s(mem('vld8', H8(0, 0), 2, 0, 1, 16))
    k.v('vmov', V8(0, 0, True), None, V8(0, 15), 16)     # columns 0-15 = column 15
    k.s(mem('vst8', H8(0, 0), 0, 0, 1, 16))
    k.s('rts')
    ks.append(k)

    for kind in ('lt', 'l', 't', 'none'):
        k = K('h264_vpred16_dc_%s' % kind, '16x16 DC, %s' % kind)
        if kind == 'none':
            k.s('mov r2, #128')
        else:
            if 't' in kind:
                k.s('mov r2, r0')
                k.s('sub r2, r1')
                k.s(mem('vld8', H8(40, 0), 2))
                k.v('vmov', DISCARD, None, H8(40, 0), sum_reg=2)
            if 'l' in kind:
                k.s('mov r3, r0')
                k.s('add r3, #-16')
                k.s(mem('vld8', H8(0, 0), 3, 0, 1, 16))
                k.v('vmov', DISCARD, None, V8(0, 15),
                    sum_reg=3 if kind == 'lt' else 2)
            if kind == 'lt':
                k.s('add r2, r3')
                k.s('add r2, #16')
                k.s('lsr r2, #5')
            else:
                k.s('add r2, #8')
                k.s('lsr r2, #4')
        k.v('vmov', I16_OUT.stepped(), None, Reg(2), 16)
        out(k)
        k.s('rts')
        ks.append(k)

    k = K('h264_vpred16_plane', '16x16 Plane')
    plane(k, 16)
    ks.append(k)
    return ks


def c8_kernels():
    ks = []

    def top(k):
        k.s('mov r4, r0')
        k.s('sub r4, r1')
        k.s(mem('vldp8', H8(40, 0), 4))
        k.s(mem('vldq8', H8(40, 0), 4))

    def left(k):
        k.s('mov r5, r0')
        k.s('add r5, #-8')
        k.s(mem('vldp8', H8(0, 0), 5, 0, 1, 8))
        k.s(mem('vldq8', H8(0, 0), 5, 0, 1, 8))

    def store(k, src):
        k.s(bitplanes(0x00ff))
        k.s(vstr(src.stepped(), 0, 1, 8))

    k = K('h264_vpred8_v', 'chroma Vertical')
    top(k)
    k.v('vmov', DC_ROWS.stepped(), None, H8(40, 0), 8)
    store(k, DC_ROWS)
    k.s('rts')
    ks.append(k)

    k = K('h264_vpred8_h', 'chroma Horizontal')
    left(k)
    k.v('vmov', V8(0, 16, True), None, V8(0, 7), 8)      # columns 16-23
    store(k, H8(0, 16))
    k.s('rts')
    ks.append(k)

    # DC per 4x4 quarter (8.3.4.1-3): sums of top[0..3], top[4..7],
    # left[0..3], left[4..7]; rows 16-19 and 20-23 from two pattern rows
    for kind in ('lt', 'l', 't', 'none'):
        k = K('h264_vpred8_dc_%s' % kind, 'chroma DC, %s' % kind)
        A, B = H8(8, 0), H8(9, 0)                          # rows 0-3, 4-7
        if kind == 'none':
            k.s('mov r2, #128')
            k.v('vmov', DC_ROWS.stepped(), None, Reg(2), 8)
        else:
            if 't' in kind:
                top(k)
            if 'l' in kind:
                left(k)
            for mask, regs in ((0x000f, (2, 3)), (0x00f0, (4, 5))):
                k.s(bitplanes(mask))
                if 't' in kind:
                    k.v('vmov', DISCARD, None, H8(40, 0), pred='ifnz', sum_reg=regs[0])
                if 'l' in kind:
                    k.v('vmov', DISCARD, None, V8(0, 7), pred='ifnz', sum_reg=regs[1])
            # r2 T0, r3 L0, r4 T1, r5 L1
            if kind == 'lt':
                k.s('add r3, r2')
                k.s('add r3, #4')
                k.s('lsr r3, #3')                          # quarter 0
                k.s('mov r2, r4')
                k.s('add r2, r5')
                k.s('add r2, #4')
                k.s('lsr r2, #3')                          # quarter 3
                k.s('add r4, #2')
                k.s('lsr r4, #2')                          # quarter 1: top
                k.s('add r5, #2')
                k.s('lsr r5, #2')                          # quarter 2: left
                quads = (3, 4, 5, 2)
            elif kind == 't':
                for r in (2, 4):
                    k.s('add r%d, #2' % r)
                    k.s('lsr r%d, #2' % r)
                quads = (2, 4, 2, 4)
            else:
                for r in (3, 5):
                    k.s('add r%d, #2' % r)
                    k.s('lsr r%d, #2' % r)
                quads = (3, 3, 5, 5)
            k.v('vmov', A, None, Reg(quads[0]))
            k.v('vmov', B, None, Reg(quads[2]))
            k.s(bitplanes(0x00f0))
            k.v('vmov', A, None, Reg(quads[1]), pred='ifnz')
            k.v('vmov', B, None, Reg(quads[3]), pred='ifnz')
            k.v('vmov', DC_ROWS.stepped(), None, A, 4)
            k.v('vmov', H8(20, 0, True), None, B, 4)
        store(k, DC_ROWS)
        k.s('rts')
        ks.append(k)

    k = K('h264_vpred8_plane', 'chroma Plane')
    plane(k, 8)
    ks.append(k)
    return ks


def intra_kernels():
    return i4_kernels() + i16_kernels() + c8_kernels()


RODATA = """
    .section .rodata
    .align 5
    .global h264_vpu_ramp16
h264_vpu_ramp16:
    .short -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8
    .global h264_vpu_ramp8
h264_vpu_ramp8:
    .short -3, -2, -1, 0, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0
    .text
"""


# ---------------------------------------------------------------------------

HEADER = """/***************************************************************************
 *             __________               __   ___.
 *   Open      \\______   \\ ____   ____ |  | _\\_ |__   _______  ___
 *   Source     |       _//  _ \\_/ ___\\|  |/ /| __ \\ /  _ \\  \\/  /
 *   Jukebox    |    |   (  <_> )  \\___|    < | \\_\\ (  <_> > <  <
 *   Firmware   |____|_  /\\____/ \\___  >__|_ \\|___  /\\____/__/\\_ \\
 *                     \\/            \\/     \\/    \\/            \\/
 *
 * GENERATED by bcmfw/h264/gen_vpu.py: do not edit.
 * %s
 *
 * Copyright (C) 2026 ProkBox contributors
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
"""

LUMA_H = (4, 8, 16)
CHROMA_H = (2, 4, 8)
CHROMA_KINDS = ('copy', 'h', 'v', 'hv')


def kernels():
    ks = []
    for h in LUMA_H:
        for case in range(16):
            ks.append(luma_kernel(h, case))
    for h in CHROMA_H:
        for kind in CHROMA_KINDS:
            ks.append(chroma_kernel(h, kind))
    ks += idct_kernels()
    ks += dbk_kernels()
    ks += intra_kernels()
    return ks


def gen_s(ks):
    out = [HEADER % 'The VideoCore II vector kernels (decoder.md, "Vector kernels").',
           '    .syntax vc2', '    .text']
    for k in ks:
        out += k.text()
    out.append(RODATA)
    return '\n'.join(out) + '\n'


def gen_h(ks):
    out = [HEADER % 'Prototypes and dispatch tables of vpu_kernels.S.',
           '#ifndef H264_VPU_KERNELS_H', '#define H264_VPU_KERNELS_H', '',
           '#include <stdint.h>', '',
           'typedef void h264_vmc_fn(uint8_t *dst, int dstride, const uint8_t *src,',
           '                         int sstride, int mask, int phase);', '']
    for k in ks:
        if k.name.startswith('h264_vmc_'):
            out.append('h264_vmc_fn %s;' % k.name)
    out += ['', '/* one loop-filter edge: pix = q0 of the first line, stride = the',
            ' * plane width, mask = lanes (lines) to filter, tc0 = 16 bytes */',
            'typedef void h264_vdbk_fn(uint8_t *pix, int stride, int alpha, int beta,',
            '                          int mask, const void *tc0);']
    for k in ks:
        if k.name.startswith('h264_vdbk_'):
            out.append('h264_vdbk_fn %s;' % k.name)
    out += ['', '/* intra prediction: dst, stride and two kernel arguments (see',
            ' * gen_vpu.py, "Intra prediction") */',
            'typedef void h264_vpred_fn(uint8_t *dst, int stride, int a, int b);']
    for k in ks:
        if k.name.startswith('h264_vpred'):
            out.append('h264_vpred_fn %s;' % k.name)
    out += ['', '/* [h >> 3][fy << 2 | fx], h = 4, 8, 16 */',
            '#define H264_VMC_LUMA_TABLE { \\']
    for h in LUMA_H:
        names = ['h264_vmc_luma%d_%d%d' % (h, c & 3, c >> 2) for c in range(16)]
        out.append('    { %s }, \\' % ', '.join(names))
    out += ['}', '', '/* [h >> 2][kind], h = 2, 4, 8; kind = (fx != 0) | (fy != 0) << 1 */',
            '#define H264_VMC_CHROMA_TABLE { \\']
    for h in CHROMA_H:
        names = ['h264_vmc_chroma%d_%s' % (h, kd) for kd in ('copy', 'h', 'v', 'hv')]
        out.append('    { %s }, \\' % ', '.join(names))
    out += ['}', '', '#endif', '']
    return '\n'.join(out)


def main(argv):
    ks = kernels()
    files = {os.path.join(HERE, 'vpu_kernels.S'):
             prokbox_header.convert(gen_s(ks), '.S')[0],
             os.path.join(HERE, 'vpu_kernels.h'):
             prokbox_header.convert(gen_h(ks), '.h')[0]}
    # forms that drop their offset on silicon (device sessions 18-21): a B
    # operand's +rN without ++, and a vector memory op's offset without a
    # stride register
    text = files[os.path.join(HERE, 'vpu_kernels.S')]
    bad = re.findall(r'\bb[0-9a-f]{2}(?:\.[0-3])?(?:\.t)?\+r\d(?!\+\+)', text)
    bad += re.findall(r'v(?:ld|st)\w* \S+, \(r\d[+-]\d+\)(?!\+=)', text)
    # a scalar B operand in r7 reads as 0 on silicon (device session 22)
    bad += re.findall(r'^\s+v[a-z]+[.0-9]* .*, r7(?: |$)', text, re.M)
    # vmulhr (not vmulhrs/vmulhru) reads B signed (device session 26)
    bad += re.findall(r'^\s+vmulhr\.\d .*#0x[89a-f][0-9a-f]{3}', text, re.M)
    if bad:
        raise SystemExit('gen_vpu: forms that drop their offset on silicon: %s'
                         % ', '.join(bad[:4]))
    if '--check' in argv:
        stale = [p for p, t in files.items()
                 if not os.path.exists(p) or open(p).read() != t]
        if stale:
            print('gen_vpu: stale: %s' % ', '.join(map(os.path.basename, stale)))
            return 1
        print('gen_vpu: up to date')
        return 0
    for p, t in files.items():
        with open(p, 'w') as f:
            f.write(t)
    n = sum(len([l for l in k.lines if not l.endswith(':')]) for k in ks)
    print('gen_vpu: %d kernels, %d instructions' % (len(ks), n))
    return 0


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
