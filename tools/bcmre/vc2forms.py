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
"""Encoding-class gate for our VideoCore II images (compiler.md,
"Conservative mode").

An instruction kind (vc2sim.cover_key: encoding class plus operation,
e.g. 'alu_imm5 add', 'bcc hi', 'ldst_postinc ldh') may appear in an image
only if it occurs in Apple's static code (so the encoding exists on the
chip) or has been verified on the device (device_verified.txt).  The
16-bit 'cmp rd, #imm5' that every image used until device session 1 is
the counter-example: the table guessed it by analogy, and the chip runs
that encoding as ld/st (--rs).

  apple_kinds(corpus_root)  kinds in Apple's code: every sized function
                            of every .vll, plus a trace of vmcs.bin and
                            RenderServer.bin from 0x200 and the vector
                            words, in every version present; cached in
                            build/bcmre/apple_kinds.json
  DEVICE_VERIFIED           kinds verified on the device
  APPLE_STATIC_ONLY         kinds images may execute although the vc2sim
                            self-test never runs them (hello_test et al.)
  scan(prog, apple, exempt) instructions whose kind is in neither set

Usage:
  vc2forms.py --apple-kinds [CORPUS_ROOT]      list the kinds with counts
  vc2forms.py --scan FILE.lst|FILE.map ... [--exempt SYM=KIND,KIND ...]
        report every instruction of an image whose kind is unproven
        (a .map is read with the .lst next to it); exit 1 if any

Apple's files are reference material only (read, never shipped).  Run
with python3 -I.
"""
import collections
import contextlib
import fnmatch
import glob
import io
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import vc2dis as D  # noqa: E402
import vc2sim as S  # noqa: E402

CORPUS_ROOT = os.path.expanduser('~/Git/re/bcm2722/extract')
CACHE = os.path.join(ROOT, 'build', 'bcmre', 'apple_kinds.json')
VERIFIED_FILE = os.path.join(HERE, 'device_verified.txt')

# Kinds our images may execute although the vc2sim self-test never runs
# them: the exact encoding occurs in Apple's static code and its
# operation and condition are executed by the self-test in another
# encoding.  Condition hi is run by 'addcmpb hi'; the 16- and 32-bit
# conditional branch forms are run with other conditions.  Static
# sites over all versions' .vll + vmcs.bin + RenderServer.bin (1.3
# alone): see apple_kinds(counts=True) and compiler.md.
APPLE_STATIC_ONLY = frozenset({'bcc hi', 'bcc.s hi'})


def load_verified(path=VERIFIED_FILE):
    """{kind: (session, evidence)} from device_verified.txt
    ('kind | session/date | evidence' per line, '#' comments)."""
    out = {}
    if not os.path.exists(path):
        return out
    for ln in open(path):
        ln = ln.split('#', 1)[0].strip()
        if not ln:
            continue
        parts = [p.strip() for p in ln.split('|')]
        if len(parts) != 3 or not all(parts):
            raise ValueError('%s: bad line %r (kind | session | evidence)' % (path, ln))
        out[parts[0]] = (parts[1], parts[2])
    return out


DEVICE_VERIFIED = frozenset(load_verified())


# --------------------------------------------------------------------------
# Apple's static kinds

def corpus_files(corpus_root=CORPUS_ROOT):
    """(vll paths, raw image paths) of every version under corpus_root."""
    vlls, raws = [], []
    for vc in sorted(glob.glob(os.path.join(corpus_root, '*', 'vc', 'VideoCore'))):
        vlls += sorted(glob.glob(os.path.join(vc, 'Library', '*.vll')))
        for n in ('vmcs.bin', 'RenderServer.bin'):
            p = os.path.join(vc, 'Boot', n)
            if os.path.exists(p):
                raws.append(p)
    return vlls, raws


def have_corpus(corpus_root=CORPUS_ROOT):
    vlls, raws = corpus_files(corpus_root)
    return bool(vlls and raws)


def _stamp(paths):
    own = [os.path.join(HERE, n) for n in ('vc2dis.py', 'vc2sim.py', 'vc2forms.py')]
    return [[p, os.path.getsize(p), int(os.path.getmtime(p))] for p in list(paths) + own]


def _sweep_counts(vlls, raws):
    c = collections.Counter()
    for path in vlls:
        f = D.Vll(path)
        for v, sz, _ in f.funcs:
            items, _ = D.sweep(f.read, v, v + sz)
            for it in items:
                if not isinstance(it, tuple):
                    c[S.cover_key(it)] += 1
    for path in raws:
        data = open(path, 'rb').read()
        roots = [0x200] + [w for w in struct.unpack_from('<64I', data, 0)
                           if 0x200 <= w < len(data) and not w & 1]
        with contextlib.redirect_stdout(io.StringIO()):
            seen = D.trace(path, 0, roots)
        for ins in seen.values():
            c[S.cover_key(ins)] += 1
    return c


def apple_kinds(corpus_root=CORPUS_ROOT, counts=False, cache=CACHE):
    """Kinds in Apple's static code (a set; with counts=True a Counter of
    static sites).  Empty when the corpus is absent."""
    vlls, raws = corpus_files(corpus_root)
    if not vlls:
        return collections.Counter() if counts else frozenset()
    stamp = _stamp(vlls + raws)
    c = None
    try:
        with open(cache) as f:
            j = json.load(f)
        if j.get('stamp') == stamp:
            c = collections.Counter(j['counts'])
    except (OSError, ValueError):
        pass
    if c is None:
        c = _sweep_counts(vlls, raws)
        try:
            os.makedirs(os.path.dirname(cache), exist_ok=True)
            with open(cache, 'w') as f:
                json.dump({'stamp': stamp, 'counts': c}, f, indent=0, sort_keys=True)
        except OSError:
            pass
    return c if counts else frozenset(c)


def allowed(apple):
    return frozenset(apple) | DEVICE_VERIFIED


# --------------------------------------------------------------------------
# Scanning an image

class Insn(collections.namedtuple('Insn', 'addr sym kind text words')):
    def line(self):
        return '  %08x  %-26s %-34s ; %s in <%s>' % (
            self.addr, ' '.join('%04x' % w for w in self.words), self.text,
            self.kind, self.sym)


def code_insns(prog):
    """Every instruction of a vc2cc Program, decoded from its image:
    Insn and switch items only (data, case tables, padding skipped)."""
    import vc2cc as C
    rd = lambda x: int.from_bytes(prog.image[x - prog.base:x - prog.base + 2],
                                  'little') if prog.base <= x < prog.base + len(prog.image) - 1 else None
    out = []
    a = prog.base
    for s in prog.ORDER:
        pad = ((a + 3) & ~3) - a
        if s == 'text' and prog.entry is not None:
            pad = prog.entry - a
        a += pad
        for it in prog.tr.sections[s]:
            if isinstance(it, (C.Label, C.SetSym)):
                continue
            n = it._n
            if isinstance(it, (C.Insn, C.Switch)):
                b = a
                while b < a + n:
                    ins = D.decode(rd, b)
                    if ins is None:
                        break
                    out.append(ins)
                    b += ins.size
            a += n
    return out


def inner_labels(prog):
    """Labels of native assembly that are not global: places inside a
    function (probe.S's c7, a kernel's _loop), not region starts."""
    return {n for n in prog.tr.native_labels if n not in prog.tr.globals}


def _parse_listing(text):
    """(instructions, region-start symbols) from a vc2cc listing."""
    syms, insns = [], []
    for ln in text.splitlines():
        m = re.match(r'^([0-9a-f]{8}) <(.+)>( \(label\))?$', ln)
        if m:
            if not m.group(3):
                syms.append((int(m.group(1), 16), m.group(2)))
            continue
        m = re.match(r'^  ([0-9a-f]{8})  ((?:[0-9a-f]{4} )+)\s*(.*?)\s*(;.*)?$', ln)
        if not m or m.group(3) == '.hw':        # data, case tables, padding
            continue
        ws = [int(w, 16) for w in m.group(2).split()]
        ins = D.decode_words(ws, int(m.group(1), 16))
        if ins is not None:
            insns.append(ins)
    return insns, syms


def _sym_index(syms):
    syms = sorted((a, n) for a, n in syms if not n.startswith(('.L', '__')))
    starts = [a for a, _ in syms]

    def at(addr):
        import bisect
        k = bisect.bisect_right(starts, addr) - 1
        return syms[k][1] if k >= 0 else '?'
    return at


def scan(prog_or_listing, apple, exempt=None):
    """Offending instructions: kind not in apple | DEVICE_VERIFIED, and not
    allowed by exempt ({symbol: kinds}: the region from that symbol to the
    next function symbol (C function or global; not a native-assembly
    label inside a function) may contain those kinds, e.g. a self-check's forms under
    test; symbols and kinds are fnmatch patterns, so 'bench_vc_*' and
    'valu*' work).  prog_or_listing: a vc2cc Program,
    or listing text, or a path to a .lst (or .map next to its .lst).
    Returns a list of Insn (addr, sym, kind, text, words)."""
    p = prog_or_listing
    if hasattr(p, 'tr'):
        insns = code_insns(p)
        inner = inner_labels(p)
        syms = [(a, n) for n, a in p.symbols.items() if n not in inner]
    else:
        if isinstance(p, str) and '\n' not in p and os.path.exists(
                re.sub(r'\.map$', '.lst', p)):
            p = open(re.sub(r'\.map$', '.lst', p)).read()
        insns, syms = _parse_listing(p)
    ok = allowed(apple)
    exempt = exempt or {}
    where = _sym_index(syms)
    bad = []
    for ins in insns:
        k = S.cover_key(ins)
        if k in ok:
            continue
        sym = where(ins.addr)
        if any(fnmatch.fnmatchcase(sym, sp) and
               any(fnmatch.fnmatchcase(k, kp) for kp in kinds)
               for sp, kinds in exempt.items()):
            continue
        bad.append(Insn(ins.addr, sym, k, ins.text, ins.ws))
    return bad


def predec_sites(prog_or_listing):
    """16-bit instructions in 0x7000-0x77ff (ld/st (--rs)) of an image."""
    p = prog_or_listing
    if hasattr(p, 'tr'):
        insns = code_insns(p)
    else:
        if isinstance(p, str) and '\n' not in p:
            p = open(re.sub(r'\.map$', '.lst', p)).read()
        insns, _ = _parse_listing(p)
    return [i for i in insns if i.size == 2 and 0x7000 <= i.ws[0] <= 0x77ff]


_ALLOWED = None


def unproven_marker(ins):
    """'   ; unproven form' if ins's kind is not in Apple's static code or
    DEVICE_VERIFIED (and the corpus is available), else ''."""
    global _ALLOWED
    if _ALLOWED is None:
        apple = apple_kinds()
        _ALLOWED = allowed(apple) if apple else False
    if not _ALLOWED:
        return ''
    return '' if S.cover_key(ins) in _ALLOWED else '   ; unproven form'


def main(argv):
    if not argv or argv[0] not in ('--apple-kinds', '--scan'):
        sys.exit(__doc__)
    if argv[0] == '--apple-kinds':
        root = argv[1] if len(argv) > 1 else CORPUS_ROOT
        c = apple_kinds(root, counts=True)
        if not c:
            sys.exit('no corpus under %s' % root)
        for k, n in sorted(c.items()):
            print('%-30s %7d' % (k, n))
        print('%d kinds; device-verified: %s; Apple-static-only: %s' % (
            len(c), ', '.join(sorted(DEVICE_VERIFIED)) or 'none',
            ', '.join('%s (%d)' % (k, c[k]) for k in sorted(APPLE_STATIC_ONLY))))
        return 0
    files, exempt = [], {}
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == '--exempt':
            sym, _, kinds = args.pop(0).partition('=')
            exempt.setdefault(sym, set()).update(k.replace(':', ' ') for k in kinds.split(','))
        else:
            files.append(a)
    apple = apple_kinds()
    if not apple:
        print('skip: no Apple corpus under %s' % CORPUS_ROOT)
        return 0
    rc = 0
    for path in files:
        bad = scan(path, apple, exempt)
        pre = predec_sites(path)
        print('%s: %d unproven instruction(s), %d in 0x7000-0x77ff' % (
            path, len(bad), len(pre)))
        for b in bad:
            print(b.line())
        rc |= bool(bad or pre)
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
