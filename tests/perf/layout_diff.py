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

"""Hot-symbol and IRAM layout diff of two firmware images (one-build WP5,
the simplified performance gate; out/briefs/one-build-brief.md section 8).

    tests/perf/layout_diff.py BASE.elf NEW.elf [--hot FILE] [--max-iram-grow B]
                              [--allow-removed FILE] [--allow-grown FILE]

Prints, for BASE and NEW:
  - _end and the size of every loaded section (.text .rodata .data .iram
    .ibss .init .bss .ncbss);
  - IRAM: every symbol in .iram/.ibss (0x40000000..), by name: moved
    (address), resized, added or removed. IRAM holds the hottest code and
    data on purpose (codecs, PCM, the DSP), so any change is listed;
  - hot code outside IRAM: every function matching a pattern in the hot
    list (default tests/perf/hot_symbols.txt; "!pattern" excludes): its
    size, and whether its
    16-byte cache-line offset (address % 16) changed (information only:
    any growth before it moves it). A plain address shift is expected
    whenever code before it grows and is not reported.

--allow-removed FILE (default tests/perf/removed_ok.txt) lists IRAM
symbols whose removal is intended, one per line: "<symbol> <reason>"; a
line without a reason is a usage error (exit 2).

--allow-grown FILE (default tests/perf/grown_ok.txt) lists hot functions
whose growth past 64 B is intended: "<symbol> <max size in bytes> <reason>".
The function may be at most that big in NEW; a larger size is a failure
(exit 1), a line without a size or reason a usage error (exit 2).

Exit 1 when an IRAM symbol of BASE is missing from NEW (linker veneers
and allow-listed symbols excepted), a hot function
grew by more than 64 B, or IRAM grew by more than --max-iram-grow bytes
(default 64); exit 2 on a usage error or when either image has no
symbols (a gate that inspected nothing must not pass). Uses
arm-none-eabi-nm and arm-none-eabi-objdump.
"""
import fnmatch
import os
import subprocess
import sys

NM = 'arm-none-eabi-nm'
OBJDUMP = 'arm-none-eabi-objdump'
SECTIONS = ('.text', '.rodata', '.data', '.iram', '.ibss', '.init', '.bss',
            '.ncbss')
IRAM_LO, IRAM_HI = 0x40000000, 0x40020000
LINE = 16
HERE = os.path.dirname(os.path.abspath(__file__))


def sections(elf):
    out = subprocess.run([OBJDUMP, '-h', elf], capture_output=True,
                         text=True, check=True).stdout
    got = {}
    for l in out.splitlines():
        f = l.split()
        if len(f) >= 7 and f[0].isdigit():
            got[f[1]] = int(f[2], 16)
    return got


def symbols(elf):
    """name -> (addr, size, type) for every sized symbol."""
    out = subprocess.run([NM, '-S', '--defined-only', elf],
                         capture_output=True, text=True, check=True).stdout
    got, end = {}, None
    for l in out.splitlines():
        f = l.split()
        if len(f) == 3 and f[2] == '_end':
            end = int(f[0], 16)
        if len(f) != 4:
            continue
        addr, size, typ, name = int(f[0], 16), int(f[1], 16), f[2], f[3]
        got[name] = (addr, size, typ)
    return got, end


def hot_patterns(path):
    pats = []
    for l in open(path):
        l = l.split('#', 1)[0].strip()
        if l:
            pats.append(l)
    return pats


def main(argv):
    args = [a for a in argv[1:]]
    hot_file = os.path.join(HERE, 'hot_symbols.txt')
    max_iram = 64
    allow_file = os.path.join(HERE, 'removed_ok.txt')
    while '--allow-removed' in args:
        i = args.index('--allow-removed')
        allow_file = args[i + 1]
        del args[i:i + 2]
    grown_file = os.path.join(HERE, 'grown_ok.txt')
    while '--allow-grown' in args:
        i = args.index('--allow-grown')
        grown_file = args[i + 1]
        del args[i:i + 2]
    while '--hot' in args:
        i = args.index('--hot')
        hot_file = args[i + 1]
        del args[i:i + 2]
    while '--max-iram-grow' in args:
        i = args.index('--max-iram-grow')
        max_iram = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    base, new = args
    allowed = {}
    if os.path.exists(allow_file):
        for l in open(allow_file):
            l = l.split('#', 1)[0].strip() if l.lstrip().startswith('#') else l.strip()
            if not l:
                continue
            f = l.split(None, 1)
            if len(f) < 2 or not f[1].strip():
                print('layout_diff: %s: "%s" has no reason' % (allow_file, l), file=sys.stderr)
                return 2
            allowed[f[0]] = f[1].strip()
    grown_ok = {}
    if os.path.exists(grown_file):
        for l in open(grown_file):
            l = l.strip()
            if not l or l.startswith('#'):
                continue
            f = l.split(None, 2)
            if len(f) < 3 or not f[1].isdigit() or not f[2].strip().strip('"'):
                print('layout_diff: %s: "%s" needs <symbol> <max bytes> <reason>'
                      % (grown_file, l), file=sys.stderr)
                return 2
            grown_ok[f[0]] = (int(f[1]), f[2].strip().strip('"'))
    sb, eb = symbols(base)
    sn, en = symbols(new)
    if not sb or not sn or eb is None or en is None:
        print('layout_diff: no symbols or no _end in an image', file=sys.stderr)
        return 2
    secb, secn = sections(base), sections(new)
    bad = []
    print('_end %d -> %d (%+d B)' % (eb, en, en - eb))
    for s in SECTIONS:
        a, b = secb.get(s, 0), secn.get(s, 0)
        print('%-8s %8d -> %8d (%+d)' % (s, a, b, b - a))
    iram_grow = (secn.get('.iram', 0) + secn.get('.ibss', 0)) - \
        (secb.get('.iram', 0) + secb.get('.ibss', 0))
    if iram_grow > max_iram:
        bad.append('IRAM grew %d B (> %d)' % (iram_grow, max_iram))

    def in_iram(v):
        return IRAM_LO <= v[0] < IRAM_HI and v[1] > 0
    ib = {k: v for k, v in sb.items() if in_iram(v)}
    inn = {k: v for k, v in sn.items() if in_iram(v)}
    moved = sorted(k for k in ib if k in inn and ib[k][0] != inn[k][0])
    resized = sorted(k for k in ib if k in inn and ib[k][1] != inn[k][1])
    gone = sorted(set(ib) - set(inn))
    added = sorted(set(inn) - set(ib))
    print('IRAM symbols: base %d, new %d; moved %d, resized %d, removed %d, added %d'
          % (len(ib), len(inn), len(moved), len(resized), len(gone), len(added)))
    for k in moved:
        print('  moved   %-40s 0x%08x -> 0x%08x' % (k, ib[k][0], inn[k][0]))
    for k in resized:
        print('  resized %-40s %d -> %d' % (k, ib[k][1], inn[k][1]))
    for k in gone:
        if k in allowed:
            print('  removed %-40s %d B (allowed: %s)' % (k, ib[k][1], allowed[k]))
            continue
        print('  removed %-40s %d B' % (k, ib[k][1]))
        if not k.endswith('_veneer'):   # a linker long-call stub: one call fewer
            bad.append('IRAM symbol %s removed' % k)
    for k in added:
        print('  added   %-40s %d B' % (k, inn[k][1]))

    pats = hot_patterns(hot_file)
    neg = [p[1:] for p in pats if p.startswith('!')]
    pos = [p for p in pats if not p.startswith('!')]
    hot = sorted(k for k in sb if sb[k][2] in 'tT' and not in_iram(sb[k])
                 and any(fnmatch.fnmatchcase(k, p) for p in pos)
                 and not any(fnmatch.fnmatchcase(k, p) for p in neg))
    if not hot:
        print('layout_diff: no hot function matched %s' % hot_file, file=sys.stderr)
        return 2
    grew, realign, missing = [], [], []
    for k in hot:
        if k not in sn:
            missing.append(k)
            continue
        (a0, s0, _), (a1, s1, _) = sb[k], sn[k]
        if s1 != s0:
            grew.append((k, s0, s1))
            if s1 - s0 > 64:
                if k in grown_ok and s1 <= grown_ok[k][0]:
                    print('  grown   %-40s %d -> %d (allowed to %d: %s)'
                          % (k, s0, s1, grown_ok[k][0], grown_ok[k][1]))
                else:
                    bad.append('hot %s grew %d B' % (k, s1 - s0))
        if a0 % LINE != a1 % LINE:
            realign.append((k, a0 % LINE, a1 % LINE))
    print('hot functions (outside IRAM): %d matched; size changed %d, '
          'line offset changed %d, missing in new %d'
          % (len(hot), len(grew), len(realign), len(missing)))
    for k, s0, s1 in grew:
        print('  size    %-40s %d -> %d (%+d)' % (k, s0, s1, s1 - s0))
    for k, o0, o1 in realign:
        print('  line    %-40s offset %d -> %d' % (k, o0, o1))
    for k in missing:
        print('  missing %s' % k)
    if bad:
        print('LAYOUT DIFF: FAIL: ' + '; '.join(bad))
        return 1
    print('LAYOUT DIFF: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
