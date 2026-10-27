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

"""Generate apps/ipodui/checkglyph.h (stdlib only).

    art/check/check.py            # writes apps/ipodui/checkglyph.h
    art/check/check.py --ascii    # also prints the glyphs as text

Two original drawings, rasterised with 8x8 supersampling into 4-bit
coverage (0 = empty, 15 = full ink):

  * the checklist tick (Settings > Main Menu / Music Menu): a short
    left stroke and a long right stroke, 2.7 px thick with round ends;
  * the roller digits 0-9 (Volume Limit's Enter Combination, Date and
    Time): a plain rounded sans design built from strokes and arcs, 3 px
    thick, 18 x 24 px, stored two pixels per byte (high nibble first).

No Apple artwork or font is used; proportions follow the owner photo
extras-screenlock-new-combination.jpg (digit height ~ 24 of a 43 px box).
"""
import math
import os
import sys

SS = 8

# ---------------------------------------------------------------- geometry


def arc(cx, cy, rx, ry, a0, a1, n=24):
    """Points of an elliptical arc, angles in degrees, 0 = right,
    90 = down (screen coordinates), swept from a0 to a1."""
    pts = []
    for i in range(n + 1):
        a = math.radians(a0 + (a1 - a0) * i / n)
        pts.append((cx + rx * math.cos(a), cy + ry * math.sin(a)))
    return pts


def dist_poly(px, py, pts):
    best = 1e9
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy
        t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((px - ax) * dx +
                                                   (py - ay) * dy) / L2))
        d = math.hypot(px - (ax + t * dx), py - (ay + t * dy))
        best = min(best, d)
    return best


def raster(w, h, strokes, sw):
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            n = 0
            for sy in range(SS):
                for sx in range(SS):
                    X, Y = x + (sx + .5) / SS, y + (sy + .5) / SS
                    for s in strokes:
                        if dist_poly(X, Y, s) <= sw / 2:
                            n += 1
                            break
            row.append(round(15 * n / (SS * SS)))
        rows.append(row)
    return rows


# ---------------------------------------------------------------- the tick

CK_W, CK_H = 16, 13
CK_SW = 2.7
CK_STROKES = [[(1.9, 7.4), (5.6, 11.0), (14.0, 1.9)]]


def tick():
    return raster(CK_W, CK_H, CK_STROKES, CK_SW)


# ---------------------------------------------------------------- digits

B_W, B_H = 16, 22                   # the design grid
RD_W, RD_H = 18, 24                 # the stored tile (scaled up from it)
RD_SW = 3.7
X0, X1, XM = 2.6, 13.4, 8.0         # left, right, middle of the ink
Y0, Y1 = 1.8, 20.2                  # top and bottom of the ink


def digit_strokes(d):
    if d == 0:
        return [arc(XM, 11, 5.4, 9.2, 0, 360, 48)]
    if d == 1:
        return [[(5.0, 6.0), (9.4, Y0)], [(9.4, Y0), (9.4, Y1)]]
    if d == 2:
        return [arc(XM, 6.4, 5.4, 4.6, 195, 400, 24) +
                [(3.0, Y1), (X1, Y1)]]
    if d == 3:
        return [arc(XM, 6.2, 5.0, 4.4, 200, 450, 20),
                arc(XM, 14.6, 5.6, 5.6, 270, 520, 24)]
    if d == 4:
        return [[(10.6, Y0), (10.6, Y1)], [(10.6, Y0), (2.4, 14.2)],
                [(2.4, 14.2), (14.0, 14.2)]]
    if d == 5:
        return [[(12.6, Y0), (4.2, Y0), (3.6, 9.6)],
                arc(XM, 14.4, 5.7, 5.8, 250, 520, 28)]
    if d == 6:
        return [arc(XM, 14.4, 5.4, 5.8, 0, 360, 36),
                [(2.7, 14.4), (3.2, 9.0), (5.6, 4.4), (9.2, 2.2),
                 (12.0, Y0)]]
    if d == 7:
        return [[(2.4, Y0), (13.6, Y0), (6.6, Y1)]]
    if d == 8:
        return [arc(XM, 6.0, 4.6, 4.2, 0, 360, 32),
                arc(XM, 14.9, 5.5, 5.3, 0, 360, 36)]
    if d == 9:
        return [[(B_W - X, B_H - Y) for X, Y in s]
                for s in digit_strokes(6)]
    raise ValueError(d)


def scaled(strokes):
    sx, sy = RD_W / B_W, RD_H / B_H
    return [[(x * sx, y * sy) for x, y in s] for s in strokes]


def digits():
    return [raster(RD_W, RD_H, scaled(digit_strokes(d)), RD_SW)
            for d in range(10)]


# ---------------------------------------------------------------- output

def c_rows(rows):
    return ['    {' + ', '.join('%2d' % v for v in r) + '},' for r in rows]


def packed(rows):
    """Rows of 4-bit values -> bytes, high nibble first, one row after
    another (RD_W is even)."""
    out = []
    for r in rows:
        for i in range(0, len(r), 2):
            out.append(r[i] << 4 | r[i + 1])
    return out


def main():
    root = os.path.normpath(os.path.join(os.path.dirname(__file__),
                                         '..', '..'))
    path = os.path.join(root, 'apps', 'ipodui', 'checkglyph.h')
    tk = tick()
    dg = digits()
    o = []
    o.append('/* Generated by art/check/check.py -- do not edit.')
    o.append(' * 4-bit coverage, 0 (none) .. 15 (full ink). Original artwork. */')
    o.append('#ifndef _IPODUI_CHECKGLYPH_H_')
    o.append('#define _IPODUI_CHECKGLYPH_H_')
    o.append('')
    o.append('/* the checklist tick: one byte per pixel, [row][col] */')
    o.append('#define CHECK_W %d' % CK_W)
    o.append('#define CHECK_H %d' % CK_H)
    o.append('static const unsigned char check_cov[CHECK_H][CHECK_W] = {')
    o += c_rows(tk)
    o.append('};')
    o.append('')
    o.append('/* the roller digits 0-9: RD_H rows of RD_W / 2 bytes, two')
    o.append(' * pixels per byte (high nibble = left pixel) */')
    o.append('#define RD_W %d' % RD_W)
    o.append('#define RD_H %d' % RD_H)
    o.append('#define RD_ROW (RD_W / 2)')
    o.append('static const unsigned char rdigit_cov[10][RD_H * RD_ROW] = {')
    for d in range(10):
        b = packed(dg[d])
        o.append('    { /* %d */' % d)
        for i in range(0, len(b), 8):
            o.append('        ' + ', '.join('0x%02x' % v
                                            for v in b[i:i + 8]) + ',')
        o.append('    },')
    o.append('};')
    o.append('')
    o.append('#endif /* _IPODUI_CHECKGLYPH_H_ */')
    with open(path, 'w') as f:
        f.write('\n'.join(o) + '\n')
    prokbox_header.stamp(path)
    print('wrote', path)
    if '--ascii' in sys.argv:
        shade = ' .:-=+*#%@'
        for name, rows in [('tick', tk)] + [(str(i), dg[i])
                                            for i in range(10)]:
            print(name)
            for r in rows:
                print(''.join(shade[min(9, v * 10 // 16)] * 2 for v in r))


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
import os as _os  # noqa: E402
import sys as _sys  # noqa: E402
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    main()
