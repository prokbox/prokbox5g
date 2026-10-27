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

"""The visualizer's engine (#126; bcmfw/jobs/jviz.h, docs/research/bcm2722/
visualizer.md), in vc2sim and on the Mac:

1. The vector FFT (jv_fft256, jv_fft512; bcmfw/video/gen_viz.py) against
   the C reference (jviz.c jviz_raw_ref), both built by vc2cc into one
   program: every power of every bin bit for bit, on sines (on and between
   bins, full scale and quiet), chirps, noise, silence, full-scale square
   waves (clipping), impulses, DC, Nyquist and random words, at 256 and
   512 samples.
2. The C reference built for the Mac (ctypes) equals the vc2sim build's
   output, and its X[k] against a double-precision FFT of the same
   windowed input (numpy): the SNR in dB per signal class, the worst bin
   error, and how far below full scale the noise floor sits.
3. The bands (jviz_bands, jviz_l2) against a Python model.
4. The bars (jviz_bars_draw) against a Python model of its definition,
   and through the reference executor's J_OP_BARS (jref.c, JREF_VIZ);
   the vector bars (viz_vpu.c viz_bars, gen_viz.py jv_bars16) against
   jviz_bars_draw in vc2sim and the model, every pixel of the surface: the
   v1 frame, 120 random frames over every flag combination (odd and even
   widths and gaps, rects at any x, 16 to 256 rows, heights 0, 1, full and
   past the rect, peaks over their bars and above the rect), and frames it
   declines (the C draws them); its start-up check (viz_bars_init).
5. Costs: the kernel's instructions and estimated cycles per FFT, the C
   reference's instructions, the bars' per frame (S7 timing: a vector op
   of REP n about n + 0.5 cycles).
6. The accumulator (quirks #31, #32): under every rule that fits silicon
   (vc2sim v_acc_sat sat32, read16, hi16; v_a16_reg_x dbl16, near32) and
   the old wrapping model, the kernel equals the C on a third of the
   signals and on the worst case (every sample 32767, every table word
   -32768: step 2's sum is 2^31), and its start-up check passes; the
   kernel before (gen_viz.py --mutate acc1, every sum in one chain) and
   tw1 (step 2's alone) differ on the worst case and fail the check under
   each saturating rule, and pass only under the wrapping model.
7. --mutations: gen_viz.py --mutate sign (a twiddle's sign), scale (a
   stage's halving dropped), lane (the partner block's column 3 from the
   wrong column): each must make bins differ; mask (the bars' select 1,
   not -1, for a kept pixel), grad (the gradient rows of the group below),
   cap (a pixel in either cap bound capped): each must make bars frames
   differ and fail the bars' start-up check.  The kernel is regenerated
   after.

Usage: bcmfw/tests/viz_test.py [--mutations] [-v]
"""
import ctypes
import math
import os
import random
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
ROOT = os.path.dirname(FW)
JOBS = os.path.join(FW, 'jobs')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
import vc2cc as C       # noqa: E402
import vc2sim as S      # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mutant           # noqa: E402

OUT = os.path.join(ROOT, 'build', 'viz')
GEN = os.path.join(FW, 'video', 'gen_viz.py')
CFLAGS = ['-std=gnu99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
          '-fno-tree-loop-distribute-patterns', '-I' + FW, '-I' + JOBS]
AT = 0x800000           # pcm, then raw outputs
SIZES = (256, 512)
VERBOSE = False
COST = {}


def split(x):
    """natural order to the input's split order (evens, then odds)"""
    return list(x[0::2]) + list(x[1::2])


def clip16(v):
    return max(-32768, min(32767, int(round(v))))


def signals(n, rng):
    """(class, name, samples in natural order)"""
    out = []
    t = np.arange(n)
    for amp, an in ((32767, 'full'), (8000, '-12dB'), (300, '-41dB')):
        for f in (1, 5, 17.0, 17.5, 40.25, n / 4, n / 2 - 3, 3.3):
            out.append(('sine', 'sine %s bin %.2f' % (an, f),
                        [clip16(amp * math.sin(2 * math.pi * f * i / n + 0.3)) for i in t]))
    for k in range(4):
        f0, f1 = rng.uniform(0.5, 20), rng.uniform(n / 8, n / 2 - 1)
        out.append(('chirp', 'chirp %.1f-%.1f' % (f0, f1),
                    [clip16(30000 * math.sin(2 * math.pi * (f0 * i + (f1 - f0) * i * i / (2 * n)) / n))
                     for i in t]))
    for k in range(6):
        a = rng.choice((32767, 12000, 2000))
        out.append(('noise', 'noise %d' % a, [clip16(rng.gauss(0, a / 3)) for _ in t]))
        out.append(('noise', 'uniform %d' % k, [rng.randint(-32768, 32767) for _ in t]))
    out.append(('silence', 'silence', [0] * n))
    out.append(('silence', 'dc -1', [-1] * n))
    for f in (3, 21, n / 8):
        out.append(('clip', 'square full scale bin %d' % f,
                    [32767 if math.sin(2 * math.pi * f * i / n) >= 0 else -32768 for i in t]))
    out.append(('clip', 'clipped sine (x4)',
                [clip16(4 * 32767 * math.sin(2 * math.pi * 7.7 * i / n)) for i in t]))
    out.append(('clip', 'dc +full', [32767] * n))
    out.append(('clip', 'dc -full', [-32768] * n))
    out.append(('clip', 'nyquist', [32767 if i & 1 else -32768 for i in t]))
    for p in (0, 1, n // 2, n // 2 + 1, n - 1, n // 3):
        x = [0] * n
        x[p] = rng.choice((32767, -32768))
        out.append(('impulse', 'impulse at %d' % p, x))
    return out


class Sim:
    def __init__(self):
        lib = ('string.s', C.compile_c(os.path.join(FW, 'lib', 'string.c'), '-O2',
                                       ['-I' + FW, '-std=gnu99',
                                        '-fno-tree-loop-distribute-patterns']))
        self.prog = C.build([os.path.join(JOBS, 'jviz.c'),
                             os.path.join(FW, 'video', 'viz_vpu.c'),
                             os.path.join(FW, 'video', 'viz_kernels.S'),
                             os.path.join(FW, 'video', 'jobs_kernels.S'), lib], '-O2',
                            extra=CFLAGS, libs=C.DEFAULT_LIBS, no_float=True,
                            conservative=True)
        self.s = S.Sim()
        C.load_into_sim(self.prog, self.s)
        self.tabs = {n: self.s.call('jviz_tabs', n) for n in SIZES}

    def put(self, at, vals):
        self.s.mem.write(at, np.array(vals, dtype='<i2').tobytes())

    def raw(self, at):
        return np.frombuffer(bytes(self.s.mem.read(at, 1024)), dtype='<u2').copy()

    def run(self, x, n, kernel=True):
        """(raw, steps): the kernel's or the C's raw output for x"""
        self.put(AT, split(x))
        out = AT + 0x1000
        self.s.mem.write(out, bytes(1024))
        s0 = self.s.steps
        if kernel:
            self.s.call('jv_fft%d' % n, out, AT, self.tabs[n], check_abi=True,
                        max_steps=10 ** 7)
        else:
            self.s.call('jviz_raw_ref', out, AT, n, check_abi=True, max_steps=10 ** 8,
                        regs={15: 0, 22: 0})
        return self.raw(out), self.s.steps - s0


TAB = AT + 0x8000       # a crafted table, then its struct jviz_tabs
WORST_PCM, WORST_TAB = 32767, -32768


def run_worst(sim, n, kernel=True):
    """the worst case for the accumulator (viz_vpu.c viz_vpu_init's): every
    sample 32767, every table word -32768"""
    sim.put(TAB, [WORST_TAB] * 512)
    st = TAB + 0x400
    sim.s.mem.write(st, np.array([TAB, TAB, TAB, n // 32], dtype='<u4').tobytes())
    sim.put(AT, [WORST_PCM] * n)
    out = AT + 0x1000
    sim.s.mem.write(out, bytes(1024))
    if kernel:
        sim.s.call('jv_fft%d' % n, out, AT, st, check_abi=True, max_steps=10 ** 7)
    else:
        sim.s.call('jviz_raw_ref_t', out, AT, n, st, check_abi=True, max_steps=10 ** 8,
                   regs={15: 0, 22: 0})
    return sim.raw(out)


def variants(sigs):
    """The kernel under every accumulator and A-operand rule that fits
    silicon (vc2vpu.SILICON_FITS) and the old wrapping model: exact on the
    signals and the worst case, the start-up check passing.  The kernel
    before (acc1) and tw1 differ on the worst case and fail the check
    under every saturating rule (the wrapping model, which silicon
    refuted, is the only one they pass)"""
    import vc2vpu as VP
    rules = VP.SILICON_FITS['v_acc_sat'] + ('none',)
    a16s = VP.SILICON_FITS['v_a16_reg_x']
    saved = S.HYP['v_acc_sat'], S.HYP['v_a16_reg_x']
    bad = 0
    try:
        for kernel in (None, 'acc1', 'tw1'):
            regen(kernel)
            ms = Sim()
            refs = {n: run_worst(ms, n, kernel=False) for n in SIZES}
            crefs = {n: [ms.run(x, n, kernel=False)[0] for _, _, x in sigs[n][::3]]
                     for n in SIZES}
            for a16 in (a16s if kernel is None else a16s[:1]):
                for rule in rules:
                    S.HYP['v_acc_sat'], S.HYP['v_a16_reg_x'] = rule, a16
                    chk = ms.s.call('viz_vpu_init', 0, check_abi=True, max_steps=10 ** 8,
                                    regs={15: 0, 22: 0})
                    worst = sum(int(np.sum(valid(run_worst(ms, n), n) != valid(refs[n], n)))
                                for n in SIZES)
                    nd = sum(not np.array_equal(valid(ms.run(x, n)[0], n), valid(c, n))
                             for n in SIZES for (_, _, x), c in zip(sigs[n][::3], crefs[n]))
                    if kernel is None or rule == 'none':
                        ok = chk == 1 and worst == 0 and nd == 0
                    else:
                        ok = chk == 0 and worst > 0
                    bad += not ok
                    print('accumulator %-6s A +r x %-6s %-10s: start-up check %s, worst case '
                          '%d halfwords differ, %d signals differ: %s' % (
                              rule, a16, kernel or 'two chains',
                              'passes' if chk == 1 else 'fails', worst, nd,
                              'ok' if ok else 'WRONG'))
    finally:
        S.HYP['v_acc_sat'], S.HYP['v_a16_reg_x'] = saved
        regen()
    return bad


def valid(raw, n):
    """the rows that hold bins (the kernel leaves the others as junk)"""
    rows = n // 32
    return np.concatenate([raw[:16 * rows], raw[256:256 + 16 * rows]])


# the mutants go to this process's builds only, never over the tracked
# viz_kernels.S (mutant.py); regen() with no mutant: the tracked kernel
regen = mutant.switcher(GEN)


# ---- the Mac build (ctypes) --------------------------------------------------

def mac_lib():
    os.makedirs(OUT, exist_ok=True)
    so = os.path.join(OUT, 'jviz.so')
    subprocess.run(['cc', '-O2', '-shared', '-fPIC', '-std=gnu99', '-Wall', '-Wextra',
                    '-Werror', '-I' + FW, '-I' + JOBS, '-o', so,
                    os.path.join(JOBS, 'jviz.c')], check=True)
    return ctypes.CDLL(so)


def mac_fft(lib, x, n):
    pcm = (ctypes.c_int16 * n)(*split(x))
    xr = (ctypes.c_int16 * (n // 2))()
    xi = (ctypes.c_int16 * (n // 2))()
    lib.jviz_fft_ref(xr, xi, pcm, n)
    return np.array(xr[:], dtype=float), np.array(xi[:], dtype=float)


def mac_raw(lib, x, n):
    pcm = (ctypes.c_int16 * n)(*split(x))
    raw = (ctypes.c_uint16 * 512)()
    lib.jviz_raw_ref(raw, pcm, n)
    return np.array(raw[:], dtype=np.uint16)


def hann(n):
    return [round(23000 * 0.5 * (1 - math.cos(2 * math.pi * i / n))) for i in range(n)]


def double_ref(x, n):
    """X[k] / m, k < m, of the same windowed input in double precision"""
    w = np.array(hann(n), dtype=float)
    xd = np.array(x, dtype=float) * w / 65536.0
    return np.fft.fft(xd)[:n // 2] / (n // 2)


def snr_row(lib, sigs, n):
    """per class: SNR (dB) of X against the double FFT, the worst bin's
    error in units, the error floor below full scale"""
    by = {}
    for cls, name, x in sigs:
        xr, xi = mac_fft(lib, x, n)
        ref = double_ref(x, n)
        err = (xr - ref.real) ** 2 + (xi - ref.imag) ** 2
        sig = np.abs(ref) ** 2
        by.setdefault(cls, []).append((sig.sum(), err.sum(), np.sqrt(err.max()), name))
    rows = []
    for cls, v in by.items():
        s, e = sum(a for a, _, _, _ in v), sum(b for _, b, _, _ in v)
        worst = max(c for _, _, c, _ in v)
        per = [10 * math.log10(a / b) if b and a else float('inf') for a, b, _, _ in v]
        finite = [p for p in per if p != float('inf')]
        rows.append((cls, 10 * math.log10(s / e) if e and s else float('inf'),
                     min(finite) if finite else float('inf'), worst, e / (len(v) * n / 2)))
    return rows


# ---- profiling (cycle estimates) -----------------------------------------------

def vec_cycles(text):
    """isa-vector.md S7: a vector op of REP n about n + 0.5 cycles, REP 1
    about 1.5 (jobs_image_test.py's estimate); scalar ops 1 each"""
    import re
    m = re.search(r'\brep(\d+)', text)
    return int(m.group(1)) + 0.5 if m else 1.5


def prof_call(s, func, *args, regs=None):
    """Single-step one call: (instructions, vector instructions, cycles)"""
    r = s.r
    for k in range(6):
        r[k] = (args[k] if k < len(args) else 0) & 0xffffffff
    for k, v in (regs or {}).items():
        r[k] = v
    r[25] = S.STACK_TOP
    r[26] = S.RET_MAGIC
    pc = s.sym(func)
    n = nv = 0
    cyc = 0.0
    while pc != S.RET_MAGIC:
        h = s.code.get(pc) or s.fetch(pc)
        ins = h[2]
        n += 1
        if ins.e.name in ('vec48', 'vec80'):
            nv += 1
            cyc += vec_cycles(ins.text)
        else:
            cyc += 1
        r[31] = pc
        npc = h[0]()
        pc = (pc + h[1]) if npc is None else npc & S.MASK
    return n, nv, cyc


# ---- the bars ----------------------------------------------------------------------

BG, CAP, ROUND, FLIP = 1, 2, 4, 8


class Bars(ctypes.Structure):
    _fields_ = [('n', ctypes.c_uint32), ('barw', ctypes.c_uint32), ('gap', ctypes.c_uint32),
                ('cap_h', ctypes.c_uint32), ('flags', ctypes.c_uint32),
                ('bot', ctypes.c_uint16), ('top', ctypes.c_uint16), ('cap', ctypes.c_uint16),
                ('bg', ctypes.c_uint16), ('height', ctypes.c_void_p), ('peak', ctypes.c_void_p)]


def bars_model(px, pitch, w, h, b):
    """jviz.h's definition of the bars, on px (a list, the rect at 0);
    FLIP: the same frame with row y at h - 1 - y"""
    n, bw, gap = b['n'], b['barw'], b['gap']
    total = n * bw + (n - 1) * gap
    if not n or not bw or total > w or not h:
        return -1
    x0 = (w - total) // 2

    def put(y, x0_, x1, c):
        if b['flags'] & FLIP:
            y = h - 1 - y
        for x in range(x0_, x1):
            px[y * pitch + x] = c
    if b['flags'] & BG:
        for y in range(h):
            put(y, 0, w, b['bg'])
    inv = (256 << 16) // h

    def lerp(a, c, f):
        return a + (((c - a) * f) >> 8)
    for y in range(h):
        t = h - 1 - y
        f = (t * inv) >> 16
        c0, c1 = b['bot'], b['top']
        c = lerp(c0 >> 11, c1 >> 11, f) << 11 | lerp(c0 >> 5 & 63, c1 >> 5 & 63, f) << 5 | \
            lerp(c0 & 31, c1 & 31, f)
        for i in range(n):
            hh = min(b['height'][i], h)
            if t >= hh:
                continue
            x = x0 + i * (bw + gap)
            if t == hh - 1 and b['flags'] & ROUND and hh >= 2 and bw >= 3:
                put(y, x + 1, x + bw - 1, c)
            else:
                put(y, x, x + bw, c)
    if b['flags'] & CAP:
        for i in range(n):
            if not b['peak'][i]:
                continue
            top = h - b['peak'][i] if b['peak'][i] < h else 0
            for y in range(top, top + min(b['cap_h'], h - top)):
                put(y, x0 + i * (bw + gap), x0 + i * (bw + gap) + bw, b['cap'])
    return 0


def mac_bars(lib, px, pitch, w, h, b):
    buf = (ctypes.c_uint16 * len(px))(*px)
    ht = (ctypes.c_uint8 * 32)(*b['height'])
    pk = (ctypes.c_uint8 * 32)(*b['peak'])
    st = Bars(b['n'], b['barw'], b['gap'], b['cap_h'], b['flags'], b['bot'], b['top'],
              b['cap'], b['bg'], ctypes.cast(ht, ctypes.c_void_p).value,
              ctypes.cast(pk, ctypes.c_void_p).value)
    r = lib.jviz_bars_draw(buf, pitch, w, h, ctypes.byref(st), None)
    return r, list(buf)


def bars_cases(rng):
    """(w, h, bars): the v1 style's typical frame, then random ones"""
    out = []
    v1 = dict(n=24, barw=10, gap=3, cap_h=2, flags=BG | CAP | ROUND, bot=0x4A7F, top=0xDF1F,
              cap=0xFFFF, bg=0x0000, height=[rng.randint(0, 150) for _ in range(24)])
    v1['peak'] = [min(150, x + rng.randint(0, 20)) for x in v1['height']]
    out.append((320, 150, v1))
    for _ in range(60):
        n = rng.randint(1, 32)
        bw = rng.randint(1, 12)
        gap = rng.randint(0, 4)
        w = n * bw + (n - 1) * gap + rng.choice((0, 0, 1, 7, -1))
        h = rng.randint(1, 120)
        b = dict(n=n, barw=bw, gap=gap, cap_h=rng.randint(0, 5), flags=rng.randint(0, 7),
                 bot=rng.getrandbits(16), top=rng.getrandbits(16), cap=rng.getrandbits(16),
                 bg=rng.getrandbits(16),
                 height=[rng.choice((0, 1, 2, h, h + 9, rng.randint(0, 255))) for _ in range(n)],
                 peak=[rng.choice((0, 1, h, rng.randint(0, 255))) for _ in range(n)])
        b['height'] += [0] * (32 - n)
        b['peak'] += [0] * (32 - n)
        out.append((max(w, 1), h, b))
    v1['height'] += [0] * 8
    v1['peak'] += [0] * 8
    # every frame again upside down (J_BARS_FLIP: the Mirror's reflection)
    return out + [(w, h, dict(b, flags=b['flags'] | FLIP)) for w, h, b in out]


def bars_check(lib, rng):
    bad = 0
    for w, h, b in bars_cases(rng):
        pitch = w + 5
        px = [rng.getrandbits(16) for _ in range(pitch * h)]
        want = list(px)
        rm = bars_model(want, pitch, w, h, b)
        rc, got = mac_bars(lib, px, pitch, w, h, b)
        if rm != rc or got != want:
            bad += 1
            if VERBOSE:
                print('    bars %dx%d n %d: model %d, C %d' % (w, h, b['n'], rm, rc))
    return bad


# ---- the bars on the vector unit (viz_vpu.c viz_bars, jv_bars16) ------------------

BARS_ST = AT + 0x4000           # struct jviz_bars, then heights and peaks
BARS_FB = AT + 0x10000          # the surface (32-byte aligned)
ROWS_MAX, CB_MAX = 256, 21      # viz_vpu.h VIZ_BARS_ROWS_MAX, VIZ_BARS_CB_MAX


def put_bars(s, b, flags=None):
    hb = BARS_ST + 64
    s.mem.write(BARS_ST, np.array([b['n'], b['barw'], b['gap'], b['cap_h'],
                                   b['flags'] if flags is None else flags],
                                  dtype='<u4').tobytes() +
                np.array([b['bot'], b['top'], b['cap'], b['bg']], dtype='<u2').tobytes() +
                np.array([hb, hb + 32], dtype='<u4').tobytes())
    s.mem.write(hb, bytes(b['height'][:32]) + bytes(b['peak'][:32]))


def sim_bars(sim, pitch, x, w, h, b, px, vector):
    """jref_bars' choice on the BCM: viz_bars (vector), the C if it
    declines or not vector.  -> (viz_bars took it, the C's status, the
    surface's pixels)"""
    s = sim.s
    put_bars(s, b)
    s.mem.write(BARS_FB, np.array(px, dtype='<u2').tobytes())
    at = BARS_FB + 2 * x
    took = vector and s.call('viz_bars', at, pitch, x, w, h, BARS_ST, check_abi=True,
                             max_steps=10 ** 8, regs={15: 0, 22: 0})
    rc = 0
    if not took:
        rc = s.call('jviz_bars_draw', at, pitch, w, h, BARS_ST, 0, check_abi=True,
                    max_steps=10 ** 8, regs={15: 0, 22: 0})
        rc = rc - (1 << 32) if rc >> 31 else rc
    got = np.frombuffer(bytes(s.mem.read(BARS_FB, 2 * len(px))), dtype='<u2').tolist()
    return bool(took), rc, got


def full_frame():
    """the full-screen layout (owner, 2026-10-25): 320 x 240 at (0, 0), no
    title bar or counter: the v1 bars with heights and peaks over 240 rows"""
    v1 = bars_cases(random.Random(1))[0][2]
    return dict(v1, height=[x * 240 // 150 for x in v1['height']],
                peak=[min(240, x * 240 // 150 + 2) for x in v1['peak']])


def fade565(c, bg):
    """apps/ipodui/viz_core.c viz_fade565: c 5/8 of the way to bg"""
    ch = [(11, 31), (5, 63), (0, 31)]
    return sum((((c >> s & m) * 3 + (bg >> s & m) * 5) >> 3) << s for s, m in ch)


def mirror_frames(H=240):
    """the shell's Mirror over the full screen (apps/ipodui/viz.c, viz_core.c
    viz_mirror_split): the bars in the top bh rows (heights scaled by
    bh / H), a gap row, then the reflection, rh = 30 % of H rows: each
    bar's height clamped to rh, the colours faded 5/8 toward bg, no cap,
    no rounding, upside down (J_BARS_FLIP).  -> ((y, h, bars) for the
    bars, then the reflection)"""
    fr = full_frame()
    rh = H * 3 // 10
    bh = H - rh - 1
    hs = [x * bh // H for x in fr['height']]
    bars = dict(fr, height=hs, peak=[x * bh // H for x in fr['peak']])
    refl = dict(fr, height=[min(x, rh) for x in hs], peak=[0] * 32,
                flags=BG | FLIP, bot=fade565(fr['bot'], fr['bg']),
                top=fade565(fr['top'], fr['bg']), cap=fade565(fr['cap'], fr['bg']))
    return (0, bh, bars), (bh + 1, rh, refl)


def bars_vec_cases(rng):
    """(pitch, x, w, h, bars, the vector path takes it): the v1 frame
    (aligned, then at x 7), then random frames over every flag
    combination: odd and even bar widths and gaps, rects at any x (partial
    blocks at both edges), heights from 16 (one group) to 256 (sixteen),
    groups that overlap, bar heights 0, 1, 2, full and past the rect, peaks
    0, 1, at and above the rect and over the bar; then frames the vector
    path declines (the C draws them)"""
    v1 = bars_cases(random.Random(1))[0][2]
    out = [(320, 0, 320, 150, v1, True), (336, 7, 300, 150, dict(v1, n=22), True),
           (320, 0, 320, 240, full_frame(), True)]
    # flipped (J_BARS_FLIP): the full screen with every flag, the Mirror's
    # two frames at full size, and the v1 frame at x 7 (partial blocks)
    fr = full_frame()
    out += [(320, 0, 320, 240, dict(fr, flags=fr['flags'] | FLIP), True),
            (320, 0, 320, 240, dict(fr, flags=FLIP | CAP), True)]
    out += [(320, 0, 320, h, b, True) for _, h, b in mirror_frames()]
    out.append((336, 7, 300, 150, dict(v1, n=22, flags=v1['flags'] | FLIP), True))
    for k in range(120):
        n = rng.randint(1, 32)
        bw = rng.choice((1, 2, 3, 4, 5, rng.randint(1, 12)))
        gap = rng.randint(0, 5)
        total = n * bw + (n - 1) * gap
        if total > 320:
            n = max(1, 320 // (bw + gap))
            total = n * bw + (n - 1) * gap
        w = min(320, total + rng.choice((0, 0, 1, 2, 7, rng.randint(0, 40))))
        h = rng.choice((16, 17, 31, 32, 33, 47, 150, 255, 256, rng.randint(16, 256)))
        x = rng.choice((0, 1, 15, 16, 17, rng.randint(0, 31)))
        pitch = (x + w + rng.choice((0, 15, 16, 40)) + 15) & ~15
        b = dict(n=n, barw=bw, gap=gap, cap_h=rng.choice((0, 1, 2, 3, 7)),
                 flags=k & 15, bot=rng.getrandbits(16), top=rng.getrandbits(16),
                 cap=rng.getrandbits(16), bg=rng.getrandbits(16),
                 height=[min(255, rng.choice((0, 1, 2, h, h + 9, 255, rng.randint(0, h))))
                         for _ in range(n)],
                 peak=[min(255, rng.choice((0, 1, h, 255, rng.randint(0, h))))
                       for _ in range(n)])
        for i in range(n):          # most peaks at or above their bar
            if rng.random() < 0.6:
                b['peak'][i] = min(255, b['height'][i] + rng.randint(0, 9))
        b['height'] += [0] * (32 - n)
        b['peak'] += [0] * (32 - n)
        lo = x & 15
        out.append((pitch, x, w, h, b, (lo + w + 15) // 16 <= CB_MAX))
    # declined: h under 16 and over 256, a pitch not of whole blocks,
    # wider than 21 blocks, bars wider than the rect (J_E_RANGE)
    d = dict(v1)
    out += [(320, 3, 200, 15, d, False), (320, 0, 320, 1, d, False),
            (320, 5, 310, 257, d, False), (328, 0, 320, 40, d, False),
            (352, 7, 330, 40, dict(d, n=25), False), (320, 0, 300, 40, d, False),
            (320, 3, 200, 15, dict(d, flags=d['flags'] | FLIP), False)]
    return out


def bars_vec_check(sim, rng, cases=None, quiet=False):
    """viz_bars (and jref's fallback) against jviz_bars_draw in vc2sim and
    the Python model, every pixel of the surface: (cases that differ, cases,
    taken by the vector path, the vector path's choice wrong)"""
    bad = took_n = wrong = 0
    cases = cases if cases is not None else bars_vec_cases(rng)
    for pitch, x, w, h, b, vec in cases:
        px = [rng.getrandbits(16) for _ in range(pitch * h)]
        took, rv, got = sim_bars(sim, pitch, x, w, h, b, px, True)
        _, rc, want = sim_bars(sim, pitch, x, w, h, b, px, False)
        model = list(px)
        sub = [model[y * pitch + x + c] for y in range(h) for c in range(pitch - x)]
        rm = bars_model(sub, pitch - x, w, h, b)
        for y in range(h):
            model[y * pitch + x:(y + 1) * pitch] = sub[y * (pitch - x):(y + 1) * (pitch - x)]
        took_n += took
        wrong += took != vec
        if got != want or rv != rc or want != model or rc != rm:
            bad += 1
            if VERBOSE and not quiet:
                diff = [i for i in range(len(got)) if got[i] != want[i]]
                print('    bars %dx%d at x %d (pitch %d) n %d barw %d gap %d flags %d: '
                      '%d pixels differ%s' % (w, h, x, pitch, b['n'], b['barw'], b['gap'],
                                              b['flags'], len(diff),
                                              ', first at row %d col %d' % (
                                                  diff[0] // pitch, diff[0] % pitch)
                                              if diff else ''))
    return bad, len(cases), took_n, wrong


# ---- bands -----------------------------------------------------------------------

def l2_model(p):
    if p < 2:
        return 0
    e = p.bit_length() - 1
    f = (p >> (e - 4)) if e >= 4 else (p << (4 - e))
    return 16 * e + min(15, round(16 * math.log2(1 + (f & 15) / 16)))


def bands_case(lib, rng):
    bad = 0
    lib.jviz_l2.restype = ctypes.c_uint32
    for p in [0, 1, 2, 3, 15, 16, 17, 31, 32, 255, 65535, 65536, 2 ** 31, 2 ** 32 - 1] + \
            [rng.getrandbits(rng.randint(1, 32)) for _ in range(2000)]:
        if lib.jviz_l2(ctypes.c_uint32(p)) != l2_model(p):
            bad += 1
    for _ in range(200):
        m = rng.choice((128, 256))
        nb = rng.randint(1, 32)
        e = sorted(rng.randint(0, m + 4) for _ in range(nb + 1))
        p = [rng.getrandbits(rng.randint(0, 32)) for _ in range(m)]
        out = (ctypes.c_uint16 * nb)()
        lib.jviz_bands(out, (ctypes.c_uint32 * m)(*p), m, (ctypes.c_uint16 * (nb + 1))(*e), nb)
        want = [l2_model(max([p[k] for k in range(e[b], min(e[b + 1], m))] or [0]))
                for b in range(nb)]
        bad += list(out) != want
    return bad


# ---- Scope, Radial and Spectrogram (jviz.h: POLYLINE, SEGMENTS, PALROW) ------------

U8, SEG_CAP, SEG_RING, SEG_ROUND = 1, 1, 2, 4
CHK = {'JVIZ_CHK_POLY': 0, 'JVIZ_CHK_SEGS': 1, 'JVIZ_CHK_PAL': 2}


def tdiv(a, b):
    """C's division: truncated toward zero"""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def mix565(c0, c1, f):
    def lerp(a, c):
        return a + (((c - a) * f) >> 8)
    return lerp(c0 >> 11, c1 >> 11) << 11 | lerp(c0 >> 5 & 63, c1 >> 5 & 63) << 5 | \
        lerp(c0 & 31, c1 & 31)


def poly_model(px, pitch, w, h, p, src):
    """jviz.h's polyline on px (a list, the rect at 0); src: the elements"""
    n, W = p['n'], p['width']
    if n < 2 or n > 1024 or not 1 <= W <= 3 or not w or w > 320 or not h:
        return -1
    e_last = p['off'] + (n - 1) * p['step']
    if (p['half'] + (e_last >> 1) if p['half'] else e_last) >= len(src):
        return -1

    def row(e):
        if p['half']:
            e = p['half'] + (e >> 1) if e & 1 else e >> 1
        y = src[e] if p['flags'] & U8 else (h >> 1) - ((src[e] * p['gain']) >> 16)
        return min(max(y, 0), h - 1)
    lo, hi = [1 << 30] * w, [-1] * w

    def cover(c, a, b):
        lo[c] = min(lo[c], a, b)
        hi[c] = max(hi[c], a, b)
    pts = [((i * (w - 1)) // (n - 1), row(p['off'] + i * p['step'])) for i in range(n)]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if x1 <= x0 + 1:
            cover(x0, y0, y1)
        else:
            for c in range(x0, x1):
                cover(c, y0 + tdiv((y1 - y0) * (c - x0), x1 - x0),
                      y0 + tdiv((y1 - y0) * (c + 1 - x0), x1 - x0))
    cover(w - 1, pts[-1][1], pts[-1][1])
    for c in range(w):
        cs = range(max(c - W // 2, 0), min(c + (W - 1) // 2, w - 1) + 1)
        a = max(min(lo[i] for i in cs) - (W - 1) // 2, 0)
        b = min(max(hi[i] for i in cs) + W // 2, h - 1)
        for y in range(a, b + 1):
            px[y * pitch + c] = p['colour']
    return 0


SIN48Q = [round(16384 * math.sin(2 * math.pi * k / 48)) for k in range(13)]


def sin48(k):
    return round(16384 * math.sin(2 * math.pi * (k % 48) / 48))


def segs_model(px, pitch, w, h, s):
    """jviz.h's radial segments on px (the rect at 0)"""
    if not 1 <= s['n'] <= 48 or s['a0'] > 47 or s['astep'] > 47 or not 1 <= s['width'] <= 3 \
            or not 1 <= s['lmax'] <= 255 or s['capd'] > 15 or s['ringd'] > 15:
        return -1

    def dot(x, y, d, rnd, c):
        x -= (d - 1) // 2
        y -= (d - 1) // 2
        for j in range(d):
            inset = 1 if rnd and d >= 3 and j in (0, d - 1) else 0
            if 0 <= y + j < h:
                for a in range(max(x + inset, 0), min(x + d - 1 - inset, w - 1) + 1):
                    px[(y + j) * pitch + a] = c

    def at(i, r):
        k = s['a0'] + i * s['astep']
        sn, cs = sin48(k), -sin48(k + 12)
        return s['cx'] + ((r * sn + 8192) >> 14), s['cy'] + ((r * cs + 8192) >> 14)
    lmax = s['lmax']
    inv = (256 << 16) // lmax
    rnd = s['flags'] & SEG_ROUND
    if s['flags'] & SEG_RING:
        for i in range(s['n']):
            dot(*at(i, s['rring']), s['ringd'], rnd, s['ring'])
    for i in range(s['n']):
        for t in range(min(s['len'][i], lmax)):
            dot(*at(i, s['r0'] + t), s['width'], 0, mix565(s['c0'], s['c1'], (t * inv) >> 16))
    if s['flags'] & SEG_CAP:
        for i in range(s['n']):
            if s['peak'][i]:
                dot(*at(i, s['r0'] + min(s['peak'][i], lmax)), s['capd'], rnd, s['cap'])
    return 0


def pal_model(px, pitch, w, h, lut, lv):
    nb = len(lv)
    if not nb or nb > 96 or nb > w:
        return -1
    for b in range(nb):
        for y in range(h):
            for x in range(b * w // nb, (b + 1) * w // nb):
                px[y * pitch + x] = lut[lv[b]]
    return 0


POLY_F = ('src', 'len', 'n', 'off', 'step', 'half', 'gain', 'width', 'flags')
SEGS_F = ('n', 'a0', 'astep', 'width', 'flags', 'r0', 'lmax', 'rring', 'capd', 'ringd')


class Poly(ctypes.Structure):
    _fields_ = [('src', ctypes.c_void_p)] + [(f, ctypes.c_uint32) for f in POLY_F[1:]] + \
        [('colour', ctypes.c_uint16)]


class Segs(ctypes.Structure):
    _fields_ = [(f, ctypes.c_uint32) for f in SEGS_F] + \
        [('cx', ctypes.c_int32), ('cy', ctypes.c_int32)] + \
        [(f, ctypes.c_uint16) for f in ('c0', 'c1', 'cap', 'ring')] + \
        [('len', ctypes.c_void_p), ('peak', ctypes.c_void_p)]


class Pal(ctypes.Structure):
    _fields_ = [('lut', ctypes.c_void_p), ('lv', ctypes.c_void_p), ('nb', ctypes.c_uint32)]


def draw_mac(lib, kind, px, pitch, w, h, a):
    """(status, pixels) of jviz_poly_draw / jviz_segs_draw / jviz_pal_draw
    on the Mac; a: the case's dict"""
    buf = (ctypes.c_uint16 * len(px))(*px)
    keep = []
    if kind == 'poly':
        src = (ctypes.c_uint8 * len(a['src']))(*a['src']) if a['flags'] & U8 else \
            (ctypes.c_int16 * len(a['src']))(*a['src'])
        keep.append(src)
        st = Poly(ctypes.cast(src, ctypes.c_void_p).value, len(a['src']),
                  *[a[f] for f in POLY_F[2:]], a['colour'])
        r = lib.jviz_poly_draw(buf, pitch, w, h, ctypes.byref(st))
    elif kind == 'segs':
        ln = (ctypes.c_uint8 * 48)(*a['len'])
        pk = (ctypes.c_uint8 * 48)(*a['peak'])
        st = Segs(*[a[f] for f in SEGS_F], a['cx'], a['cy'], a['c0'], a['c1'], a['cap'],
                  a['ring'], ctypes.cast(ln, ctypes.c_void_p).value,
                  ctypes.cast(pk, ctypes.c_void_p).value)
        r = lib.jviz_segs_draw(buf, pitch, w, h, ctypes.byref(st))
    else:
        lut = (ctypes.c_uint16 * 256)(*a['lut'])
        lv = (ctypes.c_uint8 * 96)(*a['lv'])
        st = Pal(ctypes.cast(lut, ctypes.c_void_p).value,
                 ctypes.cast(lv, ctypes.c_void_p).value, len(a['lv']))
        r = lib.jviz_pal_draw(buf, pitch, w, h, ctypes.byref(st))
    r = r - (1 << 32) if r >= 1 << 31 else r
    return (-1 if r < 0 else r), list(buf)


DRAW_ST = AT + 0x4000           # the struct
DRAW_SRC = AT + 0x5000          # its arrays
DRAW_FB = AT + 0x40000          # the surface


def draw_put(s, kind, a):
    """the case's struct and arrays in vc2sim memory -> the struct's address"""
    u32 = lambda v: np.array(v, dtype='<u4').tobytes()
    if kind == 'poly':
        src = bytes(a['src']) if a['flags'] & U8 else np.array(a['src'], dtype='<i2').tobytes()
        s.mem.write(DRAW_SRC, src)
        s.mem.write(DRAW_ST, u32([DRAW_SRC, len(a['src'])] + [a[f] for f in POLY_F[2:]]) +
                    np.array([a['colour'], 0], dtype='<u2').tobytes())
    elif kind == 'segs':
        s.mem.write(DRAW_SRC, bytes(a['len']) + bytes(a['peak']))
        s.mem.write(DRAW_ST, u32([a[f] for f in SEGS_F]) +
                    np.array([a['cx'], a['cy']], dtype='<i4').tobytes() +
                    np.array([a['c0'], a['c1'], a['cap'], a['ring']], dtype='<u2').tobytes() +
                    u32([DRAW_SRC, DRAW_SRC + 48]))
    else:
        s.mem.write(DRAW_SRC, np.array(a['lut'], dtype='<u2').tobytes() + bytes(a['lv']))
        s.mem.write(DRAW_ST, u32([DRAW_SRC, DRAW_SRC + 512, len(a['lv'])]))
    return DRAW_ST


FN = {'poly': 'jviz_poly_draw', 'segs': 'jviz_segs_draw', 'pal': 'jviz_pal_draw'}


def draw_sim(sim, kind, px, pitch, w, h, a, x=0, prof=False):
    s = sim.s
    st = draw_put(s, kind, a)
    s.mem.write(DRAW_FB, np.array(px, dtype='<u2').tobytes())
    at = DRAW_FB + 2 * x
    if prof:
        return prof_call(s, FN[kind], at, pitch, w, h, st, regs={15: 0, 22: 0})
    r = s.call(FN[kind], at, pitch, w, h, st, check_abi=True, max_steps=10 ** 8,
               regs={15: 0, 22: 0})
    r = -1 if r >> 31 else r
    return r, np.frombuffer(bytes(s.mem.read(DRAW_FB, 2 * len(px))), dtype='<u2').tolist()


def window(rng, n=512, amp=32767):
    """a 512-sample window in split order, as SPECTRUM's slot holds it"""
    f = rng.uniform(1, 40)
    x = [clip16(amp * math.sin(2 * math.pi * f * i / n) + rng.gauss(0, amp / 8))
         for i in range(n)]
    return split(x)


def scope_frame(rng, h=240):
    """the shell's Scope (visualizer-styles: FILL bg, a 3-pixel dim glow, a
    2-pixel bright core, 320 points across 320 x h) from the slot's window:
    the first 320 samples in time (half 256), gain h/2 of full scale"""
    src = window(rng)
    base = dict(src=src, n=320, off=0, step=1, half=256, gain=(h // 2) * 65536 // 32768,
                flags=0)
    return [dict(base, width=3, colour=0x0A6A), dict(base, width=2, colour=0x5FFF)]


def radial_frame(rng, h=240):
    """the shell's Radial: 48 segments round the centre, r0 30, up to 85 long
    (a 240-row frame; 198 rows clip them), width 3, caps and the dot ring"""
    ln = [rng.randint(0, 100) for _ in range(48)]
    return dict(n=48, a0=0, astep=1, width=3, flags=SEG_CAP | SEG_RING | SEG_ROUND, r0=30,
                lmax=85, rring=26, capd=3, ringd=1, cx=160, cy=120, c0=0x4A7F, c1=0xDF1F,
                cap=0xFFFF, ring=0x2104, len=ln, peak=[min(255, v + rng.randint(0, 9))
                                                         for v in ln])


def pal_frame(rng, nb=24):
    lut = [mix565(0x0010, 0xFFE0, i) for i in range(256)]
    return dict(lut=lut, lv=[rng.randint(0, 255) for _ in range(nb)])


def draw_cases(rng):
    """(kind, w, h, x, pitch, case): the full-screen frames (240 rows and the
    volume overlay's 198), then random ones over every option and edge"""
    out = []
    for h in (240, 198):
        for p in scope_frame(rng, h):
            out.append(('poly', 320, h, 0, 320, p))
        out.append(('segs', 320, h, 0, 320, radial_frame(rng, h)))
        out.append(('pal', 320, 4, 0, 320, pal_frame(rng)))
    for _ in range(50):
        w, h = rng.randint(1, 64), rng.randint(1, 48)
        x = rng.randint(0, 5)
        u8 = rng.random() < 0.4
        ln = rng.randint(4, 200)
        src = [rng.randint(0, 255) if u8 else rng.randint(-32768, 32767) for _ in range(ln)]
        n = rng.randint(2, 150)
        half = rng.choice((0, 0, ln // 2))
        out.append(('poly', w, h, x, w + x + rng.randint(0, 3), dict(
            src=src, n=n, off=rng.randint(0, 6), step=rng.randint(0, 3), half=half,
            gain=rng.choice((0, 1, 300, 65535, rng.randint(0, 65535))),
            width=rng.choice((1, 2, 3, 3, 0, 4)), flags=U8 if u8 else 0,
            colour=rng.getrandbits(16))))
    for _ in range(40):
        w, h = rng.randint(1, 64), rng.randint(1, 48)
        x = rng.randint(0, 5)
        out.append(('segs', w, h, x, w + x + rng.randint(0, 3), dict(
            n=rng.randint(0, 49), a0=rng.randint(0, 48), astep=rng.choice((0, 1, 5, 47, 48)),
            width=rng.choice((1, 2, 3, 0)), flags=rng.randint(0, 7), r0=rng.randint(0, 20),
            lmax=rng.choice((0, 1, 9, 40, 255)), rring=rng.randint(0, 30),
            capd=rng.randint(0, 6), ringd=rng.randint(0, 5),
            cx=rng.randint(-10, w + 10), cy=rng.randint(-10, h + 10),
            c0=rng.getrandbits(16), c1=rng.getrandbits(16), cap=rng.getrandbits(16),
            ring=rng.getrandbits(16), len=[rng.randint(0, 60) for _ in range(48)],
            peak=[rng.choice((0, rng.randint(0, 255))) for _ in range(48)])))
    for _ in range(20):
        w, h = rng.randint(1, 120), rng.randint(1, 6)
        x = rng.randint(0, 5)
        lut = [rng.getrandbits(16) for _ in range(256)]
        out.append(('pal', w, h, x, w + x + rng.randint(0, 3),
                    dict(lut=lut, lv=[rng.randint(0, 255) for _ in range(rng.randint(1, 97))])))
    return out


def draw_model(kind, px, pitch, w, h, a):
    if kind == 'poly':
        return poly_model(px, pitch, w, h, a, a['src'])
    if kind == 'segs':
        return segs_model(px, pitch, w, h, a)
    return pal_model(px, pitch, w, h, a['lut'], a['lv'])


def draw_check(lib, sim, rng):
    """every case: the model, the Mac's C and vc2sim's C, every pixel of the
    surface (outside the rect untouched) and the status"""
    bad = n = 0
    for kind, w, h, x, pitch, a in draw_cases(rng):
        px = [rng.getrandbits(16) for _ in range(pitch * h)]
        sub = px[x:]
        rm = draw_model(kind, sub, pitch, w, h, a)
        want = px[:x] + sub
        rc, got = draw_mac(lib, kind, px[x:], pitch, w, h, a)
        got = px[:x] + got
        rs, gs = draw_sim(sim, kind, px, pitch, w, h, a, x)
        n += 1
        if not (rm == rc == rs and got == want and gs == want):
            bad += 1
            if VERBOSE or bad <= 3:
                print('    %s %dx%d at %d: model %s, Mac %s, vc2sim %s; Mac pixels %s, '
                      'vc2sim pixels %s' % (kind, w, h, x, rm, rc, rs,
                                            'same' if got == want else 'DIFFER',
                                            'same' if gs == want else 'DIFFER'))
    return bad, n


def pack8(vals):
    """bytes four to a word, the low first (jobs.h)"""
    vals = list(vals) + [0] * (-len(vals) % 4)
    return [sum(vals[k + i] << (8 * i) for i in range(4)) for k in range(0, len(vals), 4)]


def wire(kind, dst, x, y, w, h, a, slot=0):
    """jobs.h's payload of POLYLINE / SEGMENTS / PALROW for the case a (src
    or lut: the slot)"""
    rect = [dst, x | y << 16, w | h << 16]
    if kind == 'poly':
        return rect + [slot, a['off'] | a['step'] << 16, a['n'] | a['flags'] << 16,
                       a['gain'] | a['half'] << 16, a['colour'] | a['width'] << 16]
    if kind == 'segs':
        n = a['n']
        return rect + [(a['cx'] & 0xFFFF) | (a['cy'] & 0xFFFF) << 16,
                       n | a['a0'] << 8 | a['astep'] << 16 | a['width'] << 24 | a['flags'] << 28,
                       a['r0'] | a['lmax'] << 8 | a['rring'] << 16 | a['capd'] << 24 |
                       a['ringd'] << 28, a['c0'] | a['c1'] << 16, a['cap'] | a['ring'] << 16] + \
            pack8(a['len'][:n]) + pack8(a['peak'][:n])
    return rect + [slot, len(a['lv'])] + pack8(a['lv'])


def draw_main(sim, lib, rng):
    """POLYLINE, SEGMENTS, PALROW: frames, the start-up check, the costs"""
    fails = 0
    bad, n = draw_check(lib, sim, rng)
    print('line ops (jviz_poly_draw, jviz_segs_draw, jviz_pal_draw): %d of %d frames differ '
          '(model, Mac, vc2sim; full screen at 240 and 198 rows)' % (bad, n))
    fails += bad
    # the sin table against the definition
    t = [sim.s.call('jviz_sin48', k, check_abi=True, regs={15: 0, 22: 0}) for k in range(96)]
    t = [v - (1 << 32) if v >> 31 else v for v in t]
    sb = sum(t[k] != sin48(k) for k in range(96))
    print('jviz_sin48 against round(16384 sin(2 pi k / 48)): %d of 96 differ' % sb)
    fails += sb
    # the start-up check: the Mac's sums are jviz.h's, vc2sim agrees, and
    # viz_draw_init takes every op (none when told to fail)
    hdr = open(os.path.join(JOBS, 'jviz.h')).read()
    import re
    s = (ctypes.c_uint16 * 1200)()
    for name, op in CHK.items():
        want = int(re.search(r'#define\s+%s\s+(0x[0-9a-fA-F]+)u' % name, hdr).group(1), 16)
        mac = lib.jviz_draw_check(s, op) & 0xffffffff
        vc = sim.s.call('jviz_draw_check', DRAW_FB, op, check_abi=True, max_steps=10 ** 8,
                        regs={15: 0, 22: 0})
        if not mac == vc == want:
            print('  FAIL %s: jviz.h %08x, Mac %08x, vc2sim %08x' % (name, want, mac, vc))
            fails += 1
    ok = [sim.s.call('viz_draw_init', f, check_abi=True, max_steps=10 ** 8,
                     regs={15: 0, 22: 0}) for f in (0, 1)]
    print('line ops\' start-up check: %d (want 7), forced to fail %d (want 0)' % tuple(ok))
    fails += ok != [7, 0]
    ci = prof_call(sim.s, 'viz_draw_init', 0, regs={15: 0, 22: 0})
    print('cost the line ops\' start-up check: %d instructions (%.2f ms at 27 MHz)' % (
        ci[0], ci[2] / 27000))
    # the costs per frame (the C on the BCM: scalar, 1 cycle an instruction)
    r = random.Random(7)
    for h in (240, 198):
        sc = [draw_sim(sim, 'poly', [0] * 320 * h, 320, 320, h, p, prof=True)
              for p in scope_frame(r, h)]
        COST['scope %d' % h] = sc
        rd = draw_sim(sim, 'segs', [0] * 320 * h, 320, 320, h, radial_frame(r, h), prof=True)
        COST['radial %d' % h] = rd
        print('cost Scope 320 x %d (glow 3 px + core 2 px, 320 points): %d + %d instructions, '
              '%.2f ms at 27 MHz; Radial (48 segments, caps, ring): %d instructions, %.2f ms '
              'at 27 MHz' % (h, sc[0][0], sc[1][0], (sc[0][2] + sc[1][2]) / 27000, rd[0],
                             rd[2] / 27000))
    pr = draw_sim(sim, 'pal', [0] * 320 * 4, 320, 320, 4, pal_frame(r), prof=True)
    COST['palrow'] = pr
    print('cost PALROW (24 cells, 320 x 4): %d instructions, %.3f ms at 27 MHz' % (
        pr[0], pr[2] / 27000))
    return fails



def main(argv):
    global VERBOSE
    VERBOSE = '-v' in argv
    if not os.path.exists(C.GCC):
        print('skip: vc4-elf-gcc not found')
        return 0
    t0 = time.time()
    rng = random.Random(126)
    fails = 0
    sim = Sim()
    lib = mac_lib()
    if '--draw' in argv:        # the line ops alone
        fails = draw_main(sim, lib, random.Random(36))
        print('viz_test --draw: %d failed (%.0f s)' % (fails, time.time() - t0))
        return 1 if fails else 0
    sigs ={n: signals(n, rng) for n in SIZES}
    costs = {}
    for n in SIZES:
        nd = mac_bad = 0
        ks = cs = 0
        for cls, name, x in sigs[n]:
            kr, ksteps = sim.run(x, n)
            cr, csteps = sim.run(x, n, kernel=False)
            ks, cs = max(ks, ksteps), max(cs, csteps)
            if not np.array_equal(valid(kr, n), valid(cr, n)):
                nd += 1
                if VERBOSE or nd <= 2:
                    d = np.nonzero(valid(kr, n) != valid(cr, n))[0]
                    print('    %d: %s: %d halfwords differ, first at %d' % (n, name, len(d), d[0]))
            if not np.array_equal(mac_raw(lib, x, n), cr):
                mac_bad += 1
        costs[n] = (ks, cs)
        print('%3d samples: %d signals: kernel vs C %d differ; C vc2sim vs Mac %d differ'
              % (n, len(sigs[n]), nd, mac_bad))
        fails += nd + mac_bad
    for n in SIZES:
        # the display's yardstick: a full-scale sine's peak bin over the
        # mean error power of a bin (the floor the bars can see)
        x = [s_ for c_, nm, s_ in sigs[n] if nm == 'sine full bin 17.00'][0]
        xr, xi = mac_fft(lib, x, n)
        peak = float(np.max(xr ** 2 + xi ** 2))
        rows = snr_row(lib, sigs[n], n)
        floor = max(r[4] for r in rows)
        print('  %d: a full-scale sine\'s peak bin is %.1f dB above the worst class\'s mean '
              'bin error power (%.2f)' % (n, 10 * math.log10(peak / floor), floor))
        for cls, total, worst, err, fl in rows:
            print('  SNR %3d %-8s %6.1f dB (worst signal %6.1f dB), worst bin error %.1f, '
                  'mean error power %.2f' % (n, cls, total, worst, err, fl))
    fails += variants(sigs)
    b = bands_case(lib, rng)
    print('bands and l2 against the model: %d differ' % b)
    fails += b
    b = bars_check(lib, rng)
    print('bars (jviz_bars_draw, the Mac build) against the model: %d of 122 differ '
          '(61 upright, 61 flipped)' % b)
    fails += b
    # the vector bars: every frame against jviz_bars_draw in vc2sim and the
    # model; the start-up check passes, and fails when told to
    bad, n, took, wrong = bars_vec_check(sim, random.Random(5))
    print('vector bars (viz_bars, jv_bars16) against jviz_bars_draw and the model: %d of '
          '%d frames differ (%d on the vector unit, %d declined as expected; %d choices '
          'wrong)' % (bad, n, took, n - took, wrong))
    fails += bad + wrong
    ok = [sim.s.call('viz_bars_init', f, check_abi=True, max_steps=10 ** 8,
                     regs={15: 0, 22: 0}) for f in (0, 1)]
    print('vector bars\' start-up check: %d (want 1), forced to fail %d (want 0)' % tuple(ok))
    fails += ok != [1, 0]
    for n in SIZES:
        sim.put(AT, split(sigs[n][0][2]))
        k = prof_call(sim.s, 'jv_fft%d' % n, AT + 0x1000, AT, sim.tabs[n])
        c = prof_call(sim.s, 'jviz_raw_ref', AT + 0x1000, AT, n, regs={15: 0, 22: 0})
        sim.s.mem.write(AT + 0x2000, bytes(2048))
        pw = prof_call(sim.s, 'jviz_raw_power', AT + 0x2000, AT + 0x1000, n)
        COST[n] = (k, c, pw)
        print('cost %d: kernel %d instructions (%d vector), ~%.0f cycles; C reference %d '
              'instructions; unpacking the powers %d' % (n, k[0], k[1], k[2], c[0], pw[0]))
        for mhz in (27, 150):
            print('    at %d MHz: kernel %.1f us, C %.2f ms (a 30 fps frame is 33.3 ms)' % (
                mhz, (k[2] + pw[2]) / mhz, c[2] / mhz / 1000))
    if '--mutations' in argv:
        caught = 0
        try:
            for m in ('sign', 'scale', 'lane'):
                regen(m)
                ms = Sim()
                nd = tot = 0
                for n in SIZES:
                    for cls, name, x in sigs[n][::3]:
                        kr, _ = ms.run(x, n)
                        cr, _ = ms.run(x, n, kernel=False)
                        nd += not np.array_equal(valid(kr, n), valid(cr, n))
                        tot += 1
                caught += nd > 0
                print('mutation %-6s: %d of %d signals differ: %s' % (
                    m, nd, tot, 'caught' if nd else 'NOT CAUGHT'))
            # the bars: each mutation changes frames, and the start-up
            # check refuses it (J_VPU_BARS would stay clear)
            for m in ('mask', 'grad', 'cap'):
                regen(m)
                ms = Sim()
                cases = bars_vec_cases(random.Random(5))[::4]
                bad, n, _, _ = bars_vec_check(ms, random.Random(5), cases, quiet=True)
                init = ms.s.call('viz_bars_init', 0, check_abi=True, max_steps=10 ** 8,
                                 regs={15: 0, 22: 0})
                caught += bad > 0 and not init
                print('mutation %-6s: %d of %d bars frames differ, the start-up check %s: %s'
                      % (m, bad, n, 'refuses it' if not init else 'PASSES it',
                         'caught' if bad and not init else 'NOT CAUGHT'))
        finally:
            regen()
        fails += 6 - caught
    # the v1 bars frame (24 bars over 320 x 150, gradient, caps, rounded
    # tops, background) in C on the BCM, no vector fill
    w, h, b = bars_cases(random.Random(1))[0]
    st = AT + 0x4000
    hb = st + 64
    sim.s.mem.write(st, np.array([b['n'], b['barw'], b['gap'], b['cap_h'], b['flags']],
                                 dtype='<u4').tobytes() +
                    np.array([b['bot'], b['top'], b['cap'], b['bg']], dtype='<u2').tobytes() +
                    np.array([hb, hb + 32], dtype='<u4').tobytes())
    sim.s.mem.write(hb, bytes(b['height'][:32]) + bytes(b['peak'][:32]))
    fb = AT + 0x10000
    for what, fl in (('with its background', b['flags']), ('bars and caps alone',
                                                          b['flags'] & ~BG)):
        sim.s.mem.write(st + 16, np.array([fl], dtype='<u4').tobytes())
        bc = prof_call(sim.s, 'jviz_bars_draw', fb, 320, w, h, st, 0, regs={15: 0, 22: 0})
        COST['bars ' + what] = bc
        print('cost bars, the v1 frame %s (C, no vector fill): %d instructions: %.2f ms at '
              '27 MHz, %.2f ms at 150 MHz' % (what, bc[0], bc[2] / 27000, bc[2] / 150000))
        bv = prof_call(sim.s, 'viz_bars', fb, 320, 0, w, h, st, regs={15: 0, 22: 0})
        COST['vector bars ' + what] = bv
        print('cost bars, the v1 frame %s (vector, viz_bars): %d instructions (%d vector), '
              '~%.0f cycles: %.2f ms at 27 MHz, %.2f ms at 150 MHz' % (
                  what, bv[0], bv[1], bv[2], bv[2] / 27000, bv[2] / 150000))
    # the full-screen frame (320 x 240 at (0, 0)): vector and C
    fb2 = AT + 0x40000
    fr = full_frame()
    sim.s.mem.write(st, np.array([fr['n'], fr['barw'], fr['gap'], fr['cap_h'], fr['flags']],
                                 dtype='<u4').tobytes() +
                    np.array([fr['bot'], fr['top'], fr['cap'], fr['bg']], dtype='<u2').tobytes() +
                    np.array([hb, hb + 32], dtype='<u4').tobytes())
    sim.s.mem.write(hb, bytes(fr['height'][:32]) + bytes(fr['peak'][:32]))
    bv = prof_call(sim.s, 'viz_bars', fb2, 320, 0, 320, 240, st, regs={15: 0, 22: 0})
    bc = prof_call(sim.s, 'jviz_bars_draw', fb2, 320, 320, 240, st, 0, regs={15: 0, 22: 0})
    COST['bars full screen'] = (bv, bc)
    print('cost bars, the full-screen frame (320 x 240): vector %d instructions (%d vector), '
          '~%.0f cycles: %.2f ms at 27 MHz; C %d instructions: %.2f ms at 27 MHz' % (
              bv[0], bv[1], bv[2], bv[2] / 27000, bc[0], bc[2] / 27000))
    sim.s.mem.write(st + 16, np.array([fr['flags'] | FLIP], dtype='<u4').tobytes())
    bf = prof_call(sim.s, 'viz_bars', fb2, 320, 0, 320, 240, st, regs={15: 0, 22: 0})
    COST['bars full screen flipped'] = bf
    print('cost bars, the full-screen frame flipped (J_BARS_FLIP): vector %d instructions '
          '(%d vector), ~%.0f cycles (upright %d, ~%.0f)' % (bf[0], bf[1], bf[2], bv[0], bv[2]))
    if bf[1] != bv[1] or bf[0] > bv[0] + 64:
        print('  FAIL: the flip costs vector work (it is addressing only)')
        fails += 1
    mc = []
    for y, mh, mb in mirror_frames():
        put_bars(sim.s, mb)
        mc.append(prof_call(sim.s, 'viz_bars', fb2 + 640 * y, 320, 0, 320, mh, BARS_ST,
                            regs={15: 0, 22: 0}))
    COST['mirror full screen'] = mc
    print('cost the Mirror, full screen (bars 320 x %d, reflection 320 x %d flipped): '
          'vector %d + %d instructions, ~%.0f cycles: %.2f ms at 27 MHz' % (
              mirror_frames()[0][1], mirror_frames()[1][1], mc[0][0], mc[1][0],
              mc[0][2] + mc[1][2], (mc[0][2] + mc[1][2]) / 27000))
    bi = prof_call(sim.s, 'viz_bars_init', 0, regs={15: 0, 22: 0})
    print('cost the vector bars\' start-up check: %d instructions, ~%.0f cycles (%.2f ms at '
          '27 MHz)' % (bi[0], bi[2], bi[2] / 27000))
    fails += draw_main(sim, lib, random.Random(36))
    print('viz_test: %d failed (%.0f s)' % (fails, time.time() - t0))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
