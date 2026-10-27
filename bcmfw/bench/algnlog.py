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
# Bench row ALGN's lines: which candidate reading each check fits
# (readlog.py and scripts/bench_ingest.py use it).
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
"""Bench row ALGN (bcmfw/bench/algn.h, bcm-devtest-core.c bench_algn;
docs/research/bcm2722/bench-p19.md "ALGN"; memory-access.md "Alignment").

The device logs, in stage 4 after the bench:
  ALGN n N faults F src S buf B in U us
  ALGN k TAG status pc at r2 r3 r4 r5     (the BCM's own access checks)
  ALGNB k w0 .. w7                         (the target buffer's window)
  ALGN stopped at check k (TAG): ...       (that check hung or stopped it)
  ALGNP k TAG bad N low2 M of T [+off got want]...   (the host port's)

Each check has candidate readings; verdicts() names the one the record
fits and whether it is the one vc2sim models (or, for the port, the one
the rules in memory-access.md "Alignment" expect).  Adding a check: a
CHECKS entry here, the routine in algn.S (or a pa_cases row in
bcm-devtest-core.c)."""
import re

ALGN = re.compile(r'^\s*\d+ ALGN (\d+) (\S{4})((?: [0-9a-f]{8}){7})$')
ALGNB = re.compile(r'^\s*\d+ ALGNB (\d+)((?: [0-9a-f]{8}){8})$')
ALGNN = re.compile(r'^\s*\d+ ALGN n (\d+) faults (\d+) src ([0-9a-f]{8}) buf ([0-9a-f]{8})')
ALGNS = re.compile(r'^\s*\d+ ALGN stopped at check (\d+) \((\S+)\): (.*)$')
ALGNP = re.compile(r'^\s*\d+ ALGNP (\d+) (\S{4}) bad (\d+) low2 (\d+) of (\d+)(.*)$')
# a port check that could not write (bcm-devtest-core.c algn_port): the
# port error -1 stops the part, so the checks after it never ran
ALGNPE = re.compile(r'^\s*\d+ ALGNP (\d+) (\S{4}): port error (-?\d+)$')

FILL = [0xA5A5A5A0 + i for i in range(32)]      # algn.h ALGN_FILL
SW = 0xC3C2C1C0                                  # algn.S stores
SH = 0xD1D0


def src_byte(k):
    """algn_src: byte k = 0x10 + k"""
    return (0x10 + k) & 0xFF


def src_val(off, size):
    return sum(src_byte(off + i) << (8 * i) for i in range(size))


def buf_after(writes, nwords=16):
    """The buffer's words after byte writes [(offset, bytes)] on the fill."""
    b = bytearray(b''.join(w.to_bytes(4, 'little') for w in FILL[:nwords]))
    for off, data in writes:
        for i, x in enumerate(data):
            if 0 <= off + i < len(b):
                b[off + i] = x
    return [int.from_bytes(b[4 * i:4 * i + 4], 'little') for i in range(nwords)]


def later(r4, r5):
    """How many of the four adds after the access ran (algn.S)."""
    return {(0, 0): 0, (1, 0): 1, (1, 1): 2, (3, 1): 3, (3, 3): 4}.get((r4, r5))


# ---- the BCM's checks: (kind, offset, size, extra)
#   load: offset into algn_src, size, signed, writeback (base in r3)
#   store: offset into algn_buf, size, writeback
#   vld: offset, width (1/2), window row (0 or 1 for the stride check)
#   vst: offset, width
#   dma: source offset, destination offset, unit bytes, units
CHECKS = {
    'LW1C': ('load', 1, 4, False, False), 'LWR2': ('load', 2, 4, False, False),
    'LWR3': ('load', 3, 4, False, False), 'LWO2': ('load', 2, 4, False, False),
    'LWL2': ('load', 2, 4, False, False), 'LWP2': ('load', 2, 4, False, True),
    'LWX2': ('load', 6, 4, False, False), 'LHR1': ('load', 1, 2, False, False),
    'LHR3': ('load', 3, 2, False, False), 'LHO1': ('load', 1, 2, False, False),
    'LSH1': ('load', 1, 2, True, False), 'LHP1': ('load', 1, 2, False, True),
    'SWR1': ('store', 1, 4, False), 'SWR2': ('store', 2, 4, False),
    'SWR3': ('store', 3, 4, False), 'SWO2': ('store', 2, 4, False),
    'SWP2': ('store', 2, 4, True), 'SHR1': ('store', 1, 2, False),
    'SHR3': ('store', 3, 2, False), 'SHO1': ('store', 1, 2, False),
    'VL68': ('vld', 8, 2, 0), 'VL64': ('vld', 4, 2, 0), 'VL62': ('vld', 2, 2, 0),
    'VL88': ('vld', 8, 1, 0), 'VL84': ('vld', 4, 1, 0), 'VL82': ('vld', 2, 1, 0),
    'VLS4': ('vld', 40, 2, 1),
    'VPG4': ('vpg', 12, 4), 'VPGX': ('vpgx', 14), 'VPQS': ('vpqs', 4, 20),
    'VS68': ('vst', 8, 2), 'VS64': ('vst', 4, 2), 'VS62': ('vst', 2, 2),
    'VS80': ('vst', 0, 1), 'VS88': ('vst', 8, 1), 'VS84': ('vst', 4, 1),
    'D32C': ('dma', 0, 0, 32, 2), 'D4W9': ('dma', 4, 4, 4, 9),
    'D2H3': ('dma', 2, 2, 2, 3), 'D1B5': ('dma', 1, 1, 1, 5),
    'D32M': ('dma', 0, 4, 32, 1),
}
# Silicon's reading (sitting 2, 2026-10-25, bench-20261025-2257), which
# vc2sim models, per kind
MODEL = {'load': 'faults (vector 1) after reading rounded down, 4 later instructions ran',
         'store': 'faults (vector 1, imprecise: 4 later instructions ran), stored '
                  'rounded down',
         'vld': 'reads from the address rounded down to its size, each REP row on '
                'its own',
         'vst': 'stores at the address rounded down to its size',
         'vpg': 'each row loads its first-line lanes',
         'vpgx': 'vldp alone loads only the lanes in the first line',
         'vpqs': 'each row of a pair REP splits at its own line',
         'dma': 'copies exactly (a 32-byte unit off 32: to the destination rounded '
                'down to 32)'}


def _load(spec, st, vec, pc, at, regs, src):
    _, off, size, signed, wb = spec
    r2, r3, r4, r5 = regs
    m = (1 << (8 * size)) - 1
    cand = [('rounded down', src_val(off & ~(size - 1), size)),
            ('at the exact address', src_val(off, size))]
    if size == 4:
        w = src_val(off & ~3, 4)
        s = 8 * (off & 3)
        cand.append(('rotated (ARM-style)', (w >> s | w << (32 - s)) & 0xFFFFFFFF))
    got = r2 & m
    what = next((n for n, v in cand if v == got), 'other (r2 %08x)' % r2)
    n = later(r4, r5)
    if st == 3:
        text = 'faults (vector %d) after reading %s, %s later instruction%s ran' % (
            vec, what, '?' if n is None else n, '' if n == 1 else 's')
    elif st == 1:
        text = 'no fault, reads %s' % what
    else:
        text = 'status %d' % st
    if wb and src is not None:
        text += '; base %s' % ('written back' if r3 == src + off + size else
                               'not written back' if r3 == src + off else 'r3 %08x' % r3)
    modelled = st == 3 and vec == 1 and what == 'rounded down' and n == 4
    if wb and src is not None:
        modelled = modelled and r3 == src + off + size
    return text, modelled


def _store(spec, st, vec, pc, at, regs, win, buf=None):
    _, off, size, wb = spec
    r2, r3, r4, r5 = regs
    n = later(r4, r5)
    data = (SW if size == 4 else SH).to_bytes(size, 'little')
    cand = [('nothing stored', buf_after([])),
            ('stored rounded down', buf_after([(off & ~(size - 1), data)])),
            ('stored at the exact address', buf_after([(off, data)]))]
    w = win[:8]
    what = next((n for n, v in cand if v[:8] == w), 'other (%08x %08x)' % (w[0], w[1]))
    if st == 3:
        precise = pc == at
        text = 'faults (vector %d, %s), %s' % (
            vec, 'precise: saved pc = the store' if precise else
            'imprecise: saved pc +%d, %s later instruction%s ran' % (
                (pc - at) & 0xFFFFFFFF, '?' if n is None else n, '' if n == 1 else 's'),
            what)
    elif st == 1:
        text = 'no fault, %s' % what
    else:
        text = 'status %d' % st
    if wb and buf is not None:
        text += '; base %s' % ('written back' if r3 == buf + off + size else
                               'not written back' if r3 == buf + off else 'r3 %08x' % r3)
    modelled = st == 3 and vec == 1 and pc != at and n == 4 and what == 'stored rounded down'
    if wb and buf is not None:
        modelled = modelled and r3 == buf + off + size
    return text, modelled


def _vld(spec, st, win):
    _, off, width, row = spec
    nb = 16 * width

    def row_at(x):
        if width == 2:
            return [src_val(x + 4 * j, 4) for j in range(8)]
        return [src_byte(x + 2 * j) | src_byte(x + 2 * j + 1) << 16 for j in range(8)]
    if row:     # row 1 of REP 2 from an aligned base at a stride of 40
        places = (('each row rounded down to %d' % nb, off & ~(nb - 1)),
                  ('the stride as given (row at +%d)' % off, off))
        model = places[0][0]
    else:
        places = (('rounded down to 32', off & ~31), ('rounded down to 16', off & ~15),
                  ('rounded down to 8', off & ~7), ('at the exact address', off))
        model = 'rounded down to %d' % nb
    cand = []
    places = sorted(places, key=lambda p: p[0] != model)     # the model's name first
    for name, x in places:
        if all(row_at(x) != v for _, v in cand):
            cand.append((name, row_at(x)))
    what = next((n for n, v in cand if v == win[:8]), 'other (%08x %08x)' % (win[0], win[1]))
    if st != 1:
        return 'status %d (%s)' % (st, what), False
    # a candidate equal to the model's under another name counts as it
    ok = what == model or row_at(dict(places)[model]) == win[:8]
    return ('row 1 of REP 2, stride 40: %s' if row else 'reads %s') % what, ok


def _vst(spec, st, win):
    _, off, width = spec
    if width == 2:
        data = bytes(src_byte(i) for i in range(32))
    else:       # an H8 row: the low bytes of the H16 row loaded from src
        data = bytes(src_byte(2 * i) for i in range(16))
    cand = [('stores nothing', buf_after([]))]
    for name, x in (('stores at the exact address', off),
                    ('stores rounded down to 32', off & ~31),
                    ('stores rounded down to 16', off & ~15),
                    ('stores rounded down to 8', off & ~7)):
        v = buf_after([(x, data)])
        if v not in [c[1] for c in cand]:
            cand.append((name, v))
    what = next((n for n, v in cand if v[:8] == win[:8]), 'other (%08x %08x)' % (
        win[0], win[1]))
    if st != 1:
        return 'status %d (%s)' % (st, what), False
    # the model: the row lands at the address rounded down to its size
    # (16 bytes for 8-bit rows, 32 for 16-bit); at these offsets that is
    # also the 32 rounding, so the name may say either
    return what, buf_after([(off & ~(16 * width - 1), data)])[:8] == win[:8]


def _pair16(off):
    """word 0 of a stored H16 row whose lanes 0, 1 hold src bytes off, off+1"""
    return src_byte(off) | src_byte(off + 1) << 16


def _vpg(spec, st, regs):
    _, off, stride = spec
    want = [_pair16(off + stride * k) for k in range(4)]
    if st != 1:
        return 'status %d' % st, False
    if list(regs) == want:
        return 'a lone vldp8 at a stride of 4 loads each row\'s first-line lanes ' \
            '(phases 12, 0, 4, 8): jv_yuv16\'s gather is right', True
    if not any(regs):
        return 'a lone vldp8 at a stride of 4 loaded nothing: jv_yuv16\'s gather ' \
            'is wrong on silicon', False
    return 'rows 0-3 word 0 %s (want %s)' % (' '.join('%08x' % x for x in regs),
                                           ' '.join('%08x' % x for x in want)), False


def _vpgx(spec, st, regs):
    _, off = spec
    if st != 1:
        return 'status %d' % st, False
    first = regs[0] == _pair16(off)
    if regs[1] == 0:
        how = 'drops the lanes past the line (a vldq is needed: as modelled)'
    elif regs[1] == _pair16(off + 2):
        how = 'fetches the lanes past the line too (vldp alone suffices)'
    else:
        how = 'lanes 2-3 read %08x' % regs[1]
    ok = first and regs[1] == 0
    return 'vldp8 alone from +14: lanes 0-1 %s; %s' % (
        'right' if first else 'wrong (%08x)' % regs[0], how), ok


def _vpqs(spec, st, regs):
    _, off, stride = spec
    want = [src_byte(off + stride * k + 14) | src_byte(off + stride * k + 15) << 16
            for k in range(4)]
    if st != 1:
        return 'status %d' % st, False
    if list(regs) == want:
        return 'a vldp8/vldq8 pair REP 4 at a stride of 20: every row right (each ' \
            'splits at its own line)', True
    bad = [k for k in range(4) if regs[k] != want[k]]
    return 'a vldp8/vldq8 pair REP 4 at a stride of 20: rows %s wrong (lanes 14-15 ' \
        'read %s; want %s): the split does not follow each row' % (
            ' '.join(str(k) for k in bad), ' '.join('%08x' % regs[k] for k in bad),
            ' '.join('%08x' % want[k] for k in bad)), False


def _dma(spec, st, regs, win):
    _, so, do, unit, units = spec
    n = unit * units
    data = bytes(src_byte(so + i) for i in range(n))
    cand = [('moved nothing', buf_after([])),
            ('copies exactly', buf_after([(do, data)])),
            ('copies with both ends rounded down to 4', buf_after(
                [(do & ~3, bytes(src_byte((so & ~3) + i) for i in range(n)))])),
            ('copies whole words a unit (the model\'s guess)', buf_after(
                [(do + unit * k, bytes(src_byte(so + unit * k + i) for i in range(4)))
                 for k in range(units)]))]
    if unit == 32 and do & 31:
        cand.insert(1, ('copies to the destination rounded down to 32',
                        buf_after([(do & ~31, data)])))
    what = next((nm for nm, v in cand if v[:8] == win[:8]), 'other (%08x %08x)' % (
        win[0], win[1]))
    if st == 4:
        return 'timed out (status %08x, config %08x): %s' % (regs[0], regs[1], what), False
    want = buf_after([(do & ~31 if unit == 32 else do, data)])
    return '%s in %d us (status %08x)' % (what, regs[2], regs[0]), want[:8] == win[:8]


# ---- the host port's checks: the reading the rules expect
PORT = {
    'PWD0': ('exact', 'word data at +0 (control)'),
    'PWD2': ('low2', 'word data at +2'),
    'PWD4': ('exact', 'word data at +4'),
    'PHD2': ('?', 'halfword data at +2'),
    'PAD2': ('?', "Apple's forms (halfword address and data, no poll) at +2"),
    'P164': ('exact', '16 bytes at +4'), 'P168': ('exact', '16 bytes at +8'),
    'P16C': ('exact', '16 bytes at +12'),
    # sitting 2 [silicon]: the odd last halfword of a run of halfword
    # data writes is lost when the next write goes to a new address (the
    # old contents stay); our writers never leave one (PLW3's word tail)
    'PLH1': ('lost', 'one lone halfword, then a word elsewhere'),
    'PLH3': ('lost', 'three halfwords, then a word elsewhere'),
    'PLH5': ('lost', 'five halfwords, then a word elsewhere'),
    'PLW3': ('exact', 'three halfwords by words (ours: zero high half), then a word'),
    'R602': ('low2', '301 x 7 at pitch 602 (engine 2, halfword tail)'),
    'R604': ('exact', '301 x 7 at pitch 604 (engine 2, halfword tail)'),
    'R608': ('exact', '301 x 7 at pitch 608 (engine 2, halfword tail)'),
    'R64W': ('exact', '301 x 7 at pitch 604, our word tail'),
    'E2ND': ('exact', 'engine 2, then at once a new address and a word'),
    'E2DR': ('exact', 'engine 2, the port drained, then a new address and a word'),
    'RX13': ('exact', 'rect x 1 w 3 by the read-modify-write rule'),
    'RX1W': ('exact', 'rect x 1 w 301 by the read-modify-write rule'),
}


PORT_FILL = 0xF000      # the port checks' fill: halfword k reads 0xF000 | k


def _port(rec):
    want, what = PORT.get(rec['tag'], ('?', '?'))
    bad, low2, tot = rec['bad'], rec['low2'], rec['of']
    first = re.findall(r'\+([0-9a-f]+) ([0-9a-f]{4}) ([0-9a-f]{4})', rec['first'])
    if bad == 0:
        got = 'exact'
        text = 'lands exactly'
    elif bad == 1 and low2 == 0 and len(first) == 1 and \
            int(first[0][1], 16) == PORT_FILL | int(first[0][0], 16) // 2:
        got = 'lost'
        text = 'the last halfword is lost (+%s keeps the fill %s; %s was written)' % first[0]
    elif low2 * 10 >= bad * 8:
        got = 'low2'
        text = 'lands 2 bytes low (%d of %d halfwords): the address\'s bits 1:0 ignored' % (
            bad, tot)
    else:
        got = 'other'
        text = '%d of %d halfwords wrong (%d of them 2 bytes low)%s' % (
            bad, tot, low2, rec['first'])
    modelled = want == '?' or got == want
    return '%s: %s%s' % (what, text, '' if modelled else ' (expected %s)' % {
        'exact': 'exact', 'low2': '2 bytes low', 'lost': 'the last halfword lost'}[want]), \
        modelled


def parse_line(cur, line):
    """Add one ALGN line to the run dict cur; True if it was one."""
    a = cur.setdefault('algn', {'recs': {}, 'port': {}, 'stopped': None, 'n': None})
    m = ALGN.match(line)
    if m:
        w = [int(x, 16) for x in m.group(3).split()]
        a['recs'].setdefault(int(m.group(1)), {}).update(
            tag=m.group(2), status=w[0], pc=w[1], at=w[2], regs=w[3:7])
        return True
    m = ALGNB.match(line)
    if m:
        a['recs'].setdefault(int(m.group(1)), {})['win'] = [
            int(x, 16) for x in m.group(2).split()]
        return True
    m = ALGNN.match(line)
    if m:
        a['n'] = int(m.group(1))
        a['src'] = int(m.group(3), 16)
        a['buf'] = int(m.group(4), 16)
        return True
    m = ALGNS.match(line)
    if m:
        a['stopped'] = (int(m.group(1)), m.group(2), m.group(3))
        return True
    m = ALGNPE.match(line)
    if m:
        a['port'][m.group(2)] = dict(tag=m.group(2), err=int(m.group(3)),
                                     k=int(m.group(1)))
        return True
    m = ALGNP.match(line)
    if m:
        a['port'][m.group(2)] = dict(tag=m.group(2), bad=int(m.group(3)),
                                     low2=int(m.group(4)), of=int(m.group(5)),
                                     first=m.group(6))
        return True
    return False


def verdicts(algn):
    """{tag: (state, text, modelled)} for every ALGN record of a run
    (state 'ran', 'fault', 'timeout', 'hung', 'port')."""
    out = {}
    if not algn:
        return out
    for k in sorted(algn['recs']):
        r = algn['recs'][k]
        if 'tag' not in r:
            continue
        tag, st = r['tag'], r['status'] & 0xFF
        vec = (r['status'] >> 8) & 0xFF
        win = r.get('win', [0] * 8)
        spec = CHECKS.get(tag)
        state = {1: 'ran', 3: 'fault', 4: 'timeout'}.get(st, 'st%d' % st)
        if spec is None:
            out[tag] = (state, 'no readings for this tag (algnlog.CHECKS)', True)
            continue
        kind = spec[0]
        if kind == 'load':
            text, ok = _load(spec, st, vec, r['pc'], r['at'], r['regs'], algn.get('src'))
        elif kind == 'store':
            text, ok = _store(spec, st, vec, r['pc'], r['at'], r['regs'], win,
                              algn.get('buf'))
        elif kind == 'vld':
            text, ok = _vld(spec, st, win)
        elif kind == 'vst':
            text, ok = _vst(spec, st, win)
        elif kind == 'vpg':
            text, ok = _vpg(spec, st, r['regs'])
        elif kind == 'vpgx':
            text, ok = _vpgx(spec, st, r['regs'])
        elif kind == 'vpqs':
            text, ok = _vpqs(spec, st, r['regs'])
        else:
            text, ok = _dma(spec, st, r['regs'], win)
        if not ok:
            text += ' (NOT as modelled: %s)' % MODEL[kind]
        out[tag] = (state, text, ok)
    if algn.get('stopped'):
        k, tag, why = algn['stopped']
        out[tag] = ('hung', 'check %d stopped the run: %s' % (k, why), False)
    for tag, rec in algn['port'].items():
        if 'err' in rec:
            names = list(PORT)
            after = names[names.index(tag) + 1:] if tag in names else []
            out[tag] = ('error', '%s: the write failed (port error %d)%s' % (
                PORT.get(tag, ('?', '?'))[1], rec['err'],
                '; %s did not run' % ', '.join(after) if after and rec['err'] == -1
                else ''), False)
            continue
        text, ok = _port(rec)
        out[tag] = ('port', text, ok)
    return out


def report(algn, p):
    v = verdicts(algn)
    if not v:
        return
    p('ALGN (alignment, bench row ALGN): %d checks%s' % (
        len(v), '' if algn.get('n') is None else ', %d BCM-side' % algn['n']))
    for tag, (state, text, ok) in v.items():
        p('  %-4s %-7s %s' % (tag, state, text))
