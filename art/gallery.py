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

"""Style gallery: every material and every surface on one contact sheet.

    art/gallery.py [-o out/review/p8b/gallery.png] [--header apps/ipodui/material_data.h]
                   [--shots out/shots/ipodui-p8b-gallery] [--scale 3]

HOST-SIDE ONLY (stdlib only): it reads files and writes one PNG under
out/ (git-ignored); no firmware byte depends on it. Phase 8b brief 4.0.

Top half, MATERIALS: every colour table in the generated
apps/ipodui/material_data.h (brief 3.1: one const row table per (material,
height) pair, `ui_mat_*`, plus the shadow alpha table `ui_shadow_a`) is
drawn as a vertical strip (RGB565 rows, the object as the shell draws it,
scaled up) in the order the header lists them, with the shadow table drawn
as a seam strip over mid-grey. A table is any C array whose name starts
with `ui_mat`, or contains `shadow`, with integer literals; rows of 16-bit
RGB565 values (names without `shadow`) or 8-bit alphas (`shadow`). When
material_data.h does not exist yet (before 8b-1) this half is skipped with
a note: the in-situ half still renders.

Bottom half, IN SITU: the shots of tests/ui/ipodui-p8b-gallery.script
(header glyph states: battery 0/9/50/100 %, charging, hold lock, play,
pause; main menu; Settings; About) from --shots, run first with
    SIM_BUILD=build/sim-ui scripts/shots.sh tests/ui/ipodui-p8b-gallery.script
Missing shots are listed and left out. The legend (what each cell is, in
order) is printed and written next to the PNG as <out>.txt, since the
sheet has no text.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
from pngdiff import decode  # noqa: E402
from pngcompose import write_png  # noqa: E402

BG = (0x40, 0x40, 0x40)
SHOTS = ['main_menu', 'hdr_batt0', 'hdr_batt9', 'hdr_batt50', 'hdr_batt100',
         'hdr_charging', 'hdr_lock', 'hdr_play', 'hdr_pause',
         'settings_panel', 'settings_top', 'about_1', 'about_2', 'about_3']


def rgb565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def parse_tables(path):
    """[(name, [ints])] for the ui_mat* / *shadow* arrays of a C header
    (the first definition of each name: variant A of UI_MAT_SEL_VARIANT)."""
    txt = re.sub(r'/\*.*?\*/', '', open(path).read(), flags=re.S)
    out = []
    for m in re.finditer(r'(\w+)\s*\[[^\]]*\](?:\s*\[[^\]]*\])?\s*=\s*\{(.*?)\}\s*;',
                         txt, re.S):
        name, body = m.group(1), m.group(2)
        if not (name.startswith('ui_mat') or 'shadow' in name) \
                or any(n == name for n, _v in out):
            continue
        vals = [int(x, 0) for x in re.findall(r'0[xX][0-9a-fA-F]+|\b\d+\b', body)]
        if vals:
            out.append((name, vals))
    return out


def material_cell(name, vals, scale):
    """(w, h, rows of RGB tuples) for one table, label-free."""
    if 'shadow' in name:
        w = len(vals) * scale * 4
        row = []
        for a in vals:
            g = 0x90 * (256 - min(a, 256)) // 256     # grey 0x90 darkened
            row += [(g, g, g)] * (scale * 4)
        return w, 24 * scale // 3 * 3, [row] * (24 * scale // 3 * 3)
    w = 12 * scale
    rows = []
    for v in vals:
        c = rgb565(v & 0xffff)
        rows += [[c] * w for _ in range(scale)]
    return w, len(rows), rows


def sheet(cells, cols, pad=6, bg=BG):
    """cells: [(w, h, rows)] -> (W, H, rows) in `cols` columns."""
    grid = [cells[i:i + cols] for i in range(0, len(cells), cols)]
    colw = [max((c[0] for c in col if c), default=0)
            for col in zip(*[r + [None] * (cols - len(r)) for r in grid])]
    W = sum(colw) + pad * (cols + 1)
    out, H = [], pad
    for r in grid:
        rh = max(c[1] for c in r)
        block = [[bg] * W for _ in range(rh)]
        x = pad
        for ci in range(cols):
            if ci < len(r):
                w, h, rows = r[ci]
                for y in range(h):
                    block[y][x:x + w] = rows[y]
            x += colw[ci] + pad
        out += [[bg] * W] * pad + block
        H += pad + rh
    return W, H + pad, out + [[bg] * W] * pad


def shot_cell(path, scale):
    w, h, px = decode(path)
    rows = []
    for y in range(h):
        line = []
        for x in range(w):
            line += [px[y * w + x]] * scale
        rows += [line] * scale
    return w * scale, h * scale, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('-o', '--out', default=os.path.join(ROOT, 'out/review/p8b/gallery.png'))
    ap.add_argument('--header', default=os.path.join(ROOT, 'apps/ipodui/material_data.h'))
    ap.add_argument('--shots', default=os.path.join(ROOT, 'out/shots/ipodui-p8b-gallery'))
    ap.add_argument('--scale', type=int, default=3, help='swatch scale (shots are 1:1)')
    a = ap.parse_args()

    legend, mats, shots = [], [], []
    if os.path.exists(a.header):
        for name, vals in parse_tables(a.header):
            mats.append(material_cell(name, vals, a.scale))
            legend.append('material %s (%d %s)' % (
                name, len(vals), 'alphas' if 'shadow' in name else 'rows'))
        if not mats:
            print('gallery: note: %s has no ui_mat*/shadow tables' % a.header)
    else:
        print('gallery: note: no %s yet (before 8b-1): materials skipped'
              % os.path.relpath(a.header, ROOT))
    for s in SHOTS:
        p = os.path.join(a.shots, s + '.png')
        if os.path.exists(p):
            shots.append(shot_cell(p, 1))
            legend.append('shot %s' % s)
        else:
            print('gallery: note: no shot %s (run tests/ui/ipodui-p8b-gallery.script)' % s)
    if not (mats or shots):
        print('gallery: nothing to draw')
        return 1
    parts = []
    if mats:
        parts.append(sheet(mats, min(len(mats), 12)))
    if shots:
        parts.append(sheet(shots, 4))
    W = max(p[0] for p in parts)
    rows = []
    for w, h, r in parts:
        rows += [line + [BG] * (W - w) for line in r]
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    write_png(a.out, W, len(rows), [[c for px in r for c in px] for r in rows])
    with open(os.path.splitext(a.out)[0] + '.txt', 'w') as f:
        f.write('\n'.join('%2d  %s' % (i + 1, l) for i, l in enumerate(legend)) + '\n')
    print('gallery: %s (%dx%d, %d materials, %d shots)'
          % (a.out, W, len(rows), len(mats), len(shots)))
    for i, l in enumerate(legend):
        print('  %2d  %s' % (i + 1, l))
    return 0


if __name__ == '__main__':
    sys.exit(main())
