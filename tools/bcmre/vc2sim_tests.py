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
"""Self-test for vc2sim: run Apple's VideoCore II library functions in
the simulator and compare them with independent Python models.

The reference models below are written from public specifications
(ISO C library semantics, FIPS-197, H.264 9.1 Exp-Golomb codes, the
H.263/MPEG-4 CBPY table, Park-Miller) or from the plain meaning of a
function (a ring buffer, a bit reader), never by transcribing the
disassembly.  Inputs are randomised (seeded) plus edge cases.  Every
call also checks the calling convention: r6-r23 are filled with random
values and must come back unchanged, and sp must be restored.

Usage: vc2sim.py --selftest CORPUS_DIR [-v] [--seed N] [--only a,b]
                                       [--cover] [--controls]
  --cover     also list every executed instruction kind with its count
  --controls  flip each semantic hypothesis (vc2sim.HYP) and report what fails
"""
import os
import random
import struct
import sys
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vc2dis as D  # noqa: E402
import vc2sim as S  # noqa: E402

MASK = 0xffffffff


class Ctx:
    def __init__(self, lib, vmcs, rng, verbose):
        self.lib, self.vmcs, self.rng, self.verbose = lib, vmcs, rng, verbose
        self.rows = []

    def vll(self, name, **kw):
        return S.make_sim(os.path.join(self.lib, name + '.vll'), **kw)

    def vmcs_sim(self, **kw):
        if not self.vmcs:
            raise Skip('vmcs.bin not found')
        return S.make_sim(self.vmcs, **kw)


class Skip(Exception):
    pass


class Row:
    """One line of the result table."""
    def __init__(self, ctx, target, lib, note=''):
        self.ctx, self.target, self.lib, self.note = ctx, target, lib, note
        self.n = self.ok = 0
        self.fails = []
        self.instr = 0
        ctx.rows.append(self)

    def check(self, good, detail):
        self.n += 1
        if good:
            self.ok += 1
        elif len(self.fails) < 5:
            self.fails.append(detail)
        return good

    def guard(self, fn, detail):
        """Run fn(); an exception is a failed case."""
        try:
            return fn()
        except S.SimError as e:
            self.check(False, '%s: %s' % (detail, str(e).splitlines()[0]))
            if self.ctx.verbose:
                print(e)
            return None


def call(sim, row, func, *args, **kw):
    kw.setdefault('check_abi', True)
    before = sim.steps
    v = sim.call(func, *args, **kw)
    row.instr += sim.steps - before
    return v


# --------------------------------------------------------------------------
# Bitstream helpers (our own encoders).

def ue_bits(v):
    m = (v + 1).bit_length() - 1
    return [0] * m + [int(c) for c in format(v + 1, 'b')]


def se_bits(v):
    return ue_bits(2 * v - 1 if v > 0 else -2 * v)


def rand_ue(rng):
    k = rng.choice([0, 0, 0, 1, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 15])
    return rng.randrange((1 << k) - 1, (1 << (k + 1)) - 1)


def h264_table(sim, bits, pad=64):
    """H.264 decoder layout: entry i (2 bytes at base + 2i) holds the 16
    bits starting at bit i, first bit in bit 15."""
    bits = list(bits) + [0] * (pad + 16)
    n = len(bits) - 16
    base = sim.malloc(2 * n + 64, 4)
    out = bytearray()
    v = 0
    for b in bits[:16]:
        v = (v << 1) | b
    for i in range(n):
        out += struct.pack('<H', v & 0xffff)
        v = ((v << 1) | bits[i + 16]) & 0xffff
    sim.mem.write(base, bytes(out))
    return base


def mpeg4_table(sim, bits, pad=64):
    """MPEG-4 decoder layout: entry i holds the 16 bits ending at bit i
    (bit i-1 in bit 0); 16 zero bits precede the stream."""
    bits = [0] * 16 + list(bits) + [0] * pad
    base = sim.malloc(2 * len(bits) + 64, 4)
    out = bytearray()
    v = 0
    for i in range(len(bits)):
        out += struct.pack('<H', v & 0xffff)      # bits [i-16, i) of padded
        v = ((v << 1) | bits[i]) & 0xffff
    sim.mem.write(base, bytes(out))
    return base + 32                               # entry for stream bit 0


def bits_val(bits):
    v = 0
    for b in bits:
        v = (v << 1) | b
    return v


# --------------------------------------------------------------------------
# vmcs.bin: C library helpers (exported by name)

def t_vmcs_math(ctx):
    sim = ctx.vmcs_sim()
    rng = ctx.rng
    edges = [0, 1, 2, 3, 7, 0x7fffffff, 0x80000000, 0xfffffffe, MASK]

    row = Row(ctx, '__modu (a % b, unsigned)', 'vmcs')
    cases = [(a, b) for a in edges for b in edges if b] + \
        [(rng.getrandbits(32), rng.getrandbits(rng.randrange(1, 33)) or 1)
         for _ in range(1500)]
    for a, b in cases:
        v = row.guard(lambda: call(sim, row, '__modu', a, b), '%x %% %x' % (a, b))
        if v is not None:
            row.check(v == a % b, '%x %% %x = %x, got %x' % (a, b, a % b, v))

    row = Row(ctx, '__rem (a % b, signed)', 'vmcs')
    for a, b in cases:
        sa, sb = S.s32(a), S.s32(b)
        if sa == -0x80000000 and sb == -1:
            continue                                   # C overflow
        q = abs(sa) // abs(sb)
        q = q if (sa < 0) == (sb < 0) else -q
        want = (sa - q * sb) & MASK
        v = row.guard(lambda: call(sim, row, '__rem', a, b), '%d %% %d' % (sa, sb))
        if v is not None:
            row.check(v == want, '%d %% %d = %d, got %d' % (sa, sb, S.s32(want), S.s32(v)))

    row = Row(ctx, '__lmul (64x64 -> 64)', 'vmcs')
    for _ in range(1000):
        a, b = rng.getrandbits(64), rng.getrandbits(rng.choice([8, 32, 64]))
        want = (a * b) & ((1 << 64) - 1)
        v = row.guard(lambda: call(sim, row, '__lmul', a & MASK, a >> 32, b & MASK, b >> 32),
                      'lmul')
        if v is not None:
            got = v | sim.r[1] << 32
            row.check(got == want, '%x * %x = %x, got %x' % (a, b, want, got))

    row = Row(ctx, '__ldivs (64/64 signed)', 'vmcs',
              'covers the unexported unsigned 64-bit divide it calls')
    for _ in range(600):
        a = rng.getrandbits(rng.choice([16, 40, 63, 64]))
        b = rng.getrandbits(rng.choice([4, 16, 33, 50, 64])) or 3
        sa = a - (1 << 64) if a >> 63 else a
        sb = b - (1 << 64) if b >> 63 else b
        if sa == -(1 << 63) and sb == -1:
            continue
        q = abs(sa) // abs(sb)
        q = (q if (sa < 0) == (sb < 0) else -q) & ((1 << 64) - 1)
        v = row.guard(lambda: call(sim, row, '__ldivs', a & MASK, a >> 32,
                                   b & MASK, b >> 32), 'ldivs')
        if v is not None:
            got = v | sim.r[1] << 32
            row.check(got == q, '%d / %d = %d, got %x' % (sa, sb, q, got))

    row = Row(ctx, 'rand (Park-Miller, seed at gp+0x298)', 'vmcs')
    gp = sim.vmcs.gp
    for seed in [1, 2, 12345, 0x7ffffffe, rng.randrange(1, 0x7fffffff)]:
        sim.mem.w32(gp + 0x298, seed)
        x = seed
        for _ in range(200):
            x = x * 16807 % 0x7fffffff
            v = row.guard(lambda: call(sim, row, 'rand'), 'rand')
            if v is None:
                break
            row.check(v == x, 'seed %d: want %d got %d' % (seed, x, v))


def _cstr(sim, s):
    a = sim.malloc(len(s) + 1 + 8, 4) + 0
    sim.mem.write(a, s + b'\0')
    return a


def _rand_bytes(rng, n, alphabet=None):
    alphabet = alphabet or bytes(range(1, 256))
    return bytes(rng.choice(alphabet) for _ in range(n))


def t_vmcs_string(ctx):
    sim = ctx.vmcs_sim()
    rng = ctx.rng
    mem = sim.mem
    letters = b'abcAB\xe9 z'

    def fresh(n, align=None):
        a = sim.malloc(n + 16, 4)
        return a + (rng.randrange(4) if align is None else align)

    row = Row(ctx, 'memcpy', 'vmcs')
    for _ in range(300):
        n = rng.choice([0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 31, 64, rng.randrange(300)])
        src, dst = fresh(n), fresh(n)
        data = _rand_bytes(rng, n)
        mem.write(src, data)
        guard = mem.read(dst + n, 4)
        v = row.guard(lambda: call(sim, row, 'memcpy', dst, src, n), 'memcpy')
        if v is not None:
            row.check(mem.read(dst, n) == data and v == dst and
                      mem.read(dst + n, 4) == guard, 'memcpy n=%d' % n)

    row = Row(ctx, 'memmove (overlapping)', 'vmcs')
    for _ in range(300):
        n = rng.randrange(0, 200)
        base = fresh(400, 0)
        buf = _rand_bytes(rng, 400)
        mem.write(base, buf)
        s, d = rng.randrange(0, 200), rng.randrange(0, 200)
        want = bytearray(buf)
        want[d:d + n] = buf[s:s + n]
        v = row.guard(lambda: call(sim, row, 'memmove', base + d, base + s, n), 'memmove')
        if v is not None:
            row.check(mem.read(base, 400) == bytes(want) and v == base + d,
                      'memmove d=%d s=%d n=%d' % (d, s, n))

    # vclib_memcpy's companion at +0x160 (no export): dst > src copies
    # backward, its byte tails are ldb/stb (--rs) (isa.md, pre-decrement).
    f = sim.sym('vclib_memcpy') + 0x160
    if (mem.r16(f), mem.r16(f + 2), mem.r16(f + 0x28)) == (0x0800, 0x4404, 0x7415):
        row = Row(ctx, 'vclib memmove, backward (ldb/stb (--rs))', 'vmcs')
        for _ in range(200):
            n = rng.randrange(0, 300)
            base = fresh(900, 0)
            buf = _rand_bytes(rng, 900)
            mem.write(base, buf)
            s = rng.randrange(0, 300)
            d = s + rng.randrange(1, 300)
            want = bytearray(buf)
            want[d:d + n] = buf[s:s + n]
            v = row.guard(lambda: call(sim, row, f, base + d, base + s, n),
                          'vclib memmove')
            if v is not None:
                row.check(mem.read(base, 900) == bytes(want),
                          'memmove d=%d s=%d n=%d' % (d, s, n))

    row = Row(ctx, 'memset', 'vmcs')
    for _ in range(300):
        n = rng.choice([0, 1, 3, 4, 7, 8, 9, 33, rng.randrange(300)])
        d = fresh(n)
        mem.write(d - 4, b'\x55' * (n + 8))
        c = rng.getrandbits(32)
        v = row.guard(lambda: call(sim, row, 'memset', d, c, n), 'memset')
        if v is not None:
            row.check(mem.read(d - 4, n + 8) == b'\x55' * 4 + bytes([c & 0xff]) * n +
                      b'\x55' * 4 and v == d, 'memset n=%d c=%x' % (n, c))

    row = Row(ctx, 'memchr (unexported, 0xdf22)', 'vmcs',
              'identified by these tests (called by strlen/strchr)')
    row2 = Row(ctx, 'memcmp (unexported, 0xdf3a)', 'vmcs',
               'returns -1/0/1; identified by these tests (called by strncmp)')
    for _ in range(300):
        n = rng.randrange(0, 40)
        data = _rand_bytes(rng, n, b'abcd\0\xff')
        a = _cstr(sim, data)
        c = rng.choice(b'abcdz\0\xff') | rng.choice([0, 0x100, 0xffffff00])
        k = data.find(bytes([c & 0xff]))
        v = row.guard(lambda: call(sim, row, 0xdf22, a, c, n), 'memchr')
        if v is not None:
            row.check(v == (a + k if k >= 0 else 0), 'memchr %r %x' % (data, c))
        other = bytearray(data)
        if n and rng.random() < 0.7:
            i = rng.randrange(n)
            other[i] = rng.randrange(256)
        b = _cstr(sim, bytes(other))
        want = (data > bytes(other)) - (data < bytes(other))
        v = row2.guard(lambda: call(sim, row2, 0xdf3a, a, b, n), 'memcmp')
        if v is not None:
            row2.check(S.s32(v) == want, 'memcmp %r %r' % (data, bytes(other)))

    def strs():
        for _ in range(250):
            x = _rand_bytes(rng, rng.randrange(0, 12), letters)
            y = x if rng.random() < 0.3 else _rand_bytes(rng, rng.randrange(0, 12), letters)
            if rng.random() < 0.3 and x:
                y = x[:rng.randrange(len(x))]
            yield x, y

    sign = lambda v: (v > 0) - (v < 0)
    fold = lambda s: bytes(c + 32 if 65 <= c <= 90 else c for c in s)
    for name, fn, nargs in (
            ('strcmp', lambda x, y, n: sign((x > y) - (x < y)), 2),
            ('strncmp', lambda x, y, n: sign((x[:n] > y[:n]) - (x[:n] < y[:n])), 3),
            ('_stricmp', lambda x, y, n: sign((fold(x) > fold(y)) - (fold(x) < fold(y))), 2),
            ('_strnicmp', lambda x, y, n: sign((fold(x)[:n] > fold(y)[:n]) -
                                               (fold(x)[:n] < fold(y)[:n])), 3)):
        row = Row(ctx, name, 'vmcs', 'sign of the result')
        for x, y in strs():
            n = rng.randrange(0, 14)
            a, b = _cstr(sim, x), _cstr(sim, y)
            args = (a, b, n)[:nargs]
            v = row.guard(lambda: call(sim, row, name, *args), name)
            if v is not None:
                row.check(sign(S.s32(v)) == fn(x, y, n), '%s(%r, %r, %d) = %d' % (
                    name, x, y, n, S.s32(v)))

    row = Row(ctx, 'strlen / strchr / strrchr / strstr', 'vmcs')
    for x, y in strs():
        a = _cstr(sim, x)
        v = row.guard(lambda: call(sim, row, 'strlen', a), 'strlen')
        if v is not None:
            row.check(v == len(x), 'strlen %r' % x)
        c = rng.choice(b'aAz\0')
        for name, k in (('strchr', x.find(bytes([c])) if c else len(x)),
                        ('strrchr', x.rfind(bytes([c])) if c else len(x))):
            v = row.guard(lambda: call(sim, row, name, a, c), name)
            if v is not None:
                row.check(v == (a + k if k >= 0 else 0), '%s %r %r' % (name, x, chr(c)))
        b = _cstr(sim, y)
        k = x.find(y)
        v = row.guard(lambda: call(sim, row, 'strstr', a, b), 'strstr')
        if v is not None:
            row.check(v == (a + k if k >= 0 else 0), 'strstr %r %r' % (x, y))

    row = Row(ctx, 'strcpy / strncpy / strcat', 'vmcs')
    for x, y in strs():
        d = fresh(64, 0)
        mem.write(d, b'\xaa' * 64)
        a = _cstr(sim, x)
        v = row.guard(lambda: call(sim, row, 'strcpy', d, a), 'strcpy')
        if v is not None:
            row.check(mem.read(d, len(x) + 2) == x + b'\0\xaa' and v == d, 'strcpy %r' % x)
        b = _cstr(sim, y)
        v = row.guard(lambda: call(sim, row, 'strcat', d, b), 'strcat')
        if v is not None:
            row.check(mem.cstr(d) == x + y and v == d, 'strcat %r %r' % (x, y))
        n = rng.randrange(0, 16)
        mem.write(d, b'\xaa' * 64)
        want = (x[:n] + b'\0' * n)[:n]
        v = row.guard(lambda: call(sim, row, 'strncpy', d, a, n), 'strncpy')
        if v is not None:
            row.check(mem.read(d, n + 1) == want + b'\xaa' and v == d,
                      'strncpy %r %d -> %r' % (x, n, mem.read(d, n + 1)))

    row = Row(ctx, 'strtoul', 'vmcs')
    endp = fresh(4, 0)
    for _ in range(300):
        base = rng.choice([0, 10, 16, 8, 2, 36])
        v0 = rng.getrandbits(rng.choice([4, 16, 31, 32]))
        if base == 0:
            text = rng.choice([str(v0), hex(v0), '0%o' % v0 if v0 else '0'])
        elif base == 16:
            text = rng.choice(['%x' % v0, '0x%X' % v0])
        elif base == 8:
            text = '%o' % v0
        elif base == 2:
            text = format(v0, 'b')
        elif base == 36:
            digits, t, q = '0123456789abcdefghijklmnopqrstuvwxyz', '', v0
            while True:
                t = digits[q % 36] + t
                q //= 36
                if not q:
                    break
            text = t
        else:
            text = str(v0)
        text = rng.choice(['', ' ', '  ']) + text + rng.choice(['', '!', ' 1'])
        a = _cstr(sim, text.encode())
        v = row.guard(lambda: call(sim, row, 'strtoul', a, endp, base), 'strtoul')
        if v is not None:
            stripped = text.lstrip()
            num = stripped.split(' ')[0].rstrip('!')
            consumed = len(text) - len(stripped) + len(num)
            row.check(v == v0 and mem.r32(endp) == a + consumed,
                      'strtoul(%r, %d) = %d, end +%d' % (text, base, v, mem.r32(endp) - a))


def t_vmcs_qsort_sprintf(ctx):
    sim = ctx.vmcs_sim()
    rng = ctx.rng
    mem = sim.mem
    # comparator in VideoCore II code, assembled with vc2dis
    cmp_at = sim.malloc(64, 4)
    a = cmp_at
    for line in ('ld r0, (r0)', 'ld r1, (r1)', 'sub r0, r1', 'rts'):
        ws = D.assemble(line, a)
        for w in ws:
            mem.w16(a, w)
            a += 2
    row = Row(ctx, 'qsort (VC II comparator via bl rN)', 'vmcs')
    for _ in range(60):
        n = rng.choice([0, 1, 2, 3, 5, 8, 17, 50, 100])
        vals = [rng.randrange(-10**6, 10**6) for _ in range(n)]
        arr = sim.malloc(4 * n + 4, 4)
        for k, x in enumerate(vals):
            mem.w32(arr + 4 * k, x & MASK)
        v = row.guard(lambda: call(sim, row, 'qsort', arr, n, 4, cmp_at,
                                   max_steps=2_000_000), 'qsort n=%d' % n)
        if v is not None or n == 0:
            got = [S.s32(mem.r32(arr + 4 * k)) for k in range(n)]
            row.check(got == sorted(vals), 'qsort %r -> %r' % (vals[:6], got[:6]))

    row = Row(ctx, 'sprintf (%d %u %x %X %s %c %%, widths, >6 args)', 'vmcs',
              'arguments past r5 passed on the stack')
    buf = sim.malloc(256, 4)
    for _ in range(150):
        spec = []
        args = []
        for _k in range(rng.randrange(1, 7)):
            kind = rng.choice('duxXsc%')
            flags = rng.choice(['', '', '-', '0']) if kind in 'duxX' else ''
            width = rng.choice(['', '', '5', '8', '12'])
            if kind == 'd':
                x = rng.randrange(-2**31, 2**31)
                args.append(x & MASK)
                py = ('%' + flags + width + 'd') % x
            elif kind in 'uxX':
                x = rng.getrandbits(32)
                args.append(x)
                py = ('%' + flags + width + ('d' if kind == 'u' else kind)) % x
            elif kind == 's':
                s = _rand_bytes(rng, rng.randrange(0, 8), b'abcXYZ ').decode()
                args.append(_cstr(sim, s.encode()))
                py = ('%' + width + 's') % s
            elif kind == 'c':
                c = rng.choice(b'aZ!')
                args.append(c)
                py, width, flags = chr(c), '', ''
            else:
                py, width, flags = '%', '', ''
            spec.append(('%' + flags + width + kind, py))
        fmt = '|'.join(s for s, _ in spec)
        want = '|'.join(p for _, p in spec)
        f = _cstr(sim, fmt.encode())
        mem.write(buf, b'\xaa' * 200)
        v = row.guard(lambda: call(sim, row, 'sprintf', buf, f, *args,
                                   max_steps=2_000_000), 'sprintf %r' % fmt)
        if v is not None:
            got = mem.cstr(buf).decode('latin-1')
            row.check(got == want and S.s32(v) == len(want),
                      'sprintf(%r) = %r (%d), want %r' % (fmt, got, S.s32(v), want))


# --------------------------------------------------------------------------
# h264dec.vll

def t_h264(ctx):
    sim = ctx.vll('h264dec')
    rng = ctx.rng

    row = Row(ctx, 'CeilLog2 (= bit length; caller folds +1)', 'h264dec')
    for x in [0, 1, 2, 3, 4, 255, 256, 0x7fffffff, 0x80000000, MASK] + \
            [rng.getrandbits(rng.randrange(1, 33)) for _ in range(400)]:
        v = row.guard(lambda: call(sim, row, 'CeilLog2', x), 'CeilLog2')
        if v is not None:
            row.check(v == x.bit_length(), 'CeilLog2(%d) = %d' % (x, v))

    def reader(name, gen, model, nargs=0, extra=None):
        """gen() -> (bits for the item, arg, expected value, consumed)."""
        row = Row(ctx, name, 'h264dec', extra or '')
        for _ in range(400):
            pre = [rng.getrandbits(1) for _ in range(rng.randrange(0, 20))]
            item, arg, want, used = gen()
            post = [rng.getrandbits(1) for _ in range(40)]
            base = h264_table(sim, pre + item + post)
            start = base + 2 * len(pre)
            args = (arg,) if nargs else ()
            v = row.guard(lambda: call(sim, row, name.split()[0], *args,
                                       regs={16: start}), name)
            if v is None:
                continue
            adv = (sim.r[16] - start) // 2
            row.check(model(v) == want and adv == used,
                      '%s bits=%s arg=%r: got %r adv %d, want %r adv %d' % (
                          name, ''.join(map(str, item[:24])), arg, model(v), adv,
                          want, used))
        return row

    def g_ue():
        v = rand_ue(rng)
        b = ue_bits(v)
        return b, None, v, len(b)

    def g_se():
        k = rand_ue(rng)
        v = (k + 1) // 2 if k % 2 else -(k // 2)
        b = se_bits(v)
        return b, None, v, len(b)

    reader('h264_readue', g_ue, lambda v: v)
    reader('h264_readse', g_se, S.s32)
    reader('h264_read1bit (returns !bit: te() with range 1)',
           lambda: (lambda b: ([b], None, 1 - b, 1))(rng.getrandbits(1)), lambda v: v)

    def g_bits(lo, hi, advance):
        def g():
            n = rng.randint(lo, hi)
            b = [rng.getrandbits(1) for _ in range(n)]
            return b, n, bits_val(b), n if advance else 0
        return g
    reader('h264_readbits (peek n<=16)', g_bits(0, 16, False), lambda v: v, 1)
    reader('h264_readbits_advance', g_bits(0, 16, True), lambda v: v, 1)
    reader('h264_readbits_more_advance (n<=16)', g_bits(1, 16, True), lambda v: v, 1)
    reader('h264_advancebits', lambda: (lambda n: ([0] * n, n, None, n))(
        rng.randrange(0, 40)), lambda v: None, 1)

    def g_ueb():
        v = rng.randrange(0, 255)
        b = ue_bits(v)
        return b, None, v, len(b)
    reader('h264_readue_byte (codeNum < 255)', g_ueb, lambda v: v)

    def g_te():
        rng_ = rng.choice([0, 1, 1, 2, 5, 31])
        if rng_ == 0:
            return [], 0, 0, 0
        if rng_ == 1:
            b = rng.getrandbits(1)
            return [b], 1, 1 - b, 1
        v = rng.randrange(0, rng_ + 1)
        b = ue_bits(v)
        return b, rng_, v, len(b)
    reader('h264_readte', g_te, lambda v: v, 1)

    def g_mv():
        a, b = [rng.choice([0, 0, 1, -1, 2, -3, rng.randrange(-300, 300)]) for _ in '12']
        bits = se_bits(a) + se_bits(b)
        return bits, None, ((a & 0xffff) | (b << 16)) & MASK, len(bits)
    reader('h264_readmvpair (table path + readse fallback)', g_mv, lambda v: v)

    # predresidual: leaf helper with r4 in/out; reads the 3-bit
    # rem_intra4x4_pred_mode that follows the (unconsumed) flag bit
    row = Row(ctx, 'predresidual (rem_intra4x4_pred_mode, r4 in/out)', 'h264dec')
    for _ in range(300):
        pred, rem = rng.randrange(0, 9), rng.randrange(0, 8)
        flag = [0]
        base = h264_table(sim, flag + [int(c) for c in format(rem, '03b')] +
                          [rng.getrandbits(1) for _ in range(20)])
        v = row.guard(lambda: call(sim, row, 'predresidual', regs={16: base, 4: pred}),
                      'predresidual')
        if v is None:
            continue
        want = rem if rem < pred else rem + 1
        row.check(sim.r[4] == want and sim.r[16] == base + 6,
                  'pred %d rem %d -> %d (want %d)' % (pred, rem, sim.r[4], want))

    row = Row(ctx, 'Bitstream_Set (struct fields, r16 = window table + 32)', 'h264dec')
    buf = sim.r[16]
    for _ in range(50):
        s = sim.malloc(64, 4)
        a1, a2 = rng.getrandbits(32), rng.getrandbits(32)
        v = row.guard(lambda: call(sim, row, 'Bitstream_Set', s, a1, a2,
                                   regs={16: 0}), 'Bitstream_Set')
        if v is None:
            continue
        f = [sim.mem.r32(s + 4 * k) for k in range(8)]
        buf = f[0]
        row.check(f[1:] == [buf, buf, a2, 0, 0, 0, a1] and sim.r[16] == buf + 32 and
                  buf == 0x10e660, 'fields %s r16 %x' % (['%x' % x for x in f], sim.r[16]))


# --------------------------------------------------------------------------
# mpg4dec.vll: bit reader with r20 as cursor and r19 as refill limit

H263_CBPY_INTRA = ['0011', '00101', '00100', '1001', '00011', '0111', '000010', '1011',
                   '00010', '000011', '0101', '1010', '0100', '1000', '0110', '11']


def t_mpeg4(ctx):
    sim = ctx.vll('mpg4dec')
    rng = ctx.rng
    hi = 0x1f00000                       # r19: refill limit far away

    for name, adv_fn in (('vc_ShowBits', lambda n: 0), ('vc_GetBits', lambda n: n),
                         ('vc_FlushBits', lambda n: n)):
        row = Row(ctx, name + ' (r20 cursor, 2 bytes/bit)', 'mpg4dec')
        for _ in range(300):
            pre = [rng.getrandbits(1) for _ in range(rng.randrange(0, 40))]
            n = rng.randrange(0, 25 if name != 'vc_FlushBits' else 40)
            item = [rng.getrandbits(1) for _ in range(n)]
            base = mpeg4_table(sim, pre + item + [rng.getrandbits(1) for _ in range(40)])
            start = base + 2 * len(pre)
            v = row.guard(lambda: call(sim, row, name, n, regs={20: start, 19: hi}), name)
            if v is None:
                continue
            ok = (sim.r[20] - start) // 2 == adv_fn(n)
            if name != 'vc_FlushBits':
                ok = ok and v == bits_val(item)
            row.check(ok, '%s(%d) bits %s -> %x' % (name, n, ''.join(map(str, item)), v))

    for name in ('H263GetCBPY', 'MPEG4GetCBPY'):
        row = Row(ctx, name + ' (H.263 Table 13 / MPEG-4 B-8)', 'mpg4dec',
                  'returns the inter CBPY (15 - intra index)')
        for _ in range(200):
            idx = rng.randrange(16)
            code = [int(c) for c in H263_CBPY_INTRA[idx]]
            base = mpeg4_table(sim, code + [rng.getrandbits(1) for _ in range(30)])
            v = row.guard(lambda: call(sim, row, name, regs={20: base, 19: hi}), name)
            if v is None:
                continue
            row.check(v == 15 - idx and sim.r[20] == base + 2 * len(code),
                      '%s code %s -> %d adv %d' % (name, H263_CBPY_INTRA[idx], S.s32(v),
                                                   (sim.r[20] - base) // 2))


# --------------------------------------------------------------------------
# passthruhandler.vll: AES key schedule (FIPS-197)

def gf_mul(a, b):
    p = 0
    while b:
        if b & 1:
            p ^= a
        a = ((a << 1) ^ (0x11b if a & 0x80 else 0)) & 0x1ff
        b >>= 1
    return p & 0xff


def aes_sbox():
    inv = [0] * 256
    for x in range(1, 256):
        for y in range(1, 256):
            if gf_mul(x, y) == 1:
                inv[x] = y
                break
    sbox = []
    for x in range(256):
        b = inv[x]
        s = 0x63
        for k in range(5):
            s ^= ((b << k) | (b >> (8 - k))) & 0xff
        sbox.append(s)
    return sbox


def aes_expand(key, sbox):
    nk = len(key) // 4
    nr = nk + 6
    w = [list(key[4 * i:4 * i + 4]) for i in range(nk)]
    rcon = 1
    for i in range(nk, 4 * (nr + 1)):
        t = list(w[i - 1])
        if i % nk == 0:
            t = [sbox[b] for b in t[1:] + t[:1]]
            t[0] ^= rcon
            rcon = gf_mul(rcon, 2)
        elif nk > 6 and i % nk == 4:
            t = [sbox[b] for b in t]
        w.append([a ^ b for a, b in zip(w[i - nk], t)])
    return bytes(sum(w, [])), nr


FIPS197_A = [   # (key, last round-key word) from FIPS-197 Appendix A
    ('2b7e151628aed2a6abf7158809cf4f3c', 'b6630ca6'),
    ('8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b', '01002202'),
    ('603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4', '706c631e'),
]


def t_aes(ctx):
    sim = ctx.vll('passthruhandler')
    rng = ctx.rng
    sbox = aes_sbox()
    mem = sim.mem

    row = Row(ctx, 'aes_sbox/aes_sbox_inv/aes_rcon tables vs computed', 'passthru',
              'data check of the loader')
    tb = mem.read(sim.sym('aes_sbox'), 256)
    ti = mem.read(sim.sym('aes_sbox_inv'), 256)
    row.check(list(tb) == sbox, 'sbox')
    row.check(all(ti[sbox[x]] == x for x in range(256)), 'inverse sbox')
    rc, r = [0], 1                      # rcon[0] unused (indexed by i / Nk)
    for _ in range(10):
        rc.append(r)
        r = gf_mul(r, 2)
    got = [mem.r32(sim.sym('aes_rcon') + 4 * k) for k in range(11)]
    row.check(got == rc, 'rcon %s' % got)

    row = Row(ctx, 'aes_rotword / aes_subword', 'passthru')
    p = sim.malloc(8, 4)
    for _ in range(300):
        w = rng.getrandbits(32)
        mem.w32(p, w)
        if row.guard(lambda: call(sim, row, 'aes_rotword', p), 'rotword') is not None or True:
            b = list(w.to_bytes(4, 'little'))
            row.check(mem.read(p, 4) == bytes(b[1:] + b[:1]), 'rotword %08x' % w)
        mem.w32(p, w)
        row.guard(lambda: call(sim, row, 'aes_subword', p), 'subword')
        row.check(mem.read(p, 4) == bytes(sbox[x] for x in w.to_bytes(4, 'little')),
                  'subword %08x' % w)

    row = Row(ctx, 'aes_init (key expansion, 128/192/256)', 'passthru',
              'FIPS-197 App. A vectors + random keys; Nr and mode stored per context')
    sched = 0x100be0
    for i, (k, last) in enumerate(FIPS197_A + [(None, None)] * 60):
        if k is None:
            key = bytes(rng.getrandbits(8) for _ in range(rng.choice([16, 24, 32])))
            last = None
        else:
            key = bytes.fromhex(k)
        ref, nr = aes_expand(key, sbox)
        if last:
            row.check(ref[-4:].hex() == last, 'reference model vs FIPS-197')
        ctxi, mode = rng.randrange(2), rng.choice([1, 2])
        kp = _cstr(sim, key)
        mem.write(sched + 256 * ctxi, b'\0' * 256)
        v = row.guard(lambda: call(sim, row, 'aes_init', kp, 8 * len(key), mode, ctxi),
                      'aes_init')
        if v is None:
            continue
        got = mem.read(sched + 256 * ctxi, len(ref))
        row.check(v == 1 and got == ref and mem.r32(0x100b8c + 4 * ctxi) == nr and
                  mem.r32(0x100b84 + 4 * ctxi) == mode,
                  'aes_init %d-bit ctx %d: ret %d' % (8 * len(key), ctxi, v))
    for bits, mode in ((64, 1), (128, 3), (100, 1), (256, 0)):
        kp = _cstr(sim, bytes(32))
        v = row.guard(lambda: call(sim, row, 'aes_init', kp, bits, mode, 0), 'aes_init')
        if v is not None:
            row.check(v == 0, 'aes_init rejects bits=%d mode=%d' % (bits, mode))


# --------------------------------------------------------------------------
# mplayer.vll: ring buffer (struct {start, end, wr, rd}) and codec_string

class Fifo:
    def __init__(self, n):
        self.n, self.rd, self.wr = n, 0, 0
        self.buf = bytearray(n)

    def used(self):
        return (self.wr - self.rd) % self.n

    def space(self):
        return (self.rd - self.wr - 1) % self.n


def t_fifo(ctx):
    sim = ctx.vll('mplayer')
    rng = ctx.rng
    mem = sim.mem
    row = Row(ctx, 'mp_fifo_* (init/empty/bytes/space/size/write/peek/poke/ptr/metric/clear)',
              'mplayer', 'random operation sequences vs a ring-buffer model')
    for _ in range(40):
        n = rng.choice([8, 16, 33, 100, 256])
        buf = sim.malloc(n + 8, 4)
        f = sim.malloc(16, 4)
        m = Fifo(n)
        if row.guard(lambda: call(sim, row, 'mp_fifo_init', f, buf, n), 'init') is None:
            continue
        row.check([mem.r32(f + 4 * k) for k in range(4)] == [buf, buf + n, buf, buf], 'init')
        for _step in range(40):
            op = rng.choice(['write', 'read', 'state', 'ptr', 'metric', 'poke', 'clear'])
            ok = True
            if op == 'write':
                k = rng.randrange(0, m.space() + 1)
                data = _rand_bytes(rng, k)
                src = _cstr(sim, data)
                row.guard(lambda: call(sim, row, 'mp_fifo_write', f, src, k), 'write')
                for i, b in enumerate(data):
                    m.buf[(m.wr + i) % n] = b
                m.wr = (m.wr + k) % n
            elif op == 'read':
                k = rng.randrange(0, m.used() + 1)
                dst = sim.malloc(k + 4, 4)
                v = row.guard(lambda: call(sim, row, 'mp_fifo_peek', f, buf + m.rd, dst, k),
                              'peek')
                want = bytes(m.buf[(m.rd + i) % n] for i in range(k))
                m.rd = (m.rd + k) % n
                ok = mem.read(dst, k) == want and v == buf + m.rd
                mem.w32(f + 12, buf + m.rd)        # advance read pointer
            elif op == 'poke':
                k = rng.randrange(0, m.space() + 1)
                data = _rand_bytes(rng, k)
                src = _cstr(sim, data)
                v = row.guard(lambda: call(sim, row, 'mp_fifo_poke', f, buf + m.wr, src, k),
                              'poke')
                for i, b in enumerate(data):
                    m.buf[(m.wr + i) % n] = b
                ok = v == buf + (m.wr + k) % n     # poke doesn't move wr
            elif op == 'ptr':
                p = rng.randrange(-n + 1, 2 * n - 1)
                v = row.guard(lambda: call(sim, row, 'mp_fifo_ptr', f, buf + p), 'ptr')
                ok = v == buf + p % n
            elif op == 'metric':
                p = rng.randrange(n)
                v = row.guard(lambda: call(sim, row, 'mp_fifo_metric', f, buf + p), 'metric')
                ok = v == (p - m.rd) % n
            elif op == 'clear':
                p = rng.randrange(n)
                row.guard(lambda: call(sim, row, 'mp_fifo_clear', f, buf + p), 'clear')
                m.rd = m.wr = p
            if op == 'state' or rng.random() < 0.3:
                vals = [row.guard(lambda: call(sim, row, fn, f), fn) for fn in
                        ('mp_fifo_empty', 'mp_fifo_bytes', 'mp_fifo_space', 'mp_fifo_size')]
                ok = ok and vals == [int(m.used() == 0), m.used(), m.space(), n - 1]
            ok = ok and mem.read(buf, n) == bytes(m.buf) and \
                mem.r32(f + 8) == buf + m.wr and mem.r32(f + 12) == buf + m.rd
            if not row.check(ok, 'fifo n=%d after %s' % (n, op)):
                break

    row = Row(ctx, 'codec_string (fourcc -> text, 2dxx lea sp)', 'mplayer')
    d = sim.malloc(16, 4)
    for _ in range(200):
        bs = [rng.choice([0, 0x20, 0x41, 0x7a, rng.randrange(1, 256)]) for _ in range(4)]
        fourcc = bs[0] << 24 | bs[1] << 16 | bs[2] << 8 | bs[3]
        mem.write(d, b'\xaa' * 8)
        row.guard(lambda: call(sim, row, 'codec_string', d, fourcc), 'codec_string')
        row.check(mem.cstr(d) == bytes(b for b in bs if b), 'fourcc %08x -> %r' % (
            fourcc, mem.cstr(d)))


# --------------------------------------------------------------------------
# aacdec.vll: byte-pointer bit reader {ptr, bitpos, start}

def t_aac(ctx):
    sim = ctx.vll('aacdec')
    rng = ctx.rng
    mem = sim.mem
    row = Row(ctx, 'aac_initbits / aac_get_processed_bits / aac_byte_align', 'aacdec')
    for _ in range(200):
        bs = sim.malloc(12, 4)
        buf = sim.malloc(64, 4)
        row.guard(lambda: call(sim, row, 'aac_initbits', bs, buf), 'initbits')
        row.check([mem.r32(bs + 4 * k) for k in range(3)] == [buf, 0, buf], 'initbits')
        p, b = rng.randrange(0, 40), rng.randrange(0, 8)
        mem.w32(bs, buf + p)
        mem.w32(bs + 4, b)
        v = row.guard(lambda: call(sim, row, 'aac_get_processed_bits', bs), 'bits')
        row.check(v == 8 * p + b, 'processed bits')
        v = row.guard(lambda: call(sim, row, 'aac_byte_align', bs), 'align')
        skipped = (8 - b) % 8
        row.check(v == skipped and mem.r32(bs) == buf + p + (1 if b else 0) and
                  mem.r32(bs + 4) == 0, 'byte_align b=%d -> %r' % (b, v))


def t_adts(ctx):
    """ADTS header (ISO/IEC 13818-7 / 14496-3) after the sync word."""
    sim = ctx.vll('aacdec')
    rng = ctx.rng
    mem = sim.mem
    row = Row(ctx, 'adts_frame (fixed + variable header, CRC via ldsb)', 'aacdec',
                   'struct offsets read from the stores; MPEG-2 (ID=1) profile is '
                   'stored as profile+1')
    fields = [('id', 1, 2, 'b'), ('layer', 2, 3, 'b'), ('prot_absent', 1, 4, 'b'),
              ('profile', 2, 5, 'b'), ('sf_index', 4, 6, 'b'), ('private', 1, 7, 'b'),
              ('channels', 3, 8, 'b'), ('original', 1, 9, 'b'), ('home', 1, 10, 'b'),
              ('copyright_bit', 1, 16, 'b'), ('copyright_start', 1, 17, 'b'),
              ('frame_length', 13, 18, 'h'), ('fullness', 11, 20, 'h'),
              ('raw_blocks', 2, 22, 'b')]
    for _ in range(300):
        vals = {k: rng.getrandbits(n) for k, n, _o, _w in fields}
        bits = []
        for k, n, _o, _w in fields:
            bits += [int(c) for c in format(vals[k], '0%db' % n)]
        crc = rng.getrandbits(16)
        if not vals['prot_absent']:
            bits += [int(c) for c in format(crc, '016b')]
        lead = rng.randrange(8)
        bits = [rng.getrandbits(1) for _ in range(lead)] + bits + \
            [rng.getrandbits(1) for _ in range(40)]
        data = bytes(bits_val(bits[i:i + 8] + [0] * (8 - len(bits[i:i + 8])))
                     for i in range(0, len(bits), 8))
        buf = _cstr(sim, data)
        bs = sim.malloc(12, 4)
        mem.write(bs, struct.pack('<III', buf, lead, buf))
        hdr = sim.malloc(32, 4)
        mem.write(hdr, b'\xaa' * 32)
        v = row.guard(lambda: call(sim, row, 'adts_frame', hdr, bs), 'adts_frame')
        if v is None:
            continue
        ok = v == 0
        for k, n, o, w in fields:
            want = vals[k] + (1 if k == 'profile' and vals['id'] else 0)
            got = mem.r8(hdr + o) if w == 'b' else mem.r16(hdr + o)
            ok = ok and got == want & (0xff if w == 'b' else 0xffff)
        used = 44 + (0 if vals['prot_absent'] else 16)     # 16 fixed + 28 variable
        pos = 8 * (mem.r32(bs) - buf) + mem.r32(bs + 4)
        ok = ok and pos == lead + used
        if not vals['prot_absent']:
            ok = ok and mem.r16(hdr + 24) == crc
        row.check(ok, 'adts %r crc %04x' % (vals, crc))


def t_mvpred(ctx):
    """H.264 8.4.1.3 motion vector prediction (median, single-ref rule,
    C replaced by D, B/C copied from A)."""
    sim = ctx.vll('h264dec')
    rng = ctx.rng
    mem = sim.mem
    row = Row(ctx, 'calcmvpredictor (H.264 8.4.1.3 median MV prediction)', 'h264dec',
              'mask bits A=1 B=2 C=4 D=8; 3 stack arguments; signed min/max')
    row8 = Row(ctx, 'calcmvpredictor, only D available (mask 8)', 'h264dec',
               'Apple bug: "mov r1, r4" after r4 was reloaded with refIdx, so the '
               'MV at index refIdx is returned; modelled as such')
    tabs = [sim.malloc(16, 4) for _ in range(3)]
    out = sim.malloc(8, 4)
    for _ in range(600):
        mask = rng.randrange(16)
        ref = rng.randrange(3)
        mv = {n: (rng.choice([rng.randrange(-2048, 2048), rng.randrange(-4, 4)]),
                  rng.randrange(-512, 512), rng.randrange(3)) for n in 'ABCD'}
        for t in tabs:
            mem.write(t, b'\0' * 16)          # index 0: zero sentinel
        for k, n in enumerate('ABCD'):
            for t, v in zip(tabs, mv[n]):
                mem.w16(t + 2 * (k + 1), v & 0xffff)
        mem.write(out, struct.pack('<hhh', 0x1111, 0x2222, ref))
        av = {n: bool(mask >> k & 1) for k, n in enumerate('ABCD')}
        # model
        if not av['C']:
            av['C'], mv['C'] = av['D'], mv['D']
        nb = {n: mv[n] if av[n] else (0, 0, -1) for n in 'ABC'}
        if not av['B'] and not av['C'] and av['A']:
            nb['B'] = nb['C'] = nb['A']
        same = [n for n in 'ABC' if nb[n][2] == ref]
        if len(same) == 1:
            want = nb[same[0]][:2]
        else:
            want = tuple(sorted(nb[n][c] for n in 'ABC')[1] for c in (0, 1))
        r = row if mask != 8 else row8
        if mask == 8 and mv['D'][2] == ref:
            want = mv['ABCD'[ref - 1]][:2] if ref else (0, 0)   # index = refIdx
        v = r.guard(lambda: call(sim, r, 'calcmvpredictor', out, 1, 2, 3, 4, mask,
                                 stack_args=tabs), 'calcmvpredictor')
        if v is None:
            continue
        got = struct.unpack('<hh', mem.read(out, 4))
        r.check(got == want, 'mask %x ref %d %r -> %r want %r' % (mask, ref, mv, got, want))


def t_predec_exceptions(ctx):
    """Our own unit rows (no corpus): (--rs) pre-decrement and the
    optional synchronous exception model (vc2sim Sim.exceptions).  Not
    counted in --cover: the coverage list is what Apple's code executes."""
    cover, S.COVER = S.COVER, None
    try:
        _predec_exceptions(ctx)
    finally:
        S.COVER = cover
        S.HYP['exc_pc_offset'] = 0


def _predec_exceptions(ctx):
    code = 0x10000

    def load(sim, lines, at=code):
        a = at
        for ln in lines:
            ws = D.assemble(ln, a)
            for w in ws:
                sim.mem.w16(a, w)
                a += 2
        return a

    row = Row(ctx, '(--rs) pre-decrement, every width', 'unit', 'rs -= size, then access')
    for op, size in (('ld', 4), ('ldh', 2), ('ldb', 1), ('ldsh', 2), ('ldsb', 1)):
        sim = S.Sim()
        load(sim, ['%s r2, (--r3)' % op, 'rts'])
        sim.mem.write(0x20000, bytes(range(0x80, 0x90)))
        v = row.guard(lambda: sim.call(code, regs={3: 0x20008}), op)
        b = sim.mem.read(0x20008 - size, size)
        want = int.from_bytes(b, 'little')
        if op in ('ldsh', 'ldsb'):
            want = S.sx(want, 8 * size) & MASK
        row.check(v is not None and sim.r[2] == want and sim.r[3] == 0x20008 - size,
                  '%s: r2 %08x r3 %08x' % (op, sim.r[2], sim.r[3]))
    for op, size in (('st', 4), ('sth', 2), ('stb', 1)):
        sim = S.Sim()
        load(sim, ['%s r2, (--r3)' % op, 'rts'])
        v = row.guard(lambda: sim.call(code, regs={2: 0x11223344, 3: 0x20008}), op)
        got = sim.mem.read(0x20000, 8)
        want = bytes(8 - size) + (0x11223344).to_bytes(4, 'little')[:size]
        row.check(v is not None and got == want and sim.r[3] == 0x20008 - size,
                  '%s: mem %s r3 %08x' % (op, got.hex(), sim.r[3]))

    row = Row(ctx, 'synchronous exception model', 'unit',
              'off: SimError; on: frame sr/pc, ei cleared, vector; misaligned '
              'load deferred')
    # (name, body, regs, expected writes, instructions run after the fault)
    faults = [('misaligned', ['ld r0, (--r5)'], {5: 0x2007f},
               {0: 0x44332211, 5: 0x2007b}, 2),
              ('misaligned', ['ld r0, (r5)'], {5: 0x2007d}, {0: 0x88776655}, 4),
              ('bus', ['ld r0, (r5)'], {5: S.MEM_SIZE + 0x100}, {}, 0),
              ('undef', [None], {}, {}, 0)]
    for name, body, regs, writes, after in faults:
        for off in (0, 6):
            for on in (False, True):
                sim = S.Sim()
                sim.exceptions = on
                S.HYP['exc_pc_offset'] = off
                vec = S.HYP['exc_vec_' + name]
                handler = 0x18000
                sim.mem.w32(4 * vec, handler)
                sim.mem.w32(0x20078, 0x44332211)
                sim.mem.w32(0x2007c, 0x88776655)
                # r7: what the faulting load left in r0
                load(sim, ['mov r7, r0', 'mov r2, sr', 'ld r0, (sp+4)', 'ld r1, (sp+0)',
                           'add sp, #8', 'rts'], handler)
                # the handler's r0 is the saved pc: keep the loaded r0 in r7
                a = load(sim, ['mov r4, #7'])
                fpc = a
                if body[0] is None:
                    sim.mem.w16(a, 0x3080)          # undecodable halfword
                    a += 2
                else:
                    a = load(sim, body, a)
                a = load(sim, ['add r6, #1'] * 6, a)
                load(sim, ['rts'], a)
                sr = 1 << 30 | 0x5
                try:
                    sim.call(code, regs={**regs, 0: 0x5A5A, 6: 0, 30: sr})
                    err = None
                except S.SimError as e:
                    err = e
                if not on:
                    row.check(err is not None and getattr(err, 'hyp', '') ==
                              'exc_vec_' + name, '%s off: %r' % (name, err))
                    continue
                size = 2 if body[0] is None else len(D.assemble(body[0], fpc)) * 2
                saved = fpc + (size + 2 * after if after else 0) + off
                ok = (err is None and sim.r[0] == saved and sim.r[1] == sr and
                      not (sim.r[2] >> 30) & 1 and sim.r[4] == 7 and
                      sim.r[6] == after and sim.r[7] == writes.get(0, 0x5A5A) and
                      sim.excs and sim.excs[-1][:2] == (vec, fpc) and
                      all(sim.r[k] == writes.get(k, v) for k, v in regs.items()
                          if k != 0))
                row.check(ok, '%s %s offset %d: err %r r0 %08x r1 %08x r2 %08x r5 '
                          '%08x r6 %d' % (name, body, off, err, sim.r[0], sim.r[1],
                                          sim.r[2], sim.r[5], sim.r[6]))
    S.HYP['exc_pc_offset'] = 0

    # The device's probe v2 checks 9 and 12 (device sessions 2 and 3,
    # 2026-10-10), replayed with the same instructions and memory: the
    # silicon's P2 lines are the expected values.
    row = Row(ctx, 'misaligned load as on silicon (probe v2 checks 9, 12)', 'unit',
              'device sessions 2-3: P2R 9 r0 338 r5 7b, pc +6; P2R 12 r0 0badf00d, '
              'adds 4, pc +18')
    cases = [
        # check 9: ld r0, (--r5) with r5 = 0x7f; word at 0x78 = 0x338
        ('9', ['mov r0, #5', 'mov r5, #127'], 0x7050, ['nop'] * 4,
         {0x78: 0x338}, {0: 0x338, 5: 0x7B}, 6),
        # check 12: ld r0, (r1), r1 = probe_buf + 1, then four adds
        ('12', ['mov r1, #0x1855', 'mov r0, #0', 'mov r2, #0', 'mov r3, #0',
                'mov r4, #0', 'mov r5, #0'], 0xB810,
         ['add r2, r2, #273', 'add r3, r3, #546', 'add r4, r4, #819',
          'add r5, r5, #1092', 'mov r2, #0xcb2bc'],
         {0x1854: 0x0BADF00D},
         {0: 0x0BADF00D, 1: 0x1855, 2: 0x111, 3: 0x222, 4: 0x333, 5: 0x444}, 18)]
    for tag, pre, insn, post, mem, want, delta in cases:
        sim = S.Sim()
        sim.exceptions = True
        handler = 0x18000
        sim.mem.w32(4 * S.HYP['exc_vec_misaligned'], handler)
        for k, v in mem.items():
            sim.mem.w32(k, v)
        load(sim, ['ld r6, (sp+4)', 'add sp, #8', 'rts'], handler)
        a = load(sim, pre)
        fpc = a
        sim.mem.w16(a, insn)
        a = load(sim, post, a + 2)
        load(sim, ['rts'], a)
        err = row.guard(lambda: sim.call(code) is None or True, 'check ' + tag)
        got = {k: sim.r[k] for k in want}
        row.check(err is not None and got == want and sim.r[6] == fpc + delta and
                  sim.excs and sim.excs[-1][:2] == (1, fpc),
                  'check %s: regs %s saved pc +%d excs %s' % (
                      tag, {k: '%x' % v for k, v in got.items()},
                      sim.r[6] - fpc, sim.excs))


TESTS = [t_vmcs_math, t_vmcs_string, t_vmcs_qsort_sprintf, t_h264, t_mpeg4, t_aes,
         t_fifo, t_aac, t_adts, t_mvpred, t_predec_exceptions]

import vc2vpu_tests  # noqa: E402  (vector-unit experiments; uses Row/call above)

TESTS += vc2vpu_tests.TESTS

import vc2boot_tests  # noqa: E402  (modelled bus: vmcs.bin start-up and LCD)

TESTS += vc2boot_tests.TESTS


def run_suite(lib, vmcs, seed, only=None, verbose=False, quiet=False):
    ctx = Ctx(lib, vmcs, random.Random(seed), verbose)
    for t in TESTS:
        if only and not any(o in t.__name__ for o in only):
            continue
        try:
            t(ctx)
        except Skip as e:
            if not quiet:
                print('skip %s: %s' % (t.__name__, e))
        except Exception:
            if not quiet:
                traceback.print_exc()
            Row(ctx, t.__name__, '-', 'test crashed').check(False, 'crash')
    return ctx.rows


def report(rows, seed):
    w = max(len(r.target) for r in rows)
    print('%-*s %-9s %6s %6s %9s  %s' % (w, 'function', 'library', 'cases', 'pass',
                                         'insns', 'notes'))
    bad = 0
    for r in rows:
        print('%-*s %-9s %6d %6d %9d  %s' % (w, r.target, r.lib, r.n, r.ok, r.instr,
                                             r.note))
        for f in r.fails:
            print('    FAIL', f)
        bad += r.n - r.ok
    tot = sum(r.n for r in rows)
    print('\n%d rows, %d cases, %d failed, %d instructions simulated (seed %d)' % (
        len(rows), tot, bad, sum(r.instr for r in rows), seed))
    return bad


def controls(lib, vmcs, seed, only):
    """Flip each semantic hypothesis in turn and count what breaks."""
    print('%-26s %-8s %8s %7s  %s' % ('hypothesis', 'flipped', 'failed', 'rows', 'rows that fail'))
    for k, v in list(S.HYP.items()):
        if k.startswith('exc_'):
            continue                 # exception model: off in the self-test
        alts = [a for a in S.HYP_ALTERNATIVES.get(k, [not v]) if a != v]
        for alt in alts:
            S.HYP[k] = alt
            rows = run_suite(lib, vmcs, seed, only, quiet=True)
            S.HYP[k] = v
            bad = [r for r in rows if r.ok < r.n]
            print('%-26s %-8s %8d %7d  %s' % (k, alt, sum(r.n - r.ok for r in rows),
                                              len(bad), '; '.join(r.target.split(' (')[0]
                                                                  for r in bad)))


def main(argv):
    verbose = '-v' in argv
    cover = '--cover' in argv
    ctl = '--controls' in argv
    argv = [a for a in argv if a not in ('-v', '--cover', '--controls')]
    seed, only = 2722, None
    if '--seed' in argv:
        k = argv.index('--seed')
        seed = int(argv[k + 1])
        del argv[k:k + 2]
    if '--only' in argv:
        k = argv.index('--only')
        only = argv[k + 1].split(',')
        del argv[k:k + 2]
    if not argv:
        print(__doc__)
        return 2
    lib, vmcs = S.find_corpus(argv[0])
    if ctl:
        controls(lib, vmcs, seed, only)
        return 0
    if cover:
        import collections
        S.COVER = collections.Counter()
    import time
    t0 = time.time()
    rows = run_suite(lib, vmcs, seed, only, verbose)
    bad = report(rows, seed)
    print('%.1f s' % (time.time() - t0))
    if cover:
        print('\nexecuted instruction kinds (count):')
        for k, n in sorted(S.COVER.items()):
            print('  %-28s %9d' % (k, n))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
