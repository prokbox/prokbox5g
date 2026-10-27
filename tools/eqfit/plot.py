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

"""Plot eqfit's curves as one PNG sheet (stdlib only; PNG writer from
scripts/pngcompose.py).
    build/eqfit/eqfit --plot build/eqfit/curves.tsv
    tools/eqfit/plot.py build/eqfit/curves.tsv out/eqfit.png
One panel per preset, in presets[] order: x 20 Hz .. 20 kHz (log, grid at
100 Hz, 1 kHz, 10 kHz), y -12 .. +12 dB (grid every 6 dB). Black: the
software EQ (the iTunes curve) at 44.1 kHz; red: the hardware fit at
44.1 kHz; orange: the hardware fit at 48 kHz. The panel title is the
preset name and its maxerr; a red title means it stays on the software EQ.
"""
import math
import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', '..', 'scripts'))
from pngcompose import write_png  # noqa: E402

PW, PH, COLS, PAD, TOP = 300, 150, 4, 10, 14
FONT = {  # 3x5, rows top to bottom, 3 bits each
    'A': '25755', 'B': '65656', 'C': '34443', 'D': '65556', 'E': '74647',
    'F': '74644', 'G': '34553', 'H': '55755', 'I': '72227', 'J': '11153',
    'K': '55655', 'L': '44447', 'M': '57755', 'N': '65555', 'O': '25552',
    'P': '65644', 'Q': '25563', 'R': '65655', 'S': '34216', 'T': '72222',
    'U': '55557', 'V': '55552', 'W': '55775', 'X': '55255', 'Y': '55222',
    'Z': '71247', '0': '75557', '1': '26227', '2': '61247', '3': '61216',
    '4': '55711', '5': '74616', '6': '34757', '7': '71122', '8': '75757',
    '9': '75711', '.': '00002', ' ': '00000', '_': '00007', '-': '00700',
}


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: plot.py curves.tsv out.png')
    data, order = {}, []
    for line in open(sys.argv[1]):
        if line.startswith('#'):
            continue
        name, hz, s44, h44, s48, h48 = line.split('\t')
        if name not in data:
            data[name] = []
            order.append(name)
        data[name].append((float(hz), float(s44), float(h44), float(h48)))
    rows_n = (len(order) + COLS - 1) // COLS
    W = COLS * (PW + PAD) + PAD
    H = rows_n * (PH + TOP + PAD) + PAD
    img = [[255] * (3 * W) for _ in range(H)]

    def px(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            img[y][3 * x:3 * x + 3] = c

    def line(x0, y0, x1, y1, c):
        n = max(abs(x1 - x0), abs(y1 - y0), 1)
        for i in range(n + 1):
            px(round(x0 + (x1 - x0) * i / n), round(y0 + (y1 - y0) * i / n), c)

    def text(x, y, s, c):
        for ch in s.upper():
            g = FONT.get(ch, FONT[' '])
            for r in range(5):
                bits = int(g[r])
                for k in range(3):
                    if bits & (4 >> k):
                        px(x + k, y + r, c)
            x += 4

    for i, name in enumerate(order):
        ox = PAD + (i % COLS) * (PW + PAD)
        oy = PAD + (i // COLS) * (PH + TOP + PAD) + TOP

        def X(hz):
            return ox + round((math.log10(hz) - math.log10(20)) /
                              (3 * math.log10(10)) * (PW - 1))

        def Y(db):
            db = max(-12.0, min(12.0, db))
            return oy + round((12.0 - db) / 24.0 * (PH - 1))

        for hz in (100, 1000, 10000):
            line(X(hz), oy, X(hz), oy + PH - 1, (220, 220, 220))
        for db in (-6, 6):
            line(ox, Y(db), ox + PW - 1, Y(db), (220, 220, 220))
        line(ox, Y(0), ox + PW - 1, Y(0), (160, 160, 160))
        pts = data[name]
        err = max(max(abs(p[2] - p[1]) for p in pts), 0.0)
        for col, c in ((3, (255, 160, 0)), (2, (220, 0, 0)), (1, (0, 0, 0))):
            for a, b in zip(pts, pts[1:]):
                line(X(a[0]), Y(a[col]), X(b[0]), Y(b[col]), c)
        text(ox, oy - TOP + 4, '%s %.1f' % (name, err),
             (0, 0, 0) if err <= 1.5 else (200, 0, 0))
    write_png(sys.argv[2], W, H, img)


if __name__ == '__main__':
    main()
