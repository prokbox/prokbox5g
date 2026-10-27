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
"""A small VideoCore II vector-unit assembler front end for gen_vpu.py.

Operands are written as VRF positions, H8(y, x), H16(y, x), V8(y, x),
V16(y, x), with optional per-REP stepping and a +rN offset register; this
module turns them into vc2dis's raw field syntax (d8 coordinates, the
11-bit A field with its t5 block bits, B coordinates), assembles each
instruction, decodes it again with the simulator's operand model
(vc2vpu) and checks that the decoded operands are the ones asked for.
So an encoding mistake fails at generation time, not as a wrong pixel.

Only the operand forms below are produced (docs/research/bcm2722/isa.md,
"VRF operands"):
  D  H8/H16/V8/V16 at a d8 coordinate (x of an H operand and y of a V
     operand are multiples of 16; a 16-bit D never sits in x block 1/3).
  A  80-bit: H8 (mode 4, any y/x), V8 (mode 0), V16 (mode 2, column 0-15
     of a 32-column block), H16 (mode 2/3, a row of a 16-row block);
     t5 bit 3 (x += 16, unproven) is never used.
  B  a vector at a d8 coordinate, an immediate, or a scalar register
     with offset 0.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'tools', 'bcmre'))
import vc2dis as D    # noqa: E402
import vc2sim         # noqa: E402,F401  (vc2vpu reads its HYP table)
import vc2vpu as VP   # noqa: E402

KINDS = {'H8': (False, 1), 'H16': (False, 2), 'V8': (True, 1), 'V16': (True, 2)}
WCODE = {'H8': 0, 'H16': 1, 'V8': 2, 'V16': 3}


class Opd:
    """A VRF operand: kind, (y, x) of element 0, step per REP, +rN."""
    def __init__(self, kind, y, x, step=False, reg=None):
        assert kind in KINDS and 0 <= y < 64 and 0 <= x < 64
        self.kind, self.y, self.x, self.step, self.reg = kind, y, x, step, reg

    @property
    def vert(self):
        return KINDS[self.kind][0]

    @property
    def width(self):
        return KINDS[self.kind][1]

    def stepped(self, on=True):
        return Opd(self.kind, self.y, self.x, on, self.reg)

    def r(self, reg):
        return Opd(self.kind, self.y, self.x, self.step, reg)

    def __repr__(self):
        return '%s(%d,%d)%s%s' % (self.kind, self.y, self.x,
                                  '++' if self.step else '',
                                  '+r%d' % self.reg if self.reg is not None else '')


def H8(y, x, step=False, reg=None):
    return Opd('H8', y, x, step, reg)


def H16(y, x, step=False, reg=None):
    return Opd('H16', y, x, step, reg)


def V8(y, x, step=False, reg=None):
    return Opd('V8', y, x, step, reg)


def V16(y, x, step=False, reg=None):
    return Opd('V16', y, x, step, reg)


DISCARD = 'discard'       # 16-bit D in x block 1: the result is not written


class Reg:
    """B = scalar register (offset 0)."""
    def __init__(self, n):
        assert 0 <= n <= 7
        self.n = n


def d8(o):
    if o.vert:
        assert o.y % 16 == 0, o
        c = o.x & 15
    else:
        assert o.x % 16 == 0, o
        c = o.y & 15
    return (o.y >> 4) << 6 | (o.x >> 4) << 4 | c


def _rtext(reg):
    return '' if reg is None else '+r%d' % reg


def enc_d(o):
    """D text without t5 (added by the caller) and its .w code."""
    if o == DISCARD:
        return 'd10', 1, None
    if o.width == 2 and (o.x >> 4) & 1 and o.reg is None:
        raise ValueError('16-bit D in x block 1/3 discards: %r' % o)
    return 'd%02x' % d8(o), WCODE[o.kind], o.reg


def enc_a(o):
    """(a11, t5) of an 80-bit A operand (None: A unused, H8 at 0,0)."""
    if o is None:
        return 0, 0
    by, bx = o.y & 48, (32 if o.x >= 32 else 0)
    t5 = (1 if bx else 0) | (2 if by & 16 else 0) | (4 if by & 32 else 0)
    dy, dx = o.y - by, o.x - bx
    if o.kind == 'H8':
        a11 = 0x400 | dy << 6 | dx
        if o.step:
            t5 |= 16
    elif o.kind == 'V8':
        assert dy == 0, o
        a11 = (0x20 if o.step else 0) | dx
        t5 |= 16
    elif o.kind == 'V16':
        assert dy == 0 and dx < 16, o
        a11 = 0x200 | (0x20 if o.step else 0) | dx
        t5 |= 16
    else:                                   # H16 row
        assert dx == 0, o
        a11 = 0x200 | (0x100 if o.step else 0) | (dy & 3) << 6 | (dy >> 2) << 2
    return a11, t5


def enc_b(b, dwidth):
    if isinstance(b, int):
        return '#%d' % b if -1024 <= b <= 1024 else '#0x%04x' % (b & 0xffff)
    if isinstance(b, Reg):
        return 'r%d' % b.n
    s = 'b%02x' % d8(b)
    if b.vert:
        s += '.1'
    if b.width == 2:
        s += '.t'
    s += _rtext(b.reg)
    if b.step:
        s += '++'
    return s


REPS = {1: '', 2: 'rep2', 4: 'rep4', 8: 'rep8', 16: 'rep16', 32: 'rep32',
        64: 'rep64'}


def _same(want, got, what, text):
    """Compare an Opd with a decoded vc2vpu.Opnd."""
    if want is None:
        return
    base = want.y << 6 | want.x
    stp = (1 if want.vert else 64) if want.step else 0
    ok = (got.base == base and got.vert == want.vert and got.width == want.width
          and got.reg == want.reg and got.step == stp)
    if not ok:
        raise AssertionError('%s decodes as %r, wanted %r: %s' % (what, got, want, text))


def _decode(text):
    ws = D.assemble(text, 0)
    ins = D.decode_words(ws, 0)
    if ins is None or ins.text != text:
        raise AssertionError('round trip: %s -> %s' % (text, ins and ins.text))
    return ins


def alu(op, d, a, b, rep=1, setf=False, pred=None, clra=False, acc=False,
        sum_reg=None, x15=False):
    """One 80-bit vector ALU instruction; returns its text."""
    dtext, w, dreg = enc_d(d)
    a11, t5 = enc_a(a)
    if t5:
        dtext += '.%02x' % t5
    dtext += _rtext(dreg)
    atext = 'a%03x' % a11 + _rtext(a.reg if a is not None else None)
    inc = d != DISCARD and d.step
    mods = [REPS[rep]] if rep > 1 else []
    if setf:
        mods.append('setf')
    if pred:
        mods.append(pred)
    if clra:
        mods.append('clra')
    if acc:
        mods.append('acc')
    if x15:
        mods.append('x15')      # 32-bit accumulate, low half (isa.md)
    if sum_reg is not None:
        mods.append('sum r%d' % sum_reg)
    text = '%s.%d %s, %s, %s%s%s' % (op, w, dtext, atext, enc_b(b, d),
                                     ' inc' if inc else '',
                                     ''.join(' ' + m for m in mods if m))
    ins = _decode(text)
    vi = VP.VInsn(ins)
    if d != DISCARD:
        _same(d, vi.d, 'D', text)
    else:
        assert vi.d.discard, text
    if a is not None:
        _same(a, vi.a, 'A', text)
    if isinstance(b, Opd):
        assert vi.b[0] == 'vec', text
        _same(b, vi.b[1], 'B', text)
    elif isinstance(b, int):
        assert vi.b[0] == 'imm' and vi.b[1] & 0xffff == b & 0xffff, text
    else:
        assert vi.b == ('reg', b.n, 0), text
    assert vi.rep == rep, text
    return text


MEMOPS = {'vld8': 0x00, 'vld16': 0x02, 'vld8v': 0x04, 'vst8': 0x08,
          'vst16': 0x0a, 'vldp8': 0x10, 'vldq8': 0x18}


def st16_fields(d, off):
    """(text d8, text offset) that make a 48-bit vst16 store H16 row d to
    r(base) + off under the silicon layout (device session 5; vc2vpu
    st48_swap): hw2[15:8] = off / 32, hw2[4:0] = d's t5 block bits, hw3's
    offset field = d's row in its block << 2.  In vc2dis's load-style
    text that is 'd<off/32>' and an offset of t5 * 1024 + 32 * row."""
    assert d.kind == 'H16' and d.reg is None and d.x in (0, 16, 32, 48), d
    assert off % 32 == 0 and 0 <= off <= 255 * 32, off
    t5 = (1 if d.x & 32 else 0) | (8 if d.x & 16 else 0) | \
        (2 if d.y & 16 else 0) | (4 if d.y & 32 else 0)
    return off // 32, t5 * 1024 + 32 * (d.y & 15)


def mem(op, d, base, off=0, stride=None, rep=1, unproven=False, raw=False):
    """48-bit vector load/store: D is an H8/H16/V8 position (element 0),
    stepping one row (H) or column (V) per REP; the address is
    r(base) + off (bytes, a multiple of 8) and advances by r(stride).

    Stores do not lay their fields out like loads (device sessions 4-5,
    isa.md "Vector stores on silicon"): a vst16 is encoded with
    st16_fields, so off is a multiple of 32 (0..8160).  Rows outside
    block 0 (y >= 16 or x != 0) use t5 bits that only Apple's code shows
    (unproven=True to emit them).  vst8's layout is not device-checked:
    only d00 at offset 0.  raw=True emits the load-style fields as given
    (the bench's old-encoding control)."""
    assert d.reg is None
    if base == 7:
        # silicon reads a base field of 7 as no register (sessions 13-14)
        raise ValueError('%s with base r7: reads as address 0 on silicon' % op)
    want = {'vld16': 'H16', 'vld8v': 'V8', 'vst16': 'H16'}.get(op, 'H8')
    assert d.kind == want, (op, d)
    reps = ' ' + REPS[rep] if rep > 1 else ''
    strd = '+=r%d' % stride if stride is not None else ''
    if op == 'vst16' and not raw:
        if (d.y >= 16 or d.x) and not unproven:
            raise ValueError('vst16 from %r needs t5 block bits, unproven on silicon' % d)
        dd, toff = st16_fields(d, off)
        text = '%s d%02x, (r%d%s)%s%s' % (op, dd, base, '%+d' % toff if toff else '',
                                          strd, reps)
        ins = _decode(text)
        assert VP.st48_swap(ins.ws) == (d.y, d.x, off), text
        op9, _, _, rp, b, s = VP.mem48_fields(ins.ws)
        assert op9 == MEMOPS[op] and rp == rep and b == base and s == stride, text
        return text
    if op.startswith('vst') and (d8(d) or off) and not raw:
        raise ValueError('48-bit %s from d%02x at offset %d: layout unproven on '
                         'silicon' % (op, d8(d), off))
    text = '%s d%02x, (r%d%s)%s%s' % (op, d8(d), base, '%+d' % off if off else '',
                                      strd, reps)
    ins = _decode(text)
    op9, dd, o, rp, b, s = VP.mem48_fields(ins.ws)
    assert op9 == MEMOPS[op] and dd == d8(d) and o * 8 == off and rp == rep \
        and b == base and s == stride, text
    return text


def vstr(a, base, stride=None, rep=1, pred='ifnz'):
    """Store the enabled byte lanes of A (an H8 position stepping per REP)
    to r(base), advancing by r(stride): vc_storeback's form."""
    a11, t5 = enc_a(a)
    text = 'vstr d00.%02x+r0, a%03x%s, (r%d)%s%s %s sru0 r0' % (
        t5, a11, _rtext(a.reg), base,
        '+=r%d' % stride if stride is not None else '',
        ' ' + REPS[rep] if rep > 1 else '', pred)
    ins = _decode(text)
    ws = ins.ws
    got = VP.decode_a80(((ws[3] >> 4) & 63) << 5 | ws[2] >> 11,
                        VP._reg3((ws[3] >> 1) & 7), ws[1] & 31)
    _same(a, got, 'A', text)
    return text


def bitplanes(src):
    """vbitplanes: Z flag = lane bit clear (src: a register or an imm16)."""
    if isinstance(src, int):
        return 'vbitplanes.1 d10, a000, #0x%04x setf' % src
    return 'vbitplanes.1 d10, a000, r%d setf' % src.n


# ---- the generators' output (gen_jdec, gen_jobs, gen_viz) ---------------

def out_paths(argv, in_tree, mutate, prog):
    """The generator's output paths: in_tree (its files in the source tree),
    or with --out DIR the same names in DIR.  A mutant (--mutate) goes only
    to --out: the tracked kernels are read by every bench that builds an
    image, several at once in a gate (scripts/bcmfw_benches.sh BCM_JOBS),
    and a mutant written there was built into another bench's image (a
    decode_image strict-alignment trap from jidct_test's puty, 2026-10-26).
    Exits 2 on a mutant without --out."""
    if '--out' not in argv:
        if mutate:
            print('%s: --mutate needs --out DIR (a mutant never goes into the source '
                  'tree, which concurrent benches build from)' % prog)
            sys.exit(2)
        return list(in_tree)
    out = argv[argv.index('--out') + 1]
    os.makedirs(out, exist_ok=True)
    return [os.path.join(out, os.path.basename(p)) for p in in_tree]


def write_text(path, text):
    """path's contents become text: unchanged files are not touched, and
    the new contents land whole (a temporary file renamed over it), so a
    concurrent build reading it never sees a truncated or empty file.
    True if written"""
    try:
        with open(path) as f:
            if f.read() == text:
                return False
    except FileNotFoundError:
        pass
    tmp = '%s.%d.tmp' % (path, os.getpid())
    with open(tmp, 'w') as f:
        f.write(text)
    os.replace(tmp, path)
    return True
