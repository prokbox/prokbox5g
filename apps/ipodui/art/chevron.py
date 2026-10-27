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

"""Generate the shell's chevron coverage table (stdlib only).

    apps/ipodui/art/chevron.py      # prints the C table for paint.c

An original drawing: a '>' made of two straight strokes of constant
width with round outer ends (owner 2026-10-08: rounded stroke ends) and a
mitred point, rasterised with 16x16 supersampling into 4-bit coverage
(0 = empty, 15 = full ink). Only the proportions come from the 7G
references (fidelity audit 1 fix #8, owner photos rectified to device
pixels): ink about 8-9 x 13 px with a fringe row above and below, a 90
degree point at mid height, strokes 3 px thick. No reference pixels are
used.

History: Phase 3 to qw1 drew ink 8 x 11 px with 2.8 px butt-ended
strokes (main-alt.png).

History: the Phase 3 chevron had a 121 degree point (3.1 px across for
5.5 px of height); the reference's is 90 degrees.
"""
import math

W, H = 9, 15        # table size; ink rows 1..13, columns 0..8
SW = 3.0            # stroke width, perpendicular to the stroke
END = (1.5, 2.5)    # centre of the top stroke end (bottom one mirrored)
TIP = (6.5, 7.5)    # where the stroke centre lines meet (mid height)
SS = 16


def in_stroke(x, y, ax, ay, bx, by):
    """Inside the stroke from (ax, ay), round-capped there, running
    through (bx, by) and on past it so the two strokes overlap at the
    point."""
    dx, dy = bx - ax, by - ay
    n = math.hypot(dx, dy)
    ux, uy = dx / n, dy / n
    t = (x - ax) * ux + (y - ay) * uy
    d = abs(-(x - ax) * uy + (y - ay) * ux)
    if t < 0:
        return math.hypot(x - ax, y - ay) <= SW / 2
    return t <= n * 1.6 and d <= SW / 2


def inside(x, y):
    ex, ey = END
    tx, ty = TIP
    if y <= ty:         # upper stroke; the mitre is the line y = ty
        return in_stroke(x, y, ex, ey, tx, ty)
    return in_stroke(x, y, ex, 2 * ty - ey, tx, ty)


def angle():
    ex, ey = END
    tx, ty = TIP
    return 2 * math.degrees(math.atan2(ty - ey, tx - ex))


def main():
    rows = []
    for py in range(H):
        row = []
        for px in range(W):
            n = 0
            for sy in range(SS):
                for sx in range(SS):
                    if inside(px + (sx + .5) / SS, py + (sy + .5) / SS):
                        n += 1
            row.append(round(15 * n / (SS * SS)))
        rows.append(row)

    print('/* point angle %.0f degrees */' % angle())
    print('#define CHEV_W %d' % W)
    print('#define CHEV_H %d' % H)
    print('static const unsigned char chevron_cov[CHEV_H][CHEV_W] = {')
    for r in rows:
        print('    {' + ', '.join('%2d' % v for v in r) + '},')
    print('};')


if __name__ == '__main__':
    main()
