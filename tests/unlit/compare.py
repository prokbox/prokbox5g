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

"""tests/unlit: compare the C-rendered grey rasters with the design PNGs.

    python3 -I tests/unlit/compare.py RASTERDIR

Each <scene>.grey (from test_unlit) must equal
docs/design/unlit/stock-<scene>-lit.png after the generator's RGB565
quantisation, pixel for pixel; glyphs.grey must equal glyphs-stock.png
(2x, sampled) outside the AM/PM letters, which the face never draws.
"""
import os
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
DESIGN = os.path.join(ROOT, 'docs', 'design', 'unlit')


def read_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat, w, h = 8, b'', 0, 0
    while pos < len(data):
        n, tag = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if tag == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            assert depth == 8 and ctype == 2, 'RGB8 only'
        elif tag == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 3
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if f:
            for i in range(stride):
                a = line[i - 3] if i >= 3 else 0
                b = prev[i]
                c = prev[i - 3] if i >= 3 else 0
                if f == 1:
                    p = a
                elif f == 2:
                    p = b
                elif f == 3:
                    p = (a + b) // 2
                else:
                    q = a + b - c
                    pa, pb, pc = abs(q - a), abs(q - b), abs(q - c)
                    p = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + p) & 255
        rows.append(line)
        prev = line
    return w, h, rows


def rgb565(v):
    r = (v >> 3) << 3 | v >> 5
    g = (v >> 2) << 2 | v >> 6
    return (r, g, r)


def read_grey(path):
    data = open(path, 'rb').read()
    nl = data.index(b'\n')
    w, h = map(int, data[:nl].split())
    return w, h, data[nl + 1:]


def compare(name, png, scale, skip=None, allow=frozenset()):
    gw, gh, grey = read_grey(os.path.join(sys.argv[1], name + '.grey'))
    pw, ph, rows = read_png(os.path.join(DESIGN, png))
    if (pw, ph) != (gw * scale, gh * scale):
        print('FAIL %s: size %dx%d vs %dx%d' % (name, gw, gh, pw, ph))
        return 1
    bad = []
    for y in range(gh):
        row = rows[y * scale]
        for x in range(gw):
            if skip and skip(x, y):
                continue
            want = tuple(row[x * scale * 3:x * scale * 3 + 3])
            if rgb565(grey[y * gw + x]) != want:
                bad.append((x, y))
    if set(bad) == set(allow) and bad:
        print('ok   %s == %s (%dx%d) except %d accepted pixels %s'
              % (name, png, gw, gh, len(bad), sorted(bad)))
        return 0
    if bad:
        print('FAIL %s vs %s: %d pixels differ, first %s'
              % (name, png, len(bad), bad[:8]))
        return 1
    print('ok   %s == %s (%dx%d, threshold 0)' % (name, png, gw, gh))
    return 0


def main():
    fails = 0
    for sc in ('12h-playing', '24h-paused', '24h-stopped',
               '12h-hold-charging'):
        fails += compare(sc, 'stock-%s-lit.png' % sc, 1)
    # the letters start after 10 digit cells and the colon: x >= 6+360+17
    fails += compare('glyphs', 'glyphs-stock.png', 2,
                     skip=lambda x, y: y < 80 and x >= 383)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
