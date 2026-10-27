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

"""Dump the header of Rockbox .fnt files (stdlib only).

    scripts/fntinfo.py <font.fnt>...

Prints one line per font: height, ascent, maxwidth, depth (0 = 1 bpp,
1 = 4 bpp anti-aliased), first char, glyph count and file size. Exits 1 if
any file is unreadable or not an RB12 font. Header layout as read by
font_load_header() in firmware/font.c: "RB12", then little-endian
int16 maxwidth, height, ascent, depth; int32 firstchar, defaultchar, size
(glyph count), bits_size, noffset, nwidth.
"""
import os
import struct
import sys

HDR = struct.Struct('<4s4h6i')


def info(path):
    with open(path, 'rb') as f:
        d = f.read(HDR.size)
    if len(d) < HDR.size:
        raise ValueError('short file')
    (magic, maxwidth, height, ascent, depth, firstchar, defaultchar, size,
     nbits, noff, nwid) = HDR.unpack(d)
    if magic != b'RB12':
        raise ValueError('not an RB12 font (magic %r)' % magic)
    return dict(height=height, ascent=ascent, maxwidth=maxwidth, depth=depth,
                firstchar=firstchar, count=size, bits=nbits)


def main(paths):
    if not paths:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    rc = 0
    print('%-32s %6s %6s %8s %5s %6s %6s %8s' % (
        'font', 'height', 'ascent', 'maxwidth', 'depth', 'first', 'count',
        'bytes'))
    for p in paths:
        try:
            m = info(p)
            print('%-32s %6d %6d %8d %5d %6d %6d %8d' % (
                os.path.basename(p), m['height'], m['ascent'], m['maxwidth'],
                m['depth'], m['firstchar'], m['count'], os.path.getsize(p)))
        except (OSError, ValueError) as e:
            print('%-32s ERROR %s' % (p, e))
            rc = 1
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
