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

"""Generate the system-screen glyph coverage tables (stdlib only).

    apps/ipodui/art/sys/sysscreen.py > apps/ipodui/sysscreen_data.h

Original drawings in the 7G's flat white style (Phase 11, U2b), rasterised
with 8x8 supersampling into 4-bit coverage (0 = empty, 15 = full ink), two
pixels per byte, high nibble first, row-major (every width is even, so
each row starts on a byte). The shell colours them at draw time:

  bolt     the charging bolt over the big battery (Charging)
  plug     a mains plug over the full battery (Charged)
  warn     a warning triangle with an exclamation mark cut out, over the
           empty battery (Connect to Power)
  moon     the crescent at the left of the Brightness slider
  sun      the small sun at its right
  corner_o the big battery's outer corner (radius 8), top-left quadrant
  corner_i its inner corner (radius 4), top-left quadrant

Only the proportions come from the owner's photos (brightness.jpg) and
the user guide's descriptions; no reference pixels are used.
"""
import math

SS = 8


def raster(w, h, inside):
    rows = []
    for py in range(h):
        row = []
        for px in range(w):
            n = 0
            for sy in range(SS):
                for sx in range(SS):
                    if inside(px + (sx + .5) / SS, py + (sy + .5) / SS):
                        n += 1
            row.append(min(15, (n * 15 + SS * SS // 2) // (SS * SS)))
        rows.append(row)
    return rows


def in_poly(x, y, pts):
    inside = False
    n = len(pts)
    for i in range(n):
        x1, y1 = pts[i]
        x2, y2 = pts[(i + 1) % n]
        if (y1 > y) != (y2 > y):
            xi = x1 + (y - y1) * (x2 - x1) / (y2 - y1)
            if x < xi:
                inside = not inside
    return inside


def in_rrect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return math.hypot(x - cx, y - cy) <= r


# -- bolt: 24 x 36 ---------------------------------------------------------
BOLT_W, BOLT_H = 24, 36
BOLT = [(15.5, 0.5), (2.5, 20.5), (11.0, 20.5), (7.5, 35.5),
        (21.5, 13.5), (13.0, 13.5), (17.5, 0.5)]


def bolt(x, y):
    return in_poly(x, y, BOLT)


# -- plug: 30 x 34 ---------------------------------------------------------
PLUG_W, PLUG_H = 30, 34


def plug(x, y):
    if 8 <= x <= 11 and 0.5 <= y <= 10:            # prongs
        return True
    if 19 <= x <= 22 and 0.5 <= y <= 10:
        return True
    if in_rrect(x, y, 3, 9, 27, 21, 2.5):           # the head
        return True
    if in_poly(x, y, [(7, 20), (23, 20), (18, 27), (12, 27)]):
        return True
    return 13.5 <= x <= 16.5 and 26 <= y <= 33.5    # the cord


# -- warn: 34 x 30 ---------------------------------------------------------
WARN_W, WARN_H = 34, 30


def warn(x, y):
    if not in_poly(x, y, [(17, 0.8), (0.8, 29.2), (33.2, 29.2)]):
        return False
    if 15.4 <= x <= 18.6 and 9 <= y <= 20.5:        # the bar, cut out
        return False
    return math.hypot(x - 17, y - 24.6) > 1.9       # the dot, cut out


# -- moon / sun: 14 x 14 ---------------------------------------------------
ICON = 14


def moon(x, y):
    return (math.hypot(x - 7, y - 7) <= 6.0
            and math.hypot(x - 10.2, y - 5.0) > 5.0)


def sun(x, y):
    d = math.hypot(x - 7, y - 7)
    if d <= 3.2:
        return True
    if 4.6 <= d <= 6.6:
        a = math.atan2(y - 7, x - 7)
        k = round(a / (math.pi / 4))
        return abs(a - k * math.pi / 4) * d <= 0.65
    return False


# -- the big battery's corners ---------------------------------------------
CO, CI = 8, 4


def corner(r):
    return lambda x, y: math.hypot(max(r - x, 0), max(r - y, 0)) <= r


def emit(name, w, h, rows):
    assert w % 2 == 0
    flat = [v for r in rows for v in r]
    packed = [(flat[i] << 4) | flat[i + 1] for i in range(0, len(flat), 2)]
    print('#define %s_W %d' % (name.upper(), w))
    print('#define %s_H %d' % (name.upper(), h))
    print('static const unsigned char %s_cov[%d] = {' % (name, len(packed)))
    for i in range(0, len(packed), 12):
        print('    ' + ' '.join('0x%02x,' % b for b in packed[i:i + 12]))
    print('};')
    print()
    return len(packed)


def main():
    print('/* Generated by apps/ipodui/art/sys/sysscreen.py -- do not edit.')
    print(' * 4-bit coverage, two pixels per byte, high nibble first. */')
    print('#ifndef _IPODUI_SYSSCREEN_DATA_H_')
    print('#define _IPODUI_SYSSCREEN_DATA_H_')
    print()
    n = 0
    n += emit('bolt', BOLT_W, BOLT_H, raster(BOLT_W, BOLT_H, bolt))
    n += emit('plug', PLUG_W, PLUG_H, raster(PLUG_W, PLUG_H, plug))
    n += emit('warn', WARN_W, WARN_H, raster(WARN_W, WARN_H, warn))
    n += emit('moon', ICON, ICON, raster(ICON, ICON, moon))
    n += emit('sun', ICON, ICON, raster(ICON, ICON, sun))
    n += emit('corner_o', CO, CO, raster(CO, CO, corner(CO)))
    n += emit('corner_i', CI, CI, raster(CI, CI, corner(CI)))
    print('/* %d bytes of coverage */' % n)
    print('#endif /* _IPODUI_SYSSCREEN_DATA_H_ */')


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
import os as _os  # noqa: E402
import sys as _sys  # noqa: E402
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    prokbox_header.run_stdout(main)
