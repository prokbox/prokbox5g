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

"""Generate apps/ipodui/unlit_glyphs.h from the unlit face art (stdlib only).

    python3 -I tests/unlit/gen_glyphs.py [OUT]   # default apps/ipodui/unlit_glyphs.h

The source of truth is art/unlit/unlit.py ("stock" variant, owner
2026-10-08). Each fixed glyph (digits, colon, play, padlock) is
rasterised exactly as the generator's 1x lit PNGs are (4x4 supersampling,
crisp at 50 %), and stored as its *outer* shape only: vertical runs of
identical row masks, the masks shared through one palette. The pause glyph is
two plain bars and is drawn as rectangles. The C side (unlit_core.c) paints the outer shape in the rim grey and its 4-neighbour
erosion in the ink grey, which equals the generator's "1 px lighter rim"
layer for every glyph but the colon (whose ink is stored too).

The battery varies with the level, so it stays procedural in C; this
script only exports what is not a plain rectangle: the rounded outer
corners (pixels trimmed per row) and the charging bolt as two per-row
spans (covered >= 50 %: drawn over the empty part; > 50 %: knocked out of
the fill).

tests/unlit/run.sh renders the four scenes and the glyph sheet with the C
code and requires them to be pixel-identical to docs/design/unlit/.
"""
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'art', 'unlit'))
import unlit as U  # noqa: E402

V = 'stock'


def planes(g):
    w, h, _ = g
    pw, ph = int(math.ceil(w)), int(math.ceil(h))
    r = U.raster(g, 1)
    out = []
    for cov, _ink in r[2]:
        rows = []
        for y in range(ph):
            m = 0
            for x in range(pw):
                if cov[y][x] >= 0.5:
                    m |= 1 << x
            rows.append(m)
        out.append(rows)
    return pw, ph, out


def erode(S, pw):
    full = (1 << pw) - 1
    res = []
    for y in range(len(S)):
        up = S[y - 1] if y > 0 else 0
        dn = S[y + 1] if y + 1 < len(S) else 0
        res.append(S[y] & up & dn & ((S[y] << 1) & full) & (S[y] >> 1))
    return res


def runs(rows):
    rr = []
    for m in rows:
        if rr and rr[-1][1] == m:
            rr[-1][0] += 1
        else:
            rr.append([1, m])
    return rr


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, 'apps', 'ipodui', 'unlit_glyphs.h')
    glyphs = [('d%c' % c, U.digit(V, c)) for c in '0123456789']
    glyphs += [('colon', U.colon(V)), ('play', U.play(V)),
               ('lock', U.lock(V))]   # pause: two plain bars, in C
    pal = []
    run_tab = []
    desc = []
    for name, g in glyphs:
        pw, ph, layers = planes(g)
        S, I = layers[0], layers[1]
        assert pw <= 32
        explicit = erode(S, pw) != I
        if explicit and name != 'colon':
            raise SystemExit('%s: ink is not the erosion of the shape' % name)
        lists = [S, I] if explicit else [S]
        first = []
        for rows in lists:
            first.append(len(run_tab))
            for n, m in runs(rows):
                if m not in pal:
                    pal.append(m)
                while n > 255:
                    run_tab.append((255, pal.index(m)))
                    n -= 255
                run_tab.append((n, pal.index(m)))
        desc.append((name, pw, ph, first[0], first[1] if explicit else -1))
    assert len(pal) < 256 and len(run_tab) < 65535

    # battery: outer corner trims and the bolt
    m = U.M[V]
    bw, bh = m['bw'], m['bh']
    pw, ph, layers = planes(U.battery(V, 0.0))
    S = layers[0]
    trim = []
    for y in range(ph):
        row = S[y] & ((1 << bw) - 1)
        t = 0
        while t < bw // 2 and not (row >> t) & 1:
            t += 1
        trim.append(t)
    # symmetric corners, trimmed only in the first/last rows
    nt = 0
    while trim[nt]:
        nt += 1
    assert trim[ph - 1 - nt] == 0 and all(trim[ph - 1 - k] == trim[k]
                                         for k in range(nt))
    # the bolt: coverage per pixel of the bolt polygon alone
    o = m['bo']
    sx, sy = bw / 46.0, (bh - 2 * o) / 16.0
    bolt = U.poly([(27 * sx, o), (16 * sx, o + 9 * sy),
                   (22 * sx, o + 9 * sy), (18 * sx, bh - o),
                   (31 * sx, o + 6.5 * sy), (25 * sx, o + 6.5 * sy),
                   (32 * sx, o)])
    cov = U.raster((bw, bh, [(bolt, 0)]), 1)[2][0][0]

    def spans(pred):
        sp = []
        for y in range(o, bh - o):
            xs = [x for x in range(o, bw - o) if pred(cov[y][x])]
            if xs:
                assert xs == list(range(xs[0], xs[-1] + 1)), 'bolt row split'
                sp.append((xs[0], xs[-1] + 1))
            else:
                sp.append((0, 0))
        return sp
    k1 = spans(lambda c: c >= 0.5)
    k2 = spans(lambda c: c > 0.5)

    # Phase 8b O2: where the art's analytic erosion leaves a rim pixel
    # that the 4-neighbour erosion of the pixel shape would paint ink (the
    # bolt's 45-degree steps), per fill width: (x, y, first fill, last
    # fill), merged over consecutive widths; x | 0x80 marks the reverse
    # (the art paints ink where the erosion gives rim).
    fill_max = bw - 2 * o - 4
    seen = {}
    for fp in range(fill_max + 1):
        bpw, _bph, bl = planes(U.battery(V, fp / float(fill_max), True))
        bs, bi = bl[0], bl[1]
        be = erode(bs, bpw)
        for y in range(bh):
            for x in range(bpw):
                ai, ei = (bi[y] >> x) & 1, (be[y] >> x) & 1
                if ai != ei:
                    assert x < 0x80
                    seen.setdefault((x | (ai << 7), y), []).append(fp)
    bolt_rim = []
    for (x, y), fps in sorted(seen.items(), key=lambda e: (e[0][1], e[0][0] & 0x7f)):
        lo = fps[0]
        for a, b in zip(fps, fps[1:] + [None]):
            if b != a + 1:
                bolt_rim.append((x, y, lo, a))
                lo = b

    L = []
    L.append('/* unlit_glyphs.h: GENERATED by tests/unlit/gen_glyphs.py from')
    L.append(' * art/unlit/unlit.py ("stock"); do not edit. GPL v2+.')
    L.append(' * Fixed glyphs: vertical runs {rows, palette index} of the outer')
    L.append(' * shape; ink = 4-neighbour erosion (colon: stored). */')
    L.append('#ifndef _IPODUI_UNLIT_GLYPHS_H_')
    L.append('#define _IPODUI_UNLIT_GLYPHS_H_')
    L.append('')
    L.append('enum { %s, UG_COUNT };' % ', '.join(
        'UG_%s' % d[0].upper() for d in desc))
    L.append('')
    L.append('static const uint32_t unlit_pal[%d] = {' % len(pal))
    for i in range(0, len(pal), 4):
        L.append('    ' + ' '.join('0x%08x,' % p for p in pal[i:i + 4]))
    L.append('};')
    L.append('')
    L.append('static const unsigned char unlit_runs[%d][2] = {' % len(run_tab))
    for i in range(0, len(run_tab), 6):
        L.append('    ' + ' '.join('{%d,%d},' % r for r in run_tab[i:i + 6]))
    L.append('};')
    L.append('')
    L.append('/* w, h, first run of the shape, first run of the ink (-1: '
             'erosion) */')
    L.append('static const struct unlit_glyph unlit_glyphs[UG_COUNT] = {')
    for name, w, h, f, fi in desc:
        L.append('    { %d, %d, %d, %d },   /* %s */' % (w, h, f, fi, name))
    L.append('};')
    L.append('')
    L.append('#define UNLIT_BAT_W     %d   /* outline; + %d px nub */'
             % (bw, 4))
    L.append('#define UNLIT_BAT_H     %d' % bh)
    L.append('#define UNLIT_BAT_O     %d   /* outline thickness */' % o)
    L.append('#define UNLIT_BAT_NUB   %d   /* nub height */' % m['nub'])
    L.append('#define UNLIT_BAT_FILL  %d   /* fill width at 100 %% */'
             % (bw - 2 * o - 4))
    L.append('#define UNLIT_BAT_TRIMS %d' % nt)
    L.append('static const unsigned char unlit_bat_trim[UNLIT_BAT_TRIMS] = '
             '{ %s };' % ', '.join(str(t) for t in trim[:nt]))
    L.append('/* bolt rows UNLIT_BAT_O.. : [x0, x1) over the empty part '
             '(>= 50 %), knocked out of the fill (> 50 %) */')
    L.append('static const unsigned char unlit_bolt[%d][4] = {' % len(k1))
    for a, b in zip(k1, k2):
        L.append('    { %d, %d, %d, %d },' % (a[0], a[1], b[0], b[1]))
    L.append('};')
    L.append('/* charging: pixels the art leaves rim grey where the erosion '
             'gives ink:')
    L.append(' * x (| 0x80: ink where the erosion gives rim), y, fill width '
             'from, to */')
    L.append('#define UNLIT_BOLT_RIMS %d' % len(bolt_rim))
    L.append('static const unsigned char unlit_bolt_rim[UNLIT_BOLT_RIMS][4] = {')
    for e in bolt_rim:
        L.append('    { %d, %d, %d, %d },' % e)
    L.append('};')
    L.append('')
    L.append('#endif')
    with open(out, 'w') as fp:
        fp.write('\n'.join(L) + '\n')
    prokbox_header.stamp(out)
    size = (len(pal) * 4 + len(run_tab) * 2 + len(desc) * 4 + nt +
            len(k1) * 4 + len(bolt_rim) * 4)
    print('%s: %d palette masks, %d runs, %d B of tables'
          % (out, len(pal), len(run_tab), size))


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
import os as _os  # noqa: E402
import sys as _sys  # noqa: E402
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    main()
