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

"""Compose PNG screenshots side by side (stdlib only).

    scripts/pngcompose.py <out.png> <left1.png> <right1.png> [<left2> <right2> ...]

Each pair becomes one row: reference on the left, ours on the right, with
an 8 px grey gutter. Images are pasted at 1:1 (no scaling); smaller images
are padded. Uses the decoder from scripts/pngdiff.py.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngdiff import decode  # noqa: E402

GUTTER = 8
BG = (0x80, 0x80, 0x80)


def write_png(path, w, h, rows):
    def chunk(typ, body):
        c = struct.pack('>I', len(body)) + typ + body
        return c + struct.pack('>I', zlib.crc32(typ + body) & 0xffffffff)
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 9)))
        f.write(chunk(b'IEND', b''))


def main():
    args = sys.argv[1:]
    if len(args) < 3 or len(args) % 2 != 1:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    out, paths = args[0], args[1:]
    imgs = [decode(p) for p in paths]
    cell_w = max(i[0] for i in imgs)
    cell_h = max(i[1] for i in imgs)
    nrows = len(imgs) // 2
    W = GUTTER + 2 * (cell_w + GUTTER)
    H = GUTTER + nrows * (cell_h + GUTTER)
    canvas = [bytearray(bytes(BG) * W) for _ in range(H)]
    for n, (w, h, px) in enumerate(imgs):
        ox = GUTTER + (n % 2) * (cell_w + GUTTER)
        oy = GUTTER + (n // 2) * (cell_h + GUTTER)
        for y in range(h):
            line = canvas[oy + y]
            for x in range(w):
                line[(ox + x) * 3:(ox + x) * 3 + 3] = bytes(px[y * w + x])
    write_png(out, W, H, canvas)
    print(out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
