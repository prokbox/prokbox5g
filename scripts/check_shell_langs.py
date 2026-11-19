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

"""The shell's languages (P20): every translation complete and drawable.

    scripts/check_shell_langs.py [--root DIR] [--lang L ...]

For each language Settings > Language offers besides English
(apps/ipodui/screens/language.c: deutsch, nederlands, japanese), against
apps/lang/english.lang:
  - every phrase with a shell target (ipodui, ipodui_devmode,
    ipodui_modules) has a translation for each of its targets (a missing
    one would show English inside the other language);
  - its printf conversions are the English ones, in the same order (the
    C code passes arguments by position);
  - every character is in the shell fonts (ShellSans Bold 15, Bold 12 and
    Regular 12: U+0020..U+0237; no curly quotes, dashes, ellipsis, euro).
    Japanese (#266) may also use the ShellCJK companions: a character
    from U+3000 up must be in the cut (apps/ipodui/fonts/cjk-cut-A.txt)
    and in the companion's windows (font_sparse.h), and, when the generated
    .fnt files are at hand (apps/ipodui/fonts or build/cjk-fonts), have a
    real glyph in all three. Japanese also checks the Rockbox core strings
    the shell shows (the LANG_ ids apps/ipodui uses without a shell target:
    Off, On, Settings, Backlight ...), which come from japanese.lang's '*'
    lines. A kanji outside the cut fails: never a missing glyph;
  - "Rockbox" only in the LEGAL_ and NOTICE_ credits (CLAUDE.md rule 1);
  - no "developer" in a release (ipodui) string (validate.sh's sealed
    release check counts the word in the built .lng files).
With --lng BUILD (a built shell tree, e.g. build/sim-ui), also: every
translated shell string survives into BUILD/apps/lang/<lang>.lng as the
translation. tools/updatelang swaps English in for a translation it
refuses (a broken placeholder) and still exits 0, so a string the .lang
file translates but the .lng carries in English fails here, with a count
per language.
English's own shell strings get the glyph check too. Prints the number of
strings checked per language; exits 1 on any failure, 2 if it could not
read its inputs (a check that read nothing never passes).
"""
import argparse, glob, os, re, struct, sys

LANGS = ('deutsch', 'nederlands', 'japanese')
CJK_LANGS = ('japanese',)
# The companion's code windows and bit bases (firmware/export/font_sparse.h
# font_sparse_bit(), 0.9.0's sparse ShellCJK form; gen_cjk.py WINDOWS)
CJK_WINDOWS = ((0x3000, 0xD7A4, 0), (0xF900, 0xFB00, 42916),
               (0xFF00, 0xFFF0, 43428))
SPARSE_DEPTH, SPARSE_MAGIC = 0x100, 0x53524853
SPARSE_MAPOFF, SPARSE_BITMAP = 52, 5460
CJK_FONTS = ('14-ShellCJK-Bold.fnt', '11-ShellCJK-Bold.fnt', '11-ShellCJK.fnt')
TARGETS = ('ipodui', 'ipodui_devmode', 'ipodui_modules')
FONTS = ('15-ShellSans-Bold.fnt', '12-ShellSans-Bold.fnt', '12-ShellSans.fnt')
CONV = re.compile(r'%(?:%|[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z)?[diouxXcsp])')


def phrases(path):
    txt = open(path, encoding='utf-8').read()
    out = {}
    for b in re.findall(r'<phrase>(.*?)</phrase>', txt, re.S):
        pid = re.search(r'id:\s*(\S+)', b).group(1)
        d = {}
        for sec in ('source', 'dest'):
            m = re.search(r'<%s>(.*?)</%s>' % (sec, sec), b, re.S)
            d[sec] = dict(re.findall(r'^\s*([^:\n]+?):\s*"(.*)"\s*$',
                                     m.group(1), re.M)) if m else {}
        out[pid] = d
    return out


def font_range(path):
    d = open(path, 'rb').read()
    # RB12 header (apps/ipodui/fonts/fntcheck.py load()): magic, 4 shorts,
    # then firstchar, defaultchar, size
    first, _dflt, size = struct.unpack('<3I', d[12:24])
    return first, first + size


def cjk_glyphs(root):
    """Code points with a real glyph in every generated ShellCJK font, or
    None if they are not at hand (the cut list alone decides then)."""
    for d in ('apps/ipodui/fonts', 'build/cjk-fonts'):
        paths = [os.path.join(root, d, f) for f in CJK_FONTS]
        if all(os.path.exists(p) for p in paths):
            break
    else:
        return None, None
    have = None
    for p in paths:
        s = font_glyphs(p)
        have = s if have is None else have & s
    return have, os.path.dirname(paths[0])


def font_glyphs(p):
    """The code points with a glyph in one ShellCJK .fnt: the sparse form
    (presence bitmap, font_sparse.h) or the older in-memory RB12 one
    (offset table, the hole is the default char's offset). Raises
    ValueError when the file is neither: a font the check cannot read
    must not pass as "every glyph missing" or "every glyph there"."""
    b = open(p, 'rb').read()
    if len(b) < 36 or b[:4] != b'RB12':
        raise ValueError('%s: not an RB12 font' % p)
    _mw, _h, _a, depth, first, dflt, size, nbits, noff, _nw = \
        struct.unpack('<4H6I', b[4:36])
    if depth & SPARSE_DEPTH:
        if (len(b) < SPARSE_MAPOFF + SPARSE_BITMAP
                or struct.unpack_from('<I', b, 36)[0] != SPARSE_MAGIC):
            raise ValueError('%s: sparse header damaged' % p)
        m = b[SPARSE_MAPOFF:SPARSE_MAPOFF + SPARSE_BITMAP]
        return {lo + i for lo, hi, base in CJK_WINDOWS
                for i in range(hi - lo)
                if (m[(base + i) >> 3] >> ((base + i) & 7)) & 1}
    o = 36 + nbits
    try:
        if nbits >= 0xFFDB:
            o = (o + 3) & ~3
            offs = struct.unpack_from('<%dI' % noff, b, o)
        else:
            o = (o + 1) & ~1
            offs = struct.unpack_from('<%dH' % noff, b, o)
        hole = offs[dflt - first]
    except (struct.error, IndexError):
        raise ValueError('%s: offset table damaged' % p)
    s = {first + i for i in range(min(size, len(offs))) if offs[i] != hole}
    s.add(dflt)
    return s


def core_ids(root):
    """The non-shell LANG_ ids the shell code shows."""
    ids = set()
    for p in glob.glob(os.path.join(root, 'apps/ipodui/**/*.[ch]'),
                       recursive=True):
        ids |= set(re.findall(r'\bLANG_[A-Z0-9_]+',
                              open(p, encoding='utf-8', errors='replace').read()))
    return ids


def lng_strings(path, enum):
    """id name -> string of a built .lng (tools/genlang: a 4-byte header,
    or 8 with a shell build's string-table hash (version 0x87), 6 bytes
    per user, then "<id hi><id lo><string>\\0" entries)."""
    b = open(path, 'rb').read()
    h = 8 if len(b) > 1 and b[1] == 0x87 else 4
    if len(b) < h + 6 or b[0] != 0x1a:
        raise ValueError('%s: not a .lng file' % path)
    count, size, off = struct.unpack('>3H', b[h:h + 6])
    out, o, end = {}, off, off + size
    for _ in range(count):
        if o + 2 > end:
            break
        i = (b[o] << 8) | b[o + 1]
        z = b.index(b'\0', o + 2)
        if i < len(enum):
            out[enum[i]] = b[o + 2:z].decode('utf-8', 'replace')
        o = z + 1
    return out


def lng_survival(build, langs, shell, tr_of):
    """[failures], {lang: (translated, survived)}"""
    bad, counts = [], {}
    enum = re.findall(r'^\s*(LANG_\w+|VOICE_\w+),', open(
        os.path.join(build, 'lang_enum.h')).read(), re.M)
    if len(enum) < 100:
        raise ValueError('%s/lang_enum.h: only %d ids' % (build, len(enum)))
    for lang in langs:
        got = lng_strings(os.path.join(build, 'apps/lang/%s.lng' % lang),
                          enum)
        tr = tr_of(lang)
        n = ok = 0
        for pid, src in shell.items():
            if 'ipodui' not in src:
                continue
            d = tr.get(pid, {}).get('dest', {}).get('ipodui')
            if not d or d == 'none':
                continue
            d = re.sub(r'^~+', '', d)
            if d == src['ipodui']:
                continue                # the same as English on purpose
            n += 1
            if got.get(pid) == d:
                ok += 1
            elif got.get(pid) == src['ipodui']:
                bad.append('%s %s: the built .lng carries English (%r), '
                           'not the translation %r' % (lang, pid,
                                                        src['ipodui'], d))
            else:
                bad.append('%s %s: the built .lng carries %r, not %r'
                           % (lang, pid, got.get(pid), d))
        if n < 100:
            bad.append('%s: only %d translated strings to look for in the '
                       '.lng' % (lang, n))
        counts[lang] = (n, ok)
    return bad, counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    ap.add_argument('--lang', action='append', choices=LANGS,
                    help='check only this language (repeatable)')
    ap.add_argument('--lng', metavar='BUILD',
                    help='also check the built .lng files of this shell '
                         'build tree (every translation survives)')
    a = ap.parse_args()
    langs = tuple(a.lang) if a.lang else LANGS
    try:
        en = phrases(os.path.join(a.root, 'apps/lang/english.lang'))
        ranges = [font_range(os.path.join(a.root, 'apps/ipodui/fonts', f))
                  for f in FONTS]
    except (OSError, AttributeError) as e:
        print('FAIL: cannot read inputs: %s' % e)
        return 2
    lo = max(r[0] for r in ranges)
    hi = min(r[1] for r in ranges)
    sys.path.insert(0, os.path.join(a.root, 'apps/ipodui/fonts'))
    try:
        import cjk_cut
        cut = cjk_cut.read_cut(os.path.join(a.root,
                                            'apps/ipodui/fonts/cjk-cut-A.txt'))
    except (OSError, ImportError) as e:
        print('FAIL: cannot read the CJK cut: %s' % e)
        return 2
    if len(cut) < 18000:
        print('FAIL: the CJK cut holds only %d code points' % len(cut))
        return 2
    try:
        fnt, fntdir = cjk_glyphs(a.root)
    except (OSError, ValueError) as e:
        print('FAIL: cannot read the ShellCJK fonts: %s' % e)
        return 2
    print('ok   CJK cut: %d code points; glyphs from %s'
          % (len(cut), ('%d in %s' % (len(fnt), fntdir)) if fnt
             else 'the cut list only (no ShellCJK .fnt at hand)'))
    shell = {pid: {t: s for t, s in d['source'].items() if t in TARGETS}
             for pid, d in en.items()}
    shell = {pid: v for pid, v in shell.items() if v}
    if len(shell) < 100:
        print('FAIL: only %d shell phrases in english.lang' % len(shell))
        return 2
    bad = []

    def drawn(lang, c):
        o = ord(c)
        if lo <= o < hi:
            return True
        if lang not in CJK_LANGS or not any(lo <= o < hi
                                            for lo, hi, _ in CJK_WINDOWS):
            return False
        return o in cut and (fnt is None or o in fnt)

    def glyphs(lang, pid, s):
        miss = sorted({c for c in s if not drawn(lang, c)})
        if miss:
            bad.append('%s %s: not in the shell fonts: %s'
                       % (lang, pid, ' '.join('U+%04X' % ord(c) for c in miss)))

    for pid, src in shell.items():
        for s in src.values():
            glyphs('english', pid, s)
    print('ok   english: %d shell strings in U+%04X..U+%04X'
          % (sum(len(v) for v in shell.values()), lo, hi - 1))
    for lang in langs:
        try:
            tr = phrases(os.path.join(a.root, 'apps/lang/%s.lang' % lang))
        except OSError as e:
            print('FAIL: cannot read %s.lang: %s' % (lang, e))
            return 2
        n = 0
        for pid, src in shell.items():
            dest = tr.get(pid, {}).get('dest', {})
            for t, s in src.items():
                if t not in dest or not dest[t]:
                    bad.append('%s %s: no %s translation' % (lang, pid, t))
                    continue
                d = dest[t]
                n += 1
                if CONV.findall(d) != CONV.findall(s):
                    bad.append('%s %s: conversions %s, English %s'
                               % (lang, pid, CONV.findall(d), CONV.findall(s)))
                glyphs(lang, pid, d)
                if 'rockbox' in d.lower() and not re.match(
                        r'LANG_IPODUI_(LEGAL|NOTICE)_', pid):
                    bad.append('%s %s: names Rockbox: %r' % (lang, pid, d))
                if t == 'ipodui' and 'developer' in d.lower():
                    bad.append('%s %s: "developer" in a release string'
                               % (lang, pid))
        if lang in CJK_LANGS:
            shown = core_ids(a.root) - set(shell)
            k = 0
            for pid in sorted(shown):
                d = tr.get(pid, {}).get('dest', {}).get('*')
                if d and d != 'none':
                    glyphs(lang, pid, d)
                    k += 1
            if k < 10:
                bad.append('%s: only %d core strings found' % (lang, k))
            n += k
        print('ok   %s: %d shell strings checked' % (lang, n)
              if not any(b.startswith(lang + ' ') for b in bad)
              else '     %s: %d shell strings checked' % (lang, n))
    if a.lng:
        try:
            lbad, counts = lng_survival(
                a.lng, langs, shell,
                lambda l: phrases(os.path.join(a.root,
                                               'apps/lang/%s.lang' % l)))
        except (OSError, ValueError) as e:
            print('FAIL: cannot read the built .lng files: %s' % e)
            return 2
        for lang, (n, ok) in counts.items():
            print('%s   %s: %d of %d translations in %s/apps/lang/%s.lng'
                  % ('ok ' if n == ok else '   ', lang, ok, n, a.lng, lang))
        bad += lbad
    for b in bad:
        print('FAIL: ' + b)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
