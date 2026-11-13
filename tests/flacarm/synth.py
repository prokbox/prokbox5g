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

"""A FLAC writer that makes every decoder path happen on purpose.

    synth.py OUT.flac BITS BLOCKSIZE FRAMES SEED [var]

var: a variable-blocksize stream, frame f of BLOCKSIZE + f % 12 samples,
so that every count of outputs left over by a loop that makes two, three
or four a pass turns up; its full-precision LPC subframes cycle orders
2, 3, 4 and 20 on a smooth signal.

The reference encoder chooses its own predictors, so a stream of its output
covers what it happens to like.  This writes the frames itself, cycling
through what the decoder must handle: LPC orders 1..32 at coefficient
precisions from 5 to 15 bits and every shift 0..15 (so both of
decode_subframe_lpc's filters, and the qlevel-0 case of the 64-bit one);
fixed orders 0..4; verbatim and constant subframes; wasted bits; all four
channel assignments; Rice and Rice2 codings with partition orders 0..8;
escaped partitions, with the zero-width escape on silent partitions.

The signal is a bounded, music-like waveform (it never leaves BITS), and
each residual is computed from it with the decoder's own arithmetic, so a
correct decoder gives the waveform back; flac -d is the judge.
"""
import math
import random
import sys


class Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, v, n):
        if n == 0:
            return
        self.acc = (self.acc << n) | (v & ((1 << n) - 1))
        self.n += n
        while self.n >= 8:
            self.n -= 8
            self.out.append((self.acc >> self.n) & 0xff)
        self.acc &= (1 << self.n) - 1

    def unary(self, q):  # q zeros, then a one
        while q >= 32:
            self.put(0, 32)
            q -= 32
        self.put(1, q + 1)

    def align(self):
        if self.n:
            self.put(0, 8 - self.n)


def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xff if c & 0x80 else (c << 1) & 0xff
    return c


def crc16(data):
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xffff if c & 0x8000 else (c << 1) & 0xffff
    return c


def utf8(v):
    if v < 0x80:
        return bytes([v])
    out = []
    n = 1
    while v >> (6 * n) >= (1 << (6 - n)):
        n += 1
    for i in range(n):
        out.insert(0, 0x80 | (v & 0x3f))
        v >>= 6
    return bytes([((0xff00 >> (n + 1)) & 0xff) | v] + out)


def levinson(x, order):
    n = len(x)
    r = [sum(x[i] * x[i - k] for i in range(k, n)) for k in range(order + 1)]
    r[0] = r[0] * 1.0001 + 1e-9
    a = [0.0] * (order + 1)
    err = r[0]
    for i in range(1, order + 1):
        acc = r[i] - sum(a[j] * r[i - j] for j in range(1, i))
        k = acc / err
        a_new = a[:]
        a_new[i] = k
        for j in range(1, i):
            a_new[j] = a[j] - k * a[i - j]
        a = a_new
        err *= max(1e-9, 1 - k * k)
    return a[1:]  # x[i] ~ sum a[j] x[i-1-j]


def rice_param(vals, limit):
    m = sum(abs(v) for v in vals) / max(1, len(vals))
    k = 0
    while (1 << (k + 1)) <= m and k < limit:
        k += 1
    return k


class Writer:
    def __init__(self, bps, bs, seed):
        self.bps, self.bs = bps, bs
        self.rnd = random.Random(seed)
        self.k = 0          # cycles through the cases
        self.lpc_k = 0
        self.orders = None  # var: the orders the full-precision LPC cycles
        self.q3 = 0

    def residual(self, b, res, order, method_hint):
        bs = self.bs
        # partition order: the largest allowed of a cycling target
        po = (self.k * 3) % 9
        while po and (bs % (1 << po) or (bs >> po) < order):
            po -= 1
        need2 = any(rice_param(res[i:i + 64], 30) > 14 for i in range(0, len(res), 64))
        method = 1 if (need2 or method_hint) else 0
        pbits, esc = (5, 31) if method else (4, 15)
        b.put(method, 2)
        b.put(po, 4)
        n = bs >> po
        i = 0
        for p in range(1 << po):
            cnt = n - (order if p == 0 else 0)
            part = res[i:i + cnt]
            i += cnt
            silent = all(v == 0 for v in part)
            if silent and (self.k + p) % 2 == 0:
                b.put(esc, pbits)       # zero-width escape
                b.put(0, 5)
                continue
            if (self.k + p) % 7 == 3 or not part:
                w = max([abs(v).bit_length() + 1 for v in part] + [1])
                b.put(esc, pbits)
                b.put(w, 5)
                for v in part:
                    b.put(v, w)
                continue
            k = rice_param(part, esc - 1)
            b.put(k, pbits)
            for v in part:
                u = (v << 1) ^ (-1 if v < 0 else 0)
                b.unary(u >> k)
                b.put(u, k)

    def subframe(self, b, x, bps, indep):
        """Write x (ints, each fits bps signed) as one subframe.  Only an
        independent channel may decode to something other than x (the
        constant and silent cases): a side channel that changed could put
        its partner out of range."""
        bs = self.bs
        self.k += 1
        kind = self.k % 11
        if kind == 0 and not indep:
            kind = 1
        wasted = 0
        if kind == 5 and bps > 4:  # wasted bits: drop the low w bits first
            wasted = 1 + self.k % 7
            x = [(v >> wasted) << wasted for v in x]
        y = [v >> wasted for v in x]
        sbps = bps - wasted
        b.put(0, 1)
        if kind == 0:   # constant
            c = y[0]
            b.put(0, 6)
            self.wasted(b, wasted)
            b.put(c, sbps)
            return [c << wasted] * bs
        if kind == 1:   # verbatim
            b.put(1, 6)
            self.wasted(b, wasted)
            for v in y:
                b.put(v, sbps)
            return x
        if kind in (2, 3, 4) or kind == 9:  # fixed, orders 0..4
            order = self.k % 5
            b.put(8 | order, 6)
            self.wasted(b, wasted)
            for v in y[:order]:
                b.put(v, sbps)
            if indep and order <= 1 and self.k % 2 == 0:
                # silence (order 0) or a held value (order 1): every
                # residual zero, the zero-width escape's case
                y = y[:order] + [y[0] if order else 0] * (bs - order)
            res = [y[i] - self.fixed_pred(y, i, order) for i in range(order, bs)]
            self.residual(b, res, order, self.k % 2)
            return [v << wasted for v in y]
        # LPC: order, precision and shift all cycle
        self.lpc_k += 1
        if self.lpc_k % 3:      # full precision, orders 1..32 in turn
            order = 1 + (self.lpc_k * 7 // 3) % 32
            if self.orders:
                order = self.orders[self.lpc_k % len(self.orders)]
            prec = 15
        else:                   # low precision: the 32-bit filter's orders
            order = 1 + (self.lpc_k // 3 * 5) % 12
            prec = 5 + (self.lpc_k * 5 // 3) % 10
        qtarget = (self.lpc_k * 3) % 16
        a = levinson([float(v) for v in y], order)
        amax = max(1e-6, max(abs(c) for c in a))
        lim = (1 << (prec - 1)) - 1
        q = qtarget
        if self.orders and prec == 15 and order == 3:
            # var: order 3's register window has a loop per shift; take
            # each in turn, clamping the coefficients where they overflow
            self.q3 += 1
            q = qtarget = self.q3 % 16
            amax = 0
        while q > 0 and round(amax * (1 << q)) > lim:
            q -= 1
        coeffs = [max(-lim - 1, min(lim, int(round(c * (1 << q))))) for c in a]
        if self.lpc_k % 3 and order >= 2:
            # keep the full-precision subframes on the 64-bit filter: the
            # decoder takes the 32-bit one when sum(|c|) < 2^(32 - sbps),
            # so add K * (y[i-order+1] - y[i-order]) to the predictor (a
            # small change on a smooth signal) until the sum reaches that
            coeffs = self.widen(coeffs, sbps, lim, y, q)
        b.put(32 | (order - 1), 6)
        self.wasted(b, wasted)
        for v in y[:order]:
            b.put(v, sbps)
        b.put(prec - 1, 4)
        b.put(q, 5)
        for c in coeffs:
            b.put(c, prec)
        res = []
        for i in range(order, bs):
            s = sum(coeffs[j] * y[i - 1 - j] for j in range(order))
            p = s >> q
            r = y[i] - p
            # the format's residual is 32-bit; the decoder wraps the same way
            r = ((r + (1 << 31)) & 0xffffffff) - (1 << 31)
            res.append(r)
        self.residual(b, res, order, self.lpc_k % 2)
        return x

    @staticmethod
    def widen(coeffs, sbps, lim, y, q):
        """coeffs with sum(|c|) >= 2^(32 - sbps), or coeffs unchanged when
        the change to the prediction could reach 2^29 (the residual must
        stay a 32-bit value)."""
        if sbps > 31:               # no 32-bit filter at this width
            return coeffs
        need = 1 << (32 - sbps)
        if sum(abs(c) for c in coeffs) >= need:
            return coeffs
        o = len(coeffs)
        # K times a first or second difference of the oldest taps: the
        # second is smaller on a smooth signal, so it allows lower shifts
        for pat in ((1, -2, 1), (1, -1)):
            m = len(pat)
            if o < m:
                continue
            d = max(abs(sum(p * y[i - o + m - 1 - j] for j, p in enumerate(pat)))
                    for i in range(o, len(y)))
            for k in range(1, 2 * lim + 2):
                if (k * d >> q) >= 1 << 29:
                    break
                for sg in (1, -1):
                    c = coeffs[:]
                    for j, p in enumerate(pat):
                        c[o - m + j] += sg * k * p
                    if max(abs(v) for v in c[o - m:]) > lim:
                        continue
                    if sum(abs(v) for v in c) >= need:
                        return c
        return coeffs

    def escape_subframe(self, b, bps, w):
        """One LPC subframe whose residuals are one escaped partition of
        exactly w bits (26..31: wider than the bit reader's 25 a read).  The
        signal alternates between the extremes and the order-1 predictor
        c = 2^(w - bps - 1) at shift 0 overshoots by (1 + c) times it (a
        15-bit coefficient: 16-bit streams stop at 30 bits)."""
        lo, hi = -(1 << (bps - 1)), (1 << (bps - 1)) - 1
        y = [lo if i % 2 else hi for i in range(self.bs)]
        if bps <= 17:
            w = min(w, 30)
        c = 1 << (w - bps - 1)
        assert 0 < c < 1 << 14
        res = [y[i] - c * y[i - 1] for i in range(1, self.bs)]
        width = max(abs(v).bit_length() + 1 for v in res)
        assert width == w and max(abs(v) for v in res) < 1 << 31, (w, width)
        b.put(0, 1)
        b.put(32 | 0, 6)                    # LPC, order 1
        self.wasted(b, 0)
        b.put(y[0], bps)
        b.put(15 - 1, 4)
        b.put(0, 5)                         # shift 0
        b.put(c, 15)
        b.put(0, 2)                         # Rice, partition order 0
        b.put(0, 4)
        b.put(15, 4)                        # escape
        b.put(w, 5)
        for v in res:
            b.put(v, w)
        return y

    def edge_subframe(self, b, bps, at):
        """One LPC subframe on the 32-bit filter's bound: every sample the
        most negative one, every coefficient negative, sum(|c|) of
        2^(32 - bps) (at) or one less.  At the bound the prediction sum is
        exactly 2^31, which the 32-bit filter cannot hold; one below, it is
        the largest sum the 32-bit filter is ever given."""
        need = 1 << (32 - bps)
        lim = 1 << 14                    # the most a 15-bit coefficient holds
        order = max(2, -(-need // lim))
        coeffs = [-(need // order)] * order
        coeffs[0] -= need - sum(-c for c in coeffs)
        if not at:
            coeffs[-1] += 1
        assert sum(-c for c in coeffs) == need - (0 if at else 1)
        assert min(coeffs) >= -lim
        q = 15
        lo = -(1 << (bps - 1))
        b.put(0, 1)
        b.put(32 | (order - 1), 6)
        self.wasted(b, 0)
        for _ in range(order):
            b.put(lo, bps)
        b.put(15 - 1, 4)
        b.put(q, 5)
        for c in coeffs:
            b.put(c, 15)
        s = sum(c * lo for c in coeffs)
        assert s == (1 << 31) - (0 if at else 1 << (bps - 1))
        res = [lo - (s >> q)] * (self.bs - order)
        self.residual(b, res, order, 1)
        return [lo] * self.bs

    @staticmethod
    def fixed_pred(y, i, order):
        if order == 0:
            return 0
        if order == 1:
            return y[i - 1]
        if order == 2:
            return 2 * y[i - 1] - y[i - 2]
        if order == 3:
            return 3 * y[i - 1] - 3 * y[i - 2] + y[i - 3]
        return 4 * y[i - 1] - 6 * y[i - 2] + 4 * y[i - 3] - y[i - 4]

    @staticmethod
    def wasted(b, w):
        if w:
            b.put(1, 1)
            b.unary(w - 1)
        else:
            b.put(0, 1)


def main():
    out, bits, bs0, frames, seed = sys.argv[1:6]
    var = sys.argv[6:7] == ["var"]
    bps, bs0, frames = int(bits), int(bs0), int(frames)
    rate = 96000
    w = Writer(bps, bs0, int(seed))
    if var:
        # a smooth, low signal (so the 64-bit filter can be forced at low
        # shifts, see widen) through the register-window orders, and 20
        w.orders = (3, 2, 3, 4, 3, 4, 20)   # 7 long: 3 | lpc_k picks low precision
    rnd = w.rnd
    full = (1 << (bps - 1)) - 1
    body = bytearray()
    t = 0
    minf, maxf = 1 << 24, 0
    for f in range(frames):
        bs = bs0 + (f % 12 if var else 0)
        w.bs = bs
        # the waveform for this frame: partials plus noise, at a level that
        # changes frame to frame (loud, moderate, near-silent)
        level = ((0.6, 0.02, 0.002, 0.02) if var else (0.9, 0.3, 0.02, 0.6))[f % 4]
        parts = [(rnd.uniform(50, 500 if var else 9000), rnd.random() / k,
                  rnd.random() * 6.3)
                 for k in range(1, 7)]
        L, R = [], []
        for i in range(bs):
            tt = (t + i) / rate
            s = sum(a * math.sin(2 * math.pi * fr * tt + ph) for fr, a, ph in parts)
            s = s / 2.5 * level
            nz = 0.00001 if var else 0.002
            l = s + rnd.gauss(0, nz)
            r = 0.8 * s + rnd.gauss(0, nz + 0.05 * (f % 3 == 1 and not var))
            L.append(max(-full - 1, min(full, int(l * full))))
            R.append(max(-full - 1, min(full, int(r * full))))
        t += bs
        mode = f % 4   # independent, left/side, right/side, mid/side
        edge = f % 8 == 7 and bps <= 29   # the 32-bit filter's bound (edge_subframe)
        wide_esc = f % 8 == 3 and not var and bps <= 24   # escapes of 26..31 bits
        if edge or wide_esc:
            mode = 0
        if mode == 0:
            chans, cbps, code = (L, R), (bps, bps), 1
        elif mode == 1:
            chans, cbps, code = (L, [a - b for a, b in zip(L, R)]), (bps, bps + 1), 8
        elif mode == 2:
            chans, cbps, code = ([a - b for a, b in zip(L, R)], R), (bps + 1, bps), 9
        else:
            chans = ([(a + b) >> 1 for a, b in zip(L, R)], [a - b for a, b in zip(L, R)])
            cbps, code = (bps, bps + 1), 10
        b = Bits()
        b.put(0xfff9 if var else 0xfff8, 16)
        b.put(7, 4)                                   # blocksize: 16 bits at end
        b.put(0, 4)                                   # rate: STREAMINFO
        b.put(code, 4)
        b.put({16: 4, 24: 6}.get(bps, 0), 3)          # other widths: STREAMINFO
        b.put(0, 1)
        for byte in utf8(t - bs if var else f):
            b.put(byte, 8)
        b.put(bs - 1, 16)
        b.put(crc8(bytes(b.out)), 8)
        for n, (ch, cb) in enumerate(zip(chans, cbps)):
            if edge:
                w.edge_subframe(b, cb, n == 1)
            elif wide_esc:
                w.escape_subframe(b, cb, 26 + (f // 8 * 2 + n) % 6)
            else:
                w.subframe(b, ch, cb, mode == 0)
        b.align()
        b.put(crc16(bytes(b.out)), 16)
        minf, maxf = min(minf, len(b.out)), max(maxf, len(b.out))
        body += b.out
    total = t
    si = Bits()
    si.put(bs0, 16)
    si.put(bs0 + (11 if var else 0), 16)
    si.put(minf, 24)
    si.put(maxf, 24)
    si.put(rate, 20)
    si.put(1, 3)        # stereo
    si.put(bps - 1, 5)
    si.put(total, 36)
    si.put(0, 64)
    si.put(0, 64)       # no MD5
    head = b"fLaC" + bytes([0x80, 0, 0, 34]) + bytes(si.out)
    open(out, "wb").write(head + body)


if __name__ == "__main__":
    main()
