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
"""End-to-end test of vc2cc: C -> VC IV GCC -> VC II machine code -> vc2sim.

Each row compiles a C snippet (our own code), runs its functions in the
VideoCore II simulator on random inputs and compares with a Python model.
Every call checks the ABI: r6-r23 must come back unchanged, except r15
and r22 (vc2cc's scratch registers).

Usage:
  vc2cc_tests.py [-v] [--seed N]
  vc2cc_tests.py --torture DIR [--limit N] [-v]
        compile and run every GCC c-torture/execute test in DIR (main()
        must return 0 or call exit(0); abort() fails)

Skips cleanly (exit 0, message) when vc4-elf-gcc is not installed.
"""
import os
import random
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vc2cc as C   # noqa: E402
import vc2sim as S  # noqa: E402

M32 = 0xffffffff
M64 = (1 << 64) - 1
EXEMPT = {15: 0, 22: 0}

SRC = r'''
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;

void *my_memcpy(void *d, const void *s, u32 n)
{
    unsigned char *dp = d; const unsigned char *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

/* dense switch with code in each case: a tbb/tbh jump table */
int sw_code(int x, int y)
{
    switch (x) {
    case 0: return y + 1;
    case 1: return y * 3;
    case 2: return y - 7;
    case 3: return y << 2;
    case 4: return y ^ 0x55;
    case 5: return -y;
    case 6: return y / 3;
    case 7: return y & 0xf0;
    case 9: return y | 1;
    default: return 1000;
    }
}

/* value switch (GCC turns it into a table in .rodata) */
int sw_val(int x)
{
    switch (x) {
    case 0: return 11; case 1: return 22; case 2: return 37;
    case 3: return 41; case 4: return 59; case 5: return 63;
    case 6: return 71; case 7: return 89; default: return -1;
    }
}

u64 mul64(u64 a, u64 b) { return a * b; }
u64 umul32(u32 a, u32 b) { return (u64)a * b; }
s64 smul32(int a, int b) { return (s64)a * b; }
s64 div64(s64 a, s64 b) { return a / b; }
u64 umod64(u64 a, u64 b) { return a % b; }
u64 shl64(u64 a, int n) { return a << n; }
s64 asr64(s64 a, int n) { return a >> n; }
int lt64(s64 a, s64 b) { return a < b; }
u64 add64(u64 a, u64 b) { return a + b; }

int many(int a, int b, int c, int d, int e, int f, int g, int h, int i)
{
    return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h + 9*i;
}
int call_many(int x, int y)
{
    return many(x, y, x ^ y, x + 1, y - 1, x * 3, y * 5, x - y, 42);
}

struct big { int v[9]; short s; signed char c; };
void scopy(struct big *d, const struct big *s) { *d = *s; }
static int sval(struct big b) { return b.v[0] + b.v[8] + b.s + b.c; }
int sval_ptr(const struct big *p) { return sval(*p); }

/* CAVLC-style bit reader: 32-bit cache, Exp-Golomb */
struct br { const unsigned char *p; u32 cache; int bits; };
static void refill(struct br *b)
{
    while (b->bits <= 24) {
        b->cache |= (u32)*b->p++ << (24 - b->bits);
        b->bits += 8;
    }
}
static u32 getbits(struct br *b, int n)
{
    u32 v;
    if (n == 0)
        return 0;
    refill(b);
    v = b->cache >> (32 - n);
    b->cache <<= n;
    b->bits -= n;
    return v;
}
static u32 read_ue(struct br *b)
{
    int lz;
    refill(b);
    lz = __builtin_clz(b->cache | 1);
    getbits(b, lz);
    return getbits(b, lz + 1) - 1;
}
static int read_se(struct br *b)
{
    u32 k = read_ue(b);
    return (k & 1) ? (int)((k + 1) >> 1) : -(int)(k >> 1);
}
/* decode n se(v) values from buf into out; return bits consumed */
int decode_se(const unsigned char *buf, int *out, int n)
{
    struct br b = { buf, 0, 0 };
    int i, start;
    refill(&b);
    start = 0;
    for (i = 0; i < n; i++)
        out[i] = read_se(&b);
    return (int)(b.p - buf) * 8 - b.bits - start;
}

/* globals in .data/.bss, strings, recursion, function pointers */
int counter = 5;
static int scratch[16];
const char *msg = "VideoCore II";
int bump(int k) { counter += k; scratch[k & 15] = counter; return counter; }
int msglen(void) { int n = 0; while (msg[n]) n++; return n; }
int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
typedef int (*cmp_fn)(int, int);
static int asc(int a, int b) { return a - b; }
static int desc(int a, int b) { return b - a; }
void isort(int *a, int n, int up)
{
    cmp_fn f = up ? asc : desc;
    int i, j;
    for (i = 1; i < n; i++) {
        int v = a[i];
        for (j = i; j > 0 && f(a[j - 1], v) > 0; j--)
            a[j] = a[j - 1];
        a[j] = v;
    }
}

/* conditional execution, sub-word types, bit ops */
int clamp(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
int sel(int c, int a, int b) { return c ? a + 1 : b - 1; }
int sub_words(const short *s, const signed char *c, const unsigned short *u,
              int i)
{
    return s[i] * 3 + c[i] + u[i];
}
u32 rotl(u32 x, int n) { return (x << (n & 31)) | (x >> ((32 - n) & 31)); }
u32 rotl7(u32 x) { return (x << 7) | (x >> 25); }
u32 bits(u32 x) { return (x | (1u << 20)) & ~(1u << 3) ^ (1u << 30); }
int sext12(u32 x) { return ((int)(x << 20)) >> 20; }
int clz(u32 x) { return __builtin_clz(x); }
int ctz(u32 x) { return __builtin_ctz(x); }
int popc(u32 x) { return __builtin_popcount(x); }
int sdiv(int a, int b) { return a / b + a % b; }
u32 udiv(u32 a, u32 b) { return a / b - a % b; }
u32 bswap(u32 x) { return __builtin_bswap32(x); }
u32 big_const(u32 x) { return (x & 0x00ff00ff) * 0x01000193u + 0x9e3779b9u; }

/* varargs and struct return */
int vsum(int n, ...)
{
    __builtin_va_list ap;
    int s = 0;
    __builtin_va_start(ap, n);
    while (n--)
        s = s * 3 + __builtin_va_arg(ap, int);
    __builtin_va_end(ap);
    return s;
}
int call_vsum(int a, int b) { return vsum(8, a, b, a + b, a - b, 1, 2, 3, a ^ b); }
struct pt { int x, y, z; };
static struct pt mk(int x, int y) { struct pt p = { x, y, x * y }; return p; }
int use_mk(int x, int y) { struct pt p = mk(x + 1, y - 1); return p.x + 2 * p.y + 3 * p.z; }

/* native VC II instruction through inline asm */
int asm_msb(u32 x) { int n; __asm__("! msb %0, %1" : "=r"(n) : "r"(x)); return n + 1; }
'''


def s32(v):
    v &= M32
    return v - (1 << 32) if v >> 31 else v


def s64(v):
    v &= M64
    return v - (1 << 64) if v >> 63 else v


def cdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


class Ctx:
    def __init__(self, seed, verbose):
        self.rng = random.Random(seed)
        self.verbose = verbose
        self.rows = []
        self.sim = S.Sim()
        with tempfile.NamedTemporaryFile('w', suffix='.c', delete=False) as f:
            f.write(SRC)
            path = f.name
        try:
            self.prog = C.build([path], extern=self.sim.import_addr)
        finally:
            os.unlink(path)
        C.load_into_sim(self.prog, self.sim)

    def call(self, fn, *args, r64=False):
        args = [a & M32 for a in args]
        r0 = self.sim.call(fn, *args, check_abi=True, regs=dict(EXEMPT))
        return (r0 | self.sim.r[1] << 32) if r64 else r0

    def row(self, name, cases):
        """cases: iterable of (got, want, label)."""
        n = bad = 0
        first = None
        for got, want, label in cases:
            n += 1
            if got != want:
                bad += 1
                first = first or '%s: got %r, want %r' % (label, got, want)
        self.rows.append((name, n, n - bad, first))


def split64(v):
    return v & M32, (v >> 32) & M32


def t_memcpy(c):
    mem = c.sim.mem

    def cases():
        for _ in range(60):
            n = c.rng.randrange(0, 80)
            src, dst = c.sim.malloc(n + 8), c.sim.malloc(n + 8)
            data = bytes(c.rng.randrange(256) for _ in range(n))
            guard = bytes(c.rng.randrange(256) for _ in range(8))
            mem.write(src, data)
            mem.write(dst + n, guard)
            r = c.call('my_memcpy', dst, src, n)
            yield (r, mem.read(dst, n), mem.read(dst + n, 8)), (dst, data, guard), 'n=%d' % n
    c.row('memcpy loop (post-increment ldb/stb, addcmpb)', cases())


def t_switch(c):
    def model(x, y):
        y = s32(y)
        return {0: y + 1, 1: y * 3, 2: y - 7, 3: y << 2, 4: y ^ 0x55, 5: -y,
                6: cdiv(y, 3), 7: y & 0xf0, 9: y | 1}.get(x, 1000)

    def cases():
        for x in list(range(-2, 12)) * 4:
            y = c.rng.randrange(-5000, 5000)
            yield s32(c.call('sw_code', x, y)), s32(model(x, y)), 'sw_code(%d, %d)' % (x, y)
        vals = {0: 11, 1: 22, 2: 37, 3: 41, 4: 59, 5: 63, 6: 71, 7: 89}
        for x in range(-3, 12):
            yield s32(c.call('sw_val', x)), vals.get(x, -1), 'sw_val(%d)' % x
    c.row('switch: tbb jump table, rodata value table', cases())


def t_64bit(c):
    r = c.rng

    def rnd64():
        return r.choice([r.getrandbits(64), r.getrandbits(33), r.getrandbits(8),
                         M64 - r.getrandbits(10)])

    def cases():
        for _ in range(40):
            a, b = rnd64(), rnd64()
            yield c.call('mul64', *split64(a), *split64(b), r64=True), a * b & M64, 'mul64'
            yield c.call('add64', *split64(a), *split64(b), r64=True), (a + b) & M64, 'add64'
            x, y = r.getrandbits(32), r.getrandbits(32)
            yield c.call('umul32', x, y, r64=True), x * y, 'umul32'
            yield s64(c.call('smul32', x, y, r64=True)), s32(x) * s32(y), 'smul32'
            if b:
                yield s64(c.call('div64', *split64(a), *split64(b), r64=True)), \
                    cdiv(s64(a), s64(b)), 'div64'
                yield c.call('umod64', *split64(a), *split64(b), r64=True), a % b, 'umod64'
            n = r.randrange(64)
            yield c.call('shl64', *split64(a), n, r64=True), (a << n) & M64, 'shl64 %d' % n
            yield s64(c.call('asr64', *split64(a), n, r64=True)), s64(a) >> n, 'asr64'
            yield c.call('lt64', *split64(a), *split64(b)), int(s64(a) < s64(b)), 'lt64'
    c.row('64-bit: mulhd, libgcc divide/shift, cmn carry', cases())


def t_args(c):
    def model(*a):
        return sum((k + 1) * s32(v) for k, v in enumerate(a))

    def cases():
        for _ in range(60):
            a = [c.rng.randrange(-10000, 10000) for _ in range(9)]
            yield s32(c.call('many', *a)), s32(model(*a)), 'many%r' % (a,)
            x, y = c.rng.randrange(-1000, 1000), c.rng.randrange(-1000, 1000)
            want = model(x, y, x ^ y, x + 1, y - 1, x * 3, y * 5, x - y, 42)
            yield s32(c.call('call_many', x, y)), s32(want), 'call_many(%d, %d)' % (x, y)
    c.row('9 arguments: 3 on the stack, both directions', cases())


def t_struct(c):
    mem = c.sim.mem

    def cases():
        for _ in range(40):
            v = [c.rng.randrange(-2**31, 2**31) for _ in range(9)]
            s, ch = c.rng.randrange(-32768, 32768), c.rng.randrange(-128, 128)
            raw = struct.pack('<9ihbx', *v, s, ch)
            src, dst = c.sim.malloc(40), c.sim.malloc(48)
            mem.write(src, raw)
            mem.write(dst + 40, b'\xa5' * 8)
            c.call('scopy', dst, src)
            yield mem.read(dst, 39) + mem.read(dst + 40, 8), raw[:39] + b'\xa5' * 8, 'scopy'
            yield s32(c.call('sval_ptr', src)), s32(v[0] + v[8] + s + ch), 'by value'
    c.row('struct copy and struct by value', cases())


def ue_bits(k):
    k1 = k + 1
    n = k1.bit_length()
    return '0' * (n - 1) + format(k1, 'b')


def t_cavlc(c):
    mem = c.sim.mem

    def cases():
        for _ in range(40):
            n = c.rng.randrange(1, 40)
            vals = [c.rng.choice([0, 1, -1, c.rng.randrange(-300, 300),
                                  c.rng.randrange(-70000, 70000)]) for _ in range(n)]
            bits = ''.join(ue_bits(2 * v - 1 if v > 0 else -2 * v) for v in vals)
            data = int(bits + '0' * (-len(bits) % 8), 2).to_bytes(
                (len(bits) + 7) // 8, 'big') + bytes(8)
            buf, out = c.sim.malloc(len(data)), c.sim.malloc(4 * n)
            mem.write(buf, data)
            used = c.call('decode_se', buf, out, n)
            got = [s32(mem.r32(out + 4 * k)) for k in range(n)]
            yield (got, s32(used)), (vals, len(bits)), 'se%r' % (vals[:5],)
    c.row('CAVLC-style bit reader: se(v) / ue(v), clz', cases())


def t_globals(c):
    mem = c.sim.mem

    def cases():
        cnt = 5
        for k in range(20):
            cnt += k
            yield s32(c.call('bump', k)), cnt, 'bump(%d)' % k
        yield c.call('msglen'), len('VideoCore II'), 'msglen'
        fibs = [0, 1]
        while len(fibs) < 20:
            fibs.append(fibs[-1] + fibs[-2])
        for n in range(15):
            yield c.call('fib', n), fibs[n], 'fib(%d)' % n
        for _ in range(20):
            n = c.rng.randrange(0, 20)
            vals = [c.rng.randrange(-1000, 1000) for _ in range(n)]
            up = c.rng.randrange(2)
            a = c.sim.malloc(4 * n + 4)
            for k, v in enumerate(vals):
                mem.w32(a + 4 * k, v & M32)
            c.call('isort', a, n, up)
            got = [s32(mem.r32(a + 4 * k)) for k in range(n)]
            yield got, sorted(vals, reverse=not up), 'isort'
    c.row('.data/.bss/strings, recursion, function pointers', cases())


def t_ops(c):
    r = c.rng
    mem = c.sim.mem

    def rot(x, n):
        n &= 31
        return ((x << n) | (x >> (32 - n))) & M32 if n else x

    def cases():
        for _ in range(60):
            x, y, z = r.getrandbits(32), r.getrandbits(32), r.getrandbits(32)
            lo, hi = sorted([s32(y) >> 8, s32(z) >> 8])
            yield s32(c.call('clamp', x >> 7, lo, hi)), min(max(s32(x >> 7), lo), hi), 'clamp'
            yield s32(c.call('sel', x & 1, y, z)), s32(s32(y) + 1 if x & 1 else s32(z) - 1), 'sel'
            n = r.randrange(32)
            yield c.call('rotl', x, n), rot(x, n), 'rotl %d' % n
            yield c.call('rotl7', x), rot(x, 7), 'rotl7'
            yield c.call('bits', x), ((x | 1 << 20) & ~8 ^ 1 << 30) & M32, 'bits'
            yield s32(c.call('sext12', x)), s32(x << 20) >> 20, 'sext12'
            if x:
                yield c.call('clz', x), 32 - x.bit_length(), 'clz'
                yield c.call('ctz', x), (x & -x).bit_length() - 1, 'ctz'
            yield c.call('popc', x), bin(x).count('1'), 'popc'
            if y:
                a, b = s32(x), s32(y >> r.randrange(32)) or 1
                yield s32(c.call('sdiv', a, b)), s32(cdiv(a, b) + (a - cdiv(a, b) * b)), 'sdiv'
                b = (y >> r.randrange(32)) or 1
                yield c.call('udiv', x, b), (x // b - x % b) & M32, 'udiv'
            yield c.call('bswap', x), int.from_bytes(x.to_bytes(4, 'little'), 'big'), 'bswap'
            yield c.call('big_const', x), ((x & 0x00ff00ff) * 0x01000193 + 0x9e3779b9) & M32, 'const'
            i = r.randrange(8)
            arr = c.sim.malloc(64)
            sh = [r.randrange(-32768, 32768) for _ in range(8)]
            sc = [r.randrange(-128, 128) for _ in range(8)]
            us = [r.randrange(65536) for _ in range(8)]
            mem.write(arr, struct.pack('<8h8b8H', *sh, *sc, *us))
            yield s32(c.call('sub_words', arr, arr + 16, arr + 24, i)), \
                sh[i] * 3 + sc[i] + us[i], 'sub_words'
    c.row('cond-exec, sub-word loads, rotates, bit ops, clz/popcount', cases())


def t_varargs(c):
    def vs(*a):
        s = 0
        for v in a:
            s = s * 3 + v
        return s

    def cases():
        for _ in range(40):
            a, b = c.rng.randrange(-1000, 1000), c.rng.randrange(-1000, 1000)
            yield s32(c.call('call_vsum', a, b)), s32(vs(a, b, a + b, a - b, 1, 2, 3, a ^ b)), 'vsum'
            k = c.rng.randrange(0, 9)
            args = [c.rng.randrange(-99, 99) for _ in range(k)]
            yield s32(c.call('vsum', k, *args)), s32(vs(*args)), 'vsum direct %d' % k
            x, y = c.rng.randrange(-300, 300), c.rng.randrange(-300, 300)
            yield s32(c.call('use_mk', x, y)), s32((x + 1) + 2 * (y - 1) + 3 * (x + 1) * (y - 1)), 'mk'
            v = c.rng.getrandbits(c.rng.randrange(1, 33))
            yield c.call('asm_msb', v), v.bit_length(), 'asm msb'
    c.row('varargs, struct return, inline VC II asm', cases())


RESIDENT = r'''
int res_scale = 3;
int res_tab[4] = { 10, 20, 30, 40 };
int res_add(int a, int b) { return a * res_scale + b; }
%s
unsigned res_abi(void) { extern char __vc2_abi[]; return (unsigned)__vc2_abi; }
'''

OVERLAY = r'''
extern int res_add(int a, int b);
extern int res_tab[4];
extern int res_scale;
extern char __vc2_abi[];
int ovl_f(int x);
static int calls;               /* the overlay's own .bss */
int ovl_div = 7;                /* ... and .data */
/* the header, at the overlay's base: its ABI word and its entry */
struct ovl_hdr { unsigned abi; int (*entry)(int); };
const struct ovl_hdr ovl_hdr __attribute__((section(".vectors"))) =
    { (unsigned)__vc2_abi, ovl_f };
int ovl_f(int x)
{
    calls++;
    res_scale = 2 + (x & 1);    /* a resident variable, written */
    /* a 64-bit divide: libgcc's __divdi3, linked into the overlay */
    return res_add(x, res_tab[x & 3]) + (int)((long long)x * 1000003 / ovl_div) +
           5 + calls;
}
int ovl_calls(void) { return calls; }
%s
'''


def build_text(text, **kw):
    with tempfile.NamedTemporaryFile('w', suffix='.c', delete=False) as f:
        f.write(text)
        path = f.name
    try:
        return C.build([path], **kw)
    finally:
        os.unlink(path)


def t_fixed_base(c):
    """A fixed-base link (an overlay): a second program at its own base,
    its calls and data references resolved into a resident image through
    an import list, the ABI word on both sides"""
    names = ['res_add', 'res_tab', 'res_scale']

    def cases():
        res = build_text(RESIDENT % '', base=0x300000, exports=(names, 7))
        imp = C.Imports(res.symbols, names, 7)
        yield imp.abi, res.abi, 'resident __vc2_abi = Imports.abi'
        ovl = build_text(OVERLAY % '', base=0x340000, imports=imp)
        yield ovl.abi, res.abi, 'overlay __vc2_abi = resident'
        yield ovl.symbols['ovl_hdr'], 0x340000, 'header at the base'
        yield ovl.base <= min(v for k, v in ovl.symbols.items()
                              if not k.startswith('__')), True, 'nothing below the base'
        yield ovl.base <= ovl.symbols.get('__divdi3', 0) < ovl.ends['text'], True, \
            'library code linked into the overlay itself'
        yield all(n not in ovl.symbols for n in names), True, 'imports not copied'
        sim = S.Sim()
        C.load_into_sim(res, sim)
        C.load_into_sim(ovl, sim)
        yield sim.mem.r32(0x340000), res.abi, 'header word in memory'
        yield sim.call('res_abi', check_abi=True, regs=dict(EXEMPT)), res.abi, 'res_abi()'
        entry = sim.mem.r32(0x340004)
        yield entry, ovl.symbols['ovl_f'], 'entry word'
        tab = [10, 20, 30, 40]
        for k in range(1, 40):
            x = c.rng.randrange(-5000, 5000)
            q = abs(x) * 1000003 // 7
            want = x * (2 + (x & 1)) + tab[x & 3] + (q if x >= 0 else -q) + 5 + k
            got = s32(sim.call(entry, x & M32, check_abi=True, regs=dict(EXEMPT)))
            yield got, want, 'ovl_f(%d) through the header' % x
            yield sim.mem.r32(res.symbols['res_scale']), 2 + (x & 1), 'resident data written'
        yield sim.call('ovl_calls', check_abi=True, regs=dict(EXEMPT)), 39, 'overlay .bss'
        # the map round trip: an overlay linked from the resident's map file
        with tempfile.NamedTemporaryFile('w', suffix='.map', delete=False) as f:
            res.write_map(f)
            mp = f.name
        try:
            syms = C.read_map(mp)
        finally:
            os.unlink(mp)
        yield {n: syms[n] for n in names}, {n: res.symbols[n] for n in names}, 'read_map'
        yield C.Imports(syms, names, 7).abi, res.abi, 'ABI from the map file'
        # refused: another version, a moved export, a name not exported,
        # one the resident lacks
        yield C.Imports(res.symbols, names, 8).abi != res.abi, True, 'version changes the ABI'
        moved = build_text(RESIDENT % 'int res_pad(int a) { return a * 77 + 1; }',
                           base=0x300000, exports=(names, 7))
        yield moved.symbols['res_add'] != res.symbols['res_add'] or \
            moved.symbols['res_tab'] != res.symbols['res_tab'], True, 'an export moved'
        yield moved.abi != res.abi, True, 'a moved export changes the ABI'
        same = build_text(RESIDENT % '', base=0x300000, exports=(names, 7))
        yield same.abi, res.abi, 'a rebuild of the same resident keeps it'
        try:
            build_text(OVERLAY % 'int g(void) { return res_abi(); }', base=0x340000,
                       imports=imp)
            yield 'linked', 'refused', 'a resident name not on the list'
        except C.Gap as e:
            yield 'res_abi' in str(e), True, 'a resident name not on the list'
        try:
            C.Imports(res.symbols, names + ['nowhere'], 7)
            yield 'made', 'refused', 'an import the resident lacks'
        except C.Gap:
            yield True, True, 'an import the resident lacks'
    c.row('fixed-base link: overlay against a resident image, ABI word', cases())


TESTS = [t_memcpy, t_switch, t_64bit, t_args, t_struct, t_cavlc, t_globals,
         t_ops, t_varargs, t_fixed_base]


# ---------------------------------------------------------------------------
# GCC c-torture/execute runner.

class Exit(Exception):
    def __init__(self, code):
        self.code = code


def torture_sim():
    sim = S.Sim(poison=False)

    def stop(code):
        raise Exit(code)
    sim.add_stub('abort', lambda s, *a: stop(1))
    sim.add_stub('exit', lambda s, code, *a: stop(s32(code)))
    for n in ('printf', 'puts', 'putchar', 'fprintf', 'fflush'):
        sim.add_stub(n, lambda s, *a: 0)

    def strlen(s, p, *a):
        return len(s.mem.cstr(p))

    def memcmp(s, a, b, n, *x):
        da, db = s.mem.read(a, n), s.mem.read(b, n)
        return (da > db) - (da < db)

    def strcmp(s, a, b, *x):
        da, db = s.mem.cstr(a), s.mem.cstr(b)
        return (da > db) - (da < db)

    def strcpy(s, d, p, *x):
        s.mem.write(d, s.mem.cstr(p) + b'\0')
        return d

    def calloc(s, n, m, *x):
        return s.malloc(n * m)

    def strchr(s, p, ch, *x):
        t = s.mem.cstr(p) + b'\0'
        k = t.find(bytes([ch & 255]))
        return p + k if k >= 0 else 0

    def strncmp(s, a, b, n, *x):
        da, db = s.mem.cstr(a)[:n], s.mem.cstr(b)[:n]
        return (da > db) - (da < db)

    def mempcpy(s, d, p, n, *x):
        s.mem.write(d, s.mem.read(p, n))
        return d + n
    for n, f in (('strlen', strlen), ('memcmp', memcmp), ('strcmp', strcmp),
                 ('strcpy', strcpy), ('calloc', calloc), ('strchr', strchr),
                 ('strncmp', strncmp), ('mempcpy', mempcpy),
                 ('abs', lambda s, v, *x: abs(s32(v))),
                 ('labs', lambda s, v, *x: abs(s32(v))),
                 ('isprint', lambda s, c, *x: int(32 <= c < 127)),
                 ('fabs', lambda s, lo, hi, *x: (lo, hi & 0x7fffffff)),
                 ('alloca', lambda s, n, *x: s.malloc(n))):
        sim.add_stub(n, f)
    return sim


def dg_options(path):
    """Extra flags a torture test asks for ({ dg-options "..." })."""
    import re
    import shlex
    with open(path, errors='replace') as f:
        m = re.search(r'\{\s*dg-options\s+"([^"]*)"', f.read())
    return shlex.split(m.group(1)) if m else []


def run_torture(d, limit=None, verbose=False):
    files = sorted(f for f in os.listdir(d) if f.endswith('.c'))[:limit]
    res = {'pass': [], 'fail': [], 'compile': [], 'float': [], 'sim': []}
    for f in files:
        p = os.path.join(d, f)
        sim = torture_sim()
        try:
            # hosted: main() returns 0 by default, builtins as the tests expect
            text = C.compile_c(p, '-O2', ['-w', '-DNO_TRAMPOLINES', '-fhosted',
                                          '-fbuiltin'] + dg_options(p))
        except RuntimeError:
            res['compile'].append(f)
            continue
        try:
            prog = C.build([(f, text)], extern=sim.import_addr)
        except C.Gap as e:
            res['float' if ' f' in str(e) else 'compile'].append(f)
            continue
        except Exception as e:  # noqa: BLE001 (report and go on)
            res['compile'].append(f)
            if verbose:
                print('%s: build: %s' % (f, e))
            continue
        C.load_into_sim(prog, sim)
        try:
            code = s32(sim.call('main', 0, 0, max_steps=50_000_000))
        except Exit as e:
            code = e.code
        except S.SimError as e:
            msg = str(e).split('\n')[0]
            res['sim'].append((f, msg))
            if verbose:
                print('%s: sim: %s' % (f, msg))
            continue
        (res['pass'] if code == 0 else res['fail']).append(f)
        if verbose and code:
            print('%s: exit %d' % (f, code))
    return res


def main(argv):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('-v', action='store_true')
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--torture')
    ap.add_argument('--limit', type=int)
    a = ap.parse_args(argv)
    if not os.path.exists(C.GCC):
        print('vc2cc_tests: SKIP (no VC IV GCC at %s; set VC4_GCC)' % C.GCC)
        return 0
    if a.torture:
        res = run_torture(a.torture, a.limit, a.v)
        n = sum(len(v) for v in res.values())
        print('torture: %d tests: %d pass, %d fail (abort/exit != 0), '
              '%d stopped in the simulator, %d hardware float, %d not built'
              % (n, len(res['pass']), len(res['fail']), len(res['sim']),
                 len(res['float']), len(res['compile'])))
        if res['fail']:
            print('  fail: ' + ' '.join(res['fail']))
        reasons = {}
        for f, m in res['sim']:
            k = m.split(':')[0][:60]
            reasons.setdefault(k, []).append(f)
        for k, fs in sorted(reasons.items(), key=lambda kv: -len(kv[1])):
            print('  sim %3d  %s  (%s)' % (len(fs), k, ' '.join(fs[:4])))
        return 0 if not res['fail'] else 1
    c = Ctx(a.seed, a.v)
    p = c.prog
    print('image 0x%x-0x%x: text %d bytes, rodata %d, data %d, bss %d' % (
        p.base, p.end, p.ends['text'] - p.base, p.ends['rodata'] - p.ends['text'],
        p.ends['data'] - p.ends['rodata'], p.ends['bss'] - p.ends['data']))
    fails = 0
    for t in TESTS:
        try:
            t(c)
        except S.SimError as e:
            c.rows.append((t.__name__, 1, 0, 'simulator: %s' % e))
    for name, n, ok, first in c.rows:
        print('%-58s %5d / %5d %s' % (name, ok, n, '' if ok == n else 'FAIL ' + first))
        fails += n - ok
    total = sum(r[1] for r in c.rows)
    print('%d rows, %d cases, %d failures; %d instructions simulated' % (
        len(c.rows), total, fails, c.sim.steps))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
