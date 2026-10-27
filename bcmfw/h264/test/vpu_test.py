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
"""The decoder's vector kernels against its C kernels, in vc2sim.

Builds the kernel files with H264_VPU=1 and H264_VPU_REF=1, so each
kernel exists twice: the vector version under its own name and the C
reference as <name>_c.  Both run on the same random inputs (uniform
bytes, and 0/255 extremes that reach every saturation and the 6-tap's
widest sums) and must leave memory identical, inside and around the
block.  Every call checks the ABI (callee-saved registers intact).

test/vpu_forms.py lists the instruction forms the kernels use that
Apple's kernels never run in the vc2sim self-test.

  bcmfw/h264/test/vpu_test.py [--seed N] [--n CASES] [--only mc,idct,dbk,intra]
"""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(SRC))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
sys.path.insert(0, SRC)
import vc2cc as C    # noqa: E402
import vc2sim as S   # noqa: E402

OUT = os.path.join(ROOT, 'build', 'h264', 'vpu')
FILES = ['mc.c', 'idct.c', 'deblock.c', 'intra.c', 'tables.c', 'vpu_kernels.S']
CFLAGS = ['-std=gnu99', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Werror',
          '-fno-tree-loop-distribute-patterns', '-DH264_VPU=1', '-DH264_VPU_REF=1']

SRCMEM, DSTMEM, AUXMEM = 0x400000, 0x500000, 0x600000


def build():
    os.makedirs(os.path.join(OUT, 'lib'), exist_ok=True)
    extra = ['-I' + SRC] + CFLAGS
    text = C.compile_c(os.path.join(ROOT, 'bcmfw', 'lib', 'string.c'), '-O2',
                       extra + ['-I' + os.path.join(ROOT, 'bcmfw')])
    with open(os.path.join(OUT, 'lib', 'string.s'), 'w') as f:
        f.write(text)
    return C.build([os.path.join(SRC, f) for f in FILES], '-O2', extra=extra,
                   libs=[os.path.join(OUT, 'lib')] + C.DEFAULT_LIBS,
                   no_float=True, conservative=True)


class Row:
    def __init__(self, name):
        self.name, self.n, self.bad, self.first = name, 0, 0, None

    def check(self, ok, what):
        self.n += 1
        if not ok:
            self.bad += 1
            if self.first is None:
                self.first = what


def rand_bytes(rng, n, mode):
    if mode == 'extreme':
        return bytes(rng.choice((0, 255)) for _ in range(n))
    if mode == 'flat':
        b = rng.getrandbits(8)
        return bytes(min(255, max(0, b + rng.randrange(-6, 7))) for _ in range(n))
    return bytes(rng.getrandbits(8) for _ in range(n))


def run_pair(sim, name, args, region, background):
    """Call name_c and name on the same memory; return both regions."""
    out = []
    for fn in (name + '_c', name):
        sim.mem.write(region[0], background)
        # r15 and r22 are vc2cc's scratch registers (compiler.md)
        sim.call(fn, *args, check_abi=True, max_steps=2_000_000,
                 regs={15: 0, 22: 0})
        out.append(sim.mem.read(region[0], region[1]))
    return out


# ---------------------------------------------------------------------------

LUMA_SHAPES = [(16, 16), (16, 8), (8, 16), (8, 8), (8, 4), (4, 8), (4, 4)]
CHROMA_SHAPES = [(8, 8), (8, 4), (4, 8), (4, 4), (4, 2), (2, 4), (2, 2)]


def t_mc(sim, rng, n):
    rows = []
    for chroma in (False, True):
        r = Row('h264_mc_%s (all phases, shapes)' % ('chroma' if chroma else 'luma'))
        for _ in range(n):
            for w, h in (CHROMA_SHAPES if chroma else LUMA_SHAPES):
                fx = rng.randrange(8 if chroma else 4)
                fy = rng.randrange(8 if chroma else 4)
                if rng.random() < 0.25:
                    fx = 0 if rng.random() < 0.5 else fx
                    fy = 0 if rng.random() < 0.5 else fy
                ss = rng.choice((320, 160, 21, 9, 32, 88))
                # destination rows stay on 16-byte lines, as the decoder's
                # (strides 320/160, blocks at multiples of their width):
                # a vstr writes only within its address's line on silicon
                ds = rng.choice((320, 160, 96))
                mode = rng.choice(('random', 'random', 'extreme', 'flat'))
                sim.mem.write(SRCMEM, rand_bytes(rng, 40 * ss + 64, mode))
                src = SRCMEM + 8 * ss + 8 + rng.randrange(16)
                dst = DSTMEM + 4 * ds + w * rng.randrange(64 // w)
                region = (DSTMEM, 32 * ds)
                bg = rand_bytes(rng, region[1], 'random')
                fn = 'h264_mc_chroma' if chroma else 'h264_mc_luma'
                a, b = run_pair(sim, fn, (dst, ds, src, ss, w, h, fx, fy), region, bg)
                r.check(a == b, '%dx%d phase (%d,%d) sstride %d %s' % (w, h, fx, fy, ss, mode))
        rows.append(r)
    return rows


def t_idct(sim, rng, n):
    rows = []
    for dc in (False, True):
        name = 'h264_idct4_dc_add' if dc else 'h264_idct4_add'
        r = Row(name)
        for _ in range(30 * n):
            ds = rng.choice((320, 160, 88))
            # conforming streams keep the transform's intermediates in 16
            # bits (8.5.12.1): |c| <= 2600 does for any block
            lim = rng.choice((64, 600, 2600))
            if dc:
                c = [rng.randrange(-lim * 8, lim * 8)] + [0] * 15
            else:
                c = [rng.randrange(-lim, lim + 1) if rng.random() < 0.6 else 0
                     for _ in range(16)]
            coef = AUXMEM + 32 * rng.randrange(8)
            sim.mem.write(AUXMEM, rand_bytes(rng, 512, 'random'))
            sim.mem.write(coef, b''.join((v & 0xffff).to_bytes(2, 'little') for v in c))
            aux = sim.mem.read(AUXMEM, 512)
            dst = DSTMEM + 4 * ds + 4 * rng.randrange(16)
            region = (DSTMEM, 12 * ds)
            bg = rand_bytes(rng, region[1], rng.choice(('random', 'extreme', 'flat')))
            got = []
            # the vector transforms replace the C names by macro (h264_dsp.h)
            for fn in (name + '_c', name.replace('h264_', 'h264_v')):
                sim.mem.write(AUXMEM, aux)
                sim.mem.write(region[0], bg)
                sim.call(fn, dst, ds, coef, check_abi=True, max_steps=200_000,
                         regs={15: 0, 22: 0})
                got.append((sim.mem.read(region[0], region[1]),
                            sim.mem.read(AUXMEM, 512)))
            r.check(got[0] == got[1], 'stride %d lim %d' % (ds, lim))
        rows.append(r)
    return rows


# Table 8-16/8-17 ranges: alpha 4..255, beta 2..18, tC0 0..25
def t_dbk(sim, rng, n):
    rows = []
    for luma in (True, False):
        name = 'h264_dbk_luma' if luma else 'h264_dbk_chroma'
        r = Row('%s (both directions, bS 0-4)' % name)
        for _ in range(40 * n):
            W = rng.choice((320, 352) if luma else (160, 88))
            vert = rng.random() < 0.5
            mode = rng.choice(('flat', 'flat', 'flat', 'random', 'extreme'))
            sim.mem.write(DSTMEM, rand_bytes(rng, 24 * W, mode))
            if vert:
                x = 4 * rng.randrange(1, (W - 16) // 4)
                pix, xs, ys = DSTMEM + 4 * W + x, 1, W
            else:
                x = (16 if luma else 8) * rng.randrange(0, (W - 16) // 16)
                pix, xs, ys = DSTMEM + 8 * W + x, W, 1
            kind = rng.choice(('strong', 'normal', 'normal', 'mixed'))
            if kind == 'strong':
                bs = [4] * 4
            elif kind == 'normal':
                bs = [rng.randrange(4) for _ in range(4)]
            else:
                bs = [rng.choice((0, 1, 2, 3, 4)) for _ in range(4)]
            # tC0 comes from Table 8-17 by bS and the edge's indexA, so
            # equal bS means equal tC0 within an edge (the vector wrapper's
            # uniform path relies on it)
            tcb = {b: rng.randrange(26) for b in set(bs)}
            tc0 = [tcb[b] if b and b < 4 else 0 for b in bs]
            alpha = rng.choice((4, 15, 40, 90, 182, 255))
            beta = rng.choice((2, 4, 8, 13, 18))
            sim.mem.write(AUXMEM, bytes(bs) + bytes(4) + bytes(t & 255 for t in tc0))
            region = (DSTMEM, 24 * W)
            bg = sim.mem.read(DSTMEM, region[1])
            a, b = run_pair(sim, name, (pix, xs, ys, alpha, beta, AUXMEM, AUXMEM + 8),
                            region, bg)
            r.check(a == b, '%s %s bS %s alpha %d beta %d %s' % (
                'vertical' if vert else 'horizontal', kind, bs, alpha, beta, mode))
        rows.append(r)
    return rows


AV_L, AV_T, AV_TR, AV_TL = 1, 2, 4, 8


def t_intra(sim, rng, n):
    """Every mode and neighbour case against the C prediction: the return
    value (-1 when a needed neighbour is missing) and the memory around the
    block must match."""
    rows = []
    for size, modes, name in ((4, 9, 'h264_pred4x4'), (16, 4, 'h264_pred16x16'),
                              (8, 4, 'h264_pred_chroma8x8')):
        r = Row('%s (all modes, neighbour cases)' % name)
        for _ in range(12 * n):
            W = rng.choice((320, 352, 176) if size != 8 else (160, 176, 88))
            mode = rng.randrange(modes)
            avail = rng.randrange(16)
            if rng.random() < 0.7:          # mostly cases that predict
                avail |= AV_L | AV_T | AV_TL | (AV_TR if rng.random() < 0.6 else 0)
            mem = rng.choice(('random', 'random', 'extreme', 'flat'))
            region = (DSTMEM, 40 * W)
            bg = rand_bytes(rng, region[1], mem)
            align = 4 if size == 4 else 16 if size == 16 else 8
            x = align * rng.randrange(1, (W - 32) // align)
            dst = DSTMEM + 20 * W + x
            got = []
            for fn in (name + '_c', name):
                sim.mem.write(region[0], bg)
                ret = sim.call(fn, dst, W, mode, avail, check_abi=True,
                               max_steps=200_000, regs={15: 0, 22: 0})
                got.append((ret, sim.mem.read(region[0], region[1])))
            r.check(got[0] == got[1], 'mode %d avail %d stride %d x %d %s' % (
                mode, avail, W, x, mem))
        rows.append(r)
    return rows


TESTS = {'mc': t_mc, 'idct': t_idct, 'dbk': t_dbk, 'intra': t_intra}


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--n', type=int, default=6)
    ap.add_argument('--only')
    ap.add_argument('--model', action='store_true',
                    help="the corpus store model and clamping vmulhrs instead of the "
                         "silicon profile")
    a = ap.parse_args(argv)
    # The silicon profile is vc2vpu's default since device session 5: the
    # 48-bit store layout ('swap') and vmulhrs into 8 bits without a clamp
    # (isa.md, "Vector stores on silicon"); --model is the old reading
    if a.model:
        S.HYP['v_st48_nonzero'] = 'model'
        S.HYP['v_mulhrs_sat'] = True
    if not os.path.exists(C.GCC):
        print('vpu_test: vc4-elf-gcc not found (%s): skipped' % C.GCC)
        return 0
    prog = build()
    sim = C.load_into_sim(prog)
    rng = random.Random(a.seed)
    bad = 0
    # wrong on silicon (vc2vpu's silicon readings), run in C by the image
    # until fitted (h264_dsp.h H264_BAD_*): reported, not counted
    known_bad = ()
    for name, fn in TESTS.items():
        if a.only and name not in a.only.split(','):
            continue
        for r in fn(sim, rng, a.n):
            known = r.name.split(' ')[0] in known_bad and r.bad
            print('  %-44s %5d cases, %d failed%s%s' % (
                r.name, r.n, r.bad, ' (first: %s)' % r.first if r.first else '',
                ' [known bad on silicon: the image runs it in C]' if known else ''))
            bad += 0 if known else r.bad
    print('VPU KERNELS %s' % ('PASSED' if not bad else 'FAILED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
