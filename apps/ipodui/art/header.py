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

"""Pre-render the header's glyphs as tiles (stdlib only).

    python3 -I apps/ipodui/art/header.py [OUT]   # default apps/ipodui/header_tiles.h

Phase 8b brief 4.2, lean rule 1: the header's background rows are fixed
(art/materials.py HEADER), so the play and pause glyphs, the hold lock and
the battery are blended over them here, once, and header.c draws each
with one lcd_bitmap_part(). The battery is the empty-glass tile with the
fill drawn over it at run time, one row colour per body row, from three
stored columns per gel (green, red when low). Every pixel is computed as header.c computed it at run time before
Phase 8b (ui_paint_blend(), 0..15 weights on unpacked RGB565), with the
fills now from the material kit: the sky gel for the play/pause glyph and
the green/red gel for the battery.

Each tile covers rows y0..y0+h-1 of the header (y0 below) and is opaque:
pixels outside the glyph are the header background of their row.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, 'art'))
sys.path.insert(0, HERE)
import materials as M  # noqa: E402
from play import play_map  # noqa: E402

pack, unpack = M.pack, M.unpack
BG = [pack((g, g, g)) for g in M.HEADER]


def rgb(r, g, b):
    return pack((r, g, b))


def blend(bg, fg, a):
    """ui_paint_blend(): fg over bg with weight a/15, C integer maths."""
    b8, f8 = unpack(bg), unpack(fg)
    return pack(tuple(b + M.cdiv((f - b) * a, 15) for b, f in zip(b8, f8)))


class Tile:
    def __init__(self, name, w, h, y0):
        self.name, self.w, self.h, self.y0 = name, w, h, y0
        self.px = [[BG[y0 + r]] * w for r in range(h)]

    def bg(self, r):
        return BG[self.y0 + r]

    def put(self, c, r, v):
        self.px[r][c] = v


# -- play / pause (header.c PLAY_*): 11 x 12 ink, a 1 px glow margin left
#    and below; per-row fill from the sky gel
PLAY_W, PLAY_H, PLAY_Y, PAUSE_BAR_W = 11, 12, 3, 4
PLAY_RIM = rgb(0x1f, 0x3d, 0x5c)
PLAY_GLOW = rgb(0xc4, 0xe6, 0xff)
PLAY_FILL = M.rows('GEL_SKY', PLAY_H)   # per variant: set_variant()


def set_variant(v):
    """art/materials.py SEL variant v (1 A, 2 B, 3 C) for the gel glyphs."""
    global PLAY_FILL
    M.sel_variant(v)
    PLAY_FILL = M.rows('GEL_SKY', PLAY_H)


def glyph_px(t, c, r, fill, cov, rim, glow):
    back = t.bg(r)
    if glow:
        back = blend(back, PLAY_GLOW, glow * 4)
    if rim:
        fill = blend(fill, PLAY_RIM, rim * 4)
    t.put(c, r, blend(back, fill, cov) if cov else back)


def play_tile():
    pm = play_map()
    t = Tile('hdr_play', len(pm[0]), len(pm), PLAY_Y)    # x origin: ink x - 1
    for r, row in enumerate(pm):
        for c, m in enumerate(row):
            if m:
                glyph_px(t, c, r, PLAY_FILL[min(r, PLAY_H - 1)],
                         m >> 4, (m >> 2) & 3, m & 3)
    return t


def pause_tile():
    t = Tile('hdr_pause', PLAY_W + 1, PLAY_H + 1, PLAY_Y)  # x origin: ink x - 1
    for r in range(PLAY_H + 1):
        for bar in range(2):
            bx = 1 + bar * (PLAY_W - PAUSE_BAR_W)
            if r == PLAY_H:                 # glow row under the bars
                for c in range(-1, PAUSE_BAR_W):
                    glyph_px(t, bx + c, r, 0, 0, 0, 1 if c < 0 else 2)
                continue
            glyph_px(t, bx - 1, r, 0, 0, 0, 3)      # glow column
            for c in range(PAUSE_BAR_W):
                vedge = c == 0 or c == PAUSE_BAR_W - 1
                hedge = r == 0 or r == PLAY_H - 1
                glyph_px(t, bx + c, r, PLAY_FILL[r],
                         11 if vedge and hedge else 15,
                         2 if vedge or hedge else 0, 2)
    return t


# -- hold lock (header.c LOCK_*): 9 x 13, shackle over a shaded body
LOCK_W, LOCK_H, LOCK_Y, LOCK_BODY_Y = 9, 13, 3, 5
LOCK_SHACKLE = rgb(0x3c, 0x3c, 0x3c)
LOCK_TOP = rgb(0x64, 0x64, 0x64)
LOCK_BOT = rgb(0x2e, 0x2e, 0x2e)
LOCK_EDGE = rgb(0x8c, 0x8c, 0x8c)
LOCK_SHACKLE_COV = [
    [0, 8, 15, 15, 15, 8, 0],
    [12, 6, 0, 0, 0, 6, 12],
    [15, 3, 0, 0, 0, 3, 15],
    [15, 2, 0, 0, 0, 2, 15],
    [15, 2, 0, 0, 0, 2, 15],
]


def lock_tile():
    t = Tile('hdr_lock', LOCK_W, LOCK_H, LOCK_Y)
    body_h = LOCK_H - 1 - LOCK_BODY_Y
    for r in range(LOCK_BODY_Y):
        for c, a in enumerate(LOCK_SHACKLE_COV[r]):
            if a:
                t.put(1 + c, r, blend(t.bg(r), LOCK_SHACKLE, a))
    for r in range(body_h):
        tr = LOCK_BODY_Y + r
        col = blend(LOCK_TOP, LOCK_BOT, r * 15 // (body_h - 1))
        for c in range(LOCK_W):
            t.put(c, tr, col)
        if r == body_h - 1:                 # rounded bottom corners
            t.put(0, tr, blend(t.bg(tr), col, 9))
            t.put(LOCK_W - 1, tr, blend(t.bg(tr), col, 9))
    for c in range(LOCK_W):
        t.put(c, LOCK_H - 1, LOCK_EDGE)
    return t


# -- battery (header.c BATT_*): 24 x 12 body, 2 x 4 nub, emboss row
BATT_W, BATT_H, BATT_Y, BATT_NUB_W, BATT_NUB_H = 24, 12, 3, 2, 4
BATT_FILL_W, BATT_FILL_H = BATT_W - 2, BATT_H - 2
LINE_TOP = rgb(0x5e, 0x6b, 0x4f)
LINE_BOT = rgb(0x6d, 0x83, 0x5d)
LINE_L = rgb(0x60, 0x6e, 0x50)
LINE_R = rgb(0x68, 0x7a, 0x5e)
NUB = rgb(0x5e, 0x64, 0x5c)
NUB_GLASS = rgb(0xc8, 0xd0, 0xc6)
EMBOSS = rgb(0xee, 0xf4, 0xea)
GLASS = [rgb(*v) for v in [                 # the empty part (main.png)
    (0xd4, 0xd6, 0xd2), (0xfb, 0xfb, 0xfb), (0xe9, 0xea, 0xe8),
    (0xe4, 0xe6, 0xe4), (0xe0, 0xe1, 0xdf), (0xb9, 0xbb, 0xb8),
    (0xa7, 0xa9, 0xa6), (0xac, 0xae, 0xab), (0xb1, 0xb3, 0xb0),
    (0x9f, 0xa1, 0x9e)]]


def battery_tile(name, pal):
    """The whole battery with every interior column in 'pal'."""
    w = BATT_W + BATT_NUB_W
    t = Tile(name, w, BATT_H + 1, BATT_Y)
    x1, y1 = BATT_W - 1, BATT_H - 1
    for c in range(1, x1):
        t.put(c, 0, LINE_TOP)
        t.put(c, y1, LINE_BOT)
    for c in (0, x1):
        t.put(c, 0, blend(t.bg(0), LINE_TOP, 8))
        t.put(c, y1, blend(t.bg(y1), LINE_BOT, 8))
    for r in range(1, y1):
        t.put(0, r, LINE_L)
        t.put(x1, r, LINE_R)
    for r in range(BATT_FILL_H):
        for c in range(1, x1):
            t.put(c, 1 + r, pal[r])
        t.put(1, 1 + r, blend(pal[r], LINE_L, 5))
        t.put(x1 - 1, 1 + r, blend(pal[r], LINE_R, 5))
    n = (BATT_H - BATT_NUB_H) // 2
    nb = n + BATT_NUB_H - 1
    t.put(x1 + 1, n, blend(t.bg(n), NUB, 12))
    t.put(x1 + 1, nb, blend(t.bg(nb), NUB, 12))
    t.put(x1 + 2, n, blend(t.bg(n), NUB, 5))
    t.put(x1 + 2, nb, blend(t.bg(nb), NUB, 5))
    for r in range(n + 1, nb):
        t.put(x1 + 1, r, NUB_GLASS)
        t.put(x1 + 2, r, blend(t.bg(n + 1), NUB, 11))
    e = y1 + 1                              # emboss under the body
    for c in range(1, x1):
        t.put(c, e, blend(t.bg(e), EMBOSS, 12))
    for c in (0, x1):
        t.put(c, e, blend(t.bg(e), EMBOSS, 6))
    return t


def emit_tile(L, t):
    up = t.name.upper()
    L.append('#define %s_W %d' % (up, t.w))
    L.append('#define %s_H %d' % (up, t.h))
    L.append('#define %s_Y %d   /* header row of tile row 0 */' % (up, t.y0))
    L.append('static const unsigned short %s[%s_W * %s_H] = {'
             % (t.name, up, up))
    for row in t.px:
        for i in range(0, len(row), 13):
            L.append('    ' + ' '.join('0x%04x,' % v for v in row[i:i + 13]))
    L.append('};')
    L.append('')
    return t.w * t.h * 2


def emit_fill(L, name, gel):
    """The battery fill: each body row is one colour, so only three
    columns are stored per gel, taken from the whole-battery tile (so the
    pixels are that tile's): the left end (shaded toward the outline), the
    body and the right end (a full fill only). header.c draws them over
    the glass."""
    t = battery_tile(name, M.rows(gel, BATT_FILL_H))
    L.append('static const unsigned short %s[3][HDR_BATT_FILL_H] = {' % name)
    for c in (1, 2, BATT_W - 2):
        L.append('    { ' + ' '.join('0x%04x,' % t.px[1 + r][c]
                                     for r in range(BATT_FILL_H)) + ' },')
    L.append('};')
    return 3 * BATT_FILL_H * 2


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, 'apps', 'ipodui', 'header_tiles.h')
    L = ['/* apps/ipodui/header_tiles.h: GENERATED by apps/ipodui/art/header.py',
         ' * from art/materials.py; do not edit. Included by header.c only.',
         ' * Header glyphs pre-blended over the header rows (RGB565, row-major,',
         ' * opaque). GPL v2+. */',
         '#ifndef IPODUI_HEADER_TILES_H',
         '#define IPODUI_HEADER_TILES_H', '',
         '#ifndef UI_MAT_SEL_VARIANT     /* gel-variants: 1 A, 2 B (default), 3 C */',
         '#define UI_MAT_SEL_VARIANT 2',
         '#endif', '',
         '#define HDR_BATT_FILL_H %d' % BATT_FILL_H,
         '/* fill columns per gel: [left end, body, right end][row] */']
    size = emit_fill(L, 'hdr_batt_red', 'GEL_RED')
    L.append('')
    size += emit_fill(L, 'hdr_batt_amber', 'GEL_AMBER')
    L.append('')
    for t in (lock_tile(), battery_tile('hdr_batt_glass', GLASS)):
        size += emit_tile(L, t)
    # the gel glyphs and the green fill follow the selection-bar variant
    for v in sorted(M.SEL_NAMES):
        set_variant(v)
        L.append('#%s UI_MAT_SEL_VARIANT == %d    /* variant %s */'
                 % ('if' if v == 1 else 'elif', v, M.SEL_NAMES[v]))
        n = emit_tile(L, play_tile()) + emit_tile(L, pause_tile())
        n += emit_fill(L, 'hdr_batt_green', 'GEL_GREEN')
        size += n if v == 1 else 0
    set_variant(1)
    L.append('#else')
    L.append('#error "UI_MAT_SEL_VARIANT: 1, 2 or 3"')
    L.append('#endif')
    L.append('')
    L.append('#endif')
    with open(out, 'w') as fp:
        fp.write('\n'.join(L) + '\n')
    prokbox_header.stamp(out)
    print('%s: 4 tiles + 3 fill tables, %d B' % (os.path.relpath(out, ROOT), size))


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
import os as _os  # noqa: E402
import sys as _sys  # noqa: E402
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    main()
