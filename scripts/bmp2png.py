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

"""bmp2png.py IN.bmp OUT.png: convert a simulator screenshot to PNG.

The portable stand-in for macOS `sips -s format png` in scripts/shots.sh,
bit-exact with it so the committed goldens (written by sips on the Mac)
compare equal on Linux. The simulator writes RGB565 BI_BITFIELDS BMPs;
sips expands each channel with rounding, v8 = (v * 255 + max / 2) // max,
not bit replication. Other BMPs are passed to PIL as they are.
"""
import struct
import sys

from PIL import Image


def main(src, dst):
    data = open(src, 'rb').read()
    off, = struct.unpack_from('<I', data, 10)
    w, h, _, bpp, comp = struct.unpack_from('<iiHHI', data, 18)
    if bpp != 16 or comp != 3:
        Image.open(src).convert('RGB').save(dst)
        return
    rm, gm, bm = struct.unpack_from('<III', data, 54)
    if (rm, gm, bm) != (0xF800, 0x07E0, 0x001F):
        sys.exit('bmp2png: unexpected masks %x %x %x' % (rm, gm, bm))
    stride = (w * 2 + 3) & ~3
    e5 = bytes((v * 255 + 15) // 31 for v in range(32))
    e6 = bytes((v * 255 + 31) // 63 for v in range(64))
    out = bytearray(w * abs(h) * 3)
    rows = range(abs(h) - 1, -1, -1) if h > 0 else range(abs(h))
    i = 0
    for y in rows:
        row = data[off + y * stride: off + y * stride + w * 2]
        for (p,) in struct.iter_unpack('<H', row):
            out[i] = e5[p >> 11]
            out[i + 1] = e6[(p >> 5) & 63]
            out[i + 2] = e5[p & 31]
            i += 3
    Image.frombytes('RGB', (w, abs(h)), bytes(out)).save(dst)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
