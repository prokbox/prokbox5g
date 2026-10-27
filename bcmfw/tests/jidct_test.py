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

"""jdec's IDCTs on the vector unit (jv_idct4n, bcmfw/video/gen_jdec.py;
jhook.h jdec_idct; jpeg-spike.md sections 13-14), block by block in
vc2sim against the decoder's own C (jpeg_load.c jpeg_idct4v/4h, 8v/8h and
16v/16h, built by vc2cc with it: bcmfw/jpeg/jidct.c), at each size (4, 8
and 16 points):

1. Blocks from the corpus: the blocks jdec sends the kernel while it
   decodes those files (dumped on the Mac by jidct.c with JIDCT_DUMP; the
   ones with no AC coefficient the decoder makes itself, so the made
   blocks below cover those).
2. Made blocks: any int16 values; values in +-2047 and +-2048 (the
   prototype's pass-1 limit, which this kernel doesn't have: every block
   takes it); sparse ones; DC only; the extremes -32768 and 32767; the
   start-up check's rounding probes; zero.
   Every pixel of every block must equal the C's, the kernel must leave
   its coefficients 0, and the C must equal this file's model of it
   (gen_jdec's 1-D transforms), so the model and the C keep each other
   honest.
3. The put and the fill (jv_put, jv_fill; jhook.h jdec_put, jdec_fill)
   against jpeg_load.c's C put as it was (jidct.c jdec_put_c,
   jdec_fill_c), at each size: random row buffers (row steps of 32 to 192
   bytes), one to four blocks a call at random places and components,
   grey or not, and blocks that share lines (Cb and Cr over one place, two
   4-point blocks side by side), every byte of the buffer compared.
4. The start-up check (jdec_vpu.c idct_check) passes.  Under the
   accumulator rule silicon settled (vc2sim v_acc_sat sat32: ACSA) and
   the vertical D's +r y it settled (v_vd_reg_y align16: PUTV), and under
   the old models, the kernel stays exact and the check passes; the
   one-chain form before (rep8) fails it under sat32 (and passes only
   under the wrapping model silicon refuted), and the put before (puty,
   +r y 8 or 12 on a V8 D) fails it under align16 (and passes only under
   the old 'given' model): the check's failure on silicon in sitting 2.
5. --mutations: gen_jdec.py --mutate round, x15, lane (a rounding term
   one off, the low halves of pass 2's products dropped, a pixel in the
   wrong lane), m4, m16 (one constant of the 4- or 16-point matrix one
   off), store16 (the 16-point's blocks 2 and 3 not stored) and rep8
   (the eight products in one REP 8 chain, the form before, which
   saturates past 2^31 under vc2sim's silicon accumulator): each must
   make blocks differ, and all but the matrix ones the start-up check
   fail too (a constant one off rarely moves a pixel of its four blocks;
   it is the image's data, which this test checks).  mask, stride, dc
   and puty (the fill's lane mask one lane on, the put's interleave
   stride 3, the fill's value one off, the put's V8 D at +r y 8 or 12):
   each must make puts or fills differ and the start-up check fail.  The kernel is regenerated after.

Usage: bcmfw/tests/jidct_test.py [--all] [--mutations]
  default: up to 8,000 corpus blocks a size; --all: every one.
Needs the corpus (tests/jpeg/make_corpus.sh)."""
import array
import os
import random
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
ROOT = os.path.dirname(FW)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
sys.path.insert(0, os.path.join(FW, 'video'))
import jpeg_test as J   # noqa: E402
import vc2cc as C       # noqa: E402
import vc2sim as S      # noqa: E402
import mutant           # noqa: E402

OUT = os.path.join(ROOT, 'build', 'jidct')
GEN = os.path.join(FW, 'video', 'gen_jdec.py')
SIZES = (4, 8, 16)
# (file, w, h): at its own size every component's blocks are 8 points
# (4:4:4, grey) or the luma's (4:2:0, 4:2:2) and a 4:2:0 file's chroma 16;
# at half size the luma's 4, a 4:2:0 file's chroma 8
DUMPS = (('t600_420_q85.jpg', 600, 600), ('t600_420_q85.jpg', 300, 300),
         ('t600_420_q95.jpg', 600, 600), ('t600_420_q50.jpg', 600, 600),
         ('t600_444_q85.jpg', 600, 600), ('t600_grey_q85.jpg', 600, 600),
         ('t600_grey_q85.jpg', 300, 300), ('t600_422_q85.jpg', 600, 600),
         ('t600_420_rst.jpg', 300, 300), ('w640x360_420_q85.jpg', 640, 360),
         ('noise500_420_q85.jpg', 500, 500), ('noise500_420_q85.jpg', 250, 250),
         ('noise500_444_rst.jpg', 500, 500), ('h333x500_420_q95.jpg', 333, 500),
         ('h333x500_420_q95.jpg', 167, 250), ('h333x500_444_q85.jpg', 333, 500))
QUICK = 8000
# jdec_vpu.c idct_probe: one column whose pass-1 output 0 is 16 << 11 and
# 16 << 11 - 1 before the shift, so a rounding term one off moves a row
PROBES = {8: ((0, 1, 1, -1, 0, 3, 0, 0), (4, 0, -1, -1, 0, 3, 0, 0)),
          4: ((-12, 25, -13, -7), (12, 4, 11, -45)),
          16: ((1, 1, 2, 0, -1, 0, 0, 0), (-1, 1, 1, -1, 0, 1, 0, 2))}
AT = 0x800000           # blocks, then the kernel's and the C's pixels
CHUNK = 1024            # blocks a call


def s16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


def model(F, size):
    """jpeg_load.c's size-point IDCT on F[v][u] (natural order, 64
    values), from gen_jdec's 1-D transforms: the int16 workspace, then
    range_limit"""
    import gen_jdec as G
    fn, inputs = G.IDCT_1D[size]
    Y = [[0] * inputs for _ in range(size)]
    for u in range(inputs):
        col = [F[8 * v + u] for v in range(inputs)]
        for n, t in enumerate(fn(col, 0)):
            Y[n][u] = s16((t + G.P1_BIAS[size][n]) >> 11)
    out = []
    for n in range(size):
        for t in fn(Y[n], (16 + (128 << 5)) << 13):
            t &= 0xffffffff
            t = (t - (1 << 32) if t & 0x80000000 else t) >> 18
            out.append(max(0, min(255, t)))
    return out


def made_blocks(rng, n, size):
    out = []
    for k in range(n):
        kind = k % 7
        if kind == 0:                           # anything
            b = [rng.randint(-32768, 32767) for _ in range(64)]
        elif kind == 1:                         # the old pass-1 limit and past it
            b = [rng.choice((-2048, -2047, 2047, 2048, rng.randint(-2048, 2048)))
                 for _ in range(64)]
        elif kind == 2:                         # sparse, like real blocks
            b = [0] * 64
            b[0] = rng.randint(-1024, 1016)
            for _ in range(rng.randint(1, 12)):
                p = rng.randrange(64)
                b[p] = rng.randint(-300, 300) // (1 + (p >> 3) + (p & 7))
        elif kind == 3:                         # DC only
            b = [rng.randint(-32768, 32767) if rng.random() < 0.3 else
                 rng.randint(-1100, 1100)] + [0] * 63
        elif kind == 4:                         # the extremes
            b = [rng.choice((-32768, 32767, 0)) for _ in range(64)]
        elif kind == 5:                         # one coefficient, at the edge
            b = [0] * 64
            b[rng.randrange(64)] = rng.choice((-32768, 32767, -2048, 2047))
        elif k % 14 == 6:                       # the check's rounding probes
            b = [0] * 64
            for v, x in enumerate(PROBES[size][k // 14 % 2]):
                b[8 * v] = x
        else:
            b = [0] * 64
        out.append(b)
    return out


def mac_dump():
    """the corpus' blocks as jdec sends them (the Mac's jdec), by size"""
    os.makedirs(OUT, exist_ok=True)
    env = ['-I' + os.path.join(J.JPEG, 'env'), '-I' + J.REC, '-I' + J.JOBS, '-I' + FW,
           '-I' + J.JPEG]
    objs = []
    for src in ([os.path.join(J.REC, f) for f in ('jpeg_load.c', 'resize.c', 'bmp.c')] +
                [os.path.join(J.JOBS, 'jdec.c'), os.path.join(J.JPEG, 'jdone.c'),
                 os.path.join(J.JPEG, 'jidct.c')]):
        o = os.path.join(OUT, os.path.basename(src)[:-2] + '.o')
        r = subprocess.run(['cc', '-std=gnu11', '-O2', '-w', '-DJIDCT_DUMP', '-c', src, '-o', o] +
                           env, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError('Mac build of %s:\n%s' % (src, r.stderr))
        objs.append(o)
    exe = os.path.join(OUT, 'jidct_dump')
    subprocess.run(['cc', '-o', exe] + objs, check=True)
    blocks = {n: [] for n in SIZES}
    for name, w, h in DUMPS:
        path = os.path.join(J.CORPUS, name)
        if not os.path.exists(path):
            continue
        r = subprocess.run([exe, path, str(w), str(h)], capture_output=True, check=True)
        a = array.array('h')
        a.frombytes(r.stdout)
        got = {n: 0 for n in SIZES}
        for i in range(0, len(a), 65):
            blocks[a[i]].append(list(a[i + 1:i + 65]))
            got[a[i]] += 1
        print('  %-24s %4d x %-4d %s' % (name, w, h, ', '.join(
            '%d at %d' % (got[n], n) for n in SIZES if got[n])))
    return blocks


class Sim:
    def __init__(self):
        self.prog = J.vc_build(extra=[os.path.join(J.JPEG, 'jidct.c')])
        self.s = S.Sim()
        C.load_into_sim(self.prog, self.s)

    def check(self):
        return self.s.call('jt_check', max_steps=10 ** 8)

    def put(self, case):
        """one jt_put call: the kernel's and the C's row buffers"""
        s = self.s
        size, step, rows, offs, grey, v, buf, px = case
        nb = len(buf)
        b_vec, b_ref = AT, AT + nb
        b_px, b_offs = AT + 2 * nb, AT + 2 * nb + 1024
        s.mem.b[b_vec:b_vec + nb] = buf
        s.mem.b[b_ref:b_ref + nb] = buf
        s.mem.b[b_px:b_px + 1024] = px
        s.mem.b[b_offs:b_offs + 16] = array.array('I', offs + [0] * (4 - len(offs))).tobytes()
        s.call('jt_put', b_px, b_vec, b_ref, b_offs, len(offs), step,
               stack_args=(size, grey, v & 0xffffffff), max_steps=10 ** 8)
        return bytes(s.mem.b[b_vec:b_vec + nb]), bytes(s.mem.b[b_ref:b_ref + nb])

    def run(self, blocks, size):
        """the kernel's and the C's pixels of every block, and the first
        group (from 1) the kernel left nonzero coefficients in, or 0"""
        s, vec, ref, bad, ss = self.s, [], [], 0, size * size
        for i in range(0, len(blocks), CHUNK):
            part = blocks[i:i + CHUNK]
            n = len(part)
            data = array.array('h', [x for b in part for x in b]).tobytes()
            v_at = AT + len(data)
            r_at = v_at + ss * n
            s.mem.b[AT:AT + len(data)] = data
            rc = s.call('jt_blocks', AT, n, v_at, r_at, size, max_steps=4 * 10 ** 9)
            if rc and not bad:
                bad = i // 4 + rc
            vec += [bytes(s.mem.b[v_at + ss * k:v_at + ss * k + ss]) for k in range(n)]
            ref += [bytes(s.mem.b[r_at + ss * k:r_at + ss * k + ss]) for k in range(n)]
        return vec, ref, bad


def compare(sim, blocks, size, label):
    t0 = time.time()
    vec, ref, bad = sim.run(blocks, size)
    diff = [k for k in range(len(blocks)) if vec[k] != ref[k]]
    mdiff = [k for k in range(len(blocks)) if list(ref[k]) != model(blocks[k], size)]
    print('%-30s %3d points %6d blocks: kernel vs C %d differ; C vs model %d differ; '
          'coefficients %s (%.0f s)' % (label, size, len(blocks), len(diff), len(mdiff),
                                        'left 0' if not bad else 'NOT 0 from group %d' % bad,
                                        time.time() - t0))
    for k in diff[:3]:
        print('    block %d: %s\n      kernel %s\n      C      %s' % (
            k, blocks[k][:16], list(vec[k][:16]), list(ref[k][:16])))
    return len(diff) + len(mdiff) + (1 if bad else 0)


def put_cases(rng, n, size):
    """jt_put's cases: (size, row step, rows, offsets, grey, v (-1: the
    put), buffer, px).  The row buffer is 32-byte aligned (AT); blocks at
    multiples of their width, at rows 0 or size; the next block often in
    the same place (another component: Cb then Cr) or, at 4 points, the
    other half of the line"""
    out = []
    w = 4 * size                                # a block row's bytes
    for k in range(n):
        step = 32 * rng.randint(max(1, w // 32), 6)
        rows = 2 * size
        grey = k % 5 == 4
        put = k % 2 == 0
        offs = []
        for q in range(rng.randint(1, 4)):
            if offs and rng.random() < 0.5:
                o = offs[-1] & ~3
                if size == 4 and rng.random() < 0.5:
                    o ^= 16 if (o % 32) in (0, 16) else 0
            else:
                o = rng.randrange(step // w) * w + rng.randrange(2) * size * step
            offs.append(o + (0 if grey else rng.randrange(3)))
        out.append((size, step, rows, offs, int(grey), -1 if put else rng.randrange(256),
                    bytes(rng.randrange(256) for _ in range(step * rows)),
                    bytes(rng.randrange(256) for _ in range(1024))))
    return out


def compare_puts(sim, cases, label):
    t0 = time.time()
    bad = []
    for i, c in enumerate(cases):
        vec, ref = sim.put(c)
        if vec != ref:
            bad.append(i)
    sizes = sorted({c[0] for c in cases})
    print('%-30s %s points %5d calls (%d puts, %d fills): kernel vs C %d differ (%.0f s)' % (
        label, '/'.join(map(str, sizes)), len(cases), sum(c[5] < 0 for c in cases),
        sum(c[5] >= 0 for c in cases), len(bad), time.time() - t0))
    for i in bad[:3]:
        c = cases[i]
        print('    case %d: size %d step %d offsets %s grey %d v %d' % ((i,) + c[:2] + c[3:6]))
    return len(bad)


# the mutants go to this process's builds only, never over the tracked
# jdec_kernels.S, which concurrent benches build their images from
# (mutant.py); regen() with no mutant: the tracked kernel again
regen = mutant.switcher(GEN)


# a constant of M one off is a fault in the image's own data, not in the
# vector unit: this test must see it (blocks differ); the start-up check,
# which is there for the chip, needn't (it rarely moves a pixel of its
# four blocks)
TABLE_MUTANTS = ('m4', 'm16')
MUTANTS = {'round': (4, 8, 16), 'x15': (4, 8, 16), 'lane': (8,), 'm4': (4,), 'm16': (16,),
           'store16': (16,), 'rep8': (16,)}
# the put's and the fill's: the IDCT's blocks are unchanged, so these are
# judged by the put cases (and the start-up check)
PUT_MUTANTS = ('mask', 'stride', 'dc', 'puty')


def variants(made):
    """The kernel and the start-up check under every rule that fits
    silicon (vc2vpu.SILICON_FITS v_acc_sat: X15S, REPA, ACSA; v_vd_reg_y:
    PUTV) and the old models: the kernel is exact and the check passes
    under each.  The forms before fail where silicon does: --mutate rep8
    (one REP 8 chain) differs and fails the check under the saturating
    accumulator, passing only under the wrapping model silicon refuted;
    --mutate puty (the put's copy in rows 16 - size .. 15 through a
    vertical D's +r y of 8 or 12) fails the check under the aligned y,
    passing only under the old 'given' model: sitting 2's J_VPU 0x23,
    MAC clear, on the display-only image"""
    import itertools
    import vc2vpu as VP
    acc_rules = VP.SILICON_FITS['v_acc_sat'] + ('none',)
    vd_rules = VP.SILICON_FITS['v_vd_reg_y'] + ('given',)
    blocks = made[16][:140]
    bad = 0
    saved = {k: S.HYP[k] for k in ('v_acc_sat', 'v_vd_reg_y')}
    strict = S.STRICT_ALIGN
    try:
        for kernel in (None, 'rep8', 'puty'):
            regen(kernel)
            if kernel == 'puty':
                # strict mode refuses the form before outright (a V D's
                # +r y off 16); then each rule's own reading, strict off
                try:
                    Sim().check()
                    refused = False
                except S.AlignTrap:
                    refused = True
                bad += not refused
                print('puty under strict alignment: %s' % (
                    'refused (a V D +r y of 8): ok' if refused else 'NOT refused: WRONG'))
                S.set_strict_align('')
            for acc, vd in itertools.product(acc_rules, vd_rules):
                S.HYP['v_acc_sat'], S.HYP['v_vd_reg_y'] = acc, vd
                ms = Sim()
                chk = ms.check()
                vec, ref, _ = ms.run(blocks, 16)
                nd = sum(1 for a, b in zip(vec, ref) if a != b)
                if kernel == 'puty':        # the IDCT is the same; the put isn't
                    ok = chk == (1 if vd == 'given' else 0) and nd == 0
                elif kernel == 'rep8' and acc != 'none':
                    ok = chk == 0 and nd > 0
                else:
                    ok = chk == 1 and nd == 0
                bad += not ok
                print('accumulator %-6s, V D +r y %-7s %-12s: start-up check %s, %d of %d '
                      '16-point blocks differ: %s' % (
                          acc, vd, kernel or 'as built', 'passes' if chk == 1 else 'fails',
                          nd, len(blocks), 'ok' if ok else 'WRONG'))
    finally:
        S.set_strict_align(strict)
        S.HYP.update(saved)
        regen()
    return bad


def main(argv):
    if not os.path.exists(C.GCC):
        print('skip: vc4-elf-gcc not found')
        return 0
    if not os.path.isdir(J.CORPUS):
        print('jidct_test: corpus missing: tests/jpeg/make_corpus.sh')
        return 1
    t0 = time.time()
    print('corpus blocks (the Mac\'s jdec, as it sends them to the kernel):')
    corpus = mac_dump()
    rng = random.Random(2722)
    made = {n: made_blocks(rng, 1400, n) for n in SIZES}
    if '--all' not in argv:
        corpus = {n: rng.sample(b, QUICK) if len(b) > QUICK else b for n, b in corpus.items()}
    fails = 0
    sim = Sim()
    ok = sim.check()
    print('start-up check (jdec_vpu.c idct_check): %s' % ('passes' if ok == 1 else 'FAILS'))
    fails += ok != 1
    for n in SIZES:
        fails += compare(sim, corpus[n], n, 'corpus')
        fails += compare(sim, made[n], n, 'made (any, +-2048, DC, probes)')
    puts = [c for n in SIZES for c in put_cases(rng, 300, n)]
    fails += compare_puts(sim, puts, 'put and fill')
    fails += variants(made)
    if '--mutations' in argv:
        caught = 0
        try:
            for m, sizes in MUTANTS.items():
                regen(m)
                ms = Sim()
                chk = ms.check()
                nd = tot = 0
                for n in sizes:
                    vec, ref, bad = ms.run(made[n][:280] + corpus[n][:280], n)
                    nd += sum(1 for a, b in zip(vec, ref) if a != b)
                    tot += len(vec)
                hit = nd > 0 and (chk == 0 or m in TABLE_MUTANTS)
                caught += hit
                print('mutation %-8s: %d of %d blocks differ (%s points), start-up check %s: %s' % (
                    m, nd, tot, '/'.join(map(str, sizes)), 'fails' if chk == 0 else 'passes',
                    'caught' if hit else 'NOT CAUGHT'))
            for m in PUT_MUTANTS:
                regen(m)
                strict = S.set_strict_align('' if m == 'puty' else S.STRICT_ALIGN)
                ms = Sim()
                chk = ms.check()
                nd = sum(1 for c in puts[::5] if len(set(ms.put(c))) > 1)
                S.set_strict_align(strict)
                hit = nd > 0 and chk == 0
                caught += hit
                print('mutation %-8s: %d of %d put calls differ, start-up check %s: %s' % (
                    m, nd, len(puts[::5]), 'fails' if chk == 0 else 'passes',
                    'caught' if hit else 'NOT CAUGHT'))
        finally:
            regen()
        fails += len(MUTANTS) + len(PUT_MUTANTS) - caught
    print('jidct_test: %d failed (%.0f s)' % (fails, time.time() - t0))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
