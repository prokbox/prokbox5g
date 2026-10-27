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
"""ADTS stream -> our .au file: what the player would hand the chip, an
AudioSpecificConfig (as an MP4's esds carries it) and raw access units
(no ADTS headers).

.au layout (little-endian):
  'AACU', u32 asc_len, asc bytes,
  then per access unit: u32 len, len bytes          (until end of file)

Usage: adts2au.py IN.aac OUT.au
"""
import struct
import sys


def parse_adts(data):
    """(asc bytes, [raw AU bytes]) from an ADTS stream."""
    aus, asc, pos = [], None, 0
    while pos + 7 <= len(data):
        h = data[pos:pos + 9]
        if h[0] != 0xFF or (h[1] & 0xF6) != 0xF0:
            raise ValueError('lost ADTS sync at byte %d' % pos)
        protection_absent = h[1] & 1
        profile = h[2] >> 6
        sfi = (h[2] >> 2) & 15
        chcfg = ((h[2] & 1) << 2) | (h[3] >> 6)
        flen = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5)
        nblocks = (h[6] & 3) + 1
        if nblocks != 1:
            raise ValueError('ADTS frame with %d raw blocks' % nblocks)
        hlen = 7 if protection_absent else 9
        cur = struct.pack('>H', ((profile + 1) << 11) | (sfi << 7) | (chcfg << 3))
        if asc is None:
            asc = cur
        elif asc != cur:
            raise ValueError('ADTS configuration changes at byte %d' % pos)
        aus.append(data[pos + hlen:pos + flen])
        pos += flen
    if pos != len(data):
        raise ValueError('%d trailing bytes' % (len(data) - pos))
    return asc, aus


def write_au(path, asc, aus):
    with open(path, 'wb') as f:
        f.write(b'AACU' + struct.pack('<I', len(asc)) + asc)
        for au in aus:
            f.write(struct.pack('<I', len(au)) + au)


def read_au(path):
    data = open(path, 'rb').read()
    if data[:4] != b'AACU':
        raise ValueError('%s: not an .au file' % path)
    n = struct.unpack_from('<I', data, 4)[0]
    asc, pos, aus = data[8:8 + n], 8 + n, []
    while pos < len(data):
        k = struct.unpack_from('<I', data, pos)[0]
        aus.append(data[pos + 4:pos + 4 + k])
        pos += 4 + k
    return asc, aus


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    asc, aus = parse_adts(open(argv[0], 'rb').read())
    write_au(argv[1], asc, aus)
    print('%s: ASC %s, %d access units, largest %d bytes' % (
        argv[1], asc.hex(), len(aus), max(len(a) for a in aus)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
