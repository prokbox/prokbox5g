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

"""Compare test_usbscreen's raw RGB565 screens with the reference PNGs.

    check.py <rawdir> <pngdir>

Decodes the generator's 8-bit RGB PNGs (stdlib zlib, filter type 0 as
art/usb/usbscreen.py writes them), expands each raw RGB565 pixel the same
way and requires every pixel to match. A missing file fails.
"""
import os
import struct
import sys
import zlib

PAIRS = [('idle-usbc', 'usb-idle-usbc'), ('idle-dock30', 'usb-idle-dock30')] \
    + [('f%02d' % k, 'usb-io-f%02d' % k) for k in range(6)]


def read_png(path):
    with open(path, 'rb') as fp:
        data = fp.read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat, w, h = 8, b'', 0, 0
    while pos < len(data):
        n, tag = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if tag == b'IHDR':
            w, h = struct.unpack('>II', body[:8])
        elif tag == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    rows = []
    stride = 1 + 3 * w
    for y in range(h):
        line = raw[y * stride:(y + 1) * stride]
        assert line[0] == 0, 'unexpected PNG filter'
        rows.append(line[1:])
    return w, h, rows


def expand(v):
    r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
    return ((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31)


def main(rawdir, pngdir):
    bad = 0
    for raw_name, png_name in PAIRS:
        rp = os.path.join(rawdir, raw_name + '.565')
        pp = os.path.join(pngdir, png_name + '.png')
        if not os.path.exists(rp) or not os.path.exists(pp):
            print('FAIL missing %s or %s' % (rp, pp))
            bad += 1
            continue
        w, h, rows = read_png(pp)
        with open(rp, 'rb') as fp:
            px = fp.read()
        if len(px) != w * h * 2:
            print('FAIL %s: size' % raw_name)
            bad += 1
            continue
        diff = 0
        for y in range(h):
            for x in range(w):
                v = px[2 * (y * w + x)] | px[2 * (y * w + x) + 1] << 8
                if bytes(expand(v)) != rows[y][3 * x:3 * x + 3]:
                    diff += 1
        print('%s %s: %d pixels differ' % ('ok  ' if not diff else 'FAIL',
                                           png_name, diff))
        bad += diff != 0
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
