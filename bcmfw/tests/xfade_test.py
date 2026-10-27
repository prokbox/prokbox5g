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
"""Stage 5b's cross-fade (bcmfw/video/xfade.c and xfade_kernels.S,
offload.md) in vc2sim: whole-panel cross-fades (160x240, the drift's 8
steps and the highlight fade's 6), both store kernels (single-row and
REP 16), bit for bit against a port of the shell's xfade_rect
(apps/ipodui/preview.c), with the planes made by the image's own
xfade_unpack; plus the cost per step: instructions counted by the
simulator, cycles estimated from the device's own REP timings (bench
B4K, device sessions 5-7).  It also checks that the shell's xfade_rect
and nav_xfade_weight still read as this port does, so a change to the
shell's fade shows here first.

  bcmfw/tests/xfade_test.py [--seed N]
"""
import os
import random
import struct
import subprocess
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
import gen_xfade as G  # noqa: E402

CFLAGS = ['-std=gnu99', '-Wall', '-Wextra', '-Werror']
OUT, STATE, SRC, PIX = 0x800000, 0x400000, 0x500000, 0x600000   # program at 0x200000
PITCH = 640                                   # the LCD framebuffer's
W, H = 160, 240
PLANE = 2 * W * H

# Device REP timings, cycles per instruction at REP 16 (bench B4K,
# sessions 5-6, identical both runs): ALU 16.13-16.25, vld16 16.63,
# vst16 16.50 (one row, no REP: 1.50, S161); scalar ~1.03; a call/return
# pair 2.59.
CYC = {'valu': 16.25, 'vld16': 16.63, 'vst16': 16.50, 'vst16_1': 1.50, 'scalar': 1.03}


def xfade_weight(k, n):
    """nav_xfade_weight() (apps/ipodui/nav_core.c)."""
    if k >= n:
        return 256
    if k < 1:
        k = 1
    return (256 + (n - k + 1) // 2) // (n - k + 1)


def xfade_rect_ref(dst, src, weight):
    """apps/ipodui/preview.c xfade_rect(), on lists of RGB565 values."""
    if weight >= 256:
        return list(src)
    w32 = (weight + 4) >> 3
    iw = 32 - w32
    if w32 == 0:
        w32, iw = 1, 31
    out = []
    for d, s in zip(dst, src):
        if d == s:
            out.append(d)
            continue
        d = (d | (d << 16)) & 0x07e0f81f
        s = (s | (s << 16)) & 0x07e0f81f
        r = ((d * iw + s * w32 + 0x02008010) >> 5) & 0x07e0f81f
        out.append((r | (r >> 16)) & 0xffff)
    return out


def avg2_ref(a, b):
    """preview.c AVG2, per pixel: each RGB565 channel's floor average."""
    return [(x & y) + (((x ^ y) & 0xf7de) >> 1) for x, y in zip(a, b)]


def put_planes(sim, base, px):
    """The window into planes by the image's own xfade_unpack."""
    sim.mem.write(PIX, struct.pack('<%dH' % (W * H), *px))
    sim.call('xfade_unpack', base, PIX, W, check_abi=True, max_steps=10_000_000,
             regs={15: 0, 22: 0})


def get_px(sim, base, w, h):
    out = []
    for y in range(h):
        out += struct.unpack('<%dH' % w, sim.mem.read(base + y * PITCH, 2 * w))
    return out


def picture(rng, w, h, kind):
    if kind == 'random':
        return [rng.getrandbits(16) for _ in range(w * h)]
    if kind == 'extreme':
        return [rng.choice((0, 0xffff, 0xf800, 0x07e0, 0x001f)) for _ in range(w * h)]
    # a cover-like picture: smooth gradients with flat areas
    a, b = rng.randrange(1, 9), rng.randrange(1, 9)
    return [(((x * a // 4) & 31) << 11) | (((y * b // 4) & 63) << 5) | ((x + y) // 16 & 31)
            for y in range(h) for x in range(w)]


def static_cycles(rep, lines=None):
    """(instructions, cycle estimate) of one kernel call."""
    n = cyc = 0
    for line in lines if lines is not None else G.xfade_blk(rep):
        op = line.split()[0]
        n += 1
        if op.startswith('vld16'):
            cyc += CYC['vld16']
        elif op.startswith('vst16'):
            cyc += CYC['vst16'] if 'rep16' in line else CYC['vst16_1']
        elif op.startswith('v'):
            cyc += CYC['valu']
        else:
            cyc += CYC['scalar']
    return n, cyc


SHELL = os.path.join(ROOT, 'apps', 'ipodui')
HALF_REV = '91b06b7784'         # s8-int: the 8b half steps (p8b-smooth 9fcbf9aed4)


def shell_unchanged():
    """The shell's fade as this file ports it (a changed shell: port it)."""
    pv = open(os.path.join(SHELL, 'preview.c')).read()
    nav = open(os.path.join(SHELL, 'nav_core.c')).read()
    want = ['unsigned w32 = (weight + 4) >> 3, iw = 32 - w32;',
            'd = (d | (d << 16)) & 0x07e0f81f;',
            'r = ((d * iw + s * w32 + 0x02008010) >> 5) & 0x07e0f81f;',
            'w32 = 1, iw = 31;', 'if (weight >= 256)']
    miss = [w for w in want if w not in pv]
    if 'return (256 + (n - k + 1) / 2) / (n - k + 1);' not in nav:
        miss.append('nav_xfade_weight')
    return miss


def shell_half(rev=HALF_REV):
    """The 8b half step as ported (AVG2, NAV_DRIFT_HALF), checked in the
    shell's source at the commit that defined it: s8-int (p8b-smooth);
    this branch's own shell has no half steps yet."""
    def show(path):
        try:
            return subprocess.check_output(['git', '-C', ROOT, 'show', '%s:%s' % (rev, path)],
                                           stderr=subprocess.DEVNULL).decode()
        except (subprocess.CalledProcessError, OSError):
            return None
    pv, hd = show('apps/ipodui/preview.c'), show('apps/ipodui/nav_core.h')
    if pv is None or hd is None:
        return None                     # the commit isn't here: not checked
    want = [(pv, '#define AVG2(a, b) (((a) & (b)) + ((((a) ^ (b)) & 0xf7def7deu) >> 1))'),
            (pv, '#define HALF_SLOW_US    25000'), (pv, '#define HALF_SLOW_MAX   4'),
            (hd, '#define NAV_DRIFT_HALF      ((NAV_DRIFT_STEP + 1) / 2)'),
            (hd, '#define NAV_DRIFT_STEP      15')]
    return [w for src, w in want if w not in src]


def vector_r7():
    """Vector ops naming r7 in the kernels: on silicon a scalar B operand
    or a memory base in r7 reads as 0 (quirk #19, vc2vpu v_sreg7 and
    v_mem_base7), which the simulator's default hides only if the kernel
    meant 0.  The kernels must not name it at all."""
    import re
    path = os.path.join(VID, 'xfade_kernels.S')
    return [line.strip() for line in open(path)
            if re.match(r'\s*v[a-z0-9]+\s', line) and re.search(r'\br7\b', line)]


def run_kernel(rep, rng, cost):
    prog = C.build([os.path.join(VID, 'xfade.c'), G.OUT_S], '-O2',
                   extra=CFLAGS + ['-DBCMFW_DRIFT_REP_STORE=%d' % rep],
                   libs=C.DEFAULT_LIBS, no_float=True, conservative=True)
    sim = S.Sim()
    C.load_into_sim(prog, sim)
    bad = cases = 0
    steps = []
    for kind in ('cover', 'random', 'extreme'):
        for n in (8, 6):                      # NAV_DRIFT_FADE_STEPS, NAV_PANEL_XFADE_STEPS
            cur = picture(rng, W, H, kind)
            new = picture(rng, W, H, kind)
            put_planes(sim, STATE, cur)
            put_planes(sim, SRC, new)
            for k in range(1, n):             # step n (256) is the window copy
                wt = xfade_weight(k, n)
                s0 = sim.steps
                sim.call('xfade_panel', OUT, STATE, SRC, wt,
                         check_abi=True, max_steps=10_000_000,
                         regs={15: 0, 22: 0})
                steps.append(sim.steps - s0)
                cur = xfade_rect_ref(cur, new, wt)
                cases += 1
                if get_px(sim, OUT, W, H) != cur:
                    bad += 1
                    print('FAIL %s stores: %s fade, %d steps, step %d (weight %d)' % (
                        'REP 16' if rep else 'single', kind, n, k, wt))
    blocks = (W // 16) * (H // 16)
    kn, kc = static_cycles(rep)
    per_step = sum(steps) / len(steps)
    loop = per_step - blocks * kn
    cyc = blocks * kc + loop * CYC['scalar']
    cost.append('%s stores: xfade_blk %d instructions, ~%.0f cycles a block; a step '
                '%.0f instructions, ~%.0f k cycles: %.2f ms at 27 MHz, %.2f at 150'
                % ('REP 16' if rep else 'single', kn, kc, per_step, cyc / 1e3,
                   cyc / 27e3, cyc / 150e3))
    return cases, bad


COVER = 0x700000                # a 300 x 300 cover, for the half step's windows


def run_half(rng, cost):
    """The half step and the staging copy, bit for bit against AVG2, from
    windows at every parity of x (the shell's odd-start re-pairing)."""
    prog = C.build([os.path.join(VID, 'xfade.c'), G.OUT_S], '-O2', extra=CFLAGS,
                   libs=C.DEFAULT_LIBS, no_float=True, conservative=True)
    sim = S.Sim()
    C.load_into_sim(prog, sim)
    side = 300
    bad = cases = 0
    steps = []
    for kind in ('cover', 'random', 'extreme'):
        cv = picture(rng, side, side, kind)
        sim.mem.write(COVER, struct.pack('<%dH' % (side * side), *cv))
        for (x0, y0), (dx, dy) in (((92, 30), (-1, 0)), ((47, 8), (1, 1)),
                                   ((70, 52), (0, -1)), ((3, 0), (1, 0))):
            x1, y1 = x0 + dx, y0 + dy
            a = [cv[(y0 + r) * side + x0 + c] for r in range(H) for c in range(W)]
            b = [cv[(y1 + r) * side + x1 + c] for r in range(H) for c in range(W)]
            for y in range(H):              # the panel shows window a
                sim.mem.write(OUT + y * PITCH, struct.pack('<%dH' % W, *a[y * W:(y + 1) * W]))
            sim.call('xfade_stage', PIX, COVER + 2 * (y1 * side + x1), side,
                     check_abi=True, max_steps=10_000_000, regs={15: 0, 22: 0})
            s0 = sim.steps
            sim.call('xfade_half', OUT, PIX, check_abi=True, max_steps=10_000_000,
                     regs={15: 0, 22: 0})
            steps.append(sim.steps - s0)
            cases += 1
            if get_px(sim, OUT, W, H) != avg2_ref(a, b):
                bad += 1
                print('FAIL half step: %s, (%d,%d) -> (%d,%d)' % (kind, x0, y0, x1, y1))
            sim.call('xfade_copy', OUT, PIX, check_abi=True, max_steps=10_000_000,
                     regs={15: 0, 22: 0})
            cases += 1
            if get_px(sim, OUT, W, H) != b:
                bad += 1
                print('FAIL staged copy: %s, (%d,%d)' % (kind, x1, y1))
    blocks = (W // 16) * (H // 16)
    kn, kc = static_cycles(0, G.half_blk())
    per_step = sum(steps) / len(steps)
    cyc = blocks * kc + (per_step - blocks * kn) * CYC['scalar']
    cost.append('half step: half_blk %d instructions, ~%.0f cycles a block; a step %.0f '
                'instructions, ~%.0f k cycles: %.2f ms at 27 MHz, %.2f at 150 (plus the '
                'staging copy, scalar: about %d instructions)'
                % (kn, kc, per_step, cyc / 1e3, cyc / 27e3, cyc / 150e3, stage_cost(sim)))
    return cases, bad


def stage_cost(sim):
    s0 = sim.steps
    sim.call('xfade_stage', PIX, COVER + 2 * 1, 300, check_abi=True,
             max_steps=10_000_000, regs={15: 0, 22: 0})
    return sim.steps - s0


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--seed', type=int, default=2722)
    a = ap.parse_args(argv)
    if not os.path.exists(C.GCC):
        print('skip: vc4-elf-gcc not found (%s)' % C.GCC)
        return 0
    if G.main(['--check']):
        return 1
    t0 = time.time()
    miss = shell_unchanged()
    if miss:
        print('FAIL the shell\'s fade changed: %s (port it here and in '
              'bcm-devtest-core.c drift_xfade)' % miss)
    rng = random.Random(a.seed)
    cost = []
    cases = bad = 0
    for rep in (0, 1):
        c, b = run_kernel(rep, rng, cost)
        cases += c
        bad += b
    c, b = run_half(rng, cost)
    cases += c
    bad += b
    half = shell_half()
    if half:
        print('FAIL the shell\'s half step changed at %s: %s (port it here and in '
              'bcm-devtest-core.c drift_avg)' % (HALF_REV, half))
        bad += 1
    elif half is None:
        print('note: %s not in this repository: the half step\'s shell check skipped'
              % HALF_REV)
    r7 = vector_r7()
    if r7:
        print('FAIL vector ops name r7 (reads as 0 on silicon, quirk #19): %s' % r7[:3])
        bad += 1
    for line in cost:
        print(line)
    bad += 1 if miss else 0
    print('%d rows, %d cases, %d failed (%.0f s)' % (5, cases + 3, bad, time.time() - t0))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
