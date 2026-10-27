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

"""QW2 review sheets (Phase 11, Q; stdlib only).

    tests/npscreen/qw2_sheets.py --photos DIR [--out out/review]

DIR holds the rectified owner captures (320 x 240, out/fidelity/rect of
the fidelity audit): np-normal.png (photo 1203) and np-ab-vol.png
(photo 1222). Shots come from out/shots/ (run the ipodui-qw2-*,
ipodui-p5-normal, -p5-volume, -p5-art-fit, -p5-noart, -p6-book-noart
scripts first) and goldens from tests/ui/golden/. Writes:

  p11-q-geometry.png    owner question Q1: both candidate art boxes on
                        the rectified photo 1203 and on ours, 3x (red: the
                        spec / code box x 10-147, y 34-175 / 43-167; cyan:
                        the coordinator's x 10-145, y 32-167 / 42-162)
  p11-q-speakers.png    the bar row's speakers at 8x: before (golden),
                        after (ours), the rectified photo 1222
  p11-q-trackchange.png Now Playing before (golden) / after (ours) /
                        diff for real art, fitted art, the placeholders,
                        and the volume-limit triangle
"""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
from pngdiff import decode  # noqa: E402
from pngcompose import write_png  # noqa: E402

GUT = 8
BG = (0x80, 0x80, 0x80)
SPEC = ((10, 34), (147, 43), (147, 167), (10, 175))
COORD = ((10, 32), (145, 42), (145, 162), (10, 167))
RED, CYAN = (255, 40, 40), (0, 200, 255)


class Img:
    def __init__(self, w, h, px):
        self.w, self.h, self.px = w, h, list(px)

    @classmethod
    def load(cls, path):
        w, h, px = decode(path)
        return cls(w, h, px)

    def crop(self, x, y, w, h):
        return Img(w, h, [self.px[(y + j) * self.w + x + i]
                          for j in range(h) for i in range(w)])

    def scale(self, k):
        out = []
        for j in range(self.h * k):
            row = self.px[(j // k) * self.w:(j // k + 1) * self.w]
            for p in row:
                out.extend([p] * k)
        return Img(self.w * k, self.h * k, out)

    def line(self, a, b, c, k):
        """a, b in source pixels (centres), drawn on a k-scaled image"""
        (x0, y0), (x1, y1) = a, b
        x0, y0 = x0 * k + k // 2, y0 * k + k // 2
        x1, y1 = x1 * k + k // 2, y1 * k + k // 2
        n = max(abs(x1 - x0), abs(y1 - y0), 1)
        for i in range(n + 1):
            x = round(x0 + (x1 - x0) * i / n)
            y = round(y0 + (y1 - y0) * i / n)
            if 0 <= x < self.w and 0 <= y < self.h:
                self.px[y * self.w + x] = c

    def quad(self, pts, c, k):
        for i in range(4):
            self.line(pts[i], pts[(i + 1) % 4], c, k)


def diff(a, b):
    return Img(a.w, a.h, [(0, 0, 0) if (p[0] >> 3, p[1] >> 2, p[2] >> 3)
                          == (q[0] >> 3, q[1] >> 2, q[2] >> 3)
                          else (255, 255, 255) for p, q in zip(a.px, b.px)])


def sheet(path, rows):
    """rows: lists of Img (None = blank cell)"""
    cw = max(i.w for r in rows for i in r if i)
    rh = [max(i.h for i in r if i) for r in rows]
    ncol = max(len(r) for r in rows)
    W = ncol * cw + (ncol + 1) * GUT
    H = sum(rh) + (len(rows) + 1) * GUT
    canvas = [[BG] * W for _ in range(H)]
    y = GUT
    for r, h in zip(rows, rh):
        x = GUT
        for im in r:
            if im:
                for j in range(im.h):
                    canvas[y + j][x:x + im.w] = im.px[j * im.w:(j + 1) * im.w]
            x += cw + GUT
        y += h + GUT
    write_png(path, W, H, [bytes(v for p in row for v in p) for row in canvas])
    print(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--photos', required=True)
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'review'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    shots = os.path.join(ROOT, 'out', 'shots')
    gold = os.path.join(ROOT, 'tests', 'ui', 'golden')

    def S(script, name):
        return Img.load(os.path.join(shots, script, name + '.png'))

    def G(script, name):
        return Img.load(os.path.join(gold, script, name + '.png'))

    # Q1 geometry
    k = 3
    photo = Img.load(os.path.join(a.photos, 'np-normal.png')).crop(0, 20, 170, 200).scale(k)
    ours = S('ipodui-p5-normal', 'normal').crop(0, 20, 170, 200).scale(k)
    for im in (photo, ours):
        sh = [(x, y - 20) for x, y in SPEC]
        co = [(x, y - 20) for x, y in COORD]
        im.quad(co, CYAN, k)
        im.quad(sh, RED, k)
    sheet(os.path.join(a.out, 'p11-q-geometry.png'), [[photo, ours]])

    # speakers at 8x: x 36-56 and 268-294 of the bar row, y 204-221
    def spk(im):
        l = im.crop(36, 204, 22, 18).scale(8)
        r = im.crop(268, 204, 28, 18).scale(8)
        return [l, r]
    before = G('ipodui-p5-volume', 'v50')
    after = S('ipodui-p5-volume', 'v50')
    photo2 = Img.load(os.path.join(a.photos, 'np-ab-vol.png'))
    sheet(os.path.join(a.out, 'p11-q-speakers.png'),
          [spk(before), spk(after), spk(photo2)])

    # track change, placeholders, triangle: golden | ours | diff
    rows = []
    for script, name in (('ipodui-p5-normal', 'normal'),
                         ('ipodui-p5-art-fit', 'tall'),
                         ('ipodui-p5-art-fit', 'banner'),
                         ('ipodui-p5-noart', 'noart'),
                         ('ipodui-p6-book-noart', 'np_book'),
                         ('ipodui-p5-volume', 'v50')):
        g, o = G(script, name), S(script, name)
        rows.append([g, o, diff(g, o)])
    rows.append([S('ipodui-qw2-trackchange', 'asleep'),
                 S('ipodui-qw2-trackchange', 'awake'),
                 S('ipodui-qw2-vlimit', 'limit_v50')])
    sheet(os.path.join(a.out, 'p11-q-trackchange.png'), rows)


if __name__ == '__main__':
    main()
