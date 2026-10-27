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

"""tests/bootscreen: compare the host-rendered boot screen with the reference.

    compare.py FB.raw REFERENCE.png OUT.png

FB.raw is 320 x 240 little-endian RGB565 (test_bootscreen). It is expanded
to 8-bit the way the generator writes its PNGs (r << 3 | r >> 2, ...),
written to OUT.png (the "boot-bl" image of the boot hand-off sheet) and
compared pixel by pixel with REFERENCE.png at threshold 0. Exit 1 on any
difference, 2 on a missing or unreadable file. Stdlib only.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', '..', 'scripts'))
import pngdiff  # noqa: E402

W, H = 320, 240


def write_png(path, w, h, px):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for r, g, b in px[y * w:(y + 1) * w]:
            raw += bytes((r, g, b))

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as fp:
        fp.write(png)


def main():
    if len(sys.argv) != 4:
        sys.exit('usage: compare.py FB.raw REFERENCE.png OUT.png')
    fb, ref, out = sys.argv[1:]
    try:
        data = open(fb, 'rb').read()
        rw, rh, rpx = pngdiff.decode(ref)
    except (OSError, ValueError) as e:
        print('FAIL: %s' % e)
        return 2
    if len(data) != W * H * 2 or (rw, rh) != (W, H):
        print('FAIL: size: fb %d B, reference %dx%d' % (len(data), rw, rh))
        return 2
    px = []
    for i in range(W * H):
        v = data[2 * i] | data[2 * i + 1] << 8
        r, g, b = v >> 11, (v >> 5) & 63, v & 31
        px.append((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2))
    write_png(out, W, H, px)
    bad = [i for i in range(W * H) if px[i] != rpx[i]]
    if bad:
        i = bad[0]
        print('FAIL: %d pixels differ from %s; first at (%d, %d): %s vs %s'
              % (len(bad), ref, i % W, i // W, px[i], rpx[i]))
        return 1
    print('ok   host boot screen == %s (0 of %d pixels differ) -> %s'
          % (ref, W * H, out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
