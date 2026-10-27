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
"""Table-driven disassembler for VideoCore II (BCM2722) machine code.

The encodings were recovered from Apple's .vll libraries (reference
material only; see docs/research/bcm2722/isa.md for the evidence behind
every table row and its confidence).  Instructions are 16-bit
little-endian halfwords, 1, 2, 3 or 5 halfwords long.

Usage:
  vc2dis.py FILE.vll [FUNC ...]     disassemble functions (all if none)
  vc2dis.py --check DIR             coverage/consistency report over *.vll
  vc2dis.py --table                 print the encoding table
  vc2dis.py --raw FILE.bin BASE [START [END]]   linear sweep of a raw image
  vc2dis.py --raw FILE.vll [START [END]]        same over a .vll's sections
                                                at their link addresses
  vc2dis.py --trace FILE.bin BASE ROOT...       recursive descent from ROOTs
                                                (plus push-after-return seeds)
  vc2dis.py --unknown DIR [N]       most frequent undecoded leading halfwords
  vc2dis.py --roundtrip DIR [IMG.bin BASE ROOT]...
                                    disassemble, reassemble, compare bytes
  vc2dis.py --asm [ADDR]            assemble lines from stdin to hex
"""
import collections
import glob
import os
import re
import struct
import sys

# --------------------------------------------------------------------------
# Bit fields.  A field is a tuple of (word, hi, lo) slices, concatenated
# most-significant first; word 0 is the first halfword.

def F(*parts):
    return tuple(parts)


def getf(ws, field):
    v = 0
    for w, hi, lo in field:
        n = hi - lo + 1
        v = (v << n) | ((ws[w] >> lo) & ((1 << n) - 1))
    return v


def setf(ws, field, v):
    for w, hi, lo in reversed(field):
        n = hi - lo + 1
        m = ((1 << n) - 1) << lo
        ws[w] = (ws[w] & ~m) | ((v & ((1 << n) - 1)) << lo)
        v >>= n


def width(field):
    return sum(hi - lo + 1 for _, hi, lo in field)


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class EncodeError(ValueError):
    pass


COND = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc',
        'hi', 'ls', 'ge', 'lt', 'gt', 'le', '', 'nv']

# 32 scalar ALU operations, VideoCore II numbering (NOT the VC IV order).
# Missing numbers are seen in code but not pinned yet (rendered aluN).
ALU = {
    0: 'btst', 1: 'cmn', 2: 'bmask', 3: 'bic', 4: 'asr', 5: 'eor', 6: 'lsl', 7: 'and',
    8: 'lsr', 10: 'mul', 11: 'rsub', 12: 'add', 13: 'sub', 14: 'mov',
    15: 'or', 16: 'cmp', 18: 'max', 19: 'min', 20: 'adds2', 21: 'adds4',
    22: 'adds8', 23: 'adds16', 24: 'neg', 25: 'msb', 26: 'sxtb', 27: 'sxth',
    28: 'brev', 30: 'abs', 31: 'not',
}
# Confidence of the less certain names (sim.md "Semantics corrections"):
# 30 abs medium; 26 sxtb / 27 sxth low-medium (unary sign extensions).
# Ops whose immediate is printed unsigned (shift counts, bit numbers, masks).
ALU_UIMM = {0, 2, 4, 6, 7, 8}
# Second 3-operand family (2e8x/2e9x): VC IV's "c4..c7" group order.
EXT = {
    0: 'mulhd.ss', 1: 'mulhd.su', 2: 'mulhd.us', 3: 'mulhd.uu',
    4: 'div.ss', 5: 'div.su', 6: 'div.us', 7: 'div.uu',
    8: 'adds', 9: 'subs', 10: 'shls', 11: 'clamp16',
    12: 'addscale5', 13: 'addscale6', 14: 'addscale7', 15: 'addscale8',
}
LDST = ['ld', 'st', 'ldh', 'sth', 'ldb', 'stb', 'ldsh', 'ldsb']
SYS = {2: 'nop', 3: 'sleep', 10: 'ei', 11: 'di', 18: 'bkpt', 24: 'rti'}
REGALIAS = {24: 'gp', 25: 'sp', 26: 'lr', 30: 'sr', 31: 'pc'}
REGNUM = {v: k for k, v in REGALIAS.items()}


def reg(n):
    return REGALIAS.get(n, 'r%d' % n)


def aluname(p):
    return ALU.get(p, 'alu%d' % p)


def parse_int(s):
    s = s.strip()
    neg = s.startswith('-')
    s = s.lstrip('+-')
    v = int(s, 0)
    return -v if neg else v


# --------------------------------------------------------------------------
# Operand field types.  Each renders a value from the instruction words and
# encodes it back; the assembler and the disassembler share them.

NUM = r'[-+]?(?:0x[0-9a-fA-F]+|\d+)'


class Reg:
    pat = r'r\d+|gp|sp|lr|sr|pc'

    def __init__(self, bits):
        self.bits = bits

    def show(self, i):
        return reg(getf(i.ws, self.bits))

    def put(self, ws, addr, s):
        n = REGNUM[s] if s in REGNUM else int(s[1:])
        if n >= 1 << width(self.bits):
            raise EncodeError('register %s out of range' % s)
        setf(ws, self.bits, n)


class Imm:
    """Immediate, optionally signed (signed may be a function of the
    instruction words) and scaled.  fmt: '%d' or '0x%x'."""
    pat = NUM

    def __init__(self, bits, signed=False, scale=1, fmt='%d', mask=0):
        self.bits, self.signed, self.scale, self.fmt = bits, signed, scale, fmt
        self.mask = mask           # low bits of the field that are not offset

    def _signed(self, ws):
        return self.signed(ws) if callable(self.signed) else self.signed

    def value(self, ws):
        v = getf(ws, self.bits) & ~self.mask
        if self._signed(ws):
            v = sx(v, width(self.bits))
        return v * self.scale

    def number(self, i):
        return self.value(i.ws)

    def show(self, i):
        v = self.value(i.ws)
        if v < 0 and self.fmt.startswith('0x'):
            return '-' + self.fmt % -v
        return self.fmt % v

    def put(self, ws, addr, s):
        v = parse_int(s)
        if v % self.scale:
            raise EncodeError('%d not a multiple of %d' % (v, self.scale))
        v //= self.scale
        n = width(self.bits)
        lo, hi = (-(1 << (n - 1)), (1 << (n - 1)) - 1) if self._signed(ws) else (0, (1 << n) - 1)
        if not lo <= v <= hi:
            # accept the other signedness when the bit pattern is the same
            if not (-(1 << (n - 1)) <= v <= (1 << n) - 1):
                raise EncodeError('%s out of range' % s)
        if v & self.mask:
            raise EncodeError('%s: low bits are opcode bits' % s)
        keep = getf(ws, self.bits) & self.mask
        setf(ws, self.bits, (v & ~self.mask) | keep)


class Target:
    """pc-relative target printed as an absolute address."""
    pat = r'0x[0-9a-fA-F]+'

    def __init__(self, bits, scale=2, signed=True, align=False, mask=0):
        self.bits, self.scale, self.signed, self.align = bits, scale, signed, align
        self.mask = mask

    def addr(self, ws, a):
        v = getf(ws, self.bits) & ~self.mask
        if self.signed:
            v = sx(v, width(self.bits))
        return ((a & ~3) if self.align else a) + v * self.scale

    def number(self, i):
        return self.addr(i.ws, i.addr)

    def show(self, i):
        i.target = self.addr(i.ws, i.addr)
        return '0x%x' % i.target

    def put(self, ws, addr, s):
        d = int(s, 16) - ((addr & ~3) if self.align else addr)
        if d % self.scale:
            raise EncodeError('misaligned target')
        d //= self.scale
        n = width(self.bits)
        lo, hi = (-(1 << (n - 1)), (1 << (n - 1)) - 1) if self.signed else (0, (1 << n) - 1)
        if not lo <= d <= hi or d & self.mask:
            raise EncodeError('target out of range')
        keep = getf(ws, self.bits) & self.mask
        setf(ws, self.bits, (d & ~self.mask) | keep)


class Tab:
    """Value printed through a name table, with a numbered fallback."""
    def __init__(self, bits, table, fallback):
        self.bits, self.table, self.fallback = bits, table, fallback
        names = sorted((n for n in (table.values() if isinstance(table, dict)
                                    else table)), key=len, reverse=True)
        fb = fallback.replace('%d', r'\d+').replace('.', r'\.')
        self.pat = '|'.join([re.escape(n) for n in names if n] + [fb]) + \
            ('|' if '' in (table.values() if isinstance(table, dict) else table) else '')

    def show(self, i):
        v = getf(i.ws, self.bits)
        t = self.table
        name = t.get(v) if isinstance(t, dict) else t[v]
        return self.fallback % v if name is None else name

    def put(self, ws, addr, s):
        t = self.table
        items = t.items() if isinstance(t, dict) else enumerate(t)
        for k, n in items:
            if n == s:
                setf(ws, self.bits, k)
                return
        m = re.fullmatch(self.fallback.replace('%d', r'(\d+)').replace('.', r'\.'), s)
        if not m:
            raise EncodeError('bad name %s' % s)
        setf(ws, self.bits, int(m.group(1)))


class Word:
    """A raw halfword (for instructions whose fields are not decoded)."""
    pat = r'0x[0-9a-fA-F]{4}'

    def __init__(self, w):
        self.bits = F((w, 15, 0))

    def show(self, i):
        return '0x%04x' % getf(i.ws, self.bits)

    def put(self, ws, addr, s):
        setf(ws, self.bits, int(s, 16))


class RegList:
    """push/pop byte: L bb mmmmm (VC IV layout)."""
    pat = r'[a-z0-9, -]*'

    def __init__(self, pop):
        self.pop = pop

    def number(self, i):
        """Registers in the list (lr / pc as 26 / 31)."""
        h = i.ws[0]
        lr, base, m = (h >> 7) & 1, (0, 6, 16, 24)[(h >> 5) & 3], h & 31
        regs = [] if base == 24 and m == 15 else list(range(base, base + m + 1))
        return regs + ([31 if self.pop else 26] if lr else [])

    def show(self, i):
        h = i.ws[0]
        lr, base, m = (h >> 7) & 1, (0, 6, 16, 24)[(h >> 5) & 3], h & 31
        lst = [] if base == 24 and m == 15 else (
            [reg(base)] if m == 0 else ['%s-%s' % (reg(base), reg(base + m))])
        if lr:
            lst.append('pc' if self.pop else 'lr')
        return ', '.join(lst)

    def put(self, ws, addr, s):
        parts = [p.strip() for p in s.split(',') if p.strip()]
        lr = 1 if parts and parts[-1] in ('lr', 'pc') else 0
        if lr:
            parts = parts[:-1]
        if not parts:
            base, m = 24, 15
        else:
            if len(parts) != 1:
                raise EncodeError('one register range only')
            a, _, b = parts[0].partition('-')
            num = lambda r: REGNUM[r] if r in REGNUM else int(r[1:])
            lo = num(a)
            hi = num(b) if b else lo
            if lo not in (0, 6, 16, 24) or not 0 <= hi - lo < 32:
                raise EncodeError('bad register range')
            base, m = lo, hi - lo
        ws[0] = (ws[0] & 0xff00) | lr << 7 | (0, 6, 16, 24).index(base) << 5 | m


# --------------------------------------------------------------------------
# The encoding table.
#
# Each row: name, length (halfwords), match list [(word, mask, value)],
# syntax with {field} placeholders, the fields, confidence, a one-line
# evidence note and the control-flow kind.  The first matching row wins
# when decoding, so specific rows come before general ones.  The
# assembler parses the same syntax and sets the same bits.

class Enc:
    def __init__(self, name, length, match, syntax, fields, conf, ev, kind=None):
        self.name, self.length, self.matches = name, length, match
        self.syntax, self.fields = syntax, fields
        self.conf, self.ev, self.kind = conf, ev, kind
        self.mask, self.value = match[0][1], match[0][2]
        parts = re.split(r'(\{\w+\})', syntax)
        rx = ''
        for p in parts:
            if p.startswith('{'):
                rx += '(?P<%s>%s)' % (p[1:-1], fields[p[1:-1]].pat)
            else:
                rx += re.escape(p).replace(r'\ ', r'\s*')
        self.rx = re.compile(rx + r'\s*$')

    def match(self, ws):
        return all(len(ws) > w and ws[w] & m == v for w, m, v in self.matches)

    def render(self, i):
        i.text = re.sub(r'\{(\w+)\}', lambda m: self.fields[m.group(1)].show(i),
                        self.syntax)

    def encode(self, text, addr):
        m = self.rx.match(text.strip())
        if not m:
            return None
        ws = [0] * self.length
        for w, mk, v in self.matches:
            ws[w] = (ws[w] & ~mk) | v
        for name, f in self.fields.items():
            f.put(ws, addr, m.group(name))
        return ws if self.match(ws) else None


TABLE = []


def enc(name, length, match, syntax, fields, conf, ev, kind=None):
    if isinstance(match, tuple) and not isinstance(match[0], tuple):
        match = [(0,) + match]
    TABLE.append(Enc(name, length, match, syntax, fields, conf, ev, kind))


R0 = Reg(F((0, 3, 0)))
R4 = Reg(F((0, 7, 4)))
R5 = Reg(F((0, 4, 0)))


class I:
    """A decoded instruction."""
    def __init__(self, addr, ws, e):
        self.addr, self.ws, self.e = addr, ws, e
        self.size = 2 * len(ws)
        self.text = ''
        self.target = None        # branch/call/lea target address
        self.table = None         # switch table: (kind, start, entries)

    def values(self):
        """{field: number}: registers as numbers, immediates scaled and
        signed, targets absolute, name tables as their index; vector
        operand fields as small dicts of their sub-fields."""
        out = {}
        for k, f in self.e.fields.items():
            out[k] = f.number(self) if hasattr(f, 'number') else getf(self.ws, f.bits)
        return out


# ---- 16-bit ---------------------------------------------------------------

enc('rts', 1, (0xffff, 0x4430), 'rts', {}, 'high',
    'last halfword of 86+ functions; never followed by fallthrough code',
    kind='ret')
for _n, (_s, _c, _e) in {
        'nop': (2, 'high', 'runs of 4230 after control-register writes in '
                'vmcs.bin reset code'),
        'sleep': (3, 'medium', 'vmcs.bin: "4330; b self" halt loop, '
                  '"ei; 4330; di" idle'),
        'ei': (10, 'medium', 'vmcs.bin: before rts in critical-section exits, '
               'after "mov sr, rN"'),
        'di': (11, 'medium', 'vmcs.bin: right after "mov rN, sr" at '
               'critical-section entries'),
        'bkpt': (18, 'low', 'exception stubs "5230; rti"; switch default '
                 'paths; after timeouts'),
        'rti': (24, 'medium', 'end of the vmcs.bin interrupt handler and of '
                'every exception stub')}.items():
    enc(_n, 1, (0xffff, 0x4030 | (_s & 15) << 8 | (_s >> 4) << 12), _n, {},
        _c, _e, kind='ret' if _n == 'rti' else None)
enc('bl', 1, (0xfff0, 0x4730), 'bl {d}', {'d': R0}, 'medium',
    'repeated calls with fresh r0/r1 arguments and a callee kept in r15',
    kind='callr')
enc('tbb', 1, (0xfff0, 0x4c30), 'tbb {d}', {'d': R0}, 'high',
    'followed by a byte table of halfword offsets; bounded by the guarding '
    'compare', kind='switch')
enc('tbh', 1, (0xfff0, 0x4d30), 'tbh {d}', {'d': R0}, 'high',
    'followed by a (signed) halfword table of halfword offsets', kind='switch')
enc('cmp0', 1, (0xfff0, 0x5030), 'cmp {d}, #0', {'d': R0}, 'medium',
    'stream_finished: bl audio_playgetlatency; 5030; seq r0; pop pc '
    '(returns result==0); always followed by a flag consumer')
enc('misc4', 1, (0xe0f0, 0x4030), 'sys{s} {d}',
    {'s': Imm(F((0, 12, 8))), 'd': R0}, 'low',
    '010s ssss 0011 rrrr selector group (rts/bl/tbb/tbh/nop/ei/di live '
    'here); other selectors unidentified')
enc('lea', 1, (0xe010, 0x4000), 'lea {d}, {t}',
    {'d': R0, 't': Target(F((0, 12, 5)), scale=4, signed=False, align=True)},
    'high', 'loads addresses of the string literals placed after the function '
    '("enc", "dec", ".vll" in mplayer codec_name)', kind='leapc')
_SPO = Imm(F((0, 12, 8)), scale=4)
enc('ldst_sp', 1, (0xe0f0, 0x4010), 'ld {d}, (sp+{o})', {'d': R0, 'o': _SPO},
    'high', 'frame slots inside the 3fxx allocation; aes_init stores, takes '
    'the address of and reloads one stack word around RotWord/SubWord')
enc('ldst_sp', 1, (0xe0f0, 0x4050), 'st {d}, (sp+{o})', {'d': R0, 'o': _SPO},
    'high', 'as ld')
enc('ldst_sp', 1, (0xe0f0, 0x4090), 'lea {d}, (sp+{o})', {'d': R0, 'o': _SPO},
    'high', 'address of a frame slot passed to aes_rotword/aes_subword')
enc('ldst_sp', 1, (0xe0f0, 0x40d0), 'op4sp.3 {d}, (sp+{o})',
    {'d': R0, 'o': _SPO}, 'low', 'fourth sp-relative op, unseen')
enc('addsp', 1, (0xffc0, 0x3f00), 'add sp, #{i}',
    {'i': Imm(F((0, 5, 0)), signed=True, scale=4)}, 'high',
    'prologue 3f3f/3f3c paired with epilogue 3f01/3f04; size matches the '
    'highest sp offset used')
enc('scc', 1, (0xf0f0, 0x30f0), 's{c} {d}',
    {'c': Tab(F((0, 11, 8)), COND[:14] + ['al', 'nv'], 'cc%d'), 'd': R0},
    'medium', 'mp_fifo_empty: cmp then 30f0 then rts (returns rd==wr)')
enc('bcc.s', 1, (0xf080, 0x3000), 'b{c} {t}',
    {'c': Tab(F((0, 11, 8)), COND, 'cc%d'), 't': Target(F((0, 6, 0)))},
    'high', 'aes_init key-size dispatch (cmp 192/128/256), tolower range '
    'checks; 7-bit signed halfword offset', kind='cbranch')
enc('ldst_postinc', 1, (0xf800, 0x7800), '{w} {d}, ({s})++',
    {'w': Tab(F((0, 10, 8)), LDST, 'w%d'), 'd': R0, 's': R4}, 'high',
    'aes_subword: ldb/indexed load/stb (r0)++ loop; aes_init w[] walk')
enc('ldst_predec', 1, (0xf800, 0x7000), '{w} {d}, (--{s})',
    {'w': Tab(F((0, 10, 8)), LDST, 'w%d'), 'd': R0, 's': R4},
    'medium', 'ldb/stb high, other widths medium (by analogy with (rs)++); '
    'rs -= size, then access at rs.  Twin of the (rs)++ row: memmove backward tail in vmcs.bin/RenderServer'
    '.bin (vmcs 1.3 0x1d208: 7415 7505 = ldb r5,(--r1); stb r5,(--r0)) '
    'mirrors the forward tail 7c15 7d05; device session 1: 7050 = ld r0,'
    '(--r5) with r5 = 0x7f faulted (unaligned)')
_IMM5 = Imm(F((0, 8, 4)), signed=lambda ws: 2 * ((ws[0] >> 9) & 15) not in ALU_UIMM)
enc('alu_imm5', 1, (0xf000, 0x6000), '{op} {d}, #{i}',
    {'op': Tab(F((0, 12, 9)), {k // 2: v for k, v in ALU.items()
                               if k % 2 == 0 and k < 16}, 'aluq%d'),
     'd': R0, 'i': _IMM5}, 'high',
    '6eIR loads small constants (calloc sizes, AES Nk/Nr); op q = ALU op 2q, '
    'q 0..7 only (7000-77ff is (--rs)); 6df0 = add #-1 (ue-1), 6881 = lsr '
    '#8 (RotWord)')
enc('ldst_ind', 1, (0xf800, 0xb800), '{w} {d}, ({s})',
    {'w': Tab(F((0, 10, 8)), LDST, 'w%d'), 'd': R0, 's': R4}, 'high',
    '10 111 www ssss dddd: mp_fifo_init/size, aes_rotword')
enc('ldst_ind', 1, (0xc000, 0x8000), '{w} {d}, ({s}+{x})',
    {'w': Tab(F((0, 10, 8)), LDST, 'w%d'), 'd': R0, 's': R4,
     'x': Reg(F((0, 13, 11)))}, 'medium',
    '10 iii www ssss dddd, iii<7 adds index register r0..r6 (aes_subword: '
    'ldb r1,(r4+r3) = sbox[b]; calcmvpredictor neighbour MVs); index '
    'scaling unknown')
enc('pop', 1, (0xff00, 0xe000), 'pop {l}', {'l': RegList(True)}, 'high',
    'e0aX ends functions that start with f0aX; e02X+rts pairs with f02X; '
    'byte = L bb mmmmm exactly as VC IV push/pop (bb: r0/r6/r16/r24; base '
    '24 + m 15 = lr/pc only)')
enc('push', 1, (0xff00, 0xf000), 'push {l}', {'l': RegList(False)}, 'high',
    'f0aX/f0ef/f02X prologues; callee then uses r6..r6+X')
_OFF4 = Imm(F((0, 11, 8)), scale=4)
enc('ldst_off', 1, (0xf000, 0xe000), 'ld {d}, ({s}+{o})',
    {'d': R0, 's': R4, 'o': _OFF4}, 'high', 'struct field reads')
enc('ldst_off', 1, (0xf000, 0xf000), 'st {d}, ({s}+{o})',
    {'d': R0, 's': R4, 'o': _OFF4}, 'high',
    'mp_fifo_init: st r2,(r0+4) etc.; Bitstream_Set fills a struct')
enc('alu_reg', 1, (0xc000, 0xc000), '{op} {d}, {s}',
    {'op': Tab(F((0, 12, 8)), ALU, 'alu%d'), 'd': R0, 's': R4}, 'high',
    'ce=mov (argument shuffles), cc=add, cf=or (AAC bit packing), c5=eor (AES)')

# ---- 32-bit ---------------------------------------------------------------

_CC = Tab(F((0, 11, 8)), COND, 'cc%d')
_AD = Reg(F((0, 3, 0)))
_ACM = (('{a}', Reg(F((0, 7, 4)))), ('#{a}', Imm(F((0, 7, 4)), signed=True)))
for _m in range(4):
    _a = _ACM[_m & 1]
    if _m < 2:
        _b, _bf, _t = '{b}', Reg(F((1, 13, 10))), Target(F((1, 9, 0)))
    else:
        _b, _bf, _t = '#{b}', Imm(F((1, 13, 8))), Target(F((1, 7, 0)))
    enc('addcmpb', 2, [(0, 0xf000, 0x0000), (1, 0xc000, _m << 14)],
        'addcmpb{c} {d}, %s, %s, {t}' % (_a[0], _b),
        {'c': _CC, 'd': _AD, 'a': _a[1], 'b': _bf, 't': _t}, 'high',
        'rd += a; compare rd with b; branch: counted loops (0cf2 c0fd), null '
        'checks; the four VC IV addcmpb operand modes', kind='cbranch')
enc('bl', 2, (0xf080, 0x1080), 'bl {t}',
    {'t': Target(F((0, 11, 8), (1, 15, 0), (0, 6, 0)))}, 'high',
    'all reloc type 23 (imports) sit here; local targets land on symbol starts',
    kind='call')
enc('bcc', 2, (0xf080, 0x1000), 'b{c} {t}',
    {'c': _CC, 't': Target(F((1, 15, 0), (0, 6, 0)))}, 'high',
    'same layout as VC IV 0x9xxx b.cc; targets land on instruction starts',
    kind='cbranch')
_RS, _RD = Reg(F((0, 7, 4))), Reg(F((0, 3, 0)))
enc('ldst_off16', 2, [(0, 0xff00, 0x2800), (1, 0x0003, 0x0000)],
    'ld {d}, ({s}{o})', {'d': _RD, 's': _RS,
                         'o': Imm(F((1, 15, 0)), signed=True, mask=3, fmt='%+d')},
    'high', '28: word; all offsets >= 64 (the 16-bit form covers 0..60)')
enc('ldst_off16', 2, [(0, 0xff00, 0x2800), (1, 0x0003, 0x0002)],
    'st {d}, ({s}{o})', {'d': _RD, 's': _RS,
                         'o': Imm(F((1, 15, 0)), signed=True, mask=3, fmt='%+d')},
    'high', '28, offset bit1 set: store (the value is computed just before)')
# The two halfword loads with an offset are named by what silicon does
# (quirks.md #16, #33): 0x28/o1 zero-extends (ldh), 0x29/o1 sign-extends
# (ldsh).  Apple's names, read by VC IV analogy, were the other way round.
enc('ldst_off16', 2, [(0, 0xff00, 0x2800), (1, 0x0001, 0x0001)],
    'ldh {d}, ({s}{o})', {'d': _RD, 's': _RS,
                          'o': Imm(F((1, 15, 0)), signed=True, mask=1, fmt='%+d')},
    'high', '28 with offset bit0 set (rare): offsets are 2 mod 4 or 0 mod 4, '
    'often rd == rs ("2810 0003"), so a load; zero-extended on silicon '
    '(S17: a transform\'s dump; 2026-10-25: COMMIT\'s sum)')
enc('ldst_off16', 2, [(0, 0xff00, 0x2900), (1, 0x0001, 0x0001)],
    'ldsh {d}, ({s}{o})', {'d': _RD, 's': _RS,
                           'o': Imm(F((1, 15, 0)), signed=True, mask=1, fmt='%+d')},
    'high', '29, offset bit0 set: halfword load, sign-extended on silicon '
    '(2026-10-25 Port write check: 12 of 12 COMMIT sums fit)')
enc('ldst_off16', 2, [(0, 0xff00, 0x2900), (1, 0x0001, 0x0000)],
    'sth {d}, ({s}{o})', {'d': _RD, 's': _RS,
                          'o': Imm(F((1, 15, 0)), signed=True, mask=1, fmt='%+d')},
    'medium', 'AACDecoderInit zeroes halfword fields with 29xx')
enc('ldst_off16', 2, (0xff00, 0x2a00), 'ldb {d}, ({s}{o})',
    {'d': _RD, 's': _RS, 'o': Imm(F((1, 15, 0)), signed=True, fmt='%+d')},
    'high', 'AAC bit reader fetches the next byte (r+1)')
enc('ldst_off16', 2, (0xff00, 0x2b00), 'stb {d}, ({s}{o})',
    {'d': _RD, 's': _RS, 'o': Imm(F((1, 15, 0)), signed=True, fmt='%+d')},
    'high', 'ADTS header parser stores fields byte by byte (r0+2, +3, ...)')
enc('add16', 2, (0xff00, 0x2c00), 'add {d}, {s}, #{i}',
    {'d': _RD, 's': _RS, 'i': Imm(F((1, 15, 0)), signed=True)}, 'high',
    "struct member addresses before calls, loop strides (my_memset +0x200), "
    "'A'..'Z' + 0x20 in hndlr_get_protocol")
# 2dxx (sim.md "2dxx"): o = hw0[7:4]; o 0-3 gp-relative, o 4-7
# sp-relative, o & 3 = the width class of the 28/29/2a/2b family; o 12/13
# = lea gp/sp.  Offsets unsigned 16-bit.
for _o in range(8):
    _bn = 'gp' if _o < 4 else 'sp'
    _fm = '0x%x' if _o < 4 else '%d'
    _cls = _o & 3
    # halfword loads named as the 28/29 family's [hypothesis for 2dxx]
    _rows = {0: [('ld', 3, 0, 3), ('st', 3, 2, 3), ('ldh', 1, 1, 1)],
             1: [('ldsh', 1, 1, 1), ('sth', 1, 0, 1)],
             2: [('ldb', 0, 0, 0)], 3: [('stb', 0, 0, 0)]}[_cls]
    for _nm, _mk, _vl, _ms in _rows:
        enc('gp16', 2, [(0, 0xfff0, 0x2d00 | _o << 4), (1, _mk, _vl)],
            '%s {d}, (%s+{o})' % (_nm, _bn),
            {'d': _RD, 'o': Imm(F((1, 15, 0)), fmt=_fm, mask=_ms)},
            'medium' if _o in (0, 4) else 'low',
            'gp: vmcs.bin globals (interrupt handler, ld sp,(gp+992)); sp: '
            'codec_string fourcc at sp+0, psnprintf ldb (sp+20), '
            'AACDecode_Ittiam sp+0x224, h264_fastinter8x8 sth/ldh sp+0x80')
enc('gp16', 2, (0xfff0, 0x2dc0), 'lea {d}, (gp+{o})',
    {'d': _RD, 'o': Imm(F((1, 15, 0)), fmt='0x%x')}, 'medium',
    'lea r0, gp+0x3d8 before calls to the same lock routine')
enc('gp16', 2, (0xfff0, 0x2dd0), 'lea {d}, (sp+{o})',
    {'d': _RD, 'o': Imm(F((1, 15, 0)))}, 'medium',
    'codec_string: lea r4,(sp+3) walks a fourcc stored at sp+0')
enc('gp16', 2, (0xff00, 0x2d00), 'gp{op} {d}, (gp+{o})',
    {'op': Imm(F((0, 7, 4))), 'd': _RD, 'o': Imm(F((1, 15, 0)), fmt='0x%x')},
    'low', 'o = 8-11, 14, 15: unresolved (unseen in the .vll libraries)')
enc('alu_imm16', 2, (0xff80, 0x2e00), '{op} {d}, #{i}',
    {'op': Tab(F((0, 6, 5)), ['cmp', 'mul', 'add', 'mov'], 'x%d'),
     'd': R5, 'i': Imm(F((1, 15, 0)), signed=True)}, 'high',
    '2e01 00c0/0080/0100 compare AES key bits; 2e2x 0078 struct stride '
    'multiply; 2e59 fdec/0214 frame alloc/free on sp; 2e6x mov')
_RA5, _RB5 = Reg(F((1, 4, 0))), Reg(F((1, 9, 5)))
_U5 = Imm(F((1, 9, 5)))
enc('alu3', 2, [(0, 0xffe0, 0x2ea0), (1, 0xfc1f, 14 << 10)], 'mov {d}, {b}',
    {'d': R5, 'b': _RB5}, 'high', '3-operand mov ignores ra (sr save/restore '
    'in vmcs.bin: "mov r1, sr" ... "mov sr, r1")')
enc('alu3', 2, [(0, 0xffe0, 0x2ea0), (1, 0xfc1f, 0x8000 | 14 << 10)],
    'mov {d}, #{b}', {'d': R5, 'b': _U5}, 'medium', 'immediate form of the above')
enc('alu3', 2, [(0, 0xffe0, 0x2ea0), (1, 0x8000, 0)], '{op} {d}, {a}, {b}',
    {'op': Tab(F((1, 14, 10)), ALU, 'alu%d'), 'd': R5, 'a': _RA5, 'b': _RB5},
    'high', 'my_memset address = base + offset (add), remainder (sub); '
    'lsl r2, r4, r5 in the ADTS bit reader')
enc('alu3', 2, [(0, 0xffe0, 0x2ea0), (1, 0x8000, 0x8000)],
    '{op} {d}, {a}, #{b}',
    {'op': Tab(F((1, 14, 10)), ALU, 'alu%d'), 'd': R5, 'a': _RA5, 'b': _U5},
    'high', 'my_memset asr r6, r8, #8 / lsr #23 rounding trick; RotWord '
    'lsl r2, r1, #24; cbp tests and r14, r6, #16/#8/#4/#2')
enc('alu3', 2, [(0, 0xffe0, 0x2e80), (1, 0x8000, 0)], '{op} {d}, {a}, {b}',
    {'op': Tab(F((1, 14, 10)), EXT, 'ext%d'), 'd': R5, 'a': _RA5, 'b': _RB5},
    'medium', 'div.uu r6, r13, r10 (i / Nk) in the AES key schedule; '
    'mulhd.uu by 0x028f5c28 (= x / 100) in vmcs.bin')
enc('alu3', 2, [(0, 0xffe0, 0x2e80), (1, 0x8000, 0x8000)],
    '{op} {d}, {a}, #{b}',
    {'op': Tab(F((1, 14, 10)), EXT, 'ext%d'), 'd': R5, 'a': _RA5, 'b': _U5},
    'medium', 'immediate form of the extended group')


def raw(name, length, match, conf, ev, prefix=None):
    syn = '.%s ' % (prefix or 'hw%d' % (16 * length)) + \
        ' '.join('{w%d}' % k for k in range(length))
    enc(name, length, match, syn, {'w%d' % k: Word(k) for k in range(length)},
        conf, ev)


raw('op2ec', 2, (0xffc0, 0x2ec0), 'low', 'unseen')
raw('op24', 2, (0xfc00, 0x2400), 'low', 'unseen once vector lengths are known')

# ---- 48-bit ---------------------------------------------------------------

_B27 = F((2, 10, 0), (1, 15, 0))
_BASE = Reg(F((2, 15, 11)))
_PCB = (2, 0xf800, 0xf800)
enc('mov32', 3, (0xffe0, 0x2f60), 'mov {d}, #{i}',
    {'d': R5, 'i': Imm(F((2, 15, 0), (1, 15, 0)), fmt='0x%x')}, 'high',
    'pns_decode loads 0x3c6ef35f (Numerical Recipes LCG constant); '
    '0x10000400 / 0xa5a50002 in vmcs.bin reset code')
enc('lea48', 3, [(0, 0xffe0, 0x2f20), _PCB], 'lea {d}, {t}',
    {'d': R5, 't': Target(_B27, scale=1)}, 'medium',
    'reloc type 11 (data addresses) on 2f2r 0000 f800; base field 31 = pc',
    kind='lea48')
enc('lea48', 3, (0xffe0, 0x2f20), 'lea {d}, ({b}{o})',
    {'d': R5, 'b': _BASE, 'o': Imm(_B27, signed=True, fmt='%+d')}, 'medium',
    'lea rd, gp+X in vmcs.bin reset code')
# 48-bit memory family 2f0d/2f4d/2f8d..2fed (proved by the simulator,
# sim.md "48-bit memory ops"): opcode = hw0 bit 7 : hw1 bits 1:0 ->
# ld st ldh sth / ldb stb ldsh ldsb; offset = s27 with its low two bits
# replaced by hw0 bit 6 (offset bit 1) and, when bit 7 is set, hw0 bit 5
# (offset bit 0).  With bit 7 clear, bit 5 set is lea/mov32 (above).
MEM48_OPS = ['ld', 'st', 'ldh', 'sth', 'ldb', 'stb', 'ldsh', 'ldsb']


def mem48_offset(ws, offset_bits=True):
    """Byte offset of a 2fxx memory instruction."""
    h0, h1, h2 = ws[0], ws[1], ws[2]
    o27 = sx(((h2 & 0x7ff) << 16) | h1, 27)
    if not offset_bits:
        return o27 & ~3
    b7, b6, b5 = (h0 >> 7) & 1, (h0 >> 6) & 1, (h0 >> 5) & 1
    return (o27 & ~3) | (b6 << 1) | (b5 if b7 else 0)


def mem48_decode(ws, offset_bits=True):
    """(op, base register, offset) of a 2fxx memory instruction."""
    return (MEM48_OPS[((ws[0] >> 7) & 1) << 2 | (ws[1] & 3)], ws[2] >> 11,
            mem48_offset(ws, offset_bits))


def mem48_encode_offset(ws, off):
    """ws with its offset replaced by off (opcode kept)."""
    h0, h1, h2 = ws[0], ws[1], ws[2]
    b7 = (h0 >> 7) & 1
    if not b7 and off & 1:
        raise EncodeError('odd offset for a word/halfword 48-bit access')
    h0 = (h0 & ~0x60) | ((off >> 1) & 1) << 6 | ((off & 1) << 5 if b7 else 0)
    o = ((off & ~3) | (h1 & 3)) & 0x7ffffff
    return [h0, o & 0xffff, (h2 & 0xf800) | (o >> 16)]


# pc-relative 48-bit loads/stores take their base from the instruction
# address rounded down to a word (as Thumb literal loads do); the 48-bit
# lea uses the address itself.  Evidence (hardware.md, "pc-relative
# base"): in vmcs.bin every pc-relative word ld/st at an address = 2 mod 4
# (15 of them) would be unaligned with the plain address and all are
# word-aligned with it rounded down; the 8 at 0 mod 4 agree either way.
PC48_MEM_ALIGN = True


def pc48_mem_base(addr):
    return addr & ~3 if PC48_MEM_ALIGN else addr


class Mem48Off:
    """Exact offset of a 48-bit memory op ('%+d'), or with pc=True the
    absolute target (base r31 = the instruction's address rounded down
    to a word, pc48_mem_base)."""
    def __init__(self, pc=False):
        self.pc = pc
        self.pat = r'0x[0-9a-fA-F]+' if pc else NUM

    def number(self, i):
        o = mem48_offset(i.ws)
        return pc48_mem_base(i.addr) + o if self.pc else o

    def show(self, i):
        v = self.number(i)
        if self.pc:
            i.target = v
            return '0x%x' % v
        return '%+d' % v

    def put(self, ws, addr, s):
        off = int(s, 16) - pc48_mem_base(addr) if self.pc else parse_int(s)
        if not -(1 << 26) <= off < 1 << 26:
            raise EncodeError('offset out of range')
        ws[:3] = mem48_encode_offset(ws, off)


_M48OP = Tab(F((0, 7, 7), (1, 1, 0)), MEM48_OPS, 'm48op%d')
for _b7 in (0, 1):
    _mt = (0, 0xffa0, 0x2f00) if not _b7 else (0, 0xff80, 0x2f80)
    _cf = 'medium' if not _b7 else 'medium'
    enc('mem48', 3, [_mt, _PCB], '{op} {d}, {t}',
        {'op': _M48OP, 'd': R5, 't': Mem48Off(pc=True)}, _cf,
        'pc-relative: R18 (.sdata words) / R17 (halfwords) / R16 (bytes) '
        'sites; the self-test reads them back through the loader',
        kind='mem48')
    enc('mem48', 3, [_mt], '{op} {d}, ({b}{o})',
        {'op': _M48OP, 'd': R5, 'b': _BASE, 'o': Mem48Off()}, _cf,
        'h264_readue 2f42 ffe2 87ff = ldh (r16+62); vc_readzerosdc1 '
        'ldb/ldsb at +0/+2; adts_error_check 2f83 = ldsb (r0+4); ld lr,(sp)')

# ---- vector unit (VPU) ------------------------------------------------------
#
# 48-bit (22xx/23xx) and 80-bit (20xx/21xx).  Lengths are high confidence;
# the field layout below is recovered from the H.264/slideshow kernels (see
# isa.md "Vector unit" for the evidence and confidence of every field).
# Every bit of every vector instruction is assigned to a field, so the text
# form is lossless (the --roundtrip check covers it).  VRF coordinates are
# printed as raw field values (d.., a.., b..) because their mapping to (y, x)
# is only partly understood.
#
#   hw1   0010 00Lo oooo oooo        L=1: 48-bit; op9 = hw1[8:0]
#   hw2   dddd dddd sss ttttt        D: coordinate d8, +r(sss) (7 = none),
#                                    type t5 (bit 4 set on V-vector results)
#   ALU 80:
#   hw3   aaaaa I bbbbbbbbbb         A coordinate low 5 / imm flag / B or imm low
#   hw4   BBBBBB AAAAAA sss n        B ext (or imm high 6) / A high 6 /
#                                    A +r(sss) / n = "inc" (set iff REP > 1)
#   hw5   h F PPP CC 0 RRR SS rrr    h ?, F SETF, P predication, C acc mode,
#                                    R REP, S/r scalar-result unit fn/reg
#   B ext (hw4[15:10]) = S sss t i   S=1: B is scalar r(sss) (+ signed offset
#                                    in t:i:hw3[9:0]); else B vector, +r(sss),
#                                    type t, i = B increments per REP
#   ALU 48:  hw3 = aaaaa 0 bbbbbbbbbb   or   aaaaa 1 m RRR iiiiii (imm6)
#   memory (op9 < 0x20):  hw3 = ooooooo RRR bbb sss  (offset7, REP, base
#                                    r0-r7, stride r0-r6 / 7 = none)

VPRED = ['', 'none', 'ifz', 'ifnz', 'ifn', 'ifnn', 'ifc', 'ifnc']
VREP = ['', 'rep2', 'rep4', 'rep8', 'rep16', 'rep32', 'rep64', 'repr0']
# Scalar-result unit, hw5[4:3] with the destination in hw5[2:0]: 1 = sum of
# the enabled lanes (vc_intra_4x4_dc), 2/3 = min/max (expand_range_16s).
VSRU = ['sru0', 'sum', 'min', 'max']

# Operation names by op6 = op9[8:3]; op9[2:1] is printed as a ".w" suffix
# (its meaning is open: it is not simply the element width) and op9[0]
# (48-bit only) as "x".  Unnamed operations print as vopNNN.
VOP6 = {
    4: ('vmov', 'high', 'invzigzag: D[k] = B[zz[k]] for all 16 positions; '
        'rows set from r1..r5 in vc_macroblock_init; acc seed #16'),
    8: ('vadd', 'high', 'vc_inverseinteger e = d0 + d2, out0 = e + h; '
        '(x + 32) in h264_invert; 6-tap last tap'),
    9: ('vadds', 'high', 'vc_ChromaEdges_edge p0\' = clip(p0 + delta) on '
        'the lanes that passed the alpha/beta tests (ifn)'),
    10: ('vsub', 'high', 'vc_inverseinteger f = d0 - d2, g = (d1 >> 1) - d3'),
    11: ('vsubs', 'high', 'vc_ChromaEdges_edge q0\' = clip(q0 - delta), ifn'),
    12: ('vrsub', 'high', 'B - A: alpha_blt_block 0xffff - alpha (#-1), '
         'tested; deblocking -tc = 0 - tc (#0)'),
    14: ('vmul', 'high', '6-tap filter taps #-5, #20, #20, #-5 with acc; '
         'chroma bilinear weights (8-dx), dx, (8-dy), dy from scalars'),
    26: ('vasr', 'high', 'vc_inverseinteger d1 >> 1, d3 >> 1; (x + 32) >> 6 '
         'in h264_invert'),
    28: ('vmin', 'medium', 'expand_range_16s running minimum (row 1)'),
    29: ('vmax', 'medium', 'expand_range_16s running maximum (row 2)'),
    16: ('vmulm', 'high', '(A * B) >> 8: alpha_blt_block per-byte blend, '
         'tested'),
    17: ('vmulms', 'high', 'saturating (A * B) >> 8: 6-tap (sum + 16) * 8 >> 8 '
         'clipped to 0..255, tested'),
    20: ('vmulhr', 'high', '(A * B + 0x8000) >> 16, B unsigned: chroma #1024 = '
         '(x + 32) >> 6, tested'),
    21: ('vmulhrs', 'high', 'saturating vmulhr: (G + h + 1) >> 1 via #0x8000, '
         'DDL/VL (x + 2) >> 2 via #0x4000, tested'),
    24: ('vshl', 'high', 'A << B: DDR/DDL acc += 2b (#1), tested'),
    27: ('vlsr', 'high', 'logical A >> B: (a + 2b + c + 2) >> 2 in the '
         'diagonal intra predictions, tested'),
    52: ('vmulhru', 'medium', '(A * B + 0x8000) >> 16 like vmulhr: chroma '
         'bS=4 p0\' = (2p1 + p0 + q1 + 2) >> 2 via #0x4000, tested on '
         'non-negative A; the difference from op6 20 is not seen'),
    30: ('vdist', 'high', '|A - B|: deblocking |p0 - q0|, |p1 - p0|, '
         '|q1 - q0| then vsub alpha/beta setf ifn, tested'),
    37: ('vclip', 'medium', 'clamp A to 0..B: Clip3(0, 51, qp) in '
         'vc_calc_alpha_beta, Clip1 of the centre half-pel j in '
         'vc_interp16xN_jhoz (tested in vc2sim), #255 in contour_threshold'),
    18: ('vmulhd', 'high', '(A * B) >> 16, floored, both signed: the high '
         'half of the 32-bit 6-tap products in vc_interp16xN_jhoz/jvert, '
         'partner of vmul ... x15 (tested in vc2sim)'),
    19: ('vmulhdsu', 'medium', '(A * unsigned B) >> 16, floored: the low '
         'word of a 16.16 scale in expand_range_16s, the 16.16 ramp at '
         'vmcs 0x153f8'),
    51: ('vmulhduu', 'medium', '(unsigned A * unsigned B) >> 16, floored: '
         'the low word of a 16.16 product in vc_distort'),
    44: ('vbitplanes', 'high', 'set lane flags from a 16-bit mask (#0x00f0, '
         '#0x0f0f, #0x5555, or r1 in vc_copy8x4); followed by ifnz ops'),
}
# op9 = kind (op9[4:3]: ld, st, ldp, ldq) . w (op9[2:1], as the ALU .w:
# 0 H8, 1 H16, 2 V8 -> 'v' suffix) . 0.  ldp/ldq: the first / second
# 16-byte line of a misaligned access (sim.md "Vector memory").
VMEM = {
    0x00: 'vld8', 0x02: 'vld16', 0x04: 'vld8v',
    0x08: 'vst8', 0x0a: 'vst16', 0x0c: 'vst8v',
    0x10: 'vldp8', 0x12: 'vldp16', 0x14: 'vldp8v',
    0x18: 'vldq8', 0x1a: 'vldq16', 0x1c: 'vldq8v',
}
VSTR = {0x170: 'vstr', 0x178: 'vstr.q'}


def vopname(op9):
    if op9 < 0x20:
        return VMEM.get(op9, 'vmem%02x' % op9)
    if op9 in VSTR:
        return VSTR[op9]
    n = VOP6.get(op9 >> 3)
    if n is None:
        return 'vop%03x' % op9
    return '%s.%d%s' % (n[0], (op9 >> 1) & 3, 'x' if op9 & 1 else '')


def vopconf(op9):
    """Confidence of the operation name of op9 (None if unnamed)."""
    if op9 < 0x20:
        return ('high' if op9 < 0x10 else 'medium') if op9 in VMEM else None
    if op9 in VSTR:
        return 'medium' if op9 == 0x170 else 'low'
    n = VOP6.get(op9 >> 3)
    return n[1] if n else None


def _vopnum(s):
    m = re.fullmatch(r'v(?:op|mem)([0-9a-f]+)', s)
    if m:
        return int(m.group(1), 16)
    for t in (VMEM, VSTR):
        for k, n in t.items():
            if n == s:
                return k
    m = re.fullmatch(r'([a-z][a-z0-9]*)\.(\d)(x?)', s)
    if m:
        for k, n in VOP6.items():
            if n[0] == m.group(1):
                return k << 3 | int(m.group(2)) << 1 | (1 if m.group(3) else 0)
    raise EncodeError('unknown vector op %s' % s)


class VOp:
    """op9 = hw1[8:0], printed by name."""
    pat = r'v[a-z0-9.]+'

    def __init__(self, kind):
        self.kind = kind               # 'alu', 'mem', 'str'
        self.bits = F((0, 8, 0))

    def number(self, i):
        return i.ws[0] & 0x1ff

    def show(self, i):
        return vopname(i.ws[0] & 0x1ff)

    def put(self, ws, addr, s):
        setf(ws, self.bits, _vopnum(s))


def _hx(s):
    return int(s, 16)


class VD:
    """D operand, hw2: d8 [15:8], +r(sss) [7:5] (7 = none), type [4:0]."""
    pat = r'd[0-9a-f]{2}(?:\.[0-9a-f]{2})?(?:\+r[0-6])?'

    def number(self, i):
        h = i.ws[1]
        return {'d8': h >> 8, 'reg': None if (h >> 5) & 7 == 7 else (h >> 5) & 7,
                't5': h & 31}

    def show(self, i):
        h = i.ws[1]
        s = 'd%02x' % (h >> 8)
        if h & 31:
            s += '.%02x' % (h & 31)
        if (h >> 5) & 7 != 7:
            s += '+r%d' % ((h >> 5) & 7)
        return s

    def put(self, ws, addr, s):
        m = re.fullmatch(r'd([0-9a-f]{2})(?:\.([0-9a-f]{2}))?(?:\+r([0-6]))?', s)
        t = _hx(m.group(2)) if m.group(2) else 0
        r = int(m.group(3)) if m.group(3) else 7
        if t > 31:
            raise EncodeError('D type out of range')
        ws[1] = _hx(m.group(1)) << 8 | r << 5 | t


class VA80:
    """A operand, 80-bit: 11 bits hw4[9:4]:hw3[15:11], +r(hw4[3:1]).
    lo=False (memory ops, whose hw3[15:9] is the offset): hw4[9:4] only."""
    def __init__(self, lo=True):
        self.bits = F((3, 9, 4), (2, 15, 11)) if lo else F((3, 9, 4))
        self.nd = 3 if lo else 2
        self.pat = r'a[0-9a-f]{%d}(?:\+r[0-6])?' % self.nd

    def number(self, i):
        r = (i.ws[3] >> 1) & 7
        return {'a': getf(i.ws, self.bits), 'reg': None if r == 7 else r}

    def show(self, i):
        s = 'a%0*x' % (self.nd, getf(i.ws, self.bits))
        r = (i.ws[3] >> 1) & 7
        return s + ('+r%d' % r if r != 7 else '')

    def put(self, ws, addr, s):
        m = re.fullmatch(r'a([0-9a-f]{%d})(?:\+r([0-6]))?' % self.nd, s)
        v = _hx(m.group(1))
        if v >= 1 << width(self.bits):
            raise EncodeError('A out of range')
        setf(ws, self.bits, v)
        setf(ws, F((3, 3, 1)), int(m.group(2)) if m.group(2) else 7)


class VB80:
    """B operand / immediate, 80-bit: hw3[10] = imm; hw3[9:0] + hw4[15:10]."""
    pat = r'#-?(?:0x)?[0-9a-f]+|r[0-7](?:[-+]\d+)?|b[0-9a-f]{2}(?:\.[0-3])?(?:\.t)?(?:\+r[0-6])?(?:\+\+)?'

    def number(self, i):
        h3, h4 = i.ws[2], i.ws[3]
        if h3 & 0x400:
            return {'imm': sx((h4 >> 10) << 10 | (h3 & 0x3ff), 16)}
        if h4 & 0x8000:
            return {'sreg': (h4 >> 12) & 7,
                    'off': sx(((h4 >> 10) & 3) << 10 | (h3 & 0x3ff), 12)}
        r = (h4 >> 12) & 7
        return {'b8': (h3 >> 2) & 0xff, 'type': h3 & 3, 't': (h4 >> 11) & 1,
                'reg': None if r == 7 else r, 'step': (h4 >> 10) & 1}

    def show(self, i):
        h3, h4 = i.ws[2], i.ws[3]
        if h3 & 0x400:
            v = (h4 >> 10) << 10 | (h3 & 0x3ff)
            sv = sx(v, 16)
            mask = (i.ws[0] & 0x1ff) >> 3 == 44           # vbitplanes
            return '#%d' % sv if -1024 <= sv <= 1024 and not mask else '#0x%04x' % v
        if h4 & 0x8000:
            off = sx(((h4 >> 10) & 3) << 10 | (h3 & 0x3ff), 12)
            return 'r%d%s' % ((h4 >> 12) & 7, '%+d' % off if off else '')
        s = 'b%02x' % ((h3 >> 2) & 0xff)
        if h3 & 3:
            s += '.%d' % (h3 & 3)
        if h4 & 0x800:
            s += '.t'
        if (h4 >> 12) & 7 != 7:
            s += '+r%d' % ((h4 >> 12) & 7)
        if h4 & 0x400:
            s += '++'
        return s

    def put(self, ws, addr, s):
        if s.startswith('#'):
            v = parse_int(s[1:])
            if not -0x8000 <= v <= 0xffff:
                raise EncodeError('imm16 out of range')
            v &= 0xffff
            ws[2] = (ws[2] & 0xf800) | 0x400 | (v & 0x3ff)
            ws[3] = (ws[3] & 0x03ff) | (v >> 10) << 10
            return
        m = re.fullmatch(r'r([0-7])([-+]\d+)?', s)
        if m:
            off = int(m.group(2)) if m.group(2) else 0
            if not -2048 <= off <= 2047:
                raise EncodeError('scalar offset out of range')
            off &= 0xfff
            ws[2] = (ws[2] & 0xf800) | (off & 0x3ff)
            ws[3] = (ws[3] & 0x03ff) | 0x8000 | int(m.group(1)) << 12 | (off >> 10) << 10
            return
        m = re.fullmatch(r'b([0-9a-f]{2})(?:\.([0-3]))?(\.t)?(?:\+r([0-6]))?(\+\+)?', s)
        v = _hx(m.group(1)) << 2 | (int(m.group(2)) if m.group(2) else 0)
        r = int(m.group(4)) if m.group(4) else 7
        ws[2] = (ws[2] & 0xf800) | v
        ws[3] = (ws[3] & 0x03ff) | r << 12 | (0x800 if m.group(3) else 0) | \
            (0x400 if m.group(5) else 0)


class VInc:
    """hw4[0]: set iff REP > 1 in 99.7% of ALU ops; operands step per REP."""
    pat = r'(?: inc)?'

    def __init__(self, w=3, b=0):
        self.bits = F((w, b, b))

    def show(self, i):
        return ' inc' if getf(i.ws, self.bits) else ''

    def put(self, ws, addr, s):
        setf(ws, self.bits, 1 if s.strip() else 0)


class VMods:
    """hw5 of 80-bit ops: [15] x15, [14] SETF, [13:11] predication,
    [10] clra, [9] acc, [8] x8, [7:5] REP, [4:3] SRU fn, [2:0] SRU reg
    (7 = none; for memory ops the low bits print as sru too)."""
    pat = (r'(?: rep(?:\d+|r0))?(?: setf)?(?: (?:none|ifz|ifnz|ifn|ifnn|ifc|ifnc))?'
           r'(?: clra)?(?: acc)?(?: x15)?(?: x8)?(?: (?:sru0|sum|min|max) r\d)?')

    def number(self, i):
        h = i.ws[4]
        return {'x15': h >> 15, 'setf': (h >> 14) & 1, 'pred': (h >> 11) & 7,
                'clra': (h >> 10) & 1, 'acc': (h >> 9) & 1, 'x8': (h >> 8) & 1,
                'rep': (h >> 5) & 7, 'sru': (h >> 3) & 3, 'sreg': h & 7}

    def show(self, i):
        h = i.ws[4]
        t = []
        if (h >> 5) & 7:
            t.append(VREP[(h >> 5) & 7])
        if h & 0x4000:
            t.append('setf')
        if (h >> 11) & 7:
            t.append(VPRED[(h >> 11) & 7])
        if h & 0x400:
            t.append('clra')
        if h & 0x200:
            t.append('acc')
        if h & 0x8000:
            t.append('x15')
        if h & 0x100:
            t.append('x8')
        if h & 0x1f != 7:
            t.append('%s r%d' % (VSRU[(h >> 3) & 3], h & 7))
        return ''.join(' ' + x for x in t)

    def put(self, ws, addr, s):
        h = 7
        s = ' ' + s.strip() + ' '
        m = re.search(r' rep(\d+|r0) ', s)
        if m:
            h |= VREP.index('rep' + m.group(1)) << 5
        if ' setf ' in s:
            h |= 0x4000
        for k, n in enumerate(VPRED):
            if n and ' %s ' % n in s:
                h |= k << 11
        if ' clra ' in s:
            h |= 0x400
        if ' acc ' in s:
            h |= 0x200
        if ' x15 ' in s:
            h |= 0x8000
        if ' x8 ' in s:
            h |= 0x100
        m = re.search(r' (sru0|sum|min|max) r(\d) ', s)
        if m:
            h = (h & ~0x1f) | VSRU.index(m.group(1)) << 3 | int(m.group(2))
        ws[4] = h


class VAddr:
    """Memory address, hw3[15:0] (+ hw2[7:0] for 48-bit): (rB+off)+=rS repN.
    80-bit: off7 = hw3[15:9] signed bytes (the left-neighbour loads use
    -2).  48-bit: off15 = hw2[7:0]:hw3[15:9] in units of 8 bytes, printed
    in bytes (proved: the plane prediction's second weight row at +32,
    the chroma deblocking loads at -16).  REP = hw3[8:6]; base =
    r(hw3[5:3]); stride = r(hw3[2:0]), 7 = none."""
    pat = r'\(r[0-7](?:[-+]\d+)?\)(?:\+=r[0-6])?(?: rep(?:\d+|r0))?'

    def __init__(self, hi=None, off=True):
        self.hi = hi                   # extra high offset bits (48-bit: hw2[7:0])
        self.hasoff = off              # False: hw3[15:9] belong to an operand
        self.unit = 8 if hi else 1

    def _off(self, ws):
        if not self.hasoff:
            return 0
        v = ws[2] >> 9
        n = 7
        if self.hi:
            v |= getf(ws, self.hi) << 7
            n += width(self.hi)
        return sx(v, n) * self.unit

    def number(self, i):
        h = i.ws[2]
        return {'base': (h >> 3) & 7, 'off': self._off(i.ws), 'rep': (h >> 6) & 7,
                'stride': None if h & 7 == 7 else h & 7}

    def show(self, i):
        h = i.ws[2]
        off = self._off(i.ws)
        s = '(r%d%s)' % ((h >> 3) & 7, '%+d' % off if off else '')
        if h & 7 != 7:
            s += '+=r%d' % (h & 7)
        if (h >> 6) & 7:
            s += ' ' + VREP[(h >> 6) & 7]
        return s

    def put(self, ws, addr, s):
        m = re.fullmatch(r'\(r([0-7])([-+]\d+)?\)(?:\+=r([0-6]))?(?: rep(\d+|r0))?', s)
        off = int(m.group(2)) if m.group(2) else 0
        if off % self.unit:
            raise EncodeError('offset not a multiple of %d' % self.unit)
        off //= self.unit
        n = 7 + (width(self.hi) if self.hi else 0)
        if not -(1 << (n - 1)) <= off < 1 << (n - 1) or (off and not self.hasoff):
            raise EncodeError('offset out of range')
        off &= (1 << n) - 1
        rep = VREP.index('rep' + m.group(4)) if m.group(4) else 0
        st = int(m.group(3)) if m.group(3) else 7
        keep = ws[2] & 0xfe00 if not self.hasoff else 0
        ws[2] = keep | (off & 0x7f) << 9 | rep << 6 | int(m.group(1)) << 3 | st
        if self.hi:
            setf(ws, self.hi, off >> 7)


class VRaw:
    """A raw sub-field printed as ' name=hex' (meaning not yet known);
    omitted when equal to its usual value."""
    def __init__(self, name, bits, usual=0):
        self.name, self.bits, self.usual = name, bits, usual
        self.pat = r'(?: %s=[0-9a-f]+)?' % name

    def show(self, i):
        v = getf(i.ws, self.bits)
        return '' if v == self.usual else ' %s=%x' % (self.name, v)

    def put(self, ws, addr, s):
        s = s.strip()
        setf(ws, self.bits, int(s.split('=')[1], 16) if s else self.usual)


class VImm6:
    """48-bit immediate form: hw3[9] SETF, [8:6] REP, [5:0] imm6."""
    pat = r'#\d+(?: rep(?:\d+|r0))?(?: setf)?'

    def show(self, i):
        h = i.ws[2]
        s = '#%d' % (h & 63)
        if (h >> 6) & 7:
            s += ' ' + VREP[(h >> 6) & 7]
        if h & 0x200:
            s += ' setf'
        return s

    def put(self, ws, addr, s):
        m = re.fullmatch(r'#(\d+)(?: rep(\d+|r0))?( setf)?', s)
        v = int(m.group(1))
        if v > 63:
            raise EncodeError('imm6 out of range')
        rep = VREP.index('rep' + m.group(2)) if m.group(2) else 0
        ws[2] = (ws[2] & 0xf800) | 0x400 | (0x200 if m.group(3) else 0) | rep << 6 | v


def venc(length, match, syntax, fields, conf, ev, vclass):
    enc('vec%d' % (16 * length), length, match, syntax, fields, conf, ev)
    TABLE[-1].vclass = vclass


class VHex:
    """Fixed-width hex field with a letter prefix (raw VRF coordinate)."""
    def __init__(self, prefix, bits, nd):
        self.prefix, self.bits, self.nd = prefix, bits, nd
        self.pat = r'%s[0-9a-f]{%d}' % (prefix, nd)

    def show(self, i):
        return '%s%0*x' % (self.prefix, self.nd, getf(i.ws, self.bits))

    def put(self, ws, addr, s):
        v = _hx(s[len(self.prefix):])
        if v >= 1 << width(self.bits):
            raise EncodeError('%s out of range' % s)
        setf(ws, self.bits, v)


_A5 = VHex('a', F((2, 15, 11)), 2)


class VB48(VHex):
    """48-bit vector B: hw3[9:2] coordinate, hw3[1:0] type."""
    pat = r'b[0-9a-f]{2}(?:\.[0-3])?'

    def __init__(self):
        self.bits = F((2, 9, 0))

    def show(self, i):
        v = i.ws[2] & 0x3ff
        return 'b%02x%s' % (v >> 2, '.%d' % (v & 3) if v & 3 else '')

    def put(self, ws, addr, s):
        m = re.fullmatch(r'b([0-9a-f]{2})(?:\.([0-3]))?', s)
        setf(ws, self.bits, _hx(m.group(1)) << 2 | int(m.group(2) or 0))


_B10 = VB48()
# memory, 48-bit (op9 < 0x20)
venc(3, [(0, 0xffe0, 0x2200)], '{op} {d}, {m}',
     {'op': VOp('mem'), 'd': VHex('d', F((1, 15, 8)), 2),
      'm': VAddr(hi=F((1, 7, 0)))}, 'medium',
     'contour_threshold: vld16 (r1)+=r5 rep16 / vst8 (r0)+=r4 rep16 with '
     'r0 += 256, r1 += 512 per iteration; alpha_blt_block rep8 rows of 32 '
     'bytes; base/stride registers walk with the loop', 'vmem48')
# ALU, 48-bit: immediate form, vector form
venc(3, [(0, 0xfe00, 0x2200), (2, 0x0400, 0x0400)], '{op} {d}, {a}, {b}',
     {'op': VOp('alu'), 'd': VD(), 'a': _A5, 'b': VImm6()}, 'medium',
     'imm6 = hw3[5:0] (edge-replication copies use #0); hw3[8:6] has REP '
     'codes 0/2/3/4 (REP meaning assumed from the memory form)', 'valu48')
venc(3, [(0, 0xfe00, 0x2200)], '{op} {d}, {a}, {b}',
     {'op': VOp('alu'), 'd': VD(), 'a': _A5, 'b': _B10}, 'medium',
     'expand_range_16s: vmin d01, a1, b000 / vmax d02, a2, b000 keep the '
     'running min/max rows of loaded row 0', 'valu48')
# memory, 80-bit (op9 < 0x20)
venc(5, [(0, 0xffe0, 0x2000)], '{op} {d}, {m}, {a}{bx}{inc}{mods}',
     {'op': VOp('mem'), 'd': VD(), 'm': VAddr(), 'a': VA80(lo=False),
      'bx': VRaw('bx', F((3, 15, 10)), 0x3b), 'inc': VInc(),
      'mods': VMods()}, 'medium',
     'vc_interp16x16_loadall: 16 + 4 + 1 rows (rep16/rep4/rep1) from '
     'r3 = src - 2*stride, r4 = r3 + 16*stride, r2 = r4 + 4*stride, offset '
     '-2 / +14 bytes; D +r0 = alignment', 'vmem80')
# 80-bit stores with an A operand (2170/2178)
venc(5, [(0, 0xfff0, 0x2170), (2, 0x0400, 0)], '{op} {d}, {a}, {m}{x9}{bx}{inc}{mods}',
     {'op': VOp('str'), 'd': VD(), 'a': VA80(),
      'm': VAddr(off=False), 'x9': VRaw('x9', F((2, 9, 9))),
      'bx': VRaw('bx', F((3, 15, 10))), 'inc': VInc(), 'mods': VMods()},
     'medium', 'vc_intra_4x4_storeback: four row stores to r0, r0+s, r0+2s, '
     'r0+3s (base field r0/r4/r3/r5) after vbitplanes #0x000f, ifnz; '
     'vc_storeback: (r1)+=r4 and (r2)+=r4 with r4 = stride/2 (chroma)',
     'vstr80')
# ALU, 80-bit
venc(5, [(0, 0xfe00, 0x2000)], '{op} {d}, {a}, {b}{inc}{mods}',
     {'op': VOp('alu'), 'd': VD(), 'a': VA80(), 'b': VB80(), 'inc': VInc(),
      'mods': VMods()}, 'high',
     'imm16 = hw4[15:10]:hw3[9:0] (6-tap #-5/#20, masks #0x5555/#0x0f0f); '
     'REP = hw5[7:5] (rep16 in 16-row loops, rep8 in 8-row ones); SETF '
     'on vbitplanes; ifnz after it; clra acc / acc on filter taps',
     'valu80')

# --------------------------------------------------------------------------

LEN_ONLY = {'op2ec', 'op24', 'vec48', 'vec80', 'misc4'}


def lenonly(e):
    """Row decodes only the length (raw words / placeholder selector).
    Vector rows keep the names vec48/vec80 (API compatibility) but carry a
    vclass when their fields are decoded."""
    return e.name in LEN_ONLY and not hasattr(e, 'vclass')


# Vector text whose meaning is still open: unnamed operation, or a raw
# sub-field with a non-usual value.
VUNKNOWN = re.compile(r'\bv(?:op|mem)[0-9a-f]|\b(?:x8|x9|x15|sru0)\b|\b(?:bx|x9)=')


def decode(read, addr):
    """Decode at addr; read(a) returns the halfword at a (or None)."""
    h = read(addr)
    if h is None:
        return None
    for e in TABLE:
        if h & e.mask != e.value:
            continue
        ws = [read(addr + 2 * k) for k in range(e.length)]
        if None in ws:
            return None
        if not e.match(ws):
            continue
        ins = I(addr, ws, e)
        e.render(ins)
        return ins
    return None


def decode_words(ws, addr=0):
    return decode(lambda a: ws[(a - addr) // 2] if 0 <= (a - addr) // 2 < len(ws)
                  else None, addr)


def assemble(text, addr=0, length=None):
    """Assemble one instruction.  The first table row (in decode order,
    shortest first when length is None) whose syntax matches, whose
    operands fit and whose result decodes back to the same text wins.
    'mnemonic.16/.32/.48/.80' forces a length."""
    text = ' '.join(text.split())
    m = re.match(r'^(\S+)\.(16|32|48|80)(\s|$)', text)
    if m and length is None:
        length = int(m.group(2)) // 16
        text = text[:m.start(2) - 1] + text[m.end(2):]
    rows = sorted(TABLE, key=lambda e: e.length) if length is None else \
        [e for e in TABLE if e.length == length]
    for e in rows:
        try:
            ws = e.encode(text, addr)
        except (EncodeError, KeyError, ValueError):
            continue
        if ws is None:
            continue
        back = decode_words(ws, addr)
        if back is not None and back.text == text:
            return ws
    raise EncodeError('cannot assemble: %s' % text)


def canonical_text(ins):
    """Disassembly text that assembles back to exactly ins.ws: adds a
    length suffix when the assembler would otherwise pick another form."""
    try:
        if assemble(ins.text, ins.addr) == ins.ws:
            return ins.text
    except EncodeError:
        pass
    mn, _, rest = ins.text.partition(' ')
    return '%s.%d %s' % (mn, 16 * len(ins.ws), rest) if rest else \
        '%s.%d' % (mn, 16 * len(ins.ws))


# --------------------------------------------------------------------------
# ELF (.vll) loading.

Section = collections.namedtuple(
    'Section', 'name type flags addr offset size link info align entsize')
Symbol = collections.namedtuple('Symbol', 'name value size info shndx')
Rela = collections.namedtuple('Rela', 'section offset type sym addend')

SHF_ALLOC = 2
SHT_NOBITS = 8
STT_FUNC = 2
STT_SECTION = 3


class Vll:
    """A VideoCore ELF library (.vll): the one ELF reader shared by the
    disassembler and the simulator.

    sections  [Section]            every section header, by index
    sec       {name: raw tuple}    (older API: the 10 header words)
    symtab    [Symbol]             .dynsym with type/binding and shndx
    rela      [Rela]               every .rela.* entry, symbol by index
    syms      [(value, name, size, info)]   (older API)
    relocs    {offset: (type, name, addend)} for .rela.text (older API)
    funcs     [(value, size, name)]  sized STT_FUNC symbols, sorted
    """
    def __init__(self, path):
        self.path = path
        d = self.data = open(path, 'rb').read()
        if d[:4] != b'\x7fELF' or struct.unpack_from('<H', d, 0x12)[0] != 95:
            raise ValueError('%s: not a VideoCore ELF' % path)
        shoff, = struct.unpack_from('<I', d, 0x20)
        shnum, shstr = struct.unpack_from('<HH', d, 0x30)
        secs = [struct.unpack_from('<10I', d, shoff + i * 40) for i in range(shnum)]
        nm = lambda o: d[secs[shstr][4] + o:].split(b'\0')[0].decode()
        self.sections = [Section(nm(t[0]), *t[1:]) for t in secs]
        self.sec = {nm(t[0]): t for t in secs}
        t = self.sec['.text']
        self.base, self.text = t[3], d[t[4]:t[4] + t[5]]
        ds, dstr = self.sec['.dynsym'], self.sec['.dynstr'][4]
        self.symtab, self.syms, self.funcs = [], [], []
        for i in range(ds[5] // 16):
            n, v, sz, info, _, sh = struct.unpack_from('<IIIBBH', d, ds[4] + i * 16)
            name = d[dstr + n:].split(b'\0')[0].decode()
            self.symtab.append(Symbol(name, v, sz, info, sh))
            self.syms.append((v, name, sz, info))
            if info & 0xf == STT_FUNC and sz > 0:
                self.funcs.append((v, sz, name))
        self.funcs.sort()
        self.rela = []
        for sc in self.sections:
            if not sc.name.startswith('.rela') or not sc.size:
                continue
            for i in range(sc.size // 12):
                off, info, add = struct.unpack_from('<IIi', d, sc.offset + i * 12)
                self.rela.append(Rela(sc.name, off, info & 0xff, info >> 8, add))
        self.relocs = {}
        for r in self.rela:
            if r.section == '.rela.text':
                v, name = self.syms[r.sym][:2]
                self.relocs[r.offset] = (r.type, name or '.sec+0x%x' % v, r.addend)
        self.fstarts = {v for v, _, _ in self.funcs}
        self.fstarts |= {v for v, n, sz, info in self.syms
                         if info & 0xf == STT_FUNC and self.base <= v < self.base + len(self.text)}

    def alloc_sections(self):
        """(addr, bytes) of every SHF_ALLOC section (NOBITS as zeros)."""
        for sc in self.sections:
            if sc.flags & SHF_ALLOC and sc.size:
                yield sc.addr, (bytes(sc.size) if sc.type == SHT_NOBITS else
                                self.data[sc.offset:sc.offset + sc.size])

    def reader(self):
        """read(a) over all allocated sections at their link addresses."""
        spans = list(self.alloc_sections())

        def rd(a):
            for base, b in spans:
                if base <= a and a + 2 <= base + len(b):
                    return struct.unpack_from('<H', b, a - base)[0]
            return None
        return rd

    def inside(self, a):
        import bisect
        if not hasattr(self, '_fv'):
            self._fv = [v for v, _, _ in self.funcs]
        k = bisect.bisect_right(self._fv, a) - 1
        return k >= 0 and a < self.funcs[k][0] + self.funcs[k][1]

    @property
    def allstarts(self):
        if not hasattr(self, '_starts'):
            self._starts = set()
            for v, sz, _ in self.funcs:
                items, _ = sweep(self.read, v, v + sz)
                self._starts |= {it.addr for it in items if not isinstance(it, tuple)}
        return self._starts

    def read(self, a):
        o = a - self.base
        if 0 <= o and o + 2 <= len(self.text):
            return struct.unpack_from('<H', self.text, o)[0]
        return None


def reloc_anchor(off, rtype):
    """Instruction start implied by a relocation at off."""
    return off - 2 if rtype == 11 else off


# --------------------------------------------------------------------------
# Function sweep with switch-table handling.

def sweep(read, start, end):
    """Linear sweep [start, end).  Returns (items, ok) where items are
    I objects or ('data', addr, size, note) tuples."""
    out, a = [], start
    while a < end:
        ins = decode(read, a)
        if ins is None:
            out.append(('data', a, end - a, 'undecodable'))
            return out, False
        out.append(ins)
        a += ins.size
        if ins.e.kind == 'switch' and a < end:
            tbl, a = switch_table(read, ins, a, end, out, start)
            if tbl is None:
                return out, False
    return out, a == end


def switch_bound(ins, prev):
    """Entry count from the range check guarding a switch, if visible:
    addcmpb{hi,gt} r, #0, #u skipping the switch, or addcmpb{ls,le}
    branching to it, both mean entries 0..u."""
    r = ins.ws[0] & 15
    for p in reversed(prev[-4:]):
        if isinstance(p, tuple) or p.e.name != 'addcmpb':
            continue
        h, h2 = p.ws
        if h & 15 != r or h2 >> 14 < 2 or (h >> 4) & 15 not in (0,):
            continue
        c, u = (h >> 8) & 15, (h2 >> 8) & 63
        if c in (8, 12) and p.target != ins.addr:
            return u + 1
        if c in (9, 13) and p.target == ins.addr:
            return u + 1
    # cmp rN, #u ; b{hi,gt} default ; ... ; tb[bh] rN
    prev = [p for p in prev[-5:] if not isinstance(p, tuple)]
    for k in range(len(prev) - 1):
        c, b = prev[k], prev[k + 1]
        if (c.text.startswith('cmp %s, #' % reg(r)) and b.e.kind == 'cbranch'
                and b.text[:3] in ('bhi', 'bgt')):
            return int(c.text.split('#')[1]) + 1
    return None


def switch_table(read, ins, a, end, out, start=0):
    """Table after tbb/tbh at a: entries are halfword offsets from a.
    Size comes from the guarding range check when visible, otherwise the
    table is taken to end where the lowest target begins."""
    byte = ins.e.name == 'tbb'
    n = switch_bound(ins, out[:-1])
    t0, p, targets = a, a, []
    limit = end
    while p < limit and (n is None or len(targets) < n):
        if byte:
            hw = read(p & ~1)
            if hw is None:
                return None, a
            v = (hw >> 8) if p & 1 else (hw & 0xff)
            p += 1
        else:
            v = read(p)
            if v is None:
                return None, a
            p += 2
        if not byte:
            v = sx(v, 16)                 # tbh entries are signed
        tgt = t0 + 2 * v
        if tgt == t0 or (byte and tgt < t0):
            if byte and n is None and p == limit:
                break                     # alignment pad byte
            return None, a
        if n is None and (t0 < tgt < p or not start <= tgt < end):
            p -= 1 if byte else 2         # points into the table: not an entry
            break
        targets.append(tgt)
        if tgt > t0:
            limit = min(limit, tgt)
    if not targets and n is None:
        # table runs past the symbol's end (the symbol is only the head):
        # treat the rest of the function as table
        p = t0
        while p < end:
            v = read(p & ~1)
            if v is None:
                break
            targets.append(t0 + 2 * (v >> 8 if byte and p & 1 else
                                      v & 0xff if byte else sx(v, 16)))
            p += 1 if byte else 2
    p = (p + 1) & ~1
    fwd = [t for t in targets if t > t0]
    if not targets or (fwd and p > min(fwd)):
        return None, a
    ins.table = ('byte' if byte else 'half', t0, targets)
    out.append(('data', t0, p - t0, 'switch table: ' +
                ' '.join('0x%x' % t for t in targets)))
    return targets, p


def disasm_func(v, name, size, f, out=sys.stdout):
    items, ok = sweep(f.read, v, v + size)
    out.write('\n%08x <%s> size %d%s\n' % (v, name, size, '' if ok else '  [!]'))
    for it in items:
        if isinstance(it, tuple):
            _, a, n, note = it
            out.write('  %08x  %-26s .data %d bytes  ; %s\n' % (a, '', n, note))
            continue
        ws = ' '.join('%04x' % w for w in it.ws)
        r = f.relocs.get(it.addr) or f.relocs.get(it.addr + 2)
        cm = ''
        if r:
            cm = '  ; R%d %s%+#x' % r
        elif it.target is not None and it.target in names(f):
            cm = '  ; <%s>' % names(f)[it.target]
        out.write('  %08x  %-26s %s%s\n' % (it.addr, ws, canonical_text(it), cm))


_names = {}


def names(f):
    if f.path not in _names:
        _names[f.path] = {v: n for v, n, sz, info in f.syms if n and info & 0xf == 2}
    return _names[f.path]


# --------------------------------------------------------------------------
# Consistency checking.

GENERIC = re.compile(r'\b(alu|ext|sys|gp|mem)\d|\.hw|\bv(?:op|mem)[0-9a-f]')


def check_func(f, v, size, name):
    """Return (clean, stats) for one function."""
    items, ok = sweep(f.read, v, v + size)
    st = collections.Counter()
    starts = set()
    insts = [it for it in items if not isinstance(it, tuple)]
    for it in insts:
        starts.add(it.addr)
        st['insts'] += 1
        st['bytes'] += it.size
        if not lenonly(it.e) and not GENERIC.search(it.text):
            st['named_bytes'] += it.size
        if it.e.name.startswith('vec'):
            st['vec_bytes'] += it.size
            if hasattr(it.e, 'vclass'):
                st['vec_fields_bytes'] += it.size
                if not GENERIC.search(it.text):
                    st['vec_named_bytes'] += it.size
                    st['vec_named_%s' % vopconf(it.ws[0] & 0x1ff)] += it.size
                    if not VUNKNOWN.search(it.text):
                        st['vec_full_bytes'] += it.size
        if lenonly(it.e):
            st['unknown_insts'] += 1
            st['unknown_bytes'] += it.size
            st['vec_raw_bytes' if it.e.name.startswith('vec') else 'other_unk_bytes'] += it.size
    for it in items:
        if isinstance(it, tuple):
            st['data_bytes'] += it[2]
    problems = []
    if not ok:
        problems.append('sweep')
    # relocations must land on instruction starts
    for off, (rt, _, _) in f.relocs.items():
        if v <= off < v + size:
            st['relocs'] += 1
            if reloc_anchor(off, rt) not in starts:
                problems.append('reloc@%x' % off)
            else:
                st['relocs_ok'] += 1
    for it in insts:
        k = it.e.kind
        if it.target is None:
            continue
        if k == 'call':
            st['calls'] += 1
            if it.addr in f.relocs or it.target in f.fstarts:
                st['calls_ok'] += 1
            elif f.read(it.target) is not None and (
                    it.target in f.allstarts or not f.inside(it.target)):
                st['calls_ok'] += 1       # static function or alternate entry
                st['calls_local'] += 1
            else:
                problems.append('call@%x->%x' % (it.addr, it.target))
        elif k == 'cbranch':
            st['branches'] += 1
            if v <= it.target < v + size:
                if it.target in starts:
                    st['branches_ok'] += 1
                else:
                    problems.append('br@%x->%x' % (it.addr, it.target))
            elif it.target in f.fstarts or it.target in f.allstarts:
                st['branches_ok'] += 1    # tail branch / shared code
            else:
                problems.append('brout@%x->%x' % (it.addr, it.target))
    clean = not problems and st['unknown_insts'] == 0
    return clean, st, problems


def check(dirpath, verbose=False):
    files = sorted(glob.glob(os.path.join(dirpath, '*.vll')))
    if not files:
        sys.exit('no .vll files in %s' % dirpath)
    tot = collections.Counter()
    vrows = []
    print('%-16s %6s %6s %6s %8s %8s %7s %7s %7s' % (
        'file', 'funcs', 'consis', 'clean', 'bytes', 'decoded', 'known%',
        'vec%', 'probs'))
    for path in files:
        f = Vll(path)
        c = collections.Counter()
        allprobs = []
        for v, sz, name in f.funcs:
            clean, st, probs = check_func(f, v, sz, name)
            c.update(st)
            c['funcs'] += 1
            c['fbytes'] += sz
            if not probs:
                c['consistent'] += 1
                c['consistent_bytes'] += sz
            if clean:
                c['clean'] += 1
                c['clean_bytes'] += sz
            allprobs += ['%s:%s' % (name, p) for p in probs]
        tot.update(c)
        vrows.append((os.path.basename(path), c))
        known = c['bytes'] - c['unknown_bytes']
        print('%-16s %6d %6d %6d %8d %8d %6.1f%% %6.1f%% %7d' % (
            os.path.basename(path), c['funcs'], c['consistent'], c['clean'],
            c['fbytes'], c['bytes'] + c['data_bytes'],
            100.0 * known / max(1, c['fbytes']),
            100.0 * c['vec_bytes'] / max(1, c['fbytes']), len(allprobs)))
        if verbose:
            for p in allprobs[:40]:
                print('    ', p)
    c = tot
    known = c['bytes'] - c['unknown_bytes']
    print('%-16s %6d %6d %6d %8d %8d %6.1f%% %6.1f%%' % (
        'TOTAL', c['funcs'], c['consistent'], c['clean'], c['fbytes'],
        c['bytes'] + c['data_bytes'], 100.0 * known / max(1, c['fbytes']),
        100.0 * c['vec_bytes'] / max(1, c['fbytes'])))
    print()
    print('functions swept exactly with all relocs/branches/calls consistent: '
          '%d/%d (%.1f%%), %d/%d bytes (%.1f%%)' % (
              c['consistent'], c['funcs'], 100.0 * c['consistent'] / c['funcs'],
              c['consistent_bytes'], c['fbytes'],
              100.0 * c['consistent_bytes'] / c['fbytes']))
    print('...and with every instruction identified (clean): %d/%d (%.1f%%), '
          '%d bytes (%.1f%%)' % (c['clean'], c['funcs'],
                                 100.0 * c['clean'] / c['funcs'], c['clean_bytes'],
                                 100.0 * c['clean_bytes'] / c['fbytes']))
    print('bytes in identified instructions: %.1f%%; in vector-unit '
          'instructions: %.1f%% (length only: %.1f%%); other unidentified: '
          '%.1f%%; switch-table data: %.1f%%' % (
              100.0 * known / c['fbytes'], 100.0 * c['vec_bytes'] / c['fbytes'],
              100.0 * c['vec_raw_bytes'] / c['fbytes'],
              100.0 * c['other_unk_bytes'] / c['fbytes'],
              100.0 * c['data_bytes'] / c['fbytes']))
    print('bytes whose operation is named (no aluN/extN/sysN/gpN/memN '
          'placeholders): %.1f%%' % (100.0 * c['named_bytes'] / c['fbytes']))
    print('relocations at decoded instruction starts: %d/%d; calls resolved: '
          '%d/%d; branches resolved: %d/%d' % (
              c['relocs_ok'], c['relocs'], c['calls_ok'], c['calls'],
              c['branches_ok'], c['branches']))
    print()
    print('vector unit, % of vector bytes: fields = every bit in a decoded '
          'field; named = operation named; full = named and no open sub-field')
    print('%-16s %8s %7s %7s %7s   %s' % ('file', 'vbytes', 'fields', 'named',
                                         'full', 'named by confidence h/m/l'))
    for name, v in vrows + [('TOTAL', c)]:
        n = max(1, v['vec_bytes'])
        print('%-16s %8d %6.1f%% %6.1f%% %6.1f%%   %5.1f%% %5.1f%% %5.1f%%' % (
            name, v['vec_bytes'], 100.0 * v['vec_fields_bytes'] / n,
            100.0 * v['vec_named_bytes'] / n, 100.0 * v['vec_full_bytes'] / n,
            100.0 * v['vec_named_high'] / n, 100.0 * v['vec_named_medium'] / n,
            100.0 * v['vec_named_low'] / n))


def unknown(dirpath, n=40):
    tally = collections.Counter()
    for path in sorted(glob.glob(os.path.join(dirpath, '*.vll'))):
        f = Vll(path)
        for v, sz, name in f.funcs:
            items, ok = sweep(f.read, v, v + sz)
            for it in items:
                if not isinstance(it, tuple) and lenonly(it.e):
                    tally['%s %04x' % (it.e.name, it.ws[0] & 0xfff0)] += 1
    for k, c in tally.most_common(n):
        print('%6d  %s' % (c, k))


def trace(path, base, roots, seed_prologues=True):
    """Recursive-descent disassembly of a raw image (no symbols).  Roots
    are followed through calls and branches; with seed_prologues, every
    push-with-lr that directly follows a return is also taken as a
    function start (function pointers are otherwise invisible)."""
    data = open(path, 'rb').read()
    rd = lambda a: (struct.unpack_from('<H', data, a - base)[0]
                    if 0 <= a - base <= len(data) - 2 else None)
    roots = list(roots)
    if seed_prologues:
        for a in range(base + 2, base + len(data) - 2, 2):
            h, p = rd(a), rd(a - 2)
            if ((h & 0xfff0 == 0xf0a0 and h & 15 < 10) or h == 0xf0ef) and \
                    (p == 0x4430 or p & 0xfff0 == 0xe0a0 or p == 0xe0ef or p == 0):
                roots.append(a)
    seen, funcs, work = {}, set(roots), list(roots)
    st = collections.Counter()
    while work:
        a = work.pop()
        while a not in seen:
            ins = decode(rd, a)
            if ins is None:
                st['undecodable'] += 1
                break
            if any(a + k in seen for k in range(2, ins.size, 2)):
                st['overlaps'] += 1
            seen[a] = ins
            k = ins.e.kind
            if ins.target is not None and k in ('call', 'cbranch') and \
                    rd(ins.target) is not None:
                work.append(ins.target)
                if k == 'call':
                    funcs.add(ins.target)
            if k == 'ret' or ins.text == 'rts' or (
                    ins.text.startswith('pop') and 'pc' in ins.text):
                break
            if k == 'cbranch' and ins.text.startswith('b '):
                break
            if k == 'switch':
                tg, _ = switch_table(rd, ins, a + 2, base + len(data), [])
                if tg:
                    work += [t for t in tg if rd(t) is not None]
                else:
                    st['bad_switch'] += 1
                break
            a += ins.size
    cov = sum(i.size for i in seen.values())
    named = sum(i.size for i in seen.values()
                if not lenonly(i.e) and not GENERIC.search(i.text))
    vec = sum(i.size for i in seen.values() if i.e.name.startswith('vec'))
    tgt = collections.Counter(rd(f) for f in funcs if f not in roots)
    pro = sum(n for h, n in tgt.items() if h is not None and h >> 8 == 0xf0)
    print('%s @0x%x: %d roots, %d functions, %d instructions, %d/%d bytes '
          'reached (%.1f%%); named %.1f%%, vector %.1f%% of reached; %s' % (
              os.path.basename(path), base, len(roots), len(funcs), len(seen),
              cov, len(data), 100.0 * cov / len(data),
              100.0 * named / max(1, cov), 100.0 * vec / max(1, cov),
              dict(st) or 'no undecodable/overlapping instructions'))
    print('call targets not seeded: %d, of which %d start with a push' % (
        sum(tgt.values()), pro))
    return seen


def roundtrip_insts(insts, label):
    """Disassemble -> assemble -> compare, for every instruction."""
    st = collections.Counter()
    fails = collections.Counter()
    for it in insts:
        st['insts'] += 1
        try:
            ws = assemble(it.text, it.addr)
        except EncodeError:
            ws = None
        if ws == it.ws:
            st['plain'] += 1
            continue
        txt = canonical_text(it)
        try:
            ws = assemble(txt, it.addr)
        except EncodeError:
            ws = None
        if ws == it.ws:
            st['suffixed'] += 1
            st['suffixed_' + it.e.name] += 1
        else:
            st['fail'] += 1
            fails['%s: %s' % (it.e.name, it.text.split(' ')[0])] += 1
    print('%s: %d instructions; %d reassemble byte-identical from the plain '
          'text, %d need a length suffix (non-minimal encoding chosen by the '
          'compiler), %d fail' % (label, st['insts'], st['plain'],
                                  st['suffixed'], st['fail']))
    suf = sorted(((k[9:], v) for k, v in st.items() if k.startswith('suffixed_')),
                 key=lambda x: -x[1])
    if suf:
        print('   suffixed by class:', ', '.join('%s %d' % kv for kv in suf))
    for k, v in fails.most_common(10):
        print('   FAIL %5d  %s' % (v, k))
    return st


def roundtrip(dirpath, raws=()):
    insts = []
    for path in sorted(glob.glob(os.path.join(dirpath, '*.vll'))):
        f = Vll(path)
        for v, sz, name in f.funcs:
            items, _ = sweep(f.read, v, v + sz)
            insts += [it for it in items if not isinstance(it, tuple)]
    tot = roundtrip_insts(insts, '.vll functions')
    for spec in raws:
        path, base, roots = spec
        seen = trace(path, base, roots)
        tot.update(roundtrip_insts(list(seen.values()), os.path.basename(path)))
    return tot['fail'] == 0


def print_table():
    for e in TABLE:
        m = ' '.join('w%d&%04x=%04x' % mt for mt in e.matches)
        print('%-12s %2d  %-30s %-28s %-6s %s' % (e.name, e.length * 16, m,
                                                  e.syntax, e.conf, e.ev))


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    if argv[1] == '--check':
        check(argv[2], '-v' in argv)
    elif argv[1] == '--table':
        print_table()
    elif argv[1] == '--unknown':
        unknown(argv[2], int(argv[3]) if len(argv) > 3 else 40)
    elif argv[1] == '--roundtrip':
        raws = []
        args = argv[3:]
        while len(args) >= 3 and args[0].endswith('.bin'):
            raws.append((args[0], int(args[1], 0), [int(args[2], 0)]))
            args = args[3:]
        ok = roundtrip(argv[2], raws)
        sys.exit(0 if ok else 1)
    elif argv[1] == '--asm':
        addr = int(argv[2], 0) if len(argv) > 2 else 0
        for line in sys.stdin:
            line = line.split(';')[0].strip()
            if not line:
                continue
            ws = assemble(line, addr)
            print('%08x  %-26s %s' % (addr, ' '.join('%04x' % w for w in ws), line))
            addr += 2 * len(ws)
    elif argv[1] == '--trace':
        roots = [int(x, 0) for x in argv[4:]]
        trace(argv[2], int(argv[3], 0), roots)
    elif argv[1] == '--raw':
        data = open(argv[2], 'rb').read()
        if data[:4] == b'\x7fELF':
            # A .vll: sections are mapped at their link addresses, so the
            # arguments are [START [END]] link addresses (default .text);
            # function mode (vc2dis.py FILE.vll FUNC) is usually better.
            f = Vll(argv[2])
            rd = f.reader()
            s = int(argv[3], 0) if len(argv) > 3 else f.base
            e = int(argv[4], 0) if len(argv) > 4 else f.base + len(f.text)
            sys.stderr.write('%s: ELF, sections at their link addresses '
                             '(.text 0x%x-0x%x)\n' % (argv[2], f.base, f.base + len(f.text)))
        else:
            base = int(argv[3], 0)
            s = int(argv[4], 0) if len(argv) > 4 else base
            e = int(argv[5], 0) if len(argv) > 5 else base + len(data)
            rd = lambda a: (struct.unpack_from('<H', data, a - base)[0]
                            if 0 <= a - base <= len(data) - 2 else None)
        a = s
        while a < e:
            ins = decode(rd, a)
            if ins is None:
                print('  %08x  %04x  ???' % (a, rd(a) or 0))
                a += 2
                continue
            print('  %08x  %-26s %s' % (a, ' '.join('%04x' % w for w in ins.ws),
                                        ins.text))
            a += ins.size
    else:
        f = Vll(argv[1])
        want = set(argv[2:])
        for v, sz, name in f.funcs:
            if not want or name in want:
                disasm_func(v, name, sz, f)


if __name__ == '__main__':
    main(sys.argv)
