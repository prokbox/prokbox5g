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

"""The shell's material kit: one source of truth for its key colours.

    art/materials.py [OUT]      # default apps/ipodui/material_data.h

HOST-SIDE ONLY (stdlib only). Phase 8b brief 3.1, spec index.md "Visual
language": every material (blue gel, green/red gel, grey gloss, metal,
groove) is defined here once, as key colours down the object, and this
script writes apps/ipodui/material_data.h (generated, committed):

  * the keys of every material, so material.c can evaluate any height at
    run time (ui_mat_color(), the fallback; never per pixel);
  * one const row table per (material, height) pair the shell draws at
    run time (RUNTIME below; ui_mat_rows() returns them, else NULL);
  * preview tables for the style gallery (PREVIEW below), compiled out
    (#ifdef UI_MAT_PREVIEW, never defined in a build): pairs only the
    build-time art uses (header glyphs, battery) or that no surface uses
    yet (metal, grey gloss);
  * the header background rows and the split-screen shadow alphas.

Art generators import this module (apps/ipodui/art/header.py), so a glyph
pre-rendered at build time and a surface drawn at run time share colours.

The row model (identical in material.c, integer for integer): an object n
rows tall has an optional light first row ('line'), an optional darker
last row ('edge') and a body between them; body row j of nb gets
t = j * 255 / max(nb - 1, 1) and the colour interpolated between the two
keys around t, with C's truncating division. Colours pack to RGB565
(the LCD's native order, LCD_RGBPACK).

The blue gel's keys are Now Playing's, unchanged (tests/material holds a
frozen golden of them); the sky gel is the same profile lifted toward the
lighter sky blue of the list selection bar (owner default O1).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The materials, in material.h's enum order. keys: (t 0..255, r, g, b).
MATERIALS = [
    # Now Playing's bar fill, scrubber diamond and stars (owner
    # 2026-10-08; photo 1209/1222): a faint lighter top line, medium blue
    # deepening to the richest blue around the middle, brightening toward
    # cyan in the lower third, brightest just above a 1 px darker edge.
    ('GEL_BLUE', dict(
        line=(0x7a, 0xaa, 0xf2), edge=(0x1c, 0x6a, 0xb8),
        keys=[(0, 74, 138, 238), (40, 42, 118, 234), (100, 22, 104, 228),
              (140, 14, 98, 224), (185, 22, 124, 232), (225, 34, 152, 238),
              (255, 42, 164, 242)])),
    # The same gel lifted toward sky blue: the list selection bar and the
    # header play/pause glyph. Top line and upper body after the 2009
    # guide's bar (#98DEF8 over #3EABE3, MEASUREMENTS.md "Selection bar
    # rows"), deepest near #1E88D8 below the middle, a gentle cyan lift
    # in the lower third (the owner's 2.0.5 gel), a darker foot.
    ('GEL_SKY', dict(
        line=(0x9a, 0xde, 0xf8), edge=(0x1a, 0x5e, 0x9e),
        keys=[(0, 72, 176, 232), (40, 60, 166, 228), (100, 40, 148, 222),
              (150, 30, 136, 216), (195, 36, 148, 222), (230, 46, 162, 230),
              (255, 54, 172, 236)])),
    # The battery fill: the gel profile in green (spec index.md).
    ('GEL_GREEN', dict(
        line=(0xce, 0xf6, 0xb4), edge=(0x36, 0x60, 0x26),
        keys=[(0, 150, 222, 112), (40, 126, 208, 88), (100, 84, 172, 52),
              (150, 64, 150, 36), (195, 80, 172, 50), (230, 106, 196, 74),
              (255, 124, 210, 92)])),
    # Low battery: the gel profile in red.
    ('GEL_RED', dict(
        line=(0xfc, 0xcc, 0xc4), edge=(0x80, 0x22, 0x1a),
        keys=[(0, 242, 128, 116), (40, 232, 100, 88), (100, 206, 58, 44),
              (150, 184, 38, 28), (195, 204, 58, 44), (230, 226, 88, 72),
              (255, 238, 108, 92)])),
    # Middling battery (30 % or less): the gel profile in amber. Owner
    # request 2026-10-22; the 7G's references show green and red only.
    ('GEL_AMBER', dict(
        line=(0xfc, 0xe8, 0xa8), edge=(0x88, 0x60, 0x10),
        keys=[(0, 250, 214, 104), (40, 242, 194, 70), (100, 224, 164, 24),
              (150, 204, 140, 10), (195, 222, 164, 26), (230, 238, 188, 56),
              (255, 246, 204, 80)])),
    # A grey rod lit from the left (scrollbar thumb, across its width):
    # today's sbar_gloss, as keys. No surface reads it yet.
    ('GLOSS_GREY', dict(
        line=None, edge=(0x58, 0x58, 0x58),
        keys=[(0, 0xb6, 0xb6, 0xb6), (120, 0x9c, 0x9c, 0x9c),
              (135, 0x7c, 0x7c, 0x7c), (255, 0x66, 0x66, 0x66)])),
    # Metal (Settings tiles, the panel capsule's channel; 8b-3): a dark
    # button lit from above (owner 2026-10-08, settings-*.jpg): a light
    # rim, then a smooth fall from slate grey to near black, steeper in
    # the upper half (the sheen), flattening out toward the foot.
    ('METAL', dict(
        line=(0x76, 0x7a, 0x84), edge=None,
        keys=[(0, 0x4e, 0x52, 0x5c), (60, 0x36, 0x39, 0x41),
              (128, 0x1d, 0x1f, 0x25), (200, 0x0d, 0x0e, 0x12),
              (255, 0x06, 0x07, 0x0a)])),
    # The groove (Now Playing's empty track, the capsule channel): a dark
    # top line (inner shadow), a light body slightly darker at the top and
    # in its lower middle, a darker bottom line. Keys sit exactly on the
    # 14-row bar's body rows, so that height is today's table.
    ('GROOVE', dict(
        line=(0xb4, 0xb4, 0xb4), edge=(0x9c, 0x9c, 0x9c),
        keys=[(0, 0xe2, 0xe2, 0xe2), (23, 0xec, 0xec, 0xec),
              (46, 0xee, 0xee, 0xee), (69, 0xec, 0xec, 0xec),
              (92, 0xe6, 0xe6, 0xe6), (115, 0xdf, 0xdf, 0xdf),
              (139, 0xd6, 0xd6, 0xd6), (162, 0xd2, 0xd2, 0xd2),
              (185, 0xd8, 0xd8, 0xd8), (208, 0xde, 0xde, 0xde),
              (231, 0xe0, 0xe0, 0xe0), (255, 0xde, 0xde, 0xde)])),
]
NAMES = [m[0] for m in MATERIALS]
MAT = dict(MATERIALS)

# Selection bar and battery candidates for the owner (gel-variants, fidelity
# audit row 12): the build-time switch UI_MAT_SEL_VARIANT (2 = B, the
# default; 1 = A, 3 = C) picks one; each variant replaces the sky gel (selection bar,
# header play/pause) and the green gel (battery fill) together. Measured on
# the owner's photos (photo 1212, 1186, 1199, 1214, cast-corrected by the list
# white): the 7G bar is a deep saturated blue, hue ~207, lightest at the
# top, deepening steadily to the bottom, no cyan lift, a darker foot; the
# battery a muted green with a bright gloss band in its upper third.
#   A: today's sky gel and green gel (unchanged).
#   C: the measured target.
#   B: midway, A and C averaged key for key.
SEL_C = {
    'GEL_SKY': dict(
        line=(0x7c, 0xbc, 0xec), edge=(0x0a, 0x48, 0x96),
        keys=[(0, 64, 152, 224), (50, 44, 136, 216), (110, 26, 116, 208),
              (170, 16, 100, 200), (220, 12, 92, 194), (255, 10, 86, 190)]),
    'GEL_GREEN': dict(
        line=(0xd2, 0xec, 0xb0), edge=(0x34, 0x58, 0x28),
        keys=[(0, 196, 230, 160), (60, 160, 204, 124), (100, 112, 158, 80),
              (150, 92, 138, 62), (200, 84, 128, 56), (255, 92, 136, 62)]),
}
SEL_NAMES = {1: 'A', 2: 'B', 3: 'C'}


def _mix(a, c):
    """Material halfway between a and c: keys at every t of either."""
    ts = sorted(set(k[0] for k in a['keys']) | set(k[0] for k in c['keys']))
    avg = lambda p, q: tuple((x + y) // 2 for x, y in zip(p, q))
    return dict(line=avg(a['line'], c['line']), edge=avg(a['edge'], c['edge']),
                keys=[(t,) + avg(key_lerp(a['keys'], t), key_lerp(c['keys'], t))
                      for t in ts])


_SEL_A = {n: dict(MAT[n]) for n in SEL_C}


def sel_variant(v):
    """Make variant v (1 A, 2 B, 3 C) current in MAT / MATERIALS."""
    for n in SEL_C:
        m = {1: _SEL_A[n], 3: SEL_C[n]}.get(v) or _mix(_SEL_A[n], SEL_C[n])
        MAT[n].clear()
        MAT[n].update(m)

# (material, height) pairs drawn at run time: one const table each.
RUNTIME = [
    ('GEL_BLUE', 14), ('GEL_BLUE', 15), ('GEL_BLUE', 19),   # NP bar, diamond, star
    ('GROOVE', 14),                                         # NP track
    ('GEL_SKY', 24), ('GEL_SKY', 25),                       # 9-row lists
    ('GEL_SKY', 44),                                        # two-line rows
    ('GEL_SKY', 55),                                        # art rows
    ('METAL', 76),                                          # Settings tile
]
# Gallery-only previews (compiled out): build-time art and later users.
PREVIEW = [
    ('GEL_SKY', 12),                                        # header glyph
    ('GEL_GREEN', 10), ('GEL_RED', 10), ('GEL_AMBER', 10),                    # battery fill
    ('GLOSS_GREY', 8),
]

# The header background, y 0..19 (theme.c's table, moved here): rows 1-18
# the median of x 50-95 in main.png, row 0 from main-alt.png and
# nowplaying-alt.png, flat at the top and a steep drop at y 8-13, the
# main.png profile with its gloss softened 15 % and darkened (owner
# 2026-10-06); the last row is the bottom line (Now Playing's value).
HEADER = [0xed, 0xea, 0xe3, 0xe0, 0xdf, 0xda, 0xd7, 0xd4, 0xcf, 0xc8,
          0xc0, 0xba, 0xb7, 0xb2, 0xac, 0xa6, 0xa3, 0xa3, 0x99, 0x6a]

# Split-screen shadow (spec main-menu.md): alpha /256 of black for panel
# columns 0..W-1 from the seam, quadratic falloff. Owner 2026-10-20, after
# the 8b-3 shots: "a little more intense, but not by much, and maybe 10px"
# (was the O9 default, 6 px at 30 %).
SHADOW_W = 10
SHADOW_SEAM = 92                    # 36 % of 256
SHADOW = [(SHADOW_SEAM * (SHADOW_W - c) ** 2 + SHADOW_W ** 2 // 2)
          // SHADOW_W ** 2 for c in range(SHADOW_W)]


def cdiv(a, b):
    """C's integer division (truncates toward zero)."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def pack(rgb):
    """LCD_RGBPACK on the 5.5G: RGB565."""
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def unpack(v):
    """RGB565 -> 8-bit RGB, as RGB_UNPACK_* (bit replication)."""
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def key_lerp(keys, t):
    """Now Playing's key_lerp(): the colour at t between two keys."""
    i = 1
    while i < len(keys) - 1 and t > keys[i][0]:
        i += 1
    a, b = keys[i - 1], keys[i]
    span = b[0] - a[0]
    f = cdiv((t - a[0]) * 256, span) if span else 0
    f = max(0, min(256, f))
    return tuple(a[k] + cdiv((b[k] - a[k]) * f, 256) for k in (1, 2, 3))


def row_rgb(name, i, n):
    """8-bit colour of row i of material 'name' for an object n rows tall."""
    m = MAT[name]
    lo = 1 if m['line'] else 0
    hi = n - 2 if m['edge'] else n - 1
    if m['line'] and i == 0:
        return m['line']
    if m['edge'] and i == n - 1 and n > 1:
        return m['edge']
    nb = hi - lo + 1
    return key_lerp(m['keys'], cdiv((i - lo) * 255, max(nb - 1, 1)))


def rows(name, n):
    """RGB565 rows of material 'name', n tall (what ui_mat_rows() holds)."""
    return [pack(row_rgb(name, i, n)) for i in range(n)]


def table_name(name, n):
    return 'ui_mat_%s_%d' % (name.lower(), n)


def emit_rows(L, name, n):
    v = rows(name, n)
    L.append('static const uint16_t %s[%d] = {' % (table_name(name, n), n))
    for i in range(0, n, 8):
        L.append('    ' + ' '.join('0x%04x,' % x for x in v[i:i + 8]))
    L.append('};')


def emit_materials(L):
    """The variant-dependent part: keys, definitions, row tables."""
    L.append('/* keys per material (enum ui_mat order): t 0..255, r, g, b */')
    for name, m in MATERIALS:
        L.append('static const struct ui_mat_key mat_keys_%s[%d] = {'
                 % (name.lower(), len(m['keys'])))
        for k in m['keys']:
            L.append('    { %3d, 0x%02x, 0x%02x, 0x%02x },' % k)
        L.append('};')
    L.append('static const struct ui_mat_def mat_defs[UI_MAT_COUNT] = {')
    for name, m in MATERIALS:
        line = '0x%04x' % pack(m['line']) if m['line'] else '0'
        edge = '0x%04x' % pack(m['edge']) if m['edge'] else '0'
        flags = ' | '.join(f for f, on in (('MAT_LINE', m['line']),
                                           ('MAT_EDGE', m['edge'])) if on) or '0'
        L.append('    { mat_keys_%s, %d, %s, %s, %s },   /* %s */'
                 % (name.lower(), len(m['keys']), line, edge, flags, name))
    L.append('};')
    L.append('')
    L.append('/* run-time row tables, one per (material, height) pair */')
    for name, n in RUNTIME:
        emit_rows(L, name, n)
    L.append('static const struct ui_mat_table mat_tables[] = {')
    for name, n in RUNTIME:
        L.append('    { UI_MAT_%s, %d, %s },' % (name, n, table_name(name, n)))
    L.append('};')
    L.append('')
    L.append('#ifdef UI_MAT_PREVIEW    /* the style gallery only (art/gallery.py); '
             'never built */')
    for name, n in PREVIEW:
        emit_rows(L, name, n)
    L.append('#endif')


def generate():
    L = []
    L.append('/* apps/ipodui/material_data.h: GENERATED by art/materials.py; '
             'do not edit.')
    L.append(' * Regenerate: python3 -I art/materials.py (tests/material '
             'checks it is current).')
    L.append(' * Included by material.c only. GPL v2+. */')
    L.append('#ifndef IPODUI_MATERIAL_DATA_H')
    L.append('#define IPODUI_MATERIAL_DATA_H')
    L.append('')
    L.append('#ifndef UI_MAT_SEL_VARIANT     /* gel-variants: 1 A, 2 B (default), 3 C */')
    L.append('#define UI_MAT_SEL_VARIANT 2')
    L.append('#endif')
    for v in sorted(SEL_NAMES):
        sel_variant(v)
        L.append('#%s UI_MAT_SEL_VARIANT == %d    /* variant %s */'
                 % ('if' if v == 1 else 'elif', v, SEL_NAMES[v]))
        emit_materials(L)
    sel_variant(1)
    L.append('#else')
    L.append('#error "UI_MAT_SEL_VARIANT: 1, 2 or 3"')
    L.append('#endif')
    L.append('')
    L.append('/* header background, y 0..%d (theme.c header_rows) */'
             % (len(HEADER) - 1))
    L.append('const unsigned short ui_mat_header_rows[%d] = {' % len(HEADER))
    hv = [pack((g, g, g)) for g in HEADER]
    for i in range(0, len(hv), 8):
        L.append('    ' + ' '.join('0x%04x,' % x for x in hv[i:i + 8]))
    L.append('};')
    L.append('')
    L.append('/* split-screen shadow: alpha /256 of black, column 0 = the seam */')
    L.append('const uint8_t ui_shadow_a[%d] = { %s };'
             % (SHADOW_W, ', '.join(str(a) for a in SHADOW)))
    L.append('')
    L.append('#endif')
    return '\n'.join(L) + '\n'


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, 'apps', 'ipodui', 'material_data.h')
    with open(out, 'w') as fp:
        fp.write(generate())
    prokbox_header.stamp(out)
    n = sum(h for _m, h in RUNTIME) * 2
    print('%s: %d materials, %d run-time tables (%d B), %d previews'
          % (out, len(MATERIALS), len(RUNTIME), n, len(PREVIEW)))


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
import os as _os  # noqa: E402
import sys as _sys  # noqa: E402
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    main()
