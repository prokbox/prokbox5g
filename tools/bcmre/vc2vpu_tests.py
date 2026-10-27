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
"""Vector-unit experiments for vc2sim --selftest.

Each test runs one of Apple's VPU kernels (reference material only) on
random inputs and compares it with the textbook definition of what it
computes: H.264 (ITU-T H.264 8.3, 8.4.2.2, 8.5.12, 8.7) written from the
specification, not from the disassembly.  Where a kernel's inputs come
from loaders whose misaligned-load semantics are still open, the test
places the inputs in the VRF itself; the layout it uses is the claim
under test (documented per test and in sim.md).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vc2sim_tests as T  # noqa: E402

LIB = 'h264dec'


def clip1(v):
    return 0 if v < 0 else 255 if v > 255 else v


def w16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


def _sim(ctx, lib=LIB):
    return ctx.vll(lib)


def _rand_vrf(sim, rng):
    sim.vrf.b[:] = bytes(rng.getrandbits(8) for _ in range(4096))


def _put(vrf, y, x, data):
    vrf.b[y * 64 + x:y * 64 + x + len(data)] = bytes(data)


def _get(vrf, y, x, n):
    return list(vrf.b[y * 64 + x:y * 64 + x + n])


# 4x4 zigzag scan (H.264 Table 8-13, frame scan): scan index -> raster index
ZIGZAG4 = [0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15]


def t_invzigzag(ctx):
    """invzigzag(src, dst): 16 V16 columns (one coefficient per column, 16
    blocks in the lanes) in scan order -> raster order."""
    r = T.Row(ctx, 'invzigzag (VPU)', LIB, 'V16 columns, REP2 moves')
    sim, rng = _sim(ctx), ctx.rng
    for _ in range(100):
        _rand_vrf(sim, rng)
        src, dst = rng.choice([(0, 32), (1024, 1056), (32, 0)])
        cols = [[rng.randrange(-2048, 2048) for _ in range(16)] for _ in range(16)]
        for k in range(16):
            sim.vrf.write(src + k, True, 2, cols[k], range(16))
        if r.guard(lambda: T.call(sim, r, 'invzigzag', src, dst), 'call') is None:
            continue
        want = [None] * 16
        for i, k in enumerate(ZIGZAG4):
            want[k] = cols[i]
        got = [sim.vrf.read(dst + k, True, 2) for k in range(16)]
        r.check(got == want, 'src %d dst %d' % (src, dst))


def idct4(c):
    """H.264 8.5.12.2 4x4 inverse transform (before the final (x+32)>>6)."""
    def t(a):
        e, f = a[0] + a[2], a[0] - a[2]
        g, h = (a[1] >> 1) - a[3], a[1] + (a[3] >> 1)
        return [e + h, f + g, f - g, e - h]
    o = [0] * 16
    for i in range(4):
        o[4 * i:4 * i + 4] = t(c[4 * i:4 * i + 4])
    for j in range(4):
        v = t([o[j], o[4 + j], o[8 + j], o[12 + j]])
        for i in range(4):
            o[4 * i + j] = v[i]
    return o


def t_inverseinteger(ctx):
    """vc_inverseinteger(base, tmp): coefficient k of 16 blocks in V16
    column k (+base), transformed in place, tmp = scratch columns."""
    r = T.Row(ctx, 'vc_inverseinteger (VPU)', LIB, 'H.264 8.5.12.2')
    sim, rng = _sim(ctx), ctx.rng
    for _ in range(100):
        _rand_vrf(sim, rng)
        base = rng.choice([0, 1024])
        blocks = [[rng.randrange(-4096, 4096) for _ in range(16)] for _ in range(16)]
        for k in range(16):
            sim.vrf.write(base + k, True, 2, [blocks[l][k] for l in range(16)], range(16))
        if r.guard(lambda: T.call(sim, r, 'vc_inverseinteger', base, 32), 'call') is None:
            continue
        got = [sim.vrf.read(base + k, True, 2) for k in range(16)]
        good = all([w16(v) for v in idct4(blocks[l])] == [got[k][l] for k in range(16)]
                   for l in range(16))
        r.check(good, 'base %d' % base)


def six_tap(p):
    """H.264 8.4.2.2.1 luma 6-tap on p[0..5] (E F G H I J)."""
    return p[0] - 5 * p[1] + 20 * p[2] + 20 * p[3] - 5 * p[4] + p[5]


def t_interp_hoz(ctx):
    """vc_interp16xN_hoz(32, 0, 960): rows 0-15, columns 32.. hold the
    reference from x-2; output half-pel b, H8 rows 0-15 at column 0."""
    r = T.Row(ctx, 'vc_interp16xN_hoz (VPU)', LIB, 'H.264 8.4.2.2.1 b')
    sim, rng = _sim(ctx), ctx.rng
    for _ in range(60):
        _rand_vrf(sim, rng)
        P = [[rng.getrandbits(8) for _ in range(32)] for _ in range(16)]
        for y in range(16):
            _put(sim.vrf, y, 32, P[y])
        if r.guard(lambda: T.call(sim, r, 'vc_interp16xN_hoz', 32, 0, 960), 'call') is None:
            continue
        good = all(_get(sim.vrf, y, 0, 16) ==
                   [clip1((six_tap(P[y][x:x + 6]) + 16) >> 5) for x in range(16)]
                   for y in range(16))
        r.check(good, 'case')


def t_interp_vert(ctx):
    """vc_interp16xN_vert(32, -, 960): rows 0-20 at column 32 hold the
    reference from y-2; output half-pel h, H8 rows 0-15 at column 0."""
    r = T.Row(ctx, 'vc_interp16xN_vert (VPU)', LIB, 'H.264 8.4.2.2.1 h')
    sim, rng = _sim(ctx), ctx.rng
    for _ in range(60):
        _rand_vrf(sim, rng)
        P = [[rng.getrandbits(8) for _ in range(16)] for _ in range(21)]
        for y in range(21):
            _put(sim.vrf, y, 32, P[y])
        if r.guard(lambda: T.call(sim, r, 'vc_interp16xN_vert', 32, 0, 960), 'call') is None:
            continue
        good = all(_get(sim.vrf, y, 0, 16) ==
                   [clip1((six_tap([P[y + k][x] for k in range(6)]) + 16) >> 5)
                    for x in range(16)] for y in range(16))
        r.check(good, 'case')


def t_chroma_edge_strong(ctx):
    """vc_ChromaEdges_edge, bS = 4 path: p1 p0 q0 q1 are V8 columns
    46..49 + r6 (x wraps mod 64: r6 = 32 reaches the transposed block at
    columns 14..17), lanes = 16 rows; alpha/beta are the low/high bytes of
    the H16 row 56, bS the bytes (27, 0..3)."""
    r = T.Row(ctx, 'vc_ChromaEdges_edge bS=4 (VPU)', LIB, 'H.264 8.7.2.4 chroma')
    sim, rng = _sim(ctx), ctx.rng
    for _ in range(120):
        r6 = rng.choice([0, 4, 32, 36])
        _rand_vrf(sim, rng)
        alpha = [rng.randrange(0, 64) for _ in range(16)]
        beta = [rng.randrange(0, 20) for _ in range(16)]
        _put(sim.vrf, 56, 0, alpha + beta)
        _put(sim.vrf, 27, 0, [4] * 4)
        base = (46 + r6) & 63
        cols = []
        for c in range(4):
            if rng.random() < 0.7:
                v = [100 + rng.randrange(-10, 10) for _ in range(16)]
            else:
                v = [rng.getrandbits(8) for _ in range(16)]
            cols.append(v)
            for i in range(16):
                sim.vrf.b[i * 64 + ((base + c) & 63)] = v[i]
        if r.guard(lambda: T.call(sim, r, 'vc_ChromaEdges_edge', regs={6: r6}),
                   'call') is None:
            continue
        p1, p0, q0, q1 = cols
        good = True
        for i in range(16):
            f = abs(p0[i] - q0[i]) < alpha[i] and abs(p1[i] - p0[i]) < beta[i] \
                and abs(q1[i] - q0[i]) < beta[i]
            wp = (2 * p1[i] + p0[i] + q1[i] + 2) >> 2 if f else p0[i]
            wq = (2 * q1[i] + q0[i] + p1[i] + 2) >> 2 if f else q0[i]
            good &= sim.vrf.b[i * 64 + ((base + 1) & 63)] == wp
            good &= sim.vrf.b[i * 64 + ((base + 2) & 63)] == wq
        r.check(good, 'r6 %d' % r6)


TESTS = [t_invzigzag, t_inverseinteger, t_interp_hoz, t_interp_vert,
         t_chroma_edge_strong]


# --------------------------------------------------------------------------
# Kernels that reach memory through the 48-bit loads/stores.

def _plane(sim, rng, w, h, align=16):
    a = sim.malloc(w * h + 64, align)
    data = bytes(rng.getrandbits(8) for _ in range(w * h))
    sim.mem.write(a, data)
    return a


def t_clearnzcoeffs(ctx):
    """h264_clearnzcoeffs(p): vmov.1 d00, #0; vst16 d00, (r0): 32 zero
    bytes (an H16 row) at p, nothing else."""
    r = T.Row(ctx, 'h264_clearnzcoeffs (VPU)', LIB, 'vst16 = 32 bytes')
    sim, rng = _sim(ctx), ctx.rng
    for _ in range(40):
        _rand_vrf(sim, rng)
        a = _plane(sim, rng, 96, 1)
        before = sim.mem.read(a, 96)
        p = a + 32
        if r.guard(lambda: T.call(sim, r, 'h264_clearnzcoeffs', p), 'call') is None:
            continue
        after = sim.mem.read(a, 96)
        r.check(after == before[:32] + bytes(32) + before[64:], 'p %x' % p)


def _residual16(vrf, rng, y0, x0, rows, lim=300):
    """Random H16 residual block: rows x 16 lanes at (y0, x0)."""
    res = []
    for y in range(rows):
        v = [rng.randrange(-lim, lim) for _ in range(16)]
        vrf.write((y0 + y) * 64 + x0, False, 2, v, range(16))
        res.append(v)
    return res


def intra4x4_ref(mode, P):
    """H.264 8.3.1.2.1-.9: pred[y][x] from P(x, y) (x = -1..7 above,
    y = -1..3 left; DC variants as 'dc3' both, 'dc1' left, 'dc2' above,
    'dc0' none)."""
    def f3(a, b, c):
        return (a + 2 * b + c + 2) >> 2
    pr = [[0] * 4 for _ in range(4)]
    for y in range(4):
        for x in range(4):
            if mode == 'vertical':
                v = P(x, -1)
            elif mode == 'horizontal':
                v = P(-1, y)
            elif mode.startswith('dc'):
                a = sum(P(i, -1) for i in range(4))
                lf = sum(P(-1, i) for i in range(4))
                v = {'dc3': (a + lf + 4) >> 3, 'dc1': (lf + 2) >> 2, 'dc2': (a + 2) >> 2,
                     'dc0': 128}[mode]
            elif mode == 'ddl':
                v = (P(6, -1) + 3 * P(7, -1) + 2) >> 2 if x == y == 3 else \
                    f3(P(x + y, -1), P(x + y + 1, -1), P(x + y + 2, -1))
            elif mode == 'ddr':
                if x > y:
                    v = f3(P(x - y - 2, -1), P(x - y - 1, -1), P(x - y, -1))
                elif x < y:
                    v = f3(P(-1, y - x - 2), P(-1, y - x - 1), P(-1, y - x))
                else:
                    v = f3(P(0, -1), P(-1, -1), P(-1, 0))
            elif mode == 'vr':
                z = 2 * x - y
                if z >= 0 and z % 2 == 0:
                    v = (P(x - (y >> 1) - 1, -1) + P(x - (y >> 1), -1) + 1) >> 1
                elif z >= 0:
                    v = f3(P(x - (y >> 1) - 2, -1), P(x - (y >> 1) - 1, -1), P(x - (y >> 1), -1))
                elif z == -1:
                    v = f3(P(-1, 0), P(-1, -1), P(0, -1))
                else:
                    v = f3(P(-1, y - 1), P(-1, y - 2), P(-1, y - 3))
            elif mode == 'hd':
                z = 2 * y - x
                if z >= 0 and z % 2 == 0:
                    v = (P(-1, y - (x >> 1) - 1) + P(-1, y - (x >> 1)) + 1) >> 1
                elif z >= 0:
                    v = f3(P(-1, y - (x >> 1) - 2), P(-1, y - (x >> 1) - 1), P(-1, y - (x >> 1)))
                elif z == -1:
                    v = f3(P(-1, 0), P(-1, -1), P(0, -1))
                else:
                    v = f3(P(x - 1, -1), P(x - 2, -1), P(x - 3, -1))
            elif mode == 'vl':
                if y % 2 == 0:
                    v = (P(x + (y >> 1), -1) + P(x + (y >> 1) + 1, -1) + 1) >> 1
                else:
                    v = f3(P(x + (y >> 1), -1), P(x + (y >> 1) + 1, -1), P(x + (y >> 1) + 2, -1))
            elif mode == 'hu':
                z = x + 2 * y
                if z > 5:
                    v = P(-1, 3)
                elif z == 5:
                    v = (P(-1, 2) + 3 * P(-1, 3) + 2) >> 2
                elif z % 2 == 0:
                    v = (P(-1, y + (x >> 1)) + P(-1, y + (x >> 1) + 1) + 1) >> 1
                else:
                    v = f3(P(-1, y + (x >> 1)), P(-1, y + (x >> 1) + 1), P(-1, y + (x >> 1) + 2))
            pr[y][x] = v
    return pr


# mode -> (symbol, entry offset, above-right replicated?)
INTRA4 = {
    'vertical': ('vc_intra_4x4_vertical', 0, False),
    'horizontal': ('vc_intra_4x4_horizontal', 0, False),
    'dc0': ('vc_intra_4x4_dc', 0, False), 'dc1': ('vc_intra_4x4_dc', 0, False),
    'dc2': ('vc_intra_4x4_dc', 0, False), 'dc3': ('vc_intra_4x4_dc', 0, False),
    'ddl': ('vc_intra_4x4_diagonal_down_left_special', 0x16, False),
    'ddl_special': ('vc_intra_4x4_diagonal_down_left_special', 0, True),
    'ddr': ('vc_intra_4x4_diagonal_down_right', 0, False),
    'vr': ('vc_intra_4x4_vertical_right', 0, False),
    'hd': ('vc_intra_4x4_horizontal_down', 0, False),
    'vl': ('vc_intra_4x4_vertical_left', 0, False),
    'vl_special': ('vc_intra_4x4_vertical_left_special', 0, True),
    'hu': ('vc_intra_4x4_horizontal_up', 0, False),
}


def t_intra4x4(ctx):
    """vc_intra_4x4_* + vc_intra_4x4_storeback (dst, stride, residual
    offset [, -, avail]): all nine modes, both above-right cases; the
    prediction is built at (16, 48) (lanes 4y+x), the residual is H16 at
    r2, clip(pred + res) is stored as 4 rows.  The '_special' symbols
    are the above-right-unavailable entries (p[4..7,-1] = p[3,-1]); the
    plain DDL entry is 0x16 bytes into its symbol."""
    r = T.Row(ctx, 'vc_intra_4x4_* + storeback (VPU)', LIB, 'H.264 8.3.1.2 all modes')
    sim, rng = _sim(ctx), ctx.rng
    stride = 64
    for it in range(24 * len(INTRA4)):
        mode = sorted(INTRA4)[it % len(INTRA4)]
        sym, entry, special = INTRA4[mode]
        _rand_vrf(sim, rng)
        base = _plane(sim, rng, stride, 8)
        dst = base + 2 * stride + rng.choice([4, 8, 12, 16, 20, 28, 36, 52])
        frame = sim.mem.read(base, stride * 8)

        def P(x, y):
            if special and y == -1 and x > 3:
                x = 3
            return frame[dst - base + y * stride + x]
        r2 = rng.choice([0, 32 * 64, 32 * 64 + 32])
        res = _residual16(sim.vrf, rng, r2 >> 6, r2 & 63, 1)[0]
        pred = intra4x4_ref(mode.replace('_special', ''), P)
        avail = {'dc0': 0, 'dc1': 1, 'dc2': 2, 'dc3': 3}.get(mode, 0)
        fa = sim.sym(sym) + entry
        if r.guard(lambda: T.call(sim, r, fa, dst, stride, r2, 0, avail, regs={11: 0}),
                   mode) is None:
            continue
        want = bytearray(frame)
        for y in range(4):
            for x in range(4):
                want[dst - base + y * stride + x] = clip1(pred[y][x] + res[4 * y + x])
        r.check(sim.mem.read(base, stride * 8) == bytes(want), '%s dst&15=%d' % (mode, dst & 15))


def t_intra16x16(ctx):
    """h264_calculateintrapred16x16(dst, stride, mode, avail): vertical,
    horizontal, DC (4 availability cases), plane (h264_lumaplane_data
    weights); residual H16 rows 32-47 at x 32; clip(pred + res) stored."""
    r = T.Row(ctx, 'h264_calculateintrapred16x16 (VPU)', LIB, 'H.264 8.3.3.1-.4 + residual')
    sim, rng = _sim(ctx), ctx.rng
    stride = 64
    modes = ['vertical', 'horizontal', 'dc0', 'dc1', 'dc2', 'dc3', 'plane']
    for it in range(20 * len(modes)):
        mode = modes[it % len(modes)]
        _rand_vrf(sim, rng)
        base = _plane(sim, rng, stride, 18)
        dst = base + stride + 16
        frame = sim.mem.read(base, stride * 18)

        def P(x, y):
            return frame[dst - base + y * stride + x]
        res = _residual16(sim.vrf, rng, 32, 32, 16)
        above = [P(x, -1) for x in range(16)]
        left = [P(-1, y) for y in range(16)]
        avail = 0
        if mode == 'vertical':
            pred, m = [above] * 16, 0
        elif mode == 'horizontal':
            pred, m = [[left[y]] * 16 for y in range(16)], 1
        elif mode == 'plane':
            m = 3
            hh = sum((x + 1) * (P(8 + x, -1) - P(6 - x, -1)) for x in range(8))
            vv = sum((y + 1) * (P(-1, 8 + y) - P(-1, 6 - y)) for y in range(8))
            a = 16 * (P(-1, 15) + P(15, -1))
            b, c = (5 * hh + 32) >> 6, (5 * vv + 32) >> 6
            pred = [[clip1((a + b * (x - 7) + c * (y - 7) + 16) >> 5) for x in range(16)]
                    for y in range(16)]
        else:
            m, avail = 2, int(mode[2])
            dc = {3: (sum(above) + sum(left) + 16) >> 5, 1: (sum(left) + 8) >> 4,
                  2: (sum(above) + 8) >> 4, 0: 128}[avail]
            pred = [[dc] * 16] * 16
        if r.guard(lambda: T.call(sim, r, 'h264_calculateintrapred16x16', dst, stride, m,
                                  avail), mode) is None:
            continue
        want = bytearray(frame)
        for y in range(16):
            for x in range(16):
                want[dst - base + y * stride + x] = clip1(pred[y][x] + res[y][x])
        r.check(sim.mem.read(base, stride * 18) == bytes(want), mode)


def t_combinepred(ctx):
    """h264_combinepred: luma prediction H8 rows 0-15 (x 0..15) +
    residual V16 at (32, 32); chroma prediction rows 16-31 (x 0..7) +
    residual V16 at (48, 32); saturated, in place (the D is V8)."""
    r = T.Row(ctx, 'h264_combinepred (VPU)', LIB, 'clip(pred + residual)')
    sim, rng = _sim(ctx), ctx.rng
    for _ in range(40):
        _rand_vrf(sim, rng)
        pl = [[rng.getrandbits(8) for _ in range(16)] for _ in range(16)]
        pc = [[rng.getrandbits(8) for _ in range(8)] for _ in range(16)]
        for y in range(16):
            _put(sim.vrf, y, 0, pl[y])
            _put(sim.vrf, 16 + y, 0, pc[y])
        rl = _residual16(sim.vrf, rng, 32, 32, 16)
        rc = _residual16(sim.vrf, rng, 48, 32, 16)
        if r.guard(lambda: T.call(sim, r, 'h264_combinepred'), 'call') is None:
            continue
        good = all(_get(sim.vrf, y, 0, 16) == [clip1(pl[y][x] + rl[y][x]) for x in range(16)]
                   for y in range(16))
        good &= all(_get(sim.vrf, 16 + y, 0, 8) == [clip1(pc[y][x] + rc[y][x]) for x in range(8)]
                    for y in range(16))
        r.check(good, 'case')


def t_storeback(ctx):
    """vc_storeback(y, u, v, stride): luma H8 rows 0-15 -> y, chroma
    U rows 16-23 / V rows 24-31 (8 bytes each, vstr ifnz) -> u, v with
    stride / 2."""
    r = T.Row(ctx, 'vc_storeback (VPU)', LIB, 'vst8 rep16 + vstr rep8')
    sim, rng = _sim(ctx), ctx.rng
    stride = 64
    for _ in range(40):
        _rand_vrf(sim, rng)
        yb = _plane(sim, rng, stride, 16)
        ub = _plane(sim, rng, stride // 2, 8)
        vb = _plane(sim, rng, stride // 2, 8)
        fy, fu, fv = (sim.mem.read(yb, stride * 16), sim.mem.read(ub, stride * 4),
                      sim.mem.read(vb, stride * 4))
        ox, oc = rng.choice([0, 16, 48]), rng.choice([0, 8, 16, 24])
        if r.guard(lambda: T.call(sim, r, 'vc_storeback', yb + ox, ub + oc, vb + oc, stride),
                   'call') is None:
            continue
        wy, wu, wv = bytearray(fy), bytearray(fu), bytearray(fv)
        for y in range(16):
            wy[y * stride + ox:y * stride + ox + 16] = bytes(_get(sim.vrf, y, 0, 16))
        for y in range(8):
            o = y * stride // 2 + oc
            wu[o:o + 8] = bytes(_get(sim.vrf, 16 + y, 0, 8))
            wv[o:o + 8] = bytes(_get(sim.vrf, 24 + y, 0, 8))
        r.check(sim.mem.read(yb, stride * 16) == bytes(wy) and
                sim.mem.read(ub, stride * 4) == bytes(wu) and
                sim.mem.read(vb, stride * 4) == bytes(wv), 'ox %d oc %d' % (ox, oc))


def t_interp_chroma(ctx):
    """vc_interp_chr8x8 / 8x4(u, v, stride, x8, y8, width, height):
    eighth-pel chroma prediction, U at rows 16.., V at rows 24.. (8x8) or
    U rows 16-19 / V rows 24-27 (8x4), x 0..7.  Positions inside the
    plane, any alignment (exercises vldp8/vldq8 line splitting)."""
    r = T.Row(ctx, 'vc_interp_chr8x8/8x4 (VPU)', LIB, 'H.264 8.4.2.2.2')
    sim, rng = _sim(ctx), ctx.rng
    w, h = 64, 40
    for _ in range(120):
        f, rows = rng.choice([('vc_interp_chr8x8', 8), ('vc_interp_chr8x4', 4)])
        _rand_vrf(sim, rng)
        ub, vb = _plane(sim, rng, w, h), _plane(sim, rng, w, h)
        U, V = sim.mem.read(ub, w * h), sim.mem.read(vb, w * h)
        px, py = rng.randrange(0, w - 10), rng.randrange(0, h - 10)
        dx, dy = rng.randrange(8), rng.randrange(8)
        if rng.random() < 0.2:
            dx = dy = 0
        args = (ub, vb, w, 8 * px + dx, 8 * py + dy, w, h)
        if r.guard(lambda: T.call(sim, r, f, *args), f) is None:
            continue

        def pred(P, y, x):
            a, b = P[(py + y) * w + px + x], P[(py + y) * w + px + x + 1]
            c, d = P[(py + y + 1) * w + px + x], P[(py + y + 1) * w + px + x + 1]
            return ((8 - dx) * (8 - dy) * a + dx * (8 - dy) * b + (8 - dx) * dy * c +
                    dx * dy * d + 32) >> 6
        good = all(_get(sim.vrf, 16 + y, 0, 8) == [pred(U, y, x) for x in range(8)] and
                   _get(sim.vrf, 24 + y, 0, 8) == [pred(V, y, x) for x in range(8)]
                   for y in range(rows))
        r.check(good, '%s px %d py %d dx %d dy %d' % (f, px, py, dx, dy))


def t_alpha_blt(ctx):
    """slideshow alpha_blt_block(dst, src1, src2, alpha, n): per byte,
    dst = (src2 * a >> 8) + (src1 * (255 - a) >> 8), in 256-byte blocks
    (8 rows of 16 halfwords; each byte of a halfword uses the matching
    alpha byte)."""
    r = T.Row(ctx, 'alpha_blt_block (VPU)', 'slideshow', 'per-byte alpha blend')
    sim, rng = _sim(ctx, 'slideshow'), ctx.rng
    for _ in range(30):
        _rand_vrf(sim, rng)
        n = 256 * rng.randrange(1, 4)
        d, s1, s2, al = (_plane(sim, rng, n, 1, 32) for _ in range(4))
        A, B1, B2 = sim.mem.read(al, n), sim.mem.read(s1, n), sim.mem.read(s2, n)
        if r.guard(lambda: T.call(sim, r, 'alpha_blt_block', d, s1, s2, al, n),
                   'call') is None:
            continue
        want = bytes(((B2[j] * A[j]) >> 8) + ((B1[j] * (255 - A[j])) >> 8) for j in range(n))
        r.check(sim.mem.read(d, n) == want, 'n %d' % n)


def t_interp16x16_v(ctx):
    """vc_interp16x16(ref, stride, x4, y4, width, height) with dx = 0:
    G (full pel, vldp8/vldq8 at any alignment), d, h, n (H.264 8.4.2.2.1:
    vertical half-pel h and the quarter-pel averages (G + h + 1) >> 1,
    (M + h + 1) >> 1 via vmulhrs #0x8000); output H8 rows 0-15 at x 0."""
    r = T.Row(ctx, 'vc_interp16x16 dx=0 (VPU)', LIB, 'H.264 8.4.2.2.1 G d h n')
    sim, rng = _sim(ctx), ctx.rng
    w, h = 96, 48
    for _ in range(80):
        _rand_vrf(sim, rng)
        ref = _plane(sim, rng, w, h)
        P = sim.mem.read(ref, w * h)
        px, py = rng.randrange(0, w - 16), rng.randrange(2, h - 21)
        dy = rng.randrange(4)
        if r.guard(lambda: T.call(sim, r, 'vc_interp16x16', ref, w, 4 * px, 4 * py + dy,
                                  w, h), 'dy %d' % dy) is None:
            continue

        def half(y, x):
            return clip1((six_tap([P[(py + y + k - 2) * w + px + x] for k in range(6)]) + 16)
                         >> 5)
        want = []
        for y in range(16):
            row = []
            for x in range(16):
                g, m = P[(py + y) * w + px + x], P[(py + y + 1) * w + px + x]
                row.append([g, (g + half(y, x) + 1) >> 1, half(y, x),
                            (m + half(y, x) + 1) >> 1][dy])
            want.append(row)
        r.check(all(_get(sim.vrf, y, 0, 16) == want[y] for y in range(16)),
                'px %d py %d dy %d' % (px, py, dy))


def t_copy8x4(ctx):
    """vc_copy8x4(off, mask): copy the prediction rows selected by the
    lane mask: bits 0-15 pick lanes of chroma rows 16-17 -> 48-49 and
    24-25 -> 56-57 (+off), bits 16-31 lanes of luma rows 0-3 -> 32-35
    (+2*off).  Reference: that plain meaning (a masked copy; there is no
    spec equation), so it checks predicated REP moves and +r."""
    r = T.Row(ctx, 'vc_copy8x4 (VPU)', LIB, 'masked row copy')
    sim, rng = _sim(ctx), ctx.rng
    for _ in range(60):
        _rand_vrf(sim, rng)
        off = rng.choice([0, 4, 8, 64 * 4, 64 * 2 + 8])
        mask = rng.getrandbits(32)
        before = bytes(sim.vrf.b)
        if r.guard(lambda: T.call(sim, r, 'vc_copy8x4', off, mask), 'call') is None:
            continue
        want = bytearray(before)

        def cp(src_y, dst_y, o, bits):
            for i in range(16):
                if bits >> i & 1:
                    dy, dx = dst_y + (o >> 6), (i + o) & 63
                    want[(dy & 63) * 64 + dx] = before[src_y * 64 + i]
        for j in range(2):
            cp(16 + j, 48 + j, off, mask & 0xffff)
            cp(24 + j, 56 + j, off, mask & 0xffff)
        for j in range(4):
            cp(j, 32 + j, 2 * off, mask >> 16)
        r.check(bytes(sim.vrf.b) == bytes(want), 'off %d mask %08x' % (off, mask))


TESTS += [t_copy8x4, t_interp16x16_v, t_clearnzcoeffs, t_intra4x4, t_intra16x16, t_combinepred, t_storeback,
          t_interp_chroma, t_alpha_blt]


# --------------------------------------------------------------------------
# P19: the 32-bit accumulate (hw5[15], 'x15') and vmulhd (op 18).

def _jrng(ctx):
    """A private generator, so these rows leave ctx.rng (and so every
    other row's inputs) as they were."""
    import random
    return random.Random(ctx.rng.getstate()[1][0] ^ 0x18)


def _jref_pixels(rng, h, w):
    """Reference pixels: uniform, or high-contrast 0/255 runs, which push
    j far below 0 and above 255 (Clip1 and the 32-bit sum both matter)."""
    k = rng.random()
    if k < 0.5:
        return [[rng.getrandbits(8) for _ in range(w)] for _ in range(h)]
    if k < 0.8:
        return [[rng.choice((0, 255)) for _ in range(w)] for _ in range(h)]
    lo, hi = rng.choice(((0, 255), (255, 0)))
    return [[hi if (x // 2 + y // 2) % 2 else lo for x in range(w)] for y in range(h)]


def _j1(P, y, x):
    """H.264 8.4.2.2.1 (8-241): j1 from the intermediate b1 rows."""
    b1 = [six_tap(P[y + k][x:x + 6]) for k in range(6)]
    return six_tap(b1)


def t_interp_jhoz(ctx):
    """vc_interp16xN_jhoz(960): rows 0-20, columns 32.. hold the reference
    from (x-2, y-2); output the centre half-pel j = Clip1((j1 + 512) >> 10),
    H8 rows 0-15 at column 0.  j1 needs 21 bits: the kernel splits each
    product into vmul ... x15 (low half) and vmulhd ... acc (high half)."""
    r = T.Row(ctx, 'vc_interp16xN_jhoz (VPU, x15)', LIB, 'H.264 8.4.2.2.1 j')
    sim, rng = _sim(ctx), _jrng(ctx)
    for _ in range(60):
        _rand_vrf(sim, rng)
        P = _jref_pixels(rng, 21, 21)
        for y in range(21):
            _put(sim.vrf, y, 32, P[y])
        if r.guard(lambda: T.call(sim, r, 'vc_interp16xN_jhoz', 960), 'call') is None:
            continue
        good = all(_get(sim.vrf, y, 0, 16) ==
                   [clip1((_j1(P, y, x) + 512) >> 10) for x in range(16)]
                   for y in range(16))
        r.check(good, 'case')


def t_interp_jvert(ctx):
    """vc_interp16xN_jvert(2 or 3, -, 960): the same reference; j1 >> 10
    rounded, unclipped, as H16 rows 5-20 at column 0, and the vertical
    half-pel (h at x, m at x + 1: r0 = 2, 3) as (Σ + 16), unshifted, H16
    rows 5-20 at column 32: its caller reads them there for the
    quarter-pel averages i and k."""
    r = T.Row(ctx, 'vc_interp16xN_jvert (VPU, x15)', LIB, 'H.264 8.4.2.2.1 j, h/m')
    sim, rng = _sim(ctx), _jrng(ctx)
    for n in range(60):
        _rand_vrf(sim, rng)
        P = _jref_pixels(rng, 21, 22)
        for y in range(21):
            _put(sim.vrf, y, 32, P[y])
        off = 2 + (n & 1)
        if r.guard(lambda: T.call(sim, r, 'vc_interp16xN_jvert', off, 0, 960),
                   'call') is None:
            continue
        good = True
        for y in range(16):
            got_j = [w16(v) for v in sim.vrf.read((5 + y) * 64, False, 2)]
            got_h = [w16(v) for v in sim.vrf.read((5 + y) * 64 + 32, False, 2)]
            good &= got_j == [(_j1(P, y, x) + 512) >> 10 for x in range(16)]
            good &= got_h == [six_tap([P[y + k][x + off] for k in range(6)]) + 16
                              for x in range(16)]
        r.check(good, 'r0 %d' % off)


TESTS += [t_interp_jhoz, t_interp_jvert]
