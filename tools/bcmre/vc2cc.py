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
"""C for VideoCore II: VC IV GCC as the code generator, our own back end.

VideoCore II (BCM2722) has VideoCore IV semantics with a different
encoding (docs/research/bcm2722/isa.md).  This tool runs an unmodified
VC IV GCC (vc4-elf-gcc, puppeh/gcc-vc4) to get assembly text, translates
each VC IV instruction into one or more VC II instructions (vc2dis.py
syntax), and assembles the whole program at once into a flat image: no
object files, no relocations, no linker.  Static symbols are scoped per
input file.  Branches and pc-relative operands are relaxed iteratively.

The GCC flags keep generated code in r0-r14 (+ sp, and lr only for
call/return): r15 and r22 are the translator's scratch registers, r16-r21
stay free for hand-written kernels (as in Apple's MetaWare code), r23 is
GCC's static chain (unused: no nested functions), lr is never used as a
temporary.  So compares and most ALU ops have a 16-bit VC II form.
docs/research/bcm2722/compiler.md has the background, the gap list, the
ABI notes and the toolchain build.

Usage:
  vc2cc.py [-O2] [-o IMAGE.bin] [--base ADDR] [--entry ADDR] [--map]
           [--mapfile FILE.map] [--no-float] [-I DIR] [-D NAME[=V]] FILE.c|FILE.s ...
        compile (C) or take (.s) inputs, write a flat VC II image
  vc2cc.py FILE.c ... --run FUNC [ARG ...] [--trace]
        build at --base and call FUNC in vc2sim (r0, r1 printed)
  vc2cc.py --list FILE.c ...      VC II listing of the built image
  vc2cc.py --stats FILE.s ...     translate only; count untranslatable ops

Inline assembly: a line starting with '!' is passed through as one native
VC II instruction in vc2dis syntax (vector ops, sleep, ei/di ...), e.g.
  asm volatile("! msb %0, %1" : "=r"(n) : "r"(x));

Native VC II assembly: a .s file is VC IV assembly (GCC's output, libgcc)
unless it says '.syntax vc2'.  From there on every instruction is VC II in
vc2dis syntax and is assembled as written (shortest encoding, branches
relaxed).  Symbols may appear as a whole operand ('b loop', 'bl main',
'ld r0, counter' 48-bit pc-relative) or as an immediate ('mov r0, #sym',
'#sym+4'); not inside a memory operand.  Vector instructions pass through
untouched.  Comments start with ';' or '//'.  '.rept N' ... '.endr'
repeats lines.

Linker-style symbols: __S_start and __S_end (word-rounded) for each
section S (vectors, text, rodata, data, bss).  A .S file is native
assembly run through the C preprocessor first (same -I/-D).

Firmware layout: section '.vectors' (if any) is placed at --base, then
padded with zeros up to --entry; '.text' starts at --entry and must begin
with '_start'.  Then .rodata, .data and .bss as before.  The image file
holds everything up to the end of .data (.bss is zeroed by crt0).

Fixed-base links (overlays): build(imports=Imports(...)) links a second
program at its own --base (e.g. a free region of the resident image's
map) against the resident image's symbols.  Only the names on the
import list resolve into the resident image, by their addresses in its
link map (read_map(), or the resident Program's symbols); any other
undefined name is an error, and library code is not pulled in for an
imported name.  The resident build names the same list as its exports
(build(exports=(names, version))).  Both programs then see the symbol
__vc2_abi: the CRC-32 of the version and every listed name with its
address in the resident image (abi_hash()).  The overlay stores it in
its header and the resident checks it at load: an overlay linked
against another build of the resident (any export moved) or another
version of the interface is refused.  Command line: --import-map MAP
--imports a,b,c --abi-version N (and --exports a,b,c on the resident).

--conservative: avoid the instruction kinds no Apple code exercises in
the vc2sim self-test (abs, not, addscale5-7, adds16, post-increment ldh,
48-bit pc-relative ld/st): each becomes a sequence of proven kinds.

--no-float: firmware is integer-only.  Any reference to a soft-float
libgcc routine (or a VC IV float instruction, always a gap) is an error.

Environment: VC4_GCC (default ~/Git/re/vc4-prefix/bin/vc4-elf-gcc).
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vc2dis as D  # noqa: E402

GCC = os.environ.get('VC4_GCC', os.path.expanduser(
    '~/Git/re/vc4-prefix/bin/vc4-elf-gcc'))
AT = 'r15'                  # low scratch register (16-bit forms reach it)
AT2 = 'r22'                 # second scratch (r23 is GCC's static chain)
FIXED = ['r15'] + ['r%d' % n for n in range(16, 24)] + ['lr']
GCC_FLAGS = ['-ffreestanding', '-fno-common', '-fno-builtin'] + \
    ['-ffixed-%s' % r for r in FIXED]
DEFAULT_BASE = 0x200000

INV = {'eq': 'ne', 'ne': 'eq', 'cs': 'cc', 'cc': 'cs', 'mi': 'pl', 'pl': 'mi',
       'vs': 'vc', 'vc': 'vs', 'hi': 'ls', 'ls': 'hi', 'ge': 'lt', 'lt': 'ge',
       'gt': 'le', 'le': 'gt'}
# VC IV condition spellings -> VC II (vc2dis) names.  lo/hs are VC IV's
# cs/cc: both chips' cmp sets C as a borrow (sim.md).
CONDS = {'eq': 'eq', 'ne': 'ne', 'cs': 'cs', 'lo': 'cs', 'cc': 'cc',
         'hs': 'cc', 'mi': 'mi', 'pl': 'pl', 'vs': 'vs', 'vc': 'vc',
         'hi': 'hi', 'ls': 'ls', 'ge': 'ge', 'lt': 'lt', 'gt': 'gt',
         'le': 'le'}
# VC IV ALU mnemonic -> VC II name (same semantics: rd = ra op b).
ALU2 = {'mov': 'mov', 'add': 'add', 'sub': 'sub', 'rsub': 'rsub',
        'and': 'and', 'or': 'or', 'eor': 'eor', 'bic': 'bic', 'shl': 'lsl',
        'lsr': 'lsr', 'asr': 'asr', 'mul': 'mul', 'max': 'max', 'min': 'min',
        'bmask': 'bmask', 'mulhd.ss': 'mulhd.ss', 'mulhd.su': 'mulhd.su',
        'mulhd.us': 'mulhd.us', 'mulhd.uu': 'mulhd.uu', 'div.ss': 'div.ss',
        'div.su': 'div.su', 'div.us': 'div.us', 'div.uu': 'div.uu'}
UNARY = {'neg': 'neg', 'not': 'not', 'abs': 'abs', 'msb': 'msb'}
COMMUTE = {'add', 'and', 'or', 'eor', 'mul', 'max', 'min'}
FLAGS = {'cmp': 'cmp', 'cmn': 'cmn', 'btest': 'btst'}
WIDTH = {'ld': 4, 'st': 4, 'ldh': 2, 'sth': 2, 'ldsh': 2, 'ldb': 1, 'stb': 1,
         'ldsb': 1}
REG = re.compile(r'^(r\d+|gp|sp|lr|pc)$')
REGNUM = {'gp': 24, 'sp': 25, 'lr': 26, 'pc': 31}


ABI_SYM = '__vc2_abi'


def abi_hash(version, names, symbols):
    """The ABI word of a fixed-base link: CRC-32 of the interface
    version and each exported name with its address, in list order."""
    import zlib
    text = 'vc2abi %d\n' % version + ''.join(
        '%s=%08x\n' % (n, symbols[n] & 0xFFFFFFFF) for n in names)
    return zlib.crc32(text.encode())


def read_map(path):
    """{symbol: address} from a map file (Program.write_map)."""
    out = {}
    with open(path) as f:
        for line in f:
            m = re.match(r'^([0-9a-f]{8}) (\S+)$', line.rstrip('\n'))
            if m:
                out.setdefault(m.group(2), int(m.group(1), 16))
    return out


# the symbols layout defines (Program.layout): __S_start, __S_end, __xdata_load
LINKER_SYM = re.compile(r'^__(vectors|text|rodata|data|bss|xdata|sdbss)_(start|end)$|'
                        r'^__xdata_load$')


class Imports:
    """What an overlay may use of a resident image: names (in a fixed
    order, the interface) and their addresses in the resident image's
    symbols (a Program's .symbols or read_map()).  abi: the word both
    programs see as __vc2_abi."""
    def __init__(self, symbols, names, version):
        missing = [n for n in names if n not in symbols]
        if missing:
            raise Gap('imports: the resident image has no %s' % ', '.join(missing))
        if len(set(names)) != len(names) or ABI_SYM in names:
            raise Gap('imports: a name listed twice, or %s' % ABI_SYM)
        self.names, self.version = list(names), version
        self.addr = {n: symbols[n] for n in names}
        self.abi = abi_hash(version, self.names, self.addr)


class Gap(Exception):
    """A VC IV construct with no VC II translation."""


def regno(r):
    return REGNUM[r] if r in REGNUM else int(r[1:])


def u32(v):
    return v & 0xffffffff


def s32(v):
    v &= 0xffffffff
    return v - (1 << 32) if v >> 31 else v


def is_int(s):
    return re.match(r'^[-+]?(0x[0-9a-fA-F]+|\d+)$', s) is not None


# ---------------------------------------------------------------------------
# Expressions: numbers, symbols, + - * and parentheses, evaluated late
# against the symbol table.

IDENT = re.compile(r'0[xX][0-9a-fA-F]+|\d+|[A-Za-z_.$][\w.$]*')


def eval_expr(expr, lookup):
    out, pos = [], 0
    for m in IDENT.finditer(expr):
        out.append(expr[pos:m.start()])
        t = m.group()
        out.append('(%d)' % (int(t, 0) if t[0].isdigit() else lookup(t)))
        pos = m.end()
    out.append(expr[pos:])
    text = ''.join(out)
    if not re.match(r'^[\s\d()+\-*/xa-fA-F<>&|]*$', text):
        raise ValueError('bad expression %r' % expr)
    return int(eval(text, {'__builtins__': {}}))  # noqa: S307 (digits only)


# ---------------------------------------------------------------------------
# Items of a section.  Each has size(addr) under the current layout and
# emit(addr) -> bytes.

class Label:
    def __init__(self, name):
        self.name = name


class Insn:
    """One VC IV instruction: a list of alternative VC II sequences.  Each
    line is a template; '{T:expr}' renders as an absolute hex address
    (branch targets, pc-relative operands), '{I:expr}' as a decimal
    number.  The first alternative whose lines all assemble wins; sizes
    only grow between relaxation passes."""
    def __init__(self, src, alts, unit):
        self.src, self.alts, self.unit = src, alts, unit
        self.choice = 0
        self.lens = None
        self.words = None

    def render(self, line, lookup):
        def sub(m):
            v = eval_expr(m.group(2), lookup)
            return '0x%x' % u32(v) if m.group(1) == 'T' else '%d' % v
        return re.sub(r'\{([TI]):([^}]*)\}', sub, line)

    def layout(self, addr, lookup):
        for k in range(self.choice, len(self.alts)):
            try:
                ws_all, lens, a = [], [], addr
                for j, line in enumerate(self.alts[k]):
                    text = self.render(line, lookup)
                    lo = self.lens[j] if (k == self.choice and self.lens) else 1
                    ws = assemble_min(text, a, lo)
                    ws_all.append(ws)
                    lens.append(len(ws))
                    a += 2 * len(ws)
            except D.EncodeError:
                continue
            if k != self.choice:
                self.choice, self.lens = k, None
            self.lens = lens
            self.words = [w for ws in ws_all for w in ws]
            return 2 * len(self.words)
        raise Gap('cannot encode %r: %s' % (self.src, ' | '.join(
            '; '.join(a) for a in self.alts)))

    def emit(self):
        return b''.join(w.to_bytes(2, 'little') for w in self.words)


_ASM_CACHE = {}


def assemble_min(text, addr, lo=1):
    """Shortest encoding of text that is at least lo halfwords long."""
    key = (text, addr, lo)
    if key in _ASM_CACHE:
        return _ASM_CACHE[key]
    texts = [text]
    # mov32 and a few rows print immediates in hex
    m = re.search(r'#(-?\d+)\b', text)
    if m:
        texts.append(text[:m.start(1)] + '0x%x' % u32(int(m.group(1))) +
                     text[m.end(1):])
    for n in range(lo, 6):
        for t in texts:
            try:
                ws = D.assemble(t, addr, n)
            except D.EncodeError:
                continue
            _ASM_CACHE[key] = ws
            return ws
    raise D.EncodeError('cannot assemble: %s' % text)


class Data:
    """Data directive: kind 'b'/'h'/'w' with expressions, or raw bytes."""
    def __init__(self, kind, vals, unit, raw=None):
        self.kind, self.vals, self.unit, self.raw = kind, vals, unit, raw
        self.n = {'b': 1, 'h': 2, 'w': 4}.get(kind, 1)

    def layout(self, addr, lookup):
        return len(self.raw) if self.raw is not None else self.n * len(self.vals)

    def emit_at(self, lookup):
        if self.raw is not None:
            return self.raw
        return b''.join((eval_expr(v, lookup) & ((1 << 8 * self.n) - 1))
                        .to_bytes(self.n, 'little') for v in self.vals)


class Align:
    def __init__(self, n, fill=0):
        self.n, self.fill = n, fill

    def layout(self, addr, lookup):
        return (-addr) % self.n


class Space:
    def __init__(self, n):
        self.n = n

    def layout(self, addr, lookup):
        return self.n


class Switch:
    """VC IV 'switch rN' + '.case Lx-Lbase' entries -> tbb/tbh followed by
    a table of halfword offsets from the end of the tbb/tbh (sim: nxt +
    2*entry).  GCC puts Lbase right after 'switch', so the table (a
    CaseTable item after that label) starts at nxt."""
    def __init__(self, reg, unit):
        self.reg, self.unit = reg, unit
        self.cases = []          # expressions (Lx - Lbase)
        self.half = False

    def layout(self, addr, lookup):
        return 2

    def emit_at(self, lookup):
        op = D.assemble('%s %s' % ('tbh' if self.half else 'tbb', self.reg), 0)
        return op[0].to_bytes(2, 'little')


class CaseTable:
    def __init__(self, switch):
        self.sw, self.unit = switch, switch.unit

    def entries(self, lookup):
        return [eval_expr(c, lookup) for c in self.sw.cases]

    def layout(self, addr, lookup):
        es = self.entries(lookup)
        if any(e < 0 or e > 510 or e & 1 for e in es):
            self.sw.half = True
        n = len(es) * (2 if self.sw.half else 1)
        return n + (n & 1)

    def emit_at(self, lookup):
        out = b''
        for e in self.entries(lookup):
            if e & 1:
                raise Gap('odd case offset')
            e //= 2
            out += (e & 0xffff).to_bytes(2, 'little') if self.sw.half else bytes([e])
        if len(out) & 1:
            out += b'\0'
        return out


# ---------------------------------------------------------------------------
# VC IV operand parsing and instruction translation.

def split_ops(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
            continue
        depth += ch == '('
        depth -= ch == ')'
        cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def imm_val(op):
    """('num', v) for '#123', ('sym', expr) for '#sym+4', else None."""
    if not op.startswith('#'):
        return None
    e = op[1:]
    if is_int(e):
        return ('num', int(e, 0))
    return ('sym', e)


class Translator:
    def __init__(self, conservative=False, place=None):
        # conservative: avoid instruction kinds whose VC II semantics rest
        # on VC IV analogy only (never executed by Apple's code in the
        # vc2sim self-test): abs, not, addscale5-7/adds16, post-increment
        # ldh/ldsh/sth... except the proven ones, and 48-bit pc-relative
        # loads/stores.  Costs a few bytes; used for first-light images.
        self.conservative = conservative
        # place: {object name: 'xdata' | 'sdbss'} moves those data objects
        # out of their section (Program's far placement, build(place=))
        self.place = dict(place or {})
        self.placed = set()
        self.sections = {'vectors': [], 'text': [], 'rodata': [], 'data': [],
                         'bss': [], 'xdata': [], 'sdbss': []}
        self.globals = set()
        self.local_defs = {}      # unit -> set of names defined there
        self.skip = 0
        self.gaps = {}
        self.refs = set()         # (unit, name) referenced by operands/data
        self.native_labels = set()  # labels defined in native (.syntax vc2) code

    def note_refs(self, unit, exprs):
        for e in exprs:
            for t in IDENT.findall(e):
                if not t[0].isdigit():
                    self.refs.add((unit, t))

    def undefined(self):
        gdefs = {n for u, ds in self.local_defs.items() for n in ds
                 if n in self.globals}
        return {n for u, n in self.refs
                if n not in self.local_defs.get(u, ()) and n not in gdefs}

    # -- helpers ------------------------------------------------------------

    def new_label(self):
        self.skip += 1
        return '.Lvc2_%d' % self.skip

    def tgt(self, expr):
        return '{T:%s}' % expr

    def load_imm(self, reg, imm):
        """Alternatives that put an immediate (num or sym) in reg."""
        kind, v = imm
        if kind == 'num':
            return [['mov %s, #%d' % (reg, s32(v))]]
        return [['mov %s, #{T:%s}' % (reg, v)]]

    def alu(self, op, d, a, b):
        """rd = ra op b (b: register or immediate operand string)."""
        alts = []
        im = imm_val(b)
        if im is None:
            if d == a:
                alts.append(['%s %s, %s' % (op, d, b)])
            if op in COMMUTE and d == b:
                alts.append(['%s %s, %s' % (op, d, a)])
            alts.append(['%s %s, %s, %s' % (op, d, a, b)])
            if d != a and d != b and regno(d) < 16 and regno(b) < 16 and \
                    op in ALU2.values():
                alts.append(['mov %s, %s' % (d, a), '%s %s, %s' % (op, d, b)])
            return alts
        kind, v = im
        if kind == 'sym':
            if op == 'mov':
                return self.load_imm(d, im)
            alts = [['mov %s, #{T:%s}' % (AT, v), '%s %s, %s, %s' % (op, d, a, AT)]]
            if d == a and regno(d) < 16:
                alts.insert(0, ['mov %s, #{T:%s}' % (AT, v), '%s %s, %s' % (op, d, AT)])
            return alts
        v = s32(v)
        if op == 'mov':
            return [['mov %s, #%d' % (d, v)]]
        if op == 'sub':
            op, v = 'add', -v
        if d == a:
            alts.append(['%s %s, #%d' % (op, d, v)])
        if 0 <= v < 32:
            alts.append(['%s %s, %s, #%d' % (op, d, a, v)])
        if op == 'add':
            if a == 'sp' and v >= 0:
                alts.append(['lea %s, (sp+%d)' % (d, v)])
            alts.append(['add %s, %s, #%d' % (d, a, v)])
            alts.append(['lea %s, (%s%+d)' % (d, a, v)])
        if op == 'and':
            if v > 0 and (v + 1) & v == 0:
                n = (v + 1).bit_length() - 1
                if d == a:
                    alts.append(['bmask %s, #%d' % (d, n)])
                alts.append(['bmask %s, %s, #%d' % (d, a, n)])
            if 0 <= ~v < 32:
                alts.append(['bic %s, %s, #%d' % (d, a, ~v)])
        if op in ('lsl', 'lsr', 'asr') and not 0 <= v < 32:
            raise Gap('shift count %d' % v)
        if op == 'rsub':
            # rd = v - ra
            alts.append(['neg %s, %s' % (d, a), 'add %s, #%d' % (d, v)])
        alts.append(['mov %s, #%d' % (AT, v), '%s %s, %s, %s' % (op, d, a, AT)])
        if d == a and regno(d) < 16:
            alts.append(['mov %s, #%d' % (AT, v), '%s %s, %s' % (op, d, AT)])
        return alts

    def flags(self, op, a, b):
        im = imm_val(b)
        if im is None:
            return [['%s %s, %s' % (op, a, b)], ['%s %s, %s, %s' % (op, a, a, b)]]
        if im[0] != 'num':
            return [['mov %s, #{T:%s}' % (AT, im[1]), '%s %s, %s' % (op, a, AT)]]
        v = s32(im[1])
        alts = [['%s %s, #%d' % (op, a, v)]]
        if op == 'cmp' and v == 0:
            alts.append(['cmp %s, #0' % a])
        if op == 'cmn':
            alts.append(['mov %s, #%d' % (AT, v), 'cmn %s, %s' % (a, AT)])
        else:
            alts.append(['mov %s, #%d' % (AT, v), '%s %s, %s' % (op, a, AT)])
        return alts

    def mem(self, op, d, m):
        """Load/store; m is the VC IV memory operand text."""
        if op == 'ldsh':
            return self.mem_ldsh(d, m)
        if op == 'ldh':
            return self.mem_ldh(d, m)
        return self.mem_any(op, d, m)

    @staticmethod
    def ldh_proven(ins):
        """True unless ins is an ldh that would assemble to a halfword load
        never checked on silicon with a value >= 0x8000: the gp/sp-relative
        (2dxx) and 48-bit forms.  The 16-bit 'ldh rd, (rs)', '(rs)++',
        '(rs+ri)' and the 32-bit 0x28xx '(rs+off)' (r0-r15, s16 offset)
        zero-extend on silicon (quirks.md #16, #33; bench row HWLD)."""
        mm = re.match(r'^ldh (\w+), (.*)$', ins)
        if not mm:
            return True
        d, m = mm.group(1), mm.group(2)
        if regno(d) >= 16:
            return False
        mm = re.match(r'^\((\w+)\)(\+\+)?$', m) or \
            re.match(r'^\((\w+)\+(r\d+)\)$', m)
        if mm:
            return all(regno(r) < 16 for r in mm.groups() if r and r != '++')
        mm = re.match(r'^\((\w+)([+-]\d+)\)$', m)
        if mm:
            o = int(mm.group(2))
            return regno(mm.group(1)) < 16 and -0x8000 <= o < 0x8000
        return False

    def mem_ldh(self, d, m):
        """ldh in the forms proven to zero-extend (ldh_proven); any other
        operand computes its address into r15 first."""
        if regno(d) >= 16:
            raise Gap('ldh into %s' % d)
        alts = [alt for alt in self.mem_any('ldh', d, m)
                if all(self.ldh_proven(i) for i in alt)]
        if alts:
            return alts
        mm = re.match(r'^\((\w+)\+(-?\d+)\)$', m)
        if mm:
            return [alt + ['ldh %s, (%s)' % (d, AT)]
                    for alt in self.alu('add', AT, mm.group(1), '#%s' % mm.group(2))]
        mm = re.match(r'^\((\w+)\)(\+\+)?$', m)
        if mm:
            b = mm.group(1)
            tail = [] if not mm.group(2) else ['add %s, #2' % b]
            return [['mov %s, %s' % (AT, b), 'ldh %s, (%s)' % (d, AT)] + tail]
        if '(' not in m:
            return [['mov %s, #{T:%s}' % (AT, m), 'ldh %s, (%s)' % (d, AT)]]
        raise Gap('ldh operand %s' % m)

    def mem_ldsh(self, d, m):
        """ldsh only as 'ldsh rd, (rs)': on silicon the forms with an offset
        load zero-extended, as ldh does (device session 17: a transform's
        negative coefficients read as 65513 and saturated; isa.md).  The
        address goes to r15 first for every other operand."""
        mm = re.match(r'^\((\w+)\)$', m)
        if mm:
            return [['ldsh %s, (%s)' % (d, mm.group(1))]]
        mm = re.match(r'^\((\w+)\+(-?\d+)\)$', m)
        if mm:
            b, o = mm.group(1), int(mm.group(2))
            if o == 0:
                return [['ldsh %s, (%s)' % (d, b)]]
            return [alt + ['ldsh %s, (%s)' % (d, AT)]
                    for alt in self.alu('add', AT, b, '#%d' % o)]
        mm = re.match(r'^\((\w+)\+\+\)$', m)
        if mm:
            b = mm.group(1)
            return [['ldsh %s, (%s)' % (d, b), 'add %s, #2' % b],
                    ['ldsh %s, (%s)' % (d, b), 'add %s, %s, #2' % (b, b)]]
        alts = [alt for alt in self.mem_any('ldsh', d, m)
                if all(not ins.startswith('ldsh') or
                       re.search(r'\(\w+\)$', ins) for ins in alt)]
        if not alts:
            raise Gap('ldsh operand %s' % m)
        return alts

    def mem_any(self, op, d, m):
        """Load/store; m is the VC IV memory operand text."""
        w = WIDTH[op]
        mm = re.match(r'^\((\w+)\)$', m)
        if mm:
            b = mm.group(1)
            return [['%s %s, (%s)' % (op, d, b)], ['%s %s, (%s+0)' % (op, d, b)]]
        mm = re.match(r'^\((\w+)\+(-?\d+)\)$', m)
        if mm:
            b, o = mm.group(1), int(mm.group(2))
            alts = [['%s %s, (%s%+d)' % (op, d, b, o)]]
            if o == 0:
                alts.insert(0, ['%s %s, (%s)' % (op, d, b)])
            return alts
        mm = re.match(r'^\((\w+)\+\+\)$', m)
        if mm:
            b = mm.group(1)
            if self.conservative and op not in ('ld', 'ldb', 'ldsh', 'st', 'stb', 'sth'):
                return [['%s %s, (%s)' % (op, d, b), 'add %s, #%d' % (b, w)],
                        ['%s %s, (%s+0)' % (op, d, b), 'add %s, %s, #%d' % (b, b, w)]]
            return [['%s %s, (%s)++' % (op, d, b)],
                    ['%s %s, (%s+0)' % (op, d, b), 'add %s, #%d' % (b, w)],
                    ['%s %s, (%s+0)' % (op, d, b), 'add %s, %s, #%d' % (b, b, w)]]
        mm = re.match(r'^\(--(\w+)\)$', m)
        if mm:
            # VC II has (--rs) (vc2dis ldst_predec), but only ldb/stb occur
            # in Apple's code and no width is device-verified: add + access
            b = mm.group(1)
            return [['add %s, #%d' % (b, -w), '%s %s, (%s)' % (op, d, b)],
                    ['add %s, #%d' % (b, -w), '%s %s, (%s+0)' % (op, d, b)]]
        mm = re.match(r'^\((\w+)\+(\w+)(?:<<(\d))?\)$', m)
        if mm:
            b, x, sh = mm.group(1), mm.group(2), int(mm.group(3) or 0)
            alts = []
            if 1 << sh == w:
                alts.append(['%s %s, (%s+%s)' % (op, d, b, x)])
            for add in ('add %s, %s' % (AT, b), 'add %s, %s, %s' % (AT, AT, b)):
                alts.append(['lsl %s, %s, #%d' % (AT, x, sh) if sh else
                             'mov %s, %s' % (AT, x), add,
                             '%s %s, (%s)' % (op, d, AT)])
            return alts
        if '(' not in m:
            # absolute address: 48-bit pc-relative form
            alts = [['%s %s, {T:%s}' % (op, d, m)],
                    ['mov %s, #{T:%s}' % (AT, m), '%s %s, (%s)' % (op, d, AT)]]
            return alts[::-1] if self.conservative else alts
        raise Gap('memory operand %s' % m)

    def translate(self, mn, ops, unit):
        """VC IV mnemonic (size suffix removed) + operands -> alternatives."""
        cond = None
        m = re.match(r'^(\w+(?:\.\w\w)?)\.(eq|ne|cs|cc|lo|hs|mi|pl|vs|vc|hi|'
                     r'ls|ge|lt|gt|le)$', mn)
        if m and m.group(1) not in ('b', 'bl', 'j'):
            mn, cond = m.group(1), CONDS[m.group(2)]
        if cond:
            body = self.translate(mn, ops, unit)
            skip = self.new_label()
            # conditional execution -> branch around (ALU ops do not touch
            # the flags on VC II, so a run of them keeps its condition)
            return [['b%s {T:%s}' % (INV[cond], skip)] + alt + ['@label ' + skip]
                    for alt in body]
        if mn == 'rts' or mn == 'nop':
            return [[mn]]
        if mn in ('b', 'j') and len(ops) == 1:
            if REG.match(ops[0]):
                return [['add sp, #-4', 'st %s, (sp+0)' % ops[0], 'pop pc']]
            return [['b {T:%s}' % ops[0]],
                    ['mov %s, #{T:%s}' % (AT, ops[0]), 'add sp, #-4',
                     'st %s, (sp+0)' % AT, 'pop pc']]
        if mn == 'bl':
            if REG.match(ops[0]):
                return [['bl %s' % ops[0]]]
            return [['bl {T:%s}' % ops[0]]]
        m = re.match(r'^b(eq|ne|cs|cc|lo|hs|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)$', mn)
        if m:
            c = CONDS[m.group(1)]
            if len(ops) == 1:
                return [['b%s {T:%s}' % (c, ops[0])]]
            a, b, t = ops
            im = imm_val(b)
            alts = []
            if im is None:
                alts.append(['addcmpb%s %s, #0, %s, {T:%s}' % (c, a, b, t)])
            elif im[0] == 'num' and 0 <= im[1] < 64:
                alts.append(['addcmpb%s %s, #0, #%d, {T:%s}' % (c, a, im[1], t)])
            for f in self.flags('cmp', a, b):
                alts.append(f + ['b%s {T:%s}' % (c, t)])
            return alts
        m = re.match(r'^addcmpb(eq|ne|cs|cc|lo|hs|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)$', mn)
        if m:
            c = CONDS[m.group(1)]
            d, a, b, t = ops
            alts = [['addcmpb%s %s, %s, %s, {T:%s}' % (c, d, a, b, t)]]
            for add in self.alu('add', d, d, a):
                for f in self.flags('cmp', d, b):
                    alts.append(add + f + ['b%s {T:%s}' % (c, t)])
            return alts
        if mn in ('stm', 'ldm'):
            regs, base = ops[:-1], ops[-1]
            if base not in ('(--sp)', '(sp++)'):
                raise Gap('%s with base %s' % (mn, base))
            regs = [r for r in regs]
            return [['%s %s' % ('push' if mn == 'stm' else 'pop', ', '.join(regs))]]
        if mn in WIDTH:
            return self.mem(mn, ops[0], ops[1])
        if mn in FLAGS:
            return self.flags(FLAGS[mn], ops[0], ops[1])
        if mn in UNARY:
            d, a = ops
            im = imm_val(a)
            if im is not None:
                if im[0] != 'num':
                    raise Gap('%s of a symbol' % mn)
                v = s32(im[1])
                v = {'neg': -v, 'not': ~v, 'abs': abs(v),
                     'msb': v.bit_length() - 1 if v > 0 else -1}[mn]
                return [['mov %s, #%d' % (d, v)]]
            if self.conservative and mn == 'abs':
                # |a| = (a ^ s) - s with s = a >> 31 (arithmetic)
                pre = [] if d == a else ['mov %s, %s' % (d, a)]
                return [pre + ['asr %s, %s, #31' % (AT, d), 'eor %s, %s' % (d, AT),
                               'sub %s, %s' % (d, AT)]]
            if self.conservative and mn == 'not':
                pre = [] if d == a else ['mov %s, %s' % (d, a)]
                return [pre + ['mov %s, #-1' % AT, 'eor %s, %s' % (d, AT)]]
            return [['%s %s, %s' % (UNARY[mn], d, a)]]
        if mn == 'count':
            # population count: no VC II op is known; SWAR expansion
            d, a = ops
            seq = [] if d == a else ['mov %s, %s' % (d, a)]
            seq += ['lsr %s, %s, #1' % (AT2, d), 'mov %s, #%d' % (AT, 0x55555555),
                    'and %s, %s, %s' % (AT2, AT2, AT), 'sub %s, %s, %s' % (d, d, AT2),
                    'mov %s, #%d' % (AT, 0x33333333), 'and %s, %s, %s' % (AT2, d, AT),
                    'lsr %s, %s, #2' % (d, d), 'and %s, %s, %s' % (d, d, AT),
                    'add %s, %s, %s' % (d, d, AT2), 'lsr %s, %s, #4' % (AT2, d),
                    'add %s, %s, %s' % (d, d, AT2), 'mov %s, #%d' % (AT, 0x0f0f0f0f),
                    'and %s, %s, %s' % (d, d, AT), 'mov %s, #%d' % (AT, 0x01010101),
                    'mul %s, %s, %s' % (d, d, AT), 'lsr %s, %s, #24' % (d, d)]
            return [seq]
        if mn == 'signext':
            d, a, n = (ops if len(ops) == 3 else (ops[0], ops[0], ops[1]))
            n = int(n.lstrip('#'))
            s = 31 - n
            if s == 0:
                return [['mov %s, %s' % (d, a)]] if d != a else [[]]
            return [self.alu('lsl', d, a, '#%d' % s)[0] +
                    self.alu('asr', d, d, '#%d' % s)[0],
                    ['lsl %s, %s, #%d' % (d, a, s), 'asr %s, %s, #%d' % (d, d, s)]]
        if mn in ('bitset', 'bitclear', 'bitflip'):
            d, a, b = (ops if len(ops) == 3 else (ops[0], ops[0], ops[1]))
            vop = {'bitset': 'or', 'bitclear': 'bic', 'bitflip': 'eor'}[mn]
            im = imm_val(b)
            if im is not None:
                return self.alu(vop, d, a, '#%d' % s32(1 << im[1]))
            return [['mov %s, #1' % AT, 'lsl %s, %s' % (AT, b)] +
                    alt for alt in self.alu(vop, d, a, AT)]
        if mn == 'ror':
            d, a, b = (ops if len(ops) == 3 else (ops[0], ops[0], ops[1]))
            im = imm_val(b)
            if im is not None:
                n = im[1] & 31
                return [['lsr %s, %s, #%d' % (AT, a, n),
                         'lsl %s, %s, #%d' % (AT2, a, (32 - n) & 31),
                         'or %s, %s, %s' % (d, AT, AT2)]]
            return [['neg %s, %s' % (AT, b), 'lsl %s, %s, %s' % (AT, a, AT),
                     'lsr %s, %s, %s' % (AT2, a, b), 'or %s, %s, %s' % (d, AT, AT2)]]
        m = re.match(r'^(add|sub)scale$', mn)
        if m:
            if len(ops) == 2:
                d, a, rest = ops[0], ops[0], ops[1]
            else:
                d, a, rest = ops
            mm = re.match(r'^(\w+)<<(\d+)$', rest)
            b, n = mm.group(1), int(mm.group(2))
            alts = []
            if m.group(1) == 'add' and not (self.conservative and n not in (1, 2, 3, 8)):
                name = {1: 'adds2', 2: 'adds4', 3: 'adds8', 4: 'adds16'}.get(n) \
                    or 'addscale%d' % n
                if d == a:
                    alts.append(['%s %s, %s' % (name, d, b)])
                alts.append(['%s %s, %s, %s' % (name, d, a, b)])
            alts.append(['lsl %s, %s, #%d' % (AT, b, n)] +
                        self.alu(m.group(1), d, a, AT)[-1])
            return alts
        if mn in ALU2:
            if len(ops) == 2:
                return self.alu(ALU2[mn], ops[0], ops[0], ops[1])
            return self.alu(ALU2[mn], ops[0], ops[1], ops[2])
        raise Gap('no VC II translation for %s' % mn)

    # -- file parsing -------------------------------------------------------

    def native(self, line, unit):
        """One native VC II instruction (vc2dis syntax) -> Insn.  Whole
        operands and immediates that name symbols become {T:expr}."""
        parts = line.split(None, 1)
        mn, rest = parts[0], (parts[1].strip() if len(parts) > 1 else '')
        if mn.startswith('v') or not rest:
            return Insn(line, [[line]], unit)
        ops = []
        for op in split_ops(rest):
            if REG.match(op) or re.match(r'^r\d+-r\d+$', op):
                ops.append(op)
                continue
            body = op[1:] if op.startswith('#') else op
            names = [t for t in IDENT.findall(body) if not t[0].isdigit()]
            if op.startswith('#') and not names:
                # a number or constant expression (e.g. from a #define):
                # fold it to decimal, the spelling vc2dis.assemble reads
                ops.append('#%d' % s32(eval_expr(body, lambda n: 0)))
                continue
            if not names or op.startswith('('):
                if names and op.startswith('(') and any(
                        not REG.match(n) for n in names):
                    raise Gap('symbol inside a memory operand: %s' % line)
                ops.append(op)
                continue
            self.note_refs(unit, [body])
            ops.append(('#' if op.startswith('#') else '') + '{T:%s}' % body)
        return Insn(line, [['%s %s' % (mn, ', '.join(ops))]], unit)

    @staticmethod
    def expand_rept(lines):
        """'.rept N' ... '.endr' (not nested) -> the lines N times."""
        out, k = [], 0
        while k < len(lines):
            m = re.match(r'^\s*\.rept\s+(\S+)', lines[k])
            if not m:
                out.append(lines[k])
                k += 1
                continue
            j = k + 1
            while j < len(lines) and not re.match(r'^\s*\.endr\b', lines[j]):
                j += 1
            if j == len(lines):
                raise Gap('.rept without .endr')
            out += lines[k + 1:j] * int(m.group(1), 0)
            k = j + 1
        return out

    # sources a far section may take objects from
    FAR_FROM = {'xdata': ('rodata', 'data'), 'sdbss': ('bss',)}

    def place_label(self, name, sec, base):
        """The section a data label goes to: its placement, or back to the
        directive's section after a placed object.  The alignment just
        before the label is the object's own, so it moves with it."""
        want = self.place.get(name)
        if want is not None:
            if base not in self.FAR_FROM[want]:
                raise Gap('placement: %s is in .%s, %s takes only %s' % (
                    name, base, want, '/'.join(self.FAR_FROM[want])))
            self.placed.add(name)
        elif sec in self.FAR_FROM:
            want = base
        if want is None or want == sec:
            return sec
        out = self.sections
        moved = []
        while out[sec] and isinstance(out[sec][-1], Align):
            moved.insert(0, out[sec].pop())
        out[want].extend(moved)
        return want

    def parse(self, text, unit):
        sec = 'text'
        base = sec                # the section the directives chose
        out = self.sections
        defs = self.local_defs.setdefault(unit, set())
        switch = None
        native = False
        lines = text.splitlines()
        if '.rept' in text:
            lines = self.expand_rept(lines)
        for ln, raw in enumerate(lines, 1):
            if raw.lstrip().startswith(';'):
                continue
            line = re.sub(r';.*$', '', raw) if '"' not in raw else raw
            if native:
                line = re.sub(r'//.*$', '', line)
            line = line.strip()
            if not line:
                continue
            if line.startswith('.syntax'):
                native = line.split()[1] == 'vc2'
                continue
            while True:
                m = re.match(r'^([A-Za-z_.$][\w.$]*):\s*(.*)$', line)
                if not m:
                    break
                defs.add(m.group(1))
                if native:
                    self.native_labels.add(m.group(1))
                elif self.place:
                    sec = self.place_label(m.group(1), sec, base)
                out[sec].append(Label((unit, m.group(1))))
                line = m.group(2)
            if not line:
                continue
            if line.startswith('!'):
                # native VC II instruction (inline asm), vc2dis syntax
                out[sec].append(Insn(line, [[line[1:].strip()]], unit))
                continue
            parts = line.split(None, 1)
            word, rest = parts[0], (parts[1].strip() if len(parts) > 1 else '')
            if word.startswith('.'):
                if word == '.case':
                    if not switch.cases:
                        out[sec].append(CaseTable(switch))
                    switch.cases.append(rest)
                    self.note_refs(unit, [rest])
                    continue
                if word == '.endswitch':
                    switch = None
                    continue
                new = self.directive(word, rest, sec, unit, defs)
                if new:
                    sec = base = new
                continue
            if sec in ('bss', 'sdbss', 'xdata'):
                raise Gap('code in .%s' % sec)
            if native:
                try:
                    out[sec].append(self.native(line, unit))
                except Gap as e:
                    self.gaps[str(e)] = self.gaps.get(str(e), 0) + 1
                continue
            mn = re.sub(r'\.[sml]$', '', word)
            ops = split_ops(rest)
            if mn == 'switch':
                switch = Switch(ops[0], unit)
                out[sec].append(switch)
                continue
            try:
                alts = self.translate(mn, ops, unit)
            except Gap as e:
                self.gaps[str(e)] = self.gaps.get(str(e), 0) + 1
                alts = None
            if alts is None:
                out[sec].append(Gapped('%s %s' % (word, rest)))
                continue
            # '@label' pseudo-lines split an alternative into pieces with
            # labels between them (cond-exec skips)
            if any(l.startswith('@label') for a in alts for l in a):
                lab = [l for l in alts[0] if l.startswith('@label')][0].split()[1]
                defs.add(lab)
                bodies = [[l for l in a if not l.startswith('@label')] for a in alts]
                self.note_refs(unit, re.findall(r'\{[TI]:([^}]*)\}', ' '.join(
                    l for a in bodies for l in a)))
                out[sec].append(Insn('%s %s' % (word, rest), bodies, unit))
                out[sec].append(Label((unit, lab)))
            else:
                self.note_refs(unit, re.findall(r'\{[TI]:([^}]*)\}', ' '.join(
                    l for a in alts for l in a)))
                out[sec].append(Insn('%s %s' % (word, rest), alts, unit))
        return self

    def directive(self, word, rest, sec, unit, defs):
        out = self.sections[sec]
        if word in ('.text',):
            return 'text'
        if word == '.data':
            return 'data'
        if word == '.bss':
            return 'bss'
        if word == '.section':
            name = rest.split(',')[0].strip()
            for p in ('vectors', 'text', 'rodata', 'data', 'bss'):
                if name.startswith('.' + p) or name.startswith('.s' + p):
                    return p
            raise Gap('section %s' % name)
        if word in ('.globl', '.global'):
            self.globals.update(n.strip() for n in rest.split(','))
            return None
        if word in ('.weak',):
            self.globals.update(n.strip() for n in rest.split(','))
            return None
        if word in ('.file', '.ident', '.type', '.size', '.local', '.endswitch',
                    '.hidden', '.internal', '.protected'):
            return None
        if word in ('.p2align', '.align', '.balign'):
            n = int(rest.split(',')[0], 0)
            out.append(Align(n if word == '.balign' else 1 << n))
            return None
        if word in ('.long', '.word', '.4byte'):
            self.note_refs(unit, split_ops(rest))
            out.append(Data('w', split_ops(rest), unit))
            return None
        if word in ('.short', '.hword', '.2byte'):
            self.note_refs(unit, split_ops(rest))
            out.append(Data('h', split_ops(rest), unit))
            return None
        if word == '.byte':
            self.note_refs(unit, split_ops(rest))
            out.append(Data('b', split_ops(rest), unit))
            return None
        if word in ('.zero', '.space', '.skip'):
            out.append(Space(int(rest.split(',')[0], 0)))
            return None
        if word == '.fill':
            p = [int(x, 0) for x in split_ops(rest)]
            p += [1, 0][len(p) - 1:]
            n, size, val = p[0], p[1], p[2]
            out.append(Data('b', [], unit, raw=(val & ((1 << 8 * min(size, 4)) - 1))
                            .to_bytes(size, 'little') * n))
            return None
        if word in ('.ascii', '.string', '.asciz'):
            data = cstring(rest)
            if word != '.ascii':
                data += b'\0'
            out.append(Data('b', [], unit, raw=data))
            return None
        if word in ('.comm', '.lcomm'):
            p = split_ops(rest)
            name, size = p[0], int(p[1], 0)
            align = int(p[2], 0) if len(p) > 2 else 4
            if word == '.comm':
                self.globals.add(name)
            else:
                defs.add(name)
            want = self.place.get(name, 'bss')
            if want not in ('bss', 'sdbss'):
                raise Gap('placement: %s is common (.bss), %s takes only '
                          'initialised data' % (name, want))
            if want != 'bss':
                self.placed.add(name)
            b = self.sections[want]
            b.append(Align(align))
            b.append(Label((unit, name)))
            b.append(Space(size))
            defs.add(name)
            return None
        if word in ('.set', '.equ'):
            name, val = split_ops(rest)
            defs.add(name)
            out.append(SetSym((unit, name), val))
            return None
        raise Gap('directive %s' % word)


class Gapped:
    """Placeholder for an untranslatable instruction (statistics mode)."""
    def __init__(self, src):
        self.src = src

    def layout(self, addr, lookup):
        raise Gap('untranslatable: %s' % self.src)


class SetSym:
    def __init__(self, key, expr):
        self.key, self.expr = key, expr


def cstring(s):
    s = s.strip()
    assert s[0] == '"' and s[-1] == '"', s
    s = s[1:-1]
    out, i = bytearray(), 0
    esc = {'n': 10, 't': 9, 'r': 13, '\\': 92, '"': 34, "'": 39, 'a': 7,
           'b': 8, 'f': 12, 'v': 11}
    while i < len(s):
        c = s[i]
        if c != '\\':
            out += c.encode('latin-1')
            i += 1
            continue
        n = s[i + 1]
        if n in '01234567':
            j = i + 1
            while j < len(s) and j < i + 4 and s[j] in '01234567':
                j += 1
            out.append(int(s[i + 1:j], 8) & 255)
            i = j
        elif n == 'x':
            j = i + 2
            while j < len(s) and s[j] in '0123456789abcdefABCDEF':
                j += 1
            out.append(int(s[i + 2:j], 16) & 255)
            i = j
        else:
            out.append(esc[n])
            i += 2
    return bytes(out)


# ---------------------------------------------------------------------------
# Whole-program layout.

class Program:
    ORDER = ('vectors', 'text', 'rodata', 'data', 'bss')
    # Far sections (build(place=, far_base=)): run at far_base and up (e.g.
    # SDRAM), after the image in address terms.  .xdata is initialised
    # data: the load image carries it right after .data, from
    # __xdata_load, and the program copies it to __xdata_start itself
    # before use.  .sdbss is zero-initialised and never loaded (the
    # program clears it).
    FAR = ('xdata', 'sdbss')

    def __init__(self, tr, base=DEFAULT_BASE, extern=None, entry=None,
                 far_base=None, imports=None, exports=None):
        self.tr, self.base, self.extern, self.entry = tr, base, extern, entry
        self.far_base = far_base
        self.imports = imports    # Imports: a fixed-base link's resident
        self.exports = exports    # (names, version): __vc2_abi over them
        self.addr = {}            # (unit, name) or name -> address
        self.image = b''
        self.xdata_image = b''
        self.symbols = {}

    def lookup_fn(self, unit, strict):
        defs = self.tr.local_defs.get(unit, ())

        def lookup(name):
            k = (unit, name) if name in defs else ('*', name)
            if k in self.addr:
                return self.addr[k]
            if name not in defs:
                v = self.linked(name, strict)
                if v is not None:
                    return v
            if self.extern is not None and name not in defs:
                return self.extern(name)
            if strict:
                raise KeyError('undefined symbol %s' % name)
            return self.cur
        return lookup

    def linked(self, name, strict):
        """A name a fixed-base link resolves: an import, or __vc2_abi"""
        if self.imports is not None:
            if name == ABI_SYM:
                return self.imports.abi
            return self.imports.addr.get(name)
        if self.exports is not None and name == ABI_SYM:
            names, version = self.exports
            got = {n: self.addr.get(('*', n)) for n in names}
            if any(v is None for v in got.values()):
                if strict:
                    raise Gap('exports: no global %s' % ', '.join(
                        n for n, v in got.items() if v is None))
                return 0
            return abi_hash(version, names, got)
        return None

    def layout(self, max_pass=40):
        far = any(self.tr.sections[s] for s in self.FAR)
        if far and self.far_base is None:
            raise Gap('objects placed in far sections, but no far base')
        for _ in range(max_pass):
            changed = False
            a = self.base
            self.ends = {}
            for s in self.ORDER + (self.FAR if far else ()):
                if s == 'xdata':
                    self.end = a
                    lma = (self.ends['data'] + 3) & ~3
                    if self.addr.get(('*', '__xdata_load')) != lma:
                        changed = True
                    self.addr[('*', '__xdata_load')] = lma
                    a = self.far_base
                elif s == 'sdbss':
                    a = (a + 15) & ~15
                a = (a + 3) & ~3
                if s == 'text' and self.entry is not None:
                    if a > self.entry:
                        raise Gap('.vectors ends at 0x%x, past the entry 0x%x'
                                  % (a, self.entry))
                    a = self.entry
                if self.addr.get(('*', '__%s_start' % s)) != a:
                    changed = True
                self.addr[('*', '__%s_start' % s)] = a
                for it in self.tr.sections[s]:
                    if isinstance(it, Label):
                        if self.addr.get(it.name) != a:
                            changed = True
                        self.addr[it.name] = a
                        if it.name[1] in self.tr.globals:
                            self.addr[('*', it.name[1])] = a
                        continue
                    if isinstance(it, SetSym):
                        self.cur = a
                        v = eval_expr(it.expr, self.lookup_fn(it.key[0], False))
                        if self.addr.get(it.key) != v:
                            changed = True
                        self.addr[it.key] = v
                        if it.key[1] in self.tr.globals:
                            self.addr[('*', it.key[1])] = v
                        continue
                    self.cur = a
                    n = it.layout(a, self.lookup_fn(getattr(it, 'unit', None), False))
                    if getattr(it, '_n', None) != n:
                        changed = True
                        it._n = n
                    a += n
                self.ends[s] = a
                end = (a + 3) & ~3         # word-rounded (crt0 clears words)
                if self.addr.get(('*', '__%s_end' % s)) != end:
                    changed = True
                self.addr[('*', '__%s_end' % s)] = end
            if not changed:
                break
        else:
            raise Gap('layout did not converge')
        if not far:
            self.end = a
        missing = set(self.tr.place) - self.tr.placed
        if missing:
            raise Gap('placement: no object named %s' % ', '.join(sorted(missing)))
        self.symbols = {}
        for k, v in self.addr.items():
            if k[0] == '*' or not k[1].startswith('.L'):
                self.symbols.setdefault(k[1], v)
        if self.imports is not None or self.exports is not None:
            self.abi = self.linked(ABI_SYM, True)
        if self.entry is not None and self.symbols.get('_start') != self.entry:
            raise Gap('_start is at %s, not at the entry 0x%x (it must be the '
                      'first thing in .text)' % (
                          '0x%x' % self.symbols['_start'] if '_start' in
                          self.symbols else 'nowhere', self.entry))
        return self

    def emit(self):
        out = bytearray()
        a = self.base
        main = None
        for s in self.ORDER + self.FAR:
            if s == 'xdata':
                main, out = out, bytearray()
                a = self.symbols.get('__xdata_start', 0)
            if s == 'sdbss':
                if any(not isinstance(it, (Label, SetSym, Align, Space))
                       for it in self.tr.sections[s]):
                    raise Gap('.sdbss holds initialised data')
                break
            pad = ((a + 3) & ~3) - a
            if s == 'text' and self.entry is not None:
                pad = self.entry - a
            out += b'\0' * pad
            a += pad
            for it in self.tr.sections[s]:
                if isinstance(it, (Label, SetSym)):
                    continue
                lk = self.lookup_fn(getattr(it, 'unit', None), True)
                if isinstance(it, Insn):
                    it.layout(a, lk)          # final encoding, labels fixed
                    b = it.emit()
                elif isinstance(it, (Data, Switch, CaseTable)):
                    b = it.emit_at(lk)
                elif isinstance(it, Align):
                    b = bytes([it.fill]) * it.layout(a, lk)
                elif isinstance(it, Space):
                    b = bytes(it.n)
                if len(b) != it._n:
                    raise Gap('size changed after layout at 0x%x' % a)
                out += b
                a += len(b)
        self.image, self.xdata_image = bytes(main), bytes(out)
        return self

    def load_image(self):
        """What a loader copies: the image up to the end of .data (.bss is
        zeroed at run time), then .xdata's initial bytes, if any, at
        __xdata_load."""
        img = self.image[:self.ends['data'] - self.base]
        if self.xdata_image:
            lma = self.symbols['__xdata_load'] - self.base
            img += b'\0' * (lma - len(img)) + self.xdata_image
        return img

    def write_map(self, f):
        """Section table and every symbol by address (no .L labels)."""
        prev = self.base
        f.write('section   start     end       bytes\n')
        for s in self.ORDER:
            start = self.entry if (s == 'text' and self.entry is not None) \
                else (prev + 3) & ~3
            if s == 'vectors' and not self.tr.sections['vectors']:
                continue
            f.write('%-9s %08x  %08x  %d\n' % (s, start, self.ends[s],
                                              self.ends[s] - start))
            prev = self.ends[s]
        for s in self.FAR:
            if s in self.ends:
                start = self.symbols['__%s_start' % s]
                f.write('%-9s %08x  %08x  %d\n' % (s, start, self.ends[s],
                                                  self.ends[s] - start))
        f.write('load image %d bytes (0x%x-0x%x)\n\n' % (
            len(self.load_image()), self.base, self.base + len(self.load_image())))
        for n, v in sorted(self.symbols.items(), key=lambda kv: (kv[1], kv[0])):
            f.write('%08x %s\n' % (v, n))


# ---------------------------------------------------------------------------

def compile_c(path, opt='-O2', extra=()):
    if not os.path.exists(GCC):
        raise FileNotFoundError('VC IV GCC not found at %s (set VC4_GCC)' % GCC)
    r = subprocess.run([GCC, opt, '-S', '-o', '-'] + GCC_FLAGS + list(extra) +
                       [path], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('vc4-elf-gcc failed:\n' + r.stderr)
    bad = DROPPED_SOURCE.findall(r.stdout)
    if bad:
        raise RuntimeError(
            '%s: vc4-elf-gcc emitted %s, which loses its source register: '
            'vc4.md sminsi3/smaxsi3 alternative 5 takes any register but '
            'prints the two-operand form (compiler.md, "Known GCC bugs").  '
            'Rewrite the clamp, e.g. as an if, or keep the bound in a '
            'variable' % (path, ', '.join(sorted(set(b.strip() for b in bad)))))
    return r.stdout


# vc4-elf-gcc 6.2.1's min/max with a long immediate: the pattern accepts
# a source register other than the destination and then drops it
DROPPED_SOURCE = re.compile(r'^\s*(?:min|max)\.l\s+\w+\s*,\s*#[^,\n]*$', re.M)


def preprocess(path, extra=()):
    """A .S file: native assembly through the C preprocessor (so it can
    share #defines with the C code; headers guard C with __ASSEMBLER__)."""
    if not os.path.exists(GCC):
        raise FileNotFoundError('VC IV GCC not found at %s (set VC4_GCC)' % GCC)
    r = subprocess.run([GCC, '-E', '-P', '-x', 'assembler-with-cpp'] +
                       list(extra) + [path], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError('vc4-elf-gcc -E failed:\n' + r.stderr)
    return r.stdout


def lib_index(dirs):
    """{global symbol: .s path} over library directories of VC IV
    assembly (e.g. libgcc recompiled with GCC_FLAGS)."""
    idx = {}
    for d in dirs:
        for f in sorted(os.listdir(d)):
            if not f.endswith('.s'):
                continue
            p = os.path.join(d, f)
            with open(p) as fh:
                text = fh.read()
            gl = set(re.findall(r'^\s*\.glob\w*\s+([\w.$]+)', text, re.M))
            for name in re.findall(r'^([\w.$]+):', text, re.M) + re.findall(
                    r'^\s*\.set\s+([\w.$]+)\s*,', text, re.M):
                if name in gl:
                    idx.setdefault(name, p)
    return idx


DEFAULT_LIBS = [d for d in [os.environ.get('VC2CC_LIB', os.path.expanduser(
    '~/Git/re/vc2-gcc-spike/libgcc'))] if d and os.path.isdir(d)]


# libgcc's software floating point (and complex) routines: __adddf3,
# __fixsfsi, __floatsisf, __extendsfdf2, __mulsc3 ...
SOFT_FLOAT = re.compile(r'^__(add|sub|mul|div|neg|cmp|unord|eq|ne|lt|le|gt|ge|'
                        r'fix|fixuns|float|floatun|floatuns|extend|trunc|powi?)'
                        r'(si|di|ti)?(sf|df|tf|xf|sc|dc|tc|xc)')


class FloatBanned(Gap):
    """--no-float: the program uses floating point."""


# This process's source substitutions: {real path of a file in the tree:
# the path to build instead}.  A test's mutant kernel (gen_jdec.py,
# gen_jobs.py, gen_viz.py --mutate NAME --out DIR) is built through this,
# never written over the tracked file: other benches build images from
# the tree at the same time (bcmfw/tests/mutant.py).
SUBST = {}


def subst(path):
    """path, or what SUBST builds in its place"""
    return SUBST.get(os.path.realpath(path), path) if SUBST else path


def build(inputs, opt='-O2', base=DEFAULT_BASE, extra=(), libs=None,
          extern=None, entry=None, no_float=False, conservative=False,
          place=None, far_base=None, imports=None, exports=None):
    """inputs: list of paths (.c compiled, .s taken as VC IV assembly or,
    after '.syntax vc2', native VC II) or ('name.s', text) tuples.
    Library .s files that define a still undefined symbol are pulled in
    (archive semantics); what remains goes to extern(name) -> address
    (e.g. vc2sim stubs).  entry: firmware layout (.vectors at base, .text
    from entry, _start first).  no_float: soft-float references are an
    error.  conservative: see Translator.  place: {data object name:
    'xdata' or 'sdbss'}, laid out from far_base (Program.FAR).
    imports: an Imports, a fixed-base link against a resident image (the
    module docstring); every other undefined name is an error.  exports:
    (names, version), the resident side: __vc2_abi over those globals.
    Returns a laid-out, emitted Program (.abi set with either)."""
    tr = Translator(conservative, place)
    for k, inp in enumerate(inputs):
        if not isinstance(inp, tuple):
            inp = subst(inp)
        if isinstance(inp, tuple):
            text = inp[1]
        elif inp.endswith('.c'):
            text = compile_c(inp, opt, extra)
        elif inp.endswith('.S'):
            text = preprocess(inp, extra)
        else:
            with open(inp) as f:
                text = f.read()
        tr.parse(text, k)
    if no_float:
        bad = sorted({n for _, n in tr.refs if SOFT_FLOAT.match(n)})
        if bad:
            raise FloatBanned('floating point is banned in this program (it '
                              'calls %s)' % ', '.join(bad))
    idx = lib_index(DEFAULT_LIBS if libs is None else libs)
    loaded, unit = set(), len(inputs)
    linked = set(imports.addr) | {ABI_SYM} if imports is not None else \
        {ABI_SYM} if exports is not None else set()
    while True:
        need = {idx[n] for n in tr.undefined() - linked if n in idx} - loaded
        if not need:
            break
        for p in sorted(need):
            with open(p) as fh:
                tr.parse(fh.read(), unit)
            loaded.add(p)
            unit += 1
    if no_float:
        fl = sorted(g for g in tr.gaps if re.search(
            r'\bf(add|sub|mul|div|cmp|lts|ltu|trunc|min|max)\b', g))
        if fl:
            raise FloatBanned('floating point is banned in this program: %s'
                              % '; '.join(fl))
    if tr.gaps:
        raise Gap('untranslatable: ' + '; '.join(
            '%s (x%d)' % kv for kv in sorted(tr.gaps.items())))
    if imports is not None and extern is None:
        left = sorted(n for n in tr.undefined() - linked if not LINKER_SYM.match(n))
        if left:
            raise Gap('fixed-base link: %s neither defined here nor imported '
                      'from the resident image' % ', '.join(left))
    return Program(tr, base, extern, entry, far_base, imports,
                   exports).layout().emit()


def load_into_sim(prog, sim=None):
    import vc2sim as S
    sim = sim or S.Sim()
    sim.mem.write(prog.base, prog.image)
    img = S.Image('<vc2cc>')
    img.symbols = dict(prog.symbols)
    img.text = (prog.base, prog.ends['text'])
    sim.images.append(img)
    return sim


def listing(prog):
    """VC II listing of the image's text.  Halfwords that are not
    instructions (data words, switch case tables, padding) print as .hw;
    labels inside native-assembly functions are marked '(label)'.
    With Apple's corpus present, an instruction whose kind is neither in
    Apple's static code nor device-verified ends in '; unproven form'
    (vc2forms.py)."""
    import vc2forms as F
    code = {i.addr for i in F.code_insns(prog)}
    out = []
    by_addr = {}
    for n, a in prog.symbols.items():
        by_addr.setdefault(a, []).append(n)
    inner = F.inner_labels(prog)
    a, end = prog.base, prog.ends['text']
    rd = lambda x: int.from_bytes(prog.image[x - prog.base:x - prog.base + 2],
                                  'little') if prog.base <= x < end else None
    while a < end:
        for n in by_addr.get(a, []):
            out.append('%08x <%s>%s' % (a, n, ' (label)' if n in inner else ''))
        ins = D.decode(rd, a) if a in code else None
        if ins is None:
            out.append('  %08x  %04x                       .hw' % (a, rd(a)))
            a += 2
            continue
        out.append('  %08x  %-26s %s%s' % (a, ' '.join('%04x' % w for w in ins.ws),
                                           ins.text, F.unproven_marker(ins)))
        a += ins.size
    return '\n'.join(out)


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('inputs', nargs='*')
    ap.add_argument('-O', dest='opt', default='2')
    ap.add_argument('-o', dest='out')
    ap.add_argument('--base', type=lambda s: int(s, 0), default=DEFAULT_BASE)
    ap.add_argument('--entry', type=lambda s: int(s, 0), default=None)
    ap.add_argument('--map', action='store_true')
    ap.add_argument('--mapfile')
    ap.add_argument('--no-float', action='store_true')
    ap.add_argument('--conservative', action='store_true')
    ap.add_argument('-I', dest='incs', action='append', default=[])
    ap.add_argument('-D', dest='defs', action='append', default=[])
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--stats', action='store_true')
    ap.add_argument('--run', nargs='+', metavar=('FUNC', 'ARG'))
    ap.add_argument('--trace', action='store_true')
    ap.add_argument('--import-map', help="the resident image's map (a fixed-base link)")
    ap.add_argument('--imports', default='', help='names it may use, comma separated')
    ap.add_argument('--exports', default='', help='the resident side: the same list')
    ap.add_argument('--abi-version', type=int, default=0)
    a = ap.parse_args(argv)
    opt = '-O' + a.opt
    if a.stats:
        tot, bad, lay_ok, lay_bad = 0, {}, 0, 0
        for p in a.inputs:
            tr = Translator()
            try:
                tr.parse(open(p).read(), 0)
            except Exception as e:  # noqa: BLE001 (statistics)
                bad['parse: %s' % e] = bad.get('parse: %s' % e, 0) + 1
            tot += 1
            for g, n in tr.gaps.items():
                key = re.sub(r'\d+', 'N', g)
                bad[key] = bad.get(key, 0) + n
            if not tr.gaps:
                try:
                    Program(tr).layout()
                except Gap as e:
                    key = re.sub(r'0x[0-9a-f]+', 'ADDR', str(e))
                    key = re.sub(r'\d+', 'N', key)
                    bad[key] = bad.get(key, 0) + 1
                    lay_bad += 1
                else:
                    lay_ok += 1
        print('%d files, %d laid out, %d failed layout' % (tot, lay_ok, lay_bad))
        for g, n in sorted(bad.items(), key=lambda kv: -kv[1]):
            print('%6d  %s' % (n, g))
        return 0
    extra = ['-I' + d for d in a.incs] + ['-D' + d for d in a.defs]
    names = lambda t: [n.strip() for n in t.split(',') if n.strip()]  # noqa: E731
    try:
        imports = Imports(read_map(a.import_map), names(a.imports),
                          a.abi_version) if a.import_map else None
        exports = (names(a.exports), a.abi_version) if a.exports else None
        prog = build(a.inputs, opt, a.base, extra=extra, entry=a.entry,
                     no_float=a.no_float, conservative=a.conservative,
                     imports=imports, exports=exports)
    except (Gap, RuntimeError, KeyError) as e:
        print('vc2cc: %s' % (e.args[0] if e.args else e), file=sys.stderr)
        return 1
    if a.out:
        with open(a.out, 'wb') as f:
            f.write(prog.load_image())
    if a.mapfile:
        with open(a.mapfile, 'w') as f:
            prog.write_map(f)
    if a.map:
        for n, v in sorted(prog.symbols.items(), key=lambda kv: kv[1]):
            print('%08x %s' % (v, n))
    if a.list:
        print(listing(prog))
    print('image 0x%x-0x%x: text %d, rodata %d, data %d, bss %d bytes' % (
        prog.base, prog.end, prog.ends['text'] - prog.base,
        prog.ends['rodata'] - prog.ends['text'],
        prog.ends['data'] - prog.ends['rodata'],
        prog.ends['bss'] - prog.ends['data']), file=sys.stderr)
    if a.run:
        sim = load_into_sim(prog)
        if a.trace:
            sim.trace = sys.stdout
        args = [int(x, 0) for x in a.run[1:]]
        r0 = sim.call(a.run[0], *args)
        print('r0 = %d (0x%08x), r1 = 0x%08x, %d instructions' % (
            s32(r0), r0, sim.r[1], sim.steps))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
