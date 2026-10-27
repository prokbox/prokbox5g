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

"""Compose a contact sheet of PNGs in N columns (stdlib only).

    scripts/pngsheet.py <out.png> [--cols N] [--scale K] <item>...

An item is a PNG path, or one of
    diff:<a.png>:<b.png>   per-pixel difference, black where equal and
                           white where not (an empty diff is all black),
                           at the panel's RGB565 precision: the sim's shots
                           (sips) and the host renders (bit replication)
                           expand 565 to 8 bits differently, by up to 1
    unlit:<a.png>          the shell's lit face tone-mapped to "unlit" as
                           art/unlit's preview does (paper 255 -> (182,177,
                           165), ink 56 -> (60,58,52), linear between)
    blank                  an empty cell
Cells are pasted at 1:1 (or enlarged K times, nearest neighbour) with an 8 px
grey gutter; missing files abort with the path (the sheet is a review
artifact, so a gap must be loud). Used by tests/review/p8a-sheets.sh.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngdiff import decode  # noqa: E402
from pngcompose import write_png  # noqa: E402

GUTTER = 8
BG = (0x80, 0x80, 0x80)
PAPER = (182, 177, 165)
INK = (60, 58, 52)
INK_LEVEL = 56


def px_rows(img):
    w, h, px = img
    return w, h, px


def get(img, x, y):
    w, h, px = img
    o = (y * w + x) * 3
    return px[o], px[o + 1], px[o + 2]


def load(path):
    if not os.path.isfile(path):
        sys.exit('pngsheet: missing %s' % path)
    w, h, px = decode(path)
    # normalise to a flat RGB bytes object whatever decode() returned
    if isinstance(px, (bytes, bytearray)) and len(px) == w * h * 3:
        return w, h, bytes(px)
    flat = bytearray()
    for p in px:
        flat += bytes(p[:3]) if hasattr(p, '__len__') else bytes((p, p, p))
    return w, h, bytes(flat)


def item(spec):
    if spec == 'blank':
        return 320, 240, bytes(BG) * (320 * 240)
    if spec.startswith('diff:'):
        a, b = spec[5:].split(':', 1)
        ia, ib = load(a), load(b)
        if ia[:2] != ib[:2]:
            sys.exit('pngsheet: diff size mismatch %s vs %s' % (a, b))
        out = bytearray()
        for i in range(0, len(ia[2]), 3):
            pa, pb = ia[2][i:i + 3], ib[2][i:i + 3]
            same = (pa[0] >> 3 == pb[0] >> 3 and pa[1] >> 2 == pb[1] >> 2
                    and pa[2] >> 3 == pb[2] >> 3)
            out += b'\x00\x00\x00' if same else b'\xff\xff\xff'
        return ia[0], ia[1], bytes(out)
    if spec.startswith('unlit:'):
        w, h, px = load(spec[6:])
        out = bytearray()
        for i in range(0, len(px), 3):
            v = (px[i] + px[i + 1] + px[i + 2]) // 3
            t = min(1.0, max(0.0, (v - INK_LEVEL) / (255.0 - INK_LEVEL)))
            for c in range(3):
                out.append(int(round(INK[c] + (PAPER[c] - INK[c]) * t)))
        return w, h, bytes(out)
    return load(spec)


def main():
    args = sys.argv[1:]
    cols, scale = 2, 1
    if not args:
        sys.exit(__doc__)
    out, specs, i = args[0], [], 1
    while i < len(args):
        if args[i] in ('--cols', '--scale') and i + 1 < len(args):
            if args[i] == '--cols':
                cols = int(args[i + 1])
            else:
                scale = int(args[i + 1])
            i += 2
        else:
            specs.append(args[i])
            i += 1
    if not specs:
        sys.exit(__doc__)
    imgs = [item(s) for s in specs]
    cw = max(i[0] for i in imgs) * scale
    ch = max(i[1] for i in imgs) * scale
    rows_n = (len(imgs) + cols - 1) // cols
    W = GUTTER + cols * (cw + GUTTER)
    H = GUTTER + rows_n * (ch + GUTTER)
    canvas = [bytearray(bytes(BG) * W) for _ in range(H)]
    for n, (w, h, px) in enumerate(imgs):
        ox = GUTTER + (n % cols) * (cw + GUTTER)
        oy = GUTTER + (n // cols) * (ch + GUTTER)
        for y in range(h * scale):
            sy = y // scale
            row = canvas[oy + y]
            for x in range(w * scale):
                o = (sy * w + x // scale) * 3
                row[(ox + x) * 3:(ox + x) * 3 + 3] = px[o:o + 3]
    write_png(out, W, H, canvas)
    print('pngsheet: %s (%d cells, %dx%d)' % (out, len(imgs), W, H))


if __name__ == '__main__':
    main()
