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

"""ShellCJK font cut A plus the CP932 additions (#235, owner 2026-11-09).

    apps/ipodui/fonts/cjk_cut.py [--write]

The one definition of which East Asian characters the shell's CJK
companion fonts carry. Enumerated from Python's codecs, as in the #235
brief (docs/design/cjk-text-and-sort-tags.md, tests/perf/cjk_font_measure.py
on p20-cjk-brief-rev):
  - kana and CJK punctuation: U+3000..U+303F, U+3041..U+3096,
    U+3099..U+30FF, U+31F0..U+31FF;
  - full-width ASCII and half-width kana: U+FF01..U+FF9F;
  - Hangul compatibility jamo U+3131..U+318E and all 11,172 syllables;
  - JIS X 0208 level 1 and level 2 kanji (EUC-JP rows 16..84);
  - the CP932 additions: CP932's two-byte characters that are not in
    JIS X 0208 (NEC and IBM extensions: 髙, 﨑 ...), private use excluded;
  - GB2312 level 1 (3,755 hanzi, rows 16..55) and Big5 level 1 (5,401
    hanzi, A440..C67E): the commonest simplified and traditional Chinese
    characters. 2,065 of them are not in JIS X 0208 (伦, 东, 張 ...).
    gen_cjk.py draws those from Noto Sans CJK SC (GB2312 ones) or TC
    (the Big5-only ones) so the shapes are the Chinese forms.

--write regenerates cjk-cut-A.txt (one range per line, "XXXX" or
"XXXX-YYYY", hex, inclusive), which the font generator (gen_cjk.py) and
scripts/check_ja_glyphs.py read. Without it, prints the counts.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CUT_FILE = os.path.join(HERE, 'cjk-cut-A.txt')


def jis0208():
    out = set()
    for row in range(0xA1, 0xFF):
        for cell in range(0xA1, 0xFF):
            try:
                ch = bytes([row, cell]).decode('euc_jp')
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                out.add(ord(ch))
    return out


def jis_kanji():
    out = set()
    for row in range(16, 85):
        for cell in range(1, 95):
            try:
                ch = bytes([0xa0 + row, 0xa0 + cell]).decode('euc_jp')
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                out.add(ord(ch))
    return out


def cp932_additions():
    cp = set()
    for lead in list(range(0x81, 0xA0)) + list(range(0xE0, 0xFD)):
        for t in list(range(0x40, 0x7F)) + list(range(0x80, 0xFD)):
            try:
                ch = bytes([lead, t]).decode('cp932')
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                cp.add(ord(ch))
    return {c for c in cp - jis0208() if not 0xE000 <= c <= 0xF8FF}


def _dbcs(codec, rows, cells):
    out = set()
    for a in rows:
        for b in cells:
            try:
                ch = bytes([a, b]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(ch) == 1:
                out.add(ord(ch))
    return out


def gb2312_l1():
    return _dbcs('gb2312', range(0xB0, 0xD8), range(0xA1, 0xFF))


def big5_l1():
    return _dbcs('big5', range(0xA4, 0xC7), range(0x40, 0xFF))


def cut_a():
    s = base_cut()
    s |= gb2312_l1()
    s |= big5_l1()
    return s


def region_of(c):
    """which source face draws code point c: 'jp' (cut A and CP932, the
    shapes the shell has always had), 'sc' (GB2312 level 1 not in cut A)
    or 'tc' (Big5 level 1 only)"""
    if c in _BASE:
        return 'jp'
    return 'sc' if c in _GB else 'tc'


def base_cut():
    s = set()
    for a, b in ((0x3000, 0x303F), (0x3041, 0x3096), (0x3099, 0x30FF),
                 (0x31F0, 0x31FF), (0xFF01, 0xFF9F), (0x3131, 0x318E),
                 (0xAC00, 0xD7A3)):
        s.update(range(a, b + 1))
    s |= jis_kanji()
    s |= cp932_additions()
    return s


_BASE = base_cut()
_GB = gb2312_l1()


def to_ranges(codes):
    out, codes = [], sorted(codes)
    i = 0
    while i < len(codes):
        j = i
        while j + 1 < len(codes) and codes[j + 1] == codes[j] + 1:
            j += 1
        out.append((codes[i], codes[j]))
        i = j + 1
    return out


def read_cut(path=CUT_FILE):
    """The committed cut as a set of code points (raises OSError)."""
    s = set()
    for line in open(path, encoding='ascii'):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        a, _, b = line.partition('-')
        s.update(range(int(a, 16), int(b or a, 16) + 1))
    return s


def main():
    s = cut_a()
    if '--write' in sys.argv[1:]:
        with open(CUT_FILE, 'w', encoding='ascii') as f:
            f.write('# ShellCJK cut A + CP932 additions + GB2312/Big5 level 1 (#235): %d code points.\n'
                    '# Generated by cjk_cut.py --write; do not edit.\n' % len(s))
            for a, b in to_ranges(s):
                f.write('%04X\n' % a if a == b else '%04X-%04X\n' % (a, b))
        print('wrote %s: %d code points' % (CUT_FILE, len(s)))
    else:
        print('cut: %d code points (base %d, +%d GB2312/Big5 level 1), '
              '%d CP932 additions, %d JIS kanji'
              % (len(s), len(_BASE), len(s - _BASE), len(cp932_additions()),
                 len(jis_kanji())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
