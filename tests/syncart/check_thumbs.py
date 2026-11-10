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

"""The synced covers' thumbnails in a simulator's art cache (#225 S3).

    tests/syncart/check_thumbs.py <simdisk> [min_records]

Every record whose source is the stock firmware's cover database
(src_kind 3) must hold, in its thumbs.pak slot, the fixture's pattern
(tests/syncart/mkfixture.c: red ramps 0..31 left to right, green 0..63
top to bottom, blue a constant per cover) scaled from 200 px to the
thumbnail size (dithered: within 1 step): each column's red and each
row's green near an area average of the 200 px ramp, blue within 1 of a
constant. A swapped byte order, swapped channels or a transposed image fails by a wide margin. At least
min_records (default 1) such records must exist: a check that looked at
nothing fails.
"""
import struct
import sys


def ramp_avg(n_src, n_dst, maxv):
    """area average of v(x) = x * maxv // (n_src - 1) onto n_dst cells"""
    out = []
    for d in range(n_dst):
        a = d * n_src / n_dst
        b = (d + 1) * n_src / n_dst
        tot = wt = 0.0
        x = int(a)
        while x < b:
            w = min(x + 1, b) - max(x, a)
            tot += w * (x * maxv // (n_src - 1))
            wt += w
            x += 1
        out.append(tot / wt)
    return out


def main():
    # --mirror: the pictures of mkfixture --mirror (a re-sync), so a
    # thumbnail still holding the first sync's picture fails
    args = [x for x in sys.argv[1:] if x != '--mirror']
    mirror = len(args) != len(sys.argv) - 1
    disk = args[0]
    need = int(args[1]) if len(args) > 1 else 1
    d = disk + '/.rockbox/artcache/'
    idx = open(d + 'index.dat', 'rb').read()
    pak = open(d + 'thumbs.pak', 'rb').read()
    _, _, w, h, slot_bytes, _ = struct.unpack_from('<IIHHII', pak, 0)
    n = struct.unpack_from('<I', idx, 12)[0]
    rx = ramp_avg(200, w, 31)
    if mirror:
        rx = rx[::-1]
    gy = ramp_avg(200, h, 63)
    checked = bad = 0
    worst = 0.0
    for i in range(n):
        key, slot, state, kind, typ = struct.unpack_from('<QIBBB', idx,
                                                         512 + 32 * i)
        if state != 1 or kind != 3 or (typ & 0x0f) != 0x0e:
            continue
        checked += 1
        base = 512 + slot * slot_bytes
        px = struct.unpack_from('<%dH' % (w * h), pak, base)
        blues = set(p & 31 for p in px)
        err = 0.0
        for y in range(h):
            for x in range(w):
                p = px[y * w + x]
                err = max(err, abs(((p >> 11) & 31) - rx[x]),
                          abs(((p >> 5) & 63) - gy[y]) / 2.0)
        worst = max(worst, err)
        if max(blues) - min(blues) > 1 or err > 1.5:   # dither: +-1
            bad += 1
            if bad <= 3:
                print('bad slot %d key %016x: blues=%s err=%.2f'
                      % (slot, key, sorted(blues)[:4], err))
    print('synced thumbnails: %d checked (%dx%d), %d bad, worst %.2f steps'
          % (checked, w, h, bad, worst))
    sys.exit(1 if bad or checked < need else 0)


if __name__ == '__main__':
    main()
