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

"""Compare two PNG screenshots pixel by pixel (stdlib only).

    scripts/pngdiff.py <actual.png> <golden.png> [--threshold PCT] [--rgb565]
    scripts/pngdiff.py --batch <pairs.txt> [--threshold PCT]

Prints the percentage of pixels that differ and exits 1 if it is above the
threshold (default 0.0, i.e. any differing pixel fails), 2 on a size
mismatch or unreadable file. Handles non-interlaced 8-bit greyscale, RGB,
palette, grey+alpha and RGBA PNGs (what sips writes for sim shots).
Alpha is ignored.

--rgb565 compares at the panel's precision (R>>3, G>>2, B>>3): for a sim
shot (sips expands RGB565 with rounding) against a host-rendered reference
(bit replication), which differ by up to 1 per channel at 8 bits even
where the 16-bit pixels are equal. Sim-vs-sim goldens never need it.

Batch lines may carry a 4th tab-separated field "x0,y0,x1,y1" (end
exclusive): only that rectangle is compared, and the percentage is of its
pixels (golden-region, for a shot whose rest is timing-dependent, e.g. the
art reflection under a sliding row).

--batch compares many pairs in one process (one Python interpreter start
instead of one per pair; validate.sh uses it to diff all of a script's
golden images together). <pairs.txt> has one "label<TAB>actual<TAB>golden"
per line (blank lines and "#" comments skipped); each line of output is
"ok   <label>: <result>" or "FAIL: <label>: <result>" (the gate's own
ok/FAIL convention, so a caller can echo it unchanged), in input order.
Exit status is the worst single pair: 2 if any had a size mismatch or
decode error, else 1 if any exceeded --threshold, else 0.
"""
import struct
import sys
import zlib


def decode(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat, plte = 8, [], None
    while pos < len(data):
        length, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if typ == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif typ == b'PLTE':
            plte = body
        elif typ == b'IDAT':
            idat.append(body)
        elif typ == b'IEND':
            break
        pos += 12 + length
    if depth != 8 or interlace:
        raise ValueError('only 8-bit non-interlaced PNGs are supported')
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        off = y * (stride + 1)
        ftype = raw[off]
        line = bytearray(raw[off + 1:off + 1 + stride])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 0xff
            elif ftype == 2:
                line[i] = (line[i] + b) & 0xff
            elif ftype == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xff
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xff
        rows.append(line)
        prev = line
    pixels = []
    for line in rows:
        for x in range(w):
            px = line[x * bpp:(x + 1) * bpp]
            if ctype == 0 or ctype == 4:
                pixels.append((px[0],) * 3)
            elif ctype == 3:
                pixels.append(tuple(plte[px[0] * 3:px[0] * 3 + 3]))
            else:
                pixels.append(tuple(px[:3]))
    return w, h, pixels


def diff_pair(actual_path, golden_path, threshold, region=None,
              rgb565=False):
    """Compare one pair. Returns (code, message): code 2 is a size
    mismatch or decode error, 1 a threshold-exceeding diff, 0 a pass."""
    try:
        wa, ha, pa = decode(actual_path)
        wg, hg, pg = decode(golden_path)
    except (OSError, ValueError, KeyError, zlib.error) as e:
        return 2, 'pngdiff: %s' % e
    if (wa, ha) != (wg, hg):
        return 2, 'size %dx%d != golden %dx%d' % (wa, ha, wg, hg)
    if region:
        x0, y0, x1, y1 = region
        idx = [y * wa + x for y in range(y0, min(y1, ha))
               for x in range(x0, min(x1, wa))]
        pa = [pa[i] for i in idx]
        pg = [pg[i] for i in idx]
    if rgb565:
        q = lambda p: (p[0] >> 3, p[1] >> 2, p[2] >> 3)
        diff = sum(1 for a, g in zip(pa, pg) if q(a) != q(g))
    else:
        diff = sum(1 for a, g in zip(pa, pg) if a != g)
    pct = 100.0 * diff / max(1, len(pa))
    msg = '%.3f%% pixels differ (%d of %d%s%s)' % (
        pct, diff, len(pa), ', region' if region else '',
        ', at RGB565' if rgb565 else '')
    return (0 if pct <= threshold else 1), msg


def run_batch(pairs_path, threshold):
    """Exit status: the worst code seen (2 if any pair had a size
    mismatch/decode error, else 1 if any pair exceeded the threshold,
    else 0); one result line per pair, in input order, regardless."""
    worst = 0
    with open(pairs_path, encoding='utf-8') as f:
        for lineno, line in enumerate(f, 1):
            line = line.rstrip('\n')
            if not line or line.startswith('#'):
                continue
            fields = line.split('\t')
            region = None
            if len(fields) == 4:
                region = tuple(int(v) for v in fields.pop().split(','))
            if len(fields) != 3:
                print('%s:%d: bad line (want label<TAB>actual<TAB>golden)'
                      % (pairs_path, lineno), file=sys.stderr)
                return 2
            label, actual_path, golden_path = fields
            code, msg = diff_pair(actual_path, golden_path, threshold, region)
            tag = 'ok  ' if code == 0 else 'FAIL:'
            print('%s %s: %s' % (tag, label, msg))
            worst = max(worst, code)
    return worst


def main():
    args = sys.argv[1:]
    threshold = 0.0
    rgb565 = '--rgb565' in args
    if rgb565:
        args.remove('--rgb565')
    if '--threshold' in args:
        i = args.index('--threshold')
        threshold = float(args[i + 1])
        del args[i:i + 2]
    if '--batch' in args:
        i = args.index('--batch')
        pairs_path = args[i + 1]
        del args[i:i + 2]
        if args:
            print(__doc__.strip(), file=sys.stderr)
            return 2
        return run_batch(pairs_path, threshold)
    if len(args) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    code, msg = diff_pair(args[0], args[1], threshold, rgb565=rgb565)
    print(msg)
    return code


if __name__ == '__main__':
    sys.exit(main())
