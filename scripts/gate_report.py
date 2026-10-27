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

"""One-page failure report: [golden | actual | diff] composites (stdlib only).

    scripts/gate_report.py <fails.tsv> [--out DIR] [--left NAME] [--right NAME]

<fails.tsv> has one "label<TAB>actual.png<TAB>golden.png" per line (the same
column order as scripts/pngdiff.py --batch pairs; extra columns ignored).
For each line a composite out/gate-report/<n>-<label>.png is written with
the golden on the left, the actual in the middle and a diff on the right
(differing pixels red over a dimmed copy of the golden; a size mismatch
shows only the two images). DIR/index.md links every composite, with the
label, so a failure can be reviewed in seconds. Pairs that cannot be
decoded are listed without a composite. Prints the index path.
golden_capture.sh uses --left run1 --right runN (a reproducibility
failure has no golden, only two runs).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngdiff import decode  # noqa: E402
from pngcompose import write_png, GUTTER, BG  # noqa: E402


def main():
    args = sys.argv[1:]
    out, left, right = 'out/gate-report', 'golden', 'actual'
    pos = []
    while args:
        a = args.pop(0)
        if a == '--out':
            out = args.pop(0)
        elif a == '--left':
            left = args.pop(0)
        elif a == '--right':
            right = args.pop(0)
        else:
            pos.append(a)
    if len(pos) != 1:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith('.png') or f == 'index.md':
            os.remove(os.path.join(out, f))
    lines = []
    for n, line in enumerate(open(pos[0]).read().splitlines(), 1):
        cols = line.split('\t')
        if len(cols) < 3 or not cols[0].strip():
            continue
        label, actual, golden = cols[0], cols[1], cols[2]
        slug = re.sub(r'[^A-Za-z0-9._-]+', '_', label)[:80]
        name = '%02d-%s.png' % (n, slug)
        try:
            wa, ha, pa = decode(actual)
            wg, hg, pg = decode(golden)
        except Exception as e:  # unreadable: list it, no composite
            lines.append('- `%s`: no composite (%s)' % (label, e))
            continue
        same = (wa, ha) == (wg, hg)
        cw, ch = max(wa, wg), max(ha, hg)
        ncell = 3 if same else 2
        W = GUTTER + ncell * (cw + GUTTER)
        H = GUTTER + ch + GUTTER
        canvas = [bytearray(bytes(BG) * W) for _ in range(H)]

        def paste(cell, w, h, px):
            ox = GUTTER + cell * (cw + GUTTER)
            for y in range(h):
                row = canvas[GUTTER + y]
                for x in range(w):
                    row[(ox + x) * 3:(ox + x) * 3 + 3] = bytes(px[y * w + x])

        paste(0, wg, hg, pg)
        paste(1, wa, ha, pa)
        ndiff = 0
        if same:
            dpx = []
            for g, a in zip(pg, pa):
                if g != a:
                    ndiff += 1
                    dpx.append((255, 0, 0))
                else:
                    dpx.append((g[0] // 3 + 85, g[1] // 3 + 85, g[2] // 3 + 85))
            paste(2, wa, ha, dpx)
        write_png(os.path.join(out, name), W, H, canvas)
        desc = ('%d px differ' % ndiff) if same else \
            'size %dx%d vs %dx%d' % (wa, ha, wg, hg)
        lines.append('- `%s` (%s | %s%s): %s\n\n  ![%s](%s)' % (
            label, left, right, ' | diff' if same else '', desc, label, name))
    idx = os.path.join(out, 'index.md')
    with open(idx, 'w') as f:
        f.write('# Failure report\n\n%d failing pair(s). Each composite is '
                '[%s | %s | diff].\n\n' % (len(lines), left, right))
        f.write('\n'.join(lines) + '\n')
    print(idx)
    return 0


if __name__ == '__main__':
    sys.exit(main())
