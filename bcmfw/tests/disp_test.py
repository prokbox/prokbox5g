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
"""bcmfw's display kernels (bcmfw/video: YUV 4:2:0 -> RGB565, the 2:1
box) in vc2sim, bit for bit against the exact reference dispref.py.

Rows:
  - disp_yuv_tile on random tiles at every source alignment, plus a
    sweep of every Y value against chroma extremes (each of the 65,536
    (Y, V) and (Y, U) pairs at the other chroma's 0/128/255);
  - disp_box2_16 / disp_box2_8 on random and 0/255 data;
  - disp_convert on whole pictures (random sizes up to 320x240, cropped
    origins): the w x h result, and nothing written past the tiles;
  - disp_scale2 (vector and scalar paths) on pictures up to 640x480.
Every kernel call checks the ABI (callee-saved registers).

  bcmfw/tests/disp_test.py [-v] [--seed N] [--n CASES]
"""
import os
import random
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
ROOT = os.path.dirname(FW)
VID = os.path.join(FW, 'video')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
sys.path.insert(0, VID)
import vc2cc as C     # noqa: E402
import vc2sim as S    # noqa: E402
import dispref as R   # noqa: E402

OUT = os.path.join(ROOT, 'build', 'bcmfw', 'disp')
CFLAGS = ['-std=gnu99', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Werror',
          '-fno-tree-loop-distribute-patterns']
SRC, DST, STRIP = 0x400000, 0x800000, 0x0F0000


def build():
    os.makedirs(OUT, exist_ok=True)
    return C.build([os.path.join(VID, 'disp.c'), os.path.join(VID, 'disp_kernels.S')],
                   '-O2', extra=['-I' + VID] + CFLAGS, libs=C.DEFAULT_LIBS,
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


def call(sim, fn, *args):
    sim.call(fn, *args, check_abi=True, max_steps=50_000_000, regs={15: 0, 22: 0})


def rnd(rng, n, mode='random'):
    if mode == 'extreme':
        return bytes(rng.choice((0, 255)) for _ in range(n))
    return bytes(rng.getrandbits(8) for _ in range(n))


def u16s(b):
    return list(struct.unpack('<%dH' % (len(b) // 2), b))


def put_planes(sim, rng, w, h, ys, off, mode='random'):
    """Y, U, V planes (rows ys and ys/2 apart) at SRC + off; returns
    (yaddr, uaddr, vaddr, ybytes, ubytes, vbytes) of the w x h picture."""
    cs = ys // 2
    ysz = ys * (h + 16)
    csz = cs * (h // 2 + 8)
    data = rnd(rng, ysz + 2 * csz + 256, mode)
    sim.mem.write(SRC, data)
    ya, ua = SRC + off, SRC + off + ysz
    va = ua + csz
    yb = b''.join(data[off + r * ys:off + r * ys + w] for r in range(h))
    ub = b''.join(data[ua - SRC + r * cs:ua - SRC + r * cs + w // 2] for r in range(h // 2))
    vb = b''.join(data[va - SRC + r * cs:va - SRC + r * cs + w // 2] for r in range(h // 2))
    return ya, ua, va, yb, ub, vb


def t_tile(sim, rng, n):
    r = Row('disp_yuv_tile: random tiles, every source alignment')
    for k in range(n):
        # chroma strides (ys / 2) multiples of 16: disp_convert takes the C
        # for any other (the pairs' line phase would move: strict mode)
        ys = rng.choice((64, 96, 320, 160, 640, 224))
        off = rng.randrange(32) & ~1
        mode = 'extreme' if k % 4 == 3 else 'random'
        ya, ua, va, yb, ub, vb = put_planes(sim, rng, 32, 16, ys, off, mode)
        dp = rng.choice((64, 640, 128))
        bg = rnd(rng, dp * 16 + 64)
        sim.mem.write(DST, bg)
        call(sim, 'disp_yuv_tile', DST, dp, ya, ys, ua, va)
        got = sim.mem.read(DST, dp * 16 + 64)
        want = R.convert(yb, ub, vb, 32, 16)
        ok = all(u16s(got[row * dp:row * dp + 64]) == want[row * 32:row * 32 + 32]
                 for row in range(16))
        gaps = all(got[row * dp + 64:(row + 1) * dp] == bg[row * dp + 64:(row + 1) * dp]
                   for row in range(16)) and got[16 * dp:] == bg[16 * dp:]
        r.check(ok and gaps, 'ystride %d offset %d %s: %s' % (
            ys, off, mode, 'pixels differ' if not ok else 'wrote outside the tile'))
    return r


def t_sweep(sim, rng):
    """Every Y with every V (U fixed) and every U (V fixed), at chroma
    0/128/255 for the other: tiles of 32 x 16 = 512 pixels, chroma
    constant over each 2x2 block."""
    r = Row('disp_yuv_tile: every (Y, U) and (Y, V) pair, other chroma 0/128/255')
    for which in ('u', 'v'):
        for other in (0, 128, 255):
            # chroma sample c covers 4 pixels: Y values 4 at a time
            for c0 in range(0, 256, 128):           # 128 chroma values per tile
                # a tile has 8 x 16 chroma samples: 128 chroma values, each
                # with 4 luma values; repeat the tile for all 256 Y in 64 calls
                for yq in range(64):
                    yb = bytearray(32 * 16)
                    cb = bytearray(16 * 8)
                    for cy in range(8):
                        for cx in range(16):
                            c = c0 + cy * 16 + cx
                            cb[cy * 16 + cx] = c
                            for dy in range(2):
                                for dx in range(2):
                                    yb[(2 * cy + dy) * 32 + 2 * cx + dx] = \
                                        (yq * 4 + dy * 2 + dx + c) & 255
                    ob = bytes([other]) * 128
                    ub, vb = (bytes(cb), ob) if which == 'u' else (ob, bytes(cb))
                    sim.mem.write(SRC, bytes(yb))
                    sim.mem.write(SRC + 0x1000, ub)
                    sim.mem.write(SRC + 0x2000, vb)
                    call(sim, 'disp_yuv_tile', DST, 64, SRC, 32, SRC + 0x1000, SRC + 0x2000)
                    got = u16s(sim.mem.read(DST, 64 * 16))
                    want = R.convert(bytes(yb), ub, vb, 32, 16)
                    r.check(got == want, '%s sweep, other %d, c0 %d, yq %d' % (which, other, c0, yq))
    return r


def t_box(sim, rng, n):
    rows = []
    for rows_out, fn in ((16, 'disp_box2_16'), (8, 'disp_box2_8')):
        r = Row('%s: random and 0/255 data' % fn)
        for k in range(n):
            ss = rng.choice((32, 64, 320, 640))
            ds = rng.choice((16, 160, 320))
            data = rnd(rng, ss * 2 * rows_out + 32, 'extreme' if k % 3 == 2 else 'random')
            sim.mem.write(SRC, data)
            bg = rnd(rng, ds * rows_out + 32)
            sim.mem.write(DST, bg)
            call(sim, fn, DST, ds, SRC, ss)
            got = sim.mem.read(DST, ds * rows_out + 32)
            want, _, _ = R.box2(data, 32, 2 * rows_out, ss)
            ok = all(got[y * ds:y * ds + 16] == want[y * 16:y * 16 + 16] for y in range(rows_out))
            rest = all(got[y * ds + 16:(y + 1) * ds] == bg[y * ds + 16:(y + 1) * ds]
                       for y in range(rows_out))
            r.check(ok and rest, 'sstride %d dstride %d' % (ss, ds))
        rows.append(r)
    return rows


def t_convert(sim, rng, n):
    r = Row('disp_convert: whole pictures, cropped origins')
    for k in range(n):
        w = rng.choice((320, 176, 2 * rng.randrange(8, 160)))
        h = rng.choice((240, 120, 2 * rng.randrange(8, 120)))
        ys = (w + 15) & ~15 if rng.random() < 0.5 else (w + 63) & ~63
        off = rng.choice((0, 0, 2, 18, 34))     # a crop: even
        ya, ua, va, yb, ub, vb = put_planes(sim, rng, w, h, ys + 64, off)
        ys += 64
        dp = 640
        tw, th = (w + 31) & ~31, (h + 15) & ~15
        bg = rnd(rng, dp * 256)
        sim.mem.write(DST, bg)
        call(sim, 'disp_convert', DST, dp, ya, ua, va, ys, w, h)
        got = sim.mem.read(DST, dp * 256)
        want = R.convert(yb, ub, vb, w, h)
        ok = all(u16s(got[y * dp:y * dp + 2 * w]) == want[y * w:(y + 1) * w] for y in range(h))
        rest = all(got[y * dp + 2 * tw:(y + 1) * dp] == bg[y * dp + 2 * tw:(y + 1) * dp]
                   for y in range(th)) and got[th * dp:] == bg[th * dp:]
        r.check(ok and rest, '%dx%d ystride %d offset %d: %s' % (
            w, h, ys, off, 'pixels' if not ok else 'wrote past the tiles'))
    return r


def t_scale(sim, rng, n):
    rows = []
    for vec in (True, False):
        r = Row('disp_scale2 (%s path): 2:1 box + conversion' % ('vector' if vec else 'scalar'))
        for k in range(n):
            ow = rng.choice((320, 2 * rng.randrange(40, 160)))
            oh = rng.choice((240, 2 * rng.randrange(30, 120)))
            w, h = 2 * ow, 2 * oh
            ys = (w + 63) & ~63
            off = 0 if vec else rng.choice((2, 18, 66))
            ya, ua, va, yb, ub, vb = put_planes(sim, rng, w, h, ys, off)
            if vec and (ua & 31 or va & 31):
                # keep the chroma planes 32-byte aligned for the vector path
                pass
            dp = 640
            sim.mem.write(DST, bytes(dp * 240))
            call(sim, 'disp_scale2', DST, dp, ya, ua, va, ys, ow, oh, STRIP)
            got = sim.mem.read(DST, dp * 240)
            y2, _, _ = R.box2(yb, w, h)
            u2, _, _ = R.box2(ub, w // 2, h // 2)
            v2, _, _ = R.box2(vb, w // 2, h // 2)
            want = R.convert(y2, u2, v2, ow, oh)
            ok = all(u16s(got[y * dp:y * dp + 2 * ow]) == want[y * ow:(y + 1) * ow]
                     for y in range(oh))
            r.check(ok, '%dx%d -> %dx%d offset %d' % (w, h, ow, oh, off))
        rows.append(r)
    return rows


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-v', action='store_true')
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--n', type=int, default=12)
    ap.add_argument('--no-sweep', action='store_true')
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
        print('disp_test: vc4-elf-gcc not found (%s): skipped' % C.GCC)
        return 0
    t0 = time.time()
    R.selftest()
    prog = build()
    sim = S.Sim()
    C.load_into_sim(prog, sim)
    rng = random.Random(a.seed)
    rows = [t_tile(sim, rng, 4 * a.n)]
    rows += t_box(sim, rng, 2 * a.n)
    rows.append(t_convert(sim, rng, a.n))
    rows += t_scale(sim, rng, max(2, a.n // 3))
    if not a.no_sweep:
        rows.append(t_sweep(sim, rng))
    bad = 0
    for r in rows:
        print('  %-72s %4d cases  %s' % (r.name, r.n, 'ok' if not r.bad else
                                          'FAIL %d (first: %s)' % (r.bad, r.first)))
        bad += r.bad
    print('%d rows, %d cases, %d failed (%.0f s)' % (
        len(rows), sum(r.n for r in rows), bad, time.time() - t0))
    print('DISP KERNELS %s' % ('PASSED' if not bad else 'FAILED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
