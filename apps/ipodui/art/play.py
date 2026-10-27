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

"""Generate the header's play-glyph map (stdlib only).

    apps/ipodui/art/play.py         # prints the map as a C table

apps/ipodui/art/header.py imports play_map() and pre-renders the header's
play glyph from it (Phase 8b).

An original drawing: a right-pointing triangle, 11 x 12 px, rasterised
with 16x16 supersampling. Each pixel gets one byte:

    bits 7-4  coverage of the triangle, 0..15
    bits 3-2  rim: how much of the covered part lies within RIM px of the
              triangle's edge, 0..3 (header.c darkens it towards the rim
              colour, giving the glossy glyph its dark outline)
    bits 1-0  glow: how much of the uncovered part lies within GLOW px
              of the edge, 0..3 (header.c tints it light blue)

The map has a 1 px margin on the left and at the bottom for the glow.
The proportions (11 x 12 box, point at mid height) and the look (dark
rim, light-blue glow, a per-row fill that is darkest at the top and
brightest just below the middle) follow main.png; the colours are a
per-row table (now the material kit's sky gel). No reference pixels are used.
"""
import math

W, H = 13, 13           # map: glow column, 11 ink columns, 1 spare; 12 ink rows + glow row
TRI = [(1.0, 0.0), (1.0, 12.0), (12.0, 6.0)]
RIM = 1.0
GLOW = 1.25
SS = 16


def edges():
    for i in range(3):
        yield TRI[i], TRI[(i + 1) % 3]


def dist_seg(px, py, a, b):
    ax, ay = a
    bx, by = b
    dx, dy = bx - ax, by - ay
    t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def inside(px, py):
    s = []
    for (ax, ay), (bx, by) in edges():
        s.append((bx - ax) * (py - ay) - (by - ay) * (px - ax))
    return all(v >= 0 for v in s) or all(v <= 0 for v in s)


def play_map():
    """The map rows (H lists of W bytes)."""
    rows = []
    for y in range(H):
        row = []
        for x in range(W):
            cov = rim = glow = 0
            for sy in range(SS):
                for sx in range(SS):
                    px, py = x + (sx + .5) / SS, y + (sy + .5) / SS
                    d = min(dist_seg(px, py, a, b) for a, b in edges())
                    if inside(px, py):
                        cov += 1
                        if d <= RIM:
                            rim += 1
                    elif d <= GLOW:
                        glow += 1
            n = SS * SS
            c = round(15 * cov / n)
            r = round(3 * rim / cov) if cov else 0
            g = round(3 * glow / (n - cov)) if cov < n else 0
            row.append(c << 4 | r << 2 | g)
        rows.append(row)
    return rows


def main():
    rows = play_map()
    print('#define PLAY_MAP_W %d' % W)
    print('#define PLAY_MAP_H %d' % H)
    print('static const unsigned char play_map[PLAY_MAP_H][PLAY_MAP_W] = {')
    for r in rows:
        print('    {' + ','.join('0x%02x' % v for v in r) + '},')
    print('};')


if __name__ == '__main__':
    main()
