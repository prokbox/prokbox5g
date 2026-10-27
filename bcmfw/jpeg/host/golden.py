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

"""The JPEG spike's golden table (jpeg-spike.md step S1): every corpus
file (tests/jpeg/make_corpus.sh) through the shell's own decoder and
resizer (build/jpeg-host/jpegref, bcmfw/jpeg/host/build.sh) at the art
builder's sizes: the panel 300 x 300, the thumbnail 55 x 55, and the Now
Playing 142 box, square and (for a non-square source) fitted.  One line
per output: file, asked size, the size and status the decoder returned,
and the FNV-1a of its RGB565 bytes.  The BCM port (S2) must reproduce
every line.

Usage: bcmfw/jpeg/host/golden.py [--check]   (writes or checks
tests/jpeg/golden.txt)
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
CORPUS = os.path.join(ROOT, 'build', 'jpeg', 'corpus')
REF = os.path.join(ROOT, 'build', 'jpeg-host', 'jpegref')
GOLDEN = os.path.join(ROOT, 'tests', 'jpeg', 'golden.txt')
TMP = os.path.join(ROOT, 'build', 'jpeg', 'out.rgb565')


def sof_size(path):
    """(w, h) from the first SOF marker, or None"""
    d = open(path, 'rb').read()
    i = 2
    while i + 9 < len(d):
        if d[i] != 0xFF:
            return None
        m, ln = d[i + 1], d[i + 2] << 8 | d[i + 3]
        if 0xC0 <= m <= 0xC3:
            return d[i + 7] << 8 | d[i + 8], d[i + 5] << 8 | d[i + 6]
        i += 2 + ln
    return None


def fit(w, h, box):
    m = max(w, h)
    return max(1, (w * box + m // 2) // m), max(1, (h * box + m // 2) // m)


def lines():
    out = []
    for name in sorted(os.listdir(CORPUS)):
        path = os.path.join(CORPUS, name)
        sizes = [(300, 300), (142, 142), (55, 55)]
        sz = sof_size(path)
        if sz and sz[0] != sz[1]:
            sizes.append(fit(sz[0], sz[1], 142))
        for w, h in sizes:
            r = subprocess.run([REF, path, str(w), str(h), TMP], capture_output=True,
                               text=True)
            got = r.stdout.split() or ['0', '0', 'crash', '0']
            out.append('%-24s %4d %4d  %s %s %s %s' % (name, w, h, got[0], got[1],
                                                     got[2], got[3]))
    return out


def main(argv):
    if not os.path.exists(REF):
        print('golden: build/jpeg-host/jpegref missing: bcmfw/jpeg/host/build.sh')
        return 1
    text = '# file asked-w asked-h  got-w got-h status fnv1a (golden.py)\n' + \
        '\n'.join(lines()) + '\n'
    if '--check' in argv:
        if open(GOLDEN).read() != text:
            print('golden: differs from tests/jpeg/golden.txt')
            return 1
        print('golden: matches')
        return 0
    open(GOLDEN, 'w').write(text)
    print('golden: %d lines' % (text.count('\n') - 1))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
