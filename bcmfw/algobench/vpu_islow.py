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
"""A candidate: jpeg_load.c's exact 8x8 islow IDCT on the vector unit,
through the 32-bit accumulator (hw5[15] 'x15' + vmulhd, isa.md "The
32-bit accumulator").  Prototype only: not wired into jdec or any image
(algorithms.md item 1).

Layout, four blocks q = 0..3 (an MCU's 2 x 2 luma, or any four):
  input   H16 rows 8g + j (x 0): lanes 8p + c = F[j][c] of block 2g + p
          (natural coefficient row j, column c), g = 0, 1
  pass 1  lanes = columns, 16 per group; output n of group g written
          as a V16 column at (16, n + 8g): VRF row 16 + 8p + c, lane
          8g + n, so the transpose is free (H/V aliasing)
  pass 2  group p reads H16 rows 16 + 8p + c (lanes 8g + n = block row
          n); output k as a V8 column at (32, k + 8p): H8 row 32 + 8g + n
          is [block 2g row n | block 2g + 1 row n], 16 pixels
Every product is split into vmul ... x15 (low half) and vmulhd ... acc
(floored high half), so each output's sum is exact in 32 bits.  Pass 1
prescales its inputs by 16 and its constants by 2 (32 A, read as the
high half: (A + 1024) >> 11 directly), so it needs |F| <= 2047 (every
baseline file's coefficients fit; jdec would take the C path for a
block that doesn't).  Pass 2 takes any int16 workspace value, as C does.

Usage: bcmfw/algobench/vpu_islow.py [--blocks FILE] [--ref EXE] [--scalar] [-n N]
  FILE: int16 blocks in natural order (64 each), e.g. dumped from jdec's
  decode loop before its 8x8 IDCTs; EXE: idct_bench.c built on the Mac
  (jpeg_load.c's own islow) to check this file's Python reference
  against the C, block for block:
      cc -std=gnu11 -O2 -w -Wl,-dead_strip -DIDCT_BENCH_MAIN -o EXE \
          bcmfw/algobench/idct_bench.c -Ibcmfw/jpeg/env -Iapps/recorder \
          -Ibcmfw/jobs -Ibcmfw -Ibcmfw/jpeg
  --scalar: the same C islow built by vc2cc, run in vc2sim on (up to
  6,000 of) FILE's blocks: its instructions per block."""
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(FW, 'h264'))
import vpu_asm as V                            # noqa: E402
from vpu_asm import H16, V8, V16, DISCARD     # noqa: E402

D, VP = V.D, V.VP

# jpeg_load.c's constants (CONST_BITS 13)
F0298, F0390, F0541, F0765 = 2446, 3196, 4433, 6270
F0899, F1175, F1501, F1847 = 7373, 9633, 12299, 15137
F1961, F2053, F2562, F3072 = 16069, 16819, 20995, 25172


def s16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    v &= 0xffffffff
    return v - 0x100000000 if v & 0x80000000 else v


def idct1(x, dc_bias):
    """jpeg_load.c's 8-point islow on x[0..7], before the final shift:
    the eight 32-bit sums (out0..out7).  dc_bias is added to x0 << 13."""
    z2, z3 = x[2], x[6]
    z1 = (z2 + z3) * F0541
    tmp2 = z1 + z3 * -F1847
    tmp3 = z1 + z2 * F0765
    tmp0 = (x[0] << 13) + dc_bias + (x[4] << 13)
    tmp1 = (x[0] << 13) + dc_bias - (x[4] << 13)
    t10, t13, t11, t12 = tmp0 + tmp3, tmp0 - tmp3, tmp1 + tmp2, tmp1 - tmp2
    o0, o1, o2, o3 = x[7], x[5], x[3], x[1]
    z1, z2, z3, z4 = o0 + o3, o1 + o2, o0 + o2, o1 + o3
    z5 = (z3 + z4) * F1175
    o0, o1, o2, o3 = o0 * F0298, o1 * F2053, o2 * F3072, o3 * F1501
    z1, z2, z3, z4 = z1 * -F0899, z2 * -F2562, z3 * -F1961 + z5, z4 * -F0390 + z5
    o0 += z1 + z3
    o1 += z2 + z4
    o2 += z2 + z3
    o3 += z1 + z4
    return [s32(v) for v in (t10 + o3, t11 + o2, t12 + o1, t13 + o0,
                             t13 - o0, t12 - o1, t11 - o2, t10 - o3)]


def ref_block(F):
    """F[v][u] -> 8 x 8 pixels, as jpeg_idct8v then jpeg_idct8h compute
    them (int16 workspace, the all-zero-AC shortcuts, range_limit)."""
    Y = [[0] * 8 for _ in range(8)]
    for c in range(8):
        col = [F[j][c] for j in range(8)]
        if not any(col[1:]):
            out = [s16(col[0] << 2)] * 8
        else:
            out = [s16(v >> 11) for v in idct1(col, 1 << 10)]
        for n in range(8):
            Y[n][c] = out[n]
    P = []
    for n in range(8):
        row = Y[n]
        if not any(row[1:]):
            v = 128 + ((row[0] + 16) >> 5)
            P.append([max(0, min(255, v))] * 8)
            continue
        out = idct1(row, (16 + (128 << 5)) << 13)
        P.append([max(0, min(255, v >> 18)) for v in out])
    return P


def matrix():
    """The 1-D transform as an integer matrix: out_n = sum M[n][j] x_j
    (+ the bias), read off idct1 on unit vectors (it is linear)."""
    M = [[0] * 8 for _ in range(8)]
    for j in range(8):
        e = [0] * 8
        e[j] = 1
        for n, v in enumerate(idct1(e, 0)):
            M[n][j] = v
    return M


# --------------------------------------------------------------------------
# The vector program.

def program():
    M = matrix()
    assert all(M[n][0] == 8192 and abs(M[n][4]) == 8192 for n in range(8))
    assert max(abs(2 * M[n][j]) for n in range(8) for j in range(8)) < 32768
    P = []
    # pass 1, group g: prescale (x16) the inputs that get a full product,
    # and s = (F0 + F4) * 4, d = (F0 - F4) * 4 (the 2^18 term: high half only)
    for g in range(2):
        r = 8 * g
        for j in (1, 2, 3, 5, 6, 7):
            P.append(V.alu('vshl', H16(r + j, 32), H16(r + j, 0), 4))
        P.append(V.alu('vadd', H16(r, 32), H16(r, 0), H16(r + 4, 0)))
        P.append(V.alu('vshl', H16(r, 32), H16(r, 32), 2))
        P.append(V.alu('vsub', H16(r + 4, 32), H16(r, 0), H16(r + 4, 0)))
        P.append(V.alu('vshl', H16(r + 4, 32), H16(r + 4, 32), 2))
        for n in range(8):
            # 32 * 1024 = 0x8000: the low half of the rounding term
            P.append(V.alu('vmov', DISCARD, None, -32768, clra=True, x15=True))
            P.append(V.alu('vmov', DISCARD, None, H16(r + (0 if M[n][4] > 0 else 4), 32),
                           acc=True))
            js = (1, 2, 3, 5, 6, 7)
            for j in js:
                c = 2 * M[n][j]
                P.append(V.alu('vmul', DISCARD, H16(r + j, 32), c, x15=True))
                P.append(V.alu('vmulhd', V16(16, n + 8 * g) if j == js[-1] else DISCARD,
                               H16(r + j, 32), c, acc=True))
    # pass 2, group p: rows 16 + 8p + c; outputs to V8 columns k + 8p
    for p in range(2):
        r = 16 + 8 * p
        for k in range(8):
            # (16 + 4096) << 13 = 514 << 16
            P.append(V.alu('vmov', DISCARD, None, 514, clra=True, acc=True))
            for j in range(8):
                P.append(V.alu('vmul', DISCARD, H16(r + j, 0), M[k][j], x15=True))
                P.append(V.alu('vmulhd', H16(48, 0) if j == 7 else DISCARD,
                               H16(r + j, 0), M[k][j], acc=True))
            P.append(V.alu('vasr', H16(48, 0), H16(48, 0), 2))
            P.append(V.alu('vclip', V8(32, k + 8 * p), H16(48, 0), 255))
    return P


class _Sim:
    def __init__(self):
        self.vrf = VP.VRF()
        self.r = [0] * 32
        self.mem = None


class Runner:
    """The program decoded and compiled once by vc2sim's vector model."""
    def __init__(self, prog):
        self.sim = _Sim()
        self.code = [VP.compile_vector(self.sim, D.decode_words(D.assemble(t, 0), 0))
                     for t in prog]

    def __call__(self, blocks):
        return run(self, blocks)


def run(runner, blocks):
    """Four blocks (F[v][u] each) through the program; the four 8 x 8
    pixel blocks."""
    sim = runner.sim
    for g in range(2):
        for j in range(8):
            vals = [blocks[2 * g + (i >> 3)][j][i & 7] for i in range(16)]
            sim.vrf.write((8 * g + j) * 64, False, 2, vals, range(16))
    for h in runner.code:
        h()
    out = []
    for q in range(4):
        g, p = q >> 1, q & 1
        out.append([list(sim.vrf.b[(32 + 8 * g + n) * 64 + 8 * p:(32 + 8 * g + n) * 64 + 8 * p + 8])
                    for n in range(8)])
    return out


def scalar_counts(blocks):
    """vc2sim instructions per block of jpeg_load.c's islow, built by vc2cc
    as jdec is (idct_bench.c's idct8_blocks less its copies)."""
    sys.path[:0] = [os.path.join(FW, 'tests'), os.path.join(os.path.dirname(FW), 'tools', 'bcmre')]
    import array
    import jpeg_test as J
    import vc2cc as C
    import vc2sim as S
    lib = ('string.s', C.compile_c(os.path.join(FW, 'lib', 'string.c'), '-O2',
                                   ['-I' + FW, '-std=gnu99', '-fno-tree-loop-distribute-patterns']))
    prog = C.build([os.path.join(HERE, 'idct_bench.c')] +
                   [os.path.join(J.REC, f) for f in ('resize.c', 'bmp.c')] +
                   [os.path.join(J.JOBS, 'jdec.c'), os.path.join(J.JPEG, 'jdone.c'),
                    os.path.join(J.JPEG, 'libc.c'), os.path.join(FW, 'video', 'jdec_vpu.c'),
                    os.path.join(FW, 'video', 'jdec_kernels.S'),
                    os.path.join(FW, 'video', 'jobs_kernels.S'), lib],
                   '-O2', extra=J.CFLAGS, libs=C.DEFAULT_LIBS, no_float=True, conservative=True)
    sim = S.Sim()
    C.load_into_sim(prog, sim)
    at = 0x800000
    tot = {True: [0, 0], False: [0, 0]}
    for b in blocks[::max(1, len(blocks) // 6000)]:
        flat = [x for r in b for x in r]
        sim.mem.b[at:at + 128] = array.array('h', flat).tobytes()
        s0 = sim.steps
        sim.call('idct8_blocks', at, at + 0x1000, 1, max_steps=10 ** 7)
        s1 = sim.steps
        sim.call('idct8_copies', at, at + 0x1000, 1, max_steps=10 ** 7)
        t = tot[any(flat[1:])]
        t[0] += (s1 - s0) - (sim.steps - s1)
        t[1] += 1
    for ac, label in ((False, 'DC only'), (True, 'with AC')):
        n, c = tot[ac][1], tot[ac][0]
        print('scalar islow (vc2cc, vc2sim), %s: %d blocks, %.0f instructions a block'
              % (label, n, c / max(1, n)))


def rand_block(rng, kind):
    if kind == 'full':                       # every coefficient, any value in range
        return [[rng.randint(-2047, 2047) for _ in range(8)] for _ in range(8)]
    if kind == 'edge':                       # the extremes
        return [[rng.choice((-2047, 2047, -2048 + 1, 0)) for _ in range(8)] for _ in range(8)]
    F = [[0] * 8 for _ in range(8)]          # sparse, roughly like real blocks
    F[0][0] = rng.randint(-1024, 1016)
    for _ in range(rng.randint(0, 12)):
        v, u = rng.randrange(8), rng.randrange(8)
        F[v][u] = rng.randint(-300, 300) // (1 + v + u)
    return F


def main(argv):
    n = 64
    blocks_file = ref_exe = None
    if '-n' in argv:
        n = int(argv[argv.index('-n') + 1])
    if '--blocks' in argv:
        blocks_file = argv[argv.index('--blocks') + 1]
    if '--ref' in argv:
        ref_exe = argv[argv.index('--ref') + 1]
    prog = program()
    nops = len(prog)
    p2 = 2 * 8 * (1 + 16 + 2)
    print('program: %d vector ALU ops per 4 blocks (%.1f a block): pass 1 %d, pass 2 %d'
          % (nops, nops / 4.0, nops - p2, p2))
    rng = random.Random(2722)
    sets = [[rand_block(rng, k) for _ in range(4)]
            for k in ('full', 'edge', 'sparse') for _ in range(n)]
    if blocks_file:
        import array
        a = array.array('h')
        a.frombytes(open(blocks_file, 'rb').read())
        blk = [[[a[64 * b + 8 * v + u] for u in range(8)] for v in range(8)]
               for b in range(len(a) // 64)]
        big = [b for b in blk if max(abs(x) for r in b for x in r) > 2047]
        print('%s: %d blocks, %d outside |F| <= 2047 (C path)' % (blocks_file, len(blk), len(big)))
        blk = [b for b in blk if b not in big]
        if ref_exe:
            data = b''.join(array.array('h', [x for r in b for x in r]).tobytes() for b in blk)
            got = subprocess.run([ref_exe], input=data, capture_output=True, check=True).stdout
            bad = sum(1 for i, b in enumerate(blk)
                      if bytes(x for r in ref_block(b) for x in r) != got[64 * i:64 * i + 64])
            print('Python reference vs jpeg_load.c (Mac): %d of %d blocks differ' % (bad, len(blk)))
            if bad:
                return 1
        sets += [blk[i:i + 4] for i in range(0, len(blk) - 3, 4)]
        if '--scalar' in argv:
            scalar_counts(blk)
    bad = 0
    runner = Runner(prog)
    for s in sets:
        if runner(s) != [ref_block(b) for b in s]:
            bad += 1
    print('vector islow vs the reference: %d of %d four-block sets differ' % (bad, len(sets)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
